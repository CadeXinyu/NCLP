#include "../ipc/nclp_shared.h"
#include "../network/ps_ethernet.h"
#include "../network/sfp_control.h"
#include "../protocol/nclp_command_service.h"
#include "../network/nclp_udp_packetizer.h"

#include "xil_cache.h"
#include "xil_printf.h"
#include "sleep.h"

#include <stdint.h>

#define NCLP_UDP_DRAIN_RETRY_LIMIT 20000U
#ifndef NCLP_SFP_TAKEOVER_RETRY_LIMIT
#define NCLP_SFP_TAKEOVER_RETRY_LIMIT 20000U
#endif
#ifndef NCLP_ETHERNET_REPLY_RETIRE_RETRY_LIMIT
#define NCLP_ETHERNET_REPLY_RETIRE_RETRY_LIMIT 20000U
#endif

static nclp_udp_packetizer_t g_udp_packetizer;
static uint32_t g_udp_packetizer_active;
static uint32_t g_udp_packetizer_session;
static uint32_t g_udp_block_active;
static uint32_t g_udp_block_number;
static uint32_t g_udp_block_offset;
static uint32_t g_udp_drain_retry_count;
static uint32_t g_local_udp_takeover_started;
static uint32_t g_local_udp_takeover_retry_count;
static uint32_t g_ethernet_reply_retire_retry_count;

typedef enum {
    NCLP_APPLICATION_TRANSPORT_ETHERNET = 0,
    NCLP_APPLICATION_TRANSPORT_SFP = 1
} nclp_application_transport_t;

static nclp_application_transport_t g_active_application_transport =
    NCLP_APPLICATION_TRANSPORT_ETHERNET;
static nclp_application_transport_t g_requested_application_transport =
    NCLP_APPLICATION_TRANSPORT_ETHERNET;
static uint32_t g_application_transport_transition_pending;

typedef struct {
    uint32_t active;
    uint32_t produced_blocks;
    uint32_t final_block_bytes;
    uint32_t session_id;
    uint32_t stream_mask;
    uint32_t layout_id;
    uint32_t aux_mode;
    uint32_t ring_address_low;
    uint32_t ring_address_high;
    uint32_t ring_bytes;
    uint32_t block_bytes;
} nclp_recording_snapshot_t;

