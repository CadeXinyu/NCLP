#ifndef NCLP_MAIN_RIPPLE_DETECTOR_CONTROL_H
#define NCLP_MAIN_RIPPLE_DETECTOR_CONTROL_H

#include <stdint.h>

#define NCLP_RIPPLE_DETECTOR_OK 0
#define NCLP_RIPPLE_DETECTOR_ERROR_HARDWARE (-1)
#define NCLP_RIPPLE_DETECTOR_ERROR_ARGUMENT (-2)
#define NCLP_RIPPLE_DETECTOR_ERROR_NOT_READY (-3)
#define NCLP_RIPPLE_DETECTOR_ERROR_BUSY (-4)
#define NCLP_RIPPLE_DETECTOR_ERROR_RATE (-5)
#define NCLP_RIPPLE_DETECTOR_ERROR_NOT_ACTIVE (-6)

typedef enum {
    NCLP_RIPPLE_DETECTOR_STOP = 0,
    NCLP_RIPPLE_DETECTOR_START = 1,
    NCLP_RIPPLE_DETECTOR_VALIDATE = 2
} nclp_ripple_detector_action_t;

typedef struct {
    uint32_t input_channel_id; /* logical stream [8:5], amplifier row [4:0] */
    uint32_t filter_kind;      /* hardware FIR or fourth-order IIR */
    uint32_t fir_tap_count;
    /* Public FP32 words; hardware converts them to signed Q16.16. */
    uint32_t baseline_mean_bits;
    uint32_t baseline_stddev_bits;
    uint32_t threshold_k_bits;
    uint32_t power_window_us;
    uint32_t refractory_period_ms;
} nclp_ripple_detector_config_t;

typedef struct {
    uint32_t status;
    uint32_t trigger_request_count;
    uint32_t power_sample_count;
    uint32_t mean_square; /* uint32 ADC-count-squared units */
    uint32_t sum_square_threshold_bits; /* FP32 N(mu+K sigma)^2 */
    uint32_t last_input_timestamp;
} nclp_ripple_detector_runtime_t;

int nclp_ripple_detector_init(void);
int nclp_ripple_detector_reset_defaults(void);
void nclp_ripple_detector_emergency_stop(void);
int nclp_ripple_detector_get_config(nclp_ripple_detector_config_t *config);
int nclp_ripple_detector_get_runtime(nclp_ripple_detector_runtime_t *runtime);
/* Pure software preflight. It performs the same fixed-point/range checks as
 * set_config(), without reading or writing detector registers. */
int nclp_ripple_detector_check_config(
    const nclp_ripple_detector_config_t *config);
int nclp_ripple_detector_set_config(const nclp_ripple_detector_config_t *config);

/* Private firmware upload port. FIR indices 0..255 use signed Q1.17; IIR
 * indices 256..265 use signed Q2.16. Values are transported as FP32 words. */
int nclp_ripple_detector_set_coefficient(uint32_t index,
                                         uint32_t coefficient_bits);
/* Atomic Q16.16 K update during streaming; preserves signal and refractory
 * history and commits only after hardware accepts it. */
int nclp_ripple_detector_set_k(uint32_t threshold_k_bits);
int nclp_ripple_detector_control(nclp_ripple_detector_action_t action,
                                 nclp_ripple_detector_runtime_t *runtime);

#endif
