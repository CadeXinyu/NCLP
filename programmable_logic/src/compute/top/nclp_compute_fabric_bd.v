`timescale 1ns/1ps
`default_nettype none

// Block-design boundary for compute selection and SFP application routing.
// Aurora/CDC and the PS packet mailbox are separate blocks. This fabric owns
// raw-frame conversion, packet switching, the remote trigger endpoint, and
// their compact AXI-Lite control map.
module nclp_compute_fabric_bd #(
    parameter integer PS_RX_FIFO_DEPTH_BEATS = 32
) (
    (* X_INTERFACE_INFO = "xilinx.com:signal:clock:1.0 s00_axi_aclk CLK" *)
    (* X_INTERFACE_PARAMETER = "FREQ_HZ 100000000, ASSOCIATED_BUSIF S00_AXI:S_AXIS_INTAN_RAW_FRAME:M_AXIS_ALGO_SAMPLE:S_AXIS_PS_TX:M_AXIS_PS_RX:S_AXIS_SFP_RX:M_AXIS_SFP_TX, ASSOCIATED_RESET s00_axi_aresetn" *)
    input  wire        s00_axi_aclk,
    (* X_INTERFACE_INFO = "xilinx.com:signal:reset:1.0 s00_axi_aresetn RST" *)
    (* X_INTERFACE_PARAMETER = "POLARITY ACTIVE_LOW" *)
    input  wire        s00_axi_aresetn,

    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S00_AXI AWADDR" *)
    (* X_INTERFACE_MODE = "slave S00_AXI" *)
    (* X_INTERFACE_PARAMETER = "DATA_WIDTH 32, PROTOCOL AXI4LITE, FREQ_HZ 100000000" *)
    input  wire [11:0] s00_axi_awaddr,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S00_AXI AWPROT" *)
    input  wire [2:0]  s00_axi_awprot,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S00_AXI AWVALID" *)
    input  wire        s00_axi_awvalid,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S00_AXI AWREADY" *)
    output wire        s00_axi_awready,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S00_AXI WDATA" *)
    input  wire [31:0] s00_axi_wdata,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S00_AXI WSTRB" *)
    input  wire [3:0]  s00_axi_wstrb,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S00_AXI WVALID" *)
    input  wire        s00_axi_wvalid,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S00_AXI WREADY" *)
    output wire        s00_axi_wready,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S00_AXI BRESP" *)
    output wire [1:0]  s00_axi_bresp,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S00_AXI BVALID" *)
    output wire        s00_axi_bvalid,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S00_AXI BREADY" *)
    input  wire        s00_axi_bready,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S00_AXI ARADDR" *)
    input  wire [11:0] s00_axi_araddr,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S00_AXI ARPROT" *)
    input  wire [2:0]  s00_axi_arprot,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S00_AXI ARVALID" *)
    input  wire        s00_axi_arvalid,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S00_AXI ARREADY" *)
    output wire        s00_axi_arready,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S00_AXI RDATA" *)
    output wire [31:0] s00_axi_rdata,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S00_AXI RRESP" *)
    output wire [1:0]  s00_axi_rresp,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S00_AXI RVALID" *)
    output wire        s00_axi_rvalid,
    (* X_INTERFACE_INFO = "xilinx.com:interface:aximm:1.0 S00_AXI RREADY" *)
    input  wire        s00_axi_rready,

    input  wire        compute_stream_active,
    input  wire        link_up,
    input  wire        link_fault,
    output wire        phy_enable,
    output wire        sfp_mode_selected,
    output wire        local_compute_stream_active,
    output wire        sfp_stim_trigger,
    output wire        ps_tx_packet_interrupted_pulse,
    (* X_INTERFACE_INFO = "xilinx.com:signal:interrupt:1.0 compute_irq INTERRUPT" *)
    (* X_INTERFACE_PARAMETER = "SENSITIVITY LEVEL_HIGH" *)
    output wire        compute_irq,

    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 S_AXIS_INTAN_RAW_FRAME TDATA" *)
    (* X_INTERFACE_MODE = "slave S_AXIS_INTAN_RAW_FRAME" *)
    (* X_INTERFACE_PARAMETER = "FREQ_HZ 100000000, TDATA_NUM_BYTES 2, HAS_TKEEP 1, HAS_TLAST 1, HAS_TREADY 1" *)
    input  wire [15:0] s_axis_intan_raw_frame_tdata,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 S_AXIS_INTAN_RAW_FRAME TKEEP" *)
    input  wire [1:0]  s_axis_intan_raw_frame_tkeep,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 S_AXIS_INTAN_RAW_FRAME TVALID" *)
    input  wire        s_axis_intan_raw_frame_tvalid,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 S_AXIS_INTAN_RAW_FRAME TREADY" *)
    output wire        s_axis_intan_raw_frame_tready,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 S_AXIS_INTAN_RAW_FRAME TLAST" *)
    input  wire        s_axis_intan_raw_frame_tlast,

    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 M_AXIS_ALGO_SAMPLE TDATA" *)
    (* X_INTERFACE_MODE = "master M_AXIS_ALGO_SAMPLE" *)
    (* X_INTERFACE_PARAMETER = "FREQ_HZ 100000000, TDATA_NUM_BYTES 8, HAS_TKEEP 1, HAS_TLAST 1, HAS_TREADY 1" *)
    output wire [63:0] m_axis_algo_sample_tdata,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 M_AXIS_ALGO_SAMPLE TKEEP" *)
    output wire [7:0]  m_axis_algo_sample_tkeep,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 M_AXIS_ALGO_SAMPLE TVALID" *)
    output wire        m_axis_algo_sample_tvalid,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 M_AXIS_ALGO_SAMPLE TREADY" *)
    input  wire        m_axis_algo_sample_tready,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 M_AXIS_ALGO_SAMPLE TLAST" *)
    output wire        m_axis_algo_sample_tlast,

    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 S_AXIS_PS_TX TDATA" *)
    (* X_INTERFACE_MODE = "slave S_AXIS_PS_TX" *)
    (* X_INTERFACE_PARAMETER = "FREQ_HZ 100000000, TDATA_NUM_BYTES 8, HAS_TKEEP 1, HAS_TLAST 1, HAS_TREADY 1" *)
    input  wire [63:0] s_axis_ps_tx_tdata,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 S_AXIS_PS_TX TKEEP" *)
    input  wire [7:0]  s_axis_ps_tx_tkeep,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 S_AXIS_PS_TX TVALID" *)
    input  wire        s_axis_ps_tx_tvalid,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 S_AXIS_PS_TX TREADY" *)
    output wire        s_axis_ps_tx_tready,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 S_AXIS_PS_TX TLAST" *)
    input  wire        s_axis_ps_tx_tlast,

    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 M_AXIS_PS_RX TDATA" *)
    (* X_INTERFACE_MODE = "master M_AXIS_PS_RX" *)
    (* X_INTERFACE_PARAMETER = "FREQ_HZ 100000000, TDATA_NUM_BYTES 8, HAS_TKEEP 1, HAS_TLAST 1, HAS_TREADY 1" *)
    output wire [63:0] m_axis_ps_rx_tdata,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 M_AXIS_PS_RX TKEEP" *)
    output wire [7:0]  m_axis_ps_rx_tkeep,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 M_AXIS_PS_RX TVALID" *)
    output wire        m_axis_ps_rx_tvalid,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 M_AXIS_PS_RX TREADY" *)
    input  wire        m_axis_ps_rx_tready,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 M_AXIS_PS_RX TLAST" *)
    output wire        m_axis_ps_rx_tlast,

    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 S_AXIS_SFP_RX TDATA" *)
    (* X_INTERFACE_MODE = "slave S_AXIS_SFP_RX" *)
    (* X_INTERFACE_PARAMETER = "FREQ_HZ 100000000, TDATA_NUM_BYTES 8, HAS_TKEEP 1, HAS_TLAST 1, HAS_TREADY 1" *)
    input  wire [63:0] s_axis_sfp_rx_tdata,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 S_AXIS_SFP_RX TKEEP" *)
    input  wire [7:0]  s_axis_sfp_rx_tkeep,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 S_AXIS_SFP_RX TVALID" *)
    input  wire        s_axis_sfp_rx_tvalid,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 S_AXIS_SFP_RX TREADY" *)
    output wire        s_axis_sfp_rx_tready,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 S_AXIS_SFP_RX TLAST" *)
    input  wire        s_axis_sfp_rx_tlast,

    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 M_AXIS_SFP_TX TDATA" *)
    (* X_INTERFACE_MODE = "master M_AXIS_SFP_TX" *)
    (* X_INTERFACE_PARAMETER = "FREQ_HZ 100000000, TDATA_NUM_BYTES 8, HAS_TKEEP 1, HAS_TLAST 1, HAS_TREADY 1, TUSER_WIDTH 1" *)
    output wire [63:0] m_axis_sfp_tx_tdata,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 M_AXIS_SFP_TX TKEEP" *)
    output wire [7:0]  m_axis_sfp_tx_tkeep,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 M_AXIS_SFP_TX TVALID" *)
    output wire        m_axis_sfp_tx_tvalid,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 M_AXIS_SFP_TX TREADY" *)
    input  wire        m_axis_sfp_tx_tready,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 M_AXIS_SFP_TX TLAST" *)
    output wire        m_axis_sfp_tx_tlast,
    (* X_INTERFACE_INFO = "xilinx.com:interface:axis:1.0 M_AXIS_SFP_TX TUSER" *)
    output wire        m_axis_sfp_tx_ps_owned
);
    wire [15:0] source_stream_mask;
    wire clear_diagnostics_pulse;

    wire [15:0] local_raw_frame_tdata, sfp_raw_frame_tdata;
    wire [1:0] local_raw_frame_tkeep, sfp_raw_frame_tkeep;
    wire local_raw_frame_tvalid, local_raw_frame_tready, local_raw_frame_tlast;
    wire sfp_raw_frame_tvalid, sfp_raw_frame_tready, sfp_raw_frame_tlast;

    wire [63:0] intan_tx_payload_tdata;
    wire [7:0] intan_tx_payload_tkeep;
    wire intan_tx_payload_tvalid, intan_tx_payload_tready, intan_tx_payload_tlast;
    wire sfp_packetizer_idle, sfp_packetizer_in_frame;
    wire sfp_packetizer_fault_pulse;

    wire local_decoder_idle, local_decoder_in_frame;
    wire local_decode_fault_pulse;
    wire tx_idle, tx_intan_packet_active, tx_ps_packet_active;
    wire intan_tx_packet_enqueued_pulse, ps_tx_attempt_enqueued_pulse;
    wire sfp_tx_interrupted_packet_pulse;
    wire rx_parser_idle, ps_rx_pending, rx_fault_pulse;
    wire rx_stim_trigger_pulse;
    wire [31:0] rx_malformed_packet_count;
    wire [31:0] rx_fifo_overflow_count;
    wire [31:0] rx_dropped_packet_count;
    wire [31:0] ps_rx_packet_count;
    wire [31:0] rx_stim_trigger_count;
    wire [31:0] rx_stim_trigger_reject_count;

    wire intan_session_eos_accepted = s_axis_intan_raw_frame_tvalid &&
        s_axis_intan_raw_frame_tready && s_axis_intan_raw_frame_tlast &&
        s_axis_intan_raw_frame_tkeep == 2'b00;
    wire ps_tx_pending_or_active = tx_ps_packet_active || s_axis_ps_tx_tvalid;

    assign local_compute_stream_active = s00_axi_aresetn &&
        compute_stream_active && !sfp_mode_selected;
    assign sfp_stim_trigger = rx_stim_trigger_pulse;

    intan_compute_path_selector intan_path_selector (
        .resetn(s00_axi_aresetn), .sfp_mode_selected(sfp_mode_selected),
        .s_axis_tdata(s_axis_intan_raw_frame_tdata),
        .s_axis_tkeep(s_axis_intan_raw_frame_tkeep),
        .s_axis_tvalid(s_axis_intan_raw_frame_tvalid),
        .s_axis_tready(s_axis_intan_raw_frame_tready),
        .s_axis_tlast(s_axis_intan_raw_frame_tlast),
        .m_axis_local_tdata(local_raw_frame_tdata),
        .m_axis_local_tkeep(local_raw_frame_tkeep),
        .m_axis_local_tvalid(local_raw_frame_tvalid),
        .m_axis_local_tready(local_raw_frame_tready),
        .m_axis_local_tlast(local_raw_frame_tlast),
        .m_axis_sfp_tdata(sfp_raw_frame_tdata),
        .m_axis_sfp_tkeep(sfp_raw_frame_tkeep),
        .m_axis_sfp_tvalid(sfp_raw_frame_tvalid),
        .m_axis_sfp_tready(sfp_raw_frame_tready),
        .m_axis_sfp_tlast(sfp_raw_frame_tlast));

    intan_frame_depacketizer local_event_decoder (
        .clk(s00_axi_aclk), .resetn(s00_axi_aresetn),
        .source_stream_mask(source_stream_mask),
        .s_axis_raw_frame_tdata(local_raw_frame_tdata),
        .s_axis_raw_frame_tkeep(local_raw_frame_tkeep),
        .s_axis_raw_frame_tvalid(local_raw_frame_tvalid),
        .s_axis_raw_frame_tready(local_raw_frame_tready),
        .s_axis_raw_frame_tlast(local_raw_frame_tlast),
        .m_axis_sample_tdata(m_axis_algo_sample_tdata),
        .m_axis_sample_tkeep(m_axis_algo_sample_tkeep),
        .m_axis_sample_tvalid(m_axis_algo_sample_tvalid),
        .m_axis_sample_tready(m_axis_algo_sample_tready),
        .m_axis_sample_tlast(m_axis_algo_sample_tlast),
        .decoder_idle(local_decoder_idle),
        .decoder_in_frame(local_decoder_in_frame),
        .decode_fault_pulse(local_decode_fault_pulse));

    intan_sfp_packetizer sfp_frame_packetizer (
        .clk(s00_axi_aclk), .resetn(s00_axi_aresetn),
        .s_axis_tdata(sfp_raw_frame_tdata),
        .s_axis_tkeep(sfp_raw_frame_tkeep),
        .s_axis_tvalid(sfp_raw_frame_tvalid),
        .s_axis_tready(sfp_raw_frame_tready),
        .s_axis_tlast(sfp_raw_frame_tlast),
        .m_axis_tdata(intan_tx_payload_tdata),
        .m_axis_tkeep(intan_tx_payload_tkeep),
        .m_axis_tvalid(intan_tx_payload_tvalid),
        .m_axis_tready(intan_tx_payload_tready),
        .m_axis_tlast(intan_tx_payload_tlast),
        .idle(sfp_packetizer_idle), .in_frame(sfp_packetizer_in_frame),
        .fault_pulse(sfp_packetizer_fault_pulse));

    nclp_sfp_tx_arbiter tx_packet_switch (
        .clk(s00_axi_aclk), .resetn(s00_axi_aresetn),
        .transport_up(link_up),
        .s_axis_intan_tdata(intan_tx_payload_tdata),
        .s_axis_intan_tkeep(intan_tx_payload_tkeep),
        .s_axis_intan_tvalid(intan_tx_payload_tvalid),
        .s_axis_intan_tready(intan_tx_payload_tready),
        .s_axis_intan_tlast(intan_tx_payload_tlast),
        .s_axis_ps_tdata(s_axis_ps_tx_tdata),
        .s_axis_ps_tkeep(s_axis_ps_tx_tkeep),
        .s_axis_ps_tvalid(s_axis_ps_tx_tvalid),
        .s_axis_ps_tready(s_axis_ps_tx_tready),
        .s_axis_ps_tlast(s_axis_ps_tx_tlast),
        .m_axis_tdata(m_axis_sfp_tx_tdata),
        .m_axis_tkeep(m_axis_sfp_tx_tkeep),
        .m_axis_tvalid(m_axis_sfp_tx_tvalid),
        .m_axis_tready(m_axis_sfp_tx_tready),
        .m_axis_tlast(m_axis_sfp_tx_tlast),
        .m_axis_ps_owned(m_axis_sfp_tx_ps_owned),
        .idle(tx_idle), .intan_packet_active(tx_intan_packet_active),
        .ps_packet_active(tx_ps_packet_active),
        .intan_packet_enqueued_pulse(intan_tx_packet_enqueued_pulse),
        .ps_packet_attempt_enqueued_pulse(ps_tx_attempt_enqueued_pulse),
        .interrupted_packet_pulse(sfp_tx_interrupted_packet_pulse),
        .ps_packet_interrupted_pulse(ps_tx_packet_interrupted_pulse));

    nclp_sfp_rx_router #(.PS_PAYLOAD_FIFO_DEPTH_BEATS(PS_RX_FIFO_DEPTH_BEATS)) rx_packet_switch (
        .clk(s00_axi_aclk), .resetn(s00_axi_aresetn),
        .transport_up(link_up), .clear_diagnostics_pulse(clear_diagnostics_pulse),
        .ps_command_route_enabled(sfp_mode_selected),
        .stim_trigger_route_enabled(sfp_mode_selected),
        .s_axis_tdata(s_axis_sfp_rx_tdata),
        .s_axis_tkeep(s_axis_sfp_rx_tkeep),
        .s_axis_tvalid(s_axis_sfp_rx_tvalid),
        .s_axis_tready(s_axis_sfp_rx_tready),
        .s_axis_tlast(s_axis_sfp_rx_tlast),
        .m_axis_ps_tdata(m_axis_ps_rx_tdata),
        .m_axis_ps_tkeep(m_axis_ps_rx_tkeep),
        .m_axis_ps_tvalid(m_axis_ps_rx_tvalid),
        .m_axis_ps_tready(m_axis_ps_rx_tready),
        .m_axis_ps_tlast(m_axis_ps_rx_tlast),
        .stim_trigger_pulse(rx_stim_trigger_pulse),
        .ps_queue_nonempty(ps_rx_pending),
        .parser_idle(rx_parser_idle), .fault_pulse(rx_fault_pulse),
        .malformed_packet_count(rx_malformed_packet_count),
        .ps_fifo_overflow_count(rx_fifo_overflow_count),
        .dropped_packet_count(rx_dropped_packet_count),
        .ps_packet_count(ps_rx_packet_count),
        .stim_trigger_count(rx_stim_trigger_count),
        .stim_trigger_reject_count(rx_stim_trigger_reject_count));

    compute_fabric_control control (
        .s00_axi_aclk(s00_axi_aclk),
        .s00_axi_aresetn(s00_axi_aresetn),
        .s00_axi_awaddr(s00_axi_awaddr), .s00_axi_awprot(s00_axi_awprot),
        .s00_axi_awvalid(s00_axi_awvalid), .s00_axi_awready(s00_axi_awready),
        .s00_axi_wdata(s00_axi_wdata), .s00_axi_wstrb(s00_axi_wstrb),
        .s00_axi_wvalid(s00_axi_wvalid), .s00_axi_wready(s00_axi_wready),
        .s00_axi_bresp(s00_axi_bresp), .s00_axi_bvalid(s00_axi_bvalid),
        .s00_axi_bready(s00_axi_bready),
        .s00_axi_araddr(s00_axi_araddr), .s00_axi_arprot(s00_axi_arprot),
        .s00_axi_arvalid(s00_axi_arvalid), .s00_axi_arready(s00_axi_arready),
        .s00_axi_rdata(s00_axi_rdata), .s00_axi_rresp(s00_axi_rresp),
        .s00_axi_rvalid(s00_axi_rvalid), .s00_axi_rready(s00_axi_rready),
        .compute_stream_active(compute_stream_active),
        .intan_input_valid(s_axis_intan_raw_frame_tvalid),
        .local_decoder_idle(local_decoder_idle),
        .local_decoder_in_frame(local_decoder_in_frame),
        .sfp_packetizer_idle(sfp_packetizer_idle),
        .sfp_packetizer_in_frame(sfp_packetizer_in_frame),
        .tx_idle(tx_idle), .tx_intan_packet_active(tx_intan_packet_active),
        .tx_ps_pending_or_active(ps_tx_pending_or_active),
        .rx_parser_idle(rx_parser_idle), .ps_rx_pending(ps_rx_pending),
        .link_up(link_up), .link_fault(link_fault),
        .intan_session_eos_accepted(intan_session_eos_accepted),
        .local_decode_fault_pulse(local_decode_fault_pulse),
        .sfp_packetizer_fault_pulse(sfp_packetizer_fault_pulse),
        .sfp_tx_interrupted_packet_pulse(sfp_tx_interrupted_packet_pulse),
        .intan_tx_packet_enqueued_pulse(intan_tx_packet_enqueued_pulse),
        .ps_tx_attempt_enqueued_pulse(ps_tx_attempt_enqueued_pulse),
        .rx_fault_pulse(rx_fault_pulse),
        .rx_malformed_packet_count(rx_malformed_packet_count),
        .rx_fifo_overflow_count(rx_fifo_overflow_count),
        .rx_dropped_packet_count(rx_dropped_packet_count),
        .ps_rx_packet_count(ps_rx_packet_count),
        .rx_stim_trigger_count(rx_stim_trigger_count),
        .rx_stim_trigger_reject_count(rx_stim_trigger_reject_count),
        .phy_enable(phy_enable), .sfp_mode_selected(sfp_mode_selected),
        .source_stream_mask(source_stream_mask),
        .clear_diagnostics_pulse(clear_diagnostics_pulse), .compute_irq(compute_irq));
endmodule

`default_nettype wire
