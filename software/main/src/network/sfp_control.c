#include "sfp_control.h"
#include "../protocol/nclp_command_service.h"
#include "../../../common/nclp_pl_registers.h"
#include "xil_io.h"

#include <stdint.h>
#include <string.h>

#define SFP_COMMAND_BYTE_COUNT (NCLP_CMD_WORDS * 4U)

#if SFP_MAILBOX_RX_WORD_COUNT != NCLP_CMD_WORDS
#error "The SFP RX mailbox must contain exactly one binary command"
#endif

#if SFP_MAILBOX_TX_WORD_COUNT != NCLP_REPLY_WORDS
#error "The SFP TX mailbox must contain exactly one binary reply"
#endif

static uint8_t g_replay_command[SFP_COMMAND_BYTE_COUNT];
static nclp_main_reply_t g_replay_reply;
static nclp_main_reply_t g_pending_reply;
static uint32_t g_sfp_control_ready;
static uint32_t g_command_ingress_enabled;
static uint32_t g_command_service_request_pending;
static uint32_t g_reply_commit_pending;
static uint32_t g_replay_sequence;
static uint32_t g_replay_cache_valid;

static uint32_t mailbox_read(uint32_t offset)
{
    return Xil_In32((UINTPTR)(NCLP_SFP_MAILBOX_BASE_DEFAULT + offset));
}

static void mailbox_write(uint32_t offset, uint32_t value)
{
    Xil_Out32((UINTPTR)(NCLP_SFP_MAILBOX_BASE_DEFAULT + offset), value);
}

static uint32_t compute_fabric_read(uint32_t offset)
{
    return Xil_In32((UINTPTR)(NCLP_COMPUTE_FABRIC_BASE_DEFAULT + offset));
}

static uint32_t read_le32(const uint8_t *source)
{
    return (uint32_t)source[0] | ((uint32_t)source[1] << 8U) |
           ((uint32_t)source[2] << 16U) | ((uint32_t)source[3] << 24U);
}

static void write_le32(uint8_t *destination, uint32_t value)
{
    for (uint32_t byte = 0U; byte < 4U; ++byte) {
        destination[byte] = (uint8_t)(value >> (byte * 8U));
    }
}

static int compute_fabric_identity_valid(void)
{
    return compute_fabric_read(COMPUTE_FABRIC_REG_BLOCK_ID) ==
               COMPUTE_FABRIC_BLOCK_ID_EXPECTED &&
           (compute_fabric_read(COMPUTE_FABRIC_REG_ABI_VERSION) &
            NCLP_ABI_MAJOR_MASK) ==
               (COMPUTE_FABRIC_ABI_VERSION_EXPECTED & NCLP_ABI_MAJOR_MASK) &&
           (compute_fabric_read(COMPUTE_FABRIC_REG_CAPABILITIES) &
            COMPUTE_FABRIC_CAPABILITIES_REQUIRED) ==
               COMPUTE_FABRIC_CAPABILITIES_REQUIRED;
}

static int mailbox_identity_valid(void)
{
    return mailbox_read(SFP_MAILBOX_REG_BLOCK_ID) ==
               SFP_MAILBOX_BLOCK_ID_EXPECTED &&
           mailbox_read(SFP_MAILBOX_REG_ABI_VERSION) ==
               SFP_MAILBOX_ABI_VERSION_EXPECTED &&
           (mailbox_read(SFP_MAILBOX_REG_CAPABILITIES) &
            SFP_MAILBOX_CAPABILITIES_REQUIRED) ==
               SFP_MAILBOX_CAPABILITIES_REQUIRED &&
           mailbox_read(SFP_MAILBOX_REG_INFO) ==
               SFP_MAILBOX_INFO_EXPECTED;
}

static int refresh_control_readiness(void)
{
    if (compute_fabric_identity_valid() == 0 ||
        mailbox_identity_valid() == 0) {
        g_sfp_control_ready = 0U;
        return -1;
    }
    /* IRQ_ENABLE is idempotent and does not alter mailbox traffic or retained
     * diagnostics. Startup-only cleanup remains in nclp_sfp_control_init(). */
    mailbox_write(SFP_MAILBOX_REG_IRQ_ENABLE,
                  SFP_MAILBOX_IRQ_RX_AVAILABLE |
                  SFP_MAILBOX_IRQ_TX_AURORA_ACCEPTED |
                  SFP_MAILBOX_IRQ_ERROR);
    g_sfp_control_ready = 1U;
    return 0;
}

static int decode_command(const uint8_t payload[SFP_COMMAND_BYTE_COUNT],
                          nclp_main_command_t *command)
{
    if (read_le32(payload) != NCLP_CMD_MAGIC ||
        read_le32(payload + 4U) != NCLP_CMD_VERSION) {
        return -1;
    }
    command->command = read_le32(payload + 8U);
    command->sequence = read_le32(payload + 12U);
    for (uint32_t argument = 0U; argument < NCLP_CMD_WORDS - 4U;
         ++argument) {
        command->args[argument] =
            read_le32(payload + (argument + 4U) * 4U);
    }
    return 0;
}

