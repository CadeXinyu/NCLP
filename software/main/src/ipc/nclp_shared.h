#ifndef NCLP_SHARED_H
#define NCLP_SHARED_H

#include "../protocol/nclp_main.h"
#include "../protocol/nclp_results.h"
#include "xil_mmu.h"

#include <stdint.h>

#define NCLP_SHARED_BASE_ADDRESS 0x70000000U
#define NCLP_SHARED_MAGIC        0x4E434C32U
#define NCLP_SHARED_VERSION      1U
#define NCLP_SHARED_CONTROL_BYTES 0x1000U

/* Shared recording/transport error codes reported in stream_error. */
#define NCLP_STREAM_ERROR_NONE             0U
/* Value 1 is reserved by the original shared-memory contract. */
#define NCLP_STREAM_ERROR_UDP_CONFIG       2U
#define NCLP_STREAM_ERROR_UDP_SESSION      3U
#define NCLP_STREAM_ERROR_DRAIN_TIMEOUT    4U
#define NCLP_STREAM_ERROR_FORCED_ABORT     5U
#define NCLP_STREAM_ERROR_UNEXPECTED_STOP  6U
#define NCLP_STREAM_ERROR_STIM_FAULT       7U
#define NCLP_STREAM_ERROR_LINK_FAULT       8U
#define NCLP_STREAM_ERROR_COMPUTE_FAULT    9U

#define NCLP_SHARED_CMD_IDLE     0U
#define NCLP_SHARED_CMD_REQUEST  1U
#define NCLP_SHARED_CMD_RUNNING  2U
#define NCLP_SHARED_CMD_REPLY    3U

#define NCLP_SHARED_SUBMIT_ACCEPTED  0
#define NCLP_SHARED_SUBMIT_NOT_READY 1
#define NCLP_SHARED_SUBMIT_BUSY      2

#define NCLP_SHARED_POLL_PENDING     0
#define NCLP_SHARED_POLL_COMPLETE    1
#define NCLP_SHARED_POLL_IDLE        2
#define NCLP_SHARED_POLL_LOST       -1

#define NCLP_SHARED_SNAPSHOT_OK       0
#define NCLP_SHARED_SNAPSHOT_BUSY     1
#define NCLP_SHARED_SNAPSHOT_NOT_READY 2

#define NCLP_SHARED_CANCEL_ACCEPTED   0
#define NCLP_SHARED_CANCEL_NOT_ACTIVE 1
#define NCLP_SHARED_CANCEL_NOT_READY  2
#define NCLP_SHARED_CANCEL_BUSY       3

typedef struct {
    uint32_t publish_seq;
    uint32_t operation;
    uint32_t state;
    uint32_t result_id;
    uint32_t completed_units;
    uint32_t total_units;
    uint32_t current_stream;
    uint32_t current_channel;
    uint32_t current_cap_range;
    uint32_t status;
    uint32_t fail_count;
    uint32_t reserved[5];
} nclp_progress_t;

typedef struct {
    uint32_t publish_seq;
    uint32_t generation;
    uint32_t sample_rate_hz;
    uint32_t analog_lower_millihz;
    uint32_t analog_upper_hz;
    uint32_t dsp_enable;
    uint32_t dsp_requested_millihz;
    uint32_t dsp_actual_millihz;
    uint32_t dsp_code;
    uint32_t ttl_settle_enable;
    uint32_t ttl_settle_channel;
    uint32_t flags;
    uint32_t reserved[4];
} nclp_config_snapshot_t;

typedef struct {
    uint32_t control_heartbeat;
    uint32_t network_heartbeat;
    uint32_t command_state;
    uint32_t operation_generation;
    uint32_t stream_active;
    uint32_t stream_mask;
    uint32_t stream_error;
    uint32_t stream_session_id;
    uint32_t stream_layout_id;
    uint32_t stream_aux_mode;
    uint32_t physical_chip_mask;
    uint32_t detected_chip_ids;
    uint32_t control_status_word;
    uint32_t operation;
    uint32_t operation_state;
    uint32_t result_id;
    uint32_t progress_status;
} nclp_runtime_snapshot_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t control_heartbeat;
    uint32_t network_heartbeat;
    uint32_t command_state;
    /* A53-0 publishes its canonical control/status word together with the
     * detected topology under this seqlock.  The command begins at the same
     * 64-byte boundary as earlier ABI revisions. */
    uint32_t control_publish_seq;
    uint32_t control_status_word;
    uint32_t reserved0[9];

    nclp_main_command_t command;
    nclp_main_reply_t reply;
    uint32_t reserved1[2];

    uint32_t stream_active;
    uint32_t stream_count;
    uint32_t stream_mask;
    uint32_t ring_address_low;
    uint32_t ring_address_high;
    uint32_t ring_bytes;
    uint32_t block_bytes;
    uint32_t produced_blocks;
    uint32_t consumed_blocks;
    uint32_t network_abandoned_blocks;
    uint32_t stream_error;
    uint32_t stream_session_id;
    uint32_t stream_layout_id;
    uint32_t stream_aux_mode;
    uint32_t physical_chip_mask;
    uint32_t network_drained_session;
    uint32_t final_block_bytes;
    uint32_t detected_chip_ids;

    /* A53-0 increments operation_generation before starting each exclusive
     * hardware operation. A53-1 binds cancellation to that generation so a
     * delayed request can never cancel a later impedance measurement. */
    uint32_t operation_generation;
    uint32_t cancel_request_token;
    uint32_t cancel_target_operation;
    uint32_t cancel_target_result_id;
    uint32_t cancel_target_generation;
    uint32_t cancel_ack_token;
    uint32_t cancel_ack_status;
    /* A53-0 seqlock for the STOP publication tuple
     * {stream_active, produced_blocks, final_block_bytes}. */
    uint32_t stream_publish_seq;

    nclp_config_snapshot_t config;
    nclp_progress_t progress;
} nclp_shared_state_t;

_Static_assert(sizeof(nclp_progress_t) == 64U,
               "progress snapshot must be one cache line");
_Static_assert(sizeof(nclp_config_snapshot_t) == 64U,
               "configuration snapshot must be one cache line");
_Static_assert(sizeof(nclp_shared_state_t) <= NCLP_SHARED_CONTROL_BYTES,
               "control mailbox exceeds reserved page");

static inline volatile nclp_shared_state_t *nclp_shared_state(void)
{
    return (volatile nclp_shared_state_t *)(uintptr_t)NCLP_SHARED_BASE_ADDRESS;
}

static inline void nclp_shared_configure_memory(void)
{
    /* Keep inter-core ownership explicit; cached read/modify/write would let
     * one A53 overwrite fields just published by the other A53. */
    Xil_SetTlbAttributes((UINTPTR)NCLP_SHARED_BASE_ADDRESS, NORM_NONCACHE);
    Xil_SetTlbAttributes((UINTPTR)NCLP_RESULT_BASE_ADDRESS, NORM_NONCACHE);
    __asm__ volatile("dsb sy\n\tisb" ::: "memory");
}

int nclp_shared_try_submit_command(const nclp_main_command_t *command);
int nclp_shared_poll_reply(nclp_main_reply_t *reply);
int nclp_shared_read_config(nclp_config_snapshot_t *snapshot);
int nclp_shared_read_runtime(nclp_runtime_snapshot_t *snapshot);
int nclp_shared_request_impedance_cancel(uint32_t *token_out,
                                         uint32_t *result_id_out,
                                         uint32_t *generation_out,
                                         uint32_t *ack_token_out);

#endif
