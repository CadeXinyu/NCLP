`timescale 1ns / 1ps
`default_nettype none

module clock_drp_sequencer #(
    // O: output divider, D: input divider, M: feedback multiplier.
    parameter logic [7:0] DEFAULT_O = 8'd1,
    parameter logic [3:0] DEFAULT_D = 4'd1,
    parameter logic [6:0] DEFAULT_M = 7'd2,
    parameter integer LAST_COMMAND_INDEX = 0,
    // All cycle counts below use config_clk, including the settling delay
    // after the destination has acknowledged reset release.
    parameter integer GATE_OFF_CYCLES = 64,
    parameter integer RESET_HOLD_CYCLES = 4,
    parameter integer RESET_ACK_TIMEOUT_CYCLES = 1024,
    parameter integer DRP_TIMEOUT_CYCLES = 1024,
    parameter integer LOCK_TIMEOUT_CYCLES = 1_000_000,
    parameter integer LOCK_STABLE_CYCLES = 16,
    parameter integer RESET_RELEASE_CYCLES = 16
) (
    input  wire logic                              config_clk,
    input  wire logic                              reset,

    input  wire logic                              request,
    input  wire logic                              request_valid,
    input  wire logic [7:0]                        requested_o,
    input  wire logic [3:0]                        requested_d,
    input  wire logic [6:0]                        requested_m,

    output logic [7:0]                             active_o,
    output logic [3:0]                             active_d,
    output logic [6:0]                             active_m,
    output logic [3:0]                             command_index,
    input  wire clock_drp_pkg::drp_command_t command,

    input  wire logic                              primitive_locked,
    input  wire logic [15:0]                       drp_read_data,
    input  wire logic                              drp_ready,
    output logic [6:0]                             drp_address,
    output logic [15:0]                            drp_write_data,
    output logic                                   drp_enable,
    output logic                                   drp_write_enable,

    output logic                                   primitive_reset,
    output logic                                   output_enable,
    output logic                                   domain_reset_request,
    input  wire logic                              domain_reset_ack,
    output logic                                   ready,
    output logic                                   busy,
    output logic                                   done_pulse,
    output logic                                   error,
    output clock_drp_pkg::clock_error_t error_code,
    output logic                                   locked
);

    import clock_drp_pkg::*;

    typedef enum logic [3:0] {
        STATE_BOOT_RESET       = 4'd0,
        STATE_BOOT_LOCK        = 4'd1,
        STATE_START_OUTPUT     = 4'd2,
        STATE_RELEASE_DOMAIN   = 4'd3,
        STATE_IDLE             = 4'd4,
        STATE_QUIESCE          = 4'd5,
        STATE_ASSERT_RESET     = 4'd6,
        STATE_READ             = 4'd7,
        STATE_WAIT_READ        = 4'd8,
        STATE_WRITE            = 4'd9,
        STATE_WAIT_WRITE       = 4'd10,
        STATE_PROGRAM_LOCK     = 4'd11,
        STATE_FAIL             = 4'd12,
        STATE_ERROR            = 4'd13,
        STATE_GATE_OFF         = 4'd14,
        STATE_RELEASE_HOLD     = 4'd15
    } state_t;

    localparam integer WAIT_LIMIT_0 =
        (GATE_OFF_CYCLES > RESET_HOLD_CYCLES) ?
            GATE_OFF_CYCLES : RESET_HOLD_CYCLES;
    localparam integer WAIT_LIMIT_1 =
        (RESET_ACK_TIMEOUT_CYCLES > DRP_TIMEOUT_CYCLES) ?
            RESET_ACK_TIMEOUT_CYCLES : DRP_TIMEOUT_CYCLES;
    localparam integer WAIT_LIMIT_2 =
        (LOCK_TIMEOUT_CYCLES > RESET_RELEASE_CYCLES) ?
            LOCK_TIMEOUT_CYCLES : RESET_RELEASE_CYCLES;
    localparam integer WAIT_LIMIT_3 =
        (WAIT_LIMIT_0 > WAIT_LIMIT_1) ? WAIT_LIMIT_0 : WAIT_LIMIT_1;
    localparam integer WAIT_LIMIT =
        (WAIT_LIMIT_2 > WAIT_LIMIT_3) ? WAIT_LIMIT_2 : WAIT_LIMIT_3;
    localparam integer WAIT_COUNT_WIDTH =
        (WAIT_LIMIT <= 1) ? 1 : $clog2(WAIT_LIMIT);
    localparam integer STABLE_COUNT_WIDTH =
        (LOCK_STABLE_CYCLES <= 1) ? 1 : $clog2(LOCK_STABLE_CYCLES);

    state_t state;
    logic [WAIT_COUNT_WIDTH-1:0] wait_count;
    logic [STABLE_COUNT_WIDTH-1:0] stable_count;
    logic request_seen;
    logic program_inflight;

    (* ASYNC_REG = "TRUE" *) logic [1:0] locked_sync;
    (* ASYNC_REG = "TRUE" *) logic [1:0] domain_reset_sync;
    always_ff @(posedge config_clk or posedge reset) begin
        if (reset) begin
            locked_sync <= 2'b00;
            domain_reset_sync <= 2'b11;
        end else begin
            locked_sync <= {locked_sync[0], primitive_locked};
            domain_reset_sync <= {domain_reset_sync[0], domain_reset_ack};
        end
    end
    // Public lock means that the generated clock is usable, not merely that
    // the primitive's asynchronous LOCKED pin happens to be high.
    assign locked = ready && locked_sync[1] && output_enable &&
                    !domain_reset_sync[1];

    always_ff @(posedge config_clk or posedge reset) begin
        if (reset) begin
            state            <= STATE_BOOT_RESET;
            wait_count       <= '0;
            stable_count     <= '0;
            request_seen     <= 1'b0;
            program_inflight <= 1'b0;
            active_o         <= DEFAULT_O;
            active_d         <= DEFAULT_D;
            active_m         <= DEFAULT_M;
            command_index    <= 4'd0;
            drp_address      <= 7'd0;
            drp_write_data   <= 16'd0;
            drp_enable       <= 1'b0;
            drp_write_enable <= 1'b0;
            primitive_reset  <= 1'b1;
            output_enable    <= 1'b0;
            domain_reset_request <= 1'b1;
            ready            <= 1'b0;
            busy             <= 1'b0;
            done_pulse       <= 1'b0;
            error            <= 1'b0;
            error_code       <= CLOCK_ERROR_NONE;
        end else begin
            drp_enable       <= 1'b0;
            drp_write_enable <= 1'b0;
            done_pulse       <= 1'b0;

            if (!request)
                request_seen <= 1'b0;
            else if (busy && !request_seen)
                request_seen <= 1'b1;

            case (state)
                STATE_BOOT_RESET: begin
                    primitive_reset <= 1'b1;
                    output_enable   <= 1'b0;
                    domain_reset_request <= 1'b1;
                    ready           <= 1'b0;
                    if (!domain_reset_sync[1] &&
                        (wait_count >= RESET_ACK_TIMEOUT_CYCLES - 1)) begin
                        error      <= 1'b1;
                        error_code <= CLOCK_ERROR_RESET_TIMEOUT;
                        state      <= STATE_FAIL;
                    end else if (domain_reset_sync[1] &&
                                 (wait_count >= RESET_HOLD_CYCLES - 1)) begin
                        wait_count      <= '0;
                        stable_count    <= '0;
                        primitive_reset <= 1'b0;
                        state           <= STATE_BOOT_LOCK;
                    end else begin
                        wait_count <= wait_count + 1'b1;
                    end
                end

                STATE_BOOT_LOCK,
                STATE_PROGRAM_LOCK: begin
                    if (wait_count >= LOCK_TIMEOUT_CYCLES - 1) begin
                        error      <= 1'b1;
                        error_code <= CLOCK_ERROR_LOCK_TIMEOUT;
                        state      <= STATE_FAIL;
                    end else if (locked_sync[1]) begin
                        if (stable_count >= LOCK_STABLE_CYCLES - 1) begin
                            stable_count  <= '0;
                            wait_count    <= '0;
                            output_enable <= 1'b1;
                            state         <= STATE_START_OUTPUT;
                        end else begin
                            stable_count <= stable_count + 1'b1;
                            wait_count   <= wait_count + 1'b1;
                        end
                    end else begin
                        stable_count <= '0;
                        wait_count <= wait_count + 1'b1;
                    end
                end

                STATE_START_OUTPUT: begin
                    // The destination reset can only deassert after the gated
                    // output produces real edges.  Its returned level is the
                    // startup acknowledgement; no frequency-dependent delay
                    // is assumed here.
                    if (!locked_sync[1]) begin
                        error      <= 1'b1;
                        error_code <= CLOCK_ERROR_LOCK_LOST;
                        state      <= STATE_FAIL;
                    end else begin
                        wait_count           <= '0;
                        domain_reset_request <= 1'b0;
                        state                <= STATE_RELEASE_DOMAIN;
                    end
                end

                STATE_RELEASE_DOMAIN: begin
                    if (!locked_sync[1]) begin
                        error      <= 1'b1;
                        error_code <= CLOCK_ERROR_LOCK_LOST;
                        state      <= STATE_FAIL;
                    end else if (!domain_reset_sync[1]) begin
                        wait_count <= '0;
                        state      <= STATE_RELEASE_HOLD;
                    end else if (wait_count >= RESET_ACK_TIMEOUT_CYCLES - 1) begin
                        error      <= 1'b1;
                        error_code <= CLOCK_ERROR_RESET_TIMEOUT;
                        state      <= STATE_FAIL;
                    end else begin
                        wait_count <= wait_count + 1'b1;
                    end
                end

                STATE_RELEASE_HOLD: begin
                    // Destination reset is already deasserted; READY waits for
                    // RESET_RELEASE_CYCLES additional configuration clocks.
                    if (!locked_sync[1]) begin
                        error      <= 1'b1;
                        error_code <= CLOCK_ERROR_LOCK_LOST;
                        state      <= STATE_FAIL;
                    end else if (wait_count >= RESET_RELEASE_CYCLES - 1) begin
                        wait_count <= '0;
                        ready      <= 1'b1;
                        busy       <= 1'b0;
                        done_pulse <= program_inflight;
                        program_inflight <= 1'b0;
                        state      <= STATE_IDLE;
                    end else begin
                        wait_count <= wait_count + 1'b1;
                    end
                end

                STATE_IDLE: begin
                    ready <= 1'b1;
                    busy  <= 1'b0;

                    if (!locked_sync[1]) begin
                        domain_reset_request <= 1'b1;
                        output_enable   <= 1'b0;
                        primitive_reset <= 1'b1;
                        error           <= 1'b1;
                        error_code      <= CLOCK_ERROR_LOCK_LOST;
                        done_pulse      <= 1'b1;
                        state           <= STATE_ERROR;
                    end else if (request && !request_seen) begin
                        request_seen <= 1'b1;
                        done_pulse   <= 1'b0;
                        if (!request_valid) begin
                            error      <= 1'b1;
                            error_code <= CLOCK_ERROR_INVALID;
                            done_pulse <= 1'b1;
                        end else begin
                            active_o         <= requested_o;
                            active_d         <= requested_d;
                            active_m         <= requested_m;
                            program_inflight <= 1'b1;
                            wait_count       <= '0;
                            stable_count     <= '0;
                            domain_reset_request <= 1'b1;
                            ready            <= 1'b0;
                            busy             <= 1'b1;
                            error            <= 1'b0;
                            error_code       <= CLOCK_ERROR_NONE;
                            state            <= STATE_QUIESCE;
                        end
                    end
                end

                STATE_QUIESCE: begin
                    // Reset is asserted asynchronously in the destination
                    // domain.  Do not gate its clock until the assertion has
                    // returned to this control domain.
                    if (domain_reset_sync[1]) begin
                        wait_count      <= '0;
                        output_enable   <= 1'b0;
                        state           <= STATE_GATE_OFF;
                    end else if (wait_count >= RESET_ACK_TIMEOUT_CYCLES - 1) begin
                        error      <= 1'b1;
                        error_code <= CLOCK_ERROR_RESET_TIMEOUT;
                        state      <= STATE_FAIL;
                    end else begin
                        wait_count <= wait_count + 1'b1;
                    end
                end

                STATE_GATE_OFF: begin
                    // BUFGCE disables in its raw-clock domain. This delay is
                    // counted in config_clk cycles and must cover enough raw
                    // clock edges at the slowest supported output frequency.
                    if (wait_count >= GATE_OFF_CYCLES - 1) begin
                        wait_count      <= '0;
                        primitive_reset <= 1'b1;
                        state           <= STATE_ASSERT_RESET;
                    end else begin
                        wait_count <= wait_count + 1'b1;
                    end
                end

                STATE_ASSERT_RESET: begin
                    if (wait_count >= RESET_HOLD_CYCLES - 1) begin
                        wait_count    <= '0;
                        command_index <= 4'd0;
                        state         <= STATE_READ;
                    end else begin
                        wait_count <= wait_count + 1'b1;
                    end
                end

                STATE_READ: begin
                    drp_address <= command.address;
                    drp_enable  <= 1'b1;
                    wait_count  <= '0;
                    state       <= STATE_WAIT_READ;
                end

                STATE_WAIT_READ: begin
                    if (drp_ready) begin
                        drp_write_data <=
                            (drp_read_data & command.preserve_mask) | command.data;
                        wait_count <= '0;
                        state      <= STATE_WRITE;
                    end else if (wait_count >= DRP_TIMEOUT_CYCLES - 1) begin
                        error      <= 1'b1;
                        error_code <= CLOCK_ERROR_READ_TIMEOUT;
                        state      <= STATE_FAIL;
                    end else begin
                        wait_count <= wait_count + 1'b1;
                    end
                end

                STATE_WRITE: begin
                    drp_address      <= command.address;
                    drp_enable       <= 1'b1;
                    drp_write_enable <= 1'b1;
                    wait_count       <= '0;
                    state            <= STATE_WAIT_WRITE;
                end

                STATE_WAIT_WRITE: begin
                    if (drp_ready) begin
                        wait_count <= '0;
                        if (command_index == LAST_COMMAND_INDEX) begin
                            stable_count    <= '0;
                            primitive_reset <= 1'b0;
                            state           <= STATE_PROGRAM_LOCK;
                        end else begin
                            command_index <= command_index + 4'd1;
                            state         <= STATE_READ;
                        end
                    end else if (wait_count >= DRP_TIMEOUT_CYCLES - 1) begin
                        error      <= 1'b1;
                        error_code <= CLOCK_ERROR_WRITE_TIMEOUT;
                        state      <= STATE_FAIL;
                    end else begin
                        wait_count <= wait_count + 1'b1;
                    end
                end

                STATE_FAIL: begin
                    primitive_reset  <= 1'b1;
                    output_enable    <= 1'b0;
                    domain_reset_request <= 1'b1;
                    ready            <= 1'b1;
                    busy             <= 1'b0;
                    done_pulse       <= 1'b1;
                    program_inflight <= 1'b0;
                    state            <= STATE_ERROR;
                end

                STATE_ERROR: begin
                    ready <= 1'b1;
                    busy  <= 1'b0;
                    if (request && !request_seen) begin
                        request_seen <= 1'b1;
                        done_pulse   <= 1'b1;
                        if (!request_valid) begin
                            error      <= 1'b1;
                            error_code <= CLOCK_ERROR_INVALID;
                        end else begin
                            active_o         <= requested_o;
                            active_d         <= requested_d;
                            active_m         <= requested_m;
                            program_inflight <= 1'b1;
                            wait_count       <= '0;
                            stable_count     <= '0;
                            ready            <= 1'b0;
                            busy             <= 1'b1;
                            done_pulse       <= 1'b0;
                            error            <= 1'b0;
                            error_code       <= CLOCK_ERROR_NONE;
                            state            <= STATE_QUIESCE;
                        end
                    end
                end

                default: begin
                    error      <= 1'b1;
                    error_code <= CLOCK_ERROR_LOCK_LOST;
                    state      <= STATE_FAIL;
                end
            endcase
        end
    end

`ifndef SYNTHESIS
    initial begin
        if ((LAST_COMMAND_INDEX < 0) || (LAST_COMMAND_INDEX > 15))
            $fatal(1, "clock_drp_sequencer: LAST_COMMAND_INDEX must be in 0..15");
        if ((GATE_OFF_CYCLES < 1) || (RESET_HOLD_CYCLES < 1) ||
            (RESET_ACK_TIMEOUT_CYCLES < 1) || (DRP_TIMEOUT_CYCLES < 1) ||
            (LOCK_TIMEOUT_CYCLES < 1) || (LOCK_STABLE_CYCLES < 1) ||
            (RESET_RELEASE_CYCLES < 1))
            $fatal(1, "clock_drp_sequencer: cycle counts must all be positive");
    end
`endif

endmodule

`default_nettype wire
