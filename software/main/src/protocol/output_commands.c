#include "output_commands.h"
#include "../drivers/stim_control.h"
#include "../drivers/ttl_router.h"
#include "../drivers/ripple_detector_service.h"
#include "../drivers/intan_sync_control.h"
#include "../../../common/nclp_pl_registers.h"
#include <stddef.h>

/* A combined host command must never leave a partially configured action
 * eligible for ARM. Only complete configuration or initialization clears this. */
static int output_configuration_incomplete = 1;

int nclp_output_init(void)
{
    int result;
    output_configuration_incomplete = 1;
    result = nclp_stim_init();
    if (result != NCLP_STIM_OK) return result;
    result = nclp_ttl_router_init();
    if (result != NCLP_TTL_ROUTER_OK) {
        nclp_stim_emergency_stop();
        return result;
    }
    output_configuration_incomplete = 0;
    return NCLP_STIM_OK;
}

static int output_get_section(uint32_t section, uint32_t words[4])
{
    int result = nclp_stim_get_section(section, words);
    if (result == NCLP_STIM_OK && section == NCLP_STIM_SECTION_ACTION)
        result = nclp_ttl_router_get_route(&words[1]);
    return result;
}

static int output_action_route_valid(const nclp_stim_action_config_t *config,
                                     uint32_t ttl0_source,
                                     uint32_t ttl1_source)
{
    uint32_t stim_route_count;

    if (config == NULL) return 0;
    stim_route_count = (ttl0_source == TTL_ROUTER_SOURCE_STIM ? 1U : 0U) +
                       (ttl1_source == TTL_ROUTER_SOURCE_STIM ? 1U : 0U);

    if (config->mode == STIM_OUTPUT_MODE_OFF)
        return stim_route_count == 0U;
    if (config->mode == STIM_OUTPUT_MODE_TTL)
        return stim_route_count == 1U &&
               config->ttl_pulse_width_axi_cycles != 0U;
    if (config->mode == STIM_OUTPUT_MODE_DAC)
        return (stim_route_count == 0U &&
                config->ttl_pulse_width_axi_cycles == 0U) ||
               (stim_route_count == 1U &&
                config->ttl_pulse_width_axi_cycles != 0U);
    return 0;
}

static int output_set_action(const nclp_stim_action_config_t *config,
                             uint32_t ttl0_source, uint32_t ttl1_source)
{
    int result;
    uint32_t status, errors;
    if (!nclp_stim_action_valid(config) ||
        !nclp_ttl_router_route_valid(ttl0_source, ttl1_source) ||
        !output_action_route_valid(config, ttl0_source, ttl1_source))
        return NCLP_STIM_ERROR_ARGUMENT;
    result = nclp_ttl_router_get_status(&status, &errors);
    if (result != NCLP_TTL_ROUTER_OK) return result;
    if (status & TTL_ROUTER_STATUS_ROUTE_WRITE_LOCKED) return NCLP_STIM_ERROR_BUSY;
    output_configuration_incomplete = 1;
    result = nclp_ttl_router_set_route(TTL_ROUTER_SOURCE_OFF, TTL_ROUTER_SOURCE_OFF);
    if (result == NCLP_TTL_ROUTER_OK) result = nclp_stim_set_action(config);
    if (result == NCLP_STIM_OK)
        result = nclp_ttl_router_set_route(ttl0_source, ttl1_source);
    if (result != NCLP_STIM_OK) {
        nclp_stim_emergency_stop();
        (void)nclp_ttl_router_set_route(TTL_ROUTER_SOURCE_OFF, TTL_ROUTER_SOURCE_OFF);
        return result;
    }
    output_configuration_incomplete = 0;
    return NCLP_STIM_OK;
}

static int command_args_zero_from(const nclp_main_command_t *command,
                                  uint32_t first_unused)
{
    for (uint32_t index = first_unused;
         index < (NCLP_CMD_WORDS - 4U); ++index) {
        if (command->args[index] != 0U) {
            return 0;
        }
    }
    return 1;
}

