#include "ripple_detector_service.h"
#include "ripple_filter_design.h"
#include "../../../common/nclp_pl_registers.h"
#include "../../../common/nclp_wire.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

#define RIPPLE_OUTPUT_SAMPLES_PER_MILLISECOND 3U

__extension__ typedef unsigned __int128 nclp_uint128_t;

typedef struct {
    nclp_ripple_baseline_status_t public_status;
    nclp_ripple_profile_t saved_profile;
    uint32_t last_power_sample_count;
    uint64_t amplitude_sum_q16;
    nclp_uint128_t amplitude_square_sum_q32;
} baseline_context_t;

static uint32_t initialized;
static uint32_t channel_mapping_valid;
static uint32_t topology_physical_chip_mask;
static uint32_t topology_packed_chip_ids;
static nclp_ripple_profile_t active_profile;
static baseline_context_t baseline;
static int32_t coefficient_buffer[NCLP_RIPPLE_FIR_TAPS_MAX];

static void clear_baseline_result(void)
{
    const uint32_t generation = baseline.public_status.generation;
    memset(&baseline.public_status, 0, sizeof baseline.public_status);
    baseline.public_status.state = NCLP_RIPPLE_BASELINE_STATE_IDLE;
    baseline.public_status.generation = generation;
}

static uint32_t fp32_bits(float value)
{
    uint32_t bits;
    memcpy(&bits, &value, sizeof bits);
    return bits;
}

static uint32_t q16_to_fp32_bits(uint32_t value_q16)
{
    return fp32_bits((float)((double)value_q16 / 65536.0));
}

static uint64_t divide_round_nearest_even_u128(nclp_uint128_t numerator,
                                               uint64_t denominator)
{
    const nclp_uint128_t quotient = numerator / denominator;
    const nclp_uint128_t remainder = numerator % denominator;
    const nclp_uint128_t half = denominator / 2U;
    nclp_uint128_t rounded = quotient;

    if (remainder > half ||
        ((denominator & 1U) == 0U && remainder == half &&
         (quotient & 1U) != 0U)) {
        ++rounded;
    }
    return rounded > UINT64_MAX ? UINT64_MAX : (uint64_t)rounded;
}

static uint64_t integer_sqrt_floor(uint64_t value)
{
    uint64_t result = 0U;
    uint64_t bit = UINT64_C(1) << 62U;

    while (bit > value) {
        bit >>= 2U;
    }
    while (bit != 0U) {
        if (value >= result + bit) {
            value -= result + bit;
            result = (result >> 1U) + bit;
        } else {
            result >>= 1U;
        }
        bit >>= 2U;
    }
    return result;
}

static uint32_t integer_sqrt_round_nearest_even(uint64_t value)
{
    uint64_t floor_value = integer_sqrt_floor(value);
    const uint64_t floor_square = floor_value * floor_value;
    const uint64_t lower_distance = value - floor_square;
    const uint64_t next = floor_value + 1U;
    const uint64_t upper_distance = next * next - value;

    if (upper_distance < lower_distance ||
        (upper_distance == lower_distance && (floor_value & 1U) != 0U)) {
        ++floor_value;
    }
    return floor_value > UINT32_MAX ? UINT32_MAX : (uint32_t)floor_value;
}

static uint32_t chip_channel_count_from_id(uint32_t chip_id)
{
    if (chip_id == 1U) return 32U;
    if (chip_id == 2U) return 16U;
    if (chip_id == 4U) return 64U;
    return 0U;
}

static int validate_topology(uint32_t physical_chip_mask,
                             uint32_t packed_chip_ids,
                             uint32_t *channel_count)
{
    uint32_t total = 0U;

    if (channel_count == NULL || (physical_chip_mask & ~0xFFU) != 0U)
        return NCLP_RIPPLE_DETECTOR_ERROR_ARGUMENT;
    for (uint32_t physical = 0U; physical < 8U; ++physical) {
        const uint32_t selected = physical_chip_mask & (1U << physical);
        const uint32_t chip_id = (packed_chip_ids >> (physical * 4U)) & 0xFU;
        const uint32_t channels = chip_channel_count_from_id(chip_id);

        if (selected == 0U) {
            if (chip_id != 0U) return NCLP_RIPPLE_DETECTOR_ERROR_ARGUMENT;
        } else {
            if (channels == 0U) return NCLP_RIPPLE_DETECTOR_ERROR_ARGUMENT;
            total += channels;
        }
    }
    if (total == 0U)
        return NCLP_RIPPLE_DETECTOR_ERROR_ARGUMENT;
    *channel_count = total;
    return NCLP_RIPPLE_DETECTOR_OK;
}

