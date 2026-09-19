#include "headstage_internal.h"

/*
 * Persistent acquisition configuration for the A53-0 control application.
 *
 * Private configuration state is accessed through this module's declared API.
 * Only the A53-0 control core calls these functions.
 */

#define ACQUISITION_DEFAULT_SAMPLE_RATE_HZ          30000U
#define ACQUISITION_DEFAULT_ANALOG_UPPER_HZ          7500U
#define ACQUISITION_DEFAULT_ANALOG_LOWER_MILLIHZ     1000U
#define ACQUISITION_DEFAULT_DSP_REQUEST_MILLIHZ      1000U
#define ACQUISITION_SAMPLE_CLOCK_POLL_LIMIT           5000U
#define ACQUISITION_SAMPLE_CLOCK_POLL_US               100U
#define ACQUISITION_DSP_CODE_MIN                        1U
#define ACQUISITION_DSP_CODE_MAX                       15U
#define ACQUISITION_TTL_CHANNEL_MAX                    15U

typedef struct {
    uint32_t sample_rate_hz;
    uint32_t sample_clock_config;
    uint8_t output_divide;
    uint8_t input_divide;
    uint8_t feedback_multiply;
    uint8_t rhd_reg1;
    uint8_t rhd_reg2;
} acquisition_rate_profile_t;

/* O/D/M implement Fout = 140 MHz * M / (D * O).  The Intan SCLK is Fout/4,
 * which gives exactly the listed per-channel sample rate for 35 SPI words. */
static const acquisition_rate_profile_t g_acquisition_rate_profiles[] = {
    { 5000U, INTAN_SAMPLE_CLOCK_CONFIG_PACK(80U, 4U, 32U), 80U, 4U, 32U, 0x48U, 0x28U},
    {10000U, INTAN_SAMPLE_CLOCK_CONFIG_PACK(40U, 4U, 32U), 40U, 4U, 32U, 0x44U, 0x12U},
    {15000U, INTAN_SAMPLE_CLOCK_CONFIG_PACK(30U, 4U, 36U), 30U, 4U, 36U, 0x43U, 0x07U},
    {20000U, INTAN_SAMPLE_CLOCK_CONFIG_PACK(20U, 4U, 32U), 20U, 4U, 32U, 0x42U, 0x04U},
    {25000U, INTAN_SAMPLE_CLOCK_CONFIG_PACK(16U, 4U, 32U), 16U, 4U, 32U, 0x42U, 0x04U},
    {30000U, INTAN_SAMPLE_CLOCK_CONFIG_PACK(15U, 4U, 36U), 15U, 4U, 36U, 0x42U, 0x04U}
};

static nclp_acquisition_config_t g_acquisition_config = {
    ACQUISITION_DEFAULT_SAMPLE_RATE_HZ,
    ACQUISITION_DEFAULT_ANALOG_UPPER_HZ,
    ACQUISITION_DEFAULT_ANALOG_LOWER_MILLIHZ,
    0U,
    ACQUISITION_DEFAULT_DSP_REQUEST_MILLIHZ,
    1166U,
    0U,
    0U,
    INTAN_SAMPLE_CLOCK_CONFIG_PACK(15U, 4U, 36U),
    12U,
    0x42U,
    0x04U,
    0U
};

/* This tracks the last PLL profile verified by this firmware.  The PL reset
 * value is the same 30 kS/s profile used above. */
static uint32_t g_acquisition_hardware_rate_hz =
    ACQUISITION_DEFAULT_SAMPLE_RATE_HZ;

static const acquisition_rate_profile_t *acquisition_config_find_rate_profile(
    uint32_t sample_rate_hz)
{
    for (uint32_t i = 0U;
         i < (sizeof(g_acquisition_rate_profiles) /
              sizeof(g_acquisition_rate_profiles[0]));
         ++i) {
        if (g_acquisition_rate_profiles[i].sample_rate_hz == sample_rate_hz) {
            return &g_acquisition_rate_profiles[i];
        }
    }

    return NULL;
}