static uint32_t stim_reply_status(int result)
{
    if (result == NCLP_STIM_ERROR_ARGUMENT) {
        return NCLP_COMMAND_STATUS_ARGUMENT;
    }
    if (result == NCLP_STIM_ERROR_BUSY) {
        return NCLP_COMMAND_STATUS_BUSY;
    }
    return result == NCLP_STIM_OK ? NCLP_COMMAND_STATUS_OK :
                                    NCLP_COMMAND_STATUS_HARDWARE;
}

static uint32_t intan_sync_reply_status(int result)
{
    if (result == NCLP_INTAN_SYNC_ERROR_ARGUMENT)
        return NCLP_COMMAND_STATUS_ARGUMENT;
    if (result == NCLP_INTAN_SYNC_ERROR_BUSY)
        return NCLP_COMMAND_STATUS_BUSY;
    return result == NCLP_INTAN_SYNC_OK ? NCLP_COMMAND_STATUS_OK :
           NCLP_COMMAND_STATUS_HARDWARE;
}

static uint32_t ripple_reply_status(int result)
{
    if (result == NCLP_RIPPLE_DETECTOR_ERROR_ARGUMENT) return NCLP_COMMAND_STATUS_ARGUMENT;
    if (result == NCLP_RIPPLE_DETECTOR_ERROR_NOT_READY) return NCLP_COMMAND_STATUS_PREREQUISITE;
    if (result == NCLP_RIPPLE_DETECTOR_ERROR_BUSY) return NCLP_COMMAND_STATUS_BUSY;
    if (result == NCLP_RIPPLE_DETECTOR_ERROR_RATE) return NCLP_COMMAND_STATUS_PREREQUISITE;
    if (result == NCLP_RIPPLE_DETECTOR_ERROR_NOT_ACTIVE) return NCLP_COMMAND_STATUS_NOT_ACTIVE;
    return result == NCLP_RIPPLE_DETECTOR_OK ? NCLP_COMMAND_STATUS_OK :
           NCLP_COMMAND_STATUS_HARDWARE;
}
static int ripple_get_config_page(uint32_t page, uint32_t words[4])
{
    nclp_ripple_profile_t profile;
    int result;
    if (page > NCLP_RIPPLE_CONFIG_THRESHOLD)
        return NCLP_RIPPLE_DETECTOR_ERROR_ARGUMENT;
    result = nclp_ripple_service_get_profile(&profile);
    if (result) return result;
    if (page == NCLP_RIPPLE_CONFIG_PROFILE) {
        words[0] = profile.detected_channel_index;
        words[1] = profile.filter_kind;
        words[2] = profile.fir_tap_count;
        words[3] = profile.power_window_us;
    } else if (page == NCLP_RIPPLE_CONFIG_BAND) {
        words[0] = profile.low_cutoff_millihz;
        words[1] = profile.high_cutoff_millihz;
        words[2] = profile.refractory_period_ms;
        words[3] = profile.baseline_duration_ms;
    } else {
        words[0] = profile.baseline_mean_bits;
        words[1] = profile.baseline_stddev_bits;
        words[2] = profile.threshold_k_bits;
        words[3] = profile.input_channel_id;
    }
    return NCLP_RIPPLE_DETECTOR_OK;
}

static void reply_set_words(nclp_main_reply_t *reply,
                            const uint32_t words[4])
{
    reply->data0 = words[0];
    reply->data1 = words[1];
    reply->data2 = words[2];
    reply->data3 = words[3];
}

