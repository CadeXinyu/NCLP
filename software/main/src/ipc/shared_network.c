#include "nclp_shared.h"

#include "xil_cache.h"

#include <stddef.h>

static uint32_t g_command_pending;
static uint32_t g_pending_command;
static uint32_t g_pending_sequence;

static void shared_barrier(void)
{
#if defined(__aarch64__)
    __asm__ volatile("dmb sy" ::: "memory");
#else
    __atomic_thread_fence(__ATOMIC_SEQ_CST);
#endif
}

static void shared_invalidate(void)
{
    Xil_DCacheInvalidateRange((INTPTR)nclp_shared_state(),
                              sizeof(nclp_shared_state_t));
    shared_barrier();
}

static void shared_flush(void)
{
    shared_barrier();
    Xil_DCacheFlushRange((INTPTR)nclp_shared_state(),
                         sizeof(nclp_shared_state_t));
    shared_barrier();
}

static void copy_command_to_shared(volatile nclp_main_command_t *dst,
                                   const nclp_main_command_t *src)
{
    dst->command = src->command;
    dst->sequence = src->sequence;
    for (uint32_t i = 0U; i < (NCLP_CMD_WORDS - 4U); ++i) {
        dst->args[i] = src->args[i];
    }
}

static void copy_reply_from_shared(nclp_main_reply_t *dst,
                                   const volatile nclp_main_reply_t *src)
{
    dst->magic = src->magic;
    dst->version = src->version;
    dst->command = src->command;
    dst->sequence = src->sequence;
    dst->status = src->status;
    dst->data0 = src->data0;
    dst->data1 = src->data1;
    dst->data2 = src->data2;
    dst->data3 = src->data3;
    dst->fail_count = src->fail_count;
}

int nclp_shared_try_submit_command(const nclp_main_command_t *command)
{
    volatile nclp_shared_state_t *shared = nclp_shared_state();

    if (command == NULL) {
        return -1;
    }
    if (g_command_pending != 0U) {
        return NCLP_SHARED_SUBMIT_BUSY;
    }

    shared_invalidate();
    if (shared->magic != NCLP_SHARED_MAGIC ||
        shared->version != NCLP_SHARED_VERSION) {
        return NCLP_SHARED_SUBMIT_NOT_READY;
    }

    /* A53-1 may have restarted after A53-0 published an old reply.  No local
     * request can own that reply, so consume it before accepting new work. */
    if (shared->command_state == NCLP_SHARED_CMD_REPLY) {
        shared->command_state = NCLP_SHARED_CMD_IDLE;
        shared_flush();
        shared_invalidate();
    }
    if (shared->command_state != NCLP_SHARED_CMD_IDLE) {
        return NCLP_SHARED_SUBMIT_BUSY;
    }

    copy_command_to_shared(&shared->command, command);
    shared_barrier();
    shared->command_state = NCLP_SHARED_CMD_REQUEST;
    shared_flush();
    g_command_pending = 1U;
    g_pending_command = command->command;
    g_pending_sequence = command->sequence;
    return NCLP_SHARED_SUBMIT_ACCEPTED;
}

int nclp_shared_poll_reply(nclp_main_reply_t *reply)
{
    volatile nclp_shared_state_t *shared = nclp_shared_state();
    int valid;

    if (reply == NULL) {
        return NCLP_SHARED_POLL_LOST;
    }
    if (g_command_pending == 0U) {
        return NCLP_SHARED_POLL_IDLE;
    }

    shared_invalidate();
    if (shared->magic != NCLP_SHARED_MAGIC ||
        shared->version != NCLP_SHARED_VERSION) {
        g_command_pending = 0U;
        g_pending_command = 0U;
        g_pending_sequence = 0U;
        return NCLP_SHARED_POLL_LOST;
    }
    if (shared->command_state == NCLP_SHARED_CMD_REQUEST ||
        shared->command_state == NCLP_SHARED_CMD_RUNNING) {
        return NCLP_SHARED_POLL_PENDING;
    }
    if (shared->command_state != NCLP_SHARED_CMD_REPLY) {
        g_command_pending = 0U;
        g_pending_command = 0U;
        g_pending_sequence = 0U;
        return NCLP_SHARED_POLL_LOST;
    }

    shared_barrier();
    copy_reply_from_shared(reply, &shared->reply);
    valid = reply->magic == NCLP_CMD_MAGIC &&
            reply->version == NCLP_CMD_VERSION &&
            reply->command == g_pending_command &&
            reply->sequence == g_pending_sequence;
    shared->command_state = NCLP_SHARED_CMD_IDLE;
    shared_flush();
    g_command_pending = 0U;
    g_pending_command = 0U;
    g_pending_sequence = 0U;
    return valid ? NCLP_SHARED_POLL_COMPLETE : NCLP_SHARED_POLL_LOST;
}