static double acquisition_config_dsp_actual_millihz_exact(
    uint32_t sample_rate_hz,
    uint32_t code)
{
    const double two_pi = 6.28318530717958647692;
    double x = (double)(1UL << code);

    return (double)sample_rate_hz * log(x / (x - 1.0)) * 1000.0 /
           two_pi;
}

/* Match the Intan/Open Ephys implementation: choose N=1..15 using logarithmic
 * distance and return the realizable cutoff for the active sample rate. */
static int acquisition_config_quantize_dsp(
    uint32_t sample_rate_hz,
    uint32_t requested_millihz,
    uint8_t *code_out,
    uint32_t *actual_millihz_out)
{
    uint32_t best_code = 0U;
    uint32_t best_actual = 0U;
    double best_log_error = 0.0;
    double requested_log;

    if (acquisition_config_find_rate_profile(sample_rate_hz) == NULL ||
        requested_millihz == 0U || code_out == NULL ||
        actual_millihz_out == NULL) {
        return -1;
    }

    requested_log = log((double)requested_millihz);
    for (uint32_t code = ACQUISITION_DSP_CODE_MIN;
         code <= ACQUISITION_DSP_CODE_MAX;
         ++code) {
        double actual_exact = acquisition_config_dsp_actual_millihz_exact(
            sample_rate_hz, code);
        uint32_t actual = (uint32_t)(actual_exact + 0.5);
        double log_error = fabs(requested_log - log(actual_exact));

        if (best_code == 0U || log_error < best_log_error) {
            best_code = code;
            best_actual = actual;
            best_log_error = log_error;
        }
    }

    *code_out = (uint8_t)best_code;
    *actual_millihz_out = best_actual;
    return 0;
}

int acquisition_config_refresh_derived(nclp_acquisition_config_t *config)
{
    const acquisition_rate_profile_t *profile;

    if (config == NULL) {
        return -1;
    }
    profile = acquisition_config_find_rate_profile(config->sample_rate_hz);
    if (profile == NULL ||
        acquisition_config_quantize_dsp(
            config->sample_rate_hz,
            config->dsp_requested_cutoff_millihz,
            &config->dsp_cutoff_code,
            &config->dsp_actual_cutoff_millihz) != 0) {
        return -1;
    }

    config->sample_clock_config = profile->sample_clock_config;
    config->rhd_reg1 = profile->rhd_reg1;
    config->rhd_reg2 = profile->rhd_reg2;
    return 0;
}

int acquisition_config_validate(const nclp_acquisition_config_t *config)
{
    nclp_acquisition_config_t expected;

    if (config == NULL ||
        acquisition_config_find_rate_profile(config->sample_rate_hz) == NULL ||
        init_find_analog_upper_cutoff(config->analog_upper_cutoff_hz) == NULL ||
        init_find_analog_lower_cutoff(
            config->analog_lower_cutoff_millihz) == NULL ||
        config->analog_lower_cutoff_millihz >=
            (config->analog_upper_cutoff_hz * 1000U) ||
        config->dsp_enabled > 1U ||
        config->dsp_requested_cutoff_millihz == 0U ||
        config->ttl_fast_settle_enabled > 1U ||
        config->ttl_fast_settle_channel > ACQUISITION_TTL_CHANNEL_MAX) {
        return -1;
    }

    expected = *config;
    if (acquisition_config_refresh_derived(&expected) != 0 ||
        expected.sample_clock_config != config->sample_clock_config ||
        expected.rhd_reg1 != config->rhd_reg1 ||
        expected.rhd_reg2 != config->rhd_reg2 ||
        expected.dsp_cutoff_code != config->dsp_cutoff_code ||
        expected.dsp_actual_cutoff_millihz !=
            config->dsp_actual_cutoff_millihz) {
        return -1;
    }

    return 0;
}

static void acquisition_config_select_init_profile(
    const nclp_acquisition_config_t *config)
{
    nclp_operation.init_rhd_reg1_value = config->rhd_reg1;
    nclp_operation.init_rhd_reg2_value = config->rhd_reg2;
}

