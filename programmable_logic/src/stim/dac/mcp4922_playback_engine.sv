// SPDX-License-Identifier: MIT
`timescale 1ns/1ps
`default_nettype none

// Triggered paired-waveform player for the MCP4922.
//
// One synchronous RAM word contains two 12-bit DAC codes:
//   ram_rd_data[11:0]  = channel A
//   ram_rd_data[27:16] = channel B
//
// PRIME_DAC validates and snapshots the complete playback configuration, then
// writes zero to both physical DAC channels. Only completion of both zero
// frames primes the engine. Every trigger starts at START_INDEX and then wraps
// END_INDEX back to LOOP_INDEX.
//
// Normal completion and STOP both finish any already-accepted channel pair and
// then write hard-coded zero frames to A and B. STOP while otherwise idle also
// performs this zero pair so the physical outputs can always be recovered.
// Cleanup frames never increment completed_update_count. Channel enables affect
// waveform frames only; PRIME and cleanup always touch both physical channels.
// With LDAC tied low, each output changes when its complete 16-bit frame ends
// and CS rises.
module mcp4922_playback_engine #(
    parameter integer RAM_DEPTH             = 1024,
    parameter integer RAM_ADDR_WIDTH            = (RAM_DEPTH > 1) ? $clog2(RAM_DEPTH) : 1,
    parameter integer CLKS_PER_HALF_BIT     = 5,
    parameter integer CS_SETUP_CLKS         = 1,
    parameter integer CS_HOLD_CLKS          = 1,
    parameter integer CS_HIGH_CLKS          = 4,
    // A one-cycle synchronous inferred BRAM requires one wait state between
    // changing ram_rd_addr and sampling ram_rd_data in this controller.
    parameter integer RAM_READ_WAIT_CYCLES  = 1
) (
    input  wire                         clk,
    input  wire                         resetn,

    input  wire                         prime_pulse,
    input  wire                         trigger_pulse,
    input  wire                         stop_pulse,
    input  wire                         clear_diagnostics,

    input  wire                         cfg_a_enable,
    input  wire                         cfg_b_enable,
    input  wire                         cfg_continuous,
    input  wire [31:0]                  cfg_update_period_clocks,
    input  wire [RAM_ADDR_WIDTH-1:0]        cfg_start_index,
    input  wire [RAM_ADDR_WIDTH-1:0]        cfg_loop_index,
    input  wire [RAM_ADDR_WIDTH-1:0]        cfg_end_index,
    input  wire [31:0]                  cfg_finite_update_count,

    output reg  [RAM_ADDR_WIDTH-1:0]        ram_rd_addr,
    input  wire [31:0]                  ram_rd_data,

    // Active through PRIME, waveform playback and mandatory zero completion.
    output reg                          transaction_active,
    output wire                         busy,
    output reg  [RAM_ADDR_WIDTH-1:0]        current_waveform_index,
    output reg  [31:0]                  completed_update_count,
    output reg                          done_pulse,
    output reg                          prime_done_pulse,
    output reg                          prime_fault_pulse,
    // ZERO_DONE proves physical completion of the B-zero frame. STOP_DONE is
    // the correlated subset produced only for a consumed STOP request.
    output reg                          zero_done_pulse,
    output reg                          stop_done_pulse,
    output reg                          unserved_trigger_sticky,
    output reg                          invalid_config_sticky,
    output reg                          timing_late_sticky,

    output wire                         dac_sync_n,
    output wire                         dac_sclk,
    output wire                         dac_sdin
);
    localparam integer REQUIRED_RAM_ADDR_WIDTH =
        (RAM_DEPTH > 1) ? $clog2(RAM_DEPTH) : 1;

    localparam integer RAM_WAIT_COUNT_WIDTH =
        (RAM_READ_WAIT_CYCLES > 0) ? $clog2(RAM_READ_WAIT_CYCLES + 1) : 1;

    localparam [3:0] ST_IDLE              = 4'd0;
    localparam [3:0] ST_RAM_WAIT          = 4'd1;
    localparam [3:0] ST_WAVE_OFFER_FIRST  = 4'd2;
    localparam [3:0] ST_WAVE_OFFER_SECOND = 4'd3;
    localparam [3:0] ST_WAVE_WAIT_DONE    = 4'd4;
    localparam [3:0] ST_ZERO_OFFER_A      = 4'd5;
    localparam [3:0] ST_ZERO_OFFER_B      = 4'd6;
    localparam [3:0] ST_ZERO_WAIT_DONE    = 4'd7;

    localparam [3:0] ST_FINAL_HOLD        = 4'd8;

    reg [3:0] state;

    // PRIME is the only point at which external configuration is sampled.
    reg active_a_enable;
    reg active_b_enable;
    reg active_continuous;
    reg [31:0] active_update_period_clocks;
    reg [RAM_ADDR_WIDTH-1:0] active_start_index;
    reg [RAM_ADDR_WIDTH-1:0] active_loop_index;
    reg [RAM_ADDR_WIDTH-1:0] active_end_index;
    reg [31:0] active_finite_update_count;

    reg [31:0] waveform_word;
    reg [RAM_WAIT_COUNT_WIDTH-1:0] ram_wait_count;
    reg [31:0] update_cycles_remaining;
    reg first_update_pending;
    reg stop_requested;
    reg transaction_is_prime;
    reg primed;
    reg stop_waiting_for_frame_idle;
    // The frame master accepts one active and one queued frame. Do not treat an
    // accepted pair as complete until every member has produced frame_done.
    reg [1:0] frame_completions_remaining;

    reg frame_valid;
    reg [15:0] frame_word;
    wire frame_ready;
    wire frame_busy;
    wire frame_done;
    wire frame_fire = frame_valid && frame_ready;

    wire [15:0] channel_a_word = 16'h3000 | {4'd0, waveform_word[11:0]};
    wire [15:0] channel_b_word = 16'hB000 | {4'd0, waveform_word[27:16]};

    // The configured interval is measured between acceptance of the first SPI
    // frame in adjacent waveform updates. The first update begins immediately.
    wire launch_due = first_update_pending || (update_cycles_remaining <= 32'd1);

    wire indices_valid = (cfg_start_index < RAM_DEPTH) &&
                         (cfg_loop_index  < RAM_DEPTH) &&
                         (cfg_end_index   < RAM_DEPTH) &&
                         (cfg_start_index <= cfg_loop_index) &&
                         (cfg_loop_index  <= cfg_end_index);
    wire prime_config_valid = (cfg_a_enable || cfg_b_enable) &&
                              indices_valid &&
                              (cfg_update_period_clocks != 32'd0) &&
                              (cfg_continuous ||
                               (cfg_finite_update_count != 32'd0));

    wire [RAM_ADDR_WIDTH-1:0] next_waveform_index =
        (current_waveform_index >= active_end_index) ?
            active_loop_index : current_waveform_index + 1'b1;

    wire current_pair_in_progress =
        (state == ST_WAVE_OFFER_SECOND) || (state == ST_WAVE_WAIT_DONE);
    wire another_waveform_update_needed = active_continuous ||
        (current_pair_in_progress ?
            ((completed_update_count + 32'd1) < active_finite_update_count) :
            (completed_update_count < active_finite_update_count));

    assign busy = transaction_active || stop_waiting_for_frame_idle || frame_busy || frame_valid ||
                  (state != ST_IDLE);

    spi_frame_master #(
        .MAX_FRAME_BITS(16),
        .FRAME_BITS_W(5),
        .CLKS_PER_HALF_BIT(CLKS_PER_HALF_BIT),
        .SPI_MODE(0),
        .CS_SETUP_CLKS(CS_SETUP_CLKS),
        .CS_HOLD_CLKS(CS_HOLD_CLKS),
        .CS_HIGH_CLKS(CS_HIGH_CLKS),
        .MSB_FIRST(1'b1),
        .MOSI_IDLE(1'b0)
    ) frame_master_i (
        .i_Rst_L(resetn),
        .i_Clk(clk),
        .i_Frame_Valid(frame_valid),
        .i_MOSI_Frame(frame_word),
        .i_Frame_Bits(5'd16),
        .o_Frame_Ready(frame_ready),
        .o_Busy(frame_busy),
        .o_Done(frame_done),
        .o_SPI_CS_n(dac_sync_n),
        .o_SPI_Clk(dac_sclk),
        .o_SPI_MOSI(dac_sdin)
    );

`ifndef SYNTHESIS
    initial begin
        if (RAM_DEPTH < 1)
            $fatal(1, "mcp4922_playback_engine: RAM_DEPTH must be >= 1");
        if (RAM_ADDR_WIDTH != REQUIRED_RAM_ADDR_WIDTH)
            $fatal(1, "mcp4922_playback_engine: RAM_ADDR_WIDTH must match RAM_DEPTH");
        if (RAM_READ_WAIT_CYCLES < 0)
            $fatal(1, "mcp4922_playback_engine: RAM_READ_WAIT_CYCLES must be nonnegative");
    end
`endif

    // A frame remains offered until accepted. The frame master may accept the
    // second frame while the first is still active, so the sequential state
    // machine separately counts completion of every accepted group member.
    always @* begin
        frame_valid = 1'b0;
        frame_word  = 16'd0;

        case (state)
            ST_WAVE_OFFER_FIRST: begin
                if (launch_due && !stop_requested && !stop_pulse) begin
                    frame_valid = 1'b1;
                    frame_word = active_a_enable ? channel_a_word : channel_b_word;
                end
            end

            ST_WAVE_OFFER_SECOND: begin
                frame_valid = 1'b1;
                frame_word  = channel_b_word;
            end

            ST_ZERO_OFFER_A, ST_FINAL_HOLD: begin
                frame_valid = (state == ST_ZERO_OFFER_A) || launch_due || stop_requested || stop_pulse;
                frame_word  = 16'h3000;
            end

            ST_ZERO_OFFER_B: begin
                frame_valid = 1'b1;
                frame_word  = 16'hB000;
            end

            default: begin
                frame_valid = 1'b0;
                frame_word  = 16'd0;
            end
        endcase
    end

    always @(posedge clk or negedge resetn) begin
        if (!resetn) begin
            state                         <= ST_IDLE;
            active_a_enable               <= 1'b0;
            active_b_enable               <= 1'b0;
            active_continuous             <= 1'b0;
            active_update_period_clocks   <= 32'd1;
            active_start_index            <= {RAM_ADDR_WIDTH{1'b0}};
            active_loop_index             <= {RAM_ADDR_WIDTH{1'b0}};
            active_end_index              <= {RAM_ADDR_WIDTH{1'b0}};
            active_finite_update_count    <= 32'd0;
            waveform_word                   <= 32'd0;
            ram_rd_addr                   <= {RAM_ADDR_WIDTH{1'b0}};
            ram_wait_count                <= {RAM_WAIT_COUNT_WIDTH{1'b0}};
            update_cycles_remaining              <= 32'd0;
            first_update_pending                  <= 1'b0;
            stop_requested                <= 1'b0;
            transaction_is_prime                  <= 1'b0;
            primed                        <= 1'b0;
            stop_waiting_for_frame_idle              <= 1'b0;
            frame_completions_remaining         <= 2'd0;
            transaction_active                       <= 1'b0;
            current_waveform_index        <= {RAM_ADDR_WIDTH{1'b0}};
            completed_update_count        <= 32'd0;
            done_pulse                    <= 1'b0;
            prime_done_pulse              <= 1'b0;
            prime_fault_pulse             <= 1'b0;
            zero_done_pulse               <= 1'b0;
            stop_done_pulse               <= 1'b0;
            unserved_trigger_sticky       <= 1'b0;
            invalid_config_sticky         <= 1'b0;
            timing_late_sticky            <= 1'b0;
        end else begin
            done_pulse        <= 1'b0;
            prime_done_pulse  <= 1'b0;
            prime_fault_pulse <= 1'b0;
            zero_done_pulse   <= 1'b0;
            stop_done_pulse   <= 1'b0;

            if (clear_diagnostics) begin
                unserved_trigger_sticky <= 1'b0;
                invalid_config_sticky   <= 1'b0;
                timing_late_sticky      <= 1'b0;
            end

            if (transaction_active && !transaction_is_prime &&
                (update_cycles_remaining != 32'd0)) begin
                update_cycles_remaining <= update_cycles_remaining - 32'd1;
            end

            if (stop_pulse && transaction_active)
                stop_requested <= 1'b1;

            // Report a missed update deadline without changing data order.
            if (transaction_active && !transaction_is_prime && !first_update_pending &&
                !stop_requested && !stop_pulse &&
                another_waveform_update_needed &&
                (update_cycles_remaining == 32'd1) &&
                !((state == ST_WAVE_OFFER_FIRST) && frame_ready)) begin
                timing_late_sticky <= 1'b1;
            end

            // STOP has command priority. PRIME is the sole configuration
            // snapshot point; triggers consume only the validated snapshot.
            if (!stop_pulse && prime_pulse) begin
                if (transaction_active || (state != ST_IDLE) || frame_busy) begin
                    prime_fault_pulse <= 1'b1;
                end else if (!prime_config_valid) begin
                    invalid_config_sticky <= 1'b1;
                    primed                <= 1'b0;
                    prime_fault_pulse     <= 1'b1;
                end else begin
                    active_a_enable              <= cfg_a_enable;
                    active_b_enable              <= cfg_b_enable;
                    active_continuous            <= cfg_continuous;
                    active_update_period_clocks  <= cfg_update_period_clocks;
                    active_start_index           <= cfg_start_index;
                    active_loop_index            <= cfg_loop_index;
                    active_end_index             <= cfg_end_index;
                    active_finite_update_count   <= cfg_finite_update_count;
                    ram_rd_addr                  <= cfg_start_index;
                    current_waveform_index       <= cfg_start_index;
                    completed_update_count       <= 32'd0;
                    update_cycles_remaining             <= 32'd0;
                    first_update_pending                 <= 1'b0;
                    stop_requested               <= 1'b0;
                    transaction_is_prime                 <= 1'b1;
                    primed                       <= 1'b0;
                    stop_waiting_for_frame_idle            <= 1'b0;
                    frame_completions_remaining       <= 2'd0;
                    transaction_active                      <= 1'b1;
                    state                        <= ST_ZERO_OFFER_A;
                end
            end else if (!stop_pulse && trigger_pulse) begin
                if (transaction_active || (state != ST_IDLE) || frame_busy) begin
                    unserved_trigger_sticky <= 1'b1;
                end else if (!primed) begin
                    invalid_config_sticky <= 1'b1;
                end else begin
                    ram_rd_addr            <= active_start_index;
                    current_waveform_index <= active_start_index;
                    ram_wait_count         <= RAM_READ_WAIT_CYCLES[RAM_WAIT_COUNT_WIDTH-1:0];
                    update_cycles_remaining       <= 32'd0;
                    completed_update_count <= 32'd0;
                    first_update_pending           <= 1'b1;
                    stop_requested         <= 1'b0;
                    transaction_is_prime           <= 1'b0;
                    stop_waiting_for_frame_idle      <= 1'b0;
                    frame_completions_remaining <= 2'd0;
                    transaction_active                <= 1'b1;
                    state                  <= ST_RAM_WAIT;
                end
            end

            case (state)
                ST_IDLE: begin
                    if (stop_pulse) begin
                        // STOP is a physical safe-off command, not merely a
                        // request to end a tracked run. Preserve the validated
                        // playback snapshot while unconditionally zeroing both
                        // DAC channels and report normal DONE afterward.
                        first_update_pending           <= 1'b0;
                        update_cycles_remaining       <= 32'd0;
                        stop_requested         <= 1'b1;
                        transaction_is_prime           <= 1'b0;
                        frame_completions_remaining <= 2'd0;
                        transaction_active                <= 1'b1;
                        if (frame_busy) begin
                            stop_waiting_for_frame_idle <= 1'b1;
                            state             <= ST_IDLE;
                        end else begin
                            stop_waiting_for_frame_idle <= 1'b0;
                            state             <= ST_ZERO_OFFER_A;
                        end
                    end else if (stop_waiting_for_frame_idle) begin
                        transaction_active <= 1'b1;
                        if (!frame_busy) begin
                            stop_waiting_for_frame_idle <= 1'b0;
                            state             <= ST_ZERO_OFFER_A;
                        end
                    end else if (!prime_pulse && !trigger_pulse) begin
                        transaction_active <= 1'b0;
                    end
                end

                ST_RAM_WAIT: begin
                    if (stop_requested || stop_pulse) begin
                        first_update_pending     <= 1'b0;
                        update_cycles_remaining <= 32'd0;
                        state            <= ST_ZERO_OFFER_A;
                    end else if (ram_wait_count == {RAM_WAIT_COUNT_WIDTH{1'b0}}) begin
                        waveform_word <= ram_rd_data;
                        state       <= ST_WAVE_OFFER_FIRST;
                    end else begin
                        ram_wait_count <= ram_wait_count - 1'b1;
                    end
                end

                ST_WAVE_OFFER_FIRST: begin
                    if (stop_requested || stop_pulse) begin
                        first_update_pending     <= 1'b0;
                        update_cycles_remaining <= 32'd0;
                        state            <= ST_ZERO_OFFER_A;
                    end else if (frame_fire) begin
                        first_update_pending     <= 1'b0;
                        update_cycles_remaining <= active_update_period_clocks;
                        if (active_a_enable && active_b_enable) begin
                            frame_completions_remaining <= 2'd2;
                            state <= ST_WAVE_OFFER_SECOND;
                        end else begin
                            frame_completions_remaining <= 2'd1;
                            state <= ST_WAVE_WAIT_DONE;
                        end
                    end
                end

                // Once the first channel frame is accepted, STOP may not tear
                // an enabled A/B pair. Keep offering B until it is accepted.
                ST_WAVE_OFFER_SECOND: begin
                    if (frame_fire)
                        state <= ST_WAVE_WAIT_DONE;
                end

                ST_WAVE_WAIT_DONE: begin
                    if (frame_done) begin
                        if (frame_completions_remaining > 2'd1) begin
                            frame_completions_remaining <=
                                frame_completions_remaining - 1'b1;
                        end else begin
                            frame_completions_remaining  <= 2'd0;
                            completed_update_count <= completed_update_count + 32'd1;
                            if (stop_requested || stop_pulse) begin
                                update_cycles_remaining <= 32'd0;
                                state            <= ST_ZERO_OFFER_A;
                            end else if (!active_continuous &&
                                ((completed_update_count + 32'd1) >= active_finite_update_count)) begin
                                // Hold the final sample for its complete update period.
                                // STOP can still bypass this wait after the A/B pair.
                                state <= ST_FINAL_HOLD;
                            end else begin
                                current_waveform_index <= next_waveform_index;
                                ram_rd_addr            <= next_waveform_index;
                                ram_wait_count         <= RAM_READ_WAIT_CYCLES[RAM_WAIT_COUNT_WIDTH-1:0];
                                state                  <= ST_RAM_WAIT;
                            end
                        end
                    end
                end

                ST_ZERO_OFFER_A, ST_FINAL_HOLD: begin
                    if (frame_fire) begin
                        update_cycles_remaining <= 32'd0;
                        frame_completions_remaining <= 2'd2;
                        state <= ST_ZERO_OFFER_B;
                    end
                end

                ST_ZERO_OFFER_B: begin
                    if (frame_fire)
                        state <= ST_ZERO_WAIT_DONE;
                end

                ST_ZERO_WAIT_DONE: begin
                    if (frame_done) begin
                        if (frame_completions_remaining > 2'd1) begin
                            frame_completions_remaining <=
                                frame_completions_remaining - 1'b1;
                        end else begin
                            frame_completions_remaining <= 2'd0;
                            transaction_active               <= 1'b0;
                            stop_requested        <= 1'b0;
                            state                 <= ST_IDLE;
                            zero_done_pulse        <= 1'b1;
                            if (stop_requested || stop_pulse)
                                stop_done_pulse <= 1'b1;

                            if (transaction_is_prime) begin
                                if (stop_requested || stop_pulse) begin
                                    primed            <= 1'b0;
                                    prime_fault_pulse <= 1'b1;
                                end else begin
                                    primed           <= 1'b1;
                                    prime_done_pulse <= 1'b1;
                                end
                            end else begin
                                done_pulse <= 1'b1;
                            end
                        end
                    end
                end

                default: begin
                    state          <= ST_IDLE;
                    transaction_active        <= 1'b0;
                    stop_requested <= 1'b0;
                    primed         <= 1'b0;
                    stop_waiting_for_frame_idle <= 1'b0;
                    frame_completions_remaining <= 2'd0;
                end
            endcase
        end
    end

endmodule

`default_nettype wire
