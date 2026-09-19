#include "../headstage/headstage_internal.h"
#include "../drivers/compute_control.h"
#include "../drivers/ripple_detector_service.h"

static void publish_control_snapshot(void);
static uint32_t command_status_word(void);
static int previous_stream_drained(void);

nclp_operation_context_t nclp_operation = {
    .streaming = 0U,
    .stream_aux_mode = NCLP_STREAM_AUX_INPUTS,
    .operation_state = NCLP_STATE_IDLE,
    .init_rhd_reg1_value = 0x42U,
    .init_rhd_reg2_value = 0x04U,
};

nclp_diagnostics_context_t nclp_diagnostics = {
    .fail_count = 0U,
};


#define NCLP_STREAM_DELIVERY_SFP 2U
static uint32_t g_stream_delivery = NCLP_STREAM_DELIVERY_LOCAL;
static uint32_t g_stream_blocks_released = 0U;
static uint32_t g_stream_output_grace_polls = 0U;
static uint32_t g_stream_session_id = 0U;
static uint32_t g_stream_layout_id = 1U;
static uint32_t g_scan_valid = 0U;
static uint32_t g_init_valid = 0U;
static uint32_t g_impedance_valid = 0U;
static uint32_t g_scan_lane_phases_a_30k = 0U;
static uint32_t g_scan_lane_phases_b_30k = 0U;
static uint32_t g_scan_lane_phases_30k_valid = 0U;
static uint32_t g_config_generation = 1U;
static uint32_t g_operation_generation = 0U;
static uint32_t g_active_operation_result_id = 0U;
static uint32_t g_cancel_latched_token = 0U;
static uint32_t g_cancel_poll_suppressed = 0U;

#define INIT_ANALOG_UPPER_CUTOFF_HZ 7500U
#define INIT_ANALOG_LOWER_CUTOFF_MILLIHZ 1000U

static void reset_detected_state(void)
{
    nclp_headstage.best_lane_phases_a = 0U;
    nclp_headstage.best_lane_phases_b = 0U;
    for (uint32_t stream = 0U; stream < SPI_SCAN_STREAMS; ++stream) {
        nclp_headstage.best_stream_score[stream] = 0U;
        nclp_headstage.best_stream_phase[stream] = 0U;
        nclp_headstage.best_stream_chip_id[stream] = 0U;
        nclp_headstage.best_stream_num_amps[stream] = 0U;
        nclp_headstage.best_stream_reg59[stream] = 0U;
    }
}


typedef struct {
    uint32_t lane_phases_a;
    uint32_t lane_phases_b;
    uint32_t stream_score[SPI_SCAN_STREAMS];
    uint32_t stream_phase[SPI_SCAN_STREAMS];
    uint8_t chip_id[SPI_SCAN_STREAMS];
    uint8_t num_amps[SPI_SCAN_STREAMS];
    uint8_t reg59[SPI_SCAN_STREAMS];
    uint8_t selected_mask;
} detection_snapshot_t;

typedef enum {
    NCLP_SCAN_OUTCOME_HARDWARE_ERROR = -1,
    NCLP_SCAN_OUTCOME_OK = 0,
    NCLP_SCAN_OUTCOME_NO_CHIP = 1
} nclp_scan_outcome_t;

static void capture_detection_snapshot(detection_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return;
    }
    snapshot->lane_phases_a = nclp_headstage.best_lane_phases_a;
    snapshot->lane_phases_b = nclp_headstage.best_lane_phases_b;
    for (uint32_t stream = 0U; stream < SPI_SCAN_STREAMS; ++stream) {
        snapshot->stream_score[stream] = nclp_headstage.best_stream_score[stream];
        snapshot->stream_phase[stream] = nclp_headstage.best_stream_phase[stream];
        snapshot->chip_id[stream] = nclp_headstage.best_stream_chip_id[stream];
        snapshot->num_amps[stream] = nclp_headstage.best_stream_num_amps[stream];
        snapshot->reg59[stream] = nclp_headstage.best_stream_reg59[stream];
    }
    snapshot->selected_mask = selected_stream_mask();
}

static void restore_detection_snapshot(const detection_snapshot_t *snapshot)
{
    if (snapshot == NULL) {
        return;
    }
    nclp_headstage.best_lane_phases_a = snapshot->lane_phases_a;
    nclp_headstage.best_lane_phases_b = snapshot->lane_phases_b;
    for (uint32_t stream = 0U; stream < SPI_SCAN_STREAMS; ++stream) {
        nclp_headstage.best_stream_score[stream] = snapshot->stream_score[stream];
        nclp_headstage.best_stream_phase[stream] = snapshot->stream_phase[stream];
        nclp_headstage.best_stream_chip_id[stream] = snapshot->chip_id[stream];
        nclp_headstage.best_stream_num_amps[stream] = snapshot->num_amps[stream];
        nclp_headstage.best_stream_reg59[stream] = snapshot->reg59[stream];
    }
}

static int detection_topology_matches(const detection_snapshot_t *reference)
{
    uint8_t current_mask;

    if (reference == NULL || reference->selected_mask == 0U) {
        return 0;
    }
    current_mask = selected_stream_mask();
    if (current_mask != reference->selected_mask) {
        return 0;
    }
    for (uint32_t stream = 0U; stream < SPI_SCAN_STREAMS; ++stream) {
        if ((current_mask & (uint8_t)(1U << stream)) != 0U &&
            (nclp_headstage.best_stream_chip_id[stream] != reference->chip_id[stream] ||
             nclp_headstage.best_stream_num_amps[stream] != reference->num_amps[stream])) {
            return 0;
        }
    }
    return 1;
}

int  write_lane_phases_verified(uint32_t packed_lane_phases_a,
                                      uint32_t packed_lane_phases_b)
{
    uint32_t readback_a = 0U;
    uint32_t readback_b = 0U;

    intan_register_write(INTAN_REG_MISO_PHASE_PRIMARY,
                         packed_lane_phases_a);
    intan_register_write(INTAN_REG_MISO_PHASE_SECONDARY,
                         packed_lane_phases_b);
    if (intan_reg_read(INTAN_REG_MISO_PHASE_PRIMARY, &readback_a) != 0 ||
        intan_reg_read(INTAN_REG_MISO_PHASE_SECONDARY, &readback_b) != 0 ||
        readback_a != packed_lane_phases_a ||
        readback_b != packed_lane_phases_b) {
        xil_printf("  FAIL MISO phases primary=0x%08lx expected=0x%08lx secondary=0x%08lx expected=0x%08lx\r\n",
                   (unsigned long)readback_a,
                   (unsigned long)packed_lane_phases_a,
                   (unsigned long)readback_b,
                   (unsigned long)packed_lane_phases_b);
        nclp_diagnostics.fail_count++;
        return -1;
    }
    return 0;
}

/* A scan always establishes verified 30 kS/s A/B phases for impedance. If the
 * user selected a lower acquisition rate, scan once more at that exact rate
 * and retain those phases for normal INIT/streaming. */
static nclp_scan_outcome_t run_configured_phase_scan(void)
{
    nclp_acquisition_config_t config;
    detection_snapshot_t scan_30k;
    nclp_scan_outcome_t outcome = NCLP_SCAN_OUTCOME_HARDWARE_ERROR;
    uint32_t scan_failures_before;

    acquisition_config_get_copy(&config);
    if (acquisition_config_disable_ttl_hardware() != 0 ||
        acquisition_config_program_rate_hardware(30000U) != 0) {
        return NCLP_SCAN_OUTCOME_HARDWARE_ERROR;
    }

    reset_detected_state();
    nclp_headstage.scan_progress_base = 0U;
    scan_failures_before = nclp_diagnostics.fail_count;
    if (program_phase_scan_aux_bank(0U) != 0 || run_phase_scan() != 0 ||
        nclp_diagnostics.fail_count != scan_failures_before) {
        goto restore_selected_rate;
    }
    capture_detection_snapshot(&scan_30k);
    if (scan_30k.selected_mask == 0U) {
        /* The Intan engine and DDR writer completed a valid scan, but no
         * supported RHD identity passed selection.  This is an expected
         * negative discovery result, not a hardware fault. */
        xil_printf("  INFO no supported RHD chip detected at 30 kS/s\r\n");
        outcome = NCLP_SCAN_OUTCOME_NO_CHIP;
        goto restore_selected_rate;
    }
    g_scan_lane_phases_a_30k = scan_30k.lane_phases_a;
    g_scan_lane_phases_b_30k = scan_30k.lane_phases_b;
    g_scan_lane_phases_30k_valid = 1U;

    if (config.sample_rate_hz == 30000U) {
        outcome = NCLP_SCAN_OUTCOME_OK;
        goto restore_selected_rate;
    }

    if (acquisition_config_program_rate_hardware(config.sample_rate_hz) != 0) {
        goto restore_selected_rate;
    }
    reset_detected_state();
    nclp_headstage.scan_progress_base = SPI_SCAN_PHASE_STEPS;
    scan_failures_before = nclp_diagnostics.fail_count;
    if (program_phase_scan_aux_bank(0U) != 0 || run_phase_scan() != 0 ||
        nclp_diagnostics.fail_count != scan_failures_before) {
        goto restore_selected_rate;
    }
    if (detection_topology_matches(&scan_30k) == 0) {
        xil_printf("  FAIL headstage topology changed between 30 kS/s and %lu S/s\r\n",
                   (unsigned long)config.sample_rate_hz);
        nclp_diagnostics.fail_count++;
        goto restore_selected_rate;
    }
    outcome = NCLP_SCAN_OUTCOME_OK;

restore_selected_rate:
    if (config.sample_rate_hz != 30000U &&
        acquisition_config_program_rate_hardware(config.sample_rate_hz) != 0) {
        /* A failed rate restore is a real hardware failure and must override
         * an otherwise benign no-chip discovery. */
        outcome = NCLP_SCAN_OUTCOME_HARDWARE_ERROR;
    }
    if (outcome != NCLP_SCAN_OUTCOME_OK) {
        g_scan_lane_phases_30k_valid = 0U;
        reset_detected_state();
    } else if (config.sample_rate_hz == 30000U) {
        restore_detection_snapshot(&scan_30k);
    }
    nclp_headstage.scan_progress_base = 0U;
    return outcome;
}

