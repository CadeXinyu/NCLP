#include "ripple_filter_design.h"
#include "../../../common/nclp_wire.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

#define RIPPLE_SAMPLE_RATE_HZ 3000.0
#define RIPPLE_KAISER_BETA 5.0
#define RIPPLE_MINIMUM_PHASE_FFT_SIZE 32768U
#define RIPPLE_LOG_MAGNITUDE_FLOOR 1.0e-12
#define RIPPLE_PI 3.14159265358979323846264338327950288

/* Static scratch keeps the half-megabyte FFT workspace out of the 64 KiB
 * control-core stack. Profile design runs only while the detector is stopped. */
static double fft_real[RIPPLE_MINIMUM_PHASE_FFT_SIZE];
static double fft_imag[RIPPLE_MINIMUM_PHASE_FFT_SIZE];
static double designed_taps[NCLP_RIPPLE_FIR_TAPS_MAX];

static const int32_t default_minimum_q17[NCLP_RIPPLE_FIR_TAPS_MAX] = {
#include "../../../../programmable_logic/hls/ripple_detector/ripple_default_initializer.inc"
};

static const int32_t fixed_iir_q16[10] = {
#include "../../../../programmable_logic/hls/ripple_detector/iir_default_initializer.inc"
};

static double bessel_i0(double value)
{
    double term = 1.0;
    double sum = 1.0;
    const double quarter_square = value * value * 0.25;

    for (uint32_t order = 1U; order <= 32U; ++order) {
        term *= quarter_square / ((double)order * (double)order);
        sum += term;
        if (term <= sum * 1.0e-16) {
            break;
        }
    }
    return sum;
}

static void fft(double *real, double *imaginary, uint32_t count, int inverse)
{
    uint32_t j = 0U;

    for (uint32_t i = 1U; i < count; ++i) {
        uint32_t bit = count >> 1U;
        while ((j & bit) != 0U) {
            j ^= bit;
            bit >>= 1U;
        }
        j ^= bit;
        if (i < j) {
            double swap = real[i];
            real[i] = real[j];
            real[j] = swap;
            swap = imaginary[i];
            imaginary[i] = imaginary[j];
            imaginary[j] = swap;
        }
    }

    for (uint32_t length = 2U; length <= count; length <<= 1U) {
        const double angle = (inverse ? 2.0 : -2.0) * RIPPLE_PI /
                             (double)length;
        const double step_real = cos(angle);
        const double step_imag = sin(angle);
        const uint32_t half = length >> 1U;

        for (uint32_t base = 0U; base < count; base += length) {
            double twiddle_real = 1.0;
            double twiddle_imag = 0.0;
            for (uint32_t offset = 0U; offset < half; ++offset) {
                const uint32_t even = base + offset;
                const uint32_t odd = even + half;
                const double product_real =
                    twiddle_real * real[odd] - twiddle_imag * imaginary[odd];
                const double product_imag =
                    twiddle_real * imaginary[odd] + twiddle_imag * real[odd];
                const double next_twiddle_real =
                    twiddle_real * step_real - twiddle_imag * step_imag;

                real[odd] = real[even] - product_real;
                imaginary[odd] = imaginary[even] - product_imag;
                real[even] += product_real;
                imaginary[even] += product_imag;
                twiddle_imag = twiddle_real * step_imag +
                               twiddle_imag * step_real;
                twiddle_real = next_twiddle_real;
            }
        }
    }

    if (inverse) {
        const double reciprocal = 1.0 / (double)count;
        for (uint32_t i = 0U; i < count; ++i) {
            real[i] *= reciprocal;
            imaginary[i] *= reciprocal;
        }
    }
}

