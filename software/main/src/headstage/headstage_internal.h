#ifndef NCLP_HEADSTAGE_INTERNAL_H
#define NCLP_HEADSTAGE_INTERNAL_H
#include <stdint.h>
#include "frame_decoder.h"
typedef struct {
    uint32_t analog_upper_cutoff_hz;
    uint8_t rh1_dac1;
    uint8_t rh1_dac2;
    uint8_t rh2_dac1;
    uint8_t rh2_dac2;
} init_analog_upper_cutoff_t;

typedef struct {
    uint32_t analog_lower_cutoff_millihz;
    uint8_t rl_dac1;
    uint8_t rl_dac2;
    uint8_t rl_dac3;
} init_analog_lower_cutoff_t;

/* Internal A53-0 module contract. Capture and detected-headstage contexts are
 * single-owner storage; the control loop serializes every operation. No other
 * core accesses these contexts. Configuration calculations keep private state.
 * Cross-module calls are declared here; local helpers retain static linkage. */
#include "xparameters.h"
#include "xil_io.h"
#include "xil_printf.h"
#include "xil_cache.h"
#include "sleep.h"
#include "../../../common/board/fan_control.h"
#include "../drivers/led_control.h"
#include "../drivers/stim_control.h"
#include "../drivers/compute_control.h"
#include "../drivers/ripple_detector_control.h"
#include "../headstage/impedance_cancel.h"
#include "../protocol/nclp_main.h"
#include "../protocol/output_commands.h"
#include "../drivers/intan_sync_control.h"
#include "../ipc/nclp_shared.h"
#include "../ipc/result_store_control.h"
#include "../diagnostics/debug_autorun.h"
#include "../../../common/nclp_pl_registers.h"
#include <stdint.h>
#include <stddef.h>
#include <math.h>
#include <string.h>

/* A53-0 production control firmware. It exclusively owns the PL,
 * DDR capture ring, headstage procedures, calculations, and fan service.
 * A53-1 submits commands and consumes produced capture blocks through the
 * non-cacheable shared state; all lwIP work stays on that network core. */

#define INTAN_BASE          ((uintptr_t)XPAR_NCLP_INTAN_SPI_MODULE_BD_0_BASEADDR)
#define SPI_DDR_WRITER_BASE    ((uintptr_t)XPAR_AXIS_DDR_CIRCULAR_WRITER_BD_0_BASEADDR)

#ifndef NCLP_DEBUG_MODE
#define NCLP_DEBUG_MODE 0U
#endif

#ifndef NCLP_LOG_LEVEL
#define NCLP_LOG_LEVEL 0U
#endif

#define NCLP_DEBUG NCLP_LOG_LEVEL
#define DEBUG_PRINT(...) do { if (NCLP_LOG_LEVEL != 0U) xil_printf(__VA_ARGS__); } while (0)

/* Register offsets and bit definitions are shared with sanity_check. */
#define DDRW_IRQ_COMPLETION_INTERVAL_BLOCKS 1U

#define SPI_CAPTURE_BLOCK_BYTES        16384U
#define SPI_DDR_RING_BLOCKS            8U
#define SPI_DDR_RING_BYTES             (SPI_CAPTURE_BLOCK_BYTES * SPI_DDR_RING_BLOCKS)
#define SPI_DDR_RING_WORDS32           (SPI_DDR_RING_BYTES / 4U)
#define SPI_DDR_COLLECT_BYTES          SPI_DDR_RING_BYTES
#define SPI_DDR_COLLECT_WORDS32        (SPI_DDR_COLLECT_BYTES / 4U)
#define SPI_CAPTURE_WAIT_TIMEOUT_MS    2000U
#define NCLP_CAPTURE_CANCELLED         (-2)

