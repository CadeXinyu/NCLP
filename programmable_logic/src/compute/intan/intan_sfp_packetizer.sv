`timescale 1ns/1ps
`default_nettype none

// Store-and-forward Intan frame width converter.
//
// The input frame boundary is supplied by the Intan acquisition block:
//   * TKEEP=2'b11 carries one opaque raw 16-bit word.
//   * TKEEP=2'b11 with TLAST=1 carries the final word of a frame.
//   * TKEEP=2'b00 with TLAST=1 is the acquisition-session EOS marker. An EOS
//     at a frame boundary is consumed locally. An EOS inside a frame marks
//     that frame as truncated and drops it.
//
// A complete valid frame is buffered before any output TVALID is asserted.
// Four consecutive input words occupy increasing 16-bit lanes of one AXIS64
// beat. The final TKEEP is 03/0f/3f/ff for one/two/three/four words. Invalid,
// truncated, and overlength frames are drained without partial output.
module intan_sfp_packetizer #(
    parameter integer MAX_FRAME_WORDS = 567
) (
    input  wire        clk,
    input  wire        resetn,

    input  wire [15:0] s_axis_tdata,
    input  wire [1:0]  s_axis_tkeep,
    input  wire        s_axis_tvalid,
    output wire        s_axis_tready,
    input  wire        s_axis_tlast,

    output reg  [63:0] m_axis_tdata,
    output reg  [7:0]  m_axis_tkeep,
    output reg         m_axis_tvalid,
    input  wire        m_axis_tready,
    output reg         m_axis_tlast,

    output wire        idle,
    output wire        in_frame,
    output reg         fault_pulse
);
    localparam integer BUFFER_BEATS = (MAX_FRAME_WORDS + 3) / 4;
    localparam integer WORD_COUNT_WIDTH =
        (MAX_FRAME_WORDS < 2) ? 1 : $clog2(MAX_FRAME_WORDS + 1);
    localparam integer BEAT_ADDR_WIDTH =
        (BUFFER_BEATS < 2) ? 1 : $clog2(BUFFER_BEATS);
    localparam [WORD_COUNT_WIDTH-1:0] MAX_FRAME_WORD_COUNT =
        MAX_FRAME_WORDS;

    (* ram_style = "block" *) reg [63:0] frame_buffer [0:BUFFER_BEATS-1];

    reg [63:0] assembly_data;
    reg [WORD_COUNT_WIDTH-1:0] input_word_count;
    reg [BEAT_ADDR_WIDTH-1:0] write_beat_index;
    reg [BEAT_ADDR_WIDTH-1:0] stored_last_index;
    reg [7:0] stored_last_keep;
    reg [BEAT_ADDR_WIDTH-1:0] output_beat_index;
    reg buffered_frame_ready;
    reg dropping_frame;

    wire input_transfer = s_axis_tvalid && s_axis_tready;
    wire [1:0] assembly_lane = input_word_count;
    wire output_transfer = m_axis_tvalid && m_axis_tready;
    wire load_first_buffer_beat = buffered_frame_ready && !m_axis_tvalid;
    wire load_next_buffer_beat = output_transfer && !m_axis_tlast;
    wire frame_buffer_read_enable = load_first_buffer_beat ||
                                    load_next_buffer_beat;
    wire [BEAT_ADDR_WIDTH-1:0] frame_buffer_read_index =
        load_next_buffer_beat ? output_beat_index + 1'b1 :
                                {BEAT_ADDR_WIDTH{1'b0}};
    wire payload_word = s_axis_tkeep == 2'b11;
    wire session_eos = s_axis_tkeep == 2'b00 && s_axis_tlast;

    reg [63:0] assembly_with_input;
    reg [7:0] keep_with_input;
    always @* begin
        assembly_with_input = assembly_data;
        case (assembly_lane)
            2'd0: assembly_with_input[15:0] = s_axis_tdata;
            2'd1: assembly_with_input[31:16] = s_axis_tdata;
            2'd2: assembly_with_input[47:32] = s_axis_tdata;
            default: assembly_with_input[63:48] = s_axis_tdata;
        endcase

        case (assembly_lane)
            2'd0: keep_with_input = 8'h03;
            2'd1: keep_with_input = 8'h0f;
            2'd2: keep_with_input = 8'h3f;
            default: keep_with_input = 8'hff;
        endcase
    end

    // A single frame buffer deliberately backpressures the next frame until
    // the stored packet has been transferred. Downstream stalls never affect
    // reception of the frame currently being collected.
    assign s_axis_tready = resetn && !buffered_frame_ready &&
                           !m_axis_tvalid;
    assign in_frame = dropping_frame ||
        input_word_count != {WORD_COUNT_WIDTH{1'b0}};
    assign idle = !in_frame && !buffered_frame_ready && !m_axis_tvalid;

    always @(posedge clk) begin
        if (!resetn) begin
            assembly_data <= 64'd0;
            input_word_count <= {WORD_COUNT_WIDTH{1'b0}};
            write_beat_index <= {BEAT_ADDR_WIDTH{1'b0}};
            stored_last_index <= {BEAT_ADDR_WIDTH{1'b0}};
            stored_last_keep <= 8'd0;
            output_beat_index <= {BEAT_ADDR_WIDTH{1'b0}};
            buffered_frame_ready <= 1'b0;
            dropping_frame <= 1'b0;
            m_axis_tdata <= 64'd0;
            m_axis_tkeep <= 8'd0;
            m_axis_tvalid <= 1'b0;
            m_axis_tlast <= 1'b0;
            fault_pulse <= 1'b0;
        end else begin
            fault_pulse <= 1'b0;

            // One synchronous RAM read port supplies the output register.
            // Select beat zero at packet launch and the next beat after each
            // accepted non-final output.
            if (frame_buffer_read_enable)
                m_axis_tdata <= frame_buffer[frame_buffer_read_index];

            // Read the RAM into the output register only after the complete
            // frame commits. Keep that register unchanged while stalled.
            if (output_transfer) begin
                if (m_axis_tlast) begin
                    m_axis_tvalid <= 1'b0;
                    m_axis_tlast <= 1'b0;
                    buffered_frame_ready <= 1'b0;
                    output_beat_index <= {BEAT_ADDR_WIDTH{1'b0}};
                end else begin
                    output_beat_index <= output_beat_index + 1'b1;
                    if ((output_beat_index + 1'b1) == stored_last_index) begin
                        m_axis_tkeep <= stored_last_keep;
                        m_axis_tlast <= 1'b1;
                    end else begin
                        m_axis_tkeep <= 8'hff;
                        m_axis_tlast <= 1'b0;
                    end
                end
            end else if (buffered_frame_ready && !m_axis_tvalid) begin
                output_beat_index <= {BEAT_ADDR_WIDTH{1'b0}};
                m_axis_tkeep <= (stored_last_index == 0) ?
                    stored_last_keep : 8'hff;
                m_axis_tlast <= stored_last_index == 0;
                m_axis_tvalid <= 1'b1;
            end

            if (input_transfer) begin
                if (dropping_frame) begin
                    // The first bad beat already reported the fault. Consume
                    // through the raw frame boundary without reporting tails.
                    if (s_axis_tlast) begin
                        dropping_frame <= 1'b0;
                    end
                end else if (payload_word) begin
                    if (input_word_count >= MAX_FRAME_WORD_COUNT) begin
                        // This beat exceeds capacity. No part of the frame has
                        // reached the output, so discard the frame atomically.
                        fault_pulse <= 1'b1;
                        assembly_data <= 64'd0;
                        input_word_count <= {WORD_COUNT_WIDTH{1'b0}};
                        write_beat_index <= {BEAT_ADDR_WIDTH{1'b0}};
                        dropping_frame <= !s_axis_tlast;
                    end else if (s_axis_tlast) begin
                        frame_buffer[write_beat_index] <= assembly_with_input;
                        stored_last_index <= write_beat_index;
                        stored_last_keep <= keep_with_input;
                        buffered_frame_ready <= 1'b1;
                        assembly_data <= 64'd0;
                        input_word_count <= {WORD_COUNT_WIDTH{1'b0}};
                        write_beat_index <= {BEAT_ADDR_WIDTH{1'b0}};
                    end else begin
                        input_word_count <= input_word_count + 1'b1;
                        if (assembly_lane == 2'd3) begin
                            frame_buffer[write_beat_index] <=
                                assembly_with_input;
                            write_beat_index <= write_beat_index + 1'b1;
                            assembly_data <= 64'd0;
                        end else begin
                            assembly_data <= assembly_with_input;
                        end
                    end
                end else if (session_eos) begin
                    // Boundary EOS is metadata only. Inside a frame it marks
                    // a truncation; the buffered frame is discarded.
                    if (in_frame)
                        fault_pulse <= 1'b1;
                    assembly_data <= 64'd0;
                    input_word_count <= {WORD_COUNT_WIDTH{1'b0}};
                    write_beat_index <= {BEAT_ADDR_WIDTH{1'b0}};
                end else begin
                    // Any other TKEEP/TLAST combination corrupts the current
                    // raw frame. Drain it through TLAST before accepting anew.
                    fault_pulse <= 1'b1;
                    assembly_data <= 64'd0;
                    input_word_count <= {WORD_COUNT_WIDTH{1'b0}};
                    write_beat_index <= {BEAT_ADDR_WIDTH{1'b0}};
                    dropping_frame <= !s_axis_tlast;
                end
            end
        end
    end

`ifndef SYNTHESIS
    initial begin
        if (MAX_FRAME_WORDS < 1)
            $fatal(1, "MAX_FRAME_WORDS must be positive");
    end
`endif
endmodule

`default_nettype wire