static int acquisition_config_sample_clock_readback(
    const acquisition_rate_profile_t *profile)
{
    uint32_t status = 0U;
    uint32_t readback = 0U;

    if (profile == NULL ||
        intan_reg_read(INTAN_REG_SAMPLE_CLOCK_STATUS, &status) != 0 ||
        intan_reg_read(INTAN_REG_SAMPLE_CLOCK_CONFIG, &readback) != 0) {
        return -1;
    }

    if ((status & (INTAN_SAMPLE_CLOCK_STATUS_BUSY |
                   INTAN_SAMPLE_CLOCK_STATUS_ERROR)) != 0U ||
        (status & (INTAN_SAMPLE_CLOCK_STATUS_READY |
                   INTAN_SAMPLE_CLOCK_STATUS_LOCKED)) !=
            (INTAN_SAMPLE_CLOCK_STATUS_READY |
             INTAN_SAMPLE_CLOCK_STATUS_LOCKED) ||
        readback != profile->sample_clock_config) {
        return -1;
    }

    return 0;
}

static int acquisition_config_program_sample_clock_once(
    const acquisition_rate_profile_t *profile)
{
    uint32_t running = 0U;
    uint32_t status = 0U;
    uint32_t readback = 0U;
    uint32_t saw_busy = 0U;

    if (profile == NULL ||
        intan_reg_read(INTAN_REG_ACQUISITION_STATUS, &running) != 0 ||
        (running & INTAN_ACQUISITION_STATUS_RUNNING) != 0U) {
        xil_printf("  FAIL sample rate: SPI must be stopped\r\n");
        return -1;
    }

    intan_register_write(INTAN_REG_SAMPLE_CLOCK_CONFIG,
                         profile->sample_clock_config);
    intan_register_write(INTAN_REG_SAMPLE_CLOCK_COMMAND,
                         INTAN_SAMPLE_CLOCK_COMMAND_APPLY);

    for (uint32_t poll = 0U;
         poll < ACQUISITION_SAMPLE_CLOCK_POLL_LIMIT; ++poll) {
        if (intan_reg_read(INTAN_REG_SAMPLE_CLOCK_STATUS, &status) != 0) {
            return -1;
        }
        if ((status & INTAN_SAMPLE_CLOCK_STATUS_BUSY) != 0U) {
            saw_busy = 1U;
        }
        if ((status & INTAN_SAMPLE_CLOCK_STATUS_ERROR) != 0U) {
            break;
        }
        if (saw_busy != 0U &&
            (status & INTAN_SAMPLE_CLOCK_STATUS_BUSY) == 0U &&
            (status & (INTAN_SAMPLE_CLOCK_STATUS_READY |
                       INTAN_SAMPLE_CLOCK_STATUS_LOCKED)) ==
                (INTAN_SAMPLE_CLOCK_STATUS_READY |
                 INTAN_SAMPLE_CLOCK_STATUS_LOCKED)) {
            break;
        }
        usleep(ACQUISITION_SAMPLE_CLOCK_POLL_US);
    }

    if (saw_busy == 0U ||
        (status & (INTAN_SAMPLE_CLOCK_STATUS_BUSY |
                   INTAN_SAMPLE_CLOCK_STATUS_ERROR)) != 0U ||
        (status & (INTAN_SAMPLE_CLOCK_STATUS_READY |
                   INTAN_SAMPLE_CLOCK_STATUS_LOCKED)) !=
            (INTAN_SAMPLE_CLOCK_STATUS_READY |
             INTAN_SAMPLE_CLOCK_STATUS_LOCKED)) {
        xil_printf("  FAIL sample clock O/D/M=%lu/%lu/%lu status=0x%08lx error=%lu busy_seen=%lu\r\n",
                   (unsigned long)profile->output_divide,
                   (unsigned long)profile->input_divide,
                   (unsigned long)profile->feedback_multiply,
                   (unsigned long)status,
                   (unsigned long)((status & INTAN_SAMPLE_CLOCK_STATUS_ERROR_CODE_MASK) >>
                                   INTAN_SAMPLE_CLOCK_STATUS_ERROR_CODE_SHIFT),
                   (unsigned long)saw_busy);
        nclp_diagnostics.fail_count++;
        return -1;
    }

    if (intan_reg_read(INTAN_REG_SAMPLE_CLOCK_CONFIG, &readback) != 0 ||
        readback != profile->sample_clock_config) {
        xil_printf("  FAIL sample clock readback got=0x%08lx expected=0x%08lx\r\n",
                   (unsigned long)readback,
                   (unsigned long)profile->sample_clock_config);
        nclp_diagnostics.fail_count++;
        return -1;
    }

    return 0;
}

