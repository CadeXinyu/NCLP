`timescale 1ns / 1ps
`default_nettype none

module stim_controller_bd #(
    parameter integer C_S00_AXI_DATA_WIDTH = 32,
    parameter integer C_S00_AXI_ADDR_WIDTH = 13,
    parameter integer RAM_DEPTH = 1024,
    parameter integer DAC_CLKS_PER_HALF_BIT = 2,
    parameter integer SAFE_OFF_RELEASE_DEBOUNCE_CYCLES = 1_000_000
)(
    input  wire        stim_trigger,
    input  wire        force_safe_off_button_n,
    output wire        stimulus_level,
    output wire        trigger_monitor_level,
    output wire        configuration_locked,
    output wire        intan_marker_active,
    output wire [13:0] intan_marker_mask,
    (* X_INTERFACE_INFO = "xilinx.com:signal:interrupt:1.0 stim_fault_irq INTERRUPT" *)
    (* X_INTERFACE_PARAMETER = "SENSITIVITY LEVEL_HIGH" *)
    output wire        stim_fault_irq,
    output wire        dac_sync_n,
    output wire        dac_sclk,
    output wire        dac_sdin,

    (* X_INTERFACE_INFO = "xilinx.com:signal:clock:1.0 clkgen_ref_clk CLK" *)
    (* X_INTERFACE_PARAMETER = "FREQ_HZ 140000000, PHASE 0.0" *)
    input wire clkgen_ref_clk,

    (* X_INTERFACE_INFO = "xilinx.com:signal:clock:1.0 s00_axi_aclk CLK" *)
    (* X_INTERFACE_PARAMETER = "FREQ_HZ 100000000, PHASE 0.0, ASSOCIATED_BUSIF S00_AXI, ASSOCIATED_RESET s00_axi_aresetn" *)
    input wire s00_axi_aclk,
    (* X_INTERFACE_INFO = "xilinx.com:signal:reset:1.0 s00_axi_aresetn RST" *)
    (* X_INTERFACE_PARAMETER = "POLARITY ACTIVE_LOW" *)
    input wire s00_axi_aresetn,

    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S00_AXI AWADDR" *)
    (* X_INTERFACE_MODE = "slave S00_AXI" *)
    (* X_INTERFACE_PARAMETER = "DATA_WIDTH 32, PROTOCOL AXI4LITE, FREQ_HZ 100000000" *)
    input wire [C_S00_AXI_ADDR_WIDTH-1:0] s00_axi_awaddr,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S00_AXI AWPROT" *) input wire [2:0] s00_axi_awprot,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S00_AXI AWVALID" *) input wire s00_axi_awvalid,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S00_AXI AWREADY" *) output wire s00_axi_awready,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S00_AXI WDATA" *) input wire [C_S00_AXI_DATA_WIDTH-1:0] s00_axi_wdata,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S00_AXI WSTRB" *) input wire [(C_S00_AXI_DATA_WIDTH/8)-1:0] s00_axi_wstrb,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S00_AXI WVALID" *) input wire s00_axi_wvalid,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S00_AXI WREADY" *) output wire s00_axi_wready,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S00_AXI BRESP" *) output wire [1:0] s00_axi_bresp,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S00_AXI BVALID" *) output wire s00_axi_bvalid,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S00_AXI BREADY" *) input wire s00_axi_bready,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S00_AXI ARADDR" *) input wire [C_S00_AXI_ADDR_WIDTH-1:0] s00_axi_araddr,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S00_AXI ARPROT" *) input wire [2:0] s00_axi_arprot,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S00_AXI ARVALID" *) input wire s00_axi_arvalid,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S00_AXI ARREADY" *) output wire s00_axi_arready,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S00_AXI RDATA" *) output wire [C_S00_AXI_DATA_WIDTH-1:0] s00_axi_rdata,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S00_AXI RRESP" *) output wire [1:0] s00_axi_rresp,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S00_AXI RVALID" *) output wire s00_axi_rvalid,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S00_AXI RREADY" *) input wire s00_axi_rready
);

    stim_controller #(
        .C_S00_AXI_DATA_WIDTH(C_S00_AXI_DATA_WIDTH),
        .C_S00_AXI_ADDR_WIDTH(C_S00_AXI_ADDR_WIDTH),
        .RAM_DEPTH(RAM_DEPTH),
        .DAC_CLKS_PER_HALF_BIT(DAC_CLKS_PER_HALF_BIT),
        .SAFE_OFF_RELEASE_DEBOUNCE_CYCLES(SAFE_OFF_RELEASE_DEBOUNCE_CYCLES),
        .SIM_CLOCK_BYPASS(0)
    ) impl (
        .stim_trigger(stim_trigger),
        .force_safe_off_button_n(force_safe_off_button_n),
        .stimulus_level(stimulus_level),
        .trigger_monitor_level(trigger_monitor_level),
        .configuration_locked(configuration_locked),
        .intan_marker_active(intan_marker_active),
        .intan_marker_mask(intan_marker_mask),
        .stim_fault_irq(stim_fault_irq),
        .dac_sync_n(dac_sync_n), .dac_sclk(dac_sclk), .dac_sdin(dac_sdin),
        .clkgen_ref_clk(clkgen_ref_clk),
        .sim_dac_clk(1'b0), .sim_dac_clock_locked(1'b0),
        .sim_dac_clock_ready(1'b0), .sim_dac_clock_program_busy(1'b0),
        .s00_axi_aclk(s00_axi_aclk), .s00_axi_aresetn(s00_axi_aresetn),
        .s00_axi_awaddr(s00_axi_awaddr), .s00_axi_awprot(s00_axi_awprot),
        .s00_axi_awvalid(s00_axi_awvalid), .s00_axi_awready(s00_axi_awready),
        .s00_axi_wdata(s00_axi_wdata), .s00_axi_wstrb(s00_axi_wstrb),
        .s00_axi_wvalid(s00_axi_wvalid), .s00_axi_wready(s00_axi_wready),
        .s00_axi_bresp(s00_axi_bresp), .s00_axi_bvalid(s00_axi_bvalid),
        .s00_axi_bready(s00_axi_bready), .s00_axi_araddr(s00_axi_araddr),
        .s00_axi_arprot(s00_axi_arprot), .s00_axi_arvalid(s00_axi_arvalid),
        .s00_axi_arready(s00_axi_arready), .s00_axi_rdata(s00_axi_rdata),
        .s00_axi_rresp(s00_axi_rresp), .s00_axi_rvalid(s00_axi_rvalid),
        .s00_axi_rready(s00_axi_rready));

endmodule

`default_nettype wire