int nclp_ripple_service_map_channel(uint32_t detected_channel_index,
                                    uint32_t physical_chip_mask,
                                    uint32_t packed_chip_ids,
                                    uint32_t *input_channel_id)
{
    uint32_t remaining = detected_channel_index;

    if (input_channel_id == NULL || (physical_chip_mask & ~0xFFU) != 0U) {
        return NCLP_RIPPLE_DETECTOR_ERROR_ARGUMENT;
    }
    for (uint32_t physical = 0U; physical < 8U; ++physical) {
        const uint32_t selected = physical_chip_mask & (1U << physical);
        const uint32_t chip_id = (packed_chip_ids >> (physical * 4U)) & 0xFU;
        const uint32_t channels = chip_channel_count_from_id(chip_id);

        if (selected == 0U) {
            if (chip_id != 0U) return NCLP_RIPPLE_DETECTOR_ERROR_ARGUMENT;
            continue;
        }
        if (channels == 0U) return NCLP_RIPPLE_DETECTOR_ERROR_ARGUMENT;
        if (remaining < channels) {
            const uint32_t logical_slot = 2U * physical +
                ((chip_id == 4U && remaining >= 32U) ? 1U : 0U);
            *input_channel_id = (logical_slot << 5U) | (remaining & 31U);
            return NCLP_RIPPLE_DETECTOR_OK;
        }
        remaining -= channels;
    }
    return NCLP_RIPPLE_DETECTOR_ERROR_ARGUMENT;
}

static int profile_to_hardware(const nclp_ripple_profile_t *profile,
                               nclp_ripple_detector_config_t *hardware)
{
    if (profile == NULL || hardware == NULL ||
        profile->filter_kind > NCLP_RIPPLE_FILTER_FIXED_IIR ||
        profile->fir_tap_count < NCLP_RIPPLE_FIR_TAPS_MIN ||
        profile->fir_tap_count > NCLP_RIPPLE_FIR_TAPS_MAX ||
        profile->low_cutoff_millihz == 0U ||
        profile->low_cutoff_millihz >= profile->high_cutoff_millihz ||
        profile->high_cutoff_millihz >= 1500000U ||
        profile->baseline_duration_ms < NCLP_RIPPLE_BASELINE_DURATION_MS_MIN ||
        profile->baseline_duration_ms > NCLP_RIPPLE_BASELINE_DURATION_MS_MAX) {
        return NCLP_RIPPLE_DETECTOR_ERROR_ARGUMENT;
    }
    if (profile->filter_kind == NCLP_RIPPLE_FILTER_FIXED_IIR &&
        (profile->low_cutoff_millihz != NCLP_RIPPLE_FIXED_IIR_LOW_MILLIHZ ||
         profile->high_cutoff_millihz != NCLP_RIPPLE_FIXED_IIR_HIGH_MILLIHZ)) {
        return NCLP_RIPPLE_DETECTOR_ERROR_ARGUMENT;
    }
    hardware->input_channel_id = profile->input_channel_id;
    hardware->filter_kind = profile->filter_kind == NCLP_RIPPLE_FILTER_FIXED_IIR ?
        RIPPLE_DETECTOR_FILTER_IIR : RIPPLE_DETECTOR_FILTER_FIR;
    hardware->fir_tap_count = profile->fir_tap_count;
    hardware->power_window_us = profile->power_window_us;
    hardware->refractory_period_ms = profile->refractory_period_ms;
    hardware->baseline_mean_bits = profile->baseline_mean_bits;
    hardware->baseline_stddev_bits = profile->baseline_stddev_bits;
    hardware->threshold_k_bits = profile->threshold_k_bits;
    return nclp_ripple_detector_check_config(hardware);
}