static uint32_t selected_physical_chip_count(void)
{
    uint32_t count = 0U;
    uint8_t mask = selected_stream_mask();

    for (uint32_t stream = 0U; stream < SPI_SCAN_STREAMS; ++stream) {
        if ((mask & (uint8_t)(1U << stream)) != 0U) {
            count++;
        }
    }
    return count;
}

static uint32_t selected_physical_channel_count(void)
{
    uint32_t count = 0U;
    uint8_t mask = selected_stream_mask();

    for (uint32_t stream = 0U; stream < SPI_SCAN_STREAMS; ++stream) {
        if ((mask & (uint8_t)(1U << stream)) != 0U) {
            count += chip_channel_count(nclp_headstage.best_stream_chip_id[stream]);
        }
    }
    return count;
}

static int stage_scan_result_records(void)
{
    uint8_t detected_mask = selected_stream_mask();

    for (uint32_t stream = 0U; stream < SPI_SCAN_STREAMS; ++stream) {
        nclp_result_record_t record = {0};
        uint32_t channels = chip_channel_count(nclp_headstage.best_stream_chip_id[stream]);
        uint32_t detected =
            (detected_mask & (uint8_t)(1U << stream)) != 0U;
        uint32_t phase_b = packed_lane_phase_get(nclp_headstage.best_lane_phases_b,
                                                  stream);

        record.scan.info = (stream & 0x7U) |
                           ((detected & 0x1U) << 3) |
                           (((channels != 0U) ? 1U : 0U) << 4) |
                           (((channels == nclp_headstage.best_stream_num_amps[stream]) ?
                             1U : 0U) << 5) |
                           ((uint32_t)nclp_headstage.best_stream_chip_id[stream] << 8) |
                           ((uint32_t)nclp_headstage.best_stream_num_amps[stream] << 16) |
                           ((uint32_t)nclp_headstage.best_stream_reg59[stream] << 24);
        record.scan.phase = nclp_scan_result_phase(
            nclp_headstage.best_stream_phase[stream], phase_b,
            nclp_headstage.best_stream_chip_id[stream] == 4U);
        record.scan.score = nclp_headstage.best_stream_score[stream];
        record.scan.logical_mask = detected ? primary_stream_mask(stream) : 0U;
        if (nclp_result_write_record(NCLP_RESULT_TYPE_SCAN, stream,
                                     &record) != 0) {
            return -1;
        }
    }
    return 0;
}

static void shared_barrier(void)
{
    __asm__ volatile("dmb sy" ::: "memory");
}

static void shared_flush(void)
{
    shared_barrier();
    Xil_DCacheFlushRange((INTPTR)nclp_shared_state(),
                         sizeof(nclp_shared_state_t));
    shared_barrier();
}

static void shared_invalidate(void)
{
    Xil_DCacheInvalidateRange((INTPTR)nclp_shared_state(),
                              sizeof(nclp_shared_state_t));
    shared_barrier();
}

static void publish_config_snapshot(void)
{
    volatile nclp_shared_state_t *shared = nclp_shared_state();
    const nclp_acquisition_config_t *config = acquisition_config_get();
    uint32_t sequence;
    uint32_t flags = 0U;

    if (g_init_valid != 0U) {
        flags |= NCLP_CONFIG_FLAG_INITIALIZED;
    }
    if (config->dsp_enabled != 0U) {
        flags |= NCLP_CONFIG_FLAG_DSP_ENABLED;
    }
    if (config->ttl_fast_settle_enabled != 0U) {
        flags |= NCLP_CONFIG_FLAG_TTL_SETTLE_ENABLED;
    }

    shared_invalidate();
    sequence = shared->config.publish_seq;
    if ((sequence & 1U) != 0U) {
        sequence++;
    }
    shared->config.publish_seq = sequence + 1U;
    shared_barrier();
    shared->config.generation = g_config_generation;
    shared->config.sample_rate_hz = config->sample_rate_hz;
    shared->config.analog_lower_millihz =
        config->analog_lower_cutoff_millihz;
    shared->config.analog_upper_hz = config->analog_upper_cutoff_hz;
    shared->config.dsp_enable = config->dsp_enabled;
    shared->config.dsp_requested_millihz =
        config->dsp_requested_cutoff_millihz;
    shared->config.dsp_actual_millihz =
        config->dsp_actual_cutoff_millihz;
    shared->config.dsp_code = config->dsp_cutoff_code;
    shared->config.ttl_settle_enable = config->ttl_fast_settle_enabled;
    shared->config.ttl_settle_channel = config->ttl_fast_settle_channel;
    shared->config.flags = flags;
    for (uint32_t i = 0U; i < 4U; ++i) {
        shared->config.reserved[i] = 0U;
    }
    shared_barrier();
    shared->config.publish_seq = sequence + 2U;
    shared_flush();
}

static void bump_config_generation(uint32_t layout_changed)
{
    g_config_generation++;
    if (g_config_generation == 0U) {
        g_config_generation = 1U;
    }
    if (layout_changed != 0U) {
        g_stream_layout_id++;
        if (g_stream_layout_id == 0U) {
            g_stream_layout_id = 1U;
        }
    }
    publish_config_snapshot();
}

static void begin_exclusive_operation(uint32_t result_id)
{
    volatile nclp_shared_state_t *shared = nclp_shared_state();

    g_operation_generation++;
    if (g_operation_generation == 0U) {
        g_operation_generation = 1U;
    }
    g_active_operation_result_id = result_id;
    g_cancel_latched_token = 0U;
    g_cancel_poll_suppressed = 0U;
    shared_invalidate();
    shared->operation_generation = g_operation_generation;
    shared->cancel_ack_status = 0U;
    shared_flush();
}

static int latch_matching_impedance_cancel(void)
{
    volatile nclp_shared_state_t *shared = nclp_shared_state();
    uint32_t token;

    shared_invalidate();
    token = shared->cancel_request_token;
    /* Acquire the target tuple published before cancel_request_token. */
    shared_barrier();
    if (token == 0U || token == shared->cancel_ack_token ||
        shared->cancel_target_operation != NCLP_CMD_IMPEDANCE ||
        shared->cancel_target_result_id != g_active_operation_result_id ||
        shared->cancel_target_generation != g_operation_generation) {
        return 0;
    }

    g_cancel_latched_token = token;
    return 1;
}

int nclp_impedance_cancel_requested(void)
{
    if (g_cancel_poll_suppressed != 0U ||
        (nclp_operation.operation_state != NCLP_STATE_IMPEDANCE &&
         nclp_operation.operation_state != NCLP_STATE_CANCELLING) ||
        latch_matching_impedance_cancel() == 0) {
        return 0;
    }

    if (nclp_operation.operation_state != NCLP_STATE_CANCELLING) {
        nclp_operation.operation_state = NCLP_STATE_CANCELLING;
        nclp_progress_set_state(NCLP_STATE_CANCELLING, 0U, nclp_diagnostics.fail_count);
        publish_control_snapshot();
        xil_printf("  INFO impedance cancellation requested token=%lu\r\n",
                   (unsigned long)g_cancel_latched_token);
    }
    return 1;
}

void nclp_impedance_cancel_acknowledge(void)
{
    if (g_cancel_poll_suppressed != 0U) {
        return;
    }

    /* Close cancellation admission before mandatory Reg5/Reg7 cleanup and
     * user-configuration restoration.  A53-1 publishes a request before its
     * final state recheck; this final latch therefore captures every CANCEL
     * for which A53-1 can return ACCEPTED. */
    nclp_operation.operation_state = NCLP_STATE_RESTORING;
    nclp_progress_set_state(NCLP_STATE_RESTORING, 0U, nclp_diagnostics.fail_count);
    publish_control_snapshot();
    (void)latch_matching_impedance_cancel();
    g_cancel_poll_suppressed = 1U;
}

static void finish_cancel_ack(uint32_t status)
{
    volatile nclp_shared_state_t *shared = nclp_shared_state();

    if (g_cancel_latched_token != 0U) {
        shared_invalidate();
        shared->cancel_ack_status = status;
        shared_barrier();
        shared->cancel_ack_token = g_cancel_latched_token;
        shared_flush();
    }
    g_cancel_latched_token = 0U;
    g_cancel_poll_suppressed = 0U;
    g_active_operation_result_id = 0U;
}

static int stopped_configuration_allowed(void)
{
    return nclp_operation.streaming == 0U && previous_stream_drained() != 0 &&
           (nclp_operation.operation_state == NCLP_STATE_IDLE ||
            nclp_operation.operation_state == NCLP_STATE_READY);
}

