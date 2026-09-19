`timescale 1ns / 1ps
`default_nettype none

module ttl_output_router #(
    parameter integer C_S00_AXI_DATA_WIDTH = 32,
    parameter integer C_S00_AXI_ADDR_WIDTH = 12
)(
    input wire stimulus_level,
    input wire route_write_locked,
    input wire trigger_monitor_level,
    input wire intan_sync,
    output wire [1:0] ttl_out,
    input wire s00_axi_aclk,
    input wire s00_axi_aresetn,
    input wire [C_S00_AXI_ADDR_WIDTH-1:0] s00_axi_awaddr,
    input wire [2:0] s00_axi_awprot,
    input wire  s00_axi_awvalid,
    output wire  s00_axi_awready,
    input wire [C_S00_AXI_DATA_WIDTH-1:0] s00_axi_wdata,
    input wire [(C_S00_AXI_DATA_WIDTH/8)-1:0] s00_axi_wstrb,
    input wire  s00_axi_wvalid,
    output wire  s00_axi_wready,
    output wire [1:0] s00_axi_bresp,
    output wire  s00_axi_bvalid,
    input wire  s00_axi_bready,
    input wire [C_S00_AXI_ADDR_WIDTH-1:0] s00_axi_araddr,
    input wire [2:0] s00_axi_arprot,
    input wire  s00_axi_arvalid,
    output wire  s00_axi_arready,
    output wire [C_S00_AXI_DATA_WIDTH-1:0] s00_axi_rdata,
    output wire [1:0] s00_axi_rresp,
    output wire  s00_axi_rvalid,
    input wire  s00_axi_rready
);

    localparam [31:0] BLOCK_ID_VALUE = 32'h5454_4C52;
    localparam [31:0] ABI_VERSION_VALUE = 32'h0002_0000;
    localparam [31:0] CAPABILITIES_VALUE = 32'h0000_000B;
    localparam [31:0] INFO_VALUE = 32'h0004_0202;
    localparam [C_S00_AXI_ADDR_WIDTH-1:0] ADDR_BLOCK_ID = 12'h000;
    localparam [C_S00_AXI_ADDR_WIDTH-1:0] ADDR_ABI_VERSION = 12'h004;
    localparam [C_S00_AXI_ADDR_WIDTH-1:0] ADDR_CAPABILITIES = 12'h008;
    localparam [C_S00_AXI_ADDR_WIDTH-1:0] ADDR_INFO = 12'h00C;
    localparam [C_S00_AXI_ADDR_WIDTH-1:0] ADDR_ROUTE = 12'h010;
    localparam [C_S00_AXI_ADDR_WIDTH-1:0] ADDR_STATUS = 12'h014;
    localparam [C_S00_AXI_ADDR_WIDTH-1:0] ADDR_ERROR_STATUS = 12'h018;

    reg [3:0] route_selectors;
    reg [1:0] error_status;
    wire reg_wr_en;
    wire [C_S00_AXI_ADDR_WIDTH-1:0] reg_wr_addr, reg_rd_addr;
    wire [31:0] reg_wr_data;
    wire [3:0] reg_wr_strb;
    reg [31:0] reg_rd_data;
    wire route_conflict = (route_selectors[1:0] != 2'd0) &&
                          (route_selectors[1:0] == route_selectors[3:2]);
    wire route_write_request = reg_wr_en && (reg_wr_addr == ADDR_ROUTE) && reg_wr_strb[0];
    wire requested_route_conflict = (reg_wr_data[1:0] != 2'd0) &&
                               (reg_wr_data[1:0] == reg_wr_data[3:2]);
    wire [1:0] error_events = {
        route_write_request && !route_write_locked && requested_route_conflict,
        route_write_request && route_write_locked
    };
    wire [1:0] error_clear_mask = reg_wr_en && (reg_wr_addr == ADDR_ERROR_STATUS) &&
                             reg_wr_strb[0] ? reg_wr_data[1:0] : 2'b00;
    wire [1:0] sampled_ttl_out;

    // Synchronize the raw Intan level before rebuilding AXI telemetry. Sampling
    // the physical mux output here would place route-selection logic before the
    // first synchronizer stage and create an unsafe combinational CDC path.
    // The physical raw-sync path remains combinational for pin-level latency.
    (* ASYNC_REG = "TRUE" *) reg intan_sync_meta, intan_sync_status;
    always @(posedge s00_axi_aclk) begin
        if (!s00_axi_aresetn) begin
            intan_sync_meta <= 1'b0;
            intan_sync_status <= 1'b0;
        end else begin
            intan_sync_meta <= intan_sync;
            intan_sync_status <= intan_sync_meta;
        end
    end

    always @(posedge s00_axi_aclk) begin
        if (!s00_axi_aresetn) begin
            route_selectors <= 4'd0;
            error_status <= 2'd0;
        end else begin
            if (route_write_request && !route_write_locked)
                route_selectors <= reg_wr_data[3:0];
            error_status <= (error_status & ~error_clear_mask) | error_events;
        end
    end

    // Sources already carry their owner's output policy. The write lock only
    // protects the selection register and never changes a selected level.
    function automatic select_source_level(
        input [1:0] selector, input stimulus,
        input sync_level, input monitor
    );
        case (selector)
            2'd1: select_source_level = stimulus;
            2'd2: select_source_level = sync_level;
            2'd3: select_source_level = monitor;
            default: select_source_level = 1'b0;
        endcase
    endfunction
    assign ttl_out[0] = route_conflict ? 1'b0 : select_source_level(
        route_selectors[1:0], stimulus_level, intan_sync, trigger_monitor_level);
    assign ttl_out[1] = route_conflict ? 1'b0 : select_source_level(
        route_selectors[3:2], stimulus_level, intan_sync, trigger_monitor_level);
    assign sampled_ttl_out[0] = route_conflict ? 1'b0 : select_source_level(
        route_selectors[1:0], stimulus_level, intan_sync_status, trigger_monitor_level);
    assign sampled_ttl_out[1] = route_conflict ? 1'b0 : select_source_level(
        route_selectors[3:2], stimulus_level, intan_sync_status, trigger_monitor_level);

    always @* begin
        case (reg_rd_addr)
            ADDR_BLOCK_ID: reg_rd_data = BLOCK_ID_VALUE;
            ADDR_ABI_VERSION: reg_rd_data = ABI_VERSION_VALUE;
            ADDR_CAPABILITIES: reg_rd_data = CAPABILITIES_VALUE;
            ADDR_INFO: reg_rd_data = INFO_VALUE;
            ADDR_ROUTE: reg_rd_data = {28'd0, route_selectors};
            ADDR_STATUS: reg_rd_data = {23'd0, trigger_monitor_level, stimulus_level,
                intan_sync_status, sampled_ttl_out, 1'b0,
                route_conflict, 1'b0, route_write_locked};
            ADDR_ERROR_STATUS: reg_rd_data = {30'd0, error_status};
            default: reg_rd_data = 32'd0;
        endcase
    end

    axil_register_slave #(
        .DATA_WIDTH(C_S00_AXI_DATA_WIDTH), .ADDR_WIDTH(C_S00_AXI_ADDR_WIDTH)
    ) registers (
        .clk(s00_axi_aclk), .resetn(s00_axi_aresetn),
        .s_axil_awaddr(s00_axi_awaddr),
        .s_axil_awprot(s00_axi_awprot),
        .s_axil_awvalid(s00_axi_awvalid),
        .s_axil_awready(s00_axi_awready),
        .s_axil_wdata(s00_axi_wdata),
        .s_axil_wstrb(s00_axi_wstrb),
        .s_axil_wvalid(s00_axi_wvalid),
        .s_axil_wready(s00_axi_wready),
        .s_axil_bresp(s00_axi_bresp),
        .s_axil_bvalid(s00_axi_bvalid),
        .s_axil_bready(s00_axi_bready),
        .s_axil_araddr(s00_axi_araddr),
        .s_axil_arprot(s00_axi_arprot),
        .s_axil_arvalid(s00_axi_arvalid),
        .s_axil_arready(s00_axi_arready),
        .s_axil_rdata(s00_axi_rdata),
        .s_axil_rresp(s00_axi_rresp),
        .s_axil_rvalid(s00_axi_rvalid),
        .s_axil_rready(s00_axi_rready),
        .reg_wr_en(reg_wr_en), .reg_wr_addr(reg_wr_addr),
        .reg_wr_data(reg_wr_data), .reg_wr_strb(reg_wr_strb),
        .reg_rd_addr(reg_rd_addr), .reg_rd_data(reg_rd_data)
    );

    initial begin
        if (C_S00_AXI_DATA_WIDTH != 32 || C_S00_AXI_ADDR_WIDTH != 12)
            $error("ttl_output_router requires a 32-bit AXI data bus and 4KB aperture");
    end
endmodule

`default_nettype wire
