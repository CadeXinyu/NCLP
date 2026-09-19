`timescale 1ns / 1ps
`default_nettype none

module stim_controller #(
    parameter integer C_S00_AXI_DATA_WIDTH = 32,
    parameter integer C_S00_AXI_ADDR_WIDTH = 13,
    parameter integer RAM_DEPTH = 1024,
    parameter integer RAM_ADDR_WIDTH = (RAM_DEPTH > 1) ? $clog2(RAM_DEPTH) : 1,
    parameter integer DAC_CLKS_PER_HALF_BIT = 2,
    parameter integer DAC_CS_SETUP_CLKS = 1,
    parameter integer DAC_CS_HOLD_CLKS = 1,
    parameter integer DAC_CS_HIGH_CLKS = 4,
    parameter integer SAFE_OFF_RELEASE_DEBOUNCE_CYCLES = 1_000_000,
    parameter integer SIM_CLOCK_BYPASS = 0
)(
    input  wire                                  stim_trigger,
    input  wire                                  force_safe_off_button_n,
    output wire                                  stimulus_level,
    output wire                                  trigger_monitor_level,
    output wire                                  configuration_locked,
    // The mask is a stable configuration payload; only the single-bit active
    // level is an event crossing into the Intan SPI clock domain.
    output reg                                   intan_marker_active,
    output wire [13:0]                           intan_marker_mask,
    output wire                                  stim_fault_irq,
    output wire                                  dac_sync_n,
    output wire                                  dac_sclk,
    output wire                                  dac_sdin,
    input  wire                                  clkgen_ref_clk,
    input  wire                                  sim_dac_clk,
    input  wire                                  sim_dac_clock_locked,
    input  wire                                  sim_dac_clock_ready,
    input  wire                                  sim_dac_clock_program_busy,
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

    localparam [1:0] ACTION_MODE_OFF = 2'd0;
    localparam [1:0] ACTION_MODE_TTL = 2'd1;
    localparam [1:0] ACTION_MODE_DAC = 2'd2;
    // The implemented clock is statically timed at the 40 MHz default.  Runtime
    // programming may select slower legal tuples, but never a faster one.
    localparam integer DAC_ENGINE_MAX_MHZ = 40;

    wire safe_off_active;
    wire safe_off_event;
    stim_safe_off_button #(
        .RELEASE_DEBOUNCE_CYCLES(SAFE_OFF_RELEASE_DEBOUNCE_CYCLES)
    ) safe_off_button_i (
        .clk(s00_axi_aclk),
        .resetn(s00_axi_aresetn),
        .button_n_async(force_safe_off_button_n),
        .safe_off_active(safe_off_active),
        .safe_off_event(safe_off_event)
    );

    // Runtime-programmable DAC-engine PLL. Default: 140 MHz * 20/(2*35)
    // = 40 MHz. The playback shifter divides that by four for 10 MHz SCLK.
    wire dac_engine_clk;
    wire dac_clock_locked;
    wire dac_clock_ready;
    wire dac_clock_program_busy;
    wire dac_clock_error;
    wire [2:0] dac_clock_error_code;
    wire dac_clock_domain_reset_request;
    wire dac_domain_reset;
    wire dac_clock_program_pulse;
    wire [7:0] shadow_dac_clock_o;
    wire [3:0] shadow_dac_clock_d;
    wire [6:0] shadow_dac_clock_m;

    generate
        if (SIM_CLOCK_BYPASS != 0) begin : g_sim_clock
            assign dac_engine_clk = sim_dac_clk;
            assign dac_clock_locked = sim_dac_clock_locked;
            assign dac_clock_ready = sim_dac_clock_ready;
            assign dac_clock_program_busy = sim_dac_clock_program_busy;
            assign dac_clock_error = 1'b0;
            assign dac_clock_error_code = 3'd0;
            assign dac_clock_domain_reset_request = !s00_axi_aresetn ||
                                                    !sim_dac_clock_locked ||
                                                    !sim_dac_clock_ready ||
                                                    sim_dac_clock_program_busy;
        end else begin : g_real_clock
            plle4_clock #(
                .DEFAULT_O(8'd35),
                .DEFAULT_D(4'd2),
                .DEFAULT_M(7'd20)
            ) dac_clkgen (
                .config_clk(s00_axi_aclk),
                .source_clk(clkgen_ref_clk),
                .reset(!s00_axi_aresetn),
                .request(dac_clock_program_pulse),
                .config_valid(dac_clock_config_valid),
                .config_o(shadow_dac_clock_o),
                .config_d(shadow_dac_clock_d),
                .config_m(shadow_dac_clock_m),
                .ready(dac_clock_ready),
                .busy(dac_clock_program_busy),
                .done_pulse(),
                .error(dac_clock_error),
                .error_code(dac_clock_error_code),
                .locked(dac_clock_locked),
                .domain_reset_request(dac_clock_domain_reset_request),
                .domain_reset_ack(dac_domain_reset),
                .clock_out(dac_engine_clk)
            );
        end
    endgenerate

    // The manager requests reset before stopping or reprogramming the PLL. This
    // one XPM crossing makes assertion asynchronous to the DAC clock and release
    // synchronous to the restarted clock. The actual destination reset is fed
    // back so the manager cannot gate/restart the clock ahead of the DAC domain.
    xpm_cdc_async_rst #(
        .DEST_SYNC_FF(2),
        .INIT_SYNC_FF(0),
        .RST_ACTIVE_HIGH(1)
    ) dac_domain_reset_sync (
        .src_arst(dac_clock_domain_reset_request),
        .dest_clk(dac_engine_clk),
        .dest_arst(dac_domain_reset)
    );
    wire dac_domain_resetn = !dac_domain_reset;

    // Shadow configuration and AXI/RAM transport signals.
    wire arm_pulse, software_disarm_pulse, software_trigger_pulse, stop_pulse;
    // Only PS software or the physical safe-off input may Disarm stimulation.
    // The optional SFP endpoint supplies a trigger pulse, never DAC control.
    wire disarm_pulse = software_disarm_pulse || safe_off_event;
    wire clear_diagnostics_pulse, prime_dac_pulse, dac_prime_invalidate_pulse;
    wire [1:0] shadow_action_mode;
    wire shadow_external_trigger_enable;
    wire [31:0] shadow_ttl_pulse_width_axi_cycles;
    wire [13:0] shadow_intan_stim_marker_mask;
    wire shadow_dac_a_enable, shadow_dac_b_enable;
    wire shadow_dac_continuous;
    wire [31:0] shadow_dac_update_period_clocks;
    wire [RAM_ADDR_WIDTH-1:0] shadow_dac_start_index;
    wire [RAM_ADDR_WIDTH-1:0] shadow_dac_loop_index;
    wire [RAM_ADDR_WIDTH-1:0] shadow_dac_end_index;
    wire [31:0] shadow_dac_finite_update_count;
    wire [RAM_ADDR_WIDTH-1:0] ram_bus_addr;
    wire ram_bus_write_enable;
    wire [3:0] ram_bus_write_strobes;
    wire [31:0] ram_bus_write_data;
    wire ram_bus_read_enable;
    wire [31:0] ram_bus_read_data;
    wire [RAM_ADDR_WIDTH-1:0] dac_ram_read_addr;
    wire [31:0] dac_ram_read_data;

    reg [1:0] active_action_mode;
    reg active_external_trigger_enable;
    reg [31:0] active_ttl_pulse_width_axi_cycles;
    reg [13:0] active_intan_stim_marker_mask;
    reg active_dac_a_enable, active_dac_b_enable;
    reg active_dac_continuous;
    reg [31:0] active_dac_update_period_clocks;
    reg [RAM_ADDR_WIDTH-1:0] active_dac_start_index;
    reg [RAM_ADDR_WIDTH-1:0] active_dac_loop_index;
    reg [RAM_ADDR_WIDTH-1:0] active_dac_end_index;
    reg [31:0] active_dac_finite_update_count;

    stim_waveform_ram #(.RAM_DEPTH(RAM_DEPTH), .RAM_ADDR_WIDTH(RAM_ADDR_WIDTH))
    waveform_ram (
        .bus_clk(s00_axi_aclk), .bus_addr(ram_bus_addr),
        .bus_write_enable(ram_bus_write_enable),
        .bus_write_strobes(ram_bus_write_strobes),
        .bus_write_data(ram_bus_write_data),
        .bus_read_enable(ram_bus_read_enable), .bus_read_data(ram_bus_read_data),
        .playback_clk(dac_engine_clk), .playback_addr(dac_ram_read_addr),
        .playback_data(dac_ram_read_data));

    // Validate the stimulus preset independently of physical TTL routing.
    wire dac_config_valid = (shadow_dac_a_enable || shadow_dac_b_enable) &&
                            (shadow_dac_update_period_clocks != 32'd0) &&
                            (shadow_dac_start_index <= shadow_dac_loop_index) &&
                            (shadow_dac_loop_index <= shadow_dac_end_index) &&
                            (shadow_dac_continuous ||
                             (shadow_dac_finite_update_count != 32'd0));
    wire ttl_config_valid = shadow_ttl_pulse_width_axi_cycles != 32'd0;
    wire [31:0] clock_vco_numerator = 32'd140 * shadow_dac_clock_m;
    wire [31:0] clock_vco_minimum = 32'd750 * shadow_dac_clock_d;
    wire [31:0] clock_vco_maximum = 32'd1500 * shadow_dac_clock_d;
    wire [31:0] clock_engine_maximum =
        DAC_ENGINE_MAX_MHZ * shadow_dac_clock_d * shadow_dac_clock_o;
    wire dac_clock_config_valid = (shadow_dac_clock_o >= 8'd1) &&
                              (shadow_dac_clock_o <= 8'd128) &&
                              (shadow_dac_clock_d >= 4'd1) &&
                              (shadow_dac_clock_d <= 4'd2) &&
                              (shadow_dac_clock_m >= 7'd2) &&
                              (shadow_dac_clock_m <= 7'd21) &&
                              (clock_vco_numerator >= clock_vco_minimum) &&
                              (clock_vco_numerator <= clock_vco_maximum) &&
                              (clock_vco_numerator <= clock_engine_maximum);
    wire configuration_valid = (shadow_action_mode == ACTION_MODE_OFF) ||
                               ((shadow_action_mode == ACTION_MODE_TTL) && ttl_config_valid) ||
                               ((shadow_action_mode == ACTION_MODE_DAC) &&
                                dac_config_valid && dac_clock_config_valid);

    // DAC config snapshot crossing.  Payload gets a one-bus-cycle head start;
    // the request has one extra destination synchronizer stage.
    localparam integer DAC_CFG_WIDTH = 67 + (3 * RAM_ADDR_WIDTH);
    wire [DAC_CFG_WIDTH-1:0] active_dac_cfg_bundle = {
        active_dac_finite_update_count,
        active_dac_end_index, active_dac_loop_index, active_dac_start_index,
        active_dac_update_period_clocks,
        active_dac_continuous, active_dac_b_enable, active_dac_a_enable};
    reg arm_request_toggle_bus, arm_launch_pending_bus, arm_request_inflight_bus;
    reg trigger_request_toggle_bus, trigger_request_inflight_bus;
    reg prime_request_toggle_bus, prime_launch_pending_bus;
    reg dac_prime_outstanding_bus;
    reg dac_launch_pending_bus;
    reg dac_stop_pending_bus;
    reg dac_stop_outstanding_bus;
    reg disarm_pending_bus;
    reg stop_request_toggle_bus;
    reg clear_request_toggle_bus;
    reg dac_abort_sticky_bus;
    reg dac_primed_bus, dac_prime_busy_bus, dac_prime_completion_pending_bus;
    reg dac_prime_cancel_pending_bus;
    reg dac_prime_done_sticky_bus, dac_prime_fault_sticky_bus;
    wire [DAC_CFG_WIDTH-1:0] dac_config_bundle;
    wire dac_trigger_pulse, dac_prime_pulse, dac_stop_pulse;
    wire dac_clear_diagnostics_pulse;
    wire arm_ack_toggle_bus, trigger_ack_toggle_bus;
    wire dac_cfg_a_enable = dac_config_bundle[0];
    wire dac_cfg_b_enable = dac_config_bundle[1];
    wire dac_cfg_continuous = dac_config_bundle[2];
    wire [31:0] dac_cfg_update_period_clocks = dac_config_bundle[34:3];
    wire [RAM_ADDR_WIDTH-1:0] dac_cfg_start_index = dac_config_bundle[35 +: RAM_ADDR_WIDTH];
    wire [RAM_ADDR_WIDTH-1:0] dac_cfg_loop_index = dac_config_bundle[35+RAM_ADDR_WIDTH +: RAM_ADDR_WIDTH];
    wire [RAM_ADDR_WIDTH-1:0] dac_cfg_end_index = dac_config_bundle[35+2*RAM_ADDR_WIDTH +: RAM_ADDR_WIDTH];
    wire [31:0] dac_cfg_finite_update_count = dac_config_bundle[35+3*RAM_ADDR_WIDTH +: 32];

    wire dac_transaction_active_dac, dac_busy_dac, dac_done_pulse_dac;
    wire dac_prime_done_pulse_dac, dac_prime_fault_pulse_dac;
    wire dac_zero_done_pulse_dac, dac_stop_done_pulse_dac;
    wire [RAM_ADDR_WIDTH-1:0] dac_current_waveform_index_dac;
    wire [31:0] dac_completed_update_count_dac;
    wire dac_unserved_trigger_sticky_dac, dac_invalid_config_sticky_dac;
    wire dac_timing_late_sticky_dac;

    mcp4922_playback_engine #(
        .RAM_DEPTH(RAM_DEPTH), .RAM_ADDR_WIDTH(RAM_ADDR_WIDTH),
        .CLKS_PER_HALF_BIT(DAC_CLKS_PER_HALF_BIT),
        .CS_SETUP_CLKS(DAC_CS_SETUP_CLKS),
        .CS_HOLD_CLKS(DAC_CS_HOLD_CLKS),
        .CS_HIGH_CLKS(DAC_CS_HIGH_CLKS), .RAM_READ_WAIT_CYCLES(1))
    dac_playback_engine (
        .clk(dac_engine_clk), .resetn(dac_domain_resetn),
        .prime_pulse(dac_prime_pulse),
        .trigger_pulse(dac_trigger_pulse), .stop_pulse(dac_stop_pulse),
        .clear_diagnostics(dac_clear_diagnostics_pulse),
        .cfg_a_enable(dac_cfg_a_enable), .cfg_b_enable(dac_cfg_b_enable),
        .cfg_continuous(dac_cfg_continuous),
        .cfg_update_period_clocks(dac_cfg_update_period_clocks),
        .cfg_start_index(dac_cfg_start_index), .cfg_loop_index(dac_cfg_loop_index),
        .cfg_end_index(dac_cfg_end_index),
        .cfg_finite_update_count(dac_cfg_finite_update_count),
        .ram_rd_addr(dac_ram_read_addr), .ram_rd_data(dac_ram_read_data),
        .transaction_active(dac_transaction_active_dac), .busy(dac_busy_dac),
        .current_waveform_index(dac_current_waveform_index_dac),
        .completed_update_count(dac_completed_update_count_dac),
        .done_pulse(dac_done_pulse_dac),
        .prime_done_pulse(dac_prime_done_pulse_dac),
        .prime_fault_pulse(dac_prime_fault_pulse_dac),
        .zero_done_pulse(dac_zero_done_pulse_dac),
        .stop_done_pulse(dac_stop_done_pulse_dac),
        .unserved_trigger_sticky(dac_unserved_trigger_sticky_dac),
        .invalid_config_sticky(dac_invalid_config_sticky_dac),
        .timing_late_sticky(dac_timing_late_sticky_dac),
        .dac_sync_n(dac_sync_n), .dac_sclk(dac_sclk), .dac_sdin(dac_sdin));

    localparam integer DAC_STATUS_WIDTH = 32 + RAM_ADDR_WIDTH;
    wire [DAC_STATUS_WIDTH-1:0] dac_status_bundle;
    wire dac_domain_available_bus, dac_clock_locked_bus;
    wire dac_transaction_active_bus, dac_busy_bus;
    wire dac_done_event_bus, dac_prime_done_event_bus, dac_prime_fault_event_bus;
    wire dac_zero_done_event_bus, dac_stop_done_event_bus;
    wire dac_unserved_event_bus, dac_invalid_event_bus, dac_timing_late_event_bus;
    wire [RAM_ADDR_WIDTH-1:0] dac_current_waveform_index_stable =
        dac_status_bundle[RAM_ADDR_WIDTH-1:0];
    wire [31:0] dac_completed_update_count_stable =
        dac_status_bundle[RAM_ADDR_WIDTH +: 32];

    stim_dac_cdc #(
        .CONFIG_WIDTH(DAC_CFG_WIDTH), .STATUS_WIDTH(DAC_STATUS_WIDTH)
    ) dac_cdc (
        .bus_clk(s00_axi_aclk), .bus_resetn(s00_axi_aresetn),
        .dac_clk(dac_engine_clk), .dac_resetn(dac_domain_resetn),
        .clock_reset_request_bus(dac_clock_domain_reset_request),
        .manager_clock_locked(dac_clock_locked), .clock_ready_bus(dac_clock_ready),
        .clock_program_busy_bus(dac_clock_program_busy),
        .status_capture_enable_bus(!dac_abort_sticky_bus),
        .config_bus(active_dac_cfg_bundle), .config_dac(dac_config_bundle),
        .arm_request_toggle_bus(arm_request_toggle_bus),
        .trigger_request_toggle_bus(trigger_request_toggle_bus),
        .prime_request_toggle_bus(prime_request_toggle_bus),
        .stop_request_toggle_bus(stop_request_toggle_bus),
        .clear_request_toggle_bus(clear_request_toggle_bus),
        .arm_ack_toggle_bus(arm_ack_toggle_bus),
        .trigger_ack_toggle_bus(trigger_ack_toggle_bus),
        .trigger_pulse_dac(dac_trigger_pulse), .prime_pulse_dac(dac_prime_pulse),
        .stop_pulse_dac(dac_stop_pulse),
        .clear_diagnostics_pulse_dac(dac_clear_diagnostics_pulse),
        .transaction_active_dac(dac_transaction_active_dac), .busy_dac(dac_busy_dac),
        .done_pulse_dac(dac_done_pulse_dac),
        .prime_done_pulse_dac(dac_prime_done_pulse_dac),
        .prime_fault_pulse_dac(dac_prime_fault_pulse_dac),
        .zero_done_pulse_dac(dac_zero_done_pulse_dac),
        .stop_done_pulse_dac(dac_stop_done_pulse_dac),
        .unserved_trigger_sticky_dac(dac_unserved_trigger_sticky_dac),
        .invalid_config_sticky_dac(dac_invalid_config_sticky_dac),
        .timing_late_sticky_dac(dac_timing_late_sticky_dac),
        .status_dac({dac_completed_update_count_dac, dac_current_waveform_index_dac}),
        .status_bus(dac_status_bundle), .domain_available_bus(dac_domain_available_bus),
        .clock_locked_bus(dac_clock_locked_bus),
        .transaction_active_bus(dac_transaction_active_bus), .busy_bus(dac_busy_bus),
        .done_event_bus(dac_done_event_bus), .prime_done_event_bus(dac_prime_done_event_bus),
        .prime_fault_event_bus(dac_prime_fault_event_bus),
        .zero_done_event_bus(dac_zero_done_event_bus), .stop_done_event_bus(dac_stop_done_event_bus),
        .unserved_event_bus(dac_unserved_event_bus), .invalid_event_bus(dac_invalid_event_bus),
        .timing_late_event_bus(dac_timing_late_event_bus)
    );

    wire dac_terminal_event_bus = dac_done_event_bus ||
                                  dac_unserved_event_bus ||
                                  dac_invalid_event_bus;

    // Bus-domain arm, trigger arbitration, and exact 100 MHz TTL duration.
    reg armed_bus, ttl_active_bus, stim_trigger_d;
    // Trigger edges only start actions. Falling edges never stop playback.
    reg arm_cancel_pending_bus, dac_run_outstanding_bus;
    // Startup, STOP_ACTIVITY, a safety-button press, or a lost DAC-clock epoch
    // creates one mandatory-zero obligation. It survives button release and
    // clock loss until the DAC domain proves both physical channels reached zero.
    reg mandatory_zero_required_bus;
    // Keep the cause of an in-flight PRIME cancellation after the debounced
    // button is released, so its eventual terminal event is not misreported as
    // a software/configuration fault.
    reg safe_off_prime_cancel_bus;
    reg [31:0] ttl_cycles_remaining;
    reg [31:0] accepted_trigger_count, unserved_trigger_count;
    wire controller_busy_bus = ttl_active_bus || dac_transaction_active_bus || dac_busy_bus ||
                            dac_launch_pending_bus || dac_run_outstanding_bus ||
                            dac_prime_busy_bus || dac_stop_pending_bus ||
                            dac_stop_outstanding_bus;
    // Software must not report a safe/idle initialization while the MCP4922
    // may still hold a pre-reset code.  Keep this status-only busy indication
    // asserted even when the DAC clock is unavailable and the physical zero
    // transaction cannot yet start.  Configuration locking remains separate so
    // an independent TTL/OFF configuration can still be prepared in that case.
    wire controller_busy_status_bus = controller_busy_bus ||
                                      mandatory_zero_required_bus;

    wire stimulation_event_active_bus = ttl_active_bus ||
                                         dac_run_outstanding_bus;

    // Register the accepted stimulation-event envelope before it leaves this
    // clock domain. A DAC event begins when its trigger is accepted and remains
    // active through terminal A/B zero acknowledgement; PRIME and unrelated
    // controller bookkeeping are excluded. Hardware safe-off terminates the
    // marker immediately even while physical zero cleanup completes.
    // Intan synchronizes this level into spi_clk; presenting a source flip-flop
    // keeps that CDC boundary glitch-free and gives report_cdc a canonical
    // register-to-synchronizer path.
    always @(posedge s00_axi_aclk) begin
        if (!s00_axi_aresetn)
            intan_marker_active <= 1'b0;
        else
            intan_marker_active <= stimulation_event_active_bus &&
                                   !safe_off_active;
    end

    assign configuration_locked = armed_bus || arm_launch_pending_bus ||
                                arm_request_inflight_bus || arm_cancel_pending_bus ||
                                trigger_request_inflight_bus ||
                                prime_launch_pending_bus || dac_prime_outstanding_bus ||
                                dac_prime_cancel_pending_bus ||
                                dac_stop_outstanding_bus ||
                                (mandatory_zero_required_bus &&
                                 dac_domain_available_bus) ||
                                disarm_pending_bus || controller_busy_bus;
    wire external_trigger_rise = stim_trigger && !stim_trigger_d;
    wire requested_trigger = software_trigger_pulse ||
                             (armed_bus && active_external_trigger_enable && external_trigger_rise);
    wire cancel_trigger_now = stop_pulse || disarm_pulse || safe_off_active;
    wire arm_ack_now = arm_request_inflight_bus &&
                       (arm_ack_toggle_bus == arm_request_toggle_bus);
    wire dac_epoch_abort_now = !dac_domain_available_bus &&
        (arm_launch_pending_bus || arm_request_inflight_bus ||
         arm_cancel_pending_bus || trigger_request_inflight_bus ||
         dac_launch_pending_bus || dac_stop_pending_bus || dac_stop_outstanding_bus ||
         dac_run_outstanding_bus || prime_launch_pending_bus ||
         dac_prime_outstanding_bus || dac_prime_busy_bus ||
         dac_prime_cancel_pending_bus || dac_primed_bus);
    wire dac_waveform_abort_now = dac_epoch_abort_now &&
                                  dac_run_outstanding_bus;
    wire dac_prime_abort_now = dac_epoch_abort_now &&
                              (prime_launch_pending_bus || dac_prime_outstanding_bus ||
                               dac_prime_busy_bus || dac_prime_completion_pending_bus ||
                               dac_prime_cancel_pending_bus || dac_primed_bus);

    // Fault incidents are formed only after every DAC-domain source has crossed
    // into s00_axi_aclk.  The register bank owns the sticky/error-mask plane;
    // these signals describe one incident cycle and never drive physical TTL.
    wire arm_command_accepted_now = arm_pulse && !prime_dac_pulse &&
        !disarm_pulse && !safe_off_active && !configuration_locked && configuration_valid &&
        ((shadow_action_mode != ACTION_MODE_DAC) ||
         (dac_domain_available_bus && !mandatory_zero_required_bus &&
          !dac_abort_sticky_bus && dac_primed_bus));
    wire prime_dac_command_accepted_now = prime_dac_pulse && !arm_pulse &&
        !disarm_pulse && !stop_pulse && !safe_off_active && !configuration_locked &&
        (shadow_action_mode == ACTION_MODE_DAC) && configuration_valid &&
        dac_domain_available_bus && !mandatory_zero_required_bus &&
        !dac_abort_sticky_bus;
    wire prime_dac_command_processed_now = prime_dac_pulse && !arm_pulse &&
                                          !disarm_pulse && !stop_pulse;
    wire rejected_command_event =
        (arm_pulse && !safe_off_active &&
         !arm_command_accepted_now) ||
        (prime_dac_pulse && !safe_off_active &&
         !prime_dac_command_accepted_now) ||
        dac_invalid_event_bus;

    wire dac_prime_fault_occurrence_event =
        !(safe_off_active || safe_off_prime_cancel_bus) && (
        (prime_dac_command_processed_now &&
         !prime_dac_command_accepted_now) ||
        dac_prime_fault_event_bus ||
        (dac_prime_done_event_bus &&
         (dac_prime_cancel_pending_bus || stop_pulse || disarm_pulse)) ||
        (dac_prime_completion_pending_bus && !dac_transaction_active_bus &&
         !dac_busy_bus &&
         (dac_prime_cancel_pending_bus || stop_pulse || disarm_pulse)) ||
        (stop_pulse &&
         (prime_launch_pending_bus || dac_prime_completion_pending_bus ||
          dac_prime_done_event_bus)) ||
        (disarm_pulse &&
         (prime_launch_pending_bus || dac_prime_completion_pending_bus ||
          dac_prime_done_event_bus)) ||
        dac_prime_abort_now);

    wire controller_trigger_dropped_event = requested_trigger &&
        !cancel_trigger_now && (active_action_mode != ACTION_MODE_OFF) &&
        ((!armed_bus || controller_busy_bus || trigger_request_inflight_bus) ||
         ((active_action_mode == ACTION_MODE_DAC) &&
          (!dac_domain_available_bus || !dac_primed_bus)) ||
         ((active_action_mode != ACTION_MODE_TTL) &&
          (active_action_mode != ACTION_MODE_DAC)));
    wire controller_trigger_accepted_event = requested_trigger &&
        !cancel_trigger_now && armed_bus && !controller_busy_bus &&
        !trigger_request_inflight_bus &&
        ((active_action_mode == ACTION_MODE_TTL) ||
         ((active_action_mode == ACTION_MODE_DAC) &&
          dac_domain_available_bus && dac_primed_bus));

    // These are independent missed-action sources.  Summing them once avoids
    // losing an increment when, for example, a new request is rejected on the
    // same AXI edge that an older DAC request returns UNSERVED or is aborted.
    wire [1:0] unserved_trigger_increment_bus =
        {1'b0, controller_trigger_dropped_event} +
        {1'b0, dac_unserved_event_bus} +
        {1'b0, dac_waveform_abort_now};
    wire trigger_dropped_event = controller_trigger_dropped_event ||
                                 dac_unserved_event_bus ||
                                 dac_waveform_abort_now;
    wire [6:1] error_event_vector = {
        trigger_dropped_event,
        dac_timing_late_event_bus,
        dac_waveform_abort_now,
        dac_epoch_abort_now,
        dac_prime_fault_occurrence_event,
        rejected_command_event
    };
    wire clear_controller_diagnostics_now = clear_diagnostics_pulse &&
        !dac_terminal_event_bus && !dac_waveform_abort_now &&
        !dac_prime_abort_now && !dac_prime_done_event_bus &&
        !dac_prime_fault_event_bus;

    always @(posedge s00_axi_aclk) begin
        if (!s00_axi_aresetn) begin
            active_action_mode <= ACTION_MODE_OFF;
            active_external_trigger_enable <= 1'b0;
            active_ttl_pulse_width_axi_cycles <= 32'd0; active_intan_stim_marker_mask <= 14'd0;
            active_dac_a_enable <= 1'b0; active_dac_b_enable <= 1'b0;
            active_dac_continuous <= 1'b0;
            active_dac_update_period_clocks <= 32'd0;
            active_dac_start_index <= {RAM_ADDR_WIDTH{1'b0}};
            active_dac_loop_index <= {RAM_ADDR_WIDTH{1'b0}};
            active_dac_end_index <= {RAM_ADDR_WIDTH{1'b0}};
            active_dac_finite_update_count <= 32'd0;
            arm_request_toggle_bus <= 1'b0; arm_launch_pending_bus <= 1'b0;
            arm_request_inflight_bus <= 1'b0; arm_cancel_pending_bus <= 1'b0;
            armed_bus <= 1'b0;
            trigger_request_toggle_bus <= 1'b0; trigger_request_inflight_bus <= 1'b0;
            prime_request_toggle_bus <= 1'b0; prime_launch_pending_bus <= 1'b0;
            dac_prime_outstanding_bus <= 1'b0;
            dac_launch_pending_bus <= 1'b0; dac_stop_pending_bus <= 1'b0;
            dac_stop_outstanding_bus <= 1'b0;
            dac_run_outstanding_bus <= 1'b0; dac_abort_sticky_bus <= 1'b0;
            dac_primed_bus <= 1'b0; dac_prime_busy_bus <= 1'b0;
            dac_prime_completion_pending_bus <= 1'b0;
            dac_prime_cancel_pending_bus <= 1'b0;
            dac_prime_done_sticky_bus <= 1'b0; dac_prime_fault_sticky_bus <= 1'b0;
            disarm_pending_bus <= 1'b0; stop_request_toggle_bus <= 1'b0;
            clear_request_toggle_bus <= 1'b0; ttl_active_bus <= 1'b0;
            ttl_cycles_remaining <= 32'd0; stim_trigger_d <= 1'b0;
            // MCP4922 retains its analog code across an FPGA/AXI reset, so the
            // first available DAC-clock epoch must explicitly zero both outputs.
            mandatory_zero_required_bus <= 1'b1;
            safe_off_prime_cancel_bus <= 1'b0;
            accepted_trigger_count <= 32'd0; unserved_trigger_count <= 32'd0;
        end else begin
            stim_trigger_d <= stim_trigger;

            // Preserve the safety request after button release and across a
            // failed DAC-clock epoch.  Any terminal event below is generated
            // only after both hard-zero frames have completed in the engine.
            if (safe_off_event || stop_pulse || dac_epoch_abort_now)
                mandatory_zero_required_bus <= 1'b1;
            if ((mandatory_zero_required_bus || safe_off_event) &&
                (dac_zero_done_event_bus || dac_stop_done_event_bus))
                mandatory_zero_required_bus <= 1'b0;
            // This acknowledgement is tagged by the engine to the consumed
            // STOP request.  A natural cleanup may assert ZERO_DONE first, but
            // cannot retire a delayed STOP still crossing into the DAC domain.
            if (dac_stop_done_event_bus)
                dac_stop_outstanding_bus <= 1'b0;

            // A request lost to a DAC-domain reset is retried when that domain
            // is usable again.  The pending/outstanding split keeps the AXI
            // configuration locked across the CDC latency of an idle zeroing.
            if (mandatory_zero_required_bus && dac_domain_available_bus &&
                !dac_stop_pending_bus && !dac_stop_outstanding_bus &&
                !dac_terminal_event_bus && !dac_prime_done_event_bus &&
                !dac_prime_fault_event_bus && !dac_zero_done_event_bus &&
                !dac_stop_done_event_bus)
                dac_stop_pending_bus <= 1'b1;

            if (ttl_active_bus) begin
                if (ttl_cycles_remaining <= 32'd1) begin
                    ttl_active_bus <= 1'b0; ttl_cycles_remaining <= 32'd0;
                end else
                    ttl_cycles_remaining <= ttl_cycles_remaining - 32'd1;
            end

            // Software primes the analog output once before arming. The
            // request is delayed one bus cycle so the held config payload reaches
            // the DAC synchronizers before the extra-staged request toggle.
            if (prime_dac_command_processed_now && !safe_off_active) begin
                if (prime_dac_command_accepted_now) begin
                    dac_primed_bus <= 1'b0;
                    dac_prime_done_sticky_bus <= 1'b0;
                    dac_prime_fault_sticky_bus <= 1'b0;
                    dac_prime_completion_pending_bus <= 1'b0;
                    dac_prime_cancel_pending_bus <= 1'b0;
                    safe_off_prime_cancel_bus <= 1'b0;
                    active_dac_a_enable <= shadow_dac_a_enable;
                    active_dac_b_enable <= shadow_dac_b_enable;
                    active_dac_continuous <= shadow_dac_continuous;
                    active_dac_update_period_clocks <= shadow_dac_update_period_clocks;
                    active_dac_start_index <= shadow_dac_start_index;
                    active_dac_loop_index <= shadow_dac_loop_index;
                    active_dac_end_index <= shadow_dac_end_index;
                    active_dac_finite_update_count <= shadow_dac_finite_update_count;
                    dac_prime_busy_bus <= 1'b1;
                    prime_launch_pending_bus <= 1'b1;
                end else begin
                    // A rejected command is diagnostic only: preserve an
                    // existing prime and any prime request already in flight.
                    dac_prime_fault_sticky_bus <= 1'b1;
                end
            end
            if (prime_launch_pending_bus && dac_domain_available_bus &&
                !stop_pulse && !disarm_pulse && !safe_off_active) begin
                prime_request_toggle_bus <= !prime_request_toggle_bus;
                prime_launch_pending_bus <= 1'b0;
                dac_prime_outstanding_bus <= 1'b1;
            end

            if (arm_command_accepted_now) begin
                active_action_mode <= shadow_action_mode;
                active_external_trigger_enable <= shadow_external_trigger_enable;
                active_ttl_pulse_width_axi_cycles <= shadow_ttl_pulse_width_axi_cycles;
                active_intan_stim_marker_mask <= shadow_intan_stim_marker_mask;
                if (shadow_action_mode == ACTION_MODE_DAC)
                    arm_launch_pending_bus <= 1'b1;
                else
                    armed_bus <= 1'b1;
            end
            if (arm_launch_pending_bus && !arm_request_inflight_bus &&
                !disarm_pulse && !safe_off_active) begin
                arm_request_toggle_bus <= !arm_request_toggle_bus;
                arm_request_inflight_bus <= 1'b1; arm_launch_pending_bus <= 1'b0;
            end
            if (arm_ack_now) begin
                arm_request_inflight_bus <= 1'b0;
                if (arm_cancel_pending_bus || disarm_pulse || safe_off_active) begin
                    arm_cancel_pending_bus <= 1'b0;
                    armed_bus <= 1'b0;
                end else begin
                    armed_bus <= 1'b1;
                end
            end
            if (trigger_request_inflight_bus &&
                (trigger_ack_toggle_bus == trigger_request_toggle_bus))
                trigger_request_inflight_bus <= 1'b0;
            if (dac_transaction_active_bus || dac_terminal_event_bus)
                dac_launch_pending_bus <= 1'b0;
            if (dac_terminal_event_bus) begin
                dac_stop_pending_bus <= 1'b0;
                dac_run_outstanding_bus <= 1'b0;
            end
            if (dac_prime_done_event_bus) begin
                prime_launch_pending_bus <= 1'b0;
                dac_stop_pending_bus <= 1'b0;
                safe_off_prime_cancel_bus <= 1'b0;
                if (dac_prime_cancel_pending_bus || stop_pulse || disarm_pulse ||
                    safe_off_active || safe_off_prime_cancel_bus) begin
                    dac_prime_outstanding_bus <= 1'b0;
                    dac_prime_busy_bus <= 1'b0;
                    dac_prime_completion_pending_bus <= 1'b0;
                    dac_prime_cancel_pending_bus <= 1'b0;
                    dac_primed_bus <= 1'b0;
                    dac_prime_done_sticky_bus <= 1'b0;
                    dac_prime_fault_sticky_bus <=
                        !(safe_off_active || safe_off_prime_cancel_bus);
                end else begin
                    dac_prime_completion_pending_bus <= 1'b1;
                end
            end
            if (dac_prime_completion_pending_bus &&
                !dac_transaction_active_bus && !dac_busy_bus) begin
                dac_prime_outstanding_bus <= 1'b0;
                dac_prime_busy_bus <= 1'b0;
                dac_prime_completion_pending_bus <= 1'b0;
                safe_off_prime_cancel_bus <= 1'b0;
                if (dac_prime_cancel_pending_bus || stop_pulse || disarm_pulse ||
                    safe_off_active || safe_off_prime_cancel_bus) begin
                    dac_prime_cancel_pending_bus <= 1'b0;
                    dac_primed_bus <= 1'b0;
                    dac_prime_done_sticky_bus <= 1'b0;
                    dac_prime_fault_sticky_bus <=
                        !(safe_off_active || safe_off_prime_cancel_bus);
                end else begin
                    dac_primed_bus <= 1'b1;
                    dac_prime_done_sticky_bus <= 1'b1;
                end
            end
            if (dac_prime_fault_event_bus) begin
                prime_launch_pending_bus <= 1'b0;
                dac_prime_outstanding_bus <= 1'b0;
                dac_prime_busy_bus <= 1'b0;
                dac_prime_completion_pending_bus <= 1'b0;
                dac_prime_cancel_pending_bus <= 1'b0;
                safe_off_prime_cancel_bus <= 1'b0;
                dac_stop_pending_bus <= 1'b0;
                dac_primed_bus <= 1'b0;
                dac_prime_done_sticky_bus <= 1'b0;
                dac_prime_fault_sticky_bus <=
                    !(safe_off_active || safe_off_prime_cancel_bus);
            end

            // Wait for a just-issued trigger/prime to become visible as a
            // running engine before stopping it.  With no launch in flight,
            // STOP is also legal while idle and performs the mandatory
            // startup/button/epoch-recovery A/B hard-zero transaction.
            if (dac_stop_pending_bus && !dac_stop_outstanding_bus &&
                dac_domain_available_bus &&
                !dac_terminal_event_bus && !dac_prime_done_event_bus &&
                !dac_prime_fault_event_bus &&
                (dac_transaction_active_bus ||
                 (!dac_launch_pending_bus && !trigger_request_inflight_bus &&
                  !dac_run_outstanding_bus && !prime_launch_pending_bus &&
                  !dac_prime_outstanding_bus && !dac_prime_busy_bus &&
                  !dac_prime_completion_pending_bus))) begin
                stop_request_toggle_bus <= !stop_request_toggle_bus;
                dac_stop_pending_bus <= 1'b0;
                dac_stop_outstanding_bus <= 1'b1;
            end

            // STOP and DISARM have explicit precedence over either a software
            // trigger or a same-cycle external trigger edge.  No trigger toggle or TTL
            // pulse is created for a cancelled event.
            if (controller_trigger_accepted_event) begin
                // The existing pulse-width register also controls an optional
                // physical TTL companion for DAC actions.  Start it from the
                // same accepted event as the DAC request so rejected/busy
                // triggers can neither create nor extend a pulse.
                if (((active_action_mode == ACTION_MODE_TTL) ||
                     (active_action_mode == ACTION_MODE_DAC)) &&
                    (active_ttl_pulse_width_axi_cycles != 32'd0)) begin
                    ttl_active_bus <= 1'b1;
                    ttl_cycles_remaining <= active_ttl_pulse_width_axi_cycles;
                end
                if (active_action_mode == ACTION_MODE_DAC) begin
                    // DAC admission already validated the clock domain and PRIME.
                    trigger_request_toggle_bus <= !trigger_request_toggle_bus;
                    trigger_request_inflight_bus <= 1'b1;
                    dac_launch_pending_bus <= 1'b1;
                    dac_run_outstanding_bus <= 1'b1;
                end
            end

            if (stop_pulse) begin
                ttl_active_bus <= 1'b0; ttl_cycles_remaining <= 32'd0;
                if (prime_launch_pending_bus) begin
                    prime_launch_pending_bus <= 1'b0;
                    dac_prime_outstanding_bus <= 1'b0;
                    dac_prime_busy_bus <= 1'b0;
                    dac_prime_completion_pending_bus <= 1'b0;
                    dac_prime_cancel_pending_bus <= 1'b0;
                    dac_primed_bus <= 1'b0;
                    dac_prime_done_sticky_bus <= 1'b0;
                    dac_prime_fault_sticky_bus <= 1'b1;
                end else if (!dac_prime_fault_event_bus &&
                             (dac_prime_outstanding_bus || dac_prime_busy_bus ||
                              dac_prime_completion_pending_bus ||
                              dac_prime_done_event_bus)) begin
                    dac_prime_cancel_pending_bus <= 1'b1;
                    dac_primed_bus <= 1'b0;
                    dac_prime_done_sticky_bus <= 1'b0;
                    if (dac_prime_completion_pending_bus ||
                        dac_prime_done_event_bus) begin
                        dac_prime_outstanding_bus <= 1'b0;
                        dac_prime_busy_bus <= 1'b0;
                        dac_prime_completion_pending_bus <= 1'b0;
                        dac_prime_cancel_pending_bus <= 1'b0;
                        dac_stop_pending_bus <= 1'b0;
                        dac_prime_fault_sticky_bus <= 1'b1;
                    end else if (!dac_prime_fault_event_bus) begin
                        dac_stop_pending_bus <= 1'b1;
                    end
                end else if (!dac_terminal_event_bus &&
                    !dac_prime_done_event_bus && !dac_prime_fault_event_bus &&
                    !dac_prime_completion_pending_bus &&
                    (dac_run_outstanding_bus || dac_transaction_active_bus ||
                     dac_launch_pending_bus || trigger_request_inflight_bus ||
                     dac_prime_outstanding_bus || dac_prime_busy_bus))
                    dac_stop_pending_bus <= 1'b1;
            end

            if (disarm_pulse) begin
                ttl_active_bus <= 1'b0;
                ttl_cycles_remaining <= 32'd0;
                armed_bus <= 1'b0;

                // ARM is a request/ack transaction even though the destination
                // has no persistent armed state.  Drain an already-issued
                // request and suppress its eventual ACK from setting ARMED.
                if (arm_launch_pending_bus)
                    arm_launch_pending_bus <= 1'b0;
                if (arm_request_inflight_bus && !arm_ack_now)
                    arm_cancel_pending_bus <= 1'b1;

                if (prime_launch_pending_bus) begin
                    prime_launch_pending_bus <= 1'b0;
                    dac_prime_outstanding_bus <= 1'b0;
                    dac_prime_busy_bus <= 1'b0;
                    dac_prime_completion_pending_bus <= 1'b0;
                    dac_prime_cancel_pending_bus <= 1'b0;
                    dac_primed_bus <= 1'b0;
                    dac_prime_done_sticky_bus <= 1'b0;
                    dac_prime_fault_sticky_bus <= !safe_off_active;
                end else if (!dac_prime_fault_event_bus &&
                             (dac_prime_outstanding_bus || dac_prime_busy_bus ||
                              dac_prime_completion_pending_bus ||
                              dac_prime_done_event_bus)) begin
                    dac_prime_cancel_pending_bus <= 1'b1;
                    dac_primed_bus <= 1'b0;
                    dac_prime_done_sticky_bus <= 1'b0;
                    if (dac_prime_completion_pending_bus ||
                        dac_prime_done_event_bus) begin
                        dac_prime_outstanding_bus <= 1'b0;
                        dac_prime_busy_bus <= 1'b0;
                        dac_prime_completion_pending_bus <= 1'b0;
                        dac_prime_cancel_pending_bus <= 1'b0;
                        dac_stop_pending_bus <= 1'b0;
                        dac_prime_fault_sticky_bus <= !safe_off_active;
                    end else if (!dac_prime_fault_event_bus) begin
                        dac_stop_pending_bus <= 1'b1;
                    end
                end

                // Intan acquisition is independent.  DISARM revokes stimulation
                // immediately, then keeps configuration locked only while an
                // already-accepted DAC transaction finishes its safe zeroing.
                if (!dac_terminal_event_bus && !dac_prime_done_event_bus &&
                    !dac_prime_fault_event_bus &&
                    !dac_prime_completion_pending_bus &&
                    (dac_run_outstanding_bus || dac_transaction_active_bus ||
                     dac_launch_pending_bus || trigger_request_inflight_bus ||
                     dac_prime_outstanding_bus || dac_prime_busy_bus)) begin
                    disarm_pending_bus <= 1'b1;
                    dac_stop_pending_bus <= 1'b1;
                end
            end
            if (disarm_pending_bus &&
                !dac_transaction_active_bus && !dac_busy_bus &&
                !dac_launch_pending_bus && !dac_run_outstanding_bus &&
                !trigger_request_inflight_bus && !dac_stop_pending_bus &&
                !dac_stop_outstanding_bus &&
                !arm_request_inflight_bus && !arm_cancel_pending_bus) begin
                disarm_pending_bus <= 1'b0;
                armed_bus <= 1'b0;
            end

            // A destination reset deliberately discards requests from the old
            // toggle epoch. Retire the matching source state. If a waveform
            // had been accepted, latch ERROR_STATUS.DAC_WAVEFORM_ABORT, count
            // the unserved action, and force a disarmed state.
            // CLEAR_DIAGNOSTICS is then required before DAC ARM can succeed
            // again. MCP4922 holds its last analog code
            // across this reset, so mandatory_zero_required_bus also retains a
            // hard-zero retry for the first usable replacement clock epoch.
            if (dac_epoch_abort_now) begin
                arm_launch_pending_bus <= 1'b0;
                arm_request_inflight_bus <= 1'b0;
                arm_cancel_pending_bus <= 1'b0;
                trigger_request_inflight_bus <= 1'b0;
                prime_launch_pending_bus <= 1'b0;
                dac_prime_outstanding_bus <= 1'b0;
                dac_prime_completion_pending_bus <= 1'b0;
                dac_prime_cancel_pending_bus <= 1'b0;
                safe_off_prime_cancel_bus <= 1'b0;
                dac_launch_pending_bus <= 1'b0;
                dac_stop_pending_bus <= 1'b0;
                dac_stop_outstanding_bus <= 1'b0;
                if (dac_prime_abort_now) begin
                    dac_prime_busy_bus <= 1'b0;
                    dac_primed_bus <= 1'b0;
                    dac_prime_done_sticky_bus <= 1'b0;
                    dac_prime_fault_sticky_bus <= 1'b1;
                    dac_abort_sticky_bus <= 1'b1;
                end
                if (dac_waveform_abort_now) begin
                    dac_run_outstanding_bus <= 1'b0;
                    dac_abort_sticky_bus <= 1'b1;
                    armed_bus <= 1'b0;
                    disarm_pending_bus <= 1'b0;
                end else begin
                    armed_bus <= 1'b0;
                end
            end
            if (clear_controller_diagnostics_now) begin
                dac_abort_sticky_bus <= 1'b0;
                dac_prime_done_sticky_bus <= 1'b0;
                dac_prime_fault_sticky_bus <= 1'b0;
                clear_request_toggle_bus <= !clear_request_toggle_bus;
            end

            // Trigger counter updates are centralized so simultaneous sources
            // accumulate.  A real event is set/count dominant over a coincident
            // CLEAR_DIAGNOSTICS request, matching the fault and safety histories.
            if (controller_trigger_accepted_event)
                accepted_trigger_count <= accepted_trigger_count + 32'd1;
            else if (clear_controller_diagnostics_now)
                accepted_trigger_count <= 32'd0;

            if (unserved_trigger_increment_bus != 2'd0)
                unserved_trigger_count <= unserved_trigger_count +
                    {30'd0, unserved_trigger_increment_bus};
            else if (clear_controller_diagnostics_now)
                unserved_trigger_count <= 32'd0;

            // Any accepted action, waveform, DAC, or clock change invalidates the
            // analog state established by PRIME_DAC. Keep this after the DONE
            // handling so invalidation wins even if both arrive together.
            if (dac_prime_invalidate_pulse) begin
                dac_primed_bus <= 1'b0;
                dac_prime_done_sticky_bus <= 1'b0;
            end

            // The board button shares DISARM above and retains a completed PRIME.
            // A press during an unfinished PRIME cancels only that transaction;
            // remember its origin until the DAC acknowledges the cancellation.
            if (safe_off_event)
                safe_off_prime_cancel_bus <= !dac_prime_fault_event_bus &&
                    !dac_prime_done_event_bus && !dac_prime_completion_pending_bus &&
                    (dac_prime_outstanding_bus || dac_prime_busy_bus);

        end
    end

    assign intan_marker_mask = active_intan_stim_marker_mask;

    // Export ready-to-map levels. Safety is owned here, before the pin router;
    // these gates are combinational so a safe-off assertion adds no AXI cycle.
    reg trigger_monitor_blocked;
    always @(posedge s00_axi_aclk) begin
        if (!s00_axi_aresetn)
            trigger_monitor_blocked <= 1'b0;
        else if (safe_off_active)
            trigger_monitor_blocked <= 1'b1;
        else if (!stim_trigger)
            trigger_monitor_blocked <= 1'b0;
    end
    assign stimulus_level = ttl_active_bus && !safe_off_active;
    assign trigger_monitor_level = stim_trigger && !safe_off_active &&
                                   !trigger_monitor_blocked;

    stim_axil_register_bank #(
        .AXI_DATA_WIDTH(C_S00_AXI_DATA_WIDTH), .AXI_ADDR_WIDTH(C_S00_AXI_ADDR_WIDTH),
        .RAM_DEPTH(RAM_DEPTH), .RAM_ADDR_WIDTH(RAM_ADDR_WIDTH)) register_bank (
        .clk(s00_axi_aclk), .resetn(s00_axi_aresetn),
        .s_axi_awaddr(s00_axi_awaddr), .s_axi_awprot(s00_axi_awprot),
        .s_axi_awvalid(s00_axi_awvalid), .s_axi_awready(s00_axi_awready),
        .s_axi_wdata(s00_axi_wdata), .s_axi_wstrb(s00_axi_wstrb),
        .s_axi_wvalid(s00_axi_wvalid), .s_axi_wready(s00_axi_wready),
        .s_axi_bresp(s00_axi_bresp), .s_axi_bvalid(s00_axi_bvalid), .s_axi_bready(s00_axi_bready),
        .s_axi_araddr(s00_axi_araddr), .s_axi_arprot(s00_axi_arprot),
        .s_axi_arvalid(s00_axi_arvalid), .s_axi_arready(s00_axi_arready),
        .s_axi_rdata(s00_axi_rdata), .s_axi_rresp(s00_axi_rresp),
        .s_axi_rvalid(s00_axi_rvalid), .s_axi_rready(s00_axi_rready),
        .arm_pulse(arm_pulse), .disarm_pulse(software_disarm_pulse),
        .software_trigger_pulse(software_trigger_pulse), .stop_pulse(stop_pulse),
        .clear_diagnostics_pulse(clear_diagnostics_pulse), .prime_dac_pulse(prime_dac_pulse),
        .dac_clock_program_pulse(dac_clock_program_pulse),
        .dac_prime_invalidate_pulse(dac_prime_invalidate_pulse),
        .cfg_action_mode(shadow_action_mode),
        .cfg_external_trigger_enable(shadow_external_trigger_enable),
        .cfg_ttl_pulse_width_axi_cycles(shadow_ttl_pulse_width_axi_cycles),
        .cfg_intan_stim_marker_mask(shadow_intan_stim_marker_mask),
        .cfg_dac_a_enable(shadow_dac_a_enable), .cfg_dac_b_enable(shadow_dac_b_enable),
        .cfg_dac_continuous(shadow_dac_continuous),
        .cfg_dac_update_period_clocks(shadow_dac_update_period_clocks),
        .cfg_dac_start_index(shadow_dac_start_index),
        .cfg_dac_loop_index(shadow_dac_loop_index), .cfg_dac_end_index(shadow_dac_end_index),
        .cfg_dac_finite_update_count(shadow_dac_finite_update_count),
        .cfg_dac_clock_o(shadow_dac_clock_o),
        .cfg_dac_clock_d(shadow_dac_clock_d),
        .cfg_dac_clock_m(shadow_dac_clock_m),
        .configuration_locked(configuration_locked), .configuration_valid(configuration_valid),
        .dac_clock_config_valid(dac_clock_config_valid),
        .armed(armed_bus),
        .controller_busy(controller_busy_status_bus),
        .ttl_active(ttl_active_bus), .dac_transaction_active(dac_transaction_active_bus), .dac_busy(dac_busy_bus),
        .dac_clock_locked(dac_clock_locked_bus), .dac_clock_ready(dac_clock_ready),
        .dac_clock_program_busy(dac_clock_program_busy),
        .dac_clock_error(dac_clock_error),
        .dac_clock_error_code(dac_clock_error_code),
        .dac_current_waveform_index(dac_current_waveform_index_stable),
        .dac_completed_update_count(dac_completed_update_count_stable),
        .accepted_trigger_count(accepted_trigger_count), .unserved_trigger_count(unserved_trigger_count),
        .dac_primed(dac_primed_bus), .dac_prime_busy(dac_prime_busy_bus),
        .dac_prime_done_sticky(dac_prime_done_sticky_bus),
        .dac_prime_fault_sticky(dac_prime_fault_sticky_bus),
        .safe_off_active(safe_off_active),
        .safe_off_event(safe_off_event),
        .error_event_vector(error_event_vector),
        .stim_fault_irq(stim_fault_irq),
        .ram_bus_addr(ram_bus_addr),
        .ram_bus_write_enable(ram_bus_write_enable),
        .ram_bus_write_strobes(ram_bus_write_strobes), .ram_bus_write_data(ram_bus_write_data),
        .ram_bus_read_enable(ram_bus_read_enable), .ram_bus_read_data(ram_bus_read_data));

endmodule

`default_nettype wire