static void enter_global_fault(uint32_t status)
{
    /* Global ownership and physical-output state must agree.  Stop both
     * trigger sources and clean up the DAC before publishing FAULT. */
    nclp_ripple_service_emergency_stop();
    nclp_stim_emergency_stop();
    nclp_operation.operation_state = NCLP_STATE_FAULT;
    nclp_progress_finish(NCLP_STATE_FAULT, status, nclp_diagnostics.fail_count);
}

static uint32_t packed_selected_stream_chip_ids(void)
{
    uint32_t packed = 0U;
    uint8_t mask = selected_stream_mask();

    for (uint32_t stream = 0U; stream < SPI_SCAN_STREAMS; ++stream) {
        if ((mask & (uint8_t)(1U << stream)) != 0U) {
            packed |= ((uint32_t)(nclp_headstage.best_stream_chip_id[stream] & 0xFU)) <<
                      (stream * 4U);
        }
    }
    return packed;
}

static uint32_t count_streams(uint32_t mask)
{
    uint32_t count = 0U;

    while (mask != 0U) {
        count += mask & 1U;
        mask >>= 1U;
    }
    return count;
}

static void publish_control_snapshot(void)
{
    volatile nclp_shared_state_t *shared = nclp_shared_state();
    uint32_t sequence;

    shared_invalidate();
    sequence = shared->control_publish_seq;
    if ((sequence & 1U) != 0U) {
        sequence++;
    }
    shared->control_publish_seq = sequence + 1U;
    shared_flush();
    shared->control_status_word = command_status_word();
    shared->physical_chip_mask = selected_stream_mask();
    shared->stream_mask = selected_logical_stream_mask();
    shared->stream_count = count_streams(shared->stream_mask);
    shared->detected_chip_ids = packed_selected_stream_chip_ids();
    shared->stream_layout_id = g_stream_layout_id;
    shared_barrier();
    shared->control_publish_seq = sequence + 2U;
    shared_flush();
    nclp_led_update(selected_stream_mask(), g_init_valid,
                    nclp_operation.operation_state == NCLP_STATE_FAULT ? 1U : 0U);
}

static uint32_t command_status_word(void)
{
    return ((nclp_operation.streaming & 0x1U) << 0) |
           (((g_stream_delivery == NCLP_STREAM_DELIVERY_UDP) ? 1U : 0U) << 1) |
           ((nclp_operation.stream_aux_mode & 0x3U) << 2) |
           ((nclp_compute_sfp_mode_selected() & 0x1U) << 4) |
           ((nclp_operation.operation_state & 0xFU) << 8) |
           ((g_scan_valid & 0x1U) << 12) |
           ((g_init_valid & 0x1U) << 13) |
           ((g_impedance_valid & 0x1U) << 14);
}

static int shared_stream_stop(void)
{
    volatile nclp_shared_state_t *shared = nclp_shared_state();
    uint32_t produced = 0U;
    uint32_t final_bytes = 0U;
    uint32_t terminal_error = 0U;
    uint32_t sequence;
    uint64_t committed_bytes = 0U;
    int terminal_result;

    shared_invalidate();
    if (shared->stream_active == 0U) {
        return 0;
    }
    terminal_result = ddrw_check_terminal(shared->block_bytes,
                                           &committed_bytes,
                                           &produced,
                                           &final_bytes,
                                           &terminal_error);
    shared_invalidate();
    sequence = shared->stream_publish_seq;
    if ((sequence & 1U) != 0U) {
        sequence++;
    }
    shared->stream_publish_seq = sequence + 1U;
    shared_flush();
    if (shared->stream_active != 0U) {
        shared->produced_blocks = produced;
        shared->final_block_bytes = final_bytes;
        if (terminal_result != 0) {
            shared->stream_error = terminal_error != 0U ? terminal_error :
                                   NCLP_STREAM_ERROR_DRAIN_TIMEOUT;
        }
    }
    shared->stream_active = 0U;
    shared_barrier();
    shared->stream_publish_seq = sequence + 2U;
    shared_flush();
    (void)committed_bytes;
    return terminal_result;
}

static void shared_stream_clear_after_reset(void)
{
    volatile nclp_shared_state_t *shared = nclp_shared_state();
    uint32_t sequence;

    shared_invalidate();
    sequence = shared->stream_publish_seq;
    if ((sequence & 1U) != 0U) {
        sequence++;
    }
    shared->stream_publish_seq = sequence + 1U;
    shared_flush();
    /* RESET is admitted only after the previous UDP session drained.  Keep
     * its session ID for monotonic uniqueness, but remove stale error/mode
     * state so STATUS describes the recovered control path. */
    shared->stream_active = 0U;
    shared->stream_error = NCLP_STREAM_ERROR_NONE;
    shared->stream_aux_mode = NCLP_STREAM_AUX_INPUTS;
    shared->final_block_bytes = 0U;
    shared_barrier();
    shared->stream_publish_seq = sequence + 2U;
    shared_flush();
}

static int previous_stream_drained(void)
{
    volatile nclp_shared_state_t *shared = nclp_shared_state();

    if (g_stream_session_id == 0U) {
        return 1;
    }
    shared_invalidate();
    return shared->network_drained_session == g_stream_session_id;
}

static void shared_stream_force_abort(uint32_t error_code)
{
    volatile nclp_shared_state_t *shared = nclp_shared_state();
    uint32_t produced = 0U;
    uint32_t last_bytes = 0U;
    uint32_t ddr_abort_failed = 0U;
    uint32_t compute_abort_failed = 0U;
    uint32_t abort_failed;
    uint32_t sequence;
    uint32_t had_active_stream;
    uint32_t shared_stream_was_active;

    /* Revoke STIM immediately.  The Intan/DDRW cleanup below can block while
     * its timeout expires, and no physical output may continue during that
     * recovery interval. */
    nclp_ripple_service_emergency_stop();
    nclp_stim_emergency_stop();
    shared_invalidate();
    shared_stream_was_active = shared->stream_active;
    had_active_stream =
        shared_stream_was_active != 0U && shared->stream_session_id != 0U;

    (void)acquisition_config_disable_ttl_hardware();
    if (g_stream_delivery != NCLP_STREAM_DELIVERY_SFP) {
        ddr_abort_failed =
            ddrw_abort_active_session("DDRW forced abort") != 0;
        if (had_active_stream != 0U) {
            (void)ddr_read_produced_snapshot(&produced, &last_bytes);
        }
    }

    /* Both local and SFP acquisition use the compute stream. Retire its
     * session only after the producer, and the local DDR writer when present,
     * can no longer own an AXIS beat. */
    if (ddr_abort_failed == 0U) {
        compute_abort_failed = abort_intan_compute_stream() != 0;
    }
    abort_failed = ddr_abort_failed | compute_abort_failed;
    if (abort_failed != 0U) {
        /* A timed-out source, writer, decoder, or SFP drain still owns this
         * session. Keep the control path busy until RESET can retry. */
        nclp_operation.streaming = 1U;
    } else {
        nclp_operation.streaming = 0U;
        g_stream_output_grace_polls = 0U;
    }

    if (g_stream_delivery == NCLP_STREAM_DELIVERY_SFP) {
        shared_invalidate();
        shared->stream_error = abort_failed != 0U ?
            NCLP_STREAM_ERROR_FORCED_ABORT : error_code;
        shared_flush();
        return;
    }

    shared_invalidate();
    sequence = shared->stream_publish_seq;
    if ((sequence & 1U) != 0U) {
        sequence++;
    }
    shared->stream_publish_seq = sequence + 1U;
    shared_flush();
    if (had_active_stream != 0U) {
        shared->produced_blocks = produced;
        shared->final_block_bytes = (produced != 0U) ?
            ((last_bytes != 0U && last_bytes <= shared->block_bytes) ?
             last_bytes : shared->block_bytes) : 0U;
    }
    shared->stream_error = error_code != 0U ? error_code :
                           NCLP_STREAM_ERROR_FORCED_ABORT;
    if (abort_failed != 0U) {
        shared->stream_error = NCLP_STREAM_ERROR_FORCED_ABORT;
    }
    /* A retired DDR writer makes its exact final block safe for A53-1 even if
     * compute retirement needs a RESET retry.  Only an unsafe writer keeps
     * the shared recording session active. */
    shared->stream_active = ddr_abort_failed != 0U ?
        shared_stream_was_active : 0U;
    shared_barrier();
    shared->stream_publish_seq = sequence + 2U;
    shared_flush();
}