static void shared_barrier(void)
{
#if defined(NCLP_NETWORK_HOST_TEST)
    __asm__ volatile("" ::: "memory");
#else
    __asm__ volatile("dmb sy" ::: "memory");
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

static int read_recording_snapshot(nclp_recording_snapshot_t *snapshot)
{
    volatile nclp_shared_state_t *shared = nclp_shared_state();
    uint32_t before;
    uint32_t after;

    if (snapshot == NULL) {
        return -1;
    }
    for (uint32_t attempt = 0U; attempt < 8U; ++attempt) {
        shared_invalidate();
        before = shared->stream_publish_seq;
        if ((before & 1U) != 0U) {
            continue;
        }
        shared_barrier();
        snapshot->active = shared->stream_active;
        snapshot->produced_blocks = shared->produced_blocks;
        snapshot->final_block_bytes = shared->final_block_bytes;
        snapshot->session_id = shared->stream_session_id;
        snapshot->stream_mask = shared->stream_mask;
        snapshot->layout_id = shared->stream_layout_id;
        snapshot->aux_mode = shared->stream_aux_mode;
        snapshot->ring_address_low = shared->ring_address_low;
        snapshot->ring_address_high = shared->ring_address_high;
        snapshot->ring_bytes = shared->ring_bytes;
        snapshot->block_bytes = shared->block_bytes;
        shared_barrier();
        after = shared->stream_publish_seq;
        if (before == after && (after & 1U) == 0U) {
            return 0;
        }
    }
    return -1;
}

static int emit_udp_datagram(void *context, const uint8_t *datagram,
                             uint16_t datagram_bytes)
{
    (void)context;
    return nclp_ps_ethernet_send_datagram(datagram, datagram_bytes);
}

static int configure_udp_packetizer(
    const nclp_recording_snapshot_t *recording)
{
    nclp_udp_packetizer_config_t config;
    nclp_config_snapshot_t stored_config;
    int result;

    if (recording == NULL ||
        nclp_shared_read_config(&stored_config) != NCLP_SHARED_SNAPSHOT_OK) {
        return -1;
    }
    config.session_id = recording->session_id;
    config.sample_rate_hz = stored_config.sample_rate_hz;
    config.logical_stream_mask = (uint16_t)recording->stream_mask;
    config.layout_id = (uint16_t)recording->layout_id;
    config.sample_mode = recording->aux_mode == NCLP_SAMPLE_MODE_VDD ?
                         NCLP_UDP_MODE_VDD : NCLP_UDP_MODE_AUX;
    result = nclp_udp_packetizer_init(&g_udp_packetizer, &config);
    if (result != NCLP_UDP_PACKETIZER_OK) {
        xil_printf("  FAIL UDP packetizer config   result=%d mask=0x%04x rate=%lu\r\n",
                   result, config.logical_stream_mask,
                   (unsigned long)config.sample_rate_hz);
        return -1;
    }

    g_udp_packetizer_active = 1U;
    g_udp_packetizer_session = config.session_id;
    g_udp_block_active = 0U;
    g_udp_block_number = 0U;
    g_udp_block_offset = 0U;
    g_udp_drain_retry_count = 0U;
    xil_printf("  PASS UDP packetizer session  id=%lu mask=0x%04x rate=%lu input_frame=%u normalized_frame=%u datagram=%u mode=%s\r\n",
               (unsigned long)config.session_id,
               config.logical_stream_mask,
               (unsigned long)config.sample_rate_hz,
               g_udp_packetizer.input_frame_bytes,
               NCLP_UDP_FIXED_FRAME_BYTES,
               NCLP_UDP_FIXED_DATAGRAM_BYTES,
               config.sample_mode == NCLP_UDP_MODE_VDD ? "VDD" : "AUX1/2/3");
    return 0;
}

static void abandon_timed_out_udp_drain(void)
{
    volatile nclp_shared_state_t *shared = nclp_shared_state();
    nclp_recording_snapshot_t recording;
    uint32_t abandoned_blocks;
    uint32_t drained_session = g_udp_packetizer_session;

    if (read_recording_snapshot(&recording) != 0) {
        return;
    }
    shared_invalidate();
    abandoned_blocks = recording.produced_blocks - shared->consumed_blocks;
    if (abandoned_blocks == 0U &&
        (g_udp_packetizer.datagram_pending != 0U ||
         g_udp_packetizer.input_frame_bytes_collected != 0U)) {
        abandoned_blocks = 1U;
    }
    shared->network_abandoned_blocks += abandoned_blocks;
    shared->consumed_blocks = recording.produced_blocks;
    shared->stream_error = NCLP_STREAM_ERROR_DRAIN_TIMEOUT;
    shared->network_drained_session = drained_session;
    shared->network_heartbeat++;
    shared_flush();

    xil_printf("  WARN UDP drain timeout       session=%lu abandoned_blocks=%lu\r\n",
               (unsigned long)drained_session,
               (unsigned long)abandoned_blocks);
    g_udp_packetizer_active = 0U;
    g_udp_packetizer_session = 0U;
    g_udp_block_active = 0U;
    g_udp_block_number = 0U;
    g_udp_block_offset = 0U;
    g_udp_drain_retry_count = 0U;
}

static void note_udp_send_failure(void)
{
    nclp_recording_snapshot_t recording;

    if (read_recording_snapshot(&recording) != 0) {
        return;
    }
    if (recording.active != 0U) {
        g_udp_drain_retry_count = 0U;
        return;
    }
    if (++g_udp_drain_retry_count >= NCLP_UDP_DRAIN_RETRY_LIMIT) {
        abandon_timed_out_udp_drain();
    }
}

static int finish_udp_packetizer(void)
{
    volatile nclp_shared_state_t *shared = nclp_shared_state();
    nclp_recording_snapshot_t recording;
    uint32_t drained_session;
    int result;

    if (g_udp_packetizer_active == 0U) {
        return 0;
    }
    result = nclp_udp_packetizer_flush(&g_udp_packetizer,
                                       emit_udp_datagram, NULL);
    if (result == NCLP_UDP_PACKETIZER_SEND_FAILED) {
        note_udp_send_failure();
        return 0;
    }
    if (result != NCLP_UDP_PACKETIZER_OK) {
        return -1;
    }
    drained_session = g_udp_packetizer_session;
    g_udp_packetizer_active = 0U;
    g_udp_packetizer_session = 0U;
    g_udp_block_active = 0U;
    g_udp_drain_retry_count = 0U;
    if (read_recording_snapshot(&recording) != 0) {
        return -1;
    }
    shared_invalidate();
    if (recording.active == 0U &&
        recording.session_id == drained_session &&
        recording.produced_blocks == shared->consumed_blocks) {
        shared->network_drained_session = drained_session;
        shared->network_heartbeat++;
        shared_flush();
    }
    return 0;
}

/* Stop local delivery without emitting another UDP datagram. The A53-0
 * producer may still be retiring its local session, so keep advancing the
 * shared consumer count until its final snapshot is inactive. Only then mark
 * the exact session drained and allow SFP application ownership to open. */
static int retire_local_udp_for_sfp(void)
{
    volatile nclp_shared_state_t *shared = nclp_shared_state();
    nclp_recording_snapshot_t recording;
    uint32_t abandoned_blocks;
    uint32_t buffered_partial;
    uint32_t consumed_blocks;
    uint32_t update_shared;

    if (read_recording_snapshot(&recording) != 0) {
        return 0;
    }

    buffered_partial =
        g_udp_packetizer_active != 0U &&
        (g_udp_packetizer.datagram_pending != 0U ||
         g_udp_packetizer.input_frame_bytes_collected != 0U ||
         g_udp_block_active != 0U);
    g_udp_packetizer_active = 0U;
    g_udp_packetizer_session = 0U;
    g_udp_block_active = 0U;
    g_udp_block_number = 0U;
    g_udp_block_offset = 0U;
    g_udp_drain_retry_count = 0U;

    if (recording.session_id == 0U) {
        return 1;
    }

    shared_invalidate();
    if (shared->stream_session_id != recording.session_id) {
        return 0;
    }
    consumed_blocks = shared->consumed_blocks;
    abandoned_blocks = recording.produced_blocks - consumed_blocks;
    if (buffered_partial != 0U && abandoned_blocks == 0U) {
        abandoned_blocks = 1U;
    }
    update_shared = abandoned_blocks != 0U ||
                    consumed_blocks != recording.produced_blocks ||
                    (recording.active == 0U &&
                     shared->network_drained_session != recording.session_id);
    if (update_shared != 0U) {
        shared->network_abandoned_blocks += abandoned_blocks;
        shared->consumed_blocks = recording.produced_blocks;
        if (recording.active == 0U) {
            shared->network_drained_session = recording.session_id;
        }
        shared->network_heartbeat++;
        shared_flush();
    }
    return recording.active == 0U;
}

static int service_local_udp_stream(void)
{
    volatile nclp_shared_state_t *shared = nclp_shared_state();
    nclp_recording_snapshot_t recording;
    uint64_t ring_address;
    uint32_t consumed_blocks;
    uint32_t block_bytes;
    uint32_t offset;
    const uint8_t *block;
    size_t accepted = 0U;
    int result;

    shared_invalidate();
    if (shared->magic != NCLP_SHARED_MAGIC ||
        shared->version != NCLP_SHARED_VERSION) {
        return 0;
    }
    if (read_recording_snapshot(&recording) != 0) {
        return 0;
    }
    if (g_udp_packetizer_active == 0U) {
        if (recording.session_id == 0U) {
            return 0;
        }
        if (recording.active == 0U &&
            recording.produced_blocks == shared->consumed_blocks) {
            if (shared->network_drained_session != recording.session_id) {
                shared->network_drained_session = recording.session_id;
                shared->network_heartbeat++;
                shared_flush();
            }
            return 0;
        }
        if (configure_udp_packetizer(&recording) != 0) {
            shared->stream_error = NCLP_STREAM_ERROR_UDP_CONFIG;
            shared_flush();
            return -1;
        }
    } else if (g_udp_packetizer_session != recording.session_id) {
        shared->stream_error = NCLP_STREAM_ERROR_UDP_SESSION;
        shared_flush();
        return -1;
    }

    /* Retry a full datagram retained after a transient lwIP allocation/send
     * failure before consuming more DDR bytes. */
    result = nclp_udp_packetizer_feed(&g_udp_packetizer, NULL, 0U,
                                      emit_udp_datagram, NULL, &accepted);
    if (result == NCLP_UDP_PACKETIZER_SEND_FAILED) {
        note_udp_send_failure();
        return 0;
    }
    if (result != NCLP_UDP_PACKETIZER_OK) {
        return -1;
    }
    g_udp_drain_retry_count = 0U;

    if (read_recording_snapshot(&recording) != 0) {
        return 0;
    }
    shared_invalidate();
    if (recording.produced_blocks == shared->consumed_blocks &&
        g_udp_block_active == 0U) {
        return recording.active == 0U ? finish_udp_packetizer() : 0;
    }
    if (recording.block_bytes == 0U ||
        recording.ring_bytes < recording.block_bytes) {
        return -1;
    }

    consumed_blocks = shared->consumed_blocks;
    if (g_udp_block_active == 0U) {
        g_udp_block_active = 1U;
        g_udp_block_number = consumed_blocks;
        g_udp_block_offset = 0U;
    }
    if (g_udp_block_number != consumed_blocks) {
        g_udp_block_active = 0U;
        return -1;
    }

    ring_address = (uint64_t)recording.ring_address_low |
                   ((uint64_t)recording.ring_address_high << 32);
    offset = (consumed_blocks * recording.block_bytes) % recording.ring_bytes;
    block = (const uint8_t *)(uintptr_t)(ring_address + offset);
    block_bytes = recording.block_bytes;
    if (recording.active == 0U &&
        (consumed_blocks + 1U) == recording.produced_blocks &&
        recording.final_block_bytes != 0U &&
        recording.final_block_bytes <= block_bytes) {
        block_bytes = recording.final_block_bytes;
    }
    Xil_DCacheInvalidateRange((INTPTR)block, block_bytes);

    result = nclp_udp_packetizer_feed(
        &g_udp_packetizer, block + g_udp_block_offset,
        block_bytes - g_udp_block_offset,
        emit_udp_datagram, NULL, &accepted);
    g_udp_block_offset += (uint32_t)accepted;
    if (result != NCLP_UDP_PACKETIZER_OK &&
        result != NCLP_UDP_PACKETIZER_SEND_FAILED) {
        return -1;
    }
    if (g_udp_block_offset != block_bytes) {
        return 0;
    }

    if (read_recording_snapshot(&recording) != 0 ||
        recording.session_id == 0U ||
        recording.session_id != g_udp_packetizer_session) {
        g_udp_block_active = 0U;
        g_udp_block_offset = 0U;
        return 0;
    }
    shared_invalidate();
    shared->consumed_blocks = consumed_blocks + 1U;
    shared->network_heartbeat++;
    shared_flush();
    g_udp_block_active = 0U;
    g_udp_block_offset = 0U;
    return 0;
}

static void set_application_transport_enabled(
    nclp_application_transport_t transport)
{
    nclp_ps_ethernet_set_application_enabled(
        transport == NCLP_APPLICATION_TRANSPORT_ETHERNET ? 1U : 0U);
    nclp_sfp_control_set_command_ingress_enabled(
        transport == NCLP_APPLICATION_TRANSPORT_SFP ? 1U : 0U);
}

static void begin_application_transport_transition(
    nclp_application_transport_t requested)
{
    g_requested_application_transport = requested;
    g_application_transport_transition_pending = 1U;
    g_ethernet_reply_retire_retry_count = 0U;
    /* Stop both ingress paths immediately. The old transport still services
     * its already-owned reply until its driver reports idle. */
    nclp_ps_ethernet_set_application_enabled(0U);
    nclp_sfp_control_set_command_ingress_enabled(0U);
    if (g_active_application_transport == NCLP_APPLICATION_TRANSPORT_ETHERNET &&
        requested == NCLP_APPLICATION_TRANSPORT_SFP) {
        g_local_udp_takeover_started = 1U;
        g_local_udp_takeover_retry_count = 0U;
    } else if (requested == NCLP_APPLICATION_TRANSPORT_ETHERNET) {
        g_local_udp_takeover_started = 0U;
        g_local_udp_takeover_retry_count = 0U;
    }
}

static void service_application_transport_selection(void)
{
    nclp_application_transport_t selected =
        nclp_sfp_link_usable() != 0 ? NCLP_APPLICATION_TRANSPORT_SFP :
                                     NCLP_APPLICATION_TRANSPORT_ETHERNET;
    int old_transport_idle;

    if (g_application_transport_transition_pending == 0U) {
        if (selected == g_active_application_transport) {
            if (selected == NCLP_APPLICATION_TRANSPORT_SFP &&
                g_local_udp_takeover_started != 0U &&
                retire_local_udp_for_sfp() != 0) {
                g_local_udp_takeover_started = 0U;
                g_local_udp_takeover_retry_count = 0U;
            }
            return;
        }
        begin_application_transport_transition(selected);
    } else {
        g_requested_application_transport = selected;
    }

    if (g_requested_application_transport == g_active_application_transport) {
        /* A link blip must not leave both ingress paths muted while a local
         * producer is still active. No ownership transfer occurred, so the
         * current owner can resume immediately. */
        if (g_active_application_transport ==
            NCLP_APPLICATION_TRANSPORT_ETHERNET) {
            g_local_udp_takeover_started = 0U;
            g_local_udp_takeover_retry_count = 0U;
        }
        g_ethernet_reply_retire_retry_count = 0U;
        g_application_transport_transition_pending = 0U;
        set_application_transport_enabled(g_active_application_transport);
        return;
    }

    if (g_local_udp_takeover_started != 0U &&
        g_active_application_transport == NCLP_APPLICATION_TRANSPORT_ETHERNET &&
        g_requested_application_transport == NCLP_APPLICATION_TRANSPORT_SFP) {
        /* An accepted Ethernet command may publish a new local session when
         * it completes. Keep the retirement fence armed until that command
         * has finished, then retire the resulting recording snapshot. */
        if (nclp_command_service_pending() != 0) {
            return;
        }
        if (retire_local_udp_for_sfp() == 0) {
            if (g_local_udp_takeover_retry_count <
                NCLP_SFP_TAKEOVER_RETRY_LIMIT) {
                g_local_udp_takeover_retry_count++;
            }
            if (g_local_udp_takeover_retry_count <
                NCLP_SFP_TAKEOVER_RETRY_LIMIT) {
                return;
            }
        } else {
            g_local_udp_takeover_started = 0U;
            g_local_udp_takeover_retry_count = 0U;
        }
    }

    if (nclp_command_service_pending() != 0) {
        return;
    }
    old_transport_idle =
        g_active_application_transport == NCLP_APPLICATION_TRANSPORT_ETHERNET ?
        nclp_ps_ethernet_control_idle() : nclp_sfp_control_path_idle();
    if (old_transport_idle == 0 &&
        g_active_application_transport == NCLP_APPLICATION_TRANSPORT_ETHERNET &&
        g_requested_application_transport == NCLP_APPLICATION_TRANSPORT_SFP) {
        if (g_ethernet_reply_retire_retry_count <
            NCLP_ETHERNET_REPLY_RETIRE_RETRY_LIMIT) {
            g_ethernet_reply_retire_retry_count++;
        }
        if (g_ethernet_reply_retire_retry_count >=
            NCLP_ETHERNET_REPLY_RETIRE_RETRY_LIMIT &&
            nclp_ps_ethernet_force_retire_control() == 0) {
            old_transport_idle = nclp_ps_ethernet_control_idle();
        }
    }
    if (old_transport_idle == 0) {
        return;
    }

    g_active_application_transport = g_requested_application_transport;
    g_application_transport_transition_pending = 0U;
    g_ethernet_reply_retire_retry_count = 0U;
    set_application_transport_enabled(g_active_application_transport);
    if (g_active_application_transport == NCLP_APPLICATION_TRANSPORT_SFP &&
        g_local_udp_takeover_started != 0U) {
        xil_printf("  WARN SFP control opened while local stream retirement remains pending\r\n");
    } else {
        xil_printf("  INFO control/data transport  %s\r\n",
                   g_active_application_transport ==
                           NCLP_APPLICATION_TRANSPORT_SFP ?
                       "SFP" : "Ethernet");
    }
}

#if !defined(NCLP_NETWORK_HOST_TEST)
int main(void)
{
    int ethernet_ready;
    xil_printf("\r\nNCLP A53-1 network firmware\r\n");
    xil_printf("Role: TCP/SFP control and local-mode UDP sample streaming.\r\n");

    nclp_shared_configure_memory();
    nclp_command_service_init();
    if (nclp_sfp_control_init() != 0) {
        xil_printf("  WARN SFP control unavailable: compute fabric or packet mailbox init failed\r\n");
    }
    ethernet_ready = nclp_ps_ethernet_init() == 0;
    if (!ethernet_ready) {
        xil_printf("  WARN Ethernet unavailable; SFP control service remains active\r\n");
    }

    g_active_application_transport = NCLP_APPLICATION_TRANSPORT_ETHERNET;
    g_requested_application_transport = NCLP_APPLICATION_TRANSPORT_ETHERNET;
    g_application_transport_transition_pending = 0U;
    g_local_udp_takeover_started = 0U;
    g_local_udp_takeover_retry_count = 0U;
    g_ethernet_reply_retire_retry_count = 0U;
    set_application_transport_enabled(NCLP_APPLICATION_TRANSPORT_ETHERNET);

    xil_printf("  WAIT A53-0 control firmware shared mailbox\r\n");
    for (;;) {
        service_application_transport_selection();
        if (ethernet_ready) {
            nclp_ps_ethernet_poll();
        }
        nclp_sfp_control_poll();
        nclp_command_service_poll();
        service_application_transport_selection();
        if (g_application_transport_transition_pending == 0U &&
            g_active_application_transport ==
                NCLP_APPLICATION_TRANSPORT_ETHERNET) {
            (void)service_local_udp_stream();
        }
        usleep(50U);
    }
}
#endif