static int acquisition_config_program_sample_clock_with_rollback(
    const acquisition_rate_profile_t *target,
    const acquisition_rate_profile_t *rollback)
{
    if (acquisition_config_program_sample_clock_once(target) == 0) {
        return 0;
    }

    xil_printf("  WARN sample-clock apply failed; restoring previous rate\r\n");
    if (rollback == NULL || rollback == target ||
        acquisition_config_program_sample_clock_once(rollback) != 0) {
        xil_printf("  FAIL sample-clock rollback\r\n");
        return -1;
    }

    return -1;
}

/* Program and verify a rate without changing the persistent user config. */
int acquisition_config_program_rate_hardware(uint32_t sample_rate_hz)
{
    const acquisition_rate_profile_t *target =
        acquisition_config_find_rate_profile(sample_rate_hz);
    const acquisition_rate_profile_t *rollback =
        acquisition_config_find_rate_profile(g_acquisition_hardware_rate_hz);

    if (target == NULL) {
        return -1;
    }
    if (sample_rate_hz == g_acquisition_hardware_rate_hz &&
        acquisition_config_sample_clock_readback(target) == 0) {
        return 0;
    }
    if (acquisition_config_program_sample_clock_with_rollback(target,
                                                               rollback) != 0) {
        return -1;
    }

    g_acquisition_hardware_rate_hz = sample_rate_hz;
    return 0;
}

static uint16_t acquisition_config_ttl_word(uint32_t enabled,
                                            uint32_t channel)
{
    return (uint16_t)(((channel << INTAN_FAST_SETTLE_CHANNEL_SHIFT) &
                       INTAN_FAST_SETTLE_CHANNEL_MASK) |
                      ((enabled != 0U) ? INTAN_FAST_SETTLE_ENABLE : 0U));
}

static int acquisition_config_write_ttl_hardware(uint32_t enabled,
                                                  uint32_t channel)
{
    uint32_t readback = 0U;
    uint16_t expected;

    if (enabled > 1U || channel > ACQUISITION_TTL_CHANNEL_MAX) {
        return -1;
    }
    expected = acquisition_config_ttl_word(enabled, channel);
    intan_register_write(INTAN_REG_FAST_SETTLE_CONFIG, expected);
    if (intan_reg_read(INTAN_REG_FAST_SETTLE_CONFIG, &readback) != 0 ||
        (readback & (INTAN_FAST_SETTLE_ENABLE |
                     INTAN_FAST_SETTLE_CHANNEL_MASK)) != expected) {
        xil_printf("  FAIL TTL fast settle readback got=0x%04lx expected=0x%04x\r\n",
                   (unsigned long)(readback & 0xFFFFU),
                   (unsigned int)expected);
        nclp_diagnostics.fail_count++;
        return -1;
    }

    return 0;
}

/* Enable the persistent TTL route only at the STREAMING boundary. */
int acquisition_config_apply_ttl_hardware(uint32_t enable_desired)
{
    uint32_t enable = (enable_desired != 0U) ?
                      g_acquisition_config.ttl_fast_settle_enabled : 0U;

    return acquisition_config_write_ttl_hardware(
        enable, g_acquisition_config.ttl_fast_settle_channel);
}

int acquisition_config_disable_ttl_hardware(void)
{
    return acquisition_config_write_ttl_hardware(
        0U, g_acquisition_config.ttl_fast_settle_channel);
}

const nclp_acquisition_config_t * acquisition_config_get(void)
{
    return &g_acquisition_config;
}

void acquisition_config_get_copy(nclp_acquisition_config_t *config_out)
{
    if (config_out != NULL) {
        *config_out = g_acquisition_config;
    }
}

