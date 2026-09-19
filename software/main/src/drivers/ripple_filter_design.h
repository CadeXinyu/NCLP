#ifndef NCLP_MAIN_RIPPLE_FILTER_DESIGN_H
#define NCLP_MAIN_RIPPLE_FILTER_DESIGN_H

#include <stdint.h>

#define NCLP_RIPPLE_FILTER_DESIGN_OK 0
#define NCLP_RIPPLE_FILTER_DESIGN_ERROR_ARGUMENT (-1)
#define NCLP_RIPPLE_FILTER_DESIGN_ERROR_NUMERIC (-2)

/* Design the public FIR profile at the detector's fixed 3 kS/s rate.
 * Returned taps are exact signed Q1.17 integers. */
int nclp_ripple_filter_design_fir(uint32_t filter_kind,
                                  uint32_t low_cutoff_millihz,
                                  uint32_t high_cutoff_millihz,
                                  uint32_t tap_count,
                                  int32_t coefficients_q17[256]);

/* The fixed fourth-order 150-250 Hz Butterworth profile. Coefficients are
 * b0,b1,b2,a1,a2 for each biquad, signed Q2.16. */
void nclp_ripple_filter_fixed_iir(int32_t coefficients_q16[10]);

#endif