static void shared_stream_start(uint32_t stream_mask,
                                uint32_t physical_chip_mask,
                                uint32_t aux_mode)
{
    volatile nclp_shared_state_t *shared = nclp_shared_state();
    uintptr_t ring = (uintptr_t)nclp_capture.capture_ring_words;
    uint32_t new_session = g_stream_session_id + 1U;
    uint32_t sequence;

    if (new_session == 0U) {
        new_session = 1U;
    }

    shared_invalidate();
    sequence = shared->stream_publish_seq;
    if ((sequence & 1U) != 0U) {
        sequence++;
    }
    shared->stream_publish_seq = sequence + 1U;
    shared_flush();
    shared->stream_session_id = 0U;
    shared->ring_address_low = (uint32_t)ring;
    shared->ring_address_high = (uint32_t)(((uint64_t)ring) >> 32);
    shared->ring_bytes = nclp_capture.capture_ring_bytes;
    shared->block_bytes = SPI_CAPTURE_BLOCK_BYTES;
    shared->produced_blocks = 0U;
    shared->consumed_blocks = 0U;
    shared->network_abandoned_blocks = 0U;
    shared->stream_error = NCLP_STREAM_ERROR_NONE;
    shared->final_block_bytes = 0U;
    shared->stream_mask = stream_mask;
    shared->stream_count = count_streams(stream_mask);
    shared->stream_layout_id = g_stream_layout_id;
    shared->stream_aux_mode = aux_mode;
    shared->physical_chip_mask = physical_chip_mask;
    shared->detected_chip_ids = packed_selected_stream_chip_ids();
    shared->network_drained_session = 0U;
    shared->stream_active = 1U;
    g_stream_blocks_released = 0U;
    g_stream_output_grace_polls = STREAM_OUTPUT_START_GRACE_POLLS;
    shared->stream_session_id = new_session;
    g_stream_session_id = new_session;
    shared_barrier();
    shared->stream_publish_seq = sequence + 2U;
    shared_flush();
}

static int ddrw_release_consumed_blocks(uint32_t consumed)
{
    uint32_t readback;

    if (g_stream_blocks_released == consumed) {
        return 0;
    }
    reg_write(SPI_DDR_WRITER_BASE, DDRW_REG_CONSUMED_BLOCK_COUNT, consumed);
    readback = reg_read(SPI_DDR_WRITER_BASE,
                        DDRW_REG_CONSUMED_BLOCK_COUNT);
    if (readback != consumed) {
        xil_printf("  FAIL DDRW consumer doorbell requested=%lu readback=%lu produced=%lu outstanding=%lu\r\n",
                   (unsigned long)consumed,
                   (unsigned long)readback,
                   (unsigned long)reg_read(SPI_DDR_WRITER_BASE,
                                            DDRW_REG_PRODUCED_BLOCK_COUNT),
                   (unsigned long)reg_read(SPI_DDR_WRITER_BASE,
                                            DDRW_REG_OUTSTANDING_BLOCK_COUNT));
        nclp_diagnostics.fail_count++;
        return -1;
    }
    g_stream_blocks_released = consumed;
    return 0;
}

static void service_stream_blocks(void)
{
    volatile nclp_shared_state_t *shared = nclp_shared_state();
    uint32_t produced;
    uint32_t consumed;
    uint32_t status;
    uint32_t writer_state;
    uint32_t acquisition_status;
    uint32_t stream_status;
    uint32_t intan_error_status;
    uint32_t unaccepted_source_events;
    uint32_t network_error;
    uint32_t hardware_error = 0U;

    if (nclp_operation.streaming != 0U &&
        nclp_operation.operation_state == NCLP_STATE_FAULT) {
        return;
    }
    if (nclp_operation.streaming != 0U &&
        g_stream_delivery == NCLP_STREAM_DELIVERY_SFP) {
        acquisition_status = reg_read(INTAN_BASE, INTAN_REG_ACQUISITION_STATUS);
        intan_error_status = reg_read(INTAN_BASE, INTAN_REG_ERROR_STATUS) & INTAN_ERROR_IMPLEMENTED_MASK;
        unaccepted_source_events = reg_read(INTAN_BASE, INTAN_REG_UNACCEPTED_SOURCE_EVENT_COUNT);
        if ((acquisition_status & INTAN_ACQUISITION_STATUS_RUNNING) == 0U ||
            intan_error_status != 0U || unaccepted_source_events != 0U) {
            shared_stream_force_abort(NCLP_STREAM_ERROR_UNEXPECTED_STOP);
            enter_global_fault(1U);
            publish_control_snapshot();
        }
        return;
    }
    if (nclp_operation.streaming == 0U) {
        if (nclp_compute_sfp_mode_selected() != 0U) {
            return;
        }
        shared_invalidate();
        consumed = shared->consumed_blocks;
        if (ddrw_release_consumed_blocks(consumed) != 0) {
            shared->stream_error = NCLP_STREAM_ERROR_FORCED_ABORT;
            shared_flush();
        }
        if (nclp_operation.operation_state == NCLP_STATE_DRAINING &&
            previous_stream_drained() != 0) {
            uint32_t drain_error;

            shared_invalidate();
            drain_error = shared->stream_error;
            if (drain_error != 0U) {
                nclp_diagnostics.fail_count++;
                enter_global_fault(drain_error);
                xil_printf("  FAIL UDP drain error=0x%08lx\r\n",
                           (unsigned long)drain_error);
            } else {
                nclp_operation.operation_state = NCLP_STATE_READY;
                nclp_progress_finish(NCLP_STATE_READY, 0U, nclp_diagnostics.fail_count);
            }
            publish_control_snapshot();
        }
        return;
    }

    produced = reg_read(SPI_DDR_WRITER_BASE,
                        DDRW_REG_PRODUCED_BLOCK_COUNT);
    status = reg_read(SPI_DDR_WRITER_BASE, DDRW_REG_STATUS);
    writer_state = reg_read(SPI_DDR_WRITER_BASE,
                            DDRW_REG_SESSION_STATE);
    acquisition_status = reg_read(INTAN_BASE,
                                  INTAN_REG_ACQUISITION_STATUS);
    stream_status = reg_read(INTAN_BASE, INTAN_REG_OUTPUT_STATUS);
    intan_error_status = reg_read(INTAN_BASE, INTAN_REG_ERROR_STATUS) &
                         INTAN_ERROR_IMPLEMENTED_MASK;
    unaccepted_source_events = reg_read(
        INTAN_BASE, INTAN_REG_UNACCEPTED_SOURCE_EVENT_COUNT);
    if ((stream_status & INTAN_OUTPUT_STATUS_RECORDING_ACTIVE) != 0U) {
        g_stream_output_grace_polls = 0U;
    }
    shared_invalidate();
    consumed = shared->consumed_blocks;
    network_error = shared->stream_error;
    shared->produced_blocks = produced;
    shared->control_heartbeat++;
    if ((status & DDRW_STATUS_ERROR_MASK) != 0U) {
        hardware_error = status;
    } else if (intan_error_status != 0U) {
        hardware_error = 0x80000000U | intan_error_status;
    } else if ((stream_status & INTAN_OUTPUT_STATUS_ERROR_MASK) != 0U ||
               unaccepted_source_events != 0U) {
        hardware_error = 0x80000000U | stream_status;
    } else if ((acquisition_status &
                INTAN_ACQUISITION_STATUS_RUNNING) == 0U ||
               (status & DDRW_STATUS_RUNNING) == 0U ||
               (status & (DDRW_STATUS_EOS_SEEN |
                          DDRW_STATUS_EOS_COMMITTED |
                          DDRW_STATUS_ABORT_DONE)) != 0U ||
               ((stream_status & INTAN_OUTPUT_STATUS_RECORDING_ACTIVE) == 0U &&
                g_stream_output_grace_polls == 0U) ||
               (writer_state != DDRW_STATE_CAPTURING &&
                writer_state != DDRW_STATE_FULL_WAIT)) {
        /* START is positively acknowledged before nclp_operation.streaming is set, so a
         * later loss of either RUNNING indication (or entry into an EOS/
         * terminal writer state) is an unexpected end of a continuous
         * session, even when no sticky hardware error bit was raised. */
        hardware_error = NCLP_STREAM_ERROR_UNEXPECTED_STOP;
    }
    if ((stream_status & INTAN_OUTPUT_STATUS_RECORDING_ACTIVE) == 0U &&
        g_stream_output_grace_polls != 0U) {
        g_stream_output_grace_polls--;
    }
    if (hardware_error != 0U) {
        shared->stream_error = hardware_error;
    }
    shared_flush();

    if (ddrw_release_consumed_blocks(consumed) != 0 &&
        hardware_error == 0U) {
        hardware_error = NCLP_STREAM_ERROR_FORCED_ABORT;
    }
    if ((status & DDRW_STATUS_IRQ_PENDING) != 0U) {
        reg_write(SPI_DDR_WRITER_BASE, DDRW_REG_COMMAND,
                  DDRW_COMMAND_IRQ_ACK);
    }
    if (hardware_error != 0U || network_error != 0U) {
        uint32_t error_code = hardware_error != 0U ?
                              hardware_error : network_error;
        xil_printf("  FAIL stream aborted writer_state=%lu writer_status=0x%08lx acquisition_status=0x%08lx output_status=0x%08lx intan_error=0x%08lx unaccepted_source_events=%lu network=0x%08lx\r\n",
                   (unsigned long)writer_state,
                   (unsigned long)status,
                   (unsigned long)acquisition_status,
                   (unsigned long)stream_status,
                   (unsigned long)intan_error_status,
                   (unsigned long)unaccepted_source_events,
                   (unsigned long)network_error);
        shared_stream_force_abort(error_code);
        enter_global_fault(1U);
        publish_control_snapshot();
    }
}

