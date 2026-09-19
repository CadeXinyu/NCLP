`timescale 1ns / 1ps
`default_nettype none

// Vivado IP Integrator module-reference wrapper for the integrated NCLP Intan SPI top.
// The implementation is NCLP_Intan_SPI_Module.sv; this wrapper only exposes
// Vivado-friendly AXI/AXIS attributes.
module NCLP_Intan_SPI_Module_bd #(
    // Keep the public BD default equal to the implementation default.
    // FIFO capacities are payload bytes; reset release is in AXI clock cycles.
    parameter integer RECORDING_EVENT_FIFO_DEPTH_BYTES = 2048,
    parameter integer COMPUTE_EVENT_FIFO_DEPTH_BYTES = 2048,
    parameter integer CLOCK_RESET_RELEASE_CYCLES = 1024,
    parameter integer C_S00_AXI_DATA_WIDTH = 32,
    parameter integer C_S00_AXI_ADDR_WIDTH = 16
)(
    (* X_INTERFACE_INFO = "xilinx.com:signal:clock:1.0 s00_axi_aclk CLK" *)
    (* X_INTERFACE_PARAMETER = "FREQ_HZ 100000000, PHASE 0.0, ASSOCIATED_BUSIF S00_AXI:M_AXIS_RECORDING:M_AXIS_COMPUTE, ASSOCIATED_RESET reset" *)
    input  wire        s00_axi_aclk,
    (* X_INTERFACE_INFO = "xilinx.com:signal:clock:1.0 clkgen_ref_clk CLK" *)
    (* X_INTERFACE_PARAMETER = "FREQ_HZ 140000000, PHASE 0.0" *)
    input  wire        clkgen_ref_clk,
    (* X_INTERFACE_INFO = "xilinx.com:signal:reset:1.0 reset RST" *)
    (* X_INTERFACE_PARAMETER = "POLARITY ACTIVE_HIGH" *)
    input  wire        reset,

    input  wire        miso_a1,
    input  wire        miso_a2,
    input  wire        miso_b1,
    input  wire        miso_b2,
    input  wire        miso_c1,
    input  wire        miso_c2,
    input  wire        miso_d1,
    input  wire        miso_d2,

    output wire        spi_running,
    output wire        compute_stream_active,
    output wire        acquisition_30ksps,
    (* X_INTERFACE_INFO = "xilinx.com:signal:interrupt:1.0 intan_error_irq INTERRUPT" *)
    (* X_INTERFACE_PARAMETER = "SENSITIVITY LEVEL_HIGH" *)
    output wire        intan_error_irq,
    output wire        sclk,
    output wire        cs,
    output wire        mosi_a,
    output wire        mosi_b,
    output wire        mosi_c,
    output wire        mosi_d,
    output wire        intan_sync_out,

    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 M_AXIS_RECORDING TDATA" *)
    (* X_INTERFACE_MODE = "master M_AXIS_RECORDING" *)
    (* X_INTERFACE_PARAMETER = "FREQ_HZ 100000000, TDATA_NUM_BYTES 2, HAS_TKEEP 1, HAS_TLAST 1" *)
    output wire [15:0] m_axis_recording_tdata,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 M_AXIS_RECORDING TKEEP" *)
    output wire [1:0]  m_axis_recording_tkeep,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 M_AXIS_RECORDING TVALID" *)
    output wire        m_axis_recording_tvalid,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 M_AXIS_RECORDING TREADY" *)
    input  wire        m_axis_recording_tready,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 M_AXIS_RECORDING TLAST" *)
    output wire        m_axis_recording_tlast,

    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 M_AXIS_COMPUTE TDATA" *)
    (* X_INTERFACE_MODE = "master M_AXIS_COMPUTE" *)
    (* X_INTERFACE_PARAMETER = "FREQ_HZ 100000000, TDATA_NUM_BYTES 2, HAS_TKEEP 1, HAS_TLAST 1" *)
    output wire [15:0] m_axis_compute_tdata,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 M_AXIS_COMPUTE TKEEP" *)
    output wire [1:0]  m_axis_compute_tkeep,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 M_AXIS_COMPUTE TVALID" *)
    output wire        m_axis_compute_tvalid,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 M_AXIS_COMPUTE TREADY" *)
    input  wire        m_axis_compute_tready,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 M_AXIS_COMPUTE TLAST" *)
    output wire        m_axis_compute_tlast,

    input  wire [1:0]  ttl_in_external,
    input  wire        intan_marker_active,
    input  wire [13:0] intan_marker_mask,

    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S00_AXI AWADDR" *)
    (* X_INTERFACE_MODE = "slave S00_AXI" *)
    (* X_INTERFACE_PARAMETER = "DATA_WIDTH 32, PROTOCOL AXI4LITE, FREQ_HZ 100000000" *)
    input  wire [C_S00_AXI_ADDR_WIDTH-1 : 0]     s00_axi_awaddr,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S00_AXI AWPROT" *)
    input  wire [2 : 0]                          s00_axi_awprot,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S00_AXI AWVALID" *)
    input  wire                                  s00_axi_awvalid,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S00_AXI AWREADY" *)
    output wire                                  s00_axi_awready,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S00_AXI WDATA" *)
    input  wire [C_S00_AXI_DATA_WIDTH-1 : 0]     s00_axi_wdata,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S00_AXI WSTRB" *)
    input  wire [(C_S00_AXI_DATA_WIDTH/8)-1 : 0] s00_axi_wstrb,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S00_AXI WVALID" *)
    input  wire                                  s00_axi_wvalid,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S00_AXI WREADY" *)
    output wire                                  s00_axi_wready,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S00_AXI BRESP" *)
    output wire [1 : 0]                          s00_axi_bresp,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S00_AXI BVALID" *)
    output wire                                  s00_axi_bvalid,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S00_AXI BREADY" *)
    input  wire                                  s00_axi_bready,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S00_AXI ARADDR" *)
    input  wire [C_S00_AXI_ADDR_WIDTH-1 : 0]     s00_axi_araddr,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S00_AXI ARPROT" *)
    input  wire [2 : 0]                          s00_axi_arprot,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S00_AXI ARVALID" *)
    input  wire                                  s00_axi_arvalid,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S00_AXI ARREADY" *)
    output wire                                  s00_axi_arready,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S00_AXI RDATA" *)
    output wire [C_S00_AXI_DATA_WIDTH-1 : 0]     s00_axi_rdata,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S00_AXI RRESP" *)
    output wire [1 : 0]                          s00_axi_rresp,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S00_AXI RVALID" *)
    output wire                                  s00_axi_rvalid,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S00_AXI RREADY" *)
    input  wire                                  s00_axi_rready
);

    NCLP_Intan_SPI_Module #(
        .RECORDING_EVENT_FIFO_DEPTH_BYTES(RECORDING_EVENT_FIFO_DEPTH_BYTES),
        .COMPUTE_EVENT_FIFO_DEPTH_BYTES(COMPUTE_EVENT_FIFO_DEPTH_BYTES),
        .CLOCK_RESET_RELEASE_CYCLES(CLOCK_RESET_RELEASE_CYCLES),
        .C_S00_AXI_DATA_WIDTH(C_S00_AXI_DATA_WIDTH),
        .C_S00_AXI_ADDR_WIDTH(C_S00_AXI_ADDR_WIDTH)
    ) impl (
        .s00_axi_aclk(s00_axi_aclk),
        .clkgen_ref_clk(clkgen_ref_clk),
        .reset(reset),
        .miso_a1(miso_a1), .miso_a2(miso_a2), .miso_b1(miso_b1), .miso_b2(miso_b2),
        .miso_c1(miso_c1), .miso_c2(miso_c2), .miso_d1(miso_d1), .miso_d2(miso_d2),
        .spi_running(spi_running),
        .compute_stream_active(compute_stream_active),
        .acquisition_30ksps(acquisition_30ksps),
        .intan_error_irq(intan_error_irq),
        .sclk(sclk), .cs(cs), .mosi_a(mosi_a), .mosi_b(mosi_b), .mosi_c(mosi_c), .mosi_d(mosi_d),
        .intan_sync_out(intan_sync_out),
        .m_axis_recording_tdata(m_axis_recording_tdata),
        .m_axis_recording_tkeep(m_axis_recording_tkeep),
        .m_axis_recording_tvalid(m_axis_recording_tvalid),
        .m_axis_recording_tready(m_axis_recording_tready),
        .m_axis_recording_tlast(m_axis_recording_tlast),
        .m_axis_compute_tdata(m_axis_compute_tdata),
        .m_axis_compute_tkeep(m_axis_compute_tkeep),
        .m_axis_compute_tvalid(m_axis_compute_tvalid),
        .m_axis_compute_tready(m_axis_compute_tready),
        .m_axis_compute_tlast(m_axis_compute_tlast),
        .ttl_in_external(ttl_in_external),
        .intan_marker_active(intan_marker_active),
        .intan_marker_mask(intan_marker_mask),
        .s00_axi_awaddr(s00_axi_awaddr), .s00_axi_awprot(s00_axi_awprot), .s00_axi_awvalid(s00_axi_awvalid), .s00_axi_awready(s00_axi_awready),
        .s00_axi_wdata(s00_axi_wdata), .s00_axi_wstrb(s00_axi_wstrb), .s00_axi_wvalid(s00_axi_wvalid), .s00_axi_wready(s00_axi_wready),
        .s00_axi_bresp(s00_axi_bresp), .s00_axi_bvalid(s00_axi_bvalid), .s00_axi_bready(s00_axi_bready),
        .s00_axi_araddr(s00_axi_araddr), .s00_axi_arprot(s00_axi_arprot), .s00_axi_arvalid(s00_axi_arvalid), .s00_axi_arready(s00_axi_arready),
        .s00_axi_rdata(s00_axi_rdata), .s00_axi_rresp(s00_axi_rresp), .s00_axi_rvalid(s00_axi_rvalid), .s00_axi_rready(s00_axi_rready)
    );

endmodule

`default_nettype wire
