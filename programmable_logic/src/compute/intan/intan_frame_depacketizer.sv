`timescale 1ns/1ps
`default_nettype none

// Convert the compact Intan acquisition stream into cut-through local events.
// The compact frame is:
//   magic[63:0], timestamp[31:0], 35 channel-major stream rows, TTL[15:0].
// Disabled streams are absent, so the exact 16-bit source mask is snapshotted
// at each frame boundary.  The final TTL input word carries the frame TLAST.
// A zero-keep TLAST remains the independent acquisition-session EOS marker.
//
// Local event format:
//   [63:32] timestamp
//   [31:30] event kind: 00 amplifier, 01 AUX reply, 10 TTL
//   [29:25] flags (zero)
//   [24:21] logical stream ID
//   [20:16] amplifier channel or AUX slot
//   [15:0]  value
module intan_frame_depacketizer (
    input  wire        clk,
    input  wire        resetn,
    input  wire [15:0] source_stream_mask,

    input  wire [15:0] s_axis_raw_frame_tdata,
    input  wire [1:0]  s_axis_raw_frame_tkeep,
    input  wire        s_axis_raw_frame_tvalid,
    output wire        s_axis_raw_frame_tready,
    input  wire        s_axis_raw_frame_tlast,

    output wire [63:0] m_axis_sample_tdata,
    output wire [7:0]  m_axis_sample_tkeep,
    output wire        m_axis_sample_tvalid,
    input  wire        m_axis_sample_tready,
    output wire        m_axis_sample_tlast,

    output wire        decoder_idle,
    output wire        decoder_in_frame,
    output reg         decode_fault_pulse
);
    localparam [15:0] MAGIC_0 = 16'h1942;
    localparam [15:0] MAGIC_1 = 16'h2702;
    localparam [15:0] MAGIC_2 = 16'h1999;
    localparam [15:0] MAGIC_3 = 16'hc691;

    localparam [3:0] EXPECT_MAGIC_0 = 4'd0;
    localparam [3:0] EXPECT_MAGIC_1 = 4'd1;
    localparam [3:0] EXPECT_MAGIC_2 = 4'd2;
    localparam [3:0] EXPECT_MAGIC_3 = 4'd3;
    localparam [3:0] EXPECT_TIME_LO = 4'd4;
    localparam [3:0] EXPECT_TIME_HI = 4'd5;
    localparam [3:0] EXPECT_SAMPLES = 4'd6;
    localparam [3:0] EXPECT_TTL     = 4'd7;

    localparam [1:0] EVENT_KIND_AMPLIFIER = 2'b00;
    localparam [1:0] EVENT_KIND_AUX_REPLY = 2'b01;
    localparam [1:0] EVENT_KIND_TTL       = 2'b10;

    reg [3:0]  parser_state;
    reg [15:0] frame_stream_mask;
    reg [31:0] frame_timestamp;
    reg [5:0]  sample_row_index;
    reg [3:0]  stream_index;

    reg [63:0] event_tdata;
    reg [7:0]  event_tkeep;
    reg        event_tvalid;
    reg        event_tlast;

    wire output_transfer = event_tvalid && m_axis_sample_tready;
    wire output_slot_available = !event_tvalid || m_axis_sample_tready;
    assign s_axis_raw_frame_tready = resetn && output_slot_available;
    wire input_transfer = s_axis_raw_frame_tvalid && s_axis_raw_frame_tready;
    wire input_eos = (s_axis_raw_frame_tkeep == 2'b00) && s_axis_raw_frame_tlast;
    wire input_has_payload = s_axis_raw_frame_tkeep == 2'b11;

    assign m_axis_sample_tdata = event_tdata;
    assign m_axis_sample_tkeep = event_tkeep;
    assign m_axis_sample_tvalid = resetn && event_tvalid;
    assign m_axis_sample_tlast = event_tlast;
    assign decoder_in_frame = parser_state != EXPECT_MAGIC_0;
    assign decoder_idle = (parser_state == EXPECT_MAGIC_0) && !event_tvalid;

    function automatic [3:0] first_enabled_stream(input [15:0] mask);
        integer candidate;
        reg found;
        begin
            first_enabled_stream = 4'd0;
            found = 1'b0;
            for (candidate = 0; candidate < 16; candidate = candidate + 1) begin
                if (!found && mask[candidate]) begin
                    first_enabled_stream = candidate[3:0];
                    found = 1'b1;
                end
            end
        end
    endfunction

    function automatic [3:0] next_enabled_stream(
        input [15:0] mask,
        input [3:0] current_stream
    );
        integer candidate;
        reg found;
        begin
            next_enabled_stream = current_stream;
            found = 1'b0;
            for (candidate = 0; candidate < 16; candidate = candidate + 1) begin
                if (!found && candidate > current_stream && mask[candidate]) begin
                    next_enabled_stream = candidate[3:0];
                    found = 1'b1;
                end
            end
        end
    endfunction

    function automatic is_last_enabled_stream(
        input [15:0] mask,
        input [3:0] current_stream
    );
        integer candidate;
        begin
            is_last_enabled_stream = 1'b1;
            for (candidate = 0; candidate < 16; candidate = candidate + 1) begin
                if (candidate > current_stream && mask[candidate])
                    is_last_enabled_stream = 1'b0;
            end
        end
    endfunction

    task automatic restart_from_payload(input [15:0] payload);
        begin
            if ((payload == MAGIC_0) && (source_stream_mask != 16'd0)) begin
                frame_stream_mask <= source_stream_mask;
                parser_state <= EXPECT_MAGIC_1;
            end else begin
                parser_state <= EXPECT_MAGIC_0;
            end
        end
    endtask

    always @(posedge clk) begin
        if (!resetn) begin
            parser_state <= EXPECT_MAGIC_0;
            frame_stream_mask <= 16'h0001;
            frame_timestamp <= 32'd0;
            sample_row_index <= 6'd0;
            stream_index <= 4'd0;
            event_tdata <= 64'd0;
            event_tkeep <= 8'd0;
            event_tvalid <= 1'b0;
            event_tlast <= 1'b0;
            decode_fault_pulse <= 1'b0;
        end else begin
            decode_fault_pulse <= 1'b0;

            if (output_transfer)
                event_tvalid <= 1'b0;

            if (input_transfer) begin
                    if (input_eos) begin
                        // A session always gets a local EOS, even if the final
                        // compact frame was truncated.  This lets the local
                        // consumer drain and makes the fault independently
                        // visible through the register bank.
                        if (parser_state != EXPECT_MAGIC_0)
                            decode_fault_pulse <= 1'b1;
                        parser_state <= EXPECT_MAGIC_0;
                        event_tdata <= 64'd0;
                        event_tkeep <= 8'h00;
                        event_tvalid <= 1'b1;
                        event_tlast <= 1'b1;
                    end else if (!input_has_payload) begin
                        decode_fault_pulse <= 1'b1;
                        parser_state <= EXPECT_MAGIC_0;
                    end else if (s_axis_raw_frame_tlast &&
                                 (parser_state != EXPECT_TTL)) begin
                        // A payload TLAST before the exact TTL position is a
                        // truncated frame.  Do not publish the premature word.
                        decode_fault_pulse <= 1'b1;
                        parser_state <= EXPECT_MAGIC_0;
                    end else if (!s_axis_raw_frame_tlast &&
                                 (parser_state == EXPECT_TTL)) begin
                        // The TTL value is only valid when it closes the frame.
                        decode_fault_pulse <= 1'b1;
                        parser_state <= EXPECT_MAGIC_0;
                    end else begin
                        case (parser_state)
                            EXPECT_MAGIC_0: begin
                                if ((s_axis_raw_frame_tdata == MAGIC_0) &&
                                    (source_stream_mask != 16'd0)) begin
                                    frame_stream_mask <= source_stream_mask;
                                    parser_state <= EXPECT_MAGIC_1;
                                end else begin
                                    decode_fault_pulse <= 1'b1;
                                end
                            end
                            EXPECT_MAGIC_1: begin
                                if (s_axis_raw_frame_tdata == MAGIC_1)
                                    parser_state <= EXPECT_MAGIC_2;
                                else begin
                                    decode_fault_pulse <= 1'b1;
                                    restart_from_payload(s_axis_raw_frame_tdata);
                                end
                            end
                            EXPECT_MAGIC_2: begin
                                if (s_axis_raw_frame_tdata == MAGIC_2)
                                    parser_state <= EXPECT_MAGIC_3;
                                else begin
                                    decode_fault_pulse <= 1'b1;
                                    restart_from_payload(s_axis_raw_frame_tdata);
                                end
                            end
                            EXPECT_MAGIC_3: begin
                                if (s_axis_raw_frame_tdata == MAGIC_3)
                                    parser_state <= EXPECT_TIME_LO;
                                else begin
                                    decode_fault_pulse <= 1'b1;
                                    restart_from_payload(s_axis_raw_frame_tdata);
                                end
                            end
                            EXPECT_TIME_LO: begin
                                frame_timestamp[15:0] <= s_axis_raw_frame_tdata;
                                parser_state <= EXPECT_TIME_HI;
                            end
                            EXPECT_TIME_HI: begin
                                frame_timestamp[31:16] <= s_axis_raw_frame_tdata;
                                sample_row_index <= 6'd0;
                                stream_index <= first_enabled_stream(frame_stream_mask);
                                parser_state <= EXPECT_SAMPLES;
                            end
                            EXPECT_SAMPLES: begin
                                event_tdata <= {
                                    frame_timestamp,
                                    (sample_row_index < 6'd32) ?
                                        EVENT_KIND_AMPLIFIER :
                                        EVENT_KIND_AUX_REPLY,
                                    5'd0,
                                    stream_index,
                                    sample_row_index[4:0],
                                    s_axis_raw_frame_tdata
                                };
                                event_tkeep <= 8'hff;
                                event_tvalid <= 1'b1;
                                event_tlast <= 1'b0;

                                if (is_last_enabled_stream(frame_stream_mask,
                                                           stream_index)) begin
                                    if (sample_row_index == 6'd34) begin
                                        parser_state <= EXPECT_TTL;
                                    end else begin
                                        sample_row_index <= sample_row_index + 6'd1;
                                        stream_index <=
                                            first_enabled_stream(frame_stream_mask);
                                    end
                                end else begin
                                    stream_index <= next_enabled_stream(
                                        frame_stream_mask, stream_index);
                                end
                            end
                            EXPECT_TTL: begin
                                event_tdata <= {
                                    frame_timestamp,
                                    EVENT_KIND_TTL,
                                    5'd0,
                                    4'd0,
                                    5'd0,
                                    s_axis_raw_frame_tdata
                                };
                                event_tkeep <= 8'hff;
                                event_tvalid <= 1'b1;
                                event_tlast <= 1'b1;
                                parser_state <= EXPECT_MAGIC_0;
                            end
                            default: begin
                                decode_fault_pulse <= 1'b1;
                                parser_state <= EXPECT_MAGIC_0;
                            end
                        endcase
                    end
            end
        end
    end

`ifndef SYNTHESIS
    always @(posedge clk) begin
        if (resetn) begin
            assert (source_stream_mask != 16'd0)
                else $error("intan_frame_depacketizer requires a nonzero stream mask");
            assert (!(event_tvalid && event_tkeep != 8'hff &&
                      event_tkeep != 8'h00))
                else $error("local sample output produced an invalid keep value");
            assert (!(event_tvalid && event_tkeep == 8'hff &&
                      event_tlast &&
                      event_tdata[31:30] != EVENT_KIND_TTL))
                else $error("only a TTL event may close a local frame");
        end
    end
`endif
endmodule

`default_nettype wire