static int run_impedance_transaction(void)
{
    nclp_acquisition_config_t user_config;
    nclp_acquisition_config_t impedance_config;
    uint32_t user_lane_phases_a = nclp_headstage.best_lane_phases_a;
    uint32_t user_lane_phases_b = nclp_headstage.best_lane_phases_b;
    int measurement_status = NCLP_IMPEDANCE_ERROR;
    int restore_status = 0;

    if (g_scan_lane_phases_30k_valid == 0U) {
        xil_printf("  FAIL impedance requires verified 30 kS/s scan phases\r\n");
        return NCLP_IMPEDANCE_ERROR;
    }

    acquisition_config_get_copy(&user_config);
    impedance_config = user_config;
    impedance_config.sample_rate_hz = 30000U;
    impedance_config.analog_upper_cutoff_hz =
        INIT_ANALOG_UPPER_CUTOFF_HZ;
    impedance_config.analog_lower_cutoff_millihz =
        INIT_ANALOG_LOWER_CUTOFF_MILLIHZ;
    impedance_config.dsp_enabled = 0U;
    impedance_config.dsp_requested_cutoff_millihz = 1000U;
    impedance_config.ttl_fast_settle_enabled = 0U;
    impedance_config.ttl_fast_settle_channel = 0U;
    if (acquisition_config_refresh_derived(&impedance_config) != 0 ||
        acquisition_config_validate(&impedance_config) != 0) {
        return NCLP_IMPEDANCE_ERROR;
    }

    xil_printf("  INFO impedance temporary config rate=30000 upper=7500Hz lower=1Hz DSP=off\r\n");
    nclp_headstage.best_lane_phases_a = g_scan_lane_phases_a_30k;
    nclp_headstage.best_lane_phases_b = g_scan_lane_phases_b_30k;
    if (acquisition_config_apply_values_no_results(&impedance_config) != 0 ||
        write_lane_phases_verified(g_scan_lane_phases_a_30k,
                                   g_scan_lane_phases_b_30k) != 0) {
        measurement_status = nclp_impedance_cancel_requested() != 0 ?
                             NCLP_IMPEDANCE_CANCELLED :
                             NCLP_IMPEDANCE_ERROR;
        nclp_impedance_cancel_acknowledge();
        goto restore_user_config;
    }

    measurement_status = run_impedance_test_all();
    nclp_impedance_cancel_acknowledge();
    if (measurement_status == NCLP_IMPEDANCE_OK &&
        g_cancel_latched_token != 0U) {
        measurement_status = NCLP_IMPEDANCE_CANCELLED;
    }

restore_user_config:
    nclp_headstage.best_lane_phases_a = user_lane_phases_a;
    nclp_headstage.best_lane_phases_b = user_lane_phases_b;
    if (acquisition_config_apply_values_no_results(&user_config) != 0) {
        restore_status = -1;
    }
    if (write_lane_phases_verified(user_lane_phases_a,
                                   user_lane_phases_b) != 0) {
        restore_status = -1;
    }
    if (restore_status != 0) {
        xil_printf("  FAIL impedance configuration restore\r\n");
        return NCLP_IMPEDANCE_ERROR;
    }
    xil_printf("  PASS impedance config restored rate=%lu DSP=%s\r\n",
               (unsigned long)user_config.sample_rate_hz,
               user_config.dsp_enabled != 0U ? "on" : "off");
    return measurement_status;
}

static int reset_whole_control_path(void)
{
    uint32_t sync_words[4];

    nclp_ripple_service_emergency_stop();
    nclp_stim_emergency_stop();
    (void)shared_stream_stop();
    /* RESET also recovers a software-inactive session that stopped without an
     * EOS. Quiesce the shared producer/DDRW boundary before discarding any
     * partial local-decoder or SFP packet state. */
    if (ddrw_abort_active_session("DDRW reset compute-session quiesce") != 0) {
        nclp_operation.streaming = 1U;
        return -1;
    }
    if (abort_intan_compute_stream() != 0) {
        nclp_operation.streaming = 1U;
        return -1;
    }
    /* Recover a mailbox that was unavailable during boot without resetting
     * unrelated compute routing, masks, or diagnostics. A still-invalid SFP
     * mailbox leaves the local Ethernet path eligible. */
    (void)nclp_compute_refresh_sfp_mailbox_identity();
    g_scan_valid = 0U;
    g_init_valid = 0U;
    g_impedance_valid = 0U;
    g_scan_lane_phases_30k_valid = 0U;
    reset_detected_state();
    /* Acquisition RESET is also the system-level recovery command.  Never
     * leave the ripple detector or an independently running TTL/DAC stimulus
     * behind when the Intan and DDR paths return to their defaults. */
    if (nclp_output_init() != 0) {
        return -1;
    }
    /* Re-run identity verification as part of RESET so a transient startup
     * access failure can recover without requiring a new PS boot. */
    if (nclp_ripple_service_init() != NCLP_RIPPLE_DETECTOR_OK) {
        return -1;
    }
    if (nclp_intan_sync_set(NCLP_SYNC_MODE_OFF, 0U, 0U,
                              sync_words) != NCLP_INTAN_SYNC_OK) {
        return -1;
    }
    if (reset_board_path() != 0) {
        return -1;
    }
    shared_stream_clear_after_reset();
    acquisition_config_reset_defaults();
    g_stream_delivery = NCLP_STREAM_DELIVERY_LOCAL;
    nclp_operation.stream_aux_mode = NCLP_STREAM_AUX_INPUTS;
    nclp_operation.operation_state = NCLP_STATE_IDLE;
    bump_config_generation(1U);
    return 0;
}


