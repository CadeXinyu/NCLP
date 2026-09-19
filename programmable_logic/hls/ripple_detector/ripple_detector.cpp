#include "ripple_detector.h"
#include "coefficients.h"
#ifdef __SYNTHESIS__
#include "ap_int.h"
typedef ap_int<18> FilterOperand;
typedef ap_int<48> FirAccumulator;
typedef ap_int<40> IirState;
typedef ap_int<60> IirAccumulator;
#else
typedef int64_t FilterOperand;
typedef int64_t FirAccumulator;
typedef int64_t IirState;
typedef int64_t IirAccumulator;
#endif

namespace {
using namespace nclp_ripple;
static const int32_t IIR_SCALE = 1 << 16;
static const uint32_t RIPPLE_FIR_LANES = 32;
static const uint32_t DECIMATE5_LANES = 8;
static const uint32_t HALFBAND_LANES = 11;
static const uint32_t ACCUMULATION_CONTEXTS = 4;
static const int32_t IIR_STATE_FRACTION = 20;

static int32_t hb_history[11] = {};
static int32_t dec5_history[120] = {};
static int32_t ripple_history[MAX_FIR_TAP_COUNT] = {};
static uint32_t square_history[MAX_POWER_WINDOW_SAMPLES] = {};
static IirState iir_x_history[4] = {}, iir_y_history[4] = {};
static int32_t iir_coefficients[IIR_COEFFICIENT_COUNT] = {
#include "iir_default_initializer.inc"
};
static int32_t ripple_coefficients[MAX_FIR_TAP_COUNT] = {
#include "ripple_default_initializer.inc"
};
static uint32_t hb_position = 0, dec5_position = 0, ripple_position = 0;
static uint32_t hb_count = 0, dec5_count = 0, ripple_count = 0;
static uint32_t phase2 = 0, phase5 = 0, square_position = 0, square_count = 0;
static uint32_t active_fir_tap_count = DEFAULT_FIR_TAP_COUNT;
static uint32_t active_power_window_samples = 12;
static uint32_t active_filter_kind = FIR;
static uint32_t active_refractory_output_samples = 3000;
static uint32_t refractory_remaining = 0, last_input_timestamp = 0;
static int32_t active_baseline_mean_q16 = 0;
static int32_t active_baseline_stddev_q16 = 1 << 16;
static uint64_t square_sum = 0, active_sum_square_threshold = 192;
static uint32_t active_mean_square = 0;
static uint32_t active_sum_square_threshold_bits = 0x43400000u;
static bool timestamp_seen = false, above_threshold = false, config_valid = false;

bool ieee_is_finite(uint32_t word) { return (word & 0x7f800000u) != 0x7f800000u; }

bool ieee_to_fixed(uint32_t word, uint32_t fractional_bits,
                   int64_t minimum, int64_t maximum, int32_t &result) {
    if (!ieee_is_finite(word)) return false;
    const bool negative = (word >> 31) != 0;
    const uint32_t exponent = (word >> 23) & 0xffu;
    uint64_t mantissa = word & 0x7fffffu;
    int32_t shift;
    if (exponent == 0) {
        shift = int32_t(fractional_bits) - 149;
    } else {
        mantissa |= 1u << 23;
        shift = int32_t(exponent) + int32_t(fractional_bits) - 150;
    }
    uint64_t magnitude = 0;
    if (mantissa != 0) {
        if (shift >= 0) {
            if (shift >= 63 || mantissa > (uint64_t(-1) >> shift)) return false;
            magnitude = mantissa << shift;
        } else {
            const uint32_t right = uint32_t(-shift);
            if (right < 64) {
                magnitude = mantissa >> right;
                const uint64_t remainder = mantissa & ((uint64_t(1) << right) - 1u);
                const uint64_t halfway = uint64_t(1) << (right - 1u);
                if (remainder > halfway || (remainder == halfway && (magnitude & 1u)))
                    ++magnitude;
            }
        }
    }
    const uint64_t positive_limit = uint64_t(maximum);
    const uint64_t negative_limit = uint64_t(-(minimum + 1)) + 1u;
    if ((!negative && magnitude > positive_limit) ||
        (negative && magnitude > negative_limit)) return false;
    const int64_t signed_value = negative ? -int64_t(magnitude) : int64_t(magnitude);
    result = int32_t(signed_value);
    return true;
}

template <typename Signed>
int64_t round_shift_even(Signed value, uint32_t shift) {
    const bool negative = value < 0;
    const uint64_t magnitude = negative ? uint64_t(-(value + 1)) + 1u : uint64_t(value);
    uint64_t rounded = magnitude >> shift;
    const uint64_t remainder = magnitude & ((uint64_t(1) << shift) - 1u);
    const uint64_t halfway = uint64_t(1) << (shift - 1u);
    if (remainder > halfway || (remainder == halfway && (rounded & 1u))) ++rounded;
    return negative ? -int64_t(rounded) : int64_t(rounded);
}

int32_t saturate_signed(int64_t value, int64_t minimum, int64_t maximum) {
    if (value < minimum) return int32_t(minimum);
    if (value > maximum) return int32_t(maximum);
    return int32_t(value);
}

uint32_t integer_to_ieee(uint64_t value) {
    if (value == 0) return 0;
    uint32_t top = 0;
find_top_bit:
    for (uint32_t i = 0; i < 64; ++i)
        if ((value >> i) & 1u) top = i;
    uint64_t mantissa;
    if (top <= 23) {
        mantissa = value << (23 - top);
    } else {
        const uint32_t shift = top - 23;
        mantissa = value >> shift;
        const uint64_t remainder = value & ((uint64_t(1) << shift) - 1u);
        const uint64_t halfway = uint64_t(1) << (shift - 1u);
        if (remainder > halfway || (remainder == halfway && (mantissa & 1u))) {
            ++mantissa;
            if (mantissa == (uint64_t(1) << 24)) {
                mantissa >>= 1;
                ++top;
            }
        }
    }
    return ((top + 127u) << 23) | uint32_t(mantissa & 0x7fffffu);
}

uint32_t rounded_mean_square(uint64_t sum, uint32_t count) {
    if (count == 0) return 0;

    // The largest valid sum is 30 * 32768^2, which fits in 35 bits. Use a
    // small restoring divider so HLS can share one comparator/subtractor
    // across the 35 iterations instead of building a wide combinational
    // divider. The quotient is at most 32768^2 and therefore fits uint32_t.
    uint64_t quotient = 0;
    uint64_t remainder = 0;
    uint64_t mask = uint64_t(1) << 34;
mean_square_divide:
    for (uint32_t i = 0; i < 35; ++i) {
        remainder = (remainder << 1) | ((sum & mask) != 0 ? 1u : 0u);
        if (remainder >= count) {
            remainder -= count;
            quotient |= mask;
        }
        mask >>= 1;
    }

    const uint64_t twice_remainder = remainder << 1;
    if (twice_remainder > count ||
        (twice_remainder == count && (quotient & 1u) != 0)) ++quotient;
    return uint32_t(quotient);
}

bool sum_square_threshold_from_quantized(
        uint32_t power_window_samples, int32_t baseline_mean_q16,
        int32_t baseline_stddev_q16, int32_t threshold_k_q16,
        uint64_t &sum_square_threshold,
        uint32_t &sum_square_threshold_bits) {
    if (power_window_samples < 1 || power_window_samples > MAX_POWER_WINDOW_SAMPLES ||
        baseline_stddev_q16 < 0 || threshold_k_q16 < 0) {
        sum_square_threshold = 0; sum_square_threshold_bits = 0; return false;
    }
    const int64_t product = int64_t(baseline_stddev_q16) * int64_t(threshold_k_q16);
    const int64_t scaled_product = round_shift_even(product, 16);
    const int64_t theta_q16 = int64_t(baseline_mean_q16) + scaled_product;
    if (theta_q16 < 0 || theta_q16 > (int64_t(32768) << 16)) {
        sum_square_threshold = 0; sum_square_threshold_bits = 0; return false;
    }
    const uint64_t square_q32 = uint64_t(theta_q16) * uint64_t(theta_q16);
    sum_square_threshold = (square_q32 >> 32) * power_window_samples +
        ((square_q32 & 0xffffffffu) * power_window_samples >> 32);
    sum_square_threshold_bits = integer_to_ieee(sum_square_threshold);
    return true;
}

bool make_sum_square_threshold(
        uint32_t power_window_samples, uint32_t baseline_mean_bits,
        uint32_t baseline_stddev_bits, uint32_t threshold_k_bits,
        int32_t &baseline_mean_q16, int32_t &baseline_stddev_q16,
        uint64_t &sum_square_threshold,
        uint32_t &sum_square_threshold_bits) {
    int32_t threshold_k_q16 = 0;
    const bool decoded =
        ieee_to_fixed(baseline_mean_bits, 16, INT32_MIN, INT32_MAX,
                      baseline_mean_q16) &&
        ieee_to_fixed(baseline_stddev_bits, 16, 0, INT32_MAX,
                      baseline_stddev_q16) &&
        ieee_to_fixed(threshold_k_bits, 16, 0, INT32_MAX, threshold_k_q16);
    if (!decoded) {
        sum_square_threshold = 0; sum_square_threshold_bits = 0; return false;
    }
    return sum_square_threshold_from_quantized(
        power_window_samples, baseline_mean_q16, baseline_stddev_q16,
        threshold_k_q16, sum_square_threshold, sum_square_threshold_bits);
}

bool iir_stable() {
validate_iir_stability:
    for (uint32_t section = 0; section < 2; ++section) {
        const int64_t a1 = iir_coefficients[section * 5 + 3];
        const int64_t a2 = iir_coefficients[section * 5 + 4];
        if (!(a2 > -IIR_SCALE && a2 < IIR_SCALE &&
              IIR_SCALE + a1 + a2 > 0 && IIR_SCALE - a1 + a2 > 0)) return false;
    }
    return true;
}

bool validate(uint32_t fir_tap_count, uint32_t filter_kind,
              uint32_t power_window_samples, uint32_t baseline_mean_bits,
              uint32_t baseline_stddev_bits, uint32_t threshold_k_bits,
              int32_t &baseline_mean_q16, int32_t &baseline_stddev_q16,
              uint64_t &sum_square_threshold,
              uint32_t &sum_square_threshold_bits) {
    bool valid = fir_tap_count >= 1 && fir_tap_count <= MAX_FIR_TAP_COUNT &&
        filter_kind <= BUTTERWORTH_IIR &&
        make_sum_square_threshold(
            power_window_samples, baseline_mean_bits, baseline_stddev_bits,
            threshold_k_bits, baseline_mean_q16, baseline_stddev_q16,
            sum_square_threshold, sum_square_threshold_bits);
    if (filter_kind == BUTTERWORTH_IIR) valid = valid && iir_stable();
    if (!valid) { sum_square_threshold = 0; sum_square_threshold_bits = 0; }
    return valid;
}

void clear_history() {
clear_hb:
    for (uint32_t i = 0; i < 11; ++i) hb_history[i] = 0;
clear_dec5:
    for (uint32_t i = 0; i < 120; ++i) dec5_history[i] = 0;
clear_ripple:
    for (uint32_t i = 0; i < MAX_FIR_TAP_COUNT; ++i) ripple_history[i] = 0;
clear_square:
    for (uint32_t i = 0; i < MAX_POWER_WINDOW_SAMPLES; ++i) square_history[i] = 0;
clear_iir:
    for (uint32_t i = 0; i < 4; ++i) { iir_x_history[i] = 0; iir_y_history[i] = 0; }
    hb_position = dec5_position = ripple_position = 0;
    hb_count = dec5_count = ripple_count = 0;
    phase2 = phase5 = square_position = square_count = 0;
    refractory_remaining = last_input_timestamp = 0;
    square_sum = 0;
    active_mean_square = 0;
    timestamp_seen = above_threshold = false;
}

template <uint32_t COUNT>
struct IntegerTreeSum {
    static FirAccumulator run(const FirAccumulator *values) {
#pragma HLS INLINE
        return IntegerTreeSum<COUNT / 2>::run(values) +
               IntegerTreeSum<COUNT - COUNT / 2>::run(values + COUNT / 2);
    }
};
template <>
struct IntegerTreeSum<1> {
    static FirAccumulator run(const FirAccumulator *values) {
#pragma HLS INLINE
        return values[0];
    }
};

template <uint32_t CAPACITY, uint32_t LANES>
FirAccumulator convolve(const int32_t history[CAPACITY],
                        const int32_t coefficients[CAPACITY],
                        uint32_t newest, uint32_t taps) {
#pragma HLS ARRAY_PARTITION variable=history cyclic factor=LANES dim=1
#pragma HLS ARRAY_PARTITION variable=coefficients cyclic factor=LANES dim=1
    static_assert(CAPACITY % LANES == 0, "ring capacity must match bank count");
    const uint32_t ROWS = CAPACITY / LANES;
    const uint32_t CONTEXTS = ROWS < ACCUMULATION_CONTEXTS ? ROWS : ACCUMULATION_CONTEXTS;
    const uint32_t newest_bank = newest % LANES;
    const uint32_t newest_row = newest / LANES;
    FirAccumulator partial[LANES][CONTEXTS];
#pragma HLS ARRAY_PARTITION variable=partial complete dim=0
init_partial:
    for (uint32_t lane = 0; lane < LANES; ++lane) {
#pragma HLS UNROLL
        for (uint32_t context = 0; context < CONTEXTS; ++context) {
#pragma HLS UNROLL
            partial[lane][context] = 0;
        }
    }
fir_blocks:
    for (uint32_t block = 0; block < ROWS; ++block) {
#pragma HLS PIPELINE II=1
#pragma HLS DEPENDENCE variable=partial inter true distance=4
        const uint32_t context = block % CONTEXTS;
        if (block * LANES < taps) {
            int32_t bank_values[LANES];
#pragma HLS ARRAY_PARTITION variable=bank_values complete dim=1
            for (uint32_t bank = 0; bank < LANES; ++bank) {
#pragma HLS UNROLL
                const uint32_t offset = block + (bank > newest_bank ? 1u : 0u);
                const uint32_t row = newest_row >= offset ? newest_row - offset :
                                     newest_row + ROWS - offset;
                bank_values[bank] = history[row * LANES + bank];
            }
            for (uint32_t lane = 0; lane < LANES; ++lane) {
#pragma HLS UNROLL
                const uint32_t lag = block * LANES + lane;
                if (lag < taps) {
                    const uint32_t bank = (newest_bank + LANES - lane) % LANES;
                    const FilterOperand sample = FilterOperand(bank_values[bank]);
                    const FilterOperand coefficient = FilterOperand(coefficients[lag]);
                    partial[lane][context] += FirAccumulator(sample * coefficient);
                }
            }
        }
    }
    FirAccumulator lanes[LANES];
#pragma HLS ARRAY_PARTITION variable=lanes complete dim=1
    for (uint32_t lane = 0; lane < LANES; ++lane) {
#pragma HLS UNROLL
        lanes[lane] = IntegerTreeSum<CONTEXTS>::run(partial[lane]);
    }
    return IntegerTreeSum<LANES>::run(lanes);
}

bool filter_iir(int32_t input, int32_t &output) {
    IirState value;
#ifdef __SYNTHESIS__
    value = IirState(input);
    value <<= IIR_STATE_FRACTION;
#else
    value = int64_t(input) * (int64_t(1) << IIR_STATE_FRACTION);
#endif
iir_sections:
    for (uint32_t section = 0; section < 2; ++section) {
        const uint32_t c = section * 5, h = section * 2;
        const FilterOperand b0 = FilterOperand(iir_coefficients[c]);
        const FilterOperand b1 = FilterOperand(iir_coefficients[c + 1]);
        const FilterOperand b2 = FilterOperand(iir_coefficients[c + 2]);
        const FilterOperand a1 = FilterOperand(iir_coefficients[c + 3]);
        const FilterOperand a2 = FilterOperand(iir_coefficients[c + 4]);
        IirAccumulator accumulator = IirAccumulator(b0 * value);
        accumulator += IirAccumulator(b1 * iir_x_history[h]);
        accumulator += IirAccumulator(b2 * iir_x_history[h + 1]);
        accumulator -= IirAccumulator(a1 * iir_y_history[h]);
        accumulator -= IirAccumulator(a2 * iir_y_history[h + 1]);
        const int64_t next = round_shift_even(accumulator, 16);
        if (next < -(int64_t(1) << 39) || next > (int64_t(1) << 39) - 1) return false;
        iir_x_history[h + 1] = iir_x_history[h];
        iir_x_history[h] = value;
        iir_y_history[h + 1] = iir_y_history[h];
        iir_y_history[h] = IirState(next);
        value = IirState(next);
    }
    const int64_t integer = round_shift_even(value, IIR_STATE_FRACTION);
    output = saturate_signed(integer, INT16_MIN, INT16_MAX);
    return true;
}
}

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
                     uint32_t &sum_square_threshold_bits) {
    using namespace nclp_ripple;
#pragma HLS ARRAY_PARTITION variable=hb_history complete dim=1
#pragma HLS ARRAY_PARTITION variable=HALFBAND_COEFFICIENTS complete dim=1
#pragma HLS ARRAY_PARTITION variable=dec5_history cyclic factor=8 dim=1
#pragma HLS ARRAY_PARTITION variable=DECIMATE5_COEFFICIENTS cyclic factor=8 dim=1
#pragma HLS ARRAY_PARTITION variable=ripple_history cyclic factor=32 dim=1
#pragma HLS ARRAY_PARTITION variable=ripple_coefficients cyclic factor=32 dim=1
#pragma HLS ARRAY_PARTITION variable=iir_x_history complete dim=1
#pragma HLS ARRAY_PARTITION variable=iir_y_history complete dim=1
#pragma HLS INTERFACE ap_ctrl_hs port=return
#pragma HLS INTERFACE ap_none port=action
#pragma HLS INTERFACE ap_none port=raw_sample
#pragma HLS INTERFACE ap_none port=input_timestamp
#pragma HLS INTERFACE ap_none port=fir_tap_count
#pragma HLS INTERFACE ap_none port=filter_kind
#pragma HLS INTERFACE ap_none port=power_window_samples
#pragma HLS INTERFACE ap_none port=refractory_output_samples
#pragma HLS INTERFACE ap_none port=baseline_mean_bits
#pragma HLS INTERFACE ap_none port=baseline_stddev_bits
#pragma HLS INTERFACE ap_none port=threshold_k_bits
#pragma HLS INTERFACE ap_none port=coefficient_index
#pragma HLS INTERFACE ap_none port=coefficient_bits
#pragma HLS INTERFACE ap_vld port=result_flags
#pragma HLS INTERFACE ap_vld port=mean_square
#pragma HLS INTERFACE ap_vld port=sum_square_threshold_bits
    result_flags = config_valid ? uint32_t(CONFIG_VALID) : 0u;
    mean_square = active_mean_square;
    sum_square_threshold_bits = active_sum_square_threshold_bits;

    if (action == RESTORE_DEFAULT_COEFFICIENTS) {
restore_fir:
        for (uint32_t i = 0; i < MAX_FIR_TAP_COUNT; ++i)
            ripple_coefficients[i] = RIPPLE_MINIMUM_COEFFICIENTS[i];
restore_iir:
        for (uint32_t i = 0; i < IIR_COEFFICIENT_COUNT; ++i)
            iir_coefficients[i] = RIPPLE_IIR_COEFFICIENTS[i];
    }
    if (action == UPDATE_THRESHOLD_K) {
        int32_t staged_threshold_k_q16 = 0;
        uint64_t staged_sum_square_threshold = 0;
        uint32_t staged_sum_square_threshold_bits = 0;
        if (config_valid &&
                ieee_to_fixed(threshold_k_bits, 16, 0, INT32_MAX,
                              staged_threshold_k_q16) &&
                sum_square_threshold_from_quantized(
                    active_power_window_samples, active_baseline_mean_q16,
                    active_baseline_stddev_q16, staged_threshold_k_q16,
                    staged_sum_square_threshold,
                    staged_sum_square_threshold_bits)) {
            active_sum_square_threshold = staged_sum_square_threshold;
            active_sum_square_threshold_bits = staged_sum_square_threshold_bits;
            sum_square_threshold_bits = staged_sum_square_threshold_bits;
        } else result_flags |= UPDATE_REJECTED;
        return;
    }
    if (action == WRITE_FIR_COEFFICIENT || action == WRITE_IIR_COEFFICIENT) {
        config_valid = false;
        result_flags = 0;
        int32_t quantized = 0;
        const bool fir = action == WRITE_FIR_COEFFICIENT;
        const uint32_t limit = fir ? MAX_FIR_TAP_COUNT : IIR_COEFFICIENT_COUNT;
        const uint32_t fraction = fir ? 17u : 16u;
        if (coefficient_index < limit && ieee_to_fixed(coefficient_bits, fraction,
                -(int64_t(1) << 17), (int64_t(1) << 17) - 1, quantized)) {
            if (fir) ripple_coefficients[coefficient_index] = quantized;
            else iir_coefficients[coefficient_index] = quantized;
        } else result_flags = NUMERIC_FAULT;
        return;
    }
    if (action == APPLY_CONFIG_AND_RESET || action == VALIDATE_CONFIG ||
        action == RESTORE_DEFAULT_COEFFICIENTS) {
        int32_t staged_baseline_mean_q16 = 0, staged_baseline_stddev_q16 = 0;
        uint64_t staged_sum_square_threshold = 0;
        uint32_t staged_sum_square_threshold_bits = 0;
        const bool staged_valid = validate(
            fir_tap_count, filter_kind, power_window_samples,
            baseline_mean_bits, baseline_stddev_bits, threshold_k_bits,
            staged_baseline_mean_q16, staged_baseline_stddev_q16,
            staged_sum_square_threshold, staged_sum_square_threshold_bits);
        if (action != VALIDATE_CONFIG) {
            clear_history();
            config_valid = staged_valid;
            active_fir_tap_count = staged_valid ? fir_tap_count : DEFAULT_FIR_TAP_COUNT;
            active_filter_kind = staged_valid ? filter_kind : uint32_t(FIR);
            active_power_window_samples = staged_valid ? power_window_samples : 12u;
            active_refractory_output_samples = refractory_output_samples;
            active_baseline_mean_q16 = staged_baseline_mean_q16;
            active_baseline_stddev_q16 = staged_baseline_stddev_q16;
            active_sum_square_threshold = staged_sum_square_threshold;
            active_sum_square_threshold_bits = staged_sum_square_threshold_bits;
        }
        result_flags = staged_valid ? uint32_t(CONFIG_VALID) : 0u;
        mean_square = 0;
        sum_square_threshold_bits = staged_sum_square_threshold_bits;
        return;
    }
    if (action != PROCESS_SAMPLE || !config_valid) return;

    if (timestamp_seen && uint32_t(input_timestamp - last_input_timestamp) != 1u) {
        clear_history();
        result_flags |= GAP_RESET;
        mean_square = 0;
    }
    timestamp_seen = true;
    last_input_timestamp = input_timestamp;
    const int32_t sample = int32_t(raw_sample & 0xffffu) - 32768;
    hb_history[hb_position] = sample;
    const uint32_t newest_hb = hb_position;
    hb_position = hb_position == 10 ? 0 : hb_position + 1;
    if (hb_count < 11) ++hb_count;
    phase2 ^= 1u;
    if (phase2 != 0) return;
    const FirAccumulator hb_acc = convolve<11, HALFBAND_LANES>(
        hb_history, HALFBAND_COEFFICIENTS, newest_hb, 11);
    const int32_t mid_sample = saturate_signed(round_shift_even(hb_acc, 15),
                                               -(int64_t(1) << 17), (int64_t(1) << 17) - 1);
    if (hb_count < 11) return;
    dec5_history[dec5_position] = mid_sample;
    const uint32_t newest_dec5 = dec5_position;
    dec5_position = dec5_position == 119 ? 0 : dec5_position + 1;
    if (dec5_count < 120) ++dec5_count;
    phase5 = phase5 == 4 ? 0 : phase5 + 1;
    if (phase5 != 0) return;
    const FirAccumulator dec_acc = convolve<120, DECIMATE5_LANES>(
        dec5_history, DECIMATE5_COEFFICIENTS, newest_dec5, 120);
    const int32_t lfp_sample = saturate_signed(round_shift_even(dec_acc, 19),
                                               INT16_MIN, INT16_MAX);
    if (dec5_count < 120) return;

    int32_t ripple = 0;
    if (active_filter_kind == FIR) {
        ripple_history[ripple_position] = lfp_sample;
        const uint32_t newest_ripple = ripple_position;
        ripple_position = ripple_position == MAX_FIR_TAP_COUNT - 1 ? 0 : ripple_position + 1;
        if (ripple_count < active_fir_tap_count) ++ripple_count;
        const FirAccumulator ripple_acc = convolve<MAX_FIR_TAP_COUNT, RIPPLE_FIR_LANES>(
            ripple_history, ripple_coefficients, newest_ripple, active_fir_tap_count);
        ripple = saturate_signed(round_shift_even(ripple_acc, 17), INT16_MIN, INT16_MAX);
        if (ripple_count < active_fir_tap_count) return;
    } else if (!filter_iir(lfp_sample, ripple)) {
        clear_history();
        config_valid = false;
        result_flags = NUMERIC_FAULT;
        mean_square = 0;
        return;
    }

    const uint32_t new_square = uint32_t(int64_t(ripple) * int64_t(ripple));
    const uint32_t old_square = square_history[square_position];
    square_history[square_position] = new_square;
    square_sum = square_sum - old_square + new_square;
    square_position = square_position + 1 == active_power_window_samples ? 0 : square_position + 1;
    if (square_count < active_power_window_samples) ++square_count;
    active_mean_square = rounded_mean_square(square_sum, square_count);
    mean_square = active_mean_square;
    result_flags |= OUTPUT_VALID;
    if (refractory_remaining != 0) --refractory_remaining;
    if (square_count < active_power_window_samples) return;
    const bool above = square_sum > active_sum_square_threshold;
    const bool candidate = above && !above_threshold;
    above_threshold = above;
    if (candidate && refractory_remaining == 0) {
        result_flags |= TRIGGER;
        refractory_remaining = active_refractory_output_samples;
    }
}
