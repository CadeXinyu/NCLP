`timescale 1ns / 1ps
`default_nettype none

// AXI/event adapter for the synthesized fixed-point HLS detector. Configuration
// and threshold telemetry retain IEEE-754 binary32 words; MEAN_SQUARE is a raw
// uint32 mean square in ADC-count-squared units.
module ripple_detector #(
    parameter integer C_S00_AXI_DATA_WIDTH = 32,
    parameter integer C_S00_AXI_ADDR_WIDTH = 12
) (
    output reg stim_trigger,
    input wire local_compute_stream_active,
    input wire acquisition_30ksps,
    input wire [63:0] s_axis_algo_sample_tdata,
    input wire [7:0] s_axis_algo_sample_tkeep,
    input wire s_axis_algo_sample_tvalid,
    output wire s_axis_algo_sample_tready,
    input wire s_axis_algo_sample_tlast,
    input wire s00_axi_aclk,
    input wire s00_axi_aresetn,
    input wire [C_S00_AXI_ADDR_WIDTH-1:0] s00_axi_awaddr,
    input wire [2:0] s00_axi_awprot,
    input wire s00_axi_awvalid,
    output wire s00_axi_awready,
    input wire [C_S00_AXI_DATA_WIDTH-1:0] s00_axi_wdata,
    input wire [(C_S00_AXI_DATA_WIDTH/8)-1:0] s00_axi_wstrb,
    input wire s00_axi_wvalid,
    output wire s00_axi_wready,
    output wire [1:0] s00_axi_bresp,
    output wire s00_axi_bvalid,
    input wire s00_axi_bready,
    input wire [C_S00_AXI_ADDR_WIDTH-1:0] s00_axi_araddr,
    input wire [2:0] s00_axi_arprot,
    input wire s00_axi_arvalid,
    output wire s00_axi_arready,
    output wire [C_S00_AXI_DATA_WIDTH-1:0] s00_axi_rdata,
    output wire [1:0] s00_axi_rresp,
    output wire s00_axi_rvalid,
    input wire s00_axi_rready
);
    localparam [31:0] BLOCK_ID_VALUE = 32'h5250_5754; // RPWT
    localparam [31:0] ABI_VERSION_VALUE = 32'h0003_0000;
    localparam [31:0] CAPABILITIES_VALUE = 32'h0000_01FF;
    localparam [31:0] LIMITS_VALUE = 32'h001E_0100;
    localparam [11:0] ADDR_BLOCK_ID = 12'h000, ADDR_ABI_VERSION = 12'h004,
        ADDR_CAPABILITIES = 12'h008, ADDR_LIMITS = 12'h00C,
        ADDR_COMMAND = 12'h010, ADDR_STATUS = 12'h014,
        ADDR_INPUT_CHANNEL_ID = 12'h020, ADDR_FILTER_KIND = 12'h024,
        ADDR_FIR_TAP_COUNT = 12'h028, ADDR_POWER_WINDOW_US = 12'h02C,
        ADDR_REFRACTORY_PERIOD_MS = 12'h030,
        ADDR_BASELINE_MEAN_BITS = 12'h034,
        ADDR_BASELINE_STDDEV_BITS = 12'h038,
        ADDR_THRESHOLD_K_BITS = 12'h03C,
        ADDR_COEFFICIENT_INDEX = 12'h040,
        ADDR_COEFFICIENT_BITS = 12'h044,
        ADDR_TRIGGER_REQUEST_COUNT = 12'h050,
        ADDR_POWER_SAMPLE_COUNT = 12'h054,
        ADDR_MEAN_SQUARE = 12'h058,
        ADDR_SUM_SQUARE_THRESHOLD_BITS = 12'h05C,
        ADDR_LAST_INPUT_TIMESTAMP = 12'h060;
    localparam [7:0] COMMAND_STOP = 8'd0, COMMAND_START = 8'd1,
        COMMAND_VALIDATE = 8'd2, COMMAND_RESTORE_DEFAULTS = 8'd4;
    localparam [31:0] HLS_ACTION_APPLY_CONFIG_AND_RESET = 32'd0,
        HLS_ACTION_PROCESS_SAMPLE = 32'd1,
        HLS_ACTION_WRITE_FIR_COEFFICIENT = 32'd2,
        HLS_ACTION_VALIDATE_CONFIG = 32'd3,
        HLS_ACTION_RESTORE_DEFAULT_COEFFICIENTS = 32'd4,
        HLS_ACTION_UPDATE_THRESHOLD_K = 32'd5,
        HLS_ACTION_WRITE_IIR_COEFFICIENT = 32'd6;

    wire register_write_enable;
    wire [C_S00_AXI_ADDR_WIDTH-1:0] register_write_address;
    wire [31:0] register_write_data;
    wire [3:0] register_write_strobes;
    wire [C_S00_AXI_ADDR_WIDTH-1:0] register_read_address;
    reg [31:0] register_read_data;
    reg [31:0] input_channel_id_reg, fir_tap_count_reg, filter_kind_reg;
    reg [31:0] baseline_mean_bits_reg, baseline_stddev_bits_reg, threshold_k_bits_reg, power_window_us_reg, refractory_period_ms_reg;
    reg [31:0] coefficient_index_reg, coefficient_bits_reg;
    reg [31:0] trigger_request_count, power_sample_count, mean_square_reg;
    reg [31:0] sum_square_threshold_bits_reg, last_input_timestamp_reg;
    reg enabled, config_valid, session_closed, fault, write_reject, warmup;
    reg [5:0] power_window_fill_count;
    reg stream_active_d;
    reg core_start, core_busy, pending_valid, pending_enable;
    reg [31:0] pending_action, core_action, core_raw_sample, core_input_timestamp;
    reg core_enable_on_done;
    reg pending_threshold_k_valid;
    reg [31:0] pending_threshold_k_bits, core_threshold_k_bits;
    reg [31:0] pending_coefficient_bits, core_coefficient_bits;
    wire core_done, core_idle;
    wire [31:0] core_flags, core_mean_square, core_sum_square_threshold_bits;
    wire flags_valid, mean_square_valid, sum_square_threshold_valid;
    reg [31:0] captured_flags, captured_mean_square, captured_sum_square_threshold;
    wire busy = core_busy || pending_valid || pending_threshold_k_valid || core_start;
    wire session_live = enabled && local_compute_stream_active &&
        acquisition_30ksps && !session_closed;
    wire selected_amp = s_axis_algo_sample_tkeep == 8'hFF &&
        !s_axis_algo_sample_tlast && s_axis_algo_sample_tdata[31:30] == 2'b00 &&
        s_axis_algo_sample_tdata[24:16] == input_channel_id_reg[8:0];
    wire eos = s_axis_algo_sample_tvalid && s_axis_algo_sample_tready &&
        s_axis_algo_sample_tkeep == 8'h00 && s_axis_algo_sample_tlast;
    wire sample_accept = s_axis_algo_sample_tvalid && s_axis_algo_sample_tready &&
        selected_amp && session_live;
    wire command_write = register_write_enable &&
        register_write_address == ADDR_COMMAND && register_write_strobes[0];
    wire stopping = command_write && register_write_data[7:0] == COMMAND_STOP;
    wire scalar_config_valid = input_channel_id_reg <= 32'd511 &&
        fir_tap_count_reg >= 1 && fir_tap_count_reg <= 256 && filter_kind_reg <= 1 &&
        power_window_us_reg >= 1 && power_window_us_reg <= 10000 &&
        refractory_period_ms_reg <= 32'h5555_5555 &&
        baseline_mean_bits_reg[30:23] != 8'hFF && baseline_stddev_bits_reg[30:23] != 8'hFF &&
        threshold_k_bits_reg[30:23] != 8'hFF &&
        (!baseline_stddev_bits_reg[31] || baseline_stddev_bits_reg[30:0] == 0) &&
        (!threshold_k_bits_reg[31] || threshold_k_bits_reg[30:0] == 0);
    // Control-time conversions use integer counters; DSP-facing register words
    // retain the public IEEE-754 binary32 representation.
    wire [31:0] power_window_samples = (power_window_us_reg * 32'd3 + 32'd999) / 32'd1000;
    wire [31:0] refractory_output_samples = refractory_period_ms_reg * 32'd3;
    wire [31:0] done_flags = flags_valid ? core_flags : captured_flags;
    wire [31:0] done_mean_square = mean_square_valid ? core_mean_square : captured_mean_square;
    wire [31:0] done_sum_square_threshold_bits = sum_square_threshold_valid ?
        core_sum_square_threshold_bits : captured_sum_square_threshold;

    // Ignore/drain unselected channels, AUX and TTL even while the selected
    // detector is busy. Backpressure only the next selected sample.
    assign s_axis_algo_sample_tready = !s00_axi_aresetn || !selected_amp ||
        !session_live || (!busy && core_idle && !command_write);

    function automatic [31:0] apply_write_strobes;
        input [31:0] old_value, new_value;
        input [3:0] strobes;
        integer i;
        begin
            apply_write_strobes = old_value;
            for (i = 0; i < 4; i = i + 1)
                if (strobes[i]) apply_write_strobes[i*8 +: 8] = new_value[i*8 +: 8];
        end
    endfunction

    initial begin
        if (C_S00_AXI_DATA_WIDTH != 32 || C_S00_AXI_ADDR_WIDTH < 12)
            $fatal(1, "ripple_detector requires 32-bit AXI-Lite and at least 12 address bits");
    end

    axil_register_slave #(.DATA_WIDTH(32), .ADDR_WIDTH(C_S00_AXI_ADDR_WIDTH)) transport (
        .clk(s00_axi_aclk), .resetn(s00_axi_aresetn),
        .s_axil_awaddr(s00_axi_awaddr), .s_axil_awprot(s00_axi_awprot),
        .s_axil_awvalid(s00_axi_awvalid), .s_axil_awready(s00_axi_awready),
        .s_axil_wdata(s00_axi_wdata), .s_axil_wstrb(s00_axi_wstrb),
        .s_axil_wvalid(s00_axi_wvalid), .s_axil_wready(s00_axi_wready),
        .s_axil_bresp(s00_axi_bresp), .s_axil_bvalid(s00_axi_bvalid), .s_axil_bready(s00_axi_bready),
        .s_axil_araddr(s00_axi_araddr), .s_axil_arprot(s00_axi_arprot),
        .s_axil_arvalid(s00_axi_arvalid), .s_axil_arready(s00_axi_arready),
        .s_axil_rdata(s00_axi_rdata), .s_axil_rresp(s00_axi_rresp),
        .s_axil_rvalid(s00_axi_rvalid), .s_axil_rready(s00_axi_rready),
        .reg_wr_en(register_write_enable),
        .reg_wr_addr(register_write_address), .reg_wr_data(register_write_data),
        .reg_wr_strb(register_write_strobes),
        .reg_rd_addr(register_read_address), .reg_rd_data(register_read_data)
    );

    nclp_ripple_hls core (
        .ap_clk(s00_axi_aclk), .ap_rst_n(s00_axi_aresetn),
        .ap_start(core_start), .ap_done(core_done), .ap_idle(core_idle), .ap_ready(),
        .action(core_action), .raw_sample(core_raw_sample),
        .input_timestamp(core_input_timestamp),
        .fir_tap_count(fir_tap_count_reg), .filter_kind(filter_kind_reg),
        .power_window_samples(power_window_samples),
        .refractory_output_samples(refractory_output_samples),
        .baseline_mean_bits(baseline_mean_bits_reg),
        .baseline_stddev_bits(baseline_stddev_bits_reg),
        .threshold_k_bits(core_threshold_k_bits),
        .coefficient_index(core_action == HLS_ACTION_WRITE_IIR_COEFFICIENT ?
            coefficient_index_reg - 32'd256 : coefficient_index_reg),
        .coefficient_bits(core_coefficient_bits),
        .result_flags(core_flags), .result_flags_ap_vld(flags_valid),
        .mean_square(core_mean_square), .mean_square_ap_vld(mean_square_valid),
        .sum_square_threshold_bits(core_sum_square_threshold_bits),
        .sum_square_threshold_bits_ap_vld(sum_square_threshold_valid)
    );

    always @(*) begin
        register_read_data = 0;
        case (register_read_address)
            ADDR_BLOCK_ID: register_read_data = BLOCK_ID_VALUE;
            ADDR_ABI_VERSION: register_read_data = ABI_VERSION_VALUE;
            ADDR_CAPABILITIES: register_read_data = CAPABILITIES_VALUE;
            ADDR_LIMITS: register_read_data = LIMITS_VALUE;
            ADDR_STATUS: register_read_data = {23'd0, warmup, write_reject, fault,
                session_closed, (enabled && !local_compute_stream_active), acquisition_30ksps,
                config_valid, busy, enabled};
            ADDR_INPUT_CHANNEL_ID: register_read_data = input_channel_id_reg;
            ADDR_FILTER_KIND: register_read_data = filter_kind_reg;
            ADDR_FIR_TAP_COUNT: register_read_data = fir_tap_count_reg;
            ADDR_POWER_WINDOW_US: register_read_data = power_window_us_reg;
            ADDR_REFRACTORY_PERIOD_MS: register_read_data = refractory_period_ms_reg;
            ADDR_BASELINE_MEAN_BITS: register_read_data = baseline_mean_bits_reg;
            ADDR_BASELINE_STDDEV_BITS: register_read_data = baseline_stddev_bits_reg;
            ADDR_THRESHOLD_K_BITS: register_read_data = threshold_k_bits_reg;
            ADDR_COEFFICIENT_INDEX: register_read_data = coefficient_index_reg;
            ADDR_COEFFICIENT_BITS: register_read_data = coefficient_bits_reg;
            ADDR_TRIGGER_REQUEST_COUNT: register_read_data = trigger_request_count;
            ADDR_POWER_SAMPLE_COUNT: register_read_data = power_sample_count;
            ADDR_MEAN_SQUARE: register_read_data = mean_square_reg;
            ADDR_SUM_SQUARE_THRESHOLD_BITS: register_read_data = sum_square_threshold_bits_reg;
            ADDR_LAST_INPUT_TIMESTAMP: register_read_data = last_input_timestamp_reg;
            default: register_read_data = 0;
        endcase
    end

    always @(posedge s00_axi_aclk) begin
        if (!s00_axi_aresetn) begin
            input_channel_id_reg <= 0; fir_tap_count_reg <= 129; filter_kind_reg <= 0;
            baseline_mean_bits_reg <= 32'h0000_0000; baseline_stddev_bits_reg <= 32'h3F80_0000; threshold_k_bits_reg <= 32'h4080_0000;
            power_window_us_reg <= 4000; refractory_period_ms_reg <= 1000;
            coefficient_index_reg <= 0; coefficient_bits_reg <= 0;
            trigger_request_count <= 0; power_sample_count <= 0; mean_square_reg <= 0;
            sum_square_threshold_bits_reg <= 0; last_input_timestamp_reg <= 0;
            enabled <= 0; config_valid <= 0; session_closed <= 0;
            fault <= 0; write_reject <= 0; warmup <= 1;
            power_window_fill_count <= 0;
            stim_trigger <= 0; stream_active_d <= 0;
            core_start <= 0; core_busy <= 0; pending_valid <= 1;
            pending_action <= HLS_ACTION_APPLY_CONFIG_AND_RESET;
            pending_enable <= 0; core_enable_on_done <= 0;
            pending_threshold_k_valid <= 0; pending_threshold_k_bits <= 32'h4080_0000; core_threshold_k_bits <= 32'h4080_0000;
            pending_coefficient_bits <= 0; core_coefficient_bits <= 0;
            core_action <= HLS_ACTION_APPLY_CONFIG_AND_RESET;
            core_raw_sample <= 0; core_input_timestamp <= 0;
            captured_flags <= 0; captured_mean_square <= 0; captured_sum_square_threshold <= 0;
        end else begin
            stim_trigger <= 0;
            core_start <= 0;
            stream_active_d <= local_compute_stream_active;
            if (flags_valid) captured_flags <= core_flags;
            if (mean_square_valid) captured_mean_square <= core_mean_square;
            if (sum_square_threshold_valid) captured_sum_square_threshold <= core_sum_square_threshold_bits;

            if (core_busy && core_done) begin
                core_busy <= 0;
                if (core_action == HLS_ACTION_APPLY_CONFIG_AND_RESET ||
                    core_action == HLS_ACTION_VALIDATE_CONFIG ||
                    core_action == HLS_ACTION_RESTORE_DEFAULT_COEFFICIENTS) begin
                    config_valid <= done_flags[0] && scalar_config_valid;
                    sum_square_threshold_bits_reg <= done_sum_square_threshold_bits;
                    if (core_enable_on_done && !pending_valid && !stopping && !eos && !session_closed &&
                        acquisition_30ksps && done_flags[0] && scalar_config_valid)
                        enabled <= 1;
                    if (!done_flags[0]) fault <= 1;
                end
                if (core_action == HLS_ACTION_UPDATE_THRESHOLD_K && enabled &&
                    !pending_valid && !stopping && !eos && acquisition_30ksps) begin
                    if (done_flags[0] && !done_flags[5]) begin
                        threshold_k_bits_reg <= core_threshold_k_bits;
                        sum_square_threshold_bits_reg <= done_sum_square_threshold_bits;
                    end else write_reject <= 1;
                end
                if (core_action == HLS_ACTION_WRITE_FIR_COEFFICIENT ||
                    core_action == HLS_ACTION_WRITE_IIR_COEFFICIENT) begin
                    if (!done_flags[4] && !done_flags[5])
                        coefficient_bits_reg <= core_coefficient_bits;
                    else
                        write_reject <= 1;
                end
                if (core_action == HLS_ACTION_PROCESS_SAMPLE && session_live && !eos && !stopping) begin
                    last_input_timestamp_reg <= core_input_timestamp;
                    if (done_flags[3]) begin warmup <= 1; power_window_fill_count <= 0; end
                    if (done_flags[1]) begin
                        if (power_window_fill_count < power_window_samples) power_window_fill_count <= power_window_fill_count + 1'b1;
                        warmup <= (power_window_fill_count + 1'b1 < power_window_samples);
                        mean_square_reg <= done_mean_square;
                        if (power_sample_count != 32'hFFFF_FFFF) power_sample_count <= power_sample_count + 1'b1;
                    end
                    if (done_flags[2] && !done_flags[4]) begin
                        stim_trigger <= 1;
                        if (trigger_request_count != 32'hFFFF_FFFF) trigger_request_count <= trigger_request_count + 1'b1;
                    end
                    if (done_flags[4]) begin
                        fault <= 1; enabled <= 0; config_valid <= 0;
                        warmup <= 1; power_window_fill_count <= 0;
                        pending_valid <= 1;
                        pending_action <= HLS_ACTION_APPLY_CONFIG_AND_RESET;
                        pending_enable <= 0;
                    end
                end
            end

            if (!core_busy && core_idle && !core_start && !command_write) begin
                if (pending_valid) begin
                    core_action <= pending_action;
                    core_threshold_k_bits <= threshold_k_bits_reg;
                    core_coefficient_bits <= pending_coefficient_bits;
                    core_enable_on_done <= pending_enable;
                    pending_valid <= 0; core_start <= 1; core_busy <= 1;
                end else if (pending_threshold_k_valid) begin
                    core_action <= HLS_ACTION_UPDATE_THRESHOLD_K;
                    core_threshold_k_bits <= pending_threshold_k_bits;
                    core_enable_on_done <= 0; pending_threshold_k_valid <= 0;
                    core_start <= 1; core_busy <= 1;
                end else if (sample_accept) begin
                    core_action <= HLS_ACTION_PROCESS_SAMPLE;
                    core_raw_sample <= {16'd0, s_axis_algo_sample_tdata[15:0]};
                    core_threshold_k_bits <= threshold_k_bits_reg;
                    core_input_timestamp <= s_axis_algo_sample_tdata[63:32];
                    core_enable_on_done <= 0; core_start <= 1; core_busy <= 1;
                end
            end

            // EOS is accepted even during an HLS call. Close immediately so an
            // in-flight sample cannot emit a stale trigger; reset after it ends.
            if (eos && (enabled || pending_enable || core_enable_on_done)) begin
                session_closed <= 1; warmup <= 1;
                power_window_fill_count <= 0; core_enable_on_done <= 0;
                pending_valid <= 1;
                pending_action <= HLS_ACTION_APPLY_CONFIG_AND_RESET;
                pending_enable <= 0;
                pending_threshold_k_valid <= 0;
            end
            if (stream_active_d && !local_compute_stream_active) begin
                session_closed <= 0; warmup <= 1;
                power_window_fill_count <= 0;
                trigger_request_count <= 0; power_sample_count <= 0; mean_square_reg <= 0; last_input_timestamp_reg <= 0;
                pending_valid <= 1;
                pending_action <= HLS_ACTION_APPLY_CONFIG_AND_RESET;
                pending_enable <= 0;
                pending_threshold_k_valid <= 0;
            end
            if (enabled && !acquisition_30ksps) begin
                enabled <= 0; fault <= 1; warmup <= 1;
                power_window_fill_count <= 0;
                pending_valid <= 1;
                pending_action <= HLS_ACTION_APPLY_CONFIG_AND_RESET;
                pending_enable <= 0;
                pending_threshold_k_valid <= 0;
            end

            if (command_write) begin
                case (register_write_data[7:0])
                    COMMAND_STOP: begin
                        enabled <= 0; session_closed <= 0; warmup <= 1;
                        power_window_fill_count <= 0;
                        pending_valid <= 1;
                        pending_action <= HLS_ACTION_APPLY_CONFIG_AND_RESET;
                        pending_enable <= 0;
                        core_enable_on_done <= 0;
                        pending_threshold_k_valid <= 0;
                        trigger_request_count <= 0; power_sample_count <= 0; mean_square_reg <= 0; last_input_timestamp_reg <= 0;
                    end
                    COMMAND_START: begin
                        if (!busy && !enabled && acquisition_30ksps && scalar_config_valid && !eos) begin
                            pending_valid <= 1;
                            pending_action <= HLS_ACTION_APPLY_CONFIG_AND_RESET;
                            pending_enable <= 1;
                            config_valid <= 0; session_closed <= 0; fault <= 0; write_reject <= 0; warmup <= 1;
                            power_window_fill_count <= 0;
                            trigger_request_count <= 0; power_sample_count <= 0; mean_square_reg <= 0; last_input_timestamp_reg <= 0;
                        end else write_reject <= 1;
                    end
                    COMMAND_VALIDATE: begin
                        if (!busy && !enabled && scalar_config_valid) begin
                            pending_valid <= 1;
                            pending_action <= HLS_ACTION_VALIDATE_CONFIG;
                            pending_enable <= 0;
                            config_valid <= 0; fault <= 0; write_reject <= 0;
                        end else write_reject <= 1;
                    end
                    COMMAND_RESTORE_DEFAULTS: begin
                        if (!busy && !enabled && scalar_config_valid) begin
                            pending_valid <= 1;
                            pending_action <= HLS_ACTION_RESTORE_DEFAULT_COEFFICIENTS;
                            pending_enable <= 0;
                            config_valid <= 0; fault <= 0; write_reject <= 0; warmup <= 1;
                            power_window_fill_count <= 0;
                        end else write_reject <= 1;
                    end
                    default: write_reject <= 1;
                endcase
            end else if (register_write_enable && |register_write_strobes) begin
                // A live K update is serialized between HLS transactions. Its
                // register value commits only after the core accepts it; inputs
                // of an already-running sample remain unchanged.
                if (enabled && register_write_address == ADDR_THRESHOLD_K_BITS) begin
                    if (!pending_threshold_k_valid &&
                        !(core_busy && core_action == HLS_ACTION_UPDATE_THRESHOLD_K) &&
                        !pending_valid && !eos && acquisition_30ksps &&
                        register_write_strobes == 4'hF && register_write_data[30:23] != 8'hFF &&
                        (!register_write_data[31] || register_write_data[30:0] == 0)) begin
                        pending_threshold_k_bits <= register_write_data; pending_threshold_k_valid <= 1;
                        write_reject <= 0;
                    end else write_reject <= 1;
                end else if (!enabled && !busy) begin
                    case (register_write_address)
                        ADDR_INPUT_CHANNEL_ID: begin
                            input_channel_id_reg <= apply_write_strobes(input_channel_id_reg, register_write_data, register_write_strobes);
                            config_valid <= 0;
                        end
                        ADDR_FIR_TAP_COUNT: begin
                            fir_tap_count_reg <= apply_write_strobes(fir_tap_count_reg, register_write_data, register_write_strobes);
                            config_valid <= 0;
                        end
                        ADDR_FILTER_KIND: begin
                            filter_kind_reg <= apply_write_strobes(filter_kind_reg, register_write_data, register_write_strobes);
                            config_valid <= 0;
                        end
                        ADDR_BASELINE_MEAN_BITS: begin baseline_mean_bits_reg <= apply_write_strobes(baseline_mean_bits_reg, register_write_data, register_write_strobes); config_valid <= 0; end
                        ADDR_BASELINE_STDDEV_BITS: begin baseline_stddev_bits_reg <= apply_write_strobes(baseline_stddev_bits_reg, register_write_data, register_write_strobes); config_valid <= 0; end
                        ADDR_THRESHOLD_K_BITS: begin threshold_k_bits_reg <= apply_write_strobes(threshold_k_bits_reg, register_write_data, register_write_strobes); config_valid <= 0; end
                        ADDR_POWER_WINDOW_US: begin power_window_us_reg <= apply_write_strobes(power_window_us_reg, register_write_data, register_write_strobes); config_valid <= 0; end
                        ADDR_REFRACTORY_PERIOD_MS: begin refractory_period_ms_reg <= apply_write_strobes(refractory_period_ms_reg, register_write_data, register_write_strobes); config_valid <= 0; end
                        ADDR_COEFFICIENT_INDEX: coefficient_index_reg <= apply_write_strobes(coefficient_index_reg, register_write_data, register_write_strobes);
                        ADDR_COEFFICIENT_BITS: begin
                            if (coefficient_index_reg < 266 && register_write_strobes == 4'hF && register_write_data[30:23] != 8'hFF) begin
                                pending_coefficient_bits <= register_write_data;
                                pending_valid <= 1;
                                pending_action <= coefficient_index_reg < 256 ?
                                    HLS_ACTION_WRITE_FIR_COEFFICIENT :
                                    HLS_ACTION_WRITE_IIR_COEFFICIENT;
                                pending_enable <= 0; config_valid <= 0;
                            end else write_reject <= 1;
                        end
                        default: write_reject <= 1;
                    endcase
                end else write_reject <= 1;
            end
        end
    end
endmodule
`default_nettype wire