int nclp_main_execute_command(const nclp_main_command_t *command,
                              nclp_main_reply_t *reply)
{
    uint32_t stream;

    if (command == NULL || reply == NULL) {
        return -1;
    }
    reply->magic = NCLP_CMD_MAGIC;
    reply->version = NCLP_CMD_VERSION;
    reply->command = command->command;
    reply->sequence = command->sequence;
    reply->status = NCLP_COMMAND_STATUS_OK;
    reply->data0 = 0U;
    reply->data1 = 0U;
    reply->data2 = 0U;
    reply->data3 = 0U;

    switch (command->command) {
    case NCLP_CMD_GET_COMPUTE_STATUS: {
        uint32_t words[4];
        uint32_t invalid = 0U;
        for (uint32_t i = 0U; i < NCLP_CMD_WORDS - 4U; ++i) invalid |= command->args[i];
        if (invalid != 0U) { reply->status = NCLP_COMMAND_STATUS_ARGUMENT; break; }
        nclp_compute_read_status(words);
        reply->data0 = words[0]; reply->data1 = words[1];
        reply->data2 = words[2]; reply->data3 = words[3];
        break;
    }
    case NCLP_CMD_PING: {
        reply->data0 = 0x4F4B554EU;
        reply->data1 = NCLP_PING_PRODUCT_ID;
        reply->data2 = NCLP_PING_SOURCE_VERSION;
        reply->data3 = NCLP_CMD_VERSION;
        break;
    }
    case NCLP_CMD_GET_STATUS:
        reply->data0 = (uint32_t)selected_stream_mask();
        reply->data1 = selected_logical_stream_mask();
        reply->data2 = packed_selected_stream_chip_ids();
        reply->data3 = command_status_word() |
                       ((g_stream_layout_id & 0xFFFFU) << 16);
        break;
    case NCLP_CMD_SET_RATE:
    {
        nclp_acquisition_config_t before;
        nclp_acquisition_config_t candidate;
        uint32_t requested_rate = command->args[0];

        if (stopped_configuration_allowed() == 0) {
            reply->status = NCLP_COMMAND_STATUS_BUSY;
            break;
        }
        acquisition_config_get_copy(&before);
        candidate = before;
        candidate.sample_rate_hz = requested_rate;
        if (acquisition_config_refresh_derived(&candidate) != 0 ||
            acquisition_config_validate(&candidate) != 0) {
            reply->status = NCLP_COMMAND_STATUS_ARGUMENT;
            break;
        }
        if (requested_rate != before.sample_rate_hz) {
            if (acquisition_config_set_rate(requested_rate) != 0) {
                reply->status = NCLP_COMMAND_STATUS_HARDWARE;
                enter_global_fault(reply->status);
                break;
            }
            g_scan_valid = 0U;
            g_init_valid = 0U;
            g_impedance_valid = 0U;
            g_scan_lane_phases_30k_valid = 0U;
            reset_detected_state();
            if (nclp_ripple_service_update_topology(0U, 0U) !=
                NCLP_RIPPLE_DETECTOR_OK) {
                reply->status = NCLP_COMMAND_STATUS_HARDWARE;
                enter_global_fault(reply->status);
                break;
            }
            nclp_operation.operation_state = NCLP_STATE_IDLE;
            bump_config_generation(1U);
            nclp_progress_finish(NCLP_STATE_IDLE, 0U, nclp_diagnostics.fail_count);
            publish_control_snapshot();
        }
        reply->data0 = acquisition_config_get()->sample_rate_hz;
        reply->data1 = acquisition_config_get()->sample_clock_config;
        reply->data2 = acquisition_config_get()->dsp_actual_cutoff_millihz;
        reply->data3 = acquisition_config_get()->dsp_cutoff_code;
        break;
    }
    case NCLP_CMD_SET_BANDWIDTH:
    {
        const nclp_acquisition_config_t *config;

        if (stopped_configuration_allowed() == 0) {
            reply->status = NCLP_COMMAND_STATUS_BUSY;
            break;
        }
        if (acquisition_config_set_bandwidth(command->args[0],
                                             command->args[1]) != 0) {
            reply->status = NCLP_COMMAND_STATUS_ARGUMENT;
            break;
        }
        g_init_valid = 0U;
        g_impedance_valid = 0U;
        nclp_operation.operation_state = NCLP_STATE_IDLE;
        bump_config_generation(0U);
        nclp_progress_finish(NCLP_STATE_IDLE, 0U, nclp_diagnostics.fail_count);
        config = acquisition_config_get();
        reply->data0 = config->analog_lower_cutoff_millihz;
        reply->data1 = config->analog_upper_cutoff_hz;
        reply->data2 = config->sample_rate_hz;
        reply->data3 = g_config_generation;
        break;
    }
    case NCLP_CMD_SET_DSP:
    {
        const nclp_acquisition_config_t *config;

        if (stopped_configuration_allowed() == 0) {
            reply->status = NCLP_COMMAND_STATUS_BUSY;
            break;
        }
        if (acquisition_config_set_dsp(command->args[0],
                                       command->args[1]) != 0) {
            reply->status = NCLP_COMMAND_STATUS_ARGUMENT;
            break;
        }
        g_init_valid = 0U;
        g_impedance_valid = 0U;
        nclp_operation.operation_state = NCLP_STATE_IDLE;
        bump_config_generation(0U);
        nclp_progress_finish(NCLP_STATE_IDLE, 0U, nclp_diagnostics.fail_count);
        config = acquisition_config_get();
        reply->data0 = config->dsp_enabled;
        reply->data1 = config->dsp_requested_cutoff_millihz;
        reply->data2 = config->dsp_actual_cutoff_millihz;
        reply->data3 = config->dsp_cutoff_code;
        break;
    }
    case NCLP_CMD_SET_TTL_SETTLE:
    {
        const nclp_acquisition_config_t *config;

        if (stopped_configuration_allowed() == 0) {
            reply->status = NCLP_COMMAND_STATUS_BUSY;
            break;
        }
        if (acquisition_config_set_ttl_fast_settle(command->args[0],
                                                   command->args[1]) != 0) {
            reply->status = NCLP_COMMAND_STATUS_ARGUMENT;
            break;
        }
        bump_config_generation(0U);
        config = acquisition_config_get();
        reply->data0 = config->ttl_fast_settle_enabled;
        reply->data1 = config->ttl_fast_settle_channel;
        reply->data2 = config->sample_rate_hz;
        reply->data3 = g_config_generation;
        break;
    }
    case NCLP_CMD_SCAN:
    {
        const nclp_acquisition_config_t *config = acquisition_config_get();
        nclp_scan_outcome_t scan_outcome;
        int stream_stop_result;
        int ripple_invalidate_result;
        uint32_t result_id;
        uint32_t total_phases = config->sample_rate_hz == 30000U ?
                                SPI_SCAN_PHASE_STEPS :
                                (2U * SPI_SCAN_PHASE_STEPS);

        if (nclp_operation.streaming != 0U || previous_stream_drained() == 0 ||
            (nclp_operation.operation_state != NCLP_STATE_IDLE &&
             nclp_operation.operation_state != NCLP_STATE_READY)) {
            reply->status = NCLP_COMMAND_STATUS_BUSY;
            break;
        }
        stream_stop_result = stop_command_stream();
        ripple_invalidate_result =
            nclp_ripple_service_update_topology(0U, 0U);
        if (stream_stop_result != 0 ||
            ripple_invalidate_result != NCLP_RIPPLE_DETECTOR_OK) {
            reply->status = NCLP_COMMAND_STATUS_HARDWARE;
            g_scan_valid = 0U;
            g_init_valid = 0U;
            g_impedance_valid = 0U;
            shared_stream_force_abort(NCLP_STREAM_ERROR_FORCED_ABORT);
            enter_global_fault(reply->status);
            publish_control_snapshot();
            publish_config_snapshot();
            break;
        }
        result_id = nclp_result_begin(NCLP_RESULT_TYPE_SCAN,
                                      SPI_SCAN_STREAMS,
                                      command->sequence);
        if (result_id == 0U) {
            reply->status = NCLP_COMMAND_STATUS_HARDWARE;
            nclp_diagnostics.fail_count++;
            enter_global_fault(reply->status);
            break;
        }
        nclp_operation.operation_state = NCLP_STATE_SCANNING;
        g_scan_valid = 0U;
        g_init_valid = 0U;
        g_impedance_valid = 0U;
        g_scan_lane_phases_30k_valid = 0U;
        begin_exclusive_operation(result_id);
        nclp_progress_begin(NCLP_CMD_SCAN, NCLP_STATE_SCANNING,
                            result_id, total_phases, nclp_diagnostics.fail_count);
        publish_control_snapshot();
        (void)shared_stream_stop();
        scan_outcome = run_configured_phase_scan();
        if (scan_outcome == NCLP_SCAN_OUTCOME_NO_CHIP) {
            reply->status = NCLP_COMMAND_STATUS_NO_CHIP_DETECTED;
        } else if (scan_outcome != NCLP_SCAN_OUTCOME_OK) {
            reply->status = NCLP_COMMAND_STATUS_HARDWARE;
        } else {
            g_scan_valid = 1U;
            g_stream_layout_id++;
            if (g_stream_layout_id == 0U) {
                g_stream_layout_id = 1U;
            }
            if (nclp_ripple_service_update_topology(
                    selected_stream_mask(),
                    packed_selected_stream_chip_ids()) !=
                NCLP_RIPPLE_DETECTOR_OK) {
                g_scan_valid = 0U;
                reply->status = NCLP_COMMAND_STATUS_HARDWARE;
                nclp_diagnostics.fail_count++;
            }
        }
        if (stage_scan_result_records() != 0 || nclp_result_commit(
            NCLP_RESULT_TYPE_SCAN,
            reply->status == NCLP_COMMAND_STATUS_OK ? NCLP_RESULT_STATE_COMPLETE :
                                  NCLP_RESULT_STATE_FAILED,
            reply->status, nclp_diagnostics.fail_count, g_stream_layout_id) != 0) {
            xil_printf("  FAIL scan result commit\r\n");
            nclp_diagnostics.fail_count++;
            g_scan_valid = 0U;
            (void)nclp_ripple_service_update_topology(0U, 0U);
            reply->status = NCLP_COMMAND_STATUS_HARDWARE;
        }
        if (reply->status == NCLP_COMMAND_STATUS_OK) {
            nclp_operation.operation_state = NCLP_STATE_IDLE;
            nclp_progress_finish(NCLP_STATE_IDLE, 0U, nclp_diagnostics.fail_count);
        } else if (reply->status == NCLP_COMMAND_STATUS_NO_CHIP_DETECTED) {
            /* Keep the negative discovery result inspectable, but leave the
             * controller retryable with the hardware-fault LED off. */
            nclp_operation.operation_state = NCLP_STATE_IDLE;
            nclp_progress_finish(NCLP_STATE_IDLE, reply->status,
                                 nclp_diagnostics.fail_count);
        } else {
            enter_global_fault(reply->status);
        }
        g_active_operation_result_id = 0U;
        publish_control_snapshot();
        publish_config_snapshot();
        reply->data0 = (uint32_t)selected_stream_mask();
        reply->data1 = selected_logical_stream_mask();
        reply->data2 = packed_selected_stream_chip_ids();
        reply->data3 = g_stream_layout_id;
        break;
    }
    case NCLP_CMD_INIT:
    {
        const nclp_acquisition_config_t *config = acquisition_config_get();
        uint32_t result_id;
        uint32_t chip_count = selected_physical_chip_count();

        if (nclp_operation.streaming != 0U || previous_stream_drained() == 0 ||
            (nclp_operation.operation_state != NCLP_STATE_IDLE &&
             nclp_operation.operation_state != NCLP_STATE_READY)) {
            reply->status = NCLP_COMMAND_STATUS_BUSY;
            break;
        }
        stream = (command->args[0] == NCLP_INIT_FIRST_STREAM) ?
                 first_selected_stream() : (command->args[0] & 0x7U);
        if (g_scan_valid == 0U || selected_stream_mask() == 0U) {
            reply->status = NCLP_COMMAND_STATUS_PREREQUISITE;
        } else if (stream != first_selected_stream()) {
            reply->status = NCLP_COMMAND_STATUS_PREREQUISITE;
        } else {
            result_id = nclp_result_begin(NCLP_RESULT_TYPE_INIT,
                                          chip_count,
                                          command->sequence);
            if (result_id == 0U) {
                reply->status = NCLP_COMMAND_STATUS_HARDWARE;
                nclp_diagnostics.fail_count++;
                enter_global_fault(reply->status);
            } else {
                nclp_operation.operation_state = NCLP_STATE_INITIALIZING;
                g_init_valid = 0U;
                g_impedance_valid = 0U;
                begin_exclusive_operation(result_id);
                nclp_progress_begin(NCLP_CMD_INIT,
                                    NCLP_STATE_INITIALIZING,
                                    result_id, chip_count, nclp_diagnostics.fail_count);
                publish_control_snapshot();
                if (run_post_detection_init_all(
                        INIT_VDD_SENSE_ENABLE,
                        config->dsp_enabled,
                        config->dsp_cutoff_code,
                        config->analog_upper_cutoff_hz,
                        config->analog_lower_cutoff_millihz) != 0) {
                    reply->status = NCLP_COMMAND_STATUS_HARDWARE;
                } else {
                    g_init_valid = 1U;
                }
                if (nclp_result_commit(
                    NCLP_RESULT_TYPE_INIT,
                    reply->status == NCLP_COMMAND_STATUS_OK ?
                        NCLP_RESULT_STATE_COMPLETE :
                                          NCLP_RESULT_STATE_FAILED,
                    reply->status, nclp_diagnostics.fail_count, g_stream_layout_id) != 0) {
                    xil_printf("  FAIL init result commit\r\n");
                    nclp_diagnostics.fail_count++;
                    g_init_valid = 0U;
                    reply->status = NCLP_COMMAND_STATUS_HARDWARE;
                }
                if (reply->status == NCLP_COMMAND_STATUS_OK) {
                    nclp_operation.operation_state = NCLP_STATE_READY;
                    nclp_progress_finish(NCLP_STATE_READY, 0U,
                                         nclp_diagnostics.fail_count);
                } else {
                    enter_global_fault(reply->status);
                }
                g_active_operation_result_id = 0U;
                publish_config_snapshot();
            }
        }
        reply->data0 = stream;
        reply->data1 = config->sample_rate_hz;
        reply->data2 = config->analog_upper_cutoff_hz;
        reply->data3 = config->analog_lower_cutoff_millihz;
        break;
    }
    case NCLP_CMD_IMPEDANCE:
    {
        uint32_t channel_count = selected_physical_channel_count();
        uint32_t result_id;
        uint32_t result_state;
        int impedance_status;

        if (nclp_operation.streaming != 0U || previous_stream_drained() == 0 ||
            nclp_operation.operation_state != NCLP_STATE_READY) {
            reply->status = NCLP_COMMAND_STATUS_BUSY;
            break;
        }
        if (g_init_valid == 0U || channel_count == 0U) {
            reply->status = NCLP_COMMAND_STATUS_PREREQUISITE;
            break;
        }
        if (stop_command_stream() != 0) {
            reply->status = NCLP_COMMAND_STATUS_HARDWARE;
            g_impedance_valid = 0U;
            shared_stream_force_abort(NCLP_STREAM_ERROR_FORCED_ABORT);
            enter_global_fault(reply->status);
            publish_config_snapshot();
            break;
        }
        result_id = nclp_result_begin(NCLP_RESULT_TYPE_IMPEDANCE,
                                      channel_count,
                                      command->sequence);
        if (result_id == 0U) {
            reply->status = NCLP_COMMAND_STATUS_HARDWARE;
            nclp_diagnostics.fail_count++;
            enter_global_fault(reply->status);
            break;
        }
        nclp_operation.operation_state = NCLP_STATE_IMPEDANCE;
        g_impedance_valid = 0U;
        begin_exclusive_operation(result_id);
        nclp_progress_begin(NCLP_CMD_IMPEDANCE, NCLP_STATE_IMPEDANCE,
                            result_id, IMP_CAP_RANGES * channel_count,
                            nclp_diagnostics.fail_count);
        publish_control_snapshot();
        (void)shared_stream_stop();
        impedance_status = run_impedance_transaction();
        if (impedance_status == NCLP_IMPEDANCE_CANCELLED) {
            reply->status = NCLP_COMMAND_STATUS_CANCELLED;
            result_state = NCLP_RESULT_STATE_CANCELLED;
        } else if (impedance_status != NCLP_IMPEDANCE_OK) {
            reply->status = NCLP_COMMAND_STATUS_HARDWARE;
            result_state = NCLP_RESULT_STATE_FAILED;
        } else {
            g_impedance_valid = 1U;
            result_state = NCLP_RESULT_STATE_COMPLETE;
        }
        if (nclp_result_commit(
            NCLP_RESULT_TYPE_IMPEDANCE,
            result_state,
            reply->status, nclp_diagnostics.fail_count, g_stream_layout_id) != 0) {
            xil_printf("  FAIL impedance result commit\r\n");
            nclp_diagnostics.fail_count++;
            g_impedance_valid = 0U;
            reply->status = NCLP_COMMAND_STATUS_HARDWARE;
            result_state = NCLP_RESULT_STATE_FAILED;
        }
        if (reply->status == NCLP_COMMAND_STATUS_OK ||
            reply->status == NCLP_COMMAND_STATUS_CANCELLED) {
            nclp_operation.operation_state = NCLP_STATE_READY;
            nclp_progress_finish(NCLP_STATE_READY, reply->status,
                                 nclp_diagnostics.fail_count);
        } else {
            enter_global_fault(reply->status);
        }
        finish_cancel_ack(reply->status);
        publish_config_snapshot();
        reply->data0 = result_id;
        reply->data1 = nclp_result_staged_count(
            NCLP_RESULT_TYPE_IMPEDANCE);
        reply->data2 = sizeof(nclp_impedance_result_record_t);
        reply->data3 = result_state;
        break;
    }
    case NCLP_CMD_RESET:
        if (nclp_operation.operation_state == NCLP_STATE_FAULT &&
            nclp_operation.streaming != 0U) {
            shared_stream_force_abort(NCLP_STREAM_ERROR_FORCED_ABORT);
            if (nclp_operation.streaming != 0U) {
                reply->status = NCLP_COMMAND_STATUS_BUSY;
                break;
            }
        }
        if (nclp_operation.streaming != 0U || previous_stream_drained() == 0 ||
            nclp_operation.operation_state == NCLP_STATE_SCANNING ||
            nclp_operation.operation_state == NCLP_STATE_INITIALIZING ||
            nclp_operation.operation_state == NCLP_STATE_IMPEDANCE ||
            nclp_operation.operation_state == NCLP_STATE_CANCELLING ||
            nclp_operation.operation_state == NCLP_STATE_RESTORING) {
            reply->status = NCLP_COMMAND_STATUS_BUSY;
            break;
        }
        if (reset_whole_control_path() != 0) {
            reply->status = nclp_operation.streaming != 0U ?
                NCLP_COMMAND_STATUS_BUSY : NCLP_COMMAND_STATUS_HARDWARE;
        } else {
            /* Clear fabric diagnostics independently of SFP link recovery.
             * An offline peer does not block the selected local path. */
            int compute_clear = nclp_compute_clear_fault();
            if (compute_clear != 0)
                reply->status = NCLP_COMMAND_STATUS_HARDWARE;
        }
        if (reply->status == NCLP_COMMAND_STATUS_OK) {
            nclp_operation.operation_state = NCLP_STATE_IDLE;
            nclp_progress_finish(NCLP_STATE_IDLE, 0U, nclp_diagnostics.fail_count);
        } else {
            enter_global_fault(reply->status);
        }
        publish_config_snapshot();
        publish_control_snapshot();
        reply->data0 = command_status_word();
        break;
    case NCLP_CMD_START_STREAM: {
        uint32_t mask;
        uint32_t physical_mask = (uint32_t)selected_stream_mask();
        uint32_t aux_mode = command->args[4];

        if (nclp_operation.streaming != 0U || previous_stream_drained() == 0 ||
            nclp_operation.operation_state != NCLP_STATE_READY || g_init_valid == 0U) {
            reply->status = NCLP_COMMAND_STATUS_BUSY;
            break;
        }
        if (physical_mask == 0U) {
            reply->status = NCLP_COMMAND_STATUS_PREREQUISITE;
            break;
        }
        if (aux_mode != NCLP_STREAM_AUX_INPUTS &&
            aux_mode != NCLP_STREAM_AUX_VDD) {
            reply->status = NCLP_COMMAND_STATUS_ARGUMENT;
            break;
        }
        mask = selected_logical_stream_mask();
        if (nclp_compute_sync_transport_mode() != 0) {
            reply->status = NCLP_COMMAND_STATUS_HARDWARE;
            break;
        }
        uint32_t sfp_mode_selected =
            nclp_compute_sfp_mode_selected() != 0U;
        int start_result;
        if (sfp_mode_selected != 0U && !nclp_compute_sfp_link_ready()) {
            reply->status = NCLP_COMMAND_STATUS_NETWORK;
            break;
        }
        g_stream_delivery = sfp_mode_selected != 0U ?
                                NCLP_STREAM_DELIVERY_SFP :
                                NCLP_STREAM_DELIVERY_UDP;
        if (sfp_mode_selected != 0U) {
            start_result = arm_sfp_stream_run(
                mask, NCLP_STREAM_MODE_CONT, 0U, aux_mode);
        } else {
            start_result = arm_stream_mask_run(
                mask, NCLP_STREAM_MODE_CONT, 0U, aux_mode);
        }
        if (start_result != 0 ||
            acquisition_config_apply_ttl_hardware(1U) != 0 ||
            (sfp_mode_selected != 0U ? start_armed_sfp_stream() :
                                      start_armed_stream()) != 0) {
            reply->status = NCLP_COMMAND_STATUS_HARDWARE;
            shared_stream_force_abort(NCLP_STREAM_ERROR_FORCED_ABORT);
            g_init_valid = 0U;
            g_impedance_valid = 0U;
            enter_global_fault(reply->status);
            publish_config_snapshot();
            break;
        }
        nclp_operation.streaming = 1U;
        nclp_operation.operation_state = NCLP_STATE_STREAMING;
        nclp_operation.stream_aux_mode = aux_mode;
        if (sfp_mode_selected == 0U) {
            shared_stream_start(mask, physical_mask, aux_mode);
        }
        nclp_progress_begin(NCLP_CMD_START_STREAM,
                            NCLP_STATE_STREAMING, 0U, 0U,
                            nclp_diagnostics.fail_count);
        reply->data0 = physical_mask;
        reply->data1 = mask;
        reply->data2 = count_streams(mask);
        reply->data3 = aux_mode |
                       ((g_stream_layout_id & 0xFFFFU) << 16);
        break;
    }
    case NCLP_CMD_STOP_STREAM:
        if (nclp_operation.operation_state == NCLP_STATE_DRAINING) {
            reply->data0 = command_status_word();
            break;
        }
        if (nclp_operation.operation_state != NCLP_STATE_STREAMING || nclp_operation.streaming == 0U) {
            reply->status = NCLP_COMMAND_STATUS_NOT_ACTIVE;
            break;
        }
        if (acquisition_config_disable_ttl_hardware() != 0) {
            reply->status = NCLP_COMMAND_STATUS_HARDWARE;
            shared_stream_force_abort(NCLP_STREAM_ERROR_FORCED_ABORT);
            enter_global_fault(reply->status);
            break;
        }
        nclp_stim_emergency_stop();
        /* At 5 kS/s one millisecond spans five complete frames, giving the
         * PL enough time to inject the selected TTL-low Reg0 command. */
        usleep(1000U);
        if ((g_stream_delivery == NCLP_STREAM_DELIVERY_SFP ?
             stop_sfp_stream() : stop_command_stream()) != 0) {
            reply->status = NCLP_COMMAND_STATUS_HARDWARE;
            shared_stream_force_abort(NCLP_STREAM_ERROR_FORCED_ABORT);
            enter_global_fault(reply->status);
            break;
        }
        if (g_stream_delivery == NCLP_STREAM_DELIVERY_SFP) {
            nclp_operation.operation_state = NCLP_STATE_READY;
            nclp_progress_finish(NCLP_STATE_READY, 0U, nclp_diagnostics.fail_count);
            reply->data0 = command_status_word();
            break;
        }
        g_stream_delivery = NCLP_STREAM_DELIVERY_LOCAL;
        if (shared_stream_stop() != 0) {
            reply->status = NCLP_COMMAND_STATUS_HARDWARE;
            enter_global_fault(reply->status);
            break;
        }
        nclp_operation.operation_state = NCLP_STATE_DRAINING;
        nclp_progress_finish(NCLP_STATE_DRAINING, reply->status,
                             nclp_diagnostics.fail_count);
        reply->data0 = command_status_word();
        break;
    default: {
        const nclp_output_context_t output_context = {
            .streaming = nclp_operation.streaming,
            .operation_state = nclp_operation.operation_state,
            .stopped_configuration_allowed = stopped_configuration_allowed(),
            .physical_chip_mask = selected_stream_mask(),
            .packed_chip_ids = packed_selected_stream_chip_ids(),
            .local_detector_stream_active =
                nclp_operation.streaming != 0U &&
                g_stream_delivery == NCLP_STREAM_DELIVERY_UDP
        };
        if (!nclp_output_execute(command, reply, &output_context)) {
            reply->status = NCLP_COMMAND_STATUS_UNKNOWN;
            reply->data0 = command->command;
        }
        break;
    }
    }

    /* Publish the final command state/topology as one coherent STATUS view.
     * Long operations also publish their entry/cancellation states through
     * begin_exclusive_operation() and the cancellation hooks above. */
    publish_control_snapshot();
    reply->fail_count = nclp_diagnostics.fail_count;
    return reply->status == NCLP_COMMAND_STATUS_OK ? 0 : -1;
}

