#ifndef NCLP_MAIN_STIM_CONTROL_H
#define NCLP_MAIN_STIM_CONTROL_H

#include "../protocol/nclp_main.h"

#include <stdint.h>
#include "dac_preset.h"

#define NCLP_STIM_OK                 0
#define NCLP_STIM_ERROR_HARDWARE    (-1)
#define NCLP_STIM_ERROR_ARGUMENT    (-2)
#define NCLP_STIM_ERROR_BUSY        (-3)
#define NCLP_STIM_ERROR_NOT_READY   (-4)

typedef struct {
    uint32_t mode;
    uint32_t ttl_pulse_width_axi_cycles;
    /* Internal BD recording route: bit (logical TTL - 2), TTL 2..15.
     * E.g. TTL5 = 1U << 3; zero disables marking. Set before ARM. */
    uint32_t intan_marker_mask;
    uint32_t external_trigger_enable;
} nclp_stim_action_config_t;

/* Select the internal activity marker's recorded Intan TTL: 0=off, 2..15.
 * Set before acquisition and ARM; leaves DAC/action configuration intact. */
int nclp_stim_set_recorded_ttl(uint32_t logical_ttl);

typedef struct {
    uint32_t channel_mask;
    uint32_t update_period_clocks;
    uint32_t start_index;
    uint32_t loop_index;
    uint32_t end_index;
    uint32_t finite_update_count;
} nclp_stim_dac_config_t;

/* Verify the v3 controller and leave stimulation activity disarmed.
 * Preset loading/arming remains an explicit application action. */
int nclp_stim_init(void);

/* Return the enabled sticky error vector.  The driver reports each newly
 * observed vector once and deliberately does not clear a hardware fault. */
uint32_t nclp_stim_service(void);

/* Immediately request fail-low behavior without waiting for DAC serial
 * cleanup.  Global fault paths use this nonblocking command so a running
 * waveform or armed hardware trigger cannot outlive the owning application
 * state.  The request is ignored until the controller identity is verified. */
void nclp_stim_emergency_stop(void);

/* Read one controller view. ACTION word 1 is reserved zero here; the
 * protocol layer inserts the independently owned physical TTL route. */
int nclp_stim_get_section(uint32_t section, uint32_t words[4]);

/* Configuration calls require a disarmed, unlocked STIM controller.  They
 * validate all fields before the first write and verify PS-visible readback. */
int nclp_stim_action_valid(const nclp_stim_action_config_t *config);
int nclp_stim_set_action(const nclp_stim_action_config_t *config);
int nclp_stim_set_dac(const nclp_stim_dac_config_t *config);
int nclp_stim_set_clock(uint32_t output_divide, uint32_t input_divide,
                        uint32_t feedback_multiply,
                        uint32_t *clock_status_out);

/* Raw waveform words are paired 12-bit codes: A in [11:0], B in [27:16]. */
int nclp_stim_write_ram(uint32_t start_index, const uint32_t *words,
                        uint32_t count, uint32_t *crc32_out);
int nclp_stim_read_ram(uint32_t start_index, uint32_t *words,
                       uint32_t count);

/* Generate, configure, and PRIME a waveform while leaving STIM disarmed.
 * repeat_count is the positive number of complete waveform cycles/pulses.
 * Constant requires repeat_count=1 and uses an independent hold time. */
int nclp_stim_load_preset(const nclp_stim_preset_config_t *config,
                          nclp_dac_preset_plan_t *plan_out,
                          uint32_t *crc32_out);

/* Execute exactly one NCLP_STIM_ACTION_* value and return a live snapshot:
 * STATUS, PRIME_STATUS, ACCEPTED_TRIGGER_COUNT, UNSERVED_TRIGGER_COUNT. */
int nclp_stim_control(uint32_t action, uint32_t words[4]);

/* Set the implemented IRQ mask and W1C the requested sticky error bits. */
int nclp_stim_set_diagnostics(uint32_t irq_enable_mask,
                              uint32_t error_clear_mask,
                              uint32_t words[4]);

#endif /* NCLP_MAIN_STIM_CONTROL_H */