static int normalize_at_band_center(double *coefficients, uint32_t tap_count,
                                    double center_hz)
{
    const double omega = 2.0 * RIPPLE_PI * center_hz /
                         RIPPLE_SAMPLE_RATE_HZ;
    double response_real = 0.0;
    double response_imag = 0.0;

    for (uint32_t tap = 0U; tap < tap_count; ++tap) {
        const double phase = omega * (double)tap;
        response_real += coefficients[tap] * cos(phase);
        response_imag -= coefficients[tap] * sin(phase);
    }
    const double magnitude = hypot(response_real, response_imag);
    if (!isfinite(magnitude) || magnitude < 1.0e-12) {
        return NCLP_RIPPLE_FILTER_DESIGN_ERROR_NUMERIC;
    }
    for (uint32_t tap = 0U; tap < tap_count; ++tap) {
        coefficients[tap] /= magnitude;
    }
    return NCLP_RIPPLE_FILTER_DESIGN_OK;
}

static int design_linear(double low_hz, double high_hz, uint32_t tap_count)
{
    const double center = 0.5 * (double)(tap_count - 1U);
    const double window_scale = bessel_i0(RIPPLE_KAISER_BETA);

    for (uint32_t tap = 0U; tap < tap_count; ++tap) {
        const double offset = (double)tap - center;
        double ideal;
        if (offset == 0.0) {
            ideal = 2.0 * (high_hz - low_hz) / RIPPLE_SAMPLE_RATE_HZ;
        } else {
            ideal = (sin(2.0 * RIPPLE_PI * high_hz * offset /
                         RIPPLE_SAMPLE_RATE_HZ) -
                     sin(2.0 * RIPPLE_PI * low_hz * offset /
                         RIPPLE_SAMPLE_RATE_HZ)) /
                    (RIPPLE_PI * offset);
        }
        const double ratio = tap_count == 1U ? 0.0 :
            (2.0 * (double)tap / (double)(tap_count - 1U)) - 1.0;
        const double window = bessel_i0(RIPPLE_KAISER_BETA *
            sqrt(fmax(0.0, 1.0 - ratio * ratio))) / window_scale;
        designed_taps[tap] = ideal * window;
    }
    return normalize_at_band_center(designed_taps, tap_count,
                                    0.5 * (low_hz + high_hz));
}

static int convert_to_minimum_phase(uint32_t tap_count, double center_hz)
{
    memset(fft_real, 0, sizeof fft_real);
    memset(fft_imag, 0, sizeof fft_imag);
    for (uint32_t tap = 0U; tap < tap_count; ++tap) {
        fft_real[tap] = designed_taps[tap];
    }

    fft(fft_real, fft_imag, RIPPLE_MINIMUM_PHASE_FFT_SIZE, 0);
    for (uint32_t bin = 0U; bin < RIPPLE_MINIMUM_PHASE_FFT_SIZE; ++bin) {
        const double magnitude = fmax(hypot(fft_real[bin], fft_imag[bin]),
                                      RIPPLE_LOG_MAGNITUDE_FLOOR);
        fft_real[bin] = log(magnitude);
        fft_imag[bin] = 0.0;
    }
    fft(fft_real, fft_imag, RIPPLE_MINIMUM_PHASE_FFT_SIZE, 1);

    for (uint32_t index = 1U;
         index < RIPPLE_MINIMUM_PHASE_FFT_SIZE / 2U; ++index) {
        fft_real[index] *= 2.0;
        fft_imag[index] *= 2.0;
    }
    for (uint32_t index = RIPPLE_MINIMUM_PHASE_FFT_SIZE / 2U + 1U;
         index < RIPPLE_MINIMUM_PHASE_FFT_SIZE; ++index) {
        fft_real[index] = 0.0;
        fft_imag[index] = 0.0;
    }

    fft(fft_real, fft_imag, RIPPLE_MINIMUM_PHASE_FFT_SIZE, 0);
    for (uint32_t bin = 0U; bin < RIPPLE_MINIMUM_PHASE_FFT_SIZE; ++bin) {
        const double magnitude = exp(fft_real[bin]);
        const double phase = fft_imag[bin];
        fft_real[bin] = magnitude * cos(phase);
        fft_imag[bin] = magnitude * sin(phase);
    }
    fft(fft_real, fft_imag, RIPPLE_MINIMUM_PHASE_FFT_SIZE, 1);
    for (uint32_t tap = 0U; tap < tap_count; ++tap) {
        designed_taps[tap] = fft_real[tap];
    }
    return normalize_at_band_center(designed_taps, tap_count, center_hz);
}