void acquisition_config_reset_defaults(void)
{
    nclp_acquisition_config_t defaults = {
        ACQUISITION_DEFAULT_SAMPLE_RATE_HZ,
        ACQUISITION_DEFAULT_ANALOG_UPPER_HZ,
        ACQUISITION_DEFAULT_ANALOG_LOWER_MILLIHZ,
        0U,
        ACQUISITION_DEFAULT_DSP_REQUEST_MILLIHZ,
        0U,
        0U,
        0U,
        0U,
        0U,
        0U,
        0U,
        0U
    };

    (void)acquisition_config_refresh_derived(&defaults);
    g_acquisition_config = defaults;
    g_acquisition_hardware_rate_hz = ACQUISITION_DEFAULT_SAMPLE_RATE_HZ;
    acquisition_config_select_init_profile(&g_acquisition_config);
}

int acquisition_config_set_rate(uint32_t sample_rate_hz)
{
    nclp_acquisition_config_t candidate = g_acquisition_config;

    candidate.sample_rate_hz = sample_rate_hz;
    if (acquisition_config_refresh_derived(&candidate) != 0 ||
        acquisition_config_validate(&candidate) != 0 ||
        acquisition_config_program_rate_hardware(sample_rate_hz) != 0) {
        return -1;
    }

    g_acquisition_config = candidate;
    acquisition_config_select_init_profile(&g_acquisition_config);
    return 0;
}

int acquisition_config_set_bandwidth(uint32_t analog_lower_millihz,
                                            uint32_t analog_upper_hz)
{
    nclp_acquisition_config_t candidate = g_acquisition_config;

    candidate.analog_lower_cutoff_millihz = analog_lower_millihz;
    candidate.analog_upper_cutoff_hz = analog_upper_hz;
    if (acquisition_config_validate(&candidate) != 0) {
        return -1;
    }

    g_acquisition_config = candidate;
    return 0;
}

int acquisition_config_set_dsp(uint32_t enabled,
                                      uint32_t requested_millihz)
{
    nclp_acquisition_config_t candidate = g_acquisition_config;

    candidate.dsp_enabled = enabled;
    candidate.dsp_requested_cutoff_millihz = requested_millihz;
    if (acquisition_config_refresh_derived(&candidate) != 0 ||
        acquisition_config_validate(&candidate) != 0) {
        return -1;
    }

    g_acquisition_config = candidate;
    return 0;
}

int acquisition_config_set_ttl_fast_settle(uint32_t enabled,
                                                   uint32_t channel)
{
    nclp_acquisition_config_t candidate = g_acquisition_config;

    candidate.ttl_fast_settle_enabled = enabled;
    candidate.ttl_fast_settle_channel = channel;
    if (acquisition_config_validate(&candidate) != 0) {
        return -1;
    }

    /* Verify the complete requested value, then leave the route disabled while
     * idle.  Main enables it immediately before streaming, preventing Aux1
     * command injection from corrupting SCAN/INIT/IMPEDANCE finite runs. */
    if (acquisition_config_write_ttl_hardware(enabled, channel) != 0 ||
        acquisition_config_write_ttl_hardware(0U, channel) != 0) {
        return -1;
    }

    g_acquisition_config = candidate;
    return 0;
}

static int acquisition_config_apply_rhd_values_no_results(
    const nclp_acquisition_config_t *config)
{
    if (acquisition_config_validate(config) != 0) {
        return -1;
    }

    acquisition_config_select_init_profile(config);
    return run_post_detection_init_all_no_results(
        INIT_VDD_SENSE_ENABLE,
        config->dsp_enabled,
        config->dsp_cutoff_code,
        config->analog_upper_cutoff_hz,
        config->analog_lower_cutoff_millihz);
}

/* Apply a temporary complete configuration, e.g. the forced 30 kS/s safe
 * impedance profile, without changing the persistent user configuration or
 * publishing a normal INIT result.  TTL remains disabled for the finite run. */
int acquisition_config_apply_values_no_results(
    const nclp_acquisition_config_t *config)
{
    if (acquisition_config_validate(config) != 0 ||
        acquisition_config_disable_ttl_hardware() != 0 ||
        acquisition_config_program_rate_hardware(config->sample_rate_hz) != 0 ||
        acquisition_config_apply_rhd_values_no_results(config) != 0) {
        return -1;
    }

    return 0;
}
