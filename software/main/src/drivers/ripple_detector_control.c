#include "ripple_detector_control.h"
#include "../../../common/nclp_pl_registers.h"
#include "xil_io.h"
#include <stddef.h>
#include <stdint.h>
#include <string.h>
#include <math.h>

#define RIPPLE_POLL_LIMIT 100000U
static uint32_t initialized;
static uint32_t rd(uint32_t offset)
{
    return Xil_In32((UINTPTR)(NCLP_RIPPLE_DETECTOR_BASE_DEFAULT + offset));
}
static void wr(uint32_t offset, uint32_t value)
{
    Xil_Out32((UINTPTR)(NCLP_RIPPLE_DETECTOR_BASE_DEFAULT + offset), value);
}
static float decode_float(uint32_t bits)
{
    float value;
    memcpy(&value, &bits, sizeof value);
    return value;
}
/* Convert the public FP32 value as the fixed datapath does: round to nearest,
 * ties to even.  The explicit half-open range and rounded-result checks keep
 * values that cannot be represented by the documented Q format out. */
static int quantize_signed(float value, uint32_t fractional_bits,
                           int64_t minimum_raw, int64_t upper_raw,
                           int64_t *result)
{
    const double scale = (double)(UINT64_C(1) << fractional_bits);
    double scaled, remainder;
    int64_t quantized;
    if (!result || !isfinite(value)) return 0;
    scaled = (double)value * scale;
    if (scaled < (double)minimum_raw || scaled >= (double)upper_raw) return 0;
    quantized = (int64_t)scaled;
    remainder = scaled - (double)quantized;
    if (remainder > 0.5 || (remainder == 0.5 && quantized % 2 != 0))
        ++quantized;
    else if (remainder < -0.5 || (remainder == -0.5 && quantized % 2 != 0))
        --quantized;
    if (quantized < minimum_raw || quantized >= upper_raw) return 0;
    *result = quantized;
    return 1;
}
static int quantize_q16_16(float value, int64_t *result)
{
    return quantize_signed(value, 16U, -INT64_C(2147483648),
                           INT64_C(2147483648), result);
}
static int valid_quantized_theta(int64_t mu, int64_t sigma, int64_t k)
{
    const int64_t scale = INT64_C(65536);
    int64_t product = sigma * k;
    int64_t scaled_product = product / scale;
    int64_t remainder = product % scale;
    int64_t theta;
    /* sigma and K are nonnegative, so this is convergent Q16.16 rounding. */
    if (remainder > scale / 2 ||
        (remainder == scale / 2 && scaled_product % 2 != 0))
        ++scaled_product;
    theta = mu + scaled_product;
    return theta >= 0 && theta <= INT64_C(2147483648);
}
static int wait_idle(void)
{
    for (uint32_t index = 0; index < RIPPLE_POLL_LIMIT; ++index)
        if ((rd(RIPPLE_DETECTOR_REG_STATUS) & RIPPLE_DETECTOR_STATUS_BUSY) == 0U)
            return NCLP_RIPPLE_DETECTOR_OK;
    return NCLP_RIPPLE_DETECTOR_ERROR_BUSY;
}
static int stopped_idle(void)
{
    if (!initialized) return NCLP_RIPPLE_DETECTOR_ERROR_NOT_READY;
    return (rd(RIPPLE_DETECTOR_REG_STATUS) &
            (RIPPLE_DETECTOR_STATUS_ENABLED | RIPPLE_DETECTOR_STATUS_BUSY)) ?
           NCLP_RIPPLE_DETECTOR_ERROR_BUSY : NCLP_RIPPLE_DETECTOR_OK;
}
static int valid_config(const nclp_ripple_detector_config_t *c)
{
    float mu, sigma, k;
    int64_t mu_raw, sigma_raw, k_raw;
    if (!c || c->input_channel_id > RIPPLE_DETECTOR_INPUT_CHANNEL_ID_MAX ||
        c->filter_kind > RIPPLE_DETECTOR_FILTER_IIR ||
        c->fir_tap_count == 0U || c->fir_tap_count > RIPPLE_DETECTOR_FIR_TAP_COUNT_MAX ||
        c->power_window_us < RIPPLE_DETECTOR_POWER_WINDOW_US_MIN ||
        c->power_window_us > RIPPLE_DETECTOR_POWER_WINDOW_US_MAX ||
        c->refractory_period_ms > RIPPLE_DETECTOR_REFRACTORY_PERIOD_MS_MAX) return 0;
    mu = decode_float(c->baseline_mean_bits); sigma = decode_float(c->baseline_stddev_bits);
    k = decode_float(c->threshold_k_bits);
    return sigma >= 0.0f && k >= 0.0f &&
           quantize_q16_16(mu, &mu_raw) &&
           quantize_q16_16(sigma, &sigma_raw) &&
           quantize_q16_16(k, &k_raw) &&
           valid_quantized_theta(mu_raw, sigma_raw, k_raw);
}
int nclp_ripple_detector_check_config(
    const nclp_ripple_detector_config_t *c)
{
    return valid_config(c) ? NCLP_RIPPLE_DETECTOR_OK :
                             NCLP_RIPPLE_DETECTOR_ERROR_ARGUMENT;
}
static int verified_write(uint32_t offset, uint32_t value)
{
    wr(offset, value);
    return rd(offset) == value ? NCLP_RIPPLE_DETECTOR_OK :
                                NCLP_RIPPLE_DETECTOR_ERROR_HARDWARE;
}
int nclp_ripple_detector_get_config(nclp_ripple_detector_config_t *c)
{
    if (!c) return NCLP_RIPPLE_DETECTOR_ERROR_ARGUMENT;
    if (!initialized) return NCLP_RIPPLE_DETECTOR_ERROR_NOT_READY;
    c->input_channel_id = rd(RIPPLE_DETECTOR_REG_INPUT_CHANNEL_ID);
    c->fir_tap_count = rd(RIPPLE_DETECTOR_REG_FIR_TAP_COUNT);
    c->baseline_mean_bits = rd(RIPPLE_DETECTOR_REG_BASELINE_MEAN_BITS);
    c->baseline_stddev_bits = rd(RIPPLE_DETECTOR_REG_BASELINE_STDDEV_BITS);
    c->threshold_k_bits = rd(RIPPLE_DETECTOR_REG_THRESHOLD_K_BITS);
    c->power_window_us = rd(RIPPLE_DETECTOR_REG_POWER_WINDOW_US);
    c->refractory_period_ms = rd(RIPPLE_DETECTOR_REG_REFRACTORY_PERIOD_MS);
    c->filter_kind = rd(RIPPLE_DETECTOR_REG_FILTER_KIND);
    return NCLP_RIPPLE_DETECTOR_OK;
}
int nclp_ripple_detector_get_runtime(nclp_ripple_detector_runtime_t *r)
{
    if (!r) return NCLP_RIPPLE_DETECTOR_ERROR_ARGUMENT;
    if (!initialized) return NCLP_RIPPLE_DETECTOR_ERROR_NOT_READY;
    r->status = rd(RIPPLE_DETECTOR_REG_STATUS);
    r->trigger_request_count = rd(RIPPLE_DETECTOR_REG_TRIGGER_REQUEST_COUNT);
    r->power_sample_count = rd(RIPPLE_DETECTOR_REG_POWER_SAMPLE_COUNT);
    r->mean_square = rd(RIPPLE_DETECTOR_REG_MEAN_SQUARE);
    r->sum_square_threshold_bits = rd(RIPPLE_DETECTOR_REG_SUM_SQUARE_THRESHOLD_BITS);
    r->last_input_timestamp = rd(RIPPLE_DETECTOR_REG_LAST_INPUT_TIMESTAMP);
    return NCLP_RIPPLE_DETECTOR_OK;
}
int nclp_ripple_detector_control(nclp_ripple_detector_action_t action,
                                nclp_ripple_detector_runtime_t *runtime)
{
    nclp_ripple_detector_runtime_t live;
    nclp_ripple_detector_config_t config;
    int result;
    uint32_t status, clock_status;
    if (action != NCLP_RIPPLE_DETECTOR_STOP && action != NCLP_RIPPLE_DETECTOR_START &&
        action != NCLP_RIPPLE_DETECTOR_VALIDATE) return NCLP_RIPPLE_DETECTOR_ERROR_ARGUMENT;
    if (!initialized) return NCLP_RIPPLE_DETECTOR_ERROR_NOT_READY;
    if (action != NCLP_RIPPLE_DETECTOR_STOP) {
        result = stopped_idle();
        if (result) return result;
        if (nclp_ripple_detector_get_config(&config) || !valid_config(&config))
            return NCLP_RIPPLE_DETECTOR_ERROR_ARGUMENT;
        if (action == NCLP_RIPPLE_DETECTOR_START) {
            /* Use the actual Intan PLL profile and readiness, never infer rate
             * from AMP payload contents or a cached requested sample rate. */
            clock_status = Xil_In32((UINTPTR)(NCLP_INTAN_BASE_DEFAULT +
                                      INTAN_REG_SAMPLE_CLOCK_STATUS));
            if (Xil_In32((UINTPTR)(NCLP_INTAN_BASE_DEFAULT + INTAN_REG_SAMPLE_CLOCK_CONFIG)) !=
                    INTAN_SAMPLE_CLOCK_CONFIG_PACK(15U, 4U, 36U) ||
                (clock_status & (INTAN_SAMPLE_CLOCK_STATUS_LOCKED |
                                 INTAN_SAMPLE_CLOCK_STATUS_READY |
                                 INTAN_SAMPLE_CLOCK_STATUS_BUSY |
                                 INTAN_SAMPLE_CLOCK_STATUS_ERROR)) !=
                    (INTAN_SAMPLE_CLOCK_STATUS_LOCKED | INTAN_SAMPLE_CLOCK_STATUS_READY) ||
                !(rd(RIPPLE_DETECTOR_REG_STATUS) & RIPPLE_DETECTOR_STATUS_RATE_30K_VALID))
                return NCLP_RIPPLE_DETECTOR_ERROR_RATE;
        }
    }
    wr(RIPPLE_DETECTOR_REG_COMMAND, (uint32_t)action);
    result = wait_idle();
    if (result) return result;
    if (nclp_ripple_detector_get_runtime(&live)) return NCLP_RIPPLE_DETECTOR_ERROR_HARDWARE;
    if (runtime) *runtime = live;
    status = live.status;
    if (action == NCLP_RIPPLE_DETECTOR_STOP)
        return !(status & RIPPLE_DETECTOR_STATUS_ENABLED) ?
               NCLP_RIPPLE_DETECTOR_OK : NCLP_RIPPLE_DETECTOR_ERROR_HARDWARE;
    if (!(status & RIPPLE_DETECTOR_STATUS_CONFIG_VALID) ||
        (status & (RIPPLE_DETECTOR_STATUS_FAULT | RIPPLE_DETECTOR_STATUS_WRITE_REJECT)))
        return NCLP_RIPPLE_DETECTOR_ERROR_HARDWARE;
    return (action == NCLP_RIPPLE_DETECTOR_VALIDATE ||
            (status & RIPPLE_DETECTOR_STATUS_ENABLED)) ?
           NCLP_RIPPLE_DETECTOR_OK : NCLP_RIPPLE_DETECTOR_ERROR_HARDWARE;
}
int nclp_ripple_detector_set_config(const nclp_ripple_detector_config_t *c)
{
    const uint32_t offsets[] = {RIPPLE_DETECTOR_REG_INPUT_CHANNEL_ID, RIPPLE_DETECTOR_REG_FIR_TAP_COUNT,
        RIPPLE_DETECTOR_REG_BASELINE_MEAN_BITS, RIPPLE_DETECTOR_REG_BASELINE_STDDEV_BITS, RIPPLE_DETECTOR_REG_THRESHOLD_K_BITS,
        RIPPLE_DETECTOR_REG_POWER_WINDOW_US, RIPPLE_DETECTOR_REG_REFRACTORY_PERIOD_MS,
        RIPPLE_DETECTOR_REG_FILTER_KIND};
    uint32_t values[8];
    int result;
    if (nclp_ripple_detector_check_config(c) != NCLP_RIPPLE_DETECTOR_OK)
        return NCLP_RIPPLE_DETECTOR_ERROR_ARGUMENT;
    result = stopped_idle(); if (result) return result;
    values[0]=c->input_channel_id; values[1]=c->fir_tap_count; values[2]=c->baseline_mean_bits;
    values[3]=c->baseline_stddev_bits; values[4]=c->threshold_k_bits; values[5]=c->power_window_us; values[6]=c->refractory_period_ms;
    values[7]=c->filter_kind;
    for (uint32_t index=0; index<8U; ++index) {
        result = verified_write(offsets[index], values[index]);
        if (result) return result;
    }
    return nclp_ripple_detector_control(NCLP_RIPPLE_DETECTOR_VALIDATE, NULL);
}
int nclp_ripple_detector_set_coefficient(uint32_t index, uint32_t bits)
{
    int64_t quantized;
    float value;
    int result;
    if (index >= RIPPLE_DETECTOR_COEFFICIENT_COUNT)
        return NCLP_RIPPLE_DETECTOR_ERROR_ARGUMENT;
    value = decode_float(bits);
    if (index < RIPPLE_DETECTOR_IIR_COEFFICIENT_BASE) {
        if (!quantize_signed(value, 17U, -INT64_C(131072),
                             INT64_C(131072), &quantized))
            return NCLP_RIPPLE_DETECTOR_ERROR_ARGUMENT;
    } else if (!quantize_signed(value, 16U, -INT64_C(131072),
                                INT64_C(131072), &quantized)) {
        return NCLP_RIPPLE_DETECTOR_ERROR_ARGUMENT;
    }
    result=stopped_idle(); if (result) return result;
    result=verified_write(RIPPLE_DETECTOR_REG_COEFFICIENT_INDEX,index); if (result) return result;
    wr(RIPPLE_DETECTOR_REG_COEFFICIENT_BITS,bits);
    result=wait_idle(); if (result) return result;
    return (rd(RIPPLE_DETECTOR_REG_COEFFICIENT_BITS)==bits &&
            !(rd(RIPPLE_DETECTOR_REG_STATUS) &
              (RIPPLE_DETECTOR_STATUS_FAULT | RIPPLE_DETECTOR_STATUS_WRITE_REJECT))) ?
           NCLP_RIPPLE_DETECTOR_OK : NCLP_RIPPLE_DETECTOR_ERROR_HARDWARE;
}
int nclp_ripple_detector_set_k(uint32_t bits)
{
    nclp_ripple_detector_config_t config;
    uint32_t status, was_enabled;
    int result = nclp_ripple_detector_get_config(&config);
    if (result) return result;
    config.threshold_k_bits = bits;
    if (!valid_config(&config)) return NCLP_RIPPLE_DETECTOR_ERROR_ARGUMENT;
    was_enabled = rd(RIPPLE_DETECTOR_REG_STATUS) &
                  RIPPLE_DETECTOR_STATUS_ENABLED;
    /* A streaming write queues an atomic threshold update behind the current
     * sample. Do not issue STOP, reset histories, or rewrite other parameters. */
    wr(RIPPLE_DETECTOR_REG_THRESHOLD_K_BITS, bits);
    result = wait_idle();
    if (result) return result;
    status = rd(RIPPLE_DETECTOR_REG_STATUS);
    if (rd(RIPPLE_DETECTOR_REG_THRESHOLD_K_BITS) != bits)
        return NCLP_RIPPLE_DETECTOR_ERROR_HARDWARE;
    /* A stopped write invalidates the staged configuration. VALIDATE clears
     * prior sticky diagnostics and verifies this new configuration first. */
    if (!was_enabled)
        return nclp_ripple_detector_control(NCLP_RIPPLE_DETECTOR_VALIDATE, NULL);
    if (status & (RIPPLE_DETECTOR_STATUS_FAULT | RIPPLE_DETECTOR_STATUS_WRITE_REJECT))
        return NCLP_RIPPLE_DETECTOR_ERROR_HARDWARE;
    return (status & (RIPPLE_DETECTOR_STATUS_ENABLED | RIPPLE_DETECTOR_STATUS_CONFIG_VALID)) ==
           (RIPPLE_DETECTOR_STATUS_ENABLED | RIPPLE_DETECTOR_STATUS_CONFIG_VALID) ?
           NCLP_RIPPLE_DETECTOR_OK : NCLP_RIPPLE_DETECTOR_ERROR_HARDWARE;
}
int nclp_ripple_detector_reset_defaults(void)
{
    const nclp_ripple_detector_config_t defaults = {
        .input_channel_id = 0U,
        .filter_kind = RIPPLE_DETECTOR_FILTER_FIR,
        .fir_tap_count = RIPPLE_DETECTOR_FIR_TAP_COUNT_DEFAULT,
        .baseline_mean_bits = 0U,
        .baseline_stddev_bits = 0x3F800000U,
        .threshold_k_bits = 0x40800000U,
        .power_window_us = RIPPLE_DETECTOR_POWER_WINDOW_US_DEFAULT,
        .refractory_period_ms = RIPPLE_DETECTOR_REFRACTORY_PERIOD_MS_DEFAULT
    };
    int result=nclp_ripple_detector_control(NCLP_RIPPLE_DETECTOR_STOP,NULL);
    if (result) return result;
    result = nclp_ripple_detector_set_config(&defaults);
    if (result) return result;
    wr(RIPPLE_DETECTOR_REG_COMMAND, RIPPLE_DETECTOR_COMMAND_RESTORE_DEFAULTS);
    result = wait_idle();
    if (result) return result;
    return (rd(RIPPLE_DETECTOR_REG_STATUS) &
            (RIPPLE_DETECTOR_STATUS_CONFIG_VALID | RIPPLE_DETECTOR_STATUS_ENABLED |
             RIPPLE_DETECTOR_STATUS_FAULT | RIPPLE_DETECTOR_STATUS_WRITE_REJECT)) ==
            RIPPLE_DETECTOR_STATUS_CONFIG_VALID ? NCLP_RIPPLE_DETECTOR_OK :
            NCLP_RIPPLE_DETECTOR_ERROR_HARDWARE;
}
void nclp_ripple_detector_emergency_stop(void)
{
    if (initialized) wr(RIPPLE_DETECTOR_REG_COMMAND,0U);
}
int nclp_ripple_detector_init(void)
{
    initialized=0U;
    if (rd(RIPPLE_DETECTOR_REG_BLOCK_ID)!=RIPPLE_DETECTOR_BLOCK_ID_EXPECTED ||
        (rd(RIPPLE_DETECTOR_REG_ABI_VERSION)&NCLP_ABI_MAJOR_MASK)!=
            (RIPPLE_DETECTOR_ABI_VERSION_EXPECTED&NCLP_ABI_MAJOR_MASK) ||
        (rd(RIPPLE_DETECTOR_REG_CAPABILITIES)&RIPPLE_DETECTOR_CAPABILITIES_REQUIRED)!=
             RIPPLE_DETECTOR_CAPABILITIES_REQUIRED ||
        rd(RIPPLE_DETECTOR_REG_LIMITS)!=RIPPLE_DETECTOR_LIMITS_EXPECTED)
        return NCLP_RIPPLE_DETECTOR_ERROR_HARDWARE;
    initialized=1U;
    if (nclp_ripple_detector_reset_defaults()) {
        wr(RIPPLE_DETECTOR_REG_COMMAND,0U); initialized=0U;
        return NCLP_RIPPLE_DETECTOR_ERROR_HARDWARE;
    }
    return NCLP_RIPPLE_DETECTOR_OK;
}
