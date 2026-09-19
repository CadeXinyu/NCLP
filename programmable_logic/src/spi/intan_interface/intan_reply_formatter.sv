`timescale 1ns / 1ps
`default_nettype none

// Serializes one aligned 16-stream Intan reply into the compact acquisition
// stream shared by the recording and compute output paths.
//
// The formatter intentionally scans all 16 stream IDs, even when some streams
// are disabled.  This keeps its latency bounded and deterministic while the
// FIFO itself remains compact: disabled streams do not produce FIFO words.
// FRAME_END accompanies the TTL word and does not alter the payload format.
module intan_reply_formatter (
    input  wire         clk,
    input  wire         reset,

    input  wire [15:0]  session_stream_mask,

    input  wire         reply_valid,
    output wire         reply_ready,
    input  wire [5:0]   reply_slot,
    input  wire [31:0]  reply_timestamp,
    input  wire [15:0]  reply_ttl,
    input  wire [255:0] reply_words,
    input  wire         reply_is_final,

    output reg          final_reply_done,

    output reg  [15:0]  raw_frame_word,
    output reg          raw_frame_word_valid,
    // High only with the TTL word that completes a compact Intan frame.
    output reg          raw_frame_word_last
);
    localparam [63:0] FRAME_MAGIC = 64'hC691199927021942;

    localparam [2:0] FORMAT_IDLE    = 3'd0;
    localparam [2:0] FORMAT_PREFIX  = 3'd1;
    localparam [2:0] FORMAT_STREAMS = 3'd2;
    localparam [2:0] FORMAT_TTL     = 3'd3;
    localparam [2:0] FORMAT_DONE    = 3'd4;

    reg [2:0]   format_state;
    reg [2:0]   prefix_index;
    reg [4:0]   stream_index;
    reg [5:0]   saved_slot;
    reg [31:0]  saved_timestamp;
    reg [15:0]  saved_ttl;
    reg [255:0] saved_words;
    reg [15:0]  saved_stream_mask;
    reg         saved_is_final;

    assign reply_ready = (format_state == FORMAT_IDLE);

    always @(posedge clk) begin
        raw_frame_word_valid <= 1'b0;
        raw_frame_word_last <= 1'b0;
        final_reply_done <= 1'b0;

        if (reset) begin
            format_state <= FORMAT_IDLE;
            prefix_index <= 3'd0;
            stream_index <= 5'd0;
            saved_slot <= 6'd0;
            saved_timestamp <= 32'd0;
            saved_ttl <= 16'd0;
            saved_words <= 256'd0;
            saved_stream_mask <= 16'd0;
            saved_is_final <= 1'b0;
            raw_frame_word <= 16'd0;
        end else begin
            case (format_state)
                FORMAT_IDLE: begin
                    if (reply_valid) begin
                        saved_slot <= reply_slot;
                        saved_timestamp <= reply_timestamp;
                        saved_ttl <= reply_ttl;
                        saved_words <= reply_words;
                        saved_stream_mask <= session_stream_mask;
                        saved_is_final <= reply_is_final;
                        stream_index <= 5'd0;
                        if (reply_slot == 6'd0) begin
                            prefix_index <= 3'd0;
                            format_state <= FORMAT_PREFIX;
                        end else begin
                            format_state <= FORMAT_STREAMS;
                        end
                    end
                end

                FORMAT_PREFIX: begin
                    raw_frame_word_valid <= 1'b1;
                    case (prefix_index)
                        3'd0: raw_frame_word <= FRAME_MAGIC[15:0];
                        3'd1: raw_frame_word <= FRAME_MAGIC[31:16];
                        3'd2: raw_frame_word <= FRAME_MAGIC[47:32];
                        3'd3: raw_frame_word <= FRAME_MAGIC[63:48];
                        3'd4: raw_frame_word <= saved_timestamp[15:0];
                        default: raw_frame_word <= saved_timestamp[31:16];
                    endcase
                    if (prefix_index == 3'd5) begin
                        stream_index <= 5'd0;
                        format_state <= FORMAT_STREAMS;
                    end else begin
                        prefix_index <= prefix_index + 3'd1;
                    end
                end

                FORMAT_STREAMS: begin
                    if (stream_index < 5'd16) begin
                        if (saved_stream_mask[stream_index]) begin
                            raw_frame_word <= saved_words[stream_index*16 +: 16];
                            raw_frame_word_valid <= 1'b1;
                        end
                        stream_index <= stream_index + 5'd1;
                    end else if (saved_slot == 6'd34) begin
                        format_state <= FORMAT_TTL;
                    end else begin
                        format_state <= FORMAT_IDLE;
                    end
                end

                FORMAT_TTL: begin
                    raw_frame_word <= saved_ttl;
                    raw_frame_word_valid <= 1'b1;
                    raw_frame_word_last <= 1'b1;
                    format_state <= saved_is_final ? FORMAT_DONE : FORMAT_IDLE;
                end

                FORMAT_DONE: begin
                    // This pulse is deliberately one cycle after the final TTL
                    // word.  The owner of acquisition_session_end can therefore append
                    // EOS only after every payload word has been emitted.
                    final_reply_done <= 1'b1;
                    format_state <= FORMAT_IDLE;
                end

                default: format_state <= FORMAT_IDLE;
            endcase
        end
    end

`ifndef SYNTHESIS
    always @(posedge clk) begin
        if (!reset && reply_is_final && reply_valid && reply_ready)
            assert (reply_slot == 6'd34) else $error("Only AUX3 may be marked as the final reply");
    end
`endif
endmodule

`default_nettype wire