static int prepare_filter(const nclp_ripple_profile_t *profile,
                          uint32_t *count, uint32_t *base)
{
    if (profile == NULL || count == NULL || base == NULL)
        return NCLP_RIPPLE_DETECTOR_ERROR_ARGUMENT;

    if (profile->filter_kind == NCLP_RIPPLE_FILTER_FIXED_IIR) {
        nclp_ripple_filter_fixed_iir(coefficient_buffer);
        *count = 10U;
        *base = RIPPLE_DETECTOR_IIR_COEFFICIENT_BASE;
    } else {
        if (nclp_ripple_filter_design_fir(
                profile->filter_kind, profile->low_cutoff_millihz,
                profile->high_cutoff_millihz, profile->fir_tap_count,
                coefficient_buffer) != NCLP_RIPPLE_FILTER_DESIGN_OK) {
            return NCLP_RIPPLE_DETECTOR_ERROR_ARGUMENT;
        }
        *count = profile->fir_tap_count;
        *base = 0U;
    }
    return NCLP_RIPPLE_DETECTOR_OK;
}

static int upload_prepared_filter(const nclp_ripple_profile_t *profile,
                                  uint32_t count, uint32_t base)
{
    for (uint32_t index = 0U; index < count; ++index) {
        const uint32_t fractional_bits =
            profile->filter_kind == NCLP_RIPPLE_FILTER_FIXED_IIR ? 16U : 17U;
        const float exact_value = ldexpf((float)coefficient_buffer[index],
                                         -(int)fractional_bits);
        int result = nclp_ripple_detector_set_coefficient(
            base + index, fp32_bits(exact_value));
        if (result != NCLP_RIPPLE_DETECTOR_OK) {
            return result;
        }
    }
    return NCLP_RIPPLE_DETECTOR_OK;
}

int nclp_ripple_service_init(void)
{
    int result;
    initialized = 0U;
    channel_mapping_valid = 0U;
    topology_physical_chip_mask = 0U;
    topology_packed_chip_ids = 0U;
    memset(&baseline, 0, sizeof baseline);
    result = nclp_ripple_detector_init();
    if (result != NCLP_RIPPLE_DETECTOR_OK) return result;

    active_profile.detected_channel_index = 0U;
    active_profile.input_channel_id = 0U;
    active_profile.filter_kind = NCLP_RIPPLE_FILTER_MINIMUM_FIR;
    active_profile.low_cutoff_millihz = NCLP_RIPPLE_FIXED_IIR_LOW_MILLIHZ;
    active_profile.high_cutoff_millihz = NCLP_RIPPLE_FIXED_IIR_HIGH_MILLIHZ;
    active_profile.fir_tap_count = 129U;
    active_profile.power_window_us = 4000U;
    active_profile.refractory_period_ms = 1000U;
    active_profile.baseline_mean_bits = fp32_bits(0.0f);
    active_profile.baseline_stddev_bits = fp32_bits(1.0f);
    active_profile.threshold_k_bits = fp32_bits(4.0f);
    active_profile.baseline_duration_ms =
        NCLP_RIPPLE_BASELINE_DURATION_MS_DEFAULT;
    baseline.public_status.state = NCLP_RIPPLE_BASELINE_STATE_IDLE;
    initialized = 1U;
    return NCLP_RIPPLE_DETECTOR_OK;
}

