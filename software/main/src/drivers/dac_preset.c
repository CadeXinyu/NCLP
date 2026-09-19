#include "dac_preset.h"
#include "../../../common/nclp_wire.h"
#include "../../../common/nclp_pl_registers.h"
#include <math.h>
#include <stddef.h>

#define TWO_PI 6.283185307179586476925286766559

int nclp_dac_plan_preset(const nclp_stim_preset_config_t *config,
                         uint32_t engine_hz, nclp_dac_preset_plan_t *plan)
{
    if (config == NULL || plan == NULL || config->kind >= NCLP_DAC_PRESET_COUNT ||
        config->channel_mask < 1U || config->channel_mask > 3U ||
        config->external_trigger_enable > 1U || config->repeat_count == 0U ||
        config->minimum_a_uv > config->maximum_a_uv ||
        config->maximum_a_uv > NCLP_DAC_REFERENCE_UV ||
        config->minimum_b_uv > config->maximum_b_uv ||
        config->maximum_b_uv > NCLP_DAC_REFERENCE_UV ||
        engine_hz == 0U || engine_hz > 40000000U) return 0;
    switch (config->update_rate_hz) {
    case 2000U: case 2500U: case 5000U: case 10000U: case 20000U: case 30000U: break;
    default: return 0;
    }
    const uint32_t update_period =
        (engine_hz + config->update_rate_hz / 2U) / config->update_rate_hz;
    /* Two 16-bit frames at engine/4 plus engine/CDC bookkeeping. */
    if (update_period < 256U) return 0;
    const double update_rate = (double)engine_hz / update_period;
    uint32_t sample_count = 1U;
    uint64_t finite_updates = 1U;
    if (config->kind == NCLP_DAC_PRESET_SINE) {
        if (config->parameter == 0U) return 0;
        const double count = update_rate * 1000.0 / config->parameter;
        if (count < NCLP_DAC_SINE_MIN_SAMPLES - 0.5 ||
            count >= STIM_RAM_DEPTH_WORDS + 0.5) return 0;
        sample_count = (uint32_t)(count + 0.5);
        finite_updates = (uint64_t)sample_count * config->repeat_count;
    } else if (config->kind == NCLP_DAC_PRESET_GAUSSIAN) {
        const double half = 4.0 * config->parameter * update_rate / 1000000.0;
        if (half < 0.5 || half >= 511.5) return 0;
        sample_count = 2U * (uint32_t)(half + 0.5) + 1U;
        finite_updates = (uint64_t)sample_count * config->repeat_count;
    } else {
        if (config->minimum_a_uv != config->maximum_a_uv ||
            config->minimum_b_uv != config->maximum_b_uv ||
            config->repeat_count != 1U) return 0;
        const double count = config->parameter * update_rate / 1000000.0;
        if (count < 0.5 || count >= (double)UINT32_MAX + 0.5) return 0;
        finite_updates = (uint64_t)(count + 0.5);
    }
    if (finite_updates > UINT32_MAX) return 0;
    plan->sample_count = sample_count;
    plan->update_period_clocks = update_period;
    plan->finite_update_count = (uint32_t)finite_updates;
    return 1;
}

static uint32_t voltage_code(double uv)
{
    /* MCP4922 gain=1: Vout = Vref * code / 4096; clamp the endpoint. */
    double code = uv * 4096.0 / NCLP_DAC_REFERENCE_UV;
    if (code >= STIM_DAC_CODE_MASK) return STIM_DAC_CODE_MASK;
    if (code <= 0.0) return 0U;
    return (uint32_t)(code + 0.5);
}

uint32_t nclp_dac_preset_word(const nclp_stim_preset_config_t *config,
                             const nclp_dac_preset_plan_t *plan, uint32_t index)
{
    double fraction = 0.0;
    if (config->kind == NCLP_DAC_PRESET_SINE)
        fraction = 0.5 + 0.5 * sin(TWO_PI * index / plan->sample_count);
    else if (config->kind == NCLP_DAC_PRESET_GAUSSIAN) {
        const double z = -4.0 + 8.0 * index / (plan->sample_count - 1U);
        fraction = exp(-0.5 * z * z);
    }
    const uint32_t a = (config->channel_mask & 1U) ? voltage_code(config->minimum_a_uv +
        (config->maximum_a_uv - config->minimum_a_uv) * fraction) : 0U;
    const uint32_t b = (config->channel_mask & 2U) ? voltage_code(config->minimum_b_uv +
        (config->maximum_b_uv - config->minimum_b_uv) * fraction) : 0U;
    return STIM_DAC_PACK_CODES(a, b);
}