static void queue_protocol_rejection(uint32_t command, uint32_t sequence)
{
    memset(&g_pending_reply, 0, sizeof(g_pending_reply));
    g_pending_reply.magic = NCLP_CMD_MAGIC;
    g_pending_reply.version = NCLP_CMD_VERSION;
    g_pending_reply.command = command;
    g_pending_reply.sequence = sequence;
    g_pending_reply.status = NCLP_COMMAND_STATUS_ARGUMENT;
    g_reply_commit_pending = 1U;
    g_command_service_request_pending = 0U;
}

static int queue_service_reply(uint32_t sequence, const nclp_main_reply_t *reply)
{
    if (reply == NULL || g_reply_commit_pending != 0U ||
        sequence != g_replay_sequence) {
        return -1;
    }
    g_replay_reply = *reply;
    g_pending_reply = *reply;
    g_reply_commit_pending = 1U;
    g_command_service_request_pending = 0U;
    return 0;
}

static void acknowledge_mailbox_events(void)
{
    uint32_t status = mailbox_read(SFP_MAILBOX_REG_STATUS);

    if ((status & SFP_MAILBOX_STATUS_TX_AURORA_ACCEPTED) != 0U) {
        mailbox_write(SFP_MAILBOX_REG_COMMAND,
                      SFP_MAILBOX_COMMAND_CLEAR_TX_AURORA_ACCEPTED);
    }
    if ((status & SFP_MAILBOX_STATUS_ERROR_PENDING) != 0U) {
        uint32_t errors = mailbox_read(SFP_MAILBOX_REG_ERROR_STATUS) &
                          SFP_MAILBOX_ERROR_IMPLEMENTED_MASK;
        if (errors != 0U) {
            mailbox_write(SFP_MAILBOX_REG_ERROR_STATUS, errors);
        }
    }
}

static void commit_pending_reply(void)
{
    uint32_t status;
    const uint32_t words[SFP_MAILBOX_TX_WORD_COUNT] = {
        g_pending_reply.magic, g_pending_reply.version,
        g_pending_reply.command, g_pending_reply.sequence,
        g_pending_reply.status, g_pending_reply.data0,
        g_pending_reply.data1, g_pending_reply.data2,
        g_pending_reply.data3, g_pending_reply.fail_count
    };

    if (g_reply_commit_pending == 0U) {
        return;
    }
    status = mailbox_read(SFP_MAILBOX_REG_STATUS);
    if ((status & SFP_MAILBOX_STATUS_TX_SPACE_AVAILABLE) == 0U) {
        return;
    }
    for (uint32_t word = 0U; word < SFP_MAILBOX_TX_WORD_COUNT; ++word) {
        mailbox_write(SFP_MAILBOX_TX_DATA_WORD_OFFSET(word), words[word]);
    }
    mailbox_write(SFP_MAILBOX_REG_COMMAND, SFP_MAILBOX_COMMAND_TX_COMMIT);
    g_reply_commit_pending = 0U;
}

static void discard_uncommittable_reply_after_link_loss(void)
{
    /* Once SFP ownership has been revoked, a reply that still exists only in
     * software cannot be allowed to hold the transport transition forever.
     * Replies already committed to the PL mailbox are outside this state and
     * remain queued for transmission after the physical link recovers. */
    if (g_command_ingress_enabled == 0U &&
        g_reply_commit_pending != 0U &&
        nclp_sfp_link_usable() == 0) {
        memset(&g_pending_reply, 0, sizeof(g_pending_reply));
        g_reply_commit_pending = 0U;
    }
}

static void read_front_rx_command(uint8_t payload[SFP_COMMAND_BYTE_COUNT])
{
    for (uint32_t word = 0U; word < SFP_MAILBOX_RX_WORD_COUNT; ++word) {
        write_le32(payload + word * 4U,
                   mailbox_read(SFP_MAILBOX_RX_DATA_WORD_OFFSET(word)));
    }
}

static void dispatch_front_rx_command(void)
{
    uint8_t payload[SFP_COMMAND_BYTE_COUNT];
    nclp_main_command_t command = {0};
    uint32_t wire_command;
    uint32_t sequence;
    uint32_t sequence_delta;

    read_front_rx_command(payload);
    mailbox_write(SFP_MAILBOX_REG_COMMAND, SFP_MAILBOX_COMMAND_RX_POP);

    wire_command = read_le32(payload + 8U);
    sequence = read_le32(payload + 12U);
    if (decode_command(payload, &command) != 0 ||
        command.sequence == 0U ||
        command.command == NCLP_CMD_SET_UDP_DEST) {
        queue_protocol_rejection(wire_command, sequence);
        return;
    }
    if (g_replay_cache_valid != 0U && command.sequence == g_replay_sequence) {
        if (memcmp(payload, g_replay_command, SFP_COMMAND_BYTE_COUNT) == 0) {
            g_pending_reply = g_replay_reply;
            g_reply_commit_pending = 1U;
        } else {
            queue_protocol_rejection(command.command, command.sequence);
        }
        return;
    }
    sequence_delta = command.sequence - g_replay_sequence;
    if (g_replay_cache_valid != 0U && sequence_delta >= 0x80000000U) {
        queue_protocol_rejection(command.command, command.sequence);
        return;
    }

    g_replay_sequence = command.sequence;
    g_replay_cache_valid = 1U;
    memcpy(g_replay_command, payload, SFP_COMMAND_BYTE_COUNT);
    g_command_service_request_pending = 1U;
    if (nclp_command_service_submit(&command, queue_service_reply,
                                    command.sequence) < 0) {
        queue_protocol_rejection(command.command, command.sequence);
        g_replay_reply = g_pending_reply;
    }
}