int nclp_ripple_service_update_topology(uint32_t physical_chip_mask,
                                        uint32_t packed_chip_ids)
{
    nclp_ripple_profile_t resolved;
    nclp_ripple_detector_config_t hardware;
    uint32_t channel_count;
    const uint32_t baseline_was_collecting =
        baseline.public_status.state == NCLP_RIPPLE_BASELINE_STATE_COLLECTING;
    int result;

    if (initialized == 0U) return NCLP_RIPPLE_DETECTOR_ERROR_NOT_READY;

    /* Invalidate first so every failure leaves START and baseline collection
     * fenced off from a channel ID resolved against an older scan. */
    channel_mapping_valid = 0U;
    topology_physical_chip_mask = 0U;
    topology_packed_chip_ids = 0U;
    if (baseline_was_collecting != 0U) {
        baseline.public_status.state = NCLP_RIPPLE_BASELINE_STATE_ERROR;
        baseline.public_status.error = NCLP_RIPPLE_BASELINE_ERROR_DETECTOR;
    }
    result = nclp_ripple_detector_control(NCLP_RIPPLE_DETECTOR_STOP, NULL);
    if (result != NCLP_RIPPLE_DETECTOR_OK) return result;

    if (physical_chip_mask == 0U && packed_chip_ids == 0U) {
        if (baseline_was_collecting == 0U) clear_baseline_result();
        return NCLP_RIPPLE_DETECTOR_OK;
    }
    result = validate_topology(physical_chip_mask, packed_chip_ids,
                               &channel_count);
    if (result != NCLP_RIPPLE_DETECTOR_OK) return result;

    resolved = active_profile;
    if (resolved.detected_channel_index >= channel_count)
        resolved.detected_channel_index = 0U;
    result = nclp_ripple_service_map_channel(
        resolved.detected_channel_index, physical_chip_mask, packed_chip_ids,
        &resolved.input_channel_id);
    if (result != NCLP_RIPPLE_DETECTOR_OK) return result;
    result = profile_to_hardware(&resolved, &hardware);
    if (result != NCLP_RIPPLE_DETECTOR_OK) return result;
    result = nclp_ripple_detector_set_config(&hardware);
    if (result != NCLP_RIPPLE_DETECTOR_OK) return result;

    active_profile = resolved;
    topology_physical_chip_mask = physical_chip_mask;
    topology_packed_chip_ids = packed_chip_ids;
    channel_mapping_valid = 1U;
    if (baseline_was_collecting == 0U) clear_baseline_result();
    return NCLP_RIPPLE_DETECTOR_OK;
}

int nclp_ripple_service_get_profile(nclp_ripple_profile_t *profile)
{
    if (profile == NULL) return NCLP_RIPPLE_DETECTOR_ERROR_ARGUMENT;
    if (initialized == 0U) return NCLP_RIPPLE_DETECTOR_ERROR_NOT_READY;
    *profile = active_profile;
    return NCLP_RIPPLE_DETECTOR_OK;
}

int nclp_ripple_service_apply_profile(const nclp_ripple_profile_t *profile,
                                      uint32_t physical_chip_mask,
                                      uint32_t packed_chip_ids)
{
    nclp_ripple_profile_t requested;
    nclp_ripple_detector_config_t hardware;
    uint32_t coefficient_count;
    uint32_t coefficient_base;
    int result;

    if (initialized == 0U) return NCLP_RIPPLE_DETECTOR_ERROR_NOT_READY;
    if (profile == NULL) return NCLP_RIPPLE_DETECTOR_ERROR_ARGUMENT;
    if (baseline.public_status.state == NCLP_RIPPLE_BASELINE_STATE_COLLECTING)
        return NCLP_RIPPLE_DETECTOR_ERROR_BUSY;
    if (channel_mapping_valid == 0U ||
        physical_chip_mask != topology_physical_chip_mask ||
        packed_chip_ids != topology_packed_chip_ids)
        return NCLP_RIPPLE_DETECTOR_ERROR_NOT_READY;
    requested = *profile;
    result = nclp_ripple_service_map_channel(
        requested.detected_channel_index, physical_chip_mask, packed_chip_ids,
        &requested.input_channel_id);
    if (result != NCLP_RIPPLE_DETECTOR_OK) return result;
    result = profile_to_hardware(&requested, &hardware);
    if (result != NCLP_RIPPLE_DETECTOR_OK) return result;
    result = prepare_filter(&requested, &coefficient_count, &coefficient_base);
    if (result != NCLP_RIPPLE_DETECTOR_OK) return result;
    result = nclp_ripple_detector_control(NCLP_RIPPLE_DETECTOR_STOP, NULL);
    if (result != NCLP_RIPPLE_DETECTOR_OK) return result;
    result = upload_prepared_filter(&requested, coefficient_count,
                                    coefficient_base);
    if (result != NCLP_RIPPLE_DETECTOR_OK) return result;
    result = nclp_ripple_detector_set_config(&hardware);
    if (result != NCLP_RIPPLE_DETECTOR_OK) return result;
    active_profile = requested;
    /* A completed calibration belongs to the profile/channel that produced
     * it. Keep its generation counter, but stop presenting the old result as
     * the baseline completion for a newly applied profile. */
    clear_baseline_result();
    return NCLP_RIPPLE_DETECTOR_OK;
}