static void copy_reply_to_shared(volatile nclp_main_reply_t *dst,
                                 const nclp_main_reply_t *src)
{
    dst->magic = src->magic;
    dst->version = src->version;
    dst->command = src->command;
    dst->sequence = src->sequence;
    dst->status = src->status;
    dst->data0 = src->data0;
    dst->data1 = src->data1;
    dst->data2 = src->data2;
    dst->data3 = src->data3;
    dst->fail_count = src->fail_count;
}

static void service_shared_command(void)
{
    volatile nclp_shared_state_t *shared = nclp_shared_state();
    nclp_main_command_t command;
    nclp_main_reply_t reply;

    shared_invalidate();
    if (shared->command_state != NCLP_SHARED_CMD_REQUEST) {
        return;
    }

    shared_barrier();
    command.command = shared->command.command;
    command.sequence = shared->command.sequence;
    for (uint32_t i = 0U; i < (NCLP_CMD_WORDS - 4U); ++i) {
        command.args[i] = shared->command.args[i];
    }
    shared->command_state = NCLP_SHARED_CMD_RUNNING;
    shared_flush();

    xil_printf("\r\n[command] cmd=0x%02lx seq=%lu\r\n",
               (unsigned long)command.command,
               (unsigned long)command.sequence);
    (void)nclp_main_execute_command(&command, &reply);

    shared_invalidate();
    copy_reply_to_shared(&shared->reply, &reply);
    shared_barrier();
    shared->command_state = NCLP_SHARED_CMD_REPLY;
    shared->control_heartbeat++;
    shared_flush();
}