#define SPI_CONNECTOR_COUNT            4U
#define SPI_LANES_PER_CONNECTOR        2U
#define SPI_SCAN_STREAMS               8U
#define SPI_SCAN_STREAM_MASK           0xFFFFU
#define SPI_SCAN_CAPTURE_STREAMS       16U
#define SPI_SCAN_TIMESTEPS             9U
#define SPI_SCAN_PHASE_FIRST           0U
#define SPI_SCAN_PHASE_LAST            15U
#define SPI_SCAN_PHASE_COUNT           (SPI_SCAN_PHASE_LAST - SPI_SCAN_PHASE_FIRST + 1U)
#define SPI_SCAN_PHASE_PASSES          3U
#define SPI_SCAN_PHASE_STEPS           (SPI_SCAN_PHASE_PASSES * SPI_SCAN_PHASE_COUNT)
#define SPI_SCAN_PASS_SCORE            48U

#define SPI_FRAME_WORDS                (SPI_FRAME_DATA_BASE + \
                                        (SPI_FRAME_CHANNEL_SLOTS * SPI_SCAN_CAPTURE_STREAMS) + \
                                        SPI_FRAME_TTL_WORDS)
#define SPI_FRAME_BUFFER_WORDS         (SPI_FRAME_WORDS + SPI_FRAME_MAGIC_WORDS)
#define SPI_SCAN_CAPTURE_FRAMES        9U
#define SPI_REG59_REQUIRED_REPLIES     SPI_SCAN_CAPTURE_FRAMES
#ifndef NCLP_SCAN_DEBUG
#define NCLP_SCAN_DEBUG 0U
#endif

#define SPI_SCAN_VERBOSE               0U
#define SPI_SCAN_DEBUG_FRAMES          8U
#define SPI_SCAN_SHOULD_PRINT_FRAME(f) ((NCLP_SCAN_DEBUG != 0U) && ((f) < SPI_SCAN_DEBUG_FRAMES))
#define INTAN_START_TIMEOUT_US          100000U
#define INTAN_START_POLL_INTERVAL_US    10U

#define AUX_VDD_BANK                   3U
#define AUX_SCAN_BANK                  0U
#define AUX_SCAN_END_INDEX             8U
#define AUX_SCAN_LOOP_INDEX            0U
#define AUX_INIT_LOOP_INDEX            0U
#define AUX_INIT_PRECAL_GUARD_FRAMES    3U
/* One mixed-headstage INIT session uses a common 29-frame schedule.  Each
 * connector selects its own Aux3 bank: RHD2164 connectors issue WRITE18..21,
 * while RHD2132/RHD2216-only connectors issue READ63 in those four slots.
 * CALIBRATE and the terminal READ63 therefore occur once, at common indexes. */
#define AUX_INIT_CALIBRATE_INDEX       27U
#define AUX_INIT_LAST_INDEX            28U
#define AUX_INIT_COMMAND_COUNT         29U
#define AUX_INIT_TIMESTEPS             29U
#define AUX_INIT_CAPTURE_FRAMES        AUX_INIT_COMMAND_COUNT
#define AUX_INIT_MAX_FRAME_WORDS       (SPI_FRAME_DATA_BASE + \
                                        (SPI_FRAME_CHANNEL_SLOTS * SPI_SCAN_STREAMS) + \
                                        SPI_FRAME_TTL_WORDS)
#define AUX_INIT_MAX_CAPTURE_BYTES     (2U * AUX_INIT_MAX_FRAME_WORDS * \
                                        AUX_INIT_CAPTURE_FRAMES)
#define AUX_BANK_SELECT_SCAN_VALUE     0x0000U

/* The eight-primary-stream INIT capture is deliberately larger than one DDRW
 * block (16,646 bytes).  DDRW-v2 stores it as one full block plus an exact
 * 262-byte tail; the capacity limit is the eight-block ring/collector. */
_Static_assert(AUX_INIT_MAX_CAPTURE_BYTES <= SPI_DDR_RING_BYTES,
               "mixed INIT capture exceeds the DDR ring");
_Static_assert(AUX_INIT_MAX_CAPTURE_BYTES <= SPI_DDR_COLLECT_BYTES,
               "mixed INIT capture exceeds the software collector");