int nclp_ripple_service_get_runtime(nclp_ripple_detector_runtime_t *runtime)
{
    return nclp_ripple_detector_get_runtime(runtime);
}

int nclp_ripple_service_get_baseline_status(
    nclp_ripple_baseline_status_t *status)
{
    if (status == NULL) return NCLP_RIPPLE_DETECTOR_ERROR_ARGUMENT;
    if (initialized == 0U) return NCLP_RIPPLE_DETECTOR_ERROR_NOT_READY;
    *status = baseline.public_status;
    return NCLP_RIPPLE_DETECTOR_OK;
}

int nclp_ripple_service_set_k(uint32_t threshold_k_bits)
{
    int result;
    if (baseline.public_status.state == NCLP_RIPPLE_BASELINE_STATE_COLLECTING)
        return NCLP_RIPPLE_DETECTOR_ERROR_BUSY;
    result = nclp_ripple_detector_set_k(threshold_k_bits);
    if (result == NCLP_RIPPLE_DETECTOR_OK) {
        active_profile.threshold_k_bits = threshold_k_bits;
    }
    return result;
}

static int restore_saved_profile(void)
{
    nclp_ripple_detector_config_t hardware;
    int result = nclp_ripple_detector_control(
        NCLP_RIPPLE_DETECTOR_STOP, NULL);
    if (result != NCLP_RIPPLE_DETECTOR_OK) return result;
    result = profile_to_hardware(&baseline.saved_profile, &hardware);
    if (result != NCLP_RIPPLE_DETECTOR_OK) return result;
    return nclp_ripple_detector_set_config(&hardware);
}

int nclp_ripple_service_cancel_baseline(void)
{
    int result;
    if (initialized == 0U) return NCLP_RIPPLE_DETECTOR_ERROR_NOT_READY;
    if (baseline.public_status.state != NCLP_RIPPLE_BASELINE_STATE_COLLECTING)
        return NCLP_RIPPLE_DETECTOR_ERROR_NOT_ACTIVE;
    result = restore_saved_profile();
    if (result == NCLP_RIPPLE_DETECTOR_OK) active_profile = baseline.saved_profile;
    baseline.public_status.state = result == NCLP_RIPPLE_DETECTOR_OK ?
        NCLP_RIPPLE_BASELINE_STATE_CANCELLED :
        NCLP_RIPPLE_BASELINE_STATE_ERROR;
    baseline.public_status.error = result == NCLP_RIPPLE_DETECTOR_OK ?
        NCLP_RIPPLE_BASELINE_ERROR_NONE : NCLP_RIPPLE_BASELINE_ERROR_DETECTOR;
    return result;
}

int nclp_ripple_service_control(nclp_ripple_detector_action_t action,
                                nclp_ripple_detector_runtime_t *runtime)
{
    if (action == NCLP_RIPPLE_DETECTOR_STOP &&
        baseline.public_status.state == NCLP_RIPPLE_BASELINE_STATE_COLLECTING) {
        int result = nclp_ripple_service_cancel_baseline();
        if (result == NCLP_RIPPLE_DETECTOR_OK && runtime != NULL)
            result = nclp_ripple_detector_get_runtime(runtime);
        return result;
    }
    if (baseline.public_status.state == NCLP_RIPPLE_BASELINE_STATE_COLLECTING)
        return NCLP_RIPPLE_DETECTOR_ERROR_BUSY;
    if (action == NCLP_RIPPLE_DETECTOR_START && channel_mapping_valid == 0U)
        return NCLP_RIPPLE_DETECTOR_ERROR_NOT_READY;
    return nclp_ripple_detector_control(action, runtime);
}