int nclp_sfp_control_init(void)
{
    g_sfp_control_ready = 0U;
    g_command_ingress_enabled = 0U;
    g_command_service_request_pending = 0U;
    g_reply_commit_pending = 0U;
    g_replay_sequence = 0U;
    g_replay_cache_valid = 0U;
    memset(g_replay_command, 0, sizeof(g_replay_command));
    memset(&g_replay_reply, 0, sizeof(g_replay_reply));
    memset(&g_pending_reply, 0, sizeof(g_pending_reply));

    if (refresh_control_readiness() != 0) {
        return -1;
    }
    mailbox_write(SFP_MAILBOX_REG_ERROR_STATUS,
                  SFP_MAILBOX_ERROR_IMPLEMENTED_MASK);
    mailbox_write(SFP_MAILBOX_REG_COMMAND,
                  SFP_MAILBOX_COMMAND_CLEAR_TX_AURORA_ACCEPTED |
                  SFP_MAILBOX_COMMAND_CLEAR_DIAGNOSTICS);
    return 0;
}

int nclp_sfp_link_usable(void)
{
    uint32_t compute_link_status;
    uint32_t mailbox_status;

    /* Keep Ethernet application control reachable if this firmware cannot
     * validate or service the SFP mailbox ABI. A usable link requires the
     * compute-fabric link to be up without a latched fault and the mailbox's
     * synchronized link indication to agree. */
    if (g_sfp_control_ready == 0U) {
        return 0;
    }
    compute_link_status =
        compute_fabric_read(COMPUTE_FABRIC_REG_LINK_STATUS);
    mailbox_status = mailbox_read(SFP_MAILBOX_REG_STATUS);
    return (compute_link_status & (COMPUTE_FABRIC_LINK_STATUS_UP |
                                   COMPUTE_FABRIC_LINK_STATUS_FAULT)) ==
               COMPUTE_FABRIC_LINK_STATUS_UP &&
           (mailbox_status & SFP_MAILBOX_STATUS_LINK_UP) != 0U;
}

void nclp_sfp_control_set_command_ingress_enabled(uint32_t enabled)
{
    enabled = enabled != 0U ? 1U : 0U;
    if (enabled == g_command_ingress_enabled) {
        return;
    }
    g_command_ingress_enabled = enabled;
    /* The mailbox retains committed replies across physical-link loss, so the
     * matching firmware replay entry must span ownership blips as well. The
     * sequence domain is reset only by nclp_sfp_control_init(). */
}

int nclp_sfp_control_path_idle(void)
{
    return g_command_service_request_pending == 0U &&
           g_reply_commit_pending == 0U;
}

void nclp_sfp_control_poll(void)
{
    uint32_t status;

    if (g_sfp_control_ready == 0U) {
        if (refresh_control_readiness() != 0) {
            return;
        }
        /* A late identity recovery must not pop retained traffic or clear
         * counters/errors. Only retire the stale accepted-edge latch; normal
         * mailbox servicing begins on the following poll. */
        mailbox_write(SFP_MAILBOX_REG_COMMAND,
                      SFP_MAILBOX_COMMAND_CLEAR_TX_AURORA_ACCEPTED);
        return;
    }
    acknowledge_mailbox_events();
    commit_pending_reply();
    discard_uncommittable_reply_after_link_loss();

    status = mailbox_read(SFP_MAILBOX_REG_STATUS);
    if (g_command_ingress_enabled == 0U) {
        /* Drain unowned traffic so stale commands cannot execute after a later
         * transport takeover. A completed reply was committed above whenever
         * queue space was available; link loss discarded only a reply that
         * could not be committed. */
        if ((status & SFP_MAILBOX_STATUS_RX_AVAILABLE) != 0U) {
            mailbox_write(SFP_MAILBOX_REG_COMMAND,
                          SFP_MAILBOX_COMMAND_RX_POP);
        }
        return;
    }
    if (g_command_service_request_pending != 0U || g_reply_commit_pending != 0U ||
        nclp_command_service_pending() != 0) {
        return;
    }
    if ((status & SFP_MAILBOX_STATUS_RX_AVAILABLE) != 0U) {
        dispatch_front_rx_command();
    }
}
