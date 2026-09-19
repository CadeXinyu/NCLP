`timescale 1ns / 1ps
`default_nettype none

// ============================================================================
// pl_led_control
// ============================================================================
// Small AXI-Lite controlled active-high LED/light controller.
// Software writes logical LED-on bits and the same levels are driven at the
// board boundary. Reset clears the software mask; hardware error and running
// indications continue to reflect their inputs.
// ============================================================================
module pl_led_control #(
    parameter integer C_S00_AXI_DATA_WIDTH = 32,
    parameter integer C_S00_AXI_ADDR_WIDTH = 12
)(
    // Synchronized to s00_axi_aclk by the SPI top before entering this block.
    input  wire                                  spi_running,
    input  wire                                  hardware_error,

    output wire                                  led_a,
    output wire                                  led_b,
    output wire                                  led_c,
    output wire                                  led_d,
    output wire                                  led_error,
    output wire                                  running,

    input  wire                                  s00_axi_aclk,
    input  wire                                  s00_axi_aresetn,
    input  wire [C_S00_AXI_ADDR_WIDTH-1:0]       s00_axi_awaddr,
    input  wire [2:0]                            s00_axi_awprot,
    input  wire                                  s00_axi_awvalid,
    output wire                                  s00_axi_awready,
    input  wire [C_S00_AXI_DATA_WIDTH-1:0]       s00_axi_wdata,
    input  wire [(C_S00_AXI_DATA_WIDTH/8)-1:0]   s00_axi_wstrb,
    input  wire                                  s00_axi_wvalid,
    output wire                                  s00_axi_wready,
    output wire [1:0]                            s00_axi_bresp,
    output wire                                  s00_axi_bvalid,
    input  wire                                  s00_axi_bready,
    input  wire [C_S00_AXI_ADDR_WIDTH-1:0]       s00_axi_araddr,
    input  wire [2:0]                            s00_axi_arprot,
    input  wire                                  s00_axi_arvalid,
    output wire                                  s00_axi_arready,
    output wire [C_S00_AXI_DATA_WIDTH-1:0]       s00_axi_rdata,
    output wire [1:0]                            s00_axi_rresp,
    output wire                                  s00_axi_rvalid,
    input  wire                                  s00_axi_rready
);

    assign running = spi_running;

    localparam [31:0] BLOCK_ID_VALUE     = 32'h4C45_4453; // "LEDS"
    localparam [31:0] ABI_VERSION_VALUE  = 32'h0002_0000;
    localparam [31:0] CAPABILITIES_VALUE = 32'h0000_0007;
    // [15:8] physical output count, [7:0] software-mask bit count.
    localparam [31:0] INFO_VALUE         = 32'h0000_0605;

    localparam [11:0] ADDR_BLOCK_ID        = 12'h000;
    localparam [11:0] ADDR_ABI_VERSION     = 12'h004;
    localparam [11:0] ADDR_CAPABILITIES    = 12'h008;
    localparam [11:0] ADDR_INFO            = 12'h00C;
    localparam [11:0] ADDR_SOFTWARE_ON_MASK = 12'h010;
    localparam [11:0] ADDR_STATUS          = 12'h014;

    reg [4:0]  software_on_mask_reg;

    wire register_write_enable;
    wire [C_S00_AXI_ADDR_WIDTH-1:0] register_write_address;
    wire [C_S00_AXI_DATA_WIDTH-1:0] register_write_data;
    wire [(C_S00_AXI_DATA_WIDTH/8)-1:0] register_write_strobes;
    wire [C_S00_AXI_ADDR_WIDTH-1:0] register_read_address;
    reg  [C_S00_AXI_DATA_WIDTH-1:0] register_read_data;

    initial begin
        if (C_S00_AXI_DATA_WIDTH != 32 || C_S00_AXI_ADDR_WIDTH < 12) begin
            $error("pl_led_control: AXI-Lite must be 32-bit with >=12 address bits");
            $finish;
        end
    end

    axil_register_slave #(
        .DATA_WIDTH(C_S00_AXI_DATA_WIDTH),
        .ADDR_WIDTH(C_S00_AXI_ADDR_WIDTH)
    ) axil_transport_inst (
        .clk(s00_axi_aclk),
        .resetn(s00_axi_aresetn),
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
        .reg_wr_en(register_write_enable),
        .reg_wr_addr(register_write_address),
        .reg_wr_data(register_write_data),
        .reg_wr_strb(register_write_strobes),
        .reg_rd_addr(register_read_address),
        .reg_rd_data(register_read_data)
    );

    assign led_a     = software_on_mask_reg[0];
    assign led_b     = software_on_mask_reg[1];
    assign led_c     = software_on_mask_reg[2];
    assign led_d     = software_on_mask_reg[3];
    // A hardware fault cannot be hidden by the software LED mask.
    assign led_error = software_on_mask_reg[4] || hardware_error;

    always @(posedge s00_axi_aclk) begin
        if (!s00_axi_aresetn) begin
            software_on_mask_reg <= 5'd0;
        end else if (register_write_enable) begin
            case (register_write_address)
                ADDR_SOFTWARE_ON_MASK: begin
                    if (register_write_strobes[0]) begin
                        software_on_mask_reg <= register_write_data[4:0];
                    end
                end
                default: begin end
            endcase
        end
    end

    always @* begin
        case (register_read_address)
            ADDR_BLOCK_ID:        register_read_data = BLOCK_ID_VALUE;
            ADDR_ABI_VERSION:     register_read_data = ABI_VERSION_VALUE;
            ADDR_CAPABILITIES:    register_read_data = CAPABILITIES_VALUE;
            ADDR_INFO:            register_read_data = INFO_VALUE;
            ADDR_SOFTWARE_ON_MASK: register_read_data = {27'd0, software_on_mask_reg};
            ADDR_STATUS:          register_read_data = {25'd0, hardware_error,
                running, led_error, led_d, led_c, led_b, led_a};
            default:              register_read_data = 32'd0;
        endcase
    end

endmodule

`default_nettype wire
