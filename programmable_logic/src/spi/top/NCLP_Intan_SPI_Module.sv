`timescale 1ns / 1ps
`default_nettype none

// ============================================================================
// NCLP_Intan_SPI_Module
// ============================================================================
// Clean SPI subsystem top.
//
// This module owns the flat Intan AXI-Lite v3 register/RAM aperture, the SPI
// engine, and two independently enabled copies of the compact acquisition
// stream: one for recording and one for compute routing.
// ============================================================================

module NCLP_Intan_SPI_Module #(
    // FIFO capacities are payload bytes.  Each queue must hold one maximum
    // compact frame plus its ordered EOS: 568 16-bit events / 1136 bytes.
    parameter integer RECORDING_EVENT_FIFO_DEPTH_BYTES = 2048,
    parameter integer COMPUTE_EVENT_FIFO_DEPTH_BYTES = 2048,
    // Configuration-clock settling cycles after reset release, before clock ready.
    parameter integer CLOCK_RESET_RELEASE_CYCLES = 1024,
    parameter integer C_S00_AXI_DATA_WIDTH = 32,
    parameter integer C_S00_AXI_ADDR_WIDTH = 16
)(
    input  wire       s00_axi_aclk,
    input  wire       clkgen_ref_clk,
    input  wire       reset,

    input  wire       miso_a1,
    input  wire       miso_a2,
    input  wire       miso_b1,
    input  wire       miso_b2,
    input  wire       miso_c1,
    input  wire       miso_c2,
    input  wire       miso_d1,
    input  wire       miso_d2,

    output wire       spi_running,
    output wire       compute_stream_active,
    output wire       acquisition_30ksps,
    output wire       intan_error_irq,
    output wire       sclk,
    output wire       cs,
    output wire       mosi_a,
    output wire       mosi_b,
    output wire       mosi_c,
    output wire       mosi_d,
    output wire       intan_sync_out,

    output wire [15:0] m_axis_recording_tdata,
    output wire [1:0]  m_axis_recording_tkeep,
    output wire        m_axis_recording_tvalid,
    input  wire        m_axis_recording_tready,
    output wire        m_axis_recording_tlast,

    output wire [15:0] m_axis_compute_tdata,
    output wire [1:0]  m_axis_compute_tkeep,
    output wire        m_axis_compute_tvalid,
    input  wire        m_axis_compute_tready,
    output wire        m_axis_compute_tlast,

    input  wire [1:0]  ttl_in_external,
    input  wire        intan_marker_active,
    input  wire [13:0] intan_marker_mask,

    input  wire [C_S00_AXI_ADDR_WIDTH-1 : 0]     s00_axi_awaddr,
    input  wire [2 : 0]                          s00_axi_awprot,
    input  wire                                  s00_axi_awvalid,
    output wire                                  s00_axi_awready,
    input  wire [C_S00_AXI_DATA_WIDTH-1 : 0]     s00_axi_wdata,
    input  wire [(C_S00_AXI_DATA_WIDTH/8)-1 : 0] s00_axi_wstrb,
    input  wire                                  s00_axi_wvalid,
    output wire                                  s00_axi_wready,
    output wire [1 : 0]                          s00_axi_bresp,
    output wire                                  s00_axi_bvalid,
    input  wire                                  s00_axi_bready,
    input  wire [C_S00_AXI_ADDR_WIDTH-1 : 0]     s00_axi_araddr,
    input  wire [2 : 0]                          s00_axi_arprot,
    input  wire                                  s00_axi_arvalid,
    output wire                                  s00_axi_arready,
    output wire [C_S00_AXI_DATA_WIDTH-1 : 0]     s00_axi_rdata,
    output wire [1 : 0]                          s00_axi_rresp,
    output wire                                  s00_axi_rvalid,
    input  wire                                  s00_axi_rready
);

    wire spi_clk_w;
    wire spi_running_raw_w;
    wire spi_running_bus_w;
    wire fifo_rst_w;
    wire fifo_rst_spi_w;
    wire system_reset_spi_w;

    wire acquisition_start_request_w;
    wire acquisition_start_acknowledge_w;
    wire acquisition_start_pending_w;
    wire core_configuration_locked_w;
    wire run_continuous_w;
    wire dsp_settle_w;
    wire init_dummy_mode_w;
    wire [31:0] finite_frame_count_w;
    wire [15:0] logical_stream_enable_w;
    wire recording_output_enable_w;
    wire compute_output_enable_w;
    wire [31:0] miso_phase_primary_w;
    wire [31:0] miso_phase_secondary_w;
    wire [7:0] aux1_bank_select_w, aux2_bank_select_w, aux3_bank_select_w;
    wire [9:0] aux1_end_index_w, aux2_end_index_w, aux3_end_index_w;
    wire [9:0] aux1_loop_index_w, aux2_loop_index_w, aux3_loop_index_w;
    wire fast_settle_enable_w;
    wire [3:0] fast_settle_ttl_input_index_w;
    wire [1:0] sync_mode_w;
    wire [15:0] sync_period_frames_w;
    wire [15:0] sync_high_frames_w;

    wire clock_domain_reset_request_w;
    wire clock_program_request_w;
    wire clock_config_valid_w;
    wire [7:0] clock_output_divide_w;
    wire [3:0] clock_input_divide_w;
    wire [6:0] clock_feedback_multiply_w;
    wire clock_ready_w;
    wire clock_program_busy_w;
    wire clock_program_done_w;
    wire clock_error_w;
    wire [2:0] clock_error_code_w;
    wire clock_locked_w;
    wire [7:0] clock_active_o_w;
    wire [3:0] clock_active_d_w;
    wire [6:0] clock_active_m_w;

    // The register bank's clock settings are writable shadows. Gate the local
    // detector with the profile that completed DRP programming and startup.
    assign acquisition_30ksps = !reset && clock_ready_w && clock_locked_w &&
        !clock_program_busy_w && !clock_error_w &&
        clock_active_o_w == 8'd15 && clock_active_d_w == 4'd4 &&
        clock_active_m_w == 7'd36;

    wire diagnostic_clear_request_w;
    wire diagnostic_clear_busy_w;
    wire output_paths_drained_w;
    wire intan_error_irq_w;
    wire source_event_loss_seen_w;
    wire recording_session_end_seen_w;
    wire recording_output_active_w;
    wire recording_stall_seen_w;
    wire [31:0] recording_payload_word_count_lo_w;
    wire [31:0] recording_payload_word_count_hi_w;
    wire [31:0] recording_end_of_session_marker_count_w;
    wire [31:0] unaccepted_source_event_count_w;
    wire [31:0] recording_stall_cycle_count_w;

    wire [1:0] aux1_bus_write_strobe_w, aux2_bus_write_strobe_w,
               aux3_bus_write_strobe_w;
    wire [11:0] aux1_bus_address_w, aux2_bus_address_w, aux3_bus_address_w;
    wire [15:0] aux1_bus_write_data_w, aux2_bus_write_data_w,
                aux3_bus_write_data_w;
    wire [15:0] aux1_bus_read_data_w, aux2_bus_read_data_w,
                aux3_bus_read_data_w;

    intan_axil_register_bank #(
        .C_S00_AXI_DATA_WIDTH(C_S00_AXI_DATA_WIDTH),
        .C_S00_AXI_ADDR_WIDTH(C_S00_AXI_ADDR_WIDTH)
    ) intan_register_bank (
        .acquisition_start_request(acquisition_start_request_w),
        .acquisition_start_acknowledge(acquisition_start_acknowledge_w),
        .acquisition_start_pending(acquisition_start_pending_w),
        .acquisition_running(spi_running_bus_w),
        .core_configuration_locked(core_configuration_locked_w),
        .output_paths_drained(output_paths_drained_w),
        .run_continuous(run_continuous_w),
        .dsp_settle(dsp_settle_w),
        .init_dummy_mode(init_dummy_mode_w),
        .finite_frame_count(finite_frame_count_w),
        .logical_stream_enable(logical_stream_enable_w),
        .recording_output_enable(recording_output_enable_w),
        .compute_output_enable(compute_output_enable_w),
        .miso_phase_primary(miso_phase_primary_w),
        .miso_phase_secondary(miso_phase_secondary_w),
        .aux1_bank_select(aux1_bank_select_w),
        .aux2_bank_select(aux2_bank_select_w),
        .aux3_bank_select(aux3_bank_select_w),
        .aux1_end_index(aux1_end_index_w),
        .aux2_end_index(aux2_end_index_w),
        .aux3_end_index(aux3_end_index_w),
        .aux1_loop_index(aux1_loop_index_w),
        .aux2_loop_index(aux2_loop_index_w),
        .aux3_loop_index(aux3_loop_index_w),
        .fast_settle_enable(fast_settle_enable_w),
        .fast_settle_ttl_input_index(fast_settle_ttl_input_index_w),
        .sync_mode(sync_mode_w),
        .sync_period_frames(sync_period_frames_w),
        .sync_high_frames(sync_high_frames_w),
        .sample_clock_apply_request(clock_program_request_w),
        .sample_clock_config_valid(clock_config_valid_w),
        .sample_clock_output_divide(clock_output_divide_w),
        .sample_clock_input_divide(clock_input_divide_w),
        .sample_clock_feedback_multiply(clock_feedback_multiply_w),
        .sample_clock_locked(clock_locked_w),
        .sample_clock_ready(clock_ready_w),
        .sample_clock_busy(clock_program_busy_w),
        .sample_clock_done(clock_program_done_w),
        .sample_clock_error(clock_error_w),
        .sample_clock_error_code(clock_error_code_w),
        .diagnostic_clear_request(diagnostic_clear_request_w),
        .diagnostic_clear_busy(diagnostic_clear_busy_w),
        .intan_error_irq(intan_error_irq_w),
        .source_event_loss_seen(source_event_loss_seen_w),
        .recording_session_end_seen(recording_session_end_seen_w),
        .recording_output_active(recording_output_active_w),
        .recording_stall_seen(recording_stall_seen_w),
        .recording_payload_word_count_lo(recording_payload_word_count_lo_w),
        .recording_payload_word_count_hi(recording_payload_word_count_hi_w),
        .recording_end_of_session_marker_count(recording_end_of_session_marker_count_w),
        .unaccepted_source_event_count(unaccepted_source_event_count_w),
        .recording_stall_cycle_count(recording_stall_cycle_count_w),
        .aux1_bus_write_strobe(aux1_bus_write_strobe_w),
        .aux1_bus_address(aux1_bus_address_w),
        .aux1_bus_write_data(aux1_bus_write_data_w),
        .aux1_bus_read_data(aux1_bus_read_data_w),
        .aux2_bus_write_strobe(aux2_bus_write_strobe_w),
        .aux2_bus_address(aux2_bus_address_w),
        .aux2_bus_write_data(aux2_bus_write_data_w),
        .aux2_bus_read_data(aux2_bus_read_data_w),
        .aux3_bus_write_strobe(aux3_bus_write_strobe_w),
        .aux3_bus_address(aux3_bus_address_w),
        .aux3_bus_write_data(aux3_bus_write_data_w),
        .aux3_bus_read_data(aux3_bus_read_data_w),
        .s00_axi_aclk(s00_axi_aclk),
        .s00_axi_aresetn(!reset),
        .s00_axi_awaddr(s00_axi_awaddr),
        .s00_axi_awprot(s00_axi_awprot),
        .s00_axi_awvalid(s00_axi_awvalid),
        .s00_axi_awready(s00_axi_awready),
        .s00_axi_wdata(s00_axi_wdata),
        .s00_axi_wstrb(s00_axi_wstrb),
        .s00_axi_wvalid(s00_axi_wvalid),
        .s00_axi_wready(s00_axi_wready),
        .s00_axi_bresp(s00_axi_bresp),
        .s00_axi_bvalid(s00_axi_bvalid),
        .s00_axi_bready(s00_axi_bready),
        .s00_axi_araddr(s00_axi_araddr),
        .s00_axi_arprot(s00_axi_arprot),
        .s00_axi_arvalid(s00_axi_arvalid),
        .s00_axi_arready(s00_axi_arready),
        .s00_axi_rdata(s00_axi_rdata),
        .s00_axi_rresp(s00_axi_rresp),
        .s00_axi_rvalid(s00_axi_rvalid),
        .s00_axi_rready(s00_axi_rready)
    );

    wire [15:0] raw_frame_word_spi_w;
    wire        raw_frame_word_valid_spi_w;
    wire        raw_frame_word_last_spi_w;
    wire        acquisition_session_end_spi_w;
    wire        session_compute_output_enable_spi_w;
    wire        session_recording_output_enable_spi_w;

    // The public subsystem wrapper owns the programmable clock primitive and
    // the only reset tree for every spi_clk_w consumer.
    mmcme4_clock #(
        .DEFAULT_O(8'd15),
        .DEFAULT_D(4'd4),
        .DEFAULT_M(7'd36),
        .RESET_RELEASE_CYCLES(CLOCK_RESET_RELEASE_CYCLES)
    ) spi_clkgen (
        .config_clk(s00_axi_aclk),
        .source_clk(clkgen_ref_clk),
        .reset(reset),
        .request(clock_program_request_w),
        .config_valid(clock_config_valid_w),
        .config_o(clock_output_divide_w),
        .config_d(clock_input_divide_w),
        .config_m(clock_feedback_multiply_w),
        .ready(clock_ready_w),
        .busy(clock_program_busy_w),
        .done_pulse(clock_program_done_w),
        .error(clock_error_w),
        .error_code(clock_error_code_w),
        .locked(clock_locked_w),
        .active_o(clock_active_o_w),
        .active_d(clock_active_d_w),
        .active_m(clock_active_m_w),
        .domain_reset_request(clock_domain_reset_request_w),
        .domain_reset_ack(fifo_rst_spi_w),
        .clock_out(spi_clk_w)
    );

    // The sequencer asserts this registered request asynchronously when reset
    // rises.  Drive the XPM reset tree directly so no LUT can enter an
    // asynchronous reset path.
    assign fifo_rst_w = clock_domain_reset_request_w;

    spi_intan_interface_4_bank spi_engine (
        .s00_axi_aclk(s00_axi_aclk),
        .spi_clk(spi_clk_w),
        .reset(reset),
        .system_reset_spi(system_reset_spi_w),
        .spi_domain_reset(fifo_rst_spi_w),
        .spi_domain_reset_request_bus(clock_domain_reset_request_w),
        .acquisition_start_request(acquisition_start_request_w),
        .acquisition_start_acknowledge(acquisition_start_acknowledge_w),
        .configuration_locked(core_configuration_locked_w),
        .run_continuous(run_continuous_w),
        .dsp_settle(dsp_settle_w),
        .init_dummy_mode(init_dummy_mode_w),
        .finite_frame_count(finite_frame_count_w),
        .miso_phase_primary(miso_phase_primary_w),
        .miso_phase_secondary(miso_phase_secondary_w),
        .logical_stream_enable(logical_stream_enable_w),
        .compute_output_enable(compute_output_enable_w),
        .recording_output_enable(recording_output_enable_w),
        .aux1_bank_select(aux1_bank_select_w),
        .aux2_bank_select(aux2_bank_select_w),
        .aux3_bank_select(aux3_bank_select_w),
        .aux1_end_index(aux1_end_index_w),
        .aux2_end_index(aux2_end_index_w),
        .aux3_end_index(aux3_end_index_w),
        .aux1_loop_index(aux1_loop_index_w),
        .aux2_loop_index(aux2_loop_index_w),
        .aux3_loop_index(aux3_loop_index_w),
        .fast_settle_enable(fast_settle_enable_w),
        .fast_settle_ttl_input_index(fast_settle_ttl_input_index_w),
        .sync_mode(sync_mode_w),
        .sync_period_frames(sync_period_frames_w),
        .sync_high_frames(sync_high_frames_w),
        .aux1_bus_write_strobe(aux1_bus_write_strobe_w),
        .aux1_bus_address(aux1_bus_address_w),
        .aux1_bus_write_data(aux1_bus_write_data_w),
        .aux1_bus_read_data(aux1_bus_read_data_w),
        .aux2_bus_write_strobe(aux2_bus_write_strobe_w),
        .aux2_bus_address(aux2_bus_address_w),
        .aux2_bus_write_data(aux2_bus_write_data_w),
        .aux2_bus_read_data(aux2_bus_read_data_w),
        .aux3_bus_write_strobe(aux3_bus_write_strobe_w),
        .aux3_bus_address(aux3_bus_address_w),
        .aux3_bus_write_data(aux3_bus_write_data_w),
        .aux3_bus_read_data(aux3_bus_read_data_w),
        .MISO_A1(miso_a1),
        .MISO_A2(miso_a2),
        .MISO_B1(miso_b1),
        .MISO_B2(miso_b2),
        .MISO_C1(miso_c1),
        .MISO_C2(miso_c2),
        .MISO_D1(miso_d1),
        .MISO_D2(miso_d2),
        .SPI_running(spi_running_raw_w),
        .SCLK(sclk),
        .CS(cs),
        .MOSI_A(mosi_a),
        .MOSI_B(mosi_b),
        .MOSI_C(mosi_c),
        .MOSI_D(mosi_d),
        .intan_sync_out(intan_sync_out),
        .raw_frame_word(raw_frame_word_spi_w),
        .raw_frame_word_valid(raw_frame_word_valid_spi_w),
        .raw_frame_word_last(raw_frame_word_last_spi_w),
        .acquisition_session_end(acquisition_session_end_spi_w),
        .session_compute_output_enable(session_compute_output_enable_spi_w),
        .session_recording_output_enable(session_recording_output_enable_spi_w),
        .ttl_in_external(ttl_in_external),
        .intan_marker_active(intan_marker_active),
        .intan_marker_mask(intan_marker_mask)
    );

    // The clock manager raises fifo_rst_w before disabling or resetting the
    // MMCM. Assertion is asynchronous so it cannot be missed when spi_clk_w
    // stops; release is synchronized to the restarted clock.
    xpm_cdc_async_rst #(
        .DEST_SYNC_FF(2),
        .INIT_SYNC_FF(0),
        .RST_ACTIVE_HIGH(1)
    ) fifo_rst_spi_sync_inst (
        .src_arst(fifo_rst_w),
        .dest_clk(spi_clk_w),
        .dest_arst(fifo_rst_spi_w)
    );

    // Keep the system-reset-only state distinct from the reset used for each
    // runtime clock reconfiguration.  This one recognized async-assert,
    // sync-release tree replaces raw AXI-domain reset pins on spi_clk_w
    // registers while preserving toggle/counter state across clock APPLY.
    xpm_cdc_async_rst #(
        .DEST_SYNC_FF(2),
        .INIT_SYNC_FF(0),
        .RST_ACTIVE_HIGH(1)
    ) system_reset_spi_sync_inst (
        .src_arst(reset),
        .dest_clk(spi_clk_w),
        .dest_arst(system_reset_spi_w)
    );

    reg compute_stream_active_spi = 1'b0;
    always @(posedge spi_clk_w or posedge fifo_rst_spi_w) begin
        if (fifo_rst_spi_w)
            compute_stream_active_spi <= 1'b0;
        else
            compute_stream_active_spi <= spi_running_raw_w &&
                                         session_compute_output_enable_spi_w;
    end
    xpm_cdc_single #(
        .DEST_SYNC_FF(2), .INIT_SYNC_FF(0), .SIM_ASSERT_CHK(0), .SRC_INPUT_REG(0)
    ) compute_stream_active_cdc (
        .src_clk(spi_clk_w), .src_in(compute_stream_active_spi),
        .dest_clk(s00_axi_aclk), .dest_out(compute_stream_active)
    );

    wire session_recording_output_enable_bus_w;
    xpm_cdc_single #(
        .DEST_SYNC_FF(2),
        .INIT_SYNC_FF(0),
        .SIM_ASSERT_CHK(0),
        .SRC_INPUT_REG(1)
    ) recording_output_active_cdc (
        .src_clk(spi_clk_w),
        .src_in(session_recording_output_enable_spi_w),
        .dest_clk(s00_axi_aclk),
        .dest_out(session_recording_output_enable_bus_w)
    );
    assign recording_output_active_w = spi_running_bus_w &&
                                       session_recording_output_enable_bus_w;

    // The register bank emits a request only while the subsystem is idle.  A
    // request/acknowledge toggle handshake serializes clears so a second AXI
    // command cannot cancel the first before it reaches spi_clk_w.
    reg diagnostic_clear_request_toggle_r = 1'b0;
    always @(posedge s00_axi_aclk) begin
        if (reset) begin
            diagnostic_clear_request_toggle_r <= 1'b0;
        end else if (diagnostic_clear_request_w) begin
            diagnostic_clear_request_toggle_r <=
                !diagnostic_clear_request_toggle_r;
        end
    end

    wire diagnostic_clear_request_toggle_spi_w;
    xpm_cdc_single #(
        .DEST_SYNC_FF(2),
        .INIT_SYNC_FF(0),
        .SIM_ASSERT_CHK(0),
        .SRC_INPUT_REG(1)
    ) diagnostic_clear_request_cdc (
        .src_clk(s00_axi_aclk),
        .src_in(diagnostic_clear_request_toggle_r),
        .dest_clk(spi_clk_w),
        .dest_out(diagnostic_clear_request_toggle_spi_w)
    );

    reg diagnostic_clear_ack_toggle_spi_r = 1'b0;
    always @(posedge spi_clk_w or posedge system_reset_spi_w) begin
        if (system_reset_spi_w)
            diagnostic_clear_ack_toggle_spi_r <= 1'b0;
        else if (diagnostic_clear_request_toggle_spi_w !=
                 diagnostic_clear_ack_toggle_spi_r)
            diagnostic_clear_ack_toggle_spi_r <=
                diagnostic_clear_request_toggle_spi_w;
    end

    wire diagnostic_clear_spi_w =
        diagnostic_clear_request_toggle_spi_w !=
        diagnostic_clear_ack_toggle_spi_r;

    wire diagnostic_clear_ack_toggle_bus_w;
    xpm_cdc_single #(
        .DEST_SYNC_FF(2),
        .INIT_SYNC_FF(0),
        .SIM_ASSERT_CHK(0),
        .SRC_INPUT_REG(1)
    ) diagnostic_clear_ack_cdc (
        .src_clk(spi_clk_w),
        .src_in(diagnostic_clear_ack_toggle_spi_r),
        .dest_clk(s00_axi_aclk),
        .dest_out(diagnostic_clear_ack_toggle_bus_w)
    );

    assign diagnostic_clear_busy_w =
        diagnostic_clear_request_toggle_r !=
        diagnostic_clear_ack_toggle_bus_w;

    function [31:0] bin2gray32(input [31:0] b);
        bin2gray32 = b ^ (b >> 1);
    endfunction

    function [31:0] gray2bin32(input [31:0] g);
        integer i;
        begin
            for (i = 0; i < 32; i = i + 1) begin
                gray2bin32[i] = ^(g >> i);
            end
        end
    endfunction

    // The formatter emits one compact 16-bit stream. Each destination has its
    // own event sequencer and asynchronous FIFO, so enable and backpressure on
    // one branch never gate the other branch. Recording reserves TLAST for EOS;
    // compute also propagates the formatter's TTL-aligned frame boundary.
    wire recording_eos_pending_spi_w;
    wire recording_payload_event_valid_spi_w =
        raw_frame_word_valid_spi_w &&
        session_recording_output_enable_spi_w;
    wire recording_session_end_spi_w =
        session_recording_output_enable_spi_w && acquisition_session_end_spi_w;
    wire [15:0] recording_event_tdata_w;
    wire [1:0] recording_event_tkeep_w;
    wire recording_event_valid_w;
    wire recording_event_tlast_w;
    wire recording_event_ready_w;
    wire recording_source_drop_event_spi_w;

    spi_event_sequencer #(.DATA_WIDTH(16)) recording_event_sequencer (
        .clk(spi_clk_w),
        .reset(fifo_rst_spi_w),
        .sample_data(raw_frame_word_spi_w),
        .sample_valid(recording_payload_event_valid_spi_w),
        .sample_last(1'b0),
        .session_end(recording_session_end_spi_w),
        .event_data(recording_event_tdata_w),
        .event_keep(recording_event_tkeep_w),
        .event_valid(recording_event_valid_w),
        .event_last(recording_event_tlast_w),
        .event_ready(recording_event_ready_w),
        .sample_dropped(recording_source_drop_event_spi_w),
        .eos_pending(recording_eos_pending_spi_w)
    );

    wire recording_fifo_overflow_w;
    wire [$clog2(RECORDING_EVENT_FIFO_DEPTH_BYTES):0]
        recording_fifo_occupancy_bytes_w;
    spi_sample_event_async_fifo #(
        .DEPTH_BYTES(RECORDING_EVENT_FIFO_DEPTH_BYTES)
    ) recording_event_fifo_inst (
        .s_clk(spi_clk_w),
        .s_rst(fifo_rst_spi_w),
        .s_axis_tdata(recording_event_tdata_w),
        .s_axis_tkeep(recording_event_tkeep_w),
        .s_axis_tvalid(recording_event_valid_w),
        .s_axis_tready(recording_event_ready_w),
        .s_axis_tlast(recording_event_tlast_w),
        .m_clk(s00_axi_aclk),
        .m_rst(reset),
        .m_axis_tdata(m_axis_recording_tdata),
        .m_axis_tkeep(m_axis_recording_tkeep),
        .m_axis_tvalid(m_axis_recording_tvalid),
        .m_axis_tready(m_axis_recording_tready),
        .m_axis_tlast(m_axis_recording_tlast),
        .s_overflow(recording_fifo_overflow_w),
        .m_occupancy_bytes(recording_fifo_occupancy_bytes_w)
    );

    wire compute_eos_pending_spi_w;
    wire compute_payload_event_valid_spi_w =
        raw_frame_word_valid_spi_w && session_compute_output_enable_spi_w;
    wire compute_session_end_spi_w =
        session_compute_output_enable_spi_w && acquisition_session_end_spi_w;
    wire [15:0] compute_event_tdata_w;
    wire [1:0] compute_event_tkeep_w;
    wire compute_event_valid_w;
    wire compute_event_tlast_w;
    wire compute_event_ready_w;
    wire compute_source_drop_event_spi_w;

    spi_event_sequencer #(.DATA_WIDTH(16)) compute_event_sequencer (
        .clk(spi_clk_w),
        .reset(fifo_rst_spi_w),
        .sample_data(raw_frame_word_spi_w),
        .sample_valid(compute_payload_event_valid_spi_w),
        .sample_last(raw_frame_word_last_spi_w),
        .session_end(compute_session_end_spi_w),
        .event_data(compute_event_tdata_w),
        .event_keep(compute_event_tkeep_w),
        .event_valid(compute_event_valid_w),
        .event_last(compute_event_tlast_w),
        .event_ready(compute_event_ready_w),
        .sample_dropped(compute_source_drop_event_spi_w),
        .eos_pending(compute_eos_pending_spi_w)
    );

    wire compute_fifo_overflow_w;
    wire [$clog2(COMPUTE_EVENT_FIFO_DEPTH_BYTES):0]
        compute_fifo_occupancy_bytes_w;
    spi_sample_event_async_fifo #(
        .DEPTH_BYTES(COMPUTE_EVENT_FIFO_DEPTH_BYTES)
    ) compute_event_fifo_inst (
        .s_clk(spi_clk_w),
        .s_rst(fifo_rst_spi_w),
        .s_axis_tdata(compute_event_tdata_w),
        .s_axis_tkeep(compute_event_tkeep_w),
        .s_axis_tvalid(compute_event_valid_w),
        .s_axis_tready(compute_event_ready_w),
        .s_axis_tlast(compute_event_tlast_w),
        .m_clk(s00_axi_aclk),
        .m_rst(reset),
        .m_axis_tdata(m_axis_compute_tdata),
        .m_axis_tkeep(m_axis_compute_tkeep),
        .m_axis_tvalid(m_axis_compute_tvalid),
        .m_axis_tready(m_axis_compute_tready),
        .m_axis_tlast(m_axis_compute_tlast),
        .s_overflow(compute_fifo_overflow_w),
        .m_occupancy_bytes(compute_fifo_occupancy_bytes_w)
    );

    // Clock reconfiguration resets the SPI-side FIFO ports.  Do not allow it
    // until every prior-session event is visible as drained in the AXI clock
    // domain.  The quiet-time guard covers synchronizer latency for a source
    // write or pending EOS emitted on the last spi_clk_w edge of a session.
    wire [1:0] marker_pending_bus_w;
    xpm_cdc_array_single #(
        .DEST_SYNC_FF(2),
        .INIT_SYNC_FF(0),
        .SIM_ASSERT_CHK(0),
        .SRC_INPUT_REG(0),
        .WIDTH(2)
    ) marker_pending_cdc (
        .src_clk(spi_clk_w),
        .src_in({recording_eos_pending_spi_w, compute_eos_pending_spi_w}),
        .dest_clk(s00_axi_aclk),
        .dest_out(marker_pending_bus_w)
    );

    wire output_paths_empty_now_w =
        (recording_fifo_occupancy_bytes_w == 0) &&
        !m_axis_recording_tvalid &&
        (compute_fifo_occupancy_bytes_w == 0) && !m_axis_compute_tvalid &&
        (marker_pending_bus_w == 2'b00);
    reg [3:0] output_empty_stable_cycle_count_r = 4'd0;
    always @(posedge s00_axi_aclk) begin
        if (reset || acquisition_start_pending_w ||
            core_configuration_locked_w || spi_running_bus_w ||
            !output_paths_empty_now_w) begin
            output_empty_stable_cycle_count_r <= 4'd0;
        end else if (output_empty_stable_cycle_count_r < 4'd8) begin
            output_empty_stable_cycle_count_r <= output_empty_stable_cycle_count_r + 4'd1;
        end
    end
    assign output_paths_drained_w =
        (output_empty_stable_cycle_count_r == 4'd8);

    // Keep the two diagnostics semantically distinct:
    //   recording_stall_cycle_count counts blocked recording events,
    //   unaccepted_source_event_count counts source events not accepted by at
    //   least one enabled destination.  If both destinations reject the same
    //   source sample in one cycle, it is one dropped source sample.
    wire recording_stall_cycle_spi_w =
        recording_event_valid_w && !recording_event_ready_w;
    wire source_event_drop_spi_w = recording_source_drop_event_spi_w ||
                                   compute_source_drop_event_spi_w;
    wire source_loss_incident_spi_w = recording_fifo_overflow_w ||
                                      compute_fifo_overflow_w ||
                                      source_event_drop_spi_w;

    reg [31:0] recording_stall_cycle_count_spi_r = 32'd0;
    reg [31:0] unaccepted_source_event_count_spi_r = 32'd0;
    reg [31:0] source_loss_incident_count_spi_r = 32'd0;
    reg [31:0] recording_stall_cycle_count_gray_spi_r = 32'd0;
    reg [31:0] unaccepted_source_event_count_gray_spi_r = 32'd0;
    reg [31:0] source_loss_incident_count_gray_spi_r = 32'd0;

    wire [31:0] recording_stall_cycle_count_next_w =
        diagnostic_clear_spi_w ? 32'd0 :
        (fifo_rst_spi_w ? recording_stall_cycle_count_spi_r :
         recording_stall_cycle_spi_w ?
            (recording_stall_cycle_count_spi_r + 32'd1) :
            recording_stall_cycle_count_spi_r);
    wire [31:0] unaccepted_source_event_count_next_w =
        diagnostic_clear_spi_w ? 32'd0 :
        (fifo_rst_spi_w ? unaccepted_source_event_count_spi_r :
         source_event_drop_spi_w ?
            (unaccepted_source_event_count_spi_r + 32'd1) :
            unaccepted_source_event_count_spi_r);
    wire [31:0] source_loss_incident_count_next_w =
        fifo_rst_spi_w ? source_loss_incident_count_spi_r :
        (source_loss_incident_spi_w ?
            (source_loss_incident_count_spi_r + 32'd1) :
            source_loss_incident_count_spi_r);

    // Register binary and Gray state from the same next value.  In particular,
    // a terminal error updates the source Gray register on the final spi_clk
    // edge; the stable value can then cross while the source clock is stopped.
    always @(posedge spi_clk_w or posedge system_reset_spi_w) begin
        if (system_reset_spi_w) begin
            source_loss_incident_count_spi_r <= 32'd0;
            source_loss_incident_count_gray_spi_r <= 32'd0;
        end else begin
            source_loss_incident_count_spi_r <=
                source_loss_incident_count_next_w;
            source_loss_incident_count_gray_spi_r <=
                bin2gray32(source_loss_incident_count_next_w);
        end
    end

    always @(posedge spi_clk_w or posedge system_reset_spi_w) begin
        if (system_reset_spi_w) begin
            recording_stall_cycle_count_spi_r <= 32'd0;
            unaccepted_source_event_count_spi_r <= 32'd0;
            recording_stall_cycle_count_gray_spi_r <= 32'd0;
            unaccepted_source_event_count_gray_spi_r <= 32'd0;
        end else begin
            recording_stall_cycle_count_spi_r <=
                recording_stall_cycle_count_next_w;
            unaccepted_source_event_count_spi_r <= unaccepted_source_event_count_next_w;
            recording_stall_cycle_count_gray_spi_r <=
                bin2gray32(recording_stall_cycle_count_next_w);
            unaccepted_source_event_count_gray_spi_r <=
                bin2gray32(unaccepted_source_event_count_next_w);
        end
    end

    wire [31:0] recording_stall_cycle_count_gray_bus_w;
    wire [31:0] unaccepted_source_event_count_gray_bus_w;
    wire [31:0] source_loss_incident_count_gray_bus_w;

    xpm_cdc_array_single #(
        .DEST_SYNC_FF(2),
        .INIT_SYNC_FF(0),
        .SIM_ASSERT_CHK(0),
        .SRC_INPUT_REG(0),
        .WIDTH(32)
    ) recording_stall_cycle_count_cdc (
        .src_clk(spi_clk_w),
        .src_in(recording_stall_cycle_count_gray_spi_r),
        .dest_clk(s00_axi_aclk),
        .dest_out(recording_stall_cycle_count_gray_bus_w)
    );

    xpm_cdc_array_single #(
        .DEST_SYNC_FF(2),
        .INIT_SYNC_FF(0),
        .SIM_ASSERT_CHK(0),
        .SRC_INPUT_REG(0),
        .WIDTH(32)
    ) unaccepted_source_event_count_cdc (
        .src_clk(spi_clk_w),
        .src_in(unaccepted_source_event_count_gray_spi_r),
        .dest_clk(s00_axi_aclk),
        .dest_out(unaccepted_source_event_count_gray_bus_w)
    );

    xpm_cdc_array_single #(
        .DEST_SYNC_FF(2),
        .INIT_SYNC_FF(0),
        .SIM_ASSERT_CHK(0),
        .SRC_INPUT_REG(0),
        .WIDTH(32)
    ) source_loss_incident_count_cdc (
        .src_clk(spi_clk_w),
        .src_in(source_loss_incident_count_gray_spi_r),
        .dest_clk(s00_axi_aclk),
        .dest_out(source_loss_incident_count_gray_bus_w)
    );

    wire [31:0] recording_stall_cycle_count_bus_w =
        gray2bin32(recording_stall_cycle_count_gray_bus_w);
    wire [31:0] unaccepted_source_event_count_bus_w =
        gray2bin32(unaccepted_source_event_count_gray_bus_w);
    wire [31:0] source_loss_incident_count_bus_w =
        gray2bin32(source_loss_incident_count_gray_bus_w);

    xpm_cdc_single #(
        .DEST_SYNC_FF(2),
        .INIT_SYNC_FF(1),
        .SIM_ASSERT_CHK(0),
        .SRC_INPUT_REG(0)
    ) spi_running_cdc_inst (
        .src_clk(spi_clk_w),
        .src_in(spi_running_raw_w),
        .dest_clk(s00_axi_aclk),
        .dest_out(spi_running_bus_w)
    );

    reg [63:0] recording_payload_word_count_r = 64'd0;
    reg [31:0] recording_end_of_session_marker_count_r = 32'd0;
    reg        recording_session_end_seen_r = 1'b0;
    reg        source_event_loss_seen_r = 1'b0;
    reg [31:0] source_loss_ack_incident_count_bus_r = 32'd0;

    wire source_loss_incident_pending_w =
        (source_loss_incident_count_bus_w !=
         source_loss_ack_incident_count_bus_r);

    always @(posedge s00_axi_aclk) begin
        if (reset || diagnostic_clear_request_w) begin
            recording_payload_word_count_r <= 64'd0;
            recording_end_of_session_marker_count_r <= 32'd0;
            recording_session_end_seen_r <= 1'b0;
        end else begin
            if (m_axis_recording_tvalid && m_axis_recording_tready &&
                (m_axis_recording_tkeep == 2'b11)) begin
                recording_payload_word_count_r <=
                    recording_payload_word_count_r + 64'd1;
            end

            if (m_axis_recording_tvalid && m_axis_recording_tready &&
                m_axis_recording_tlast &&
                (m_axis_recording_tkeep == 2'b00)) begin
                recording_end_of_session_marker_count_r <=
                    recording_end_of_session_marker_count_r + 32'd1;
                recording_session_end_seen_r <= 1'b1;
            end
        end
    end

    // Bus-domain terminal session error.  Idle clear acknowledges the current
    // monotonic source sequence.  An in-flight or later source error produces a
    // sequence mismatch and conservatively reasserts the IRQ without requiring
    // a synchronized sticky level to pass through zero first.
    always @(posedge s00_axi_aclk) begin
        if (reset) begin
            source_event_loss_seen_r <= 1'b0;
            source_loss_ack_incident_count_bus_r <= 32'd0;
        end else if (diagnostic_clear_request_w) begin
            source_event_loss_seen_r <= 1'b0;
            source_loss_ack_incident_count_bus_r <=
                source_loss_incident_count_bus_w;
        end else if (source_loss_incident_pending_w) begin
            source_event_loss_seen_r <= 1'b1;
        end
    end

    assign source_event_loss_seen_w = source_event_loss_seen_r;
    assign recording_session_end_seen_w = recording_session_end_seen_r;
    assign recording_stall_seen_w =
        (recording_stall_cycle_count_bus_w != 32'd0);
    assign recording_payload_word_count_lo_w =
        recording_payload_word_count_r[31:0];
    assign recording_payload_word_count_hi_w =
        recording_payload_word_count_r[63:32];
    assign recording_end_of_session_marker_count_w =
        recording_end_of_session_marker_count_r;
    assign unaccepted_source_event_count_w = unaccepted_source_event_count_bus_w;
    assign recording_stall_cycle_count_w =
        recording_stall_cycle_count_bus_w;
    assign spi_running = spi_running_bus_w;
    assign intan_error_irq = intan_error_irq_w;

    initial begin
        if (RECORDING_EVENT_FIFO_DEPTH_BYTES < 1136)
            $error("RECORDING_EVENT_FIFO_DEPTH_BYTES must be >=1136");
        if (COMPUTE_EVENT_FIFO_DEPTH_BYTES < 1136)
            $error("COMPUTE_EVENT_FIFO_DEPTH_BYTES must be >=1136");
    end

endmodule

`resetall
