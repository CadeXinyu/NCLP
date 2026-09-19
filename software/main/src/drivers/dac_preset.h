#ifndef NCLP_DAC_PRESET_H
#define NCLP_DAC_PRESET_H
#include <stdint.h>

/* Wire inputs use physical units. Sine parameter: mHz; Gaussian: sigma in us;
 * constant: hold time in us. Repeat count is positive; constant requires 1. */
typedef struct {
    uint32_t kind;
    uint32_t channel_mask;
    uint32_t update_rate_hz;
    uint32_t parameter;
    uint32_t minimum_a_uv;
    uint32_t maximum_a_uv;
    uint32_t minimum_b_uv;
    uint32_t maximum_b_uv;
    uint32_t repeat_count;
    uint32_t external_trigger_enable;
} nclp_stim_preset_config_t;

typedef struct {
    uint32_t sample_count;
    uint32_t update_period_clocks;
    uint32_t finite_update_count;
} nclp_dac_preset_plan_t;

int nclp_dac_plan_preset(const nclp_stim_preset_config_t *config,
                         uint32_t engine_hz, nclp_dac_preset_plan_t *plan);
uint32_t nclp_dac_preset_word(const nclp_stim_preset_config_t *config,
                             const nclp_dac_preset_plan_t *plan, uint32_t index);
#endif