int nclp_ripple_service_start_baseline(uint32_t duration_ms,
                                       uint32_t local_detector_stream_active)
{
    nclp_ripple_detector_config_t hardware;
    nclp_ripple_detector_runtime_t runtime;
    int result;

    if (initialized == 0U) return NCLP_RIPPLE_DETECTOR_ERROR_NOT_READY;
    if (duration_ms < NCLP_RIPPLE_BASELINE_DURATION_MS_MIN ||
        duration_ms > NCLP_RIPPLE_BASELINE_DURATION_MS_MAX)
        return NCLP_RIPPLE_DETECTOR_ERROR_ARGUMENT;
    if (local_detector_stream_active == 0U)
        return NCLP_RIPPLE_DETECTOR_ERROR_RATE;
    if (baseline.public_status.state == NCLP_RIPPLE_BASELINE_STATE_COLLECTING)
        return NCLP_RIPPLE_DETECTOR_ERROR_BUSY;
    if (channel_mapping_valid == 0U)
        return NCLP_RIPPLE_DETECTOR_ERROR_NOT_READY;

    {
        uint32_t generation = baseline.public_status.generation + 1U;
        if (generation == 0U) generation = 1U;
        memset(&baseline, 0, sizeof baseline);
        baseline.public_status.generation = generation;
    }
    baseline.saved_profile = active_profile;
    baseline.saved_profile.baseline_duration_ms = duration_ms;
    baseline.public_status.target_samples =
        duration_ms * RIPPLE_OUTPUT_SAMPLES_PER_MILLISECOND;

    result = profile_to_hardware(&active_profile, &hardware);
    if (result != NCLP_RIPPLE_DETECTOR_OK) return result;
    /* Quantized theta is exactly 32768 while every filtered sample is bounded
     * to signed 16-bit, so strict sum > N*theta^2 cannot emit a trigger. */
    hardware.baseline_mean_bits = fp32_bits(32767.0f);
    hardware.baseline_stddev_bits = fp32_bits(1.0f);
    hardware.threshold_k_bits = fp32_bits(1.0f);
    result = nclp_ripple_detector_control(NCLP_RIPPLE_DETECTOR_STOP, NULL);
    if (result == NCLP_RIPPLE_DETECTOR_OK)
        result = nclp_ripple_detector_set_config(&hardware);
    if (result == NCLP_RIPPLE_DETECTOR_OK)
        result = nclp_ripple_detector_control(NCLP_RIPPLE_DETECTOR_START,
                                              &runtime);
    if (result != NCLP_RIPPLE_DETECTOR_OK) {
        (void)restore_saved_profile();
        baseline.public_status.state = NCLP_RIPPLE_BASELINE_STATE_ERROR;
        baseline.public_status.error = NCLP_RIPPLE_BASELINE_ERROR_DETECTOR;
        return result;
    }
    baseline.last_power_sample_count = runtime.power_sample_count;
    baseline.public_status.state = NCLP_RIPPLE_BASELINE_STATE_COLLECTING;
    active_profile.baseline_duration_ms = duration_ms;
    return NCLP_RIPPLE_DETECTOR_OK;
}

static void baseline_fail(uint32_t error)
{
    if (restore_saved_profile() != NCLP_RIPPLE_DETECTOR_OK)
        error = NCLP_RIPPLE_BASELINE_ERROR_DETECTOR;
    else
        active_profile = baseline.saved_profile;
    baseline.public_status.state = NCLP_RIPPLE_BASELINE_STATE_ERROR;
    baseline.public_status.error = error;
}

