#ifndef NCLP_PHASE_SELECTION_H
#define NCLP_PHASE_SELECTION_H

#include <stdint.h>

typedef struct {
    uint8_t first;
    uint8_t last;
    uint8_t middle;
    uint8_t length;
} nclp_phase_window_t;

/* Phase indices are linear sample taps, not a circular phase wheel.  Select
 * the longest contiguous accepted run and prefer the earlier run on a tie.
 * A two-tap run selects its later tap so a narrow window ending at phase 15
 * does not bias away from that boundary.  Longer even runs retain the lower
 * middle tap.
 */
static inline int nclp_phase_longest_window(uint16_t accepted_mask,
                                            nclp_phase_window_t *window)
{
    uint32_t best_first = 0U;
    uint32_t best_length = 0U;
    uint32_t run_first = 0U;
    uint32_t run_length = 0U;

    if (window == (nclp_phase_window_t *)0) {
        return -1;
    }

    for (uint32_t phase = 0U; phase <= 16U; ++phase) {
        uint32_t accepted = phase < 16U &&
                            (accepted_mask & (uint16_t)(1U << phase)) != 0U;

        if (accepted != 0U) {
            if (run_length == 0U) {
                run_first = phase;
            }
            run_length++;
        } else {
            if (run_length > best_length) {
                best_first = run_first;
                best_length = run_length;
            }
            run_length = 0U;
        }
    }

    if (best_length == 0U) {
        window->first = 0U;
        window->last = 0U;
        window->middle = 0U;
        window->length = 0U;
        return -1;
    }

    window->first = (uint8_t)best_first;
    window->last = (uint8_t)(best_first + best_length - 1U);
    window->middle = (uint8_t)(
        best_first +
        (best_length == 2U ? 1U : (best_length - 1U) / 2U));
    window->length = (uint8_t)best_length;
    return 0;
}

static inline int nclp_reg59_marker_matches(uint16_t reply,
                                             uint8_t expected_marker)
{
    return (uint8_t)(reply & 0x00FFU) == expected_marker;
}

/* The two RHD2164 views are calibrated independently, but MISO B must still
 * follow MISO A in the shared linear 4x-sample timeline.  The separation is
 * intentionally not fixed: independently selected window centers may differ
 * by one, two, or more taps depending on the measured timing margins.
 */
static inline int nclp_rhd2164_phase_order_valid(uint32_t phase_a,
                                                  uint32_t phase_b)
{
    return phase_b > phase_a;
}

#endif