static int quantize_q17(uint32_t tap_count, int32_t coefficients_q17[256])
{
    for (uint32_t tap = 0U; tap < tap_count; ++tap) {
        const double scaled = designed_taps[tap] * 131072.0;
        long long quantized;
        if (!isfinite(scaled) || scaled < -131072.0 || scaled > 131071.0) {
            return NCLP_RIPPLE_FILTER_DESIGN_ERROR_NUMERIC;
        }
        quantized = llrint(scaled);
        if (quantized < -131072LL || quantized > 131071LL) {
            return NCLP_RIPPLE_FILTER_DESIGN_ERROR_NUMERIC;
        }
        coefficients_q17[tap] = (int32_t)quantized;
    }
    for (uint32_t tap = tap_count; tap < NCLP_RIPPLE_FIR_TAPS_MAX; ++tap) {
        coefficients_q17[tap] = 0;
    }
    return NCLP_RIPPLE_FILTER_DESIGN_OK;
}

int nclp_ripple_filter_design_fir(uint32_t filter_kind,
                                  uint32_t low_cutoff_millihz,
                                  uint32_t high_cutoff_millihz,
                                  uint32_t tap_count,
                                  int32_t coefficients_q17[256])
{
    double low_hz;
    double high_hz;
    int result;

    if (coefficients_q17 == NULL ||
        (filter_kind != NCLP_RIPPLE_FILTER_MINIMUM_FIR &&
         filter_kind != NCLP_RIPPLE_FILTER_LINEAR_FIR) ||
        tap_count < NCLP_RIPPLE_FIR_TAPS_MIN ||
        tap_count > NCLP_RIPPLE_FIR_TAPS_MAX ||
        low_cutoff_millihz == 0U ||
        low_cutoff_millihz >= high_cutoff_millihz ||
        high_cutoff_millihz >= 1500000U) {
        return NCLP_RIPPLE_FILTER_DESIGN_ERROR_ARGUMENT;
    }

    if (filter_kind == NCLP_RIPPLE_FILTER_MINIMUM_FIR && tap_count == 129U &&
        low_cutoff_millihz == NCLP_RIPPLE_FIXED_IIR_LOW_MILLIHZ &&
        high_cutoff_millihz == NCLP_RIPPLE_FIXED_IIR_HIGH_MILLIHZ) {
        memcpy(coefficients_q17, default_minimum_q17,
               sizeof default_minimum_q17);
        return NCLP_RIPPLE_FILTER_DESIGN_OK;
    }

    low_hz = (double)low_cutoff_millihz / 1000.0;
    high_hz = (double)high_cutoff_millihz / 1000.0;
    result = design_linear(low_hz, high_hz, tap_count);
    if (result != NCLP_RIPPLE_FILTER_DESIGN_OK) {
        return result;
    }
    if (filter_kind == NCLP_RIPPLE_FILTER_MINIMUM_FIR) {
        result = convert_to_minimum_phase(tap_count, 0.5 * (low_hz + high_hz));
        if (result != NCLP_RIPPLE_FILTER_DESIGN_OK) {
            return result;
        }
    }
    return quantize_q17(tap_count, coefficients_q17);
}

void nclp_ripple_filter_fixed_iir(int32_t coefficients_q16[10])
{
    if (coefficients_q16 != NULL) {
        memcpy(coefficients_q16, fixed_iir_q16, sizeof fixed_iir_q16);
    }
}