static void finish_baseline(void)
{
    const uint64_t sample_count = baseline.public_status.collected_samples;
    const uint64_t mean_q16 = divide_round_nearest_even_u128(
        baseline.amplitude_sum_q16, sample_count);
    const nclp_uint128_t variance_numerator =
        (nclp_uint128_t)sample_count * baseline.amplitude_square_sum_q32 -
        (nclp_uint128_t)baseline.amplitude_sum_q16 *
        baseline.amplitude_sum_q16;
    const uint64_t variance_q32 = divide_round_nearest_even_u128(
        variance_numerator, sample_count * sample_count);
    const uint32_t sigma_q16 = integer_sqrt_round_nearest_even(variance_q32);
    nclp_ripple_detector_config_t hardware;
    int result;

    baseline.saved_profile.baseline_mean_bits = q16_to_fp32_bits((uint32_t)mean_q16);
    baseline.saved_profile.baseline_stddev_bits = q16_to_fp32_bits(sigma_q16);
    result = nclp_ripple_detector_control(NCLP_RIPPLE_DETECTOR_STOP, NULL);
    if (result == NCLP_RIPPLE_DETECTOR_OK)
        result = profile_to_hardware(&baseline.saved_profile, &hardware);
    if (result == NCLP_RIPPLE_DETECTOR_OK)
        result = nclp_ripple_detector_set_config(&hardware);
    if (result != NCLP_RIPPLE_DETECTOR_OK) {
        baseline_fail(NCLP_RIPPLE_BASELINE_ERROR_DETECTOR);
        return;
    }
    active_profile = baseline.saved_profile;
    baseline.public_status.state = NCLP_RIPPLE_BASELINE_STATE_COMPLETE;
    baseline.public_status.error = NCLP_RIPPLE_BASELINE_ERROR_NONE;
}

void nclp_ripple_service_poll(void)
{
    nclp_ripple_detector_runtime_t first;
    nclp_ripple_detector_runtime_t second;
    uint32_t amplitude_q16;

    if (baseline.public_status.state != NCLP_RIPPLE_BASELINE_STATE_COLLECTING)
        return;
    if (nclp_ripple_detector_get_runtime(&first) != NCLP_RIPPLE_DETECTOR_OK ||
        nclp_ripple_detector_get_runtime(&second) != NCLP_RIPPLE_DETECTOR_OK) {
        baseline_fail(NCLP_RIPPLE_BASELINE_ERROR_DETECTOR);
        return;
    }
    /* The first snapshot is the candidate; the second count is its fence. */
    if (first.power_sample_count != second.power_sample_count) return;
    if ((second.status & (RIPPLE_DETECTOR_STATUS_FAULT |
                          RIPPLE_DETECTOR_STATUS_SESSION_CLOSED)) != 0U ||
        (second.status & RIPPLE_DETECTOR_STATUS_ENABLED) == 0U) {
        baseline_fail(NCLP_RIPPLE_BASELINE_ERROR_DETECTOR);
        return;
    }
    if ((second.status & RIPPLE_DETECTOR_STATUS_WARMUP) != 0U) {
        baseline.last_power_sample_count = first.power_sample_count;
        return;
    }
    if (first.power_sample_count == baseline.last_power_sample_count) return;
    if (first.power_sample_count != baseline.last_power_sample_count + 1U) {
        baseline.public_status.missed_samples +=
            first.power_sample_count - baseline.last_power_sample_count - 1U;
        baseline_fail(NCLP_RIPPLE_BASELINE_ERROR_SAMPLE_GAP);
        return;
    }

    baseline.last_power_sample_count = first.power_sample_count;
    amplitude_q16 = integer_sqrt_round_nearest_even(
        (uint64_t)first.mean_square << 32U);
    baseline.amplitude_sum_q16 += amplitude_q16;
    baseline.amplitude_square_sum_q32 +=
        (uint64_t)amplitude_q16 * amplitude_q16;
    baseline.public_status.collected_samples++;
    if (baseline.public_status.collected_samples >=
        baseline.public_status.target_samples) {
        finish_baseline();
    }
}

void nclp_ripple_service_emergency_stop(void)
{
    nclp_ripple_detector_emergency_stop();
    if (baseline.public_status.state == NCLP_RIPPLE_BASELINE_STATE_COLLECTING) {
        if (restore_saved_profile() == NCLP_RIPPLE_DETECTOR_OK)
            active_profile = baseline.saved_profile;
        baseline.public_status.state = NCLP_RIPPLE_BASELINE_STATE_ERROR;
        baseline.public_status.error = NCLP_RIPPLE_BASELINE_ERROR_DETECTOR;
    }
}
