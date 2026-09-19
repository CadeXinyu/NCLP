`timescale 1ns / 1ps
`default_nettype none

// AXI4-Lite transport shared by the small NCLP register peripherals.
//
// AW and W are accepted independently and paired in acceptance order.  Read
// data is sampled on the AR handshake and then held until RREADY.  Peripheral
// logic only sees one-cycle register-write pulses and a combinational read
// address/data interface, keeping AXI channel state out of each register bank.
// AWPROT/ARPROT are accepted as part of AXI but do not affect register access.
module axil_register_slave #(
    parameter integer DATA_WIDTH = 32,
    parameter integer ADDR_WIDTH = 6
)(
    input  wire                          clk,
    input  wire                          resetn,

    input  wire [ADDR_WIDTH-1:0]         s_axil_awaddr,
    input  wire [2:0]                    s_axil_awprot,
    input  wire                          s_axil_awvalid,
    output wire                          s_axil_awready,

    input  wire [DATA_WIDTH-1:0]         s_axil_wdata,
    input  wire [(DATA_WIDTH/8)-1:0]     s_axil_wstrb,
    input  wire                          s_axil_wvalid,
    output wire                          s_axil_wready,

    output reg  [1:0]                    s_axil_bresp,
    output reg                           s_axil_bvalid,
    input  wire                          s_axil_bready,

    input  wire [ADDR_WIDTH-1:0]         s_axil_araddr,
    input  wire [2:0]                    s_axil_arprot,
    input  wire                          s_axil_arvalid,
    output wire                          s_axil_arready,

    output reg  [DATA_WIDTH-1:0]         s_axil_rdata,
    output reg  [1:0]                    s_axil_rresp,
    output reg                           s_axil_rvalid,
    input  wire                          s_axil_rready,

    output reg                           reg_wr_en,
    output reg  [ADDR_WIDTH-1:0]         reg_wr_addr,
    output reg  [DATA_WIDTH-1:0]         reg_wr_data,
    output reg  [(DATA_WIDTH/8)-1:0]     reg_wr_strb,

    output wire [ADDR_WIDTH-1:0]         reg_rd_addr,
    input  wire [DATA_WIDTH-1:0]         reg_rd_data
);

    localparam integer STRB_WIDTH = DATA_WIDTH / 8;
    localparam [1:0] AXI_RESP_OKAY = 2'b00;

    reg                      aw_pending;
    reg [ADDR_WIDTH-1:0]     awaddr_pending;
    reg                      w_pending;
    reg [DATA_WIDTH-1:0]     wdata_pending;
    reg [STRB_WIDTH-1:0]     wstrb_pending;

    wire aw_handshake = s_axil_awvalid && s_axil_awready;
    wire w_handshake  = s_axil_wvalid  && s_axil_wready;
    wire ar_handshake = s_axil_arvalid && s_axil_arready;

    wire write_address_available = aw_pending || aw_handshake;
    wire write_data_available = w_pending || w_handshake;
    wire write_response_available = !s_axil_bvalid || s_axil_bready;
    wire issue_register_write = write_address_available &&
                                write_data_available &&
                                write_response_available;

    assign s_axil_awready = !aw_pending;
    assign s_axil_wready = !w_pending;

    // A new read may replace the prior response on the same edge that the
    // prior response is accepted, but never while a response is stalled.
    assign s_axil_arready = !s_axil_rvalid || s_axil_rready;
    assign reg_rd_addr = s_axil_araddr;

    initial begin
        if (DATA_WIDTH <= 0 || (DATA_WIDTH % 8) != 0) begin
            $error("axil_register_slave: DATA_WIDTH must be a positive multiple of 8");
            $finish;
        end
        if (ADDR_WIDTH <= 0) begin
            $error("axil_register_slave: ADDR_WIDTH must be positive");
            $finish;
        end
    end

    always @(posedge clk) begin
        if (!resetn) begin
            aw_pending     <= 1'b0;
            awaddr_pending <= {ADDR_WIDTH{1'b0}};
            w_pending      <= 1'b0;
            wdata_pending  <= {DATA_WIDTH{1'b0}};
            wstrb_pending  <= {STRB_WIDTH{1'b0}};
            s_axil_bresp   <= AXI_RESP_OKAY;
            s_axil_bvalid  <= 1'b0;
            reg_wr_en      <= 1'b0;
            reg_wr_addr    <= {ADDR_WIDTH{1'b0}};
            reg_wr_data    <= {DATA_WIDTH{1'b0}};
            reg_wr_strb    <= {STRB_WIDTH{1'b0}};
        end else begin
            reg_wr_en <= 1'b0;

            if (issue_register_write) begin
                reg_wr_en   <= 1'b1;
                reg_wr_addr <= aw_pending ? awaddr_pending : s_axil_awaddr;
                reg_wr_data <= w_pending ? wdata_pending : s_axil_wdata;
                reg_wr_strb <= w_pending ? wstrb_pending : s_axil_wstrb;
                aw_pending  <= 1'b0;
                w_pending   <= 1'b0;
                s_axil_bresp  <= AXI_RESP_OKAY;
                s_axil_bvalid <= 1'b1;
            end else begin
                if (aw_handshake) begin
                    aw_pending     <= 1'b1;
                    awaddr_pending <= s_axil_awaddr;
                end
                if (w_handshake) begin
                    w_pending     <= 1'b1;
                    wdata_pending <= s_axil_wdata;
                    wstrb_pending <= s_axil_wstrb;
                end
                if (s_axil_bvalid && s_axil_bready) begin
                    s_axil_bvalid <= 1'b0;
                end
            end
        end
    end

    always @(posedge clk) begin
        if (!resetn) begin
            s_axil_rdata  <= {DATA_WIDTH{1'b0}};
            s_axil_rresp  <= AXI_RESP_OKAY;
            s_axil_rvalid <= 1'b0;
        end else if (ar_handshake) begin
            s_axil_rdata  <= reg_rd_data;
            s_axil_rresp  <= AXI_RESP_OKAY;
            s_axil_rvalid <= 1'b1;
        end else if (s_axil_rvalid && s_axil_rready) begin
            s_axil_rvalid <= 1'b0;
        end
    end

endmodule

`default_nettype wire
