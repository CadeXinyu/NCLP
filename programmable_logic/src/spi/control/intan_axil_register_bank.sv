`timescale 1ns / 1ps
`default_nettype none

// Flat Intan AXI-Lite ABI v3.
//
// address[15:14] = 00: registers/reserved, 01: AUX1, 10: AUX2,
//                      11: AUX3
// address[13:2]  = AUX command address (bank[1:0], index[9:0])
// address[1:0]   = zero for every defined access
//
// The three AUX windows are synchronous read/write views of the command RAMs.
// AUX writes are blocked from START acceptance through acquisition completion,
// and while a clock APPLY is queued or active. Rejections set a sticky error
// instead of returning SLVERR (Xil_Out32 does not consume a write response
// explicitly). Reads remain available while acquisition runs.
module intan_axil_register_bank #(
    parameter integer C_S00_AXI_DATA_WIDTH = 32,
    parameter integer C_S00_AXI_ADDR_WIDTH = 16
) (
    output wire        acquisition_start_request,
    input  wire        acquisition_start_acknowledge,
    output wire        acquisition_start_pending,
    input  wire        acquisition_running,
    input  wire        core_configuration_locked,
    input  wire        output_paths_drained,

    output wire        run_continuous,
    output wire        dsp_settle,
    output wire        init_dummy_mode,
    output wire [31:0] finite_frame_count,
    output wire [15:0] logical_stream_enable,
    output wire        recording_output_enable,
    output wire        compute_output_enable,
    output wire [31:0] miso_phase_primary,
    output wire [31:0] miso_phase_secondary,
    output wire [ 7:0] aux1_bank_select,
    output wire [ 7:0] aux2_bank_select,
    output wire [ 7:0] aux3_bank_select,
    output wire [ 9:0] aux1_end_index,
    output wire [ 9:0] aux2_end_index,
    output wire [ 9:0] aux3_end_index,
    output wire [ 9:0] aux1_loop_index,
    output wire [ 9:0] aux2_loop_index,
    output wire [ 9:0] aux3_loop_index,
    output wire        fast_settle_enable,
    output wire [ 3:0] fast_settle_ttl_input_index,
    output wire [ 1:0] sync_mode,
    output wire [15:0] sync_period_frames,
    output wire [15:0] sync_high_frames,

    output wire        sample_clock_apply_request,
    output wire        sample_clock_config_valid,
    output wire [ 7:0] sample_clock_output_divide,
    output wire [ 3:0] sample_clock_input_divide,
    output wire [ 6:0] sample_clock_feedback_multiply,
    input  wire        sample_clock_locked,
    input  wire        sample_clock_ready,
    input  wire        sample_clock_busy,
    input  wire        sample_clock_done,
    input  wire        sample_clock_error,
    input  wire [ 2:0] sample_clock_error_code,

    output wire        diagnostic_clear_request,
    input  wire        diagnostic_clear_busy,
    output wire        intan_error_irq,
    input  wire        source_event_loss_seen,
    input  wire        recording_session_end_seen,
    input  wire        recording_output_active,
    input  wire        recording_stall_seen,
    input  wire [31:0] recording_payload_word_count_lo,
    input  wire [31:0] recording_payload_word_count_hi,
    input  wire [31:0] recording_end_of_session_marker_count,
    input  wire [31:0] unaccepted_source_event_count,
    input  wire [31:0] recording_stall_cycle_count,

    output wire [ 1:0] aux1_bus_write_strobe,
    output wire [11:0] aux1_bus_address,
    output wire [15:0] aux1_bus_write_data,
    input  wire [15:0] aux1_bus_read_data,
    output wire [ 1:0] aux2_bus_write_strobe,
    output wire [11:0] aux2_bus_address,
    output wire [15:0] aux2_bus_write_data,
    input  wire [15:0] aux2_bus_read_data,
    output wire [ 1:0] aux3_bus_write_strobe,
    output wire [11:0] aux3_bus_address,
    output wire [15:0] aux3_bus_write_data,
    input  wire [15:0] aux3_bus_read_data,

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

    localparam [13:0] REG_BLOCK_ID                            = 14'h0000;
    localparam [13:0] REG_ABI_VERSION                         = 14'h0001;
    localparam [13:0] REG_CAPABILITIES                        = 14'h0002;
    localparam [13:0] REG_INFO                                = 14'h0003;
    localparam [13:0] REG_ACQUISITION_COMMAND                 = 14'h0004;
    localparam [13:0] REG_ACQUISITION_STATUS                  = 14'h0005;
    localparam [13:0] REG_ACQUISITION_CONFIG                  = 14'h0008;
    localparam [13:0] REG_FINITE_FRAME_COUNT                  = 14'h0009;
    localparam [13:0] REG_LOGICAL_STREAM_ENABLE               = 14'h000A;
    localparam [13:0] REG_OUTPUT_CONFIG                       = 14'h000B;
    localparam [13:0] REG_MISO_PHASE_PRIMARY                  = 14'h000C;
    localparam [13:0] REG_MISO_PHASE_SECONDARY                = 14'h000D;
    localparam [13:0] REG_SAMPLE_CLOCK_COMMAND                = 14'h0010;
    localparam [13:0] REG_SAMPLE_CLOCK_CONFIG                 = 14'h0011;
    localparam [13:0] REG_SAMPLE_CLOCK_STATUS                 = 14'h0012;
    localparam [13:0] REG_AUX1_BANK_SELECT                    = 14'h0014;
    localparam [13:0] REG_AUX1_END_INDEX                      = 14'h0015;
    localparam [13:0] REG_AUX1_LOOP_INDEX                     = 14'h0016;
    localparam [13:0] REG_AUX2_BANK_SELECT                    = 14'h0018;
    localparam [13:0] REG_AUX2_END_INDEX                      = 14'h0019;
    localparam [13:0] REG_AUX2_LOOP_INDEX                     = 14'h001A;
    localparam [13:0] REG_AUX3_BANK_SELECT                    = 14'h001C;
    localparam [13:0] REG_AUX3_END_INDEX                      = 14'h001D;
    localparam [13:0] REG_AUX3_LOOP_INDEX                     = 14'h001E;
    localparam [13:0] REG_FAST_SETTLE_CONFIG                  = 14'h0020;
    localparam [13:0] REG_SYNC_MODE                           = 14'h0024;
    localparam [13:0] REG_SYNC_PERIOD_FRAMES                  = 14'h0025;
    localparam [13:0] REG_SYNC_HIGH_FRAMES                    = 14'h0026;
    localparam [13:0] REG_OUTPUT_STATUS                       = 14'h0028;
    localparam [13:0] REG_DIAGNOSTIC_COMMAND                  = 14'h0029;
    localparam [13:0] REG_RECORDING_PAYLOAD_WORD_COUNT_LO     = 14'h002A;
    localparam [13:0] REG_RECORDING_PAYLOAD_WORD_COUNT_HI     = 14'h002B;
    localparam [13:0] REG_RECORDING_END_OF_SESSION_MARKER_COUNT = 14'h002C;
    localparam [13:0] REG_UNACCEPTED_SOURCE_EVENT_COUNT       = 14'h002D;
    localparam [13:0] REG_RECORDING_STALL_CYCLE_COUNT         = 14'h002E;
    localparam [13:0] REG_ERROR_STATUS                        = 14'h0030;
    localparam [13:0] REG_ERROR_ENABLE                        = 14'h0031;
    localparam [13:0] REG_ERROR_INCIDENT_COUNT                = 14'h0032;
    localparam [13:0] REG_LAST_ERROR_CODE                     = 14'h0033;
    localparam [13:0] REG_STREAM_SOURCE_MAP_LO                = 14'h0038;
    localparam [13:0] REG_STREAM_SOURCE_MAP_HI                = 14'h0039;

    localparam [31:0] BLOCK_ID_VALUE                 = 32'h494E_544E;
    localparam [31:0] ABI_VERSION_VALUE              = 32'h0003_0000;
    localparam [31:0] CAPABILITIES_VALUE             = 32'h0000_03FF;
    localparam [31:0] INFO_VALUE                     = 32'h1010_0400;
    localparam [31:0] STREAM_SOURCE_MAP_LO_VALUE     = 32'hB3A2_9180;
    localparam [31:0] STREAM_SOURCE_MAP_HI_VALUE     = 32'hF7E6_D5C4;
    localparam [31:0] DEFAULT_SAMPLE_CLOCK_CONFIG    = 32'h0002_440F;

    localparam [6:0] ERROR_SOURCE_EVENT_LOSS         = 7'b000_0001;
    localparam [6:0] ERROR_CLOCK_FAILURE             = 7'b100_0000;

    function automatic [31:0] apply_wstrb(
        input [31:0] current_value,
        input [31:0] requested_value,
        input [ 3:0] write_strobe
    );
        integer byte_index;
        begin
            apply_wstrb = current_value;
            for (byte_index = 0; byte_index < 4; byte_index = byte_index + 1)
                if (write_strobe[byte_index])
                    apply_wstrb[byte_index*8 +: 8] =
                        requested_value[byte_index*8 +: 8];
        end
    endfunction

    function automatic logic clock_profile_valid(
        input [7:0] profile_o,
        input [3:0] profile_d,
        input [6:0] profile_m
    );
        begin
            clock_profile_valid =
                (profile_d == 4'd4) &&
                (((profile_o == 8'd80) && (profile_m == 7'd32)) ||
                 ((profile_o == 8'd40) && (profile_m == 7'd32)) ||
                 ((profile_o == 8'd30) && (profile_m == 7'd36)) ||
                 ((profile_o == 8'd20) && (profile_m == 7'd32)) ||
                 ((profile_o == 8'd16) && (profile_m == 7'd32)) ||
                 ((profile_o == 8'd15) && (profile_m == 7'd36)));
        end
    endfunction

    reg [C_S00_AXI_ADDR_WIDTH-1:0] write_address_r;
    reg                              write_address_valid_r;
    reg [31:0]                       write_data_r;
    reg [ 3:0]                       write_strobe_r;
    reg                              write_data_valid_r;
    reg                              bvalid_r;
    reg [1:0]                        bresp_r;
    reg                              rvalid_r;
    reg [1:0]                        rresp_r;
    reg [31:0]                       rdata_r;
    reg                              aux_read_pending_r;
    reg [1:0]                        aux_read_select_r;

    wire aw_fire = s00_axi_awvalid && s00_axi_awready;
    wire w_fire  = s00_axi_wvalid && s00_axi_wready;
    wire [C_S00_AXI_ADDR_WIDTH-1:0] write_address_w =
        write_address_valid_r ? write_address_r : s00_axi_awaddr;
    wire [31:0] write_data_w = write_data_valid_r ? write_data_r : s00_axi_wdata;
    wire [3:0] write_strobe_w = write_data_valid_r ? write_strobe_r : s00_axi_wstrb;
    wire write_fire = !bvalid_r &&
                      (write_address_valid_r || aw_fire) &&
                      (write_data_valid_r || w_fire);
    wire [1:0] write_region_w = write_address_w[15:14];
    wire [13:0] write_word_w = write_address_w[15:2];
    wire write_aligned_w = (write_address_w[1:0] == 2'b00);

    // Writes take priority over reads on the synchronous AXI-side BRAM port.
    // AR is accepted only when neither half of a write is presented or held.
    assign s00_axi_awready = !bvalid_r && !write_address_valid_r;
    assign s00_axi_wready  = !bvalid_r && !write_data_valid_r;
    assign s00_axi_bvalid  = bvalid_r;
    assign s00_axi_bresp   = bresp_r;
    assign s00_axi_arready = !rvalid_r && !aux_read_pending_r &&
                             !write_address_valid_r && !write_data_valid_r &&
                             !s00_axi_awvalid && !s00_axi_wvalid;
    assign s00_axi_rvalid = rvalid_r;
    assign s00_axi_rresp  = rresp_r;
    assign s00_axi_rdata  = rdata_r;
    wire read_fire = s00_axi_arvalid && s00_axi_arready;

    reg [2:0] acquisition_config_r;
    reg [31:0] finite_frame_count_r;
    reg [15:0] logical_stream_enable_r;
    reg [1:0] output_config_r;
    reg [31:0] miso_phase_primary_r;
    reg [31:0] miso_phase_secondary_r;
    reg [7:0] aux1_bank_select_r, aux2_bank_select_r, aux3_bank_select_r;
    reg [9:0] aux1_end_index_r, aux2_end_index_r, aux3_end_index_r;
    reg [9:0] aux1_loop_index_r, aux2_loop_index_r, aux3_loop_index_r;
    reg [7:0] fast_settle_config_r;
    reg [1:0] sync_mode_r;
    reg [15:0] sync_period_frames_r, sync_high_frames_r;
    reg [31:0] sample_clock_config_r;
    reg sample_clock_apply_request_r;
    reg sample_clock_apply_inflight_r;
    reg sample_clock_request_issued_r;
    reg acquisition_start_pending_r;
    reg diagnostic_clear_request_r;

    reg [6:0] error_status_r;
    reg [6:0] error_enable_r;
    reg [31:0] error_incident_count_r;
    reg [3:0] last_error_code_r;
    reg source_event_loss_d_r;
    reg recording_stall_d_r;
    reg sample_clock_error_d_r;

    assign run_continuous = acquisition_config_r[0];
    assign dsp_settle = acquisition_config_r[1];
    assign init_dummy_mode = acquisition_config_r[2];
    assign finite_frame_count = finite_frame_count_r;
    assign logical_stream_enable = logical_stream_enable_r;
    assign recording_output_enable = output_config_r[0];
    assign compute_output_enable = output_config_r[1];
    assign miso_phase_primary = miso_phase_primary_r;
    assign miso_phase_secondary = miso_phase_secondary_r;
    assign aux1_bank_select = aux1_bank_select_r;
    assign aux2_bank_select = aux2_bank_select_r;
    assign aux3_bank_select = aux3_bank_select_r;
    assign aux1_end_index = aux1_end_index_r;
    assign aux2_end_index = aux2_end_index_r;
    assign aux3_end_index = aux3_end_index_r;
    assign aux1_loop_index = aux1_loop_index_r;
    assign aux2_loop_index = aux2_loop_index_r;
    assign aux3_loop_index = aux3_loop_index_r;
    assign fast_settle_enable = fast_settle_config_r[0];
    assign fast_settle_ttl_input_index = fast_settle_config_r[7:4];
    assign sync_mode = sync_mode_r;
    assign sync_period_frames = sync_period_frames_r;
    assign sync_high_frames = sync_high_frames_r;

    assign sample_clock_output_divide = sample_clock_config_r[7:0];
    assign sample_clock_input_divide = sample_clock_config_r[11:8];
    assign sample_clock_feedback_multiply = sample_clock_config_r[18:12];
    assign sample_clock_config_valid = clock_profile_valid(
        sample_clock_output_divide,
        sample_clock_input_divide,
        sample_clock_feedback_multiply);
    assign sample_clock_apply_request = sample_clock_apply_request_r;
    assign diagnostic_clear_request = diagnostic_clear_request_r;
    assign acquisition_start_pending = acquisition_start_pending_r;

    wire access_locked_w = acquisition_start_pending_r ||
                           core_configuration_locked ||
                           sample_clock_apply_inflight_r;
    wire start_config_frozen_w = acquisition_start_pending_r ||
                                 (core_configuration_locked &&
                                  !acquisition_running);
    assign acquisition_start_request = acquisition_start_pending_r &&
                                       sample_clock_ready &&
                                       sample_clock_locked &&
                                       !sample_clock_busy &&
                                       !sample_clock_apply_inflight_r &&
                                       !sample_clock_error;

    wire source_event_loss_rise_w = source_event_loss_seen &&
                                     !source_event_loss_d_r;
    wire recording_stall_rise_w = recording_stall_seen &&
                                  !recording_stall_d_r;
    wire sample_clock_error_rise_w = sample_clock_error &&
                                      !sample_clock_error_d_r;
    wire start_command_write_w = write_fire && write_aligned_w &&
        (write_region_w == 2'b00) &&
        (write_word_w == REG_ACQUISITION_COMMAND) &&
        write_strobe_w[0] && write_data_w[0];
    wire aux_sequence_configuration_valid_w =
        (aux1_loop_index_r <= aux1_end_index_r) &&
        (aux2_loop_index_r <= aux2_end_index_r) &&
        (aux3_loop_index_r <= aux3_end_index_r);
    wire sync_configuration_valid_w =
        (sync_mode_r <= 2'd2) &&
        ((sync_mode_r != 2'd1) ||
         ((sync_period_frames_r != 16'd0) &&
          (sync_high_frames_r != 16'd0) &&
          (sync_high_frames_r <= sync_period_frames_r)));
    wire start_configuration_valid_w =
        (logical_stream_enable_r != 16'd0) &&
        (acquisition_config_r[0] || (finite_frame_count_r != 32'd0)) &&
        aux_sequence_configuration_valid_w &&
        sync_configuration_valid_w;
    wire start_rejected_write_w = start_command_write_w &&
        (access_locked_w || sample_clock_busy || sample_clock_error ||
         !output_paths_drained || !start_configuration_valid_w);
    wire start_pending_clock_error_w = acquisition_start_pending_r &&
                                       sample_clock_error;
    wire start_accepted_write_w = start_command_write_w &&
                                  !start_rejected_write_w;

    wire clock_command_write_w = write_fire && write_aligned_w &&
        (write_region_w == 2'b00) &&
        (write_word_w == REG_SAMPLE_CLOCK_COMMAND) &&
        write_strobe_w[0] && write_data_w[0];
    // A live manager error is deliberately not a rejection condition: a
    // valid APPLY is the supported path out of the manager's error state.
    wire clock_request_rejected_write_w = clock_command_write_w &&
        (access_locked_w || sample_clock_busy || !sample_clock_ready);
    wire clock_request_accepted_write_w = clock_command_write_w &&
                                           !clock_request_rejected_write_w;

    function automatic logic protected_config_register(input [13:0] word_address);
        begin
            case (word_address)
                REG_ACQUISITION_CONFIG,
                REG_SAMPLE_CLOCK_CONFIG,
                REG_FINITE_FRAME_COUNT,
                REG_LOGICAL_STREAM_ENABLE,
                REG_OUTPUT_CONFIG,
                REG_MISO_PHASE_PRIMARY,
                REG_MISO_PHASE_SECONDARY,
                REG_AUX1_BANK_SELECT,
                REG_AUX2_BANK_SELECT,
                REG_AUX3_BANK_SELECT,
                REG_AUX1_END_INDEX,
                REG_AUX2_END_INDEX,
                REG_AUX3_END_INDEX,
                REG_AUX1_LOOP_INDEX,
                REG_AUX2_LOOP_INDEX,
                REG_AUX3_LOOP_INDEX,
                REG_FAST_SETTLE_CONFIG,
                REG_SYNC_MODE,
                REG_SYNC_PERIOD_FRAMES,
                REG_SYNC_HIGH_FRAMES:
                    protected_config_register = 1'b1;
                default: protected_config_register = 1'b0;
            endcase
        end
    endfunction

    wire protected_config_write_w = write_fire && write_aligned_w &&
        (write_region_w == 2'b00) &&
        (|write_strobe_w) &&
        protected_config_register(write_word_w);
    wire clock_config_write_blocked_w = protected_config_write_w &&
        (write_word_w == REG_SAMPLE_CLOCK_CONFIG) &&
        (access_locked_w || sample_clock_busy);
    wire control_write_rejected_w = protected_config_write_w &&
        (start_config_frozen_w || clock_config_write_blocked_w);

    wire aux_write_attempt_w = write_fire && write_aligned_w &&
        (write_region_w != 2'b00) && (|write_strobe_w[1:0]);
    wire aux_write_rejected_w = aux_write_attempt_w && access_locked_w;
    wire aux_write_allowed_w = aux_write_attempt_w && !access_locked_w;

    wire diagnostic_command_write_w = write_fire && write_aligned_w &&
        (write_region_w == 2'b00) &&
        (write_word_w == REG_DIAGNOSTIC_COMMAND) &&
        write_strobe_w[0] && write_data_w[0];
    // Busy serializes only CLEAR itself.  It must not enter access_locked_w:
    // if spi_clk has stopped, a recovery APPLY must remain admissible.
    wire diagnostic_command_rejected_w = diagnostic_command_write_w &&
        (access_locked_w || diagnostic_clear_busy);

    wire [6:0] incident_status_set_w = {
        sample_clock_error_rise_w,
        (control_write_rejected_w || diagnostic_command_rejected_w),
        clock_request_rejected_write_w,
        (start_rejected_write_w || start_pending_clock_error_w),
        aux_write_rejected_w,
        recording_stall_rise_w,
        source_event_loss_rise_w
    };
    wire [2:0] incident_increment_w =
        {2'd0, incident_status_set_w[0]} +
        {2'd0, incident_status_set_w[1]} +
        {2'd0, incident_status_set_w[2]} +
        {2'd0, incident_status_set_w[3]} +
        {2'd0, incident_status_set_w[4]} +
        {2'd0, incident_status_set_w[5]} +
        {2'd0, incident_status_set_w[6]};
    wire error_status_clear_write_w = write_fire && write_aligned_w &&
        (write_region_w == 2'b00) &&
        (write_word_w == REG_ERROR_STATUS);
    wire [31:0] error_status_clear_value_w = apply_wstrb(
        32'd0, write_data_w, write_strobe_w);

    wire [11:0] ram_bus_address_w = write_fire ?
        write_address_w[13:2] : s00_axi_araddr[13:2];
    assign aux1_bus_address = ram_bus_address_w;
    assign aux2_bus_address = ram_bus_address_w;
    assign aux3_bus_address = ram_bus_address_w;
    assign aux1_bus_write_data = write_data_w[15:0];
    assign aux2_bus_write_data = write_data_w[15:0];
    assign aux3_bus_write_data = write_data_w[15:0];
    assign aux1_bus_write_strobe =
        (aux_write_allowed_w && write_region_w == 2'b01) ?
        write_strobe_w[1:0] : 2'b00;
    assign aux2_bus_write_strobe =
        (aux_write_allowed_w && write_region_w == 2'b10) ?
        write_strobe_w[1:0] : 2'b00;
    assign aux3_bus_write_strobe =
        (aux_write_allowed_w && write_region_w == 2'b11) ?
        write_strobe_w[1:0] : 2'b00;

    assign intan_error_irq = |(error_status_r & error_enable_r);

    always @(posedge s00_axi_aclk) begin
        sample_clock_apply_request_r <= 1'b0;
        diagnostic_clear_request_r <= 1'b0;

        if (!s00_axi_aresetn) begin
            write_address_r <= {C_S00_AXI_ADDR_WIDTH{1'b0}};
            write_address_valid_r <= 1'b0;
            write_data_r <= 32'd0;
            write_strobe_r <= 4'd0;
            write_data_valid_r <= 1'b0;
            bvalid_r <= 1'b0;
            bresp_r <= 2'b00;

            acquisition_config_r <= 3'd0;
            finite_frame_count_r <= 32'd0;
            logical_stream_enable_r <= 16'd0;
            output_config_r <= 2'd0;
            miso_phase_primary_r <= 32'd0;
            miso_phase_secondary_r <= 32'd0;
            aux1_bank_select_r <= 8'd0;
            aux2_bank_select_r <= 8'd0;
            aux3_bank_select_r <= 8'd0;
            aux1_end_index_r <= 10'd0;
            aux2_end_index_r <= 10'd0;
            aux3_end_index_r <= 10'd0;
            aux1_loop_index_r <= 10'd0;
            aux2_loop_index_r <= 10'd0;
            aux3_loop_index_r <= 10'd0;
            fast_settle_config_r <= 8'd0;
            sync_mode_r <= 2'd0;
            sync_period_frames_r <= 16'd1;
            sync_high_frames_r <= 16'd1;
            sample_clock_config_r <= DEFAULT_SAMPLE_CLOCK_CONFIG;
            sample_clock_apply_inflight_r <= 1'b0;
            sample_clock_request_issued_r <= 1'b0;
            acquisition_start_pending_r <= 1'b0;

            error_status_r <= 7'd0;
            error_enable_r <= ERROR_SOURCE_EVENT_LOSS | ERROR_CLOCK_FAILURE;
            error_incident_count_r <= 32'd0;
            last_error_code_r <= 4'd0;
            source_event_loss_d_r <= 1'b0;
            recording_stall_d_r <= 1'b0;
            sample_clock_error_d_r <= 1'b0;
        end else begin
            source_event_loss_d_r <= source_event_loss_seen;
            recording_stall_d_r <= recording_stall_seen;
            sample_clock_error_d_r <= sample_clock_error;

            if (bvalid_r && s00_axi_bready)
                bvalid_r <= 1'b0;

            if (aw_fire && !write_fire) begin
                write_address_r <= s00_axi_awaddr;
                write_address_valid_r <= 1'b1;
            end
            if (w_fire && !write_fire) begin
                write_data_r <= s00_axi_wdata;
                write_strobe_r <= s00_axi_wstrb;
                write_data_valid_r <= 1'b1;
            end

            if (write_fire) begin
                write_address_valid_r <= 1'b0;
                write_data_valid_r <= 1'b0;
                bvalid_r <= 1'b1;
                bresp_r <= 2'b00;

                if (write_aligned_w && write_region_w == 2'b00) begin
                    if (!control_write_rejected_w) begin
                        case (write_word_w)
                            REG_ACQUISITION_CONFIG:
                                acquisition_config_r <= apply_wstrb(
                                    {29'd0, acquisition_config_r},
                                    write_data_w, write_strobe_w);
                            REG_FINITE_FRAME_COUNT:
                                finite_frame_count_r <= apply_wstrb(
                                    finite_frame_count_r, write_data_w,
                                    write_strobe_w);
                            REG_LOGICAL_STREAM_ENABLE:
                                logical_stream_enable_r <= apply_wstrb(
                                    {16'd0, logical_stream_enable_r},
                                    write_data_w, write_strobe_w);
                            REG_OUTPUT_CONFIG:
                                output_config_r <= apply_wstrb(
                                    {30'd0, output_config_r}, write_data_w,
                                    write_strobe_w);
                            REG_MISO_PHASE_PRIMARY:
                                miso_phase_primary_r <= apply_wstrb(
                                    miso_phase_primary_r, write_data_w,
                                    write_strobe_w);
                            REG_MISO_PHASE_SECONDARY:
                                miso_phase_secondary_r <= apply_wstrb(
                                    miso_phase_secondary_r, write_data_w,
                                    write_strobe_w);
                            REG_AUX1_BANK_SELECT:
                                aux1_bank_select_r <= apply_wstrb(
                                    {24'd0, aux1_bank_select_r}, write_data_w,
                                    write_strobe_w);
                            REG_AUX2_BANK_SELECT:
                                aux2_bank_select_r <= apply_wstrb(
                                    {24'd0, aux2_bank_select_r}, write_data_w,
                                    write_strobe_w);
                            REG_AUX3_BANK_SELECT:
                                aux3_bank_select_r <= apply_wstrb(
                                    {24'd0, aux3_bank_select_r}, write_data_w,
                                    write_strobe_w);
                            REG_AUX1_END_INDEX:
                                aux1_end_index_r <= apply_wstrb(
                                    {22'd0, aux1_end_index_r}, write_data_w,
                                    write_strobe_w);
                            REG_AUX2_END_INDEX:
                                aux2_end_index_r <= apply_wstrb(
                                    {22'd0, aux2_end_index_r}, write_data_w,
                                    write_strobe_w);
                            REG_AUX3_END_INDEX:
                                aux3_end_index_r <= apply_wstrb(
                                    {22'd0, aux3_end_index_r}, write_data_w,
                                    write_strobe_w);
                            REG_AUX1_LOOP_INDEX:
                                aux1_loop_index_r <= apply_wstrb(
                                    {22'd0, aux1_loop_index_r}, write_data_w,
                                    write_strobe_w);
                            REG_AUX2_LOOP_INDEX:
                                aux2_loop_index_r <= apply_wstrb(
                                    {22'd0, aux2_loop_index_r}, write_data_w,
                                    write_strobe_w);
                            REG_AUX3_LOOP_INDEX:
                                aux3_loop_index_r <= apply_wstrb(
                                    {22'd0, aux3_loop_index_r}, write_data_w,
                                    write_strobe_w);
                            REG_FAST_SETTLE_CONFIG:
                                fast_settle_config_r <= apply_wstrb(
                                    {24'd0, fast_settle_config_r}, write_data_w,
                                    write_strobe_w);
                            REG_SYNC_MODE:
                                sync_mode_r <= apply_wstrb(
                                    {30'd0, sync_mode_r}, write_data_w,
                                    write_strobe_w);
                            REG_SYNC_PERIOD_FRAMES:
                                sync_period_frames_r <= apply_wstrb(
                                    {16'd0, sync_period_frames_r}, write_data_w,
                                    write_strobe_w);
                            REG_SYNC_HIGH_FRAMES:
                                sync_high_frames_r <= apply_wstrb(
                                    {16'd0, sync_high_frames_r}, write_data_w,
                                    write_strobe_w);
                            default: ;
                        endcase
                    end

                    case (write_word_w)
                        REG_SAMPLE_CLOCK_CONFIG:
                            if (!access_locked_w &&
                                !sample_clock_busy)
                                sample_clock_config_r <= apply_wstrb(
                                    sample_clock_config_r, write_data_w,
                                    write_strobe_w) & 32'h0007_FFFF;
                        REG_ERROR_ENABLE:
                            error_enable_r <= apply_wstrb(
                                {25'd0, error_enable_r}, write_data_w,
                                write_strobe_w);
                        default: ;
                    endcase
                end
            end

            if (start_accepted_write_w)
                acquisition_start_pending_r <= 1'b1;
            if (acquisition_start_acknowledge)
                acquisition_start_pending_r <= 1'b0;
            if (start_pending_clock_error_w)
                acquisition_start_pending_r <= 1'b0;

            if (clock_request_accepted_write_w) begin
                sample_clock_apply_inflight_r <= 1'b1;
                sample_clock_request_issued_r <= output_paths_drained;
                if (output_paths_drained)
                    sample_clock_apply_request_r <= 1'b1;
            end else if (sample_clock_apply_inflight_r &&
                         !sample_clock_request_issued_r &&
                         output_paths_drained) begin
                sample_clock_apply_request_r <= 1'b1;
                sample_clock_request_issued_r <= 1'b1;
            end
            if (sample_clock_done) begin
                sample_clock_apply_inflight_r <= 1'b0;
                sample_clock_request_issued_r <= 1'b0;
            end
            if (diagnostic_command_write_w && !diagnostic_command_rejected_w)
                diagnostic_clear_request_r <= 1'b1;

            // ERROR_STATUS is write-one-to-clear.  New incidents below take
            // precedence over a simultaneous clear.
            if (error_status_clear_write_w)
                error_status_r <=
                    (error_status_r & ~error_status_clear_value_w[6:0]) |
                    incident_status_set_w;
            else if (|incident_status_set_w)
                error_status_r <= error_status_r | incident_status_set_w;

            // Later assignments retain the existing highest-code priority when
            // more than one incident occurs in the same cycle.
            if (source_event_loss_rise_w) begin
                last_error_code_r <= 4'd1;
            end
            if (recording_stall_rise_w) begin
                last_error_code_r <= 4'd2;
            end
            if (aux_write_rejected_w) begin
                last_error_code_r <= 4'd3;
            end
            if (start_rejected_write_w || start_pending_clock_error_w) begin
                last_error_code_r <= 4'd4;
            end
            if (clock_request_rejected_write_w) begin
                last_error_code_r <= 4'd5;
            end
            if (control_write_rejected_w || diagnostic_command_rejected_w) begin
                last_error_code_r <= 4'd6;
            end
            if (sample_clock_error_rise_w) begin
                last_error_code_r <= 4'd7;
            end
            if (incident_increment_w != 0)
                error_incident_count_r <= error_incident_count_r +
                                          incident_increment_w;
        end
    end

    wire [31:0] acquisition_status_w = {
        28'd0,
        error_status_r[3],
        access_locked_w,
        acquisition_start_pending_r,
        acquisition_running
    };
    wire [2:0] sample_clock_status_error_code_w = sample_clock_error ?
        sample_clock_error_code : (error_status_r[4] ? 3'd7 : 3'd0);
    wire [31:0] sample_clock_status_w = {
        25'd0,
        sample_clock_status_error_code_w,
        (sample_clock_error || error_status_r[4]),
        (sample_clock_busy || sample_clock_apply_inflight_r),
        sample_clock_ready,
        sample_clock_locked
    };
    wire [31:0] output_status_w = {
        28'd0,
        recording_stall_seen,
        recording_output_active,
        recording_session_end_seen,
        source_event_loss_seen
    };

    always @(posedge s00_axi_aclk) begin
        if (!s00_axi_aresetn) begin
            rvalid_r <= 1'b0;
            rresp_r <= 2'b00;
            rdata_r <= 32'd0;
            aux_read_pending_r <= 1'b0;
            aux_read_select_r <= 2'b00;
        end else begin
            if (rvalid_r && s00_axi_rready)
                rvalid_r <= 1'b0;

            if (aux_read_pending_r) begin
                aux_read_pending_r <= 1'b0;
                rvalid_r <= 1'b1;
                rresp_r <= 2'b00;
                case (aux_read_select_r)
                    2'b01: rdata_r <= {16'd0, aux1_bus_read_data};
                    2'b10: rdata_r <= {16'd0, aux2_bus_read_data};
                    2'b11: rdata_r <= {16'd0, aux3_bus_read_data};
                    default: rdata_r <= 32'd0;
                endcase
            end else if (read_fire) begin
                rresp_r <= 2'b00;
                if (s00_axi_araddr[1:0] != 2'b00) begin
                    rvalid_r <= 1'b1;
                    rdata_r <= 32'd0;
                end else if (s00_axi_araddr[15:14] != 2'b00) begin
                    aux_read_pending_r <= 1'b1;
                    aux_read_select_r <= s00_axi_araddr[15:14];
                end else begin
                    rvalid_r <= 1'b1;
                    case (s00_axi_araddr[15:2])
                        REG_BLOCK_ID:                 rdata_r <= BLOCK_ID_VALUE;
                        REG_ABI_VERSION:              rdata_r <= ABI_VERSION_VALUE;
                        REG_CAPABILITIES:             rdata_r <= CAPABILITIES_VALUE;
                        REG_INFO:                     rdata_r <= INFO_VALUE;
                        REG_STREAM_SOURCE_MAP_LO:     rdata_r <= STREAM_SOURCE_MAP_LO_VALUE;
                        REG_STREAM_SOURCE_MAP_HI:     rdata_r <= STREAM_SOURCE_MAP_HI_VALUE;
                        REG_ACQUISITION_COMMAND:      rdata_r <= 32'd0;
                        REG_ACQUISITION_CONFIG:       rdata_r <= {29'd0, acquisition_config_r};
                        REG_ACQUISITION_STATUS:       rdata_r <= acquisition_status_w;
                        REG_FINITE_FRAME_COUNT:       rdata_r <= finite_frame_count_r;
                        REG_LOGICAL_STREAM_ENABLE:    rdata_r <= {16'd0, logical_stream_enable_r};
                        REG_OUTPUT_CONFIG:            rdata_r <= {30'd0, output_config_r};
                        REG_MISO_PHASE_PRIMARY:       rdata_r <= miso_phase_primary_r;
                        REG_MISO_PHASE_SECONDARY:     rdata_r <= miso_phase_secondary_r;
                        REG_SAMPLE_CLOCK_CONFIG:      rdata_r <= sample_clock_config_r;
                        REG_SAMPLE_CLOCK_COMMAND:     rdata_r <= 32'd0;
                        REG_SAMPLE_CLOCK_STATUS:      rdata_r <= sample_clock_status_w;
                        REG_AUX1_BANK_SELECT:         rdata_r <= {24'd0, aux1_bank_select_r};
                        REG_AUX2_BANK_SELECT:         rdata_r <= {24'd0, aux2_bank_select_r};
                        REG_AUX3_BANK_SELECT:         rdata_r <= {24'd0, aux3_bank_select_r};
                        REG_AUX1_END_INDEX:           rdata_r <= {22'd0, aux1_end_index_r};
                        REG_AUX2_END_INDEX:           rdata_r <= {22'd0, aux2_end_index_r};
                        REG_AUX3_END_INDEX:           rdata_r <= {22'd0, aux3_end_index_r};
                        REG_AUX1_LOOP_INDEX:          rdata_r <= {22'd0, aux1_loop_index_r};
                        REG_AUX2_LOOP_INDEX:          rdata_r <= {22'd0, aux2_loop_index_r};
                        REG_AUX3_LOOP_INDEX:          rdata_r <= {22'd0, aux3_loop_index_r};
                        REG_FAST_SETTLE_CONFIG:       rdata_r <= {24'd0, fast_settle_config_r};
                        REG_SYNC_MODE:                rdata_r <= {30'd0, sync_mode_r};
                        REG_SYNC_PERIOD_FRAMES:       rdata_r <= {16'd0, sync_period_frames_r};
                        REG_SYNC_HIGH_FRAMES:         rdata_r <= {16'd0, sync_high_frames_r};
                        REG_OUTPUT_STATUS:            rdata_r <= output_status_w;
                        REG_DIAGNOSTIC_COMMAND:       rdata_r <= 32'd0;
                        REG_RECORDING_PAYLOAD_WORD_COUNT_LO: rdata_r <= recording_payload_word_count_lo;
                        REG_RECORDING_PAYLOAD_WORD_COUNT_HI: rdata_r <= recording_payload_word_count_hi;
                        REG_RECORDING_END_OF_SESSION_MARKER_COUNT: rdata_r <= recording_end_of_session_marker_count;
                        REG_UNACCEPTED_SOURCE_EVENT_COUNT:  rdata_r <= unaccepted_source_event_count;
                        REG_RECORDING_STALL_CYCLE_COUNT: rdata_r <= recording_stall_cycle_count;
                        REG_ERROR_STATUS:             rdata_r <= {25'd0, error_status_r};
                        REG_ERROR_ENABLE:             rdata_r <= {25'd0, error_enable_r};
                        REG_ERROR_INCIDENT_COUNT:     rdata_r <= error_incident_count_r;
                        REG_LAST_ERROR_CODE:          rdata_r <= {28'd0, last_error_code_r};
                        default:                      rdata_r <= 32'd0;
                    endcase
                end
            end
        end
    end

    initial begin
        if (C_S00_AXI_DATA_WIDTH != 32)
            $error("Intan ABI v3 requires a 32-bit AXI-Lite data bus");
        if (C_S00_AXI_ADDR_WIDTH != 16)
            $error("Intan ABI v3 requires an exact 16-bit AXI-Lite aperture");
    end

endmodule

`default_nettype wire