int nclp_output_execute(const nclp_main_command_t *command,
                         nclp_main_reply_t *reply,
                         const nclp_output_context_t *context)
{
    if (command == NULL || reply == NULL || context == NULL) return 0;
    switch (command->command) {
    case NCLP_CMD_STIM_GET:
    {
        uint32_t words[4] = {0U, 0U, 0U, 0U};
        int result;

        if (!command_args_zero_from(command, 1U)) {
            reply->status = NCLP_COMMAND_STATUS_ARGUMENT;
            break;
        }
        result = output_get_section(command->args[0], words);
        reply->status = stim_reply_status(result);
        reply_set_words(reply, words);
        break;
    }
    case NCLP_CMD_STIM_SET_ACTION:
    {
        nclp_stim_action_config_t config;
        uint32_t words[4] = {0U, 0U, 0U, 0U};
        int result;

        if (!command_args_zero_from(command, 6U)) {
            reply->status = NCLP_COMMAND_STATUS_ARGUMENT;
            break;
        }
        config.mode = command->args[0];
        config.ttl_pulse_width_axi_cycles = command->args[3];
        config.intan_marker_mask = command->args[4];
        config.external_trigger_enable = command->args[5];
        result = output_set_action(&config, command->args[1], command->args[2]);
        if (result == NCLP_STIM_OK) {
            result = output_get_section(NCLP_STIM_SECTION_ACTION, words);
        }
        reply->status = stim_reply_status(result);
        reply_set_words(reply, words);
        break;
    }
    case NCLP_CMD_DAC_SET_INTAN_TTL:
    {
        uint32_t words[4] = {0U, 0U, 0U, 0U};
        int result;
        if (!command_args_zero_from(command, 1U)) {
            reply->status = NCLP_COMMAND_STATUS_ARGUMENT;
            break;
        }
        if (!context->stopped_configuration_allowed) {
            reply->status = NCLP_COMMAND_STATUS_BUSY;
            break;
        }
        result = nclp_stim_set_recorded_ttl(command->args[0]);
        if (result == NCLP_STIM_OK)
            result = output_get_section(NCLP_STIM_SECTION_ACTION, words);
        reply->status = stim_reply_status(result);
        reply_set_words(reply, words);
        break;
    }
    case NCLP_CMD_DAC_SET_CONFIG:
    {
        nclp_stim_dac_config_t config;
        uint32_t words[4] = {0U, 0U, 0U, 0U};
        int result;

        if (!command_args_zero_from(command, 6U)) {
            reply->status = NCLP_COMMAND_STATUS_ARGUMENT;
            break;
        }
        config.channel_mask = command->args[0];
        config.update_period_clocks = command->args[1];
        config.start_index = command->args[2];
        config.loop_index = command->args[3];
        config.end_index = command->args[4];
        config.finite_update_count = command->args[5];
        result = nclp_stim_set_dac(&config);
        if (result == NCLP_STIM_OK) {
            result = output_get_section(
                NCLP_STIM_SECTION_DAC_CONFIG_0, words);
        }
        reply->status = stim_reply_status(result);
        reply_set_words(reply, words);
        break;
    }
    case NCLP_CMD_DAC_SET_CLOCK:
    {
        uint32_t clock_status = 0U;
        uint32_t engine_hz;
        int result;

        if (!command_args_zero_from(command, 3U)) {
            reply->status = NCLP_COMMAND_STATUS_ARGUMENT;
            break;
        }
        /* Clock reprogramming can take long enough to starve the finite DDR
         * ring, so perform it only while acquisition streaming is stopped. */
        if (context->streaming != 0U) {
            reply->status = NCLP_COMMAND_STATUS_BUSY;
            break;
        }
        result = nclp_stim_set_clock(command->args[0], command->args[1],
                                     command->args[2], &clock_status);
        reply->status = stim_reply_status(result);
        if (result == NCLP_STIM_OK) {
            engine_hz = (uint32_t)((140000000ULL * command->args[2]) /
                        (command->args[1] * command->args[0]));
            reply->data0 =
                (command->args[0] << STIM_DAC_CLOCK_CONFIG_O_SHIFT) |
                (command->args[1] << STIM_DAC_CLOCK_CONFIG_D_SHIFT) |
                (command->args[2] << STIM_DAC_CLOCK_CONFIG_M_SHIFT);
            reply->data1 = clock_status;
            reply->data2 = engine_hz;
            reply->data3 = engine_hz / 4U;
        } else {
            reply->data1 = clock_status;
        }
        break;
    }
    case NCLP_CMD_DAC_WRITE:
    {
        uint32_t count = command->args[1];
        uint32_t crc = 0U;
        int result;

        if (count == 0U || count > 10U ||
            !command_args_zero_from(command, 2U + count)) {
            reply->status = NCLP_COMMAND_STATUS_ARGUMENT;
            break;
        }
        result = nclp_stim_write_ram(command->args[0],
                                     &command->args[2], count, &crc);
        reply->status = stim_reply_status(result);
        reply->data0 = command->args[0];
        reply->data1 = count;
        reply->data2 = command->args[0] + count - 1U;
        reply->data3 = crc;
        break;
    }
    case NCLP_CMD_DAC_READ:
    {
        uint32_t words[4] = {0U, 0U, 0U, 0U};
        uint32_t count = command->args[1];
        int result;

        if (count == 0U || count > 4U ||
            !command_args_zero_from(command, 2U)) {
            reply->status = NCLP_COMMAND_STATUS_ARGUMENT;
            break;
        }
        result = nclp_stim_read_ram(command->args[0], words, count);
        reply->status = stim_reply_status(result);
        reply_set_words(reply, words);
        break;
    }
    case NCLP_CMD_DAC_PRESET:
    {
        nclp_stim_preset_config_t config;
        nclp_stim_action_config_t action;
        nclp_dac_preset_plan_t plan = {0U, 0U, 0U};
        uint32_t action_words[4] = {0U, 0U, 0U, 0U};
        uint32_t crc = 0U;
        int result;

        if (!command_args_zero_from(command, 10U)) {
            reply->status = NCLP_COMMAND_STATUS_ARGUMENT;
            break;
        }
        /* Generating up to 1024 transcendental samples is cold-path work and
         * must not block A53-0's live DDR-ring consumer. */
        if (context->streaming != 0U) {
            reply->status = NCLP_COMMAND_STATUS_BUSY;
            break;
        }
        if (context->operation_state == NCLP_STATE_FAULT) {
            reply->status = NCLP_COMMAND_STATUS_BUSY;
            break;
        }
        config.kind = command->args[0];
        config.channel_mask = command->args[1];
        config.update_rate_hz = command->args[2];
        config.parameter = command->args[3];
        config.minimum_a_uv = command->args[4];
        config.maximum_a_uv = command->args[5];
        config.minimum_b_uv = command->args[6];
        config.maximum_b_uv = command->args[7];
        config.repeat_count = command->args[8];
        config.external_trigger_enable = command->args[9];
        if (output_configuration_incomplete) {
            reply->status = NCLP_COMMAND_STATUS_BUSY;
            break;
        }
        /* Loading a preset changes the action mode to DAC while preserving
         * the physical route and pulse width. Validate that resulting tuple
         * before the first waveform or register write. */
        result = output_get_section(NCLP_STIM_SECTION_ACTION, action_words);
        if (result != NCLP_STIM_OK) {
            reply->status = stim_reply_status(result);
            break;
        }
        action.mode = STIM_OUTPUT_MODE_DAC;
        action.ttl_pulse_width_axi_cycles = action_words[2];
        action.intan_marker_mask = action_words[3];
        action.external_trigger_enable = config.external_trigger_enable;
        if (!nclp_stim_action_valid(&action) ||
            !output_action_route_valid(&action,
                action_words[1] & 3U, (action_words[1] >> 2U) & 3U)) {
            reply->status = NCLP_COMMAND_STATUS_ARGUMENT;
            break;
        }
        result = nclp_stim_load_preset(&config, &plan, &crc);
        reply->status = stim_reply_status(result);
        reply->data0 = plan.sample_count;
        reply->data1 = plan.update_period_clocks;
        reply->data2 = plan.finite_update_count;
        reply->data3 = crc;
        break;
    }
    case NCLP_CMD_STIM_CONTROL:
    {
        uint32_t words[4] = {0U, 0U, 0U, 0U};
        int result;

        if (!command_args_zero_from(command, 1U) ||
            (command->args[0] != NCLP_STIM_ACTION_ARM &&
             command->args[0] != NCLP_STIM_ACTION_DISARM &&
             command->args[0] != NCLP_STIM_ACTION_TRIGGER &&
             command->args[0] != NCLP_STIM_ACTION_CLEAR &&
             command->args[0] != NCLP_STIM_ACTION_PRIME)) {
            reply->status = NCLP_COMMAND_STATUS_ARGUMENT;
            break;
        }
        if (context->operation_state == NCLP_STATE_FAULT &&
            command->args[0] != NCLP_STIM_ACTION_DISARM &&
            command->args[0] != NCLP_STIM_ACTION_CLEAR) {
            reply->status = NCLP_COMMAND_STATUS_BUSY;
            break;
        }
        if (output_configuration_incomplete &&
            command->args[0] != NCLP_STIM_ACTION_DISARM &&
            command->args[0] != NCLP_STIM_ACTION_CLEAR) {
            reply->status = NCLP_COMMAND_STATUS_BUSY;
            break;
        }
        result = nclp_stim_control(command->args[0], words);
        reply->status = stim_reply_status(result);
        reply_set_words(reply, words);
        break;
    }
    case NCLP_CMD_STIM_DIAGNOSTICS:
    {
        uint32_t words[4] = {0U, 0U, 0U, 0U};
        int result;

        if (!command_args_zero_from(command, 2U)) {
            reply->status = NCLP_COMMAND_STATUS_ARGUMENT;
            break;
        }
        result = nclp_stim_set_diagnostics(command->args[0],
                                           command->args[1], words);
        reply->status = stim_reply_status(result);
        reply_set_words(reply, words);
        break;
    }
    case NCLP_CMD_SET_INTAN_SYNC:
    {
        uint32_t words[4] = {0U, 0U, 0U, 0U};
        int result;

        if (!command_args_zero_from(command, 3U)) {
            reply->status = NCLP_COMMAND_STATUS_ARGUMENT;
            break;
        }
        if (context->stopped_configuration_allowed == 0) {
            reply->status = NCLP_COMMAND_STATUS_BUSY;
            break;
        }
        result = nclp_intan_sync_set(command->args[0], command->args[1],
                                       command->args[2], words);
        reply->status = intan_sync_reply_status(result);
        reply_set_words(reply, words);
        break;
    }
    case NCLP_CMD_GET_INTAN_SYNC:
    {
        uint32_t words[4];

        if (!command_args_zero_from(command, 0U)) {
            reply->status = NCLP_COMMAND_STATUS_ARGUMENT;
            break;
        }
        nclp_intan_sync_read(words);
        reply_set_words(reply, words);
        break;
    }
    case NCLP_CMD_RIPPLE_GET_CONFIG:
    {
        uint32_t words[4] = {0};
        if (!command_args_zero_from(command, 1U)) {
            reply->status = NCLP_COMMAND_STATUS_ARGUMENT; break;
        }
        reply->status = ripple_reply_status(
            ripple_get_config_page(command->args[0], words));
        reply_set_words(reply, words);
        break;
    }
    case NCLP_CMD_RIPPLE_APPLY_PROFILE:
    {
        nclp_ripple_profile_t profile;
        int result;
        if (!command_args_zero_from(command, 10U)) {
            reply->status = NCLP_COMMAND_STATUS_ARGUMENT; break;
        }
        if (context->stopped_configuration_allowed == 0) {
            reply->status = NCLP_COMMAND_STATUS_BUSY; break;
        }
        result = nclp_ripple_service_get_profile(&profile);
        if (result == NCLP_RIPPLE_DETECTOR_OK) {
            profile.detected_channel_index = command->args[0];
            profile.filter_kind = command->args[1];
            profile.low_cutoff_millihz = command->args[2];
            profile.high_cutoff_millihz = command->args[3];
            profile.fir_tap_count = command->args[4];
            profile.power_window_us = command->args[5];
            profile.refractory_period_ms = command->args[6];
            profile.baseline_mean_bits = command->args[7];
            profile.baseline_stddev_bits = command->args[8];
            profile.threshold_k_bits = command->args[9];
            result = nclp_ripple_service_apply_profile(
                &profile, context->physical_chip_mask,
                context->packed_chip_ids);
        }
        reply->status = ripple_reply_status(result);
        if (result == NCLP_RIPPLE_DETECTOR_OK) {
            result = nclp_ripple_service_get_profile(&profile);
            reply->status = ripple_reply_status(result);
            reply->data0 = profile.detected_channel_index;
            reply->data1 = profile.input_channel_id;
            reply->data2 = profile.filter_kind;
            reply->data3 = profile.fir_tap_count;
        }
        break;
    }
    case NCLP_CMD_RIPPLE_SET_K:
    {
        uint32_t words[4] = {0};
        int result;
        if (!command_args_zero_from(command, 1U)) {
            reply->status = NCLP_COMMAND_STATUS_ARGUMENT; break;
        }
        if (context->operation_state == NCLP_STATE_FAULT) {
            reply->status = NCLP_COMMAND_STATUS_BUSY; break;
        }
        result = nclp_ripple_service_set_k(command->args[0]);
        if (result == NCLP_RIPPLE_DETECTOR_OK)
            result = ripple_get_config_page(
                NCLP_RIPPLE_CONFIG_THRESHOLD, words);
        reply->status = ripple_reply_status(result);
        reply_set_words(reply, words);
        break;
    }
    case NCLP_CMD_RIPPLE_BASELINE:
    {
        int result;
        if (command->args[0] > NCLP_RIPPLE_BASELINE_START) {
            reply->status = NCLP_COMMAND_STATUS_ARGUMENT; break;
        }
        if (!command_args_zero_from(
                command,
                command->args[0] == NCLP_RIPPLE_BASELINE_START ? 2U : 1U)) {
            reply->status = NCLP_COMMAND_STATUS_ARGUMENT; break;
        }
        if (context->operation_state == NCLP_STATE_FAULT &&
            command->args[0] != NCLP_RIPPLE_BASELINE_CANCEL) {
            reply->status = NCLP_COMMAND_STATUS_BUSY; break;
        }
        result = command->args[0] == NCLP_RIPPLE_BASELINE_START ?
            nclp_ripple_service_start_baseline(command->args[1],
                context->local_detector_stream_active) :
            nclp_ripple_service_cancel_baseline();
        reply->status = ripple_reply_status(result);
        if (result == NCLP_RIPPLE_DETECTOR_OK) {
            nclp_ripple_baseline_status_t status;
            result = nclp_ripple_service_get_baseline_status(&status);
            reply->status = ripple_reply_status(result);
            reply->data0 = status.state;
            reply->data1 = status.collected_samples;
            reply->data2 = status.target_samples;
            reply->data3 = status.generation;
        }
        break;
    }
    case NCLP_CMD_RIPPLE_STATUS:
    {
        nclp_ripple_detector_runtime_t runtime = {0};
        nclp_ripple_baseline_status_t baseline_status = {0};
        if (!command_args_zero_from(command, 1U) ||
            command->args[0] > NCLP_RIPPLE_STATUS_BASELINE) {
            reply->status = NCLP_COMMAND_STATUS_ARGUMENT; break;
        }
        reply->status = ripple_reply_status(
            nclp_ripple_service_get_runtime(&runtime));
        if (reply->status == NCLP_COMMAND_STATUS_OK)
            reply->status = ripple_reply_status(
                nclp_ripple_service_get_baseline_status(&baseline_status));
        if (command->args[0] == NCLP_RIPPLE_STATUS_RUNTIME) {
            reply->data0 = runtime.status;
            reply->data1 = runtime.trigger_request_count;
            reply->data2 = runtime.power_sample_count;
            reply->data3 = baseline_status.state;
        } else if (command->args[0] == NCLP_RIPPLE_STATUS_SIGNAL) {
            reply->data0 = runtime.mean_square;
            reply->data1 = runtime.sum_square_threshold_bits;
            reply->data2 = runtime.last_input_timestamp;
            reply->data3 = baseline_status.collected_samples;
        } else {
            reply->data0 = baseline_status.target_samples;
            reply->data1 = baseline_status.generation;
            reply->data2 = baseline_status.error;
            reply->data3 = baseline_status.missed_samples;
        }
        break;
    }
    case NCLP_CMD_RIPPLE_CONTROL:
    {
        nclp_ripple_detector_runtime_t runtime = {0};
        if (!command_args_zero_from(command, 1U) ||
            command->args[0] > NCLP_RIPPLE_ACTION_START) {
            reply->status = NCLP_COMMAND_STATUS_ARGUMENT; break;
        }
        if (context->operation_state == NCLP_STATE_FAULT &&
            command->args[0] != NCLP_RIPPLE_ACTION_STOP) {
            reply->status = NCLP_COMMAND_STATUS_BUSY; break;
        }
        reply->status = ripple_reply_status(nclp_ripple_service_control(
            (nclp_ripple_detector_action_t)command->args[0], &runtime));
        reply->data0 = runtime.status;
        reply->data1 = runtime.trigger_request_count;
        reply->data2 = runtime.power_sample_count;
        break;
    }
    default: return 0;
    }
    return 1;
}
