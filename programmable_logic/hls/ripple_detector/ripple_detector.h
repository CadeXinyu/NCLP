#ifndef NCLP_RIPPLE_DETECTOR_H
#define NCLP_RIPPLE_DETECTOR_H
#include <stdint.h>

namespace nclp_ripple {
static const uint32_t MAX_FIR_TAP_COUNT = 256;
static const uint32_t DEFAULT_FIR_TAP_COUNT = 129;
static const uint32_t MAX_POWER_WINDOW_SAMPLES = 30;
static const uint32_t IIR_COEFFICIENT_COUNT = 10;
enum FilterKind { FIR = 0, BUTTERWORTH_IIR = 1 };
enum ResultFlags {
    CONFIG_VALID = 1u << 0,
    OUTPUT_VALID = 1u << 1,
    TRIGGER = 1u << 2,
    GAP_RESET = 1u << 3,
    NUMERIC_FAULT = 1u << 4,
    UPDATE_REJECTED = 1u << 5
};
enum Action {
    APPLY_CONFIG_AND_RESET = 0,
    PROCESS_SAMPLE = 1,
    WRITE_FIR_COEFFICIENT = 2,
    VALIDATE_CONFIG = 3,
    RESTORE_DEFAULT_COEFFICIENTS = 4,
    UPDATE_THRESHOLD_K = 5,
    WRITE_IIR_COEFFICIENT = 6
};
}

// One call handles one selected channel's raw Intan offset-binary sample.
// Wrapper must gate enable against exactly 30000 samples/s. Configuration is
// committed by APPLY_CONFIG_AND_RESET or RESTORE_DEFAULT_COEFFICIENTS;
// VALIDATE_CONFIG validates only, and coefficient writes invalidate the
// committed configuration until the next apply action.
// Baseline mean/stddev, threshold K and coefficient inputs carry raw IEEE-754
// binary32 register bits;
// the core decodes them to fixed point only on the corresponding control action.
// mean_square is an unsigned integer in ADC-count-squared units;
// sum_square_threshold_bits remains an IEEE-754 binary32 representation of the
// exact rolling-sum comparison threshold. Output refs are scalar ap_vld, inputs
// ap_none, block control ap_ctrl_hs.
void nclp_ripple_hls(uint32_t action, uint32_t raw_sample,
                     uint32_t input_timestamp, uint32_t fir_tap_count,
                     uint32_t filter_kind,
                     uint32_t power_window_samples,
                     uint32_t refractory_output_samples,
                     uint32_t baseline_mean_bits,
                     uint32_t baseline_stddev_bits,
                     uint32_t threshold_k_bits,
                     uint32_t coefficient_index, uint32_t coefficient_bits,
                     uint32_t &result_flags, uint32_t &mean_square,
                     uint32_t &sum_square_threshold_bits);
#endif