static void copy_config_from_shared(
    nclp_config_snapshot_t *dst,
    const volatile nclp_config_snapshot_t *src)
{
    dst->publish_seq = src->publish_seq;
    dst->generation = src->generation;
    dst->sample_rate_hz = src->sample_rate_hz;
    dst->analog_lower_millihz = src->analog_lower_millihz;
    dst->analog_upper_hz = src->analog_upper_hz;
    dst->dsp_enable = src->dsp_enable;
    dst->dsp_requested_millihz = src->dsp_requested_millihz;
    dst->dsp_actual_millihz = src->dsp_actual_millihz;
    dst->dsp_code = src->dsp_code;
    dst->ttl_settle_enable = src->ttl_settle_enable;
    dst->ttl_settle_channel = src->ttl_settle_channel;
    dst->flags = src->flags;
    for (uint32_t i = 0U; i < 4U; ++i) {
        dst->reserved[i] = src->reserved[i];
    }
}

int nclp_shared_read_config(nclp_config_snapshot_t *snapshot)
{
    volatile nclp_shared_state_t *shared = nclp_shared_state();
    uint32_t before;
    uint32_t after;

    if (snapshot == NULL) {
        return NCLP_SHARED_SNAPSHOT_NOT_READY;
    }
    shared_invalidate();
    if (shared->magic != NCLP_SHARED_MAGIC ||
        shared->version != NCLP_SHARED_VERSION) {
        return NCLP_SHARED_SNAPSHOT_NOT_READY;
    }
    before = shared->config.publish_seq;
    if ((before & 1U) != 0U) {
        return NCLP_SHARED_SNAPSHOT_BUSY;
    }
    copy_config_from_shared(snapshot, &shared->config);
    shared_barrier();
    after = shared->config.publish_seq;
    if (before != after || (after & 1U) != 0U) {
        return NCLP_SHARED_SNAPSHOT_BUSY;
    }
    return NCLP_SHARED_SNAPSHOT_OK;
}

int nclp_shared_read_runtime(nclp_runtime_snapshot_t *snapshot)
{
    volatile nclp_shared_state_t *shared = nclp_shared_state();
    uint32_t control_before;
    uint32_t control_after;
    uint32_t stream_before;
    uint32_t stream_after;
    uint32_t progress_before;
    uint32_t progress_after;

    if (snapshot == NULL) {
        return NCLP_SHARED_SNAPSHOT_NOT_READY;
    }
    shared_invalidate();
    if (shared->magic != NCLP_SHARED_MAGIC ||
        shared->version != NCLP_SHARED_VERSION) {
        return NCLP_SHARED_SNAPSHOT_NOT_READY;
    }
    control_before = shared->control_publish_seq;
    if ((control_before & 1U) != 0U) {
        return NCLP_SHARED_SNAPSHOT_BUSY;
    }
    shared_barrier();
    snapshot->control_status_word = shared->control_status_word;
    snapshot->stream_mask = shared->stream_mask;
    snapshot->stream_layout_id = shared->stream_layout_id;
    snapshot->physical_chip_mask = shared->physical_chip_mask;
    snapshot->detected_chip_ids = shared->detected_chip_ids;
    shared_barrier();
    control_after = shared->control_publish_seq;
    if (control_before != control_after || (control_after & 1U) != 0U) {
        return NCLP_SHARED_SNAPSHOT_BUSY;
    }

    stream_before = shared->stream_publish_seq;
    if ((stream_before & 1U) != 0U) {
        return NCLP_SHARED_SNAPSHOT_BUSY;
    }
    shared_barrier();
    snapshot->stream_active = shared->stream_active;
    snapshot->stream_error = shared->stream_error;
    snapshot->stream_session_id = shared->stream_session_id;
    snapshot->stream_aux_mode = shared->stream_aux_mode;
    shared_barrier();
    stream_after = shared->stream_publish_seq;
    if (stream_before != stream_after || (stream_after & 1U) != 0U) {
        return NCLP_SHARED_SNAPSHOT_BUSY;
    }

    progress_before = shared->progress.publish_seq;
    if ((progress_before & 1U) != 0U) {
        return NCLP_SHARED_SNAPSHOT_BUSY;
    }
    shared_barrier();
    snapshot->operation_generation = shared->operation_generation;
    snapshot->operation = shared->progress.operation;
    snapshot->operation_state = shared->progress.state;
    snapshot->result_id = shared->progress.result_id;
    snapshot->progress_status = shared->progress.status;
    shared_barrier();
    progress_after = shared->progress.publish_seq;
    if (progress_before != progress_after ||
        (progress_after & 1U) != 0U ||
        snapshot->operation_generation != shared->operation_generation) {
        return NCLP_SHARED_SNAPSHOT_BUSY;
    }

    snapshot->control_heartbeat = shared->control_heartbeat;
    snapshot->network_heartbeat = shared->network_heartbeat;
    snapshot->command_state = shared->command_state;
    return NCLP_SHARED_SNAPSHOT_OK;
}

