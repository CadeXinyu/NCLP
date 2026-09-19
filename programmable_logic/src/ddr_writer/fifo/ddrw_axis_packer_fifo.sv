`timescale 1ns / 1ps
`default_nettype none

// Packs the public 16-bit Intan stream into 64-bit DDR writer entries.
//
// Accepted public transfers are deliberately narrow:
//   payload : tkeep = 2'b11, tlast = 1'b0
//   EOS     : tkeep = 2'b00, tlast = 1'b1
//
// EOS is metadata, not data.  It either marks a partially packed entry or is
// emitted as a zero-byte marker when no payload is pending.  This lets the
// downstream writer distinguish an aligned end-of-session from ordinary data.
module ddrw_axis_packer_fifo #(
    parameter integer DEPTH_ENTRIES = 4096,
    parameter integer OCCUPANCY_WIDTH = (DEPTH_ENTRIES <= 1) ? 1 : $clog2(DEPTH_ENTRIES + 1)
) (
    input  wire                     clk,
    input  wire                     rst,
    input  wire                     clear,
    input  wire                     accept_enable,
    input  wire                     discard_mode,

    input  wire [15:0]              s_axis_tdata,
    input  wire [1:0]               s_axis_tkeep,
    input  wire                     s_axis_tvalid,
    output wire                     s_axis_tready,
    input  wire                     s_axis_tlast,

    output wire [63:0]              m_entry_data,
    output wire [3:0]               m_entry_byte_count,
    output wire                     m_entry_eos,
    output wire                     m_entry_valid,
    input  wire                     m_entry_ready,

    output wire [OCCUPANCY_WIDTH-1:0]   occupancy_entries,
    output wire [OCCUPANCY_WIDTH-1:0]   high_water_entries,
    output wire                     partial_pending,
    output reg                      protocol_error_pulse,
    output reg                      discard_eos_pulse
);
    localparam integer REQUIRED_OCCUPANCY_WIDTH =
        (DEPTH_ENTRIES <= 1) ? 1 : $clog2(DEPTH_ENTRIES + 1);
    localparam integer POINTER_WIDTH = (DEPTH_ENTRIES <= 1) ? 1 : $clog2(DEPTH_ENTRIES);
    localparam integer ENTRY_WIDTH = 69;

    localparam [1:0] KEEP_PAYLOAD = 2'b11;
    localparam [1:0] KEEP_EOS     = 2'b00;

    initial begin
        if (DEPTH_ENTRIES < 1) begin
            $error("ddrw_axis_packer_fifo DEPTH_ENTRIES must be at least one");
            $finish;
        end
        if (OCCUPANCY_WIDTH != REQUIRED_OCCUPANCY_WIDTH) begin
            $error("ddrw_axis_packer_fifo OCCUPANCY_WIDTH must equal $clog2(DEPTH_ENTRIES + 1)");
            $finish;
        end
    end

    // One URAM entry is {eos, byte_count, data} = 69 bits.  Keep the RAM read
    // register on a strict synchronous-read template; the separate head-valid
    // controller below provides FWFT behavior without adding a mux or reset to
    // the UltraRAM output datapath.
    (* ram_style = "ultra" *) reg [ENTRY_WIDTH-1:0] fifo_memory [0:DEPTH_ENTRIES-1];

    reg [POINTER_WIDTH-1:0] write_pointer;
    reg [POINTER_WIDTH-1:0] read_pointer;
    reg [OCCUPANCY_WIDTH-1:0] fifo_occupancy_entries;
    reg [OCCUPANCY_WIDTH-1:0] fifo_high_water_entries;
    reg [ENTRY_WIDTH-1:0] ram_read_data;
    reg                   head_valid;

    // Only the first three words wait here; the fourth enters the FIFO directly.
    reg [47:0] partial_data;
    reg [1:0]  partial_word_count;

    wire payload_transfer = (s_axis_tkeep == KEEP_PAYLOAD) && !s_axis_tlast;
    wire eos_transfer = (s_axis_tkeep == KEEP_EOS) && s_axis_tlast;
    wire legal_transfer = payload_transfer || eos_transfer;

    wire output_pop = m_entry_valid && m_entry_ready;
    wire fifo_full = (fifo_occupancy_entries == DEPTH_ENTRIES);
    wire enqueue_required = eos_transfer ||
                            (payload_transfer && (partial_word_count == 2'd3));
    wire enqueue_space = !fifo_full || output_pop;

    // Invalid transfers never consume FIFO space, and the first three payload
    // words can safely reside in the pack register even while the FIFO is full.
    // A transfer that completes an entry waits for real FIFO space.
    assign s_axis_tready = !rst && !clear && accept_enable &&
                           (discard_mode || !enqueue_required || enqueue_space);

    wire input_accept = s_axis_tvalid && s_axis_tready;
    wire normal_accept = input_accept && !discard_mode;

    reg [ENTRY_WIDTH-1:0] enqueue_entry;
    reg                   enqueue_valid;

    always @* begin
        enqueue_entry = {ENTRY_WIDTH{1'b0}};
        enqueue_valid = 1'b0;

        if (normal_accept && legal_transfer) begin
            if (payload_transfer && (partial_word_count == 2'd3)) begin
                enqueue_entry = {
                    1'b0,
                    4'd8,
                    s_axis_tdata,
                    partial_data
                };
                enqueue_valid = 1'b1;
            end else if (eos_transfer) begin
                enqueue_entry = {
                    1'b1,
                    {1'b0, partial_word_count, 1'b0},
                    {16'd0, partial_data}
                };
                enqueue_valid = 1'b1;
            end
        end
    end

    wire fifo_push = enqueue_valid;

    function automatic [POINTER_WIDTH-1:0] increment_pointer;
        input [POINTER_WIDTH-1:0] pointer;
        begin
            if (pointer == DEPTH_ENTRIES - 1) begin
                increment_pointer = {POINTER_WIDTH{1'b0}};
            end else begin
                increment_pointer = pointer + {{(POINTER_WIDTH-1){1'b0}}, 1'b1};
            end
        end
    endfunction

    wire [POINTER_WIDTH-1:0] next_read_pointer = increment_pointer(read_pointer);

    // When the visible head is consumed, synchronously fetch its successor.
    // When data first enters an empty FIFO, fetch the head automatically on the
    // following clock; the consumer never needs to issue a read request.
    wire ram_read_enable = (!head_valid && (fifo_occupancy_entries != 0)) ||
                           (output_pop && (fifo_occupancy_entries > 1));
    wire [POINTER_WIDTH-1:0] ram_read_address =
        (output_pop && (fifo_occupancy_entries > 1)) ? next_read_pointer : read_pointer;

    reg [OCCUPANCY_WIDTH-1:0] next_occupancy_entries;
    always @* begin
        next_occupancy_entries = fifo_occupancy_entries;
        case ({fifo_push, output_pop})
            2'b10: next_occupancy_entries = fifo_occupancy_entries + {{(OCCUPANCY_WIDTH-1){1'b0}}, 1'b1};
            2'b01: next_occupancy_entries = fifo_occupancy_entries - {{(OCCUPANCY_WIDTH-1){1'b0}}, 1'b1};
            default: next_occupancy_entries = fifo_occupancy_entries;
        endcase
    end

    // Strict synchronous simple-dual-port UltraRAM template.  Read data is not
    // reset; head_valid masks it after reset/clear until a real entry is read.
    always @(posedge clk) begin
        if (!rst && !clear && fifo_push) begin
            fifo_memory[write_pointer] <= enqueue_entry;
        end
        if (ram_read_enable) begin
            ram_read_data <= fifo_memory[ram_read_address];
        end
    end

    always @(posedge clk) begin
        if (rst || clear) begin
            write_pointer        <= {POINTER_WIDTH{1'b0}};
            read_pointer         <= {POINTER_WIDTH{1'b0}};
            fifo_occupancy_entries           <= {OCCUPANCY_WIDTH{1'b0}};
            fifo_high_water_entries      <= {OCCUPANCY_WIDTH{1'b0}};
            head_valid           <= 1'b0;
            partial_data         <= 48'd0;
            partial_word_count   <= 2'd0;
            protocol_error_pulse <= 1'b0;
            discard_eos_pulse    <= 1'b0;
        end else begin
            protocol_error_pulse <= 1'b0;
            discard_eos_pulse    <= 1'b0;

            if (output_pop) begin
                // A pre-existing successor is fetched on this same edge.  A
                // simultaneous push into a one-entry FIFO deliberately incurs
                // one safe refill cycle rather than relying on RAM collision
                // behavior.
                head_valid <= (fifo_occupancy_entries > 1);
            end else if (!head_valid && (fifo_occupancy_entries != 0)) begin
                head_valid <= 1'b1;
            end

            if (fifo_push) begin
                write_pointer <= increment_pointer(write_pointer);
            end
            if (output_pop) begin
                read_pointer <= increment_pointer(read_pointer);
            end

            fifo_occupancy_entries <= next_occupancy_entries;
            if (next_occupancy_entries > fifo_high_water_entries) begin
                fifo_high_water_entries <= next_occupancy_entries;
            end

            // Discard mode terminates any partially assembled word immediately,
            // even when no input transfer arrives in that cycle.
            if (discard_mode) begin
                partial_data       <= 48'd0;
                partial_word_count <= 2'd0;

                if (input_accept) begin
                    if (!legal_transfer) begin
                        protocol_error_pulse <= 1'b1;
                    end
                    if (eos_transfer) begin
                        discard_eos_pulse <= 1'b1;
                    end
                end
            end else if (normal_accept) begin
                if (!legal_transfer) begin
                    protocol_error_pulse <= 1'b1;
                end else if (payload_transfer) begin
                    case (partial_word_count)
                        2'd0: partial_data[15:0]  <= s_axis_tdata;
                        2'd1: partial_data[31:16] <= s_axis_tdata;
                        2'd2: partial_data[47:32] <= s_axis_tdata;
                        default: ; // Fourth word is already in enqueue_entry.
                    endcase

                    if (partial_word_count == 2'd3) begin
                        partial_data       <= 48'd0;
                        partial_word_count <= 2'd0;
                    end else begin
                        partial_word_count <= partial_word_count + 2'd1;
                    end
                end else begin
                    // Legal EOS has either emitted the partial payload or an
                    // explicit zero-byte marker; begin the next pack cleanly.
                    partial_data       <= 48'd0;
                    partial_word_count <= 2'd0;
                end
            end
        end
    end

    assign m_entry_data       = ram_read_data[63:0];
    assign m_entry_byte_count = ram_read_data[67:64];
    assign m_entry_eos        = ram_read_data[68];
    assign m_entry_valid      = head_valid;

    assign occupancy_entries      = fifo_occupancy_entries;
    assign high_water_entries = fifo_high_water_entries;
    assign partial_pending = (partial_word_count != 0);

endmodule

`default_nettype wire
