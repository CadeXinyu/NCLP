`timescale 1ns / 1ps
`default_nettype none

// DAC-domain transport only: held configuration, command-toggle receivers,
// return acknowledgements/events, stable status sampling and reset-epoch guards.
// The controller owns admission, outstanding transactions and safety policy.
// Preserve the source/destination stages: their relative delays are part of the
// request/payload and clock-loss contracts, not discretionary pipeline depth.
module stim_dac_cdc #(
    parameter integer CONFIG_WIDTH = 97,
    parameter integer STATUS_WIDTH = 42
) (
    input wire bus_clk,
    input wire bus_resetn,
    input wire dac_clk,
    input wire dac_resetn,
    input wire clock_reset_request_bus,
    input wire manager_clock_locked,
    input wire clock_ready_bus,
    input wire clock_program_busy_bus,
    input wire status_capture_enable_bus,
    input wire [CONFIG_WIDTH-1:0] config_bus,
    output wire [CONFIG_WIDTH-1:0] config_dac,
    input wire arm_request_toggle_bus,
    input wire trigger_request_toggle_bus,
    input wire prime_request_toggle_bus,
    input wire stop_request_toggle_bus,
    input wire clear_request_toggle_bus,
    output wire arm_ack_toggle_bus,
    output wire trigger_ack_toggle_bus,
    output wire trigger_pulse_dac,
    output wire prime_pulse_dac,
    output wire stop_pulse_dac,
    output wire clear_diagnostics_pulse_dac,
    input wire transaction_active_dac,
    input wire busy_dac,
    input wire done_pulse_dac,
    input wire prime_done_pulse_dac,
    input wire prime_fault_pulse_dac,
    input wire zero_done_pulse_dac,
    input wire stop_done_pulse_dac,
    input wire unserved_trigger_sticky_dac,
    input wire invalid_config_sticky_dac,
    input wire timing_late_sticky_dac,
    input wire [STATUS_WIDTH-1:0] status_dac,
    output wire [STATUS_WIDTH-1:0] status_bus,
    output wire domain_available_bus,
    output wire clock_locked_bus,
    output wire transaction_active_bus,
    output wire busy_bus,
    output wire done_event_bus,
    output wire prime_done_event_bus,
    output wire prime_fault_event_bus,
    output wire zero_done_event_bus,
    output wire stop_done_event_bus,
    output wire unserved_event_bus,
    output wire invalid_event_bus,
    output wire timing_late_event_bus
);
    (* ASYNC_REG = "TRUE" *) reg [CONFIG_WIDTH-1:0] dac_cfg_sync0, dac_cfg_sync1;
    (* ASYNC_REG = "TRUE" *) reg arm_request_sync0, arm_request_sync1;
    reg arm_request_sync2, arm_ack_toggle_dac;
    (* ASYNC_REG = "TRUE" *) reg arm_ack_sync0, arm_ack_sync1;
    reg [2:0] receiver_warmup_cycles;
    reg dac_cdc_initialized;

    always @(posedge dac_clk or negedge dac_resetn) begin
        if (!dac_resetn) begin
            receiver_warmup_cycles <= 3'd0;
            dac_cdc_initialized <= 1'b0;
        end else if (!dac_cdc_initialized) begin
            receiver_warmup_cycles <= receiver_warmup_cycles + 3'd1;
            // Register the destination-ready level before it crosses back to
            // AXI.  Asserting beside the old 6->7 count transition preserves
            // the original request-acceptance edge without a combinational
            // reduction feeding the bus-domain synchronizer.
            if (receiver_warmup_cycles == 3'd6)
                dac_cdc_initialized <= 1'b1;
        end
    end

    always @(posedge dac_clk or negedge dac_resetn) begin
        if (!dac_resetn) begin
            dac_cfg_sync0 <= {CONFIG_WIDTH{1'b0}};
            dac_cfg_sync1 <= {CONFIG_WIDTH{1'b0}};
            arm_request_sync0 <= 1'b0;
            arm_request_sync1 <= 1'b0;
            arm_request_sync2 <= 1'b0;
            arm_ack_toggle_dac <= 1'b0;
        end else begin
            dac_cfg_sync0 <= config_bus;
            dac_cfg_sync1 <= dac_cfg_sync0;
            arm_request_sync0 <= arm_request_toggle_bus;
            arm_request_sync1 <= arm_request_sync0;
            arm_request_sync2 <= arm_request_sync1;
            if (!dac_cdc_initialized)
                arm_ack_toggle_dac <= arm_request_sync1;
            else if (arm_request_sync1 != arm_request_sync2)
                arm_ack_toggle_dac <= arm_request_sync1;
        end
    end

    // Trigger/stop/status crossings for the DAC playback engine.
    (* ASYNC_REG = "TRUE" *) reg trigger_request_sync0, trigger_request_sync1;
    // Hold the request toggle until acknowledged; the destination consumes
    // each event once, independently of the source pulse width. Playback uses
    // the preset already transferred and acknowledged by PRIME/ARM.
    reg trigger_request_sync2, trigger_request_sync3, trigger_ack_toggle_dac;
    (* ASYNC_REG = "TRUE" *) reg prime_request_sync0, prime_request_sync1;
    reg prime_request_sync2;
    (* ASYNC_REG = "TRUE" *) reg trigger_ack_sync0, trigger_ack_sync1;
    (* ASYNC_REG = "TRUE" *) reg stop_request_sync0, stop_request_sync1;
    reg stop_request_sync2;
    (* ASYNC_REG = "TRUE" *) reg clear_request_sync0, clear_request_sync1;
    reg clear_request_sync2;
    wire dac_trigger_pulse = dac_resetn && dac_cdc_initialized &&
                             (trigger_request_sync2 ^ trigger_request_sync3);
    wire dac_prime_pulse = dac_resetn && dac_cdc_initialized &&
                          (prime_request_sync1 ^ prime_request_sync2);
    wire dac_stop_pulse = dac_resetn && dac_cdc_initialized &&
                          (stop_request_sync1 ^ stop_request_sync2);
    wire dac_clear_diagnostics_pulse = dac_resetn && dac_cdc_initialized &&
                                  (clear_request_sync1 ^ clear_request_sync2);

    always @(posedge dac_clk or negedge dac_resetn) begin
        if (!dac_resetn) begin
            trigger_request_sync0 <= 1'b0;
            trigger_request_sync1 <= 1'b0;
            trigger_request_sync2 <= 1'b0;
            trigger_request_sync3 <= 1'b0;
            trigger_ack_toggle_dac <= 1'b0;
            prime_request_sync0 <= 1'b0;
            prime_request_sync1 <= 1'b0;
            prime_request_sync2 <= 1'b0;
            stop_request_sync0 <= 1'b0;
            stop_request_sync1 <= 1'b0;
            stop_request_sync2 <= 1'b0;
            clear_request_sync0 <= 1'b0;
            clear_request_sync1 <= 1'b0;
            clear_request_sync2 <= 1'b0;
        end else begin
            trigger_request_sync0 <= trigger_request_toggle_bus;
            trigger_request_sync1 <= trigger_request_sync0;
            trigger_request_sync2 <= trigger_request_sync1;
            trigger_request_sync3 <= trigger_request_sync2;
            if (!dac_cdc_initialized)
                trigger_ack_toggle_dac <= trigger_request_sync2;
            else if (trigger_request_sync2 != trigger_request_sync3)
                trigger_ack_toggle_dac <= trigger_request_sync2;
            prime_request_sync0 <= prime_request_toggle_bus;
            prime_request_sync1 <= prime_request_sync0;
            prime_request_sync2 <= prime_request_sync1;
            stop_request_sync0 <= stop_request_toggle_bus;
            stop_request_sync1 <= stop_request_sync0;
            stop_request_sync2 <= stop_request_sync1;
            clear_request_sync0 <= clear_request_toggle_bus;
            clear_request_sync1 <= clear_request_sync0;
            clear_request_sync2 <= clear_request_sync1;
        end
    end

    reg done_toggle_dac, prime_done_toggle_dac, prime_fault_toggle_dac;
    reg zero_done_toggle_dac, stop_done_toggle_dac;
    reg dac_busy_cdc_source;
    always @(posedge dac_clk or negedge dac_resetn) begin
        if (!dac_resetn) begin
            done_toggle_dac <= 1'b0;
            prime_done_toggle_dac <= 1'b0;
            prime_fault_toggle_dac <= 1'b0;
            zero_done_toggle_dac <= 1'b0;
            stop_done_toggle_dac <= 1'b0;
            dac_busy_cdc_source <= 1'b0;
        end else begin
            // The engine's busy output is a combinational summary.  Register
            // it in the source domain so the AXI synchronizer sees one clean
            // launch flop instead of logic immediately before its first stage.
            dac_busy_cdc_source <= busy_dac;
            if (done_pulse_dac)
                done_toggle_dac <= !done_toggle_dac;
            if (prime_done_pulse_dac)
                prime_done_toggle_dac <= !prime_done_toggle_dac;
            if (prime_fault_pulse_dac)
                prime_fault_toggle_dac <= !prime_fault_toggle_dac;
            if (zero_done_pulse_dac)
                zero_done_toggle_dac <= !zero_done_toggle_dac;
            if (stop_done_pulse_dac)
                stop_done_toggle_dac <= !stop_done_toggle_dac;
        end
    end

    (* ASYNC_REG = "TRUE" *) reg transaction_active_sync0, transaction_active_sync1;
    (* ASYNC_REG = "TRUE" *) reg dac_busy_sync0, dac_busy_sync1;
    (* ASYNC_REG = "TRUE" *) reg dac_unserved_sync0, dac_unserved_sync1;
    reg dac_unserved_sync2;
    (* ASYNC_REG = "TRUE" *) reg dac_invalid_sync0, dac_invalid_sync1;
    reg dac_invalid_sync2;
    (* ASYNC_REG = "TRUE" *) reg dac_late_sync0, dac_late_sync1;
    reg dac_late_sync2;
    (* ASYNC_REG = "TRUE" *) reg dac_done_sync0, dac_done_sync1, dac_done_sync2;
    (* ASYNC_REG = "TRUE" *) reg dac_prime_done_sync0, dac_prime_done_sync1, dac_prime_done_sync2;
    (* ASYNC_REG = "TRUE" *) reg dac_prime_fault_sync0, dac_prime_fault_sync1, dac_prime_fault_sync2;
    (* ASYNC_REG = "TRUE" *) reg dac_zero_done_sync0, dac_zero_done_sync1, dac_zero_done_sync2;
    (* ASYNC_REG = "TRUE" *) reg dac_stop_done_sync0, dac_stop_done_sync1, dac_stop_done_sync2;
    (* ASYNC_REG = "TRUE" *) reg dac_locked_sync0, dac_locked_sync1;
    (* ASYNC_REG = "TRUE" *) reg [STATUS_WIDTH-1:0] dac_status_sync0;
    (* ASYNC_REG = "TRUE" *) reg [STATUS_WIDTH-1:0] dac_status_sync1;
    reg [STATUS_WIDTH-1:0] previous_status_sample;
    reg [STATUS_WIDTH-1:0] stable_status_bus;
    wire dac_bus_epoch_valid;
    always @(posedge bus_clk) begin
        if (!bus_resetn) begin
            arm_ack_sync0 <= 1'b0; arm_ack_sync1 <= 1'b0;
            trigger_ack_sync0 <= 1'b0; trigger_ack_sync1 <= 1'b0;
            transaction_active_sync0 <= 1'b0; transaction_active_sync1 <= 1'b0;
            dac_busy_sync0 <= 1'b0; dac_busy_sync1 <= 1'b0;
            dac_unserved_sync0 <= 1'b0; dac_unserved_sync1 <= 1'b0;
            dac_unserved_sync2 <= 1'b0;
            dac_invalid_sync0 <= 1'b0; dac_invalid_sync1 <= 1'b0;
            dac_invalid_sync2 <= 1'b0;
            dac_late_sync0 <= 1'b0; dac_late_sync1 <= 1'b0;
            dac_late_sync2 <= 1'b0;
            dac_done_sync0 <= 1'b0; dac_done_sync1 <= 1'b0; dac_done_sync2 <= 1'b0;
            dac_prime_done_sync0 <= 1'b0; dac_prime_done_sync1 <= 1'b0;
            dac_prime_done_sync2 <= 1'b0;
            dac_prime_fault_sync0 <= 1'b0; dac_prime_fault_sync1 <= 1'b0;
            dac_prime_fault_sync2 <= 1'b0;
            dac_zero_done_sync0 <= 1'b0; dac_zero_done_sync1 <= 1'b0;
            dac_zero_done_sync2 <= 1'b0;
            dac_stop_done_sync0 <= 1'b0; dac_stop_done_sync1 <= 1'b0;
            dac_stop_done_sync2 <= 1'b0;
            dac_locked_sync0 <= 1'b0; dac_locked_sync1 <= 1'b0;
            dac_status_sync0 <= {STATUS_WIDTH{1'b0}};
            dac_status_sync1 <= {STATUS_WIDTH{1'b0}};
            previous_status_sample <= {STATUS_WIDTH{1'b0}};
            stable_status_bus <= {STATUS_WIDTH{1'b0}};
        end else begin
            arm_ack_sync0 <= arm_ack_toggle_dac; arm_ack_sync1 <= arm_ack_sync0;
            trigger_ack_sync0 <= trigger_ack_toggle_dac; trigger_ack_sync1 <= trigger_ack_sync0;
            transaction_active_sync0 <= transaction_active_dac; transaction_active_sync1 <= transaction_active_sync0;
            dac_busy_sync0 <= dac_busy_cdc_source; dac_busy_sync1 <= dac_busy_sync0;
            dac_unserved_sync0 <= unserved_trigger_sticky_dac;
            dac_unserved_sync1 <= dac_unserved_sync0;
            dac_unserved_sync2 <= dac_unserved_sync1;
            dac_invalid_sync0 <= invalid_config_sticky_dac; dac_invalid_sync1 <= dac_invalid_sync0;
            dac_invalid_sync2 <= dac_invalid_sync1;
            dac_late_sync0 <= timing_late_sticky_dac;
            dac_late_sync1 <= dac_late_sync0;
            dac_late_sync2 <= dac_late_sync1;
            dac_done_sync0 <= done_toggle_dac; dac_done_sync1 <= dac_done_sync0;
            dac_done_sync2 <= dac_done_sync1;
            dac_prime_done_sync0 <= prime_done_toggle_dac;
            dac_prime_done_sync1 <= dac_prime_done_sync0;
            dac_prime_done_sync2 <= dac_prime_done_sync1;
            dac_prime_fault_sync0 <= prime_fault_toggle_dac;
            dac_prime_fault_sync1 <= dac_prime_fault_sync0;
            dac_prime_fault_sync2 <= dac_prime_fault_sync1;
            dac_zero_done_sync0 <= zero_done_toggle_dac;
            dac_zero_done_sync1 <= dac_zero_done_sync0;
            dac_zero_done_sync2 <= dac_zero_done_sync1;
            dac_stop_done_sync0 <= stop_done_toggle_dac;
            dac_stop_done_sync1 <= dac_stop_done_sync0;
            dac_stop_done_sync2 <= dac_stop_done_sync1;
            dac_locked_sync0 <= manager_clock_locked;
            dac_locked_sync1 <= dac_locked_sync0;
            // Update the stable status after two consecutive synchronized
            // bundle samples agree. The register bank separately snapshots the
            // index/count pair across successive software register reads.
            dac_status_sync0 <= status_dac;
            dac_status_sync1 <= dac_status_sync0;
            previous_status_sample <= dac_status_sync1;
            if ((dac_status_sync1 == previous_status_sample) &&
                dac_bus_epoch_valid && dac_locked_sync1 &&
                status_capture_enable_bus)
                stable_status_bus <= dac_status_sync1;
        end
    end

    // Use the vendor-recognized single-bit synchronizer for the DAC-domain
    // ready level.  A separate AXI-domain guard preserves the required
    // asynchronous invalidation on PLLE4 lock loss without folding that reset
    // topology into the synchronizer itself (which report_cdc classifies as
    // unknown circuitry).
    wire dac_cdc_initialized_bus;
    xpm_cdc_single #(
        .DEST_SYNC_FF(2),
        .INIT_SYNC_FF(1),
        .SIM_ASSERT_CHK(0),
        .SRC_INPUT_REG(0)
    ) dac_cdc_initialized_cdc (
        .src_clk(dac_clk),
        .src_in(dac_cdc_initialized),
        .dest_clk(bus_clk),
        .dest_out(dac_cdc_initialized_bus)
    );

    // Event-toggle histories are invalidated in the AXI domain whenever the
    // manager reset/lock/ready/program state is unavailable. Revalidation
    // requires observing READY low in the new synchronized epoch before its
    // rising level is accepted.  Together with the two XPM stages, the guard
    // retains the original three-bus-cycle revalidation latency and prevents a
    // stale high from surviving an AXI reset or a PLL relock.
    reg dac_bus_epoch_valid_reg;
    reg dac_bus_epoch_low_seen;
    always @(posedge bus_clk) begin
        if (!bus_resetn || clock_reset_request_bus) begin
            dac_bus_epoch_valid_reg <= 1'b0;
            dac_bus_epoch_low_seen <= 1'b0;
        end else begin
            if (clock_program_busy_bus)
                dac_bus_epoch_low_seen <= 1'b0;
            else if (!dac_cdc_initialized_bus)
                dac_bus_epoch_low_seen <= 1'b1;

            if (!dac_locked_sync1 || !clock_ready_bus ||
                clock_program_busy_bus || !dac_cdc_initialized_bus)
                dac_bus_epoch_valid_reg <= 1'b0;
            else if (dac_bus_epoch_low_seen)
                dac_bus_epoch_valid_reg <= 1'b1;
        end
    end
    // Qualify the registered epoch with the manager-owned reset request.  The
    // real-clock branch produces this level in the AXI/configuration domain;
    // the simulation branch mirrors the same immediate clock-unavailable
    // contract.  No raw primitive LOCKED signal enters an AXI reset pin.
    assign dac_bus_epoch_valid = dac_bus_epoch_valid_reg &&
                                 !clock_reset_request_bus;
    // The destination-ready level falls on DAC-domain reset and rises only after
    // its toggle receivers have re-baselined. Qualifying it with the manager's
    // synchronized lock status prevents requests from entering a stale epoch.
    wire dac_domain_available_bus = dac_bus_epoch_valid &&
                                    dac_locked_sync1 && clock_ready_bus &&
                                    !clock_program_busy_bus;
    wire dac_done_event_bus = dac_domain_available_bus &&
                              (dac_done_sync1 ^ dac_done_sync2);
    wire dac_prime_done_event_bus = dac_domain_available_bus &&
                                   (dac_prime_done_sync1 ^ dac_prime_done_sync2);
    wire dac_prime_fault_event_bus = dac_domain_available_bus &&
                                     (dac_prime_fault_sync1 ^ dac_prime_fault_sync2);
    wire dac_zero_done_event_bus = dac_domain_available_bus &&
                                   (dac_zero_done_sync1 ^ dac_zero_done_sync2);
    wire dac_stop_done_event_bus = dac_domain_available_bus &&
                                   (dac_stop_done_sync1 ^ dac_stop_done_sync2);
    wire dac_unserved_event_bus = dac_domain_available_bus &&
                                  dac_unserved_sync1 && !dac_unserved_sync2;
    wire dac_invalid_event_bus = dac_domain_available_bus &&
                                 dac_invalid_sync1 && !dac_invalid_sync2;
    wire dac_timing_late_event_bus = dac_domain_available_bus &&
                                     dac_late_sync1 && !dac_late_sync2;

    assign config_dac = dac_cfg_sync1;
    assign arm_ack_toggle_bus = arm_ack_sync1;
    assign trigger_ack_toggle_bus = trigger_ack_sync1;
    assign trigger_pulse_dac = dac_trigger_pulse;
    assign prime_pulse_dac = dac_prime_pulse;
    assign stop_pulse_dac = dac_stop_pulse;
    assign clear_diagnostics_pulse_dac = dac_clear_diagnostics_pulse;
    assign status_bus = stable_status_bus;
    assign domain_available_bus = dac_domain_available_bus;
    assign clock_locked_bus = dac_locked_sync1;
    assign transaction_active_bus = transaction_active_sync1;
    assign busy_bus = dac_busy_sync1;
    assign done_event_bus = dac_done_event_bus;
    assign prime_done_event_bus = dac_prime_done_event_bus;
    assign prime_fault_event_bus = dac_prime_fault_event_bus;
    assign zero_done_event_bus = dac_zero_done_event_bus;
    assign stop_done_event_bus = dac_stop_done_event_bus;
    assign unserved_event_bus = dac_unserved_event_bus;
    assign invalid_event_bus = dac_invalid_event_bus;
    assign timing_late_event_bus = dac_timing_late_event_bus;
endmodule

`default_nettype wire
