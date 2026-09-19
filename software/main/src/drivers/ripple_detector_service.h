#ifndef NCLP_MAIN_RIPPLE_DETECTOR_SERVICE_H
#define NCLP_MAIN_RIPPLE_DETECTOR_SERVICE_H

#include "ripple_detector_control.h"
#include <stdint.h>

typedef struct {
    uint32_t detected_channel_index;
    uint32_t input_channel_id;
    uint32_t filter_kind;
    uint32_t low_cutoff_millihz;
    uint32_t high_cutoff_millihz;
    uint32_t fir_tap_count;
    uint32_t power_window_us;
    uint32_t refractory_period_ms;
    uint32_t baseline_mean_bits;
    uint32_t baseline_stddev_bits;
    uint32_t threshold_k_bits;
    uint32_t baseline_duration_ms;
} nclp_ripple_profile_t;

typedef struct {
    uint32_t state;
    uint32_t collected_samples;
    uint32_t target_samples;
    uint32_t generation;
    uint32_t error;
    uint32_t missed_samples;
} nclp_ripple_baseline_status_t;

int nclp_ripple_service_init(void);
void nclp_ripple_service_poll(void);
void nclp_ripple_service_emergency_stop(void);

int nclp_ripple_service_map_channel(uint32_t detected_channel_index,
                                    uint32_t physical_chip_mask,
                                    uint32_t packed_chip_ids,
                                    uint32_t *input_channel_id);
/* Stop the detector and resolve the saved dense detected-channel index against
 * the latest scan topology. Passing zero for both topology words invalidates
 * channel resolution until a later successful topology update. */
int nclp_ripple_service_update_topology(uint32_t physical_chip_mask,
                                        uint32_t packed_chip_ids);
int nclp_ripple_service_get_profile(nclp_ripple_profile_t *profile);
int nclp_ripple_service_apply_profile(const nclp_ripple_profile_t *profile,
                                      uint32_t physical_chip_mask,
                                      uint32_t packed_chip_ids);
int nclp_ripple_service_get_runtime(nclp_ripple_detector_runtime_t *runtime);
int nclp_ripple_service_get_baseline_status(
    nclp_ripple_baseline_status_t *status);
int nclp_ripple_service_set_k(uint32_t threshold_k_bits);
int nclp_ripple_service_control(nclp_ripple_detector_action_t action,
                                nclp_ripple_detector_runtime_t *runtime);
int nclp_ripple_service_start_baseline(uint32_t duration_ms,
                                       uint32_t local_detector_stream_active);
int nclp_ripple_service_cancel_baseline(void);

#endif