#define NCLP_STREAM_DELIVERY_LOCAL 0U
#define NCLP_STREAM_DELIVERY_UDP   1U
#define NCLP_STREAM_MODE_CONT  1U
#define NCLP_STREAM_AUX_INPUTS NCLP_SAMPLE_MODE_AUX
#define NCLP_STREAM_AUX_VDD    NCLP_SAMPLE_MODE_VDD
#define STREAM_OUTPUT_START_GRACE_POLLS 4U

typedef struct {
    uint32_t sample_rate_hz;
    uint32_t analog_upper_cutoff_hz;
    uint32_t analog_lower_cutoff_millihz;
    uint32_t dsp_enabled;
    uint32_t dsp_requested_cutoff_millihz;
    uint32_t dsp_actual_cutoff_millihz;
    uint32_t ttl_fast_settle_enabled;
    uint32_t ttl_fast_settle_channel;
    uint32_t sample_clock_config;
    uint8_t dsp_cutoff_code;
    uint8_t rhd_reg1;
    uint8_t rhd_reg2;
    uint8_t reserved;
} nclp_acquisition_config_t;
#define INIT_VDD_SENSE_ENABLE 1U

#define IMP_CAP_RANGES                  3U

#define NCLP_IMPEDANCE_OK                0

#define NCLP_IMPEDANCE_ERROR            (-1)

#define NCLP_IMPEDANCE_CANCELLED         1

typedef struct {
    uint32_t fail_count;
} nclp_diagnostics_context_t;
extern nclp_diagnostics_context_t nclp_diagnostics;

typedef struct {
    uint32_t best_lane_phases_a;
    uint32_t best_lane_phases_b;
    uint32_t scan_capture_debug_printed;
    uint32_t best_stream_score[SPI_SCAN_STREAMS];
    uint32_t best_stream_phase[SPI_SCAN_STREAMS];
    uint8_t best_stream_chip_id[SPI_SCAN_STREAMS];
    uint8_t best_stream_num_amps[SPI_SCAN_STREAMS];
    uint8_t best_stream_reg59[SPI_SCAN_STREAMS];
    uint32_t scan_progress_base;
} nclp_headstage_context_t;
extern nclp_headstage_context_t nclp_headstage;

typedef struct {
    uint32_t ddr_capture_armed;
    uint32_t ddr_capture_active;
    uint32_t capture_valid_bytes;
    uint32_t capture_valid_words32;
    uint32_t capture_block_bytes;
    uint32_t capture_ring_bytes;
    uint32_t capture_low_half_first;
    uint32_t capture_ring_words[SPI_DDR_RING_WORDS32] __attribute__((aligned(64)));
    uint32_t capture_collect_words[SPI_DDR_COLLECT_WORDS32] __attribute__((aligned(64)));
} nclp_capture_context_t;
extern nclp_capture_context_t nclp_capture;

typedef struct {
    uint32_t streaming;
    uint32_t stream_aux_mode;
    uint32_t operation_state;
    uint8_t init_rhd_reg1_value;
    uint8_t init_rhd_reg2_value;
} nclp_operation_context_t;
extern nclp_operation_context_t nclp_operation;

int write_lane_phases_verified(uint32_t packed_lane_phases_a,
                                      uint32_t packed_lane_phases_b);
uint16_t get_capture16(const uint32_t *words32, uint32_t index16, uint32_t low_half_first);
int magic_at_capture(const uint32_t *words32, uint32_t index16, uint32_t low_half_first);
void print_capture_word_order_debug(const char *label, uint32_t max_words32);
uint32_t find_best_capture_order_and_offset(uint32_t stream_count,
                                                   uint32_t *best_order_out,
                                                   uint32_t *best_offset16_out);
uint32_t reg_read(uintptr_t base, uint32_t offset);
void reg_write(uintptr_t base, uint32_t offset, uint32_t value);
uint16_t rhd_convert_cmd(uint32_t channel);
uint16_t rhd_read_reg_cmd(uint32_t reg);
uint16_t rhd_write_reg_cmd(uint32_t reg, uint32_t value);
uint16_t rhd_calibrate_cmd(void);
uint16_t rhd_clear_cal_cmd(void);
void intan_register_write(uint32_t reg_offset, uint32_t value);
int start_intan_capture(void);
int intan_reg_read(uint32_t reg_offset, uint32_t *value);
void intan_aux_command_write(uint32_t window_offset, uint32_t bank,
                                    uint32_t index, uint32_t value);
