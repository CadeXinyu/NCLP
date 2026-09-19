`timescale 1ns/1ps
`default_nettype none

// PS-visible control for the compute fabric.  It owns only mode/configuration
// state and fabric diagnostics. Aurora transport and the PS packet mailbox keep
// their own interfaces and status.
module compute_fabric_control (
    input  wire        s00_axi_aclk,
    input  wire        s00_axi_aresetn,
    input  wire [11:0] s00_axi_awaddr,
    input  wire [2:0]  s00_axi_awprot,
    input  wire        s00_axi_awvalid,
    output wire        s00_axi_awready,
    input  wire [31:0] s00_axi_wdata,
    input  wire [3:0]  s00_axi_wstrb,
    input  wire        s00_axi_wvalid,
    output wire        s00_axi_wready,
    output wire [1:0]  s00_axi_bresp,
    output wire        s00_axi_bvalid,
    input  wire        s00_axi_bready,
    input  wire [11:0] s00_axi_araddr,
    input  wire [2:0]  s00_axi_arprot,
    input  wire        s00_axi_arvalid,
    output wire        s00_axi_arready,
    output wire [31:0] s00_axi_rdata,
    output wire [1:0]  s00_axi_rresp,
    output wire        s00_axi_rvalid,
    input  wire        s00_axi_rready,

    input  wire        compute_stream_active,
    input  wire        intan_input_valid,
    input  wire        local_decoder_idle,
    input  wire        local_decoder_in_frame,
    input  wire        sfp_packetizer_idle,
    input  wire        sfp_packetizer_in_frame,
    input  wire        tx_idle,
    input  wire        tx_intan_packet_active,
    input  wire        tx_ps_pending_or_active,
    input  wire        rx_parser_idle,
    input  wire        ps_rx_pending,
    input  wire        link_up,
    input  wire        link_fault,

    input  wire        intan_session_eos_accepted,
    input  wire        local_decode_fault_pulse,
    input  wire        sfp_packetizer_fault_pulse,
    input  wire        sfp_tx_interrupted_packet_pulse,
    input  wire        intan_tx_packet_enqueued_pulse,
    input  wire        ps_tx_attempt_enqueued_pulse,
    input  wire        rx_fault_pulse,
    input  wire [31:0] rx_malformed_packet_count,
    input  wire [31:0] rx_fifo_overflow_count,
    input  wire [31:0] rx_dropped_packet_count,
    input  wire [31:0] ps_rx_packet_count,
    input  wire [31:0] rx_stim_trigger_count,
    input  wire [31:0] rx_stim_trigger_reject_count,

    output reg         phy_enable,
    output reg         sfp_mode_selected,
    output reg  [15:0] source_stream_mask,
    output reg         clear_diagnostics_pulse,
    output wire        compute_irq
);
    localparam [31:0] BLOCK_ID_VALUE = 32'h4e43_4650; // "NCFP"
    localparam [31:0] ABI_VERSION_VALUE = 32'h0004_0000;
    localparam [31:0] CAPABILITIES_VALUE = 32'h0000_003f;

    localparam [11:0] ADDR_BLOCK_ID = 12'h000;
    localparam [11:0] ADDR_ABI_VERSION = 12'h004;
    localparam [11:0] ADDR_CAPABILITIES = 12'h008;
    localparam [11:0] ADDR_CONTROL = 12'h010;
    localparam [11:0] ADDR_SOURCE_STREAM_MASK = 12'h014;
    localparam [11:0] ADDR_COMMAND = 12'h018;
    localparam [11:0] ADDR_STATUS = 12'h020;
    localparam [11:0] ADDR_LINK_STATUS = 12'h024;
    localparam [11:0] ADDR_CONTROL_REJECT_COUNT = 12'h030;
    localparam [11:0] ADDR_LOCAL_DECODE_FAULT_COUNT = 12'h034;
    localparam [11:0] ADDR_SFP_PACKETIZER_FAULT_COUNT = 12'h038;
    localparam [11:0] ADDR_SFP_TX_INTERRUPTED_PACKET_COUNT = 12'h03c;
    localparam [11:0] ADDR_RX_MALFORMED_PACKET_COUNT = 12'h040;
    localparam [11:0] ADDR_RX_FIFO_OVERFLOW_COUNT = 12'h044;
    localparam [11:0] ADDR_RX_DROPPED_PACKET_COUNT = 12'h048;
    localparam [11:0] ADDR_PS_RX_PACKET_COUNT = 12'h04c;
    localparam [11:0] ADDR_RX_STIM_TRIGGER_COUNT = 12'h050;
    localparam [11:0] ADDR_RX_STIM_TRIGGER_REJECT_COUNT = 12'h054;
    localparam [11:0] ADDR_INTAN_TX_PACKET_ENQUEUED_COUNT = 12'h058;
    localparam [11:0] ADDR_PS_TX_ATTEMPT_ENQUEUED_COUNT = 12'h05c;

    wire clk = s00_axi_aclk;
    wire resetn = s00_axi_aresetn;
    wire reg_wr_en;
    wire [11:0] reg_wr_addr;
    wire [31:0] reg_wr_data;
    wire [3:0] reg_wr_strb;
    wire [11:0] reg_rd_addr;
    reg [31:0] reg_rd_data;
    reg fault_irq_enable;
    reg local_decode_fault_sticky;
    reg sfp_packetizer_fault_sticky;
    reg sfp_tx_interrupted_packet_sticky;
    reg rx_fault_sticky;
    reg control_reject_sticky;
    reg [31:0] control_reject_count;
    reg [31:0] local_decode_fault_count;
    reg [31:0] sfp_packetizer_fault_count;
    reg [31:0] sfp_tx_interrupted_packet_count;
    reg [31:0] intan_tx_packet_enqueued_count;
    reg [31:0] ps_tx_attempt_enqueued_count;
    reg compute_stream_active_prev;
    reg intan_session_eos_seen;

    axil_register_slave #(.ADDR_WIDTH(12)) register_slave (
        .clk(clk), .resetn(resetn),
        .s_axil_awaddr(s00_axi_awaddr), .s_axil_awprot(s00_axi_awprot),
        .s_axil_awvalid(s00_axi_awvalid), .s_axil_awready(s00_axi_awready),
        .s_axil_wdata(s00_axi_wdata), .s_axil_wstrb(s00_axi_wstrb),
        .s_axil_wvalid(s00_axi_wvalid), .s_axil_wready(s00_axi_wready),
        .s_axil_bresp(s00_axi_bresp), .s_axil_bvalid(s00_axi_bvalid),
        .s_axil_bready(s00_axi_bready),
        .s_axil_araddr(s00_axi_araddr), .s_axil_arprot(s00_axi_arprot),
        .s_axil_arvalid(s00_axi_arvalid), .s_axil_arready(s00_axi_arready),
        .s_axil_rdata(s00_axi_rdata), .s_axil_rresp(s00_axi_rresp),
        .s_axil_rvalid(s00_axi_rvalid), .s_axil_rready(s00_axi_rready),
        .reg_wr_en(reg_wr_en), .reg_wr_addr(reg_wr_addr),
        .reg_wr_data(reg_wr_data), .reg_wr_strb(reg_wr_strb),
        .reg_rd_addr(reg_rd_addr), .reg_rd_data(reg_rd_data));

    function automatic [31:0] merge_write_bytes(
        input [31:0] old_value,
        input [31:0] new_value,
        input [3:0] strobes
    );
        integer byte_index;
        begin
            merge_write_bytes = old_value;
            for (byte_index = 0; byte_index < 4; byte_index = byte_index + 1)
                if (strobes[byte_index])
                    merge_write_bytes[byte_index*8 +: 8] =
                        new_value[byte_index*8 +: 8];
        end
    endfunction

    wire [31:0] strobed_write_data = reg_wr_data & {
        {8{reg_wr_strb[3]}}, {8{reg_wr_strb[2]}},
        {8{reg_wr_strb[1]}}, {8{reg_wr_strb[0]}}
    };
    wire compute_config_idle = !compute_stream_active && !intan_input_valid &&
        local_decoder_idle && sfp_packetizer_idle && !tx_intan_packet_active;
    wire control_write = reg_wr_en && reg_wr_addr == ADDR_CONTROL;
    wire [31:0] control_value =
        {23'd0, fault_irq_enable, 6'd0, sfp_mode_selected, phy_enable};
    wire [31:0] control_next = merge_write_bytes(
        control_value, reg_wr_data, reg_wr_strb);
    wire control_reserved_written =
        |(strobed_write_data & ~32'h00000103);
    wire mode_change_blocked = control_next[1] != sfp_mode_selected &&
                               !compute_config_idle;
    wire control_rejected = control_write &&
        (control_reserved_written || mode_change_blocked);

    wire mask_write = reg_wr_en && reg_wr_addr == ADDR_SOURCE_STREAM_MASK;
    wire [31:0] mask_next = merge_write_bytes(
        {16'd0, source_stream_mask}, reg_wr_data, reg_wr_strb);
    wire mask_rejected = mask_write &&
        (|(strobed_write_data & 32'hffff0000) ||
         mask_next[15:0] == 16'd0 ||
         (mask_next[15:0] != source_stream_mask &&
          !compute_config_idle));

    wire command_write = reg_wr_en && reg_wr_addr == ADDR_COMMAND;
    wire command_rejected = command_write &&
        (|(strobed_write_data & ~32'h00000003) ||
         (strobed_write_data[1] && !compute_config_idle));
    wire clear_diagnostics_command = command_write && !command_rejected &&
                                     strobed_write_data[0];
    wire clear_intan_eos_command = command_write && !command_rejected &&
                                   strobed_write_data[1];
    wire control_reject_event = control_rejected || mask_rejected ||
                                command_rejected;

    assign compute_irq = resetn && fault_irq_enable &&
        (link_fault || local_decode_fault_sticky ||
         sfp_packetizer_fault_sticky || sfp_tx_interrupted_packet_sticky ||
         rx_fault_sticky || control_reject_sticky);

    always @(posedge clk) begin
        if (!resetn) begin
            phy_enable <= 1'b1;
            sfp_mode_selected <= 1'b0;
            source_stream_mask <= 16'h0001;
            fault_irq_enable <= 1'b1;
            clear_diagnostics_pulse <= 1'b0;
            local_decode_fault_sticky <= 1'b0;
            sfp_packetizer_fault_sticky <= 1'b0;
            sfp_tx_interrupted_packet_sticky <= 1'b0;
            rx_fault_sticky <= 1'b0;
            control_reject_sticky <= 1'b0;
            control_reject_count <= 32'd0;
            local_decode_fault_count <= 32'd0;
            sfp_packetizer_fault_count <= 32'd0;
            sfp_tx_interrupted_packet_count <= 32'd0;
            intan_tx_packet_enqueued_count <= 32'd0;
            ps_tx_attempt_enqueued_count <= 32'd0;
            compute_stream_active_prev <= 1'b0;
            intan_session_eos_seen <= 1'b0;
        end else begin
            compute_stream_active_prev <= compute_stream_active;
            clear_diagnostics_pulse <= clear_diagnostics_command;

            if (clear_intan_eos_command ||
                (compute_stream_active && !compute_stream_active_prev))
                intan_session_eos_seen <= 1'b0;
            if (intan_session_eos_accepted)
                intan_session_eos_seen <= 1'b1;

            if (control_write && !control_rejected) begin
                phy_enable <= control_next[0];
                sfp_mode_selected <= control_next[1];
                fault_irq_enable <= control_next[8];
            end
            if (mask_write && !mask_rejected)
                source_stream_mask <= mask_next[15:0];

            if (clear_diagnostics_command) begin
                local_decode_fault_sticky <= 1'b0;
                sfp_packetizer_fault_sticky <= 1'b0;
                sfp_tx_interrupted_packet_sticky <= 1'b0;
                rx_fault_sticky <= 1'b0;
                control_reject_sticky <= 1'b0;
                control_reject_count <= 32'd0;
                local_decode_fault_count <= 32'd0;
                sfp_packetizer_fault_count <= 32'd0;
                sfp_tx_interrupted_packet_count <= 32'd0;
                intan_tx_packet_enqueued_count <= 32'd0;
                ps_tx_attempt_enqueued_count <= 32'd0;
            end

            if (control_reject_event) begin
                control_reject_sticky <= 1'b1;
                control_reject_count <=
                    (clear_diagnostics_command ? 32'd0 : control_reject_count) + 1'b1;
            end
            if (local_decode_fault_pulse) begin
                local_decode_fault_sticky <= 1'b1;
                local_decode_fault_count <=
                    (clear_diagnostics_command ? 32'd0 : local_decode_fault_count) + 1'b1;
            end
            if (sfp_packetizer_fault_pulse) begin
                sfp_packetizer_fault_sticky <= 1'b1;
                sfp_packetizer_fault_count <=
                    (clear_diagnostics_command ? 32'd0 : sfp_packetizer_fault_count) + 1'b1;
            end
            if (sfp_tx_interrupted_packet_pulse) begin
                sfp_tx_interrupted_packet_sticky <= 1'b1;
                sfp_tx_interrupted_packet_count <=
                    (clear_diagnostics_command ? 32'd0 : sfp_tx_interrupted_packet_count) + 1'b1;
            end
            if (rx_fault_pulse) begin
                rx_fault_sticky <= 1'b1;
            end
            if (intan_tx_packet_enqueued_pulse)
                intan_tx_packet_enqueued_count <=
                    (clear_diagnostics_command ? 32'd0 : intan_tx_packet_enqueued_count) + 1'b1;
            if (ps_tx_attempt_enqueued_pulse)
                ps_tx_attempt_enqueued_count <=
                    (clear_diagnostics_command ? 32'd0 : ps_tx_attempt_enqueued_count) + 1'b1;
        end
    end

    always @* begin
        reg_rd_data = 32'd0;
        case (reg_rd_addr)
            ADDR_BLOCK_ID: reg_rd_data = BLOCK_ID_VALUE;
            ADDR_ABI_VERSION: reg_rd_data = ABI_VERSION_VALUE;
            ADDR_CAPABILITIES: reg_rd_data = CAPABILITIES_VALUE;
            ADDR_CONTROL: reg_rd_data = control_value;
            ADDR_SOURCE_STREAM_MASK:
                reg_rd_data = {16'd0, source_stream_mask};
            ADDR_STATUS: reg_rd_data = {
                15'd0,
                intan_session_eos_seen,
                control_reject_sticky,
                rx_fault_sticky,
                sfp_tx_interrupted_packet_sticky,
                sfp_packetizer_fault_sticky,
                local_decode_fault_sticky,
                ps_rx_pending,
                tx_ps_pending_or_active,
                tx_intan_packet_active,
                tx_idle,
                sfp_packetizer_in_frame,
                local_decoder_in_frame,
                compute_config_idle,
                compute_stream_active,
                sfp_mode_selected,
                phy_enable,
                link_up
            };
            ADDR_LINK_STATUS:
                reg_rd_data = {29'd0, rx_parser_idle, link_fault, link_up};
            ADDR_CONTROL_REJECT_COUNT: reg_rd_data = control_reject_count;
            ADDR_LOCAL_DECODE_FAULT_COUNT:
                reg_rd_data = local_decode_fault_count;
            ADDR_SFP_PACKETIZER_FAULT_COUNT:
                reg_rd_data = sfp_packetizer_fault_count;
            ADDR_SFP_TX_INTERRUPTED_PACKET_COUNT:
                reg_rd_data = sfp_tx_interrupted_packet_count;
            ADDR_RX_MALFORMED_PACKET_COUNT:
                reg_rd_data = rx_malformed_packet_count;
            ADDR_RX_FIFO_OVERFLOW_COUNT:
                reg_rd_data = rx_fifo_overflow_count;
            ADDR_RX_DROPPED_PACKET_COUNT:
                reg_rd_data = rx_dropped_packet_count;
            ADDR_PS_RX_PACKET_COUNT: reg_rd_data = ps_rx_packet_count;
            ADDR_RX_STIM_TRIGGER_COUNT: reg_rd_data = rx_stim_trigger_count;
            ADDR_RX_STIM_TRIGGER_REJECT_COUNT:
                reg_rd_data = rx_stim_trigger_reject_count;
            ADDR_INTAN_TX_PACKET_ENQUEUED_COUNT:
                reg_rd_data = intan_tx_packet_enqueued_count;
            ADDR_PS_TX_ATTEMPT_ENQUEUED_COUNT:
                reg_rd_data = ps_tx_attempt_enqueued_count;
            default: reg_rd_data = 32'd0;
        endcase
    end
endmodule

`default_nettype wire