int nclp_shared_request_impedance_cancel(uint32_t *token_out,
                                         uint32_t *result_id_out,
                                         uint32_t *generation_out,
                                         uint32_t *ack_token_out)
{
    volatile nclp_shared_state_t *shared = nclp_shared_state();
    nclp_runtime_snapshot_t runtime;
    nclp_runtime_snapshot_t after;
    uint32_t token;
    int snapshot_result;

    snapshot_result = nclp_shared_read_runtime(&runtime);
    if (snapshot_result == NCLP_SHARED_SNAPSHOT_NOT_READY) {
        return NCLP_SHARED_CANCEL_NOT_READY;
    }
    if (snapshot_result != NCLP_SHARED_SNAPSHOT_OK) {
        return NCLP_SHARED_CANCEL_BUSY;
    }
    if (runtime.operation != NCLP_CMD_IMPEDANCE ||
        (runtime.operation_state != NCLP_STATE_IMPEDANCE &&
         runtime.operation_state != NCLP_STATE_CANCELLING) ||
        runtime.operation_generation == 0U) {
        return NCLP_SHARED_CANCEL_NOT_ACTIVE;
    }

    shared_invalidate();
    if (shared->magic != NCLP_SHARED_MAGIC ||
        shared->version != NCLP_SHARED_VERSION) {
        return NCLP_SHARED_CANCEL_NOT_READY;
    }
    if (shared->operation_generation != runtime.operation_generation ||
        shared->progress.result_id != runtime.result_id ||
        shared->progress.operation != NCLP_CMD_IMPEDANCE ||
        (shared->progress.state != NCLP_STATE_IMPEDANCE &&
         shared->progress.state != NCLP_STATE_CANCELLING)) {
        return NCLP_SHARED_CANCEL_BUSY;
    }

    /* Repeating CANCEL for the same operation is idempotent: return the
     * original token instead of publishing a second cancellation request. */
    if (shared->cancel_request_token != 0U &&
        shared->cancel_target_operation == NCLP_CMD_IMPEDANCE &&
        shared->cancel_target_result_id == runtime.result_id &&
        shared->cancel_target_generation == runtime.operation_generation) {
        token = shared->cancel_request_token;
    } else {
        token = shared->cancel_request_token + 1U;
        if (token == 0U) {
            token = 1U;
        }
        shared->cancel_target_operation = NCLP_CMD_IMPEDANCE;
        shared->cancel_target_result_id = runtime.result_id;
        shared->cancel_target_generation = runtime.operation_generation;
        shared_barrier();
        shared->cancel_request_token = token;
        shared_flush();
    }

    /* A53-0 publishes RESTORING before its final token latch.  Publish the
     * token first, then revalidate the operation before reporting ACCEPTED.
     * Consequently, either A53-0 must observe this token, or this caller sees
     * RESTORING/final state and reports that cancellation is no longer active. */
    snapshot_result = nclp_shared_read_runtime(&after);
    if (snapshot_result == NCLP_SHARED_SNAPSHOT_NOT_READY) {
        return NCLP_SHARED_CANCEL_NOT_READY;
    }
    if (snapshot_result != NCLP_SHARED_SNAPSHOT_OK) {
        return NCLP_SHARED_CANCEL_BUSY;
    }
    if (after.operation != NCLP_CMD_IMPEDANCE ||
        (after.operation_state != NCLP_STATE_IMPEDANCE &&
         after.operation_state != NCLP_STATE_CANCELLING) ||
        after.operation_generation != runtime.operation_generation ||
        after.result_id != runtime.result_id) {
        return NCLP_SHARED_CANCEL_NOT_ACTIVE;
    }

    if (token_out != NULL) {
        *token_out = token;
    }
    if (result_id_out != NULL) {
        *result_id_out = runtime.result_id;
    }
    if (generation_out != NULL) {
        *generation_out = runtime.operation_generation;
    }
    if (ack_token_out != NULL) {
        *ack_token_out = shared->cancel_ack_token;
    }
    return NCLP_SHARED_CANCEL_ACCEPTED;
}