static void initialize_shared_state(void)
{
    volatile uint32_t *words =
        (volatile uint32_t *)(uintptr_t)NCLP_SHARED_BASE_ADDRESS;
    volatile nclp_shared_state_t *shared = nclp_shared_state();

    for (uint32_t i = 0U; i <
         (sizeof(nclp_shared_state_t) + 3U) / 4U; ++i) {
        words[i] = 0U;
    }
    shared->version = NCLP_SHARED_VERSION;
    shared->command_state = NCLP_SHARED_CMD_IDLE;
    shared_barrier();
    shared->magic = NCLP_SHARED_MAGIC;
    shared_flush();
}

int main(void)
{
    uint32_t startup_ok = 1U;

    xil_printf("\r\nNCLP A53-0 control/calculation firmware\r\n");
    xil_printf("Role: headstage scan/init/impedance, PL capture ownership, and fan control.\r\n");

    nclp_shared_configure_memory();
    nclp_result_store_control_init();
    initialize_shared_state();
    acquisition_config_reset_defaults();
    publish_config_snapshot();
    nclp_progress_begin(0U, NCLP_STATE_IDLE, 0U, 0U, nclp_diagnostics.fail_count);
    if (nclp_fan_init() != 0) {
        startup_ok = 0U;
    }
    if (nclp_led_init() != 0) {
        startup_ok = 0U;
    }
    if (nclp_output_init() != 0) {
        startup_ok = 0U;
    }
    if (nclp_ripple_service_init() != NCLP_RIPPLE_DETECTOR_OK) {
        startup_ok = 0U;
    }

    if (reset_board_path() != 0) {
        xil_printf("\r\nRESULT FAIL: reset path failed\r\n");
        startup_ok = 0U;
    }

    if (nclp_compute_init() != 0) {
        startup_ok = 0U;
    }
    reset_detected_state();

    if (startup_ok != 0U && print_board_identity() != 0) {
        xil_printf("\r\nRESULT FAIL: board identity read failed\r\n");
        startup_ok = 0U;
    }

    xil_printf("  PASS A53-0 command mailbox    address=0x%08lx\r\n",
               (unsigned long)NCLP_SHARED_BASE_ADDRESS);
    if (startup_ok == 0U) {
        enter_global_fault(1U);
    } else if (NCLP_DEBUG_MODE == 1U) {
        (void)nclp_debug_autorun();
    }
    publish_control_snapshot();
    xil_printf("  WAIT commands from A53-1 network firmware mode=%s\r\n",
               NCLP_DEBUG_MODE == 1U ? "debug-autorun" : "normal");
    for (;;) {
        uint32_t stim_errors = nclp_stim_service();
        uint32_t compute_fault = nclp_compute_fault_pending() != 0;
        uint32_t sfp_link_ready = nclp_compute_sfp_link_ready() != 0;
        uint32_t sfp_takeover =
            nclp_operation.streaming != 0U &&
            g_stream_delivery != NCLP_STREAM_DELIVERY_SFP &&
            sfp_link_ready != 0U;
        uint32_t sfp_link_loss =
            nclp_operation.streaming != 0U &&
            g_stream_delivery == NCLP_STREAM_DELIVERY_SFP &&
            sfp_link_ready == 0U;

        if ((stim_errors != 0U || compute_fault != 0U ||
             sfp_takeover != 0U || sfp_link_loss != 0U) &&
            nclp_operation.operation_state != NCLP_STATE_FAULT) {
            uint32_t compute_error =
                (sfp_takeover != 0U || sfp_link_loss != 0U) ?
                NCLP_STREAM_ERROR_LINK_FAULT :
                (nclp_compute_fabric_status() &
                 COMPUTE_FABRIC_STATUS_LOCAL_DECODE_FAULT) != 0U ?
                NCLP_STREAM_ERROR_COMPUTE_FAULT :
                NCLP_STREAM_ERROR_LINK_FAULT;
            /* PS owns recovery for stimulation, local decode, and SFP-link
             * faults. Revoke physical output before exposing global FAULT. */
            if (nclp_operation.streaming != 0U) {
                shared_stream_force_abort(stim_errors != 0U ?
                    NCLP_STREAM_ERROR_STIM_FAULT : compute_error);
            }
            nclp_diagnostics.fail_count++;
            enter_global_fault(1U);
            publish_control_snapshot();
        }
        if (nclp_operation.streaming == 0U) {
            /* Link-up owns SFP control and compute streaming; link-down owns
             * the local PS/RJ45 route. A busy fabric is retried next poll. */
            (void)nclp_compute_sync_transport_mode();
        }
        service_shared_command();
        nclp_ripple_service_poll();
        service_stream_blocks();
        nclp_fan_service();
        usleep(50U);
    }
}
