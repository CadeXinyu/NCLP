`timescale 1ns/1ps
`default_nettype none

// Receive-side packet router.  The first physical beat is a routing header:
//   [6:0] target ID, [7] end of logical message, [15:8] protocol version.
// Remaining header bits are reserved and zero. PS payloads are committed to a
// private FIFO only after TLAST, then forwarded without the transport header.
// The one-word trigger route bypasses that FIFO.
module nclp_sfp_rx_router #(
    parameter integer PS_PAYLOAD_FIFO_DEPTH_BEATS = 2048
) (
    input  wire        clk,
    input  wire        resetn,
    input  wire        transport_up,
    input  wire        clear_diagnostics_pulse,
    input  wire        ps_command_route_enabled,
    input  wire        stim_trigger_route_enabled,

    input  wire [63:0] s_axis_tdata,
    input  wire [7:0]  s_axis_tkeep,
    input  wire        s_axis_tvalid,
    output wire        s_axis_tready,
    input  wire        s_axis_tlast,

    output wire [63:0] m_axis_ps_tdata,
    output wire [7:0]  m_axis_ps_tkeep,
    output wire        m_axis_ps_tvalid,
    input  wire        m_axis_ps_tready,
    output wire        m_axis_ps_tlast,

    output reg         stim_trigger_pulse,
    output wire        ps_queue_nonempty,
    output wire        parser_idle,
    output reg         fault_pulse,
    output reg  [31:0] malformed_packet_count,
    output reg  [31:0] ps_fifo_overflow_count,
    output reg  [31:0] dropped_packet_count,
    output reg  [31:0] ps_packet_count,
    output reg  [31:0] stim_trigger_count,
    output reg  [31:0] stim_trigger_reject_count
);
    localparam integer ADDR_WIDTH = $clog2(PS_PAYLOAD_FIFO_DEPTH_BEATS);
    localparam [1:0] HEADER = 2'd0;
    localparam [1:0] PAYLOAD = 2'd1;
    localparam [1:0] DROP = 2'd2;
    localparam [6:0] TARGET_PS = 7'h02;
    localparam [6:0] TARGET_STIM_TRIGGER = 7'h03;
    localparam [7:0] PROTOCOL_VERSION = 8'h01;

    reg [1:0] state;
    reg route_is_stim_trigger;
    reg stim_trigger_header_eom;

    (* ram_style = "block" *) reg [72:0] ps_fifo [0:PS_PAYLOAD_FIFO_DEPTH_BEATS-1];
    reg [ADDR_WIDTH:0] speculative_write_pointer;
    reg [ADDR_WIDTH:0] committed_write_pointer;
    reg [ADDR_WIDTH:0] read_pointer;
    reg [72:0] ps_output_word;
    reg ps_output_valid;

    wire [ADDR_WIDTH:0] occupied_words =
        speculative_write_pointer - read_pointer;
    wire ps_space_available = occupied_words < PS_PAYLOAD_FIFO_DEPTH_BEATS;
    wire committed_word_available =
        read_pointer != committed_write_pointer;
    wire load_ps_output = (!ps_output_valid || m_axis_ps_tready) &&
                          committed_word_available;
    wire input_transfer = s_axis_tvalid && s_axis_tready;
    wire [6:0] header_target = s_axis_tdata[6:0];
    wire header_valid = s_axis_tkeep == 8'hff && !s_axis_tlast &&
        s_axis_tdata[15:8] == PROTOCOL_VERSION &&
        s_axis_tdata[63:16] == 48'd0 && header_target != 7'd0;

    function automatic keep_is_contiguous(input [7:0] keep);
        begin
            case (keep)
                8'h01, 8'h03, 8'h07, 8'h0f,
                8'h1f, 8'h3f, 8'h7f, 8'hff:
                    keep_is_contiguous = 1'b1;
                default: keep_is_contiguous = 1'b0;
            endcase
        end
    endfunction

    wire payload_encoding_valid = s_axis_tlast ?
        keep_is_contiguous(s_axis_tkeep) : (s_axis_tkeep == 8'hff);

    assign s_axis_tready = resetn;
    assign m_axis_ps_tdata = ps_output_word[63:0];
    assign m_axis_ps_tkeep = ps_output_word[71:64];
    assign m_axis_ps_tlast = ps_output_word[72];
    assign m_axis_ps_tvalid = resetn && ps_output_valid;
    assign ps_queue_nonempty = ps_output_valid || committed_word_available;
    assign parser_idle = state == HEADER;

    always @(posedge clk) begin
        if (!resetn) begin
            state <= HEADER;
            route_is_stim_trigger <= 1'b0;
            stim_trigger_header_eom <= 1'b0;
            speculative_write_pointer <= 0;
            committed_write_pointer <= 0;
            read_pointer <= 0;
            ps_output_word <= 73'd0;
            ps_output_valid <= 1'b0;
            stim_trigger_pulse <= 1'b0;
            fault_pulse <= 1'b0;
            malformed_packet_count <= 32'd0;
            ps_fifo_overflow_count <= 32'd0;
            dropped_packet_count <= 32'd0;
            ps_packet_count <= 32'd0;
            stim_trigger_count <= 32'd0;
            stim_trigger_reject_count <= 32'd0;
        end else begin
            stim_trigger_pulse <= 1'b0;
            fault_pulse <= 1'b0;

            if (clear_diagnostics_pulse) begin
                malformed_packet_count <= 32'd0;
                ps_fifo_overflow_count <= 32'd0;
                dropped_packet_count <= 32'd0;
                ps_packet_count <= 32'd0;
                stim_trigger_count <= 32'd0;
                stim_trigger_reject_count <= 32'd0;
            end

            if (ps_output_valid && m_axis_ps_tready)
                ps_output_valid <= 1'b0;
            if (load_ps_output) begin
                ps_output_word <= ps_fifo[read_pointer[ADDR_WIDTH-1:0]];
                ps_output_valid <= 1'b1;
                read_pointer <= read_pointer + 1'b1;
            end

            if (!transport_up) begin
                // Keep complete PS packets, but discard a physical packet that
                // was cut off by link loss.
                state <= HEADER;
                speculative_write_pointer <= committed_write_pointer;
            end else if (input_transfer) begin
                case (state)
                    HEADER: begin
                        if (!header_valid) begin
                            malformed_packet_count <=
                                (clear_diagnostics_pulse ? 32'd0 : malformed_packet_count) + 1'b1;
                            fault_pulse <= 1'b1;
                            state <= s_axis_tlast ? HEADER : DROP;
                        end else if (header_target == TARGET_PS &&
                                     !s_axis_tdata[7]) begin
                            malformed_packet_count <=
                                (clear_diagnostics_pulse ? 32'd0 : malformed_packet_count) + 1'b1;
                            fault_pulse <= 1'b1;
                            state <= DROP;
                        end else if (header_target == TARGET_PS &&
                                     !ps_command_route_enabled) begin
                            // SFP control is inactive in local mode. Drain the
                            // complete packet without exposing stale commands
                            // if PS later selects remote compute.
                            dropped_packet_count <=
                                (clear_diagnostics_pulse ? 32'd0 : dropped_packet_count) + 1'b1;
                            state <= DROP;
                        end else if (header_target == TARGET_PS) begin
                            route_is_stim_trigger <= 1'b0;
                            state <= PAYLOAD;
                        end else if (header_target == TARGET_STIM_TRIGGER) begin
                            route_is_stim_trigger <= 1'b1;
                            stim_trigger_header_eom <= s_axis_tdata[7];
                            state <= PAYLOAD;
                        end else begin
                            dropped_packet_count <=
                                (clear_diagnostics_pulse ? 32'd0 : dropped_packet_count) + 1'b1;
                            fault_pulse <= 1'b1;
                            state <= DROP;
                        end
                    end

                    PAYLOAD: begin
                        if (!route_is_stim_trigger) begin
                            if (!payload_encoding_valid) begin
                                speculative_write_pointer <= committed_write_pointer;
                                malformed_packet_count <=
                                    (clear_diagnostics_pulse ? 32'd0 :
                                     malformed_packet_count) + 1'b1;
                                fault_pulse <= 1'b1;
                                state <= s_axis_tlast ? HEADER : DROP;
                            end else if (!ps_space_available) begin
                                speculative_write_pointer <= committed_write_pointer;
                                ps_fifo_overflow_count <=
                                    (clear_diagnostics_pulse ? 32'd0 :
                                     ps_fifo_overflow_count) + 1'b1;
                                fault_pulse <= 1'b1;
                                state <= s_axis_tlast ? HEADER : DROP;
                            end else begin
                                ps_fifo[speculative_write_pointer[ADDR_WIDTH-1:0]] <=
                                    {s_axis_tlast, s_axis_tkeep, s_axis_tdata};
                                speculative_write_pointer <=
                                    speculative_write_pointer + 1'b1;
                                if (s_axis_tlast) begin
                                    committed_write_pointer <=
                                        speculative_write_pointer + 1'b1;
                                    ps_packet_count <=
                                        (clear_diagnostics_pulse ? 32'd0 : ps_packet_count) + 1'b1;
                                    state <= HEADER;
                                end
                            end
                        end else begin
                            if (s_axis_tlast &&
                                s_axis_tkeep == 8'hff &&
                                s_axis_tdata == 64'h0000000000000001 &&
                                stim_trigger_header_eom &&
                                stim_trigger_route_enabled) begin
                                stim_trigger_pulse <= 1'b1;
                                stim_trigger_count <=
                                    (clear_diagnostics_pulse ? 32'd0 : stim_trigger_count) + 1'b1;
                            end else begin
                                stim_trigger_reject_count <=
                                    (clear_diagnostics_pulse ? 32'd0 :
                                     stim_trigger_reject_count) + 1'b1;
                                fault_pulse <= 1'b1;
                            end
                            state <= s_axis_tlast ? HEADER : DROP;
                        end
                    end

                    DROP: state <= s_axis_tlast ? HEADER : DROP;
                    default: state <= HEADER;
                endcase
            end
        end
    end

`ifndef SYNTHESIS
    initial begin
        if (PS_PAYLOAD_FIFO_DEPTH_BEATS < 4 ||
            (PS_PAYLOAD_FIFO_DEPTH_BEATS & (PS_PAYLOAD_FIFO_DEPTH_BEATS - 1)) != 0)
            $fatal(1, "PS_PAYLOAD_FIFO_DEPTH_BEATS must be a power of two >= 4");
    end
`endif
endmodule

`default_nettype wire
