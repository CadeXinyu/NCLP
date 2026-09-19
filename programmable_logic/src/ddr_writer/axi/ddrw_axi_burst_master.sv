`timescale 1ns / 1ps
`default_nettype none

// Simple, one-outstanding AXI4 write master.
//
// A command describes one aligned INCR burst of 1..16 64-bit beats.  The
// command is accepted before any write data, and completion is reported only
// after the matching B response is accepted.  The upstream data source must
// obey the normal ready/valid rule and hold s_data/s_strb stable while stalled.
module ddrw_axi_burst_master #(
    parameter integer AXI_ADDR_WIDTH = 49,
    parameter integer AXI_ID_WIDTH   = 1
)(
    input  wire                         clk,
    input  wire                         rst,

    input  wire [AXI_ADDR_WIDTH-1:0]    cmd_addr,
    input  wire [4:0]                   cmd_beats,
    input  wire                         cmd_valid,
    output wire                         cmd_ready,

    input  wire [63:0]                  s_data,
    input  wire [7:0]                   s_strb,
    input  wire                         s_valid,
    output wire                         s_ready,

    output reg                          done_valid,
    output reg  [1:0]                   done_bresp,
    output wire                         busy,

    output wire [AXI_ID_WIDTH-1:0]      m_axi_awid,
    output wire [AXI_ADDR_WIDTH-1:0]    m_axi_awaddr,
    output wire [7:0]                   m_axi_awlen,
    output wire [2:0]                   m_axi_awsize,
    output wire [1:0]                   m_axi_awburst,
    output wire                         m_axi_awlock,
    output wire [3:0]                   m_axi_awcache,
    output wire [2:0]                   m_axi_awprot,
    output wire                         m_axi_awvalid,
    input  wire                         m_axi_awready,

    output wire [63:0]                  m_axi_wdata,
    output wire [7:0]                   m_axi_wstrb,
    output wire                         m_axi_wlast,
    output wire                         m_axi_wvalid,
    input  wire                         m_axi_wready,

    input  wire [AXI_ID_WIDTH-1:0]      m_axi_bid,
    input  wire [1:0]                   m_axi_bresp,
    input  wire                         m_axi_bvalid,
    output wire                         m_axi_bready
);

    localparam [1:0] STATE_IDLE     = 2'd0;
    localparam [1:0] STATE_ADDRESS  = 2'd1;
    localparam [1:0] STATE_DATA     = 2'd2;
    localparam [1:0] STATE_RESPONSE = 2'd3;

    reg [1:0]                    state;
    reg [AXI_ADDR_WIDTH-1:0]     command_addr;
    // One zero-based final index owns both AWLEN and WLAST.
    reg [3:0]                    command_last_beat_index;
    reg [4:0]                    beat_index;

    wire command_length_valid = (cmd_beats >= 5'd1) && (cmd_beats <= 5'd16);
    wire last_data_beat = (beat_index == {1'b0, command_last_beat_index});
    wire address_handshake = m_axi_awvalid && m_axi_awready;
    wire data_handshake = m_axi_wvalid && m_axi_wready;
    wire response_handshake = m_axi_bvalid && m_axi_bready;

    assign cmd_ready = (state == STATE_IDLE) && command_length_valid;
    assign busy = (state != STATE_IDLE);

    assign m_axi_awid    = {AXI_ID_WIDTH{1'b0}};
    assign m_axi_awaddr  = command_addr;
    assign m_axi_awlen   = {4'd0, command_last_beat_index};
    assign m_axi_awsize  = 3'b011;  // 8 bytes per beat
    assign m_axi_awburst = 2'b01;   // INCR
    assign m_axi_awlock  = 1'b0;
    assign m_axi_awcache = 4'b0011;
    assign m_axi_awprot  = 3'b010;
    assign m_axi_awvalid = (state == STATE_ADDRESS);

    assign m_axi_wdata  = s_data;
    assign m_axi_wstrb  = s_strb;
    assign m_axi_wlast  = (state == STATE_DATA) && last_data_beat;
    assign m_axi_wvalid = (state == STATE_DATA) && s_valid;
    assign s_ready      = (state == STATE_DATA) && m_axi_wready;

    // With one outstanding transaction, every accepted B response belongs to
    // the currently active command.  BID is intentionally not used.
    assign m_axi_bready = (state == STATE_RESPONSE);

    always @(posedge clk) begin
        if (rst) begin
            state         <= STATE_IDLE;
            command_addr  <= {AXI_ADDR_WIDTH{1'b0}};
            command_last_beat_index <= 4'd0;
            beat_index    <= 5'd0;
            done_valid    <= 1'b0;
            done_bresp    <= 2'b00;
        end else begin
            done_valid <= 1'b0;

            case (state)
                STATE_IDLE: begin
                    beat_index <= 5'd0;
                    if (cmd_valid && cmd_ready) begin
                        command_addr  <= cmd_addr;
                        command_last_beat_index <= cmd_beats[3:0] - 4'd1;
                        state         <= STATE_ADDRESS;
                    end
                end

                STATE_ADDRESS: begin
                    if (address_handshake) begin
                        state <= STATE_DATA;
                    end
                end

                STATE_DATA: begin
                    if (data_handshake) begin
                        if (last_data_beat) begin
                            state <= STATE_RESPONSE;
                        end else begin
                            beat_index <= beat_index + 5'd1;
                        end
                    end
                end

                STATE_RESPONSE: begin
                    if (response_handshake) begin
                        done_bresp <= m_axi_bresp;
                        done_valid <= 1'b1;
                        state      <= STATE_IDLE;
                    end
                end

                default: state <= STATE_IDLE;
            endcase
        end
    end

`ifndef SYNTHESIS
    always @(posedge clk) begin
        if (!rst && cmd_valid && (state == STATE_IDLE) && !command_length_valid) begin
            $error("ddrw_axi_burst_master: cmd_beats must be in the range 1..16");
        end
    end
`endif

endmodule

`default_nettype wire
