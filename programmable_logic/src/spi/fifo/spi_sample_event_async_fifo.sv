`timescale 1ns / 1ps
`default_nettype none

// ============================================================================
// spi_sample_event_async_fifo
// ============================================================================
// Small wrapper around the project AXIS async FIFO used to move an ordered copy
// of the compact acquisition stream from spi_clk into the AXI clock domain.
// Normal events carry one 16-bit word, including headers, samples, and TTL metadata.
// The owner supplies TLAST semantics: recording uses it only for zero-byte EOS,
// while compute also marks the final TTL word of each complete frame. DEPTH_BYTES
// is storage capacity; m_occupancy_bytes includes slots occupied by EOS.
// ============================================================================

module spi_sample_event_async_fifo #(
    parameter integer DEPTH_BYTES = 1024
)(
    input  wire        s_clk,
    input  wire        s_rst,
    input  wire [15:0] s_axis_tdata,
    input  wire [1:0]  s_axis_tkeep,
    input  wire        s_axis_tvalid,
    output wire        s_axis_tready,
    input  wire        s_axis_tlast,

    input  wire        m_clk,
    input  wire        m_rst,
    output wire [15:0] m_axis_tdata,
    output wire [1:0]  m_axis_tkeep,
    output wire        m_axis_tvalid,
    input  wire        m_axis_tready,
    output wire        m_axis_tlast,

    output wire        s_overflow,
    output wire [$clog2(DEPTH_BYTES):0] m_occupancy_bytes
);

    axis_async_fifo #(
        .DEPTH(DEPTH_BYTES),
        .DATA_WIDTH(16),
        .KEEP_ENABLE(1),
        .KEEP_WIDTH(2),
        .LAST_ENABLE(1),
        .ID_ENABLE(0),
        .ID_WIDTH(8),
        .DEST_ENABLE(0),
        .DEST_WIDTH(8),
        .USER_ENABLE(0),
        .USER_WIDTH(1),
        .FRAME_FIFO(0),
        .DROP_BAD_FRAME(0),
        .DROP_WHEN_FULL(0)
    ) axis_async_fifo_inst (
        .s_clk(s_clk),
        .s_rst(s_rst),
        .s_axis_tdata(s_axis_tdata),
        .s_axis_tkeep(s_axis_tkeep),
        .s_axis_tvalid(s_axis_tvalid),
        .s_axis_tready(s_axis_tready),
        .s_axis_tlast(s_axis_tlast),
        .s_axis_tid(8'd0),
        .s_axis_tdest(8'd0),
        .s_axis_tuser(1'b0),
        .m_clk(m_clk),
        .m_rst(m_rst),
        .m_axis_tdata(m_axis_tdata),
        .m_axis_tkeep(m_axis_tkeep),
        .m_axis_tvalid(m_axis_tvalid),
        .m_axis_tready(m_axis_tready),
        .m_axis_tlast(m_axis_tlast),
        .m_axis_tid(),
        .m_axis_tdest(),
        .m_axis_tuser(),
        .s_pause_req(1'b0),
        .s_pause_ack(),
        .m_pause_req(1'b0),
        .m_pause_ack(),
        .s_status_depth(),
        .s_status_depth_commit(),
        .s_status_overflow(s_overflow),
        .s_status_bad_frame(),
        .s_status_good_frame(),
        .m_status_depth(m_occupancy_bytes),
        .m_status_depth_commit(),
        .m_status_overflow(),
        .m_status_bad_frame(),
        .m_status_good_frame()
    );

endmodule

`default_nettype wire