void stop_spi_safely(void);
int enable_intan_recording_output(void);
int enable_intan_compute_output(void);
int start_intan_sfp_capture(void);
int wait_spi_stopped(uint32_t timeout_loops);
int ddrw_wait_for_state(uint32_t expected_state,
                               uint32_t required_status,
                               uint32_t forbidden_status,
                               uint32_t timeout_us,
                               const char *label);
int ddrw_check_terminal(uint32_t block_bytes,
                               uint64_t *payload_bytes_out,
                               uint32_t *produced_out,
                               uint32_t *final_length_out,
                               uint32_t *transfer_error_out);
int ddrw_abort_active_session(const char *label);
int ddr_read_produced_snapshot(uint32_t *produced_out,
                                      uint32_t *last_len_out);
int ddr_capture_prepare(uint32_t block_bytes);
int ddr_capture_collect(uint32_t block_bytes);
int reset_board_path(void);
int print_board_identity(void);
int program_phase_scan_aux_bank(uint32_t reg59_only);
uint32_t packed_lane_phase_get(uint32_t packed, uint32_t lane);
uint32_t chip_channel_count(uint8_t chip_id);
uint8_t selected_stream_mask(void);
uint32_t first_selected_stream(void);
uint32_t primary_stream_mask(uint32_t stream);
uint32_t selected_logical_stream_mask(void);
int run_phase_scan(void);
int arm_stream_mask_run(uint32_t stream_mask, uint32_t mode,
                               uint32_t frame_count,
                               uint32_t aux_mode);
int start_armed_stream(void);
int stop_command_stream(void);
int arm_sfp_stream_run(uint32_t stream_mask, uint32_t mode,
                             uint32_t frame_count, uint32_t aux_mode);
int start_armed_sfp_stream(void);
int stop_sfp_stream(void);
int abort_intan_compute_stream(void);

int acquisition_config_refresh_derived(nclp_acquisition_config_t *config);
int acquisition_config_validate(const nclp_acquisition_config_t *config);
int acquisition_config_program_rate_hardware(uint32_t sample_rate_hz);
int acquisition_config_apply_ttl_hardware(uint32_t enable_desired);
int acquisition_config_disable_ttl_hardware(void);
const nclp_acquisition_config_t * acquisition_config_get(void);
void acquisition_config_get_copy(nclp_acquisition_config_t *config_out);
void acquisition_config_reset_defaults(void);
int acquisition_config_set_rate(uint32_t sample_rate_hz);
int acquisition_config_set_bandwidth(uint32_t analog_lower_millihz,
                                            uint32_t analog_upper_hz);
int acquisition_config_set_dsp(uint32_t enabled,
                                      uint32_t requested_millihz);
int acquisition_config_set_ttl_fast_settle(uint32_t enabled,
                                                   uint32_t channel);
int acquisition_config_apply_values_no_results(
    const nclp_acquisition_config_t *config);
const init_analog_upper_cutoff_t * init_find_analog_upper_cutoff(uint32_t analog_upper_cutoff_hz);
const init_analog_lower_cutoff_t * init_find_analog_lower_cutoff(uint32_t analog_lower_cutoff_millihz);
uint32_t aux_bank_select_value(uint32_t aux_bank);
int run_post_detection_init_all(uint32_t vdd_sense_enable, uint32_t dsp_enable,
                                       uint32_t dsp_cutoff_freq, uint32_t analog_upper_cutoff_hz,
                                       uint32_t analog_lower_cutoff_millihz);
int run_post_detection_init_all_no_results(
    uint32_t vdd_sense_enable,
    uint32_t dsp_enable,
    uint32_t dsp_cutoff_freq,
    uint32_t analog_upper_cutoff_hz,
    uint32_t analog_lower_cutoff_millihz);
int run_impedance_test_all(void);
extern const char * const nclp_stream_names[SPI_SCAN_STREAMS];
#endif
