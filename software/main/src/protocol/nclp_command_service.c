#include "nclp_command_service.h"
#include "../ipc/nclp_shared.h"
#include "../ipc/result_store_network.h"
#include <string.h>

typedef struct {
    nclp_command_reply_fn send_reply;
    uint32_t token;
    uint8_t reply_ready;
    nclp_main_command_t command;
    nclp_main_reply_t reply;
} nclp_pending_command_t;
static nclp_pending_command_t g_pending_command;

void nclp_command_service_init(void)
{
    memset(&g_pending_command, 0, sizeof(g_pending_command));
}

int nclp_command_service_pending(void)
{
    return g_pending_command.send_reply != NULL;
}

static void make_reply(const nclp_main_command_t *command,
                       nclp_main_reply_t *reply, uint32_t status)
{
    memset(reply, 0, sizeof(*reply));
    reply->magic = NCLP_CMD_MAGIC;
    reply->version = NCLP_CMD_VERSION;
    reply->command = command->command;
    reply->sequence = command->sequence;
    reply->status = status;
}

static void make_progress_reply(const nclp_main_command_t *command,
                                nclp_main_reply_t *reply)
{
    nclp_progress_t progress;
    int result = nclp_result_read_progress(&progress);

    make_reply(command, reply, NCLP_COMMAND_STATUS_OK);
    if (result != NCLP_RESULT_READ_OK) {
        reply->status = NCLP_COMMAND_STATUS_SNAPSHOT_BUSY;
        return;
    }
    reply->data0 = (progress.operation & 0xFFU) |
                   ((progress.state & 0xFU) << 8) |
                   (((progress.current_stream == 0xFFFFFFFFU ? 0xFU :
                      progress.current_stream) & 0xFU) << 12) |
                   (((progress.current_channel == 0xFFFFFFFFU ? 0xFFU :
                      progress.current_channel) & 0xFFU) << 16) |
                   (((progress.current_cap_range == 0xFFFFFFFFU ? 0x3U :
                      progress.current_cap_range) & 0x3U) << 24) |
                   ((progress.status & 0x3FU) << 26);
    reply->data1 = progress.completed_units;
    reply->data2 = progress.total_units;
    reply->data3 = progress.result_id;
    reply->fail_count = progress.fail_count;
}

static void make_ping_reply(const nclp_main_command_t *command,
                            nclp_main_reply_t *reply)
{
    nclp_runtime_snapshot_t runtime;
    int result = nclp_shared_read_runtime(&runtime);

    make_reply(command, reply, NCLP_COMMAND_STATUS_OK);
    if (result == NCLP_SHARED_SNAPSHOT_NOT_READY) {
        reply->status = NCLP_COMMAND_STATUS_NETWORK;
        return;
    }
    if (result != NCLP_SHARED_SNAPSHOT_OK) {
        reply->status = NCLP_COMMAND_STATUS_SNAPSHOT_BUSY;
        return;
    }
    reply->data0 = 0x4F4B554EU;
    reply->data1 = runtime.control_heartbeat;
    reply->data2 = runtime.network_heartbeat;
    reply->data3 = (NCLP_SHARED_VERSION & 0xFFFFU) |
                   ((NCLP_CMD_VERSION & 0xFFFFU) << 16);
}

static void make_status_reply(const nclp_main_command_t *command,
                              nclp_main_reply_t *reply)
{
    nclp_runtime_snapshot_t runtime;
    int result = nclp_shared_read_runtime(&runtime);

    make_reply(command, reply, NCLP_COMMAND_STATUS_OK);
    if (result == NCLP_SHARED_SNAPSHOT_NOT_READY) {
        reply->status = NCLP_COMMAND_STATUS_NETWORK;
        return;
    }
    if (result != NCLP_SHARED_SNAPSHOT_OK) {
        reply->status = NCLP_COMMAND_STATUS_SNAPSHOT_BUSY;
        return;
    }
    reply->data0 = runtime.physical_chip_mask;
    reply->data1 = runtime.stream_mask;
    reply->data2 = runtime.detected_chip_ids;
    reply->data3 = (runtime.control_status_word & 0xFFFFU) |
                   ((runtime.stream_layout_id & 0xFFFFU) << 16);
    reply->fail_count = runtime.stream_error;
}

static void make_config_reply(const nclp_main_command_t *command,
                              nclp_main_reply_t *reply)
{
    nclp_config_snapshot_t config;
    uint32_t section = command->args[0];
    int result;

    make_reply(command, reply, NCLP_COMMAND_STATUS_OK);
    if (section >= NCLP_CONFIG_SECTION_COUNT) {
        reply->status = NCLP_COMMAND_STATUS_ARGUMENT;
        return;
    }
    result = nclp_shared_read_config(&config);
    if (result == NCLP_SHARED_SNAPSHOT_NOT_READY) {
        reply->status = NCLP_COMMAND_STATUS_NETWORK;
        return;
    }
    if (result != NCLP_SHARED_SNAPSHOT_OK) {
        reply->status = NCLP_COMMAND_STATUS_SNAPSHOT_BUSY;
        return;
    }

    if (section == NCLP_CONFIG_SECTION_CORE) {
        reply->data0 = config.generation;
        reply->data1 = config.sample_rate_hz;
        reply->data2 = config.flags;
        reply->data3 = NCLP_CONFIG_SECTION_COUNT;
    } else if (section == NCLP_CONFIG_SECTION_FILTERS) {
        reply->data0 = config.analog_lower_millihz;
        reply->data1 = config.analog_upper_hz;
        reply->data2 = config.dsp_requested_millihz;
        reply->data3 = config.dsp_actual_millihz;
    } else {
        reply->data0 = config.dsp_enable;
        reply->data1 = config.dsp_code;
        reply->data2 = config.ttl_settle_enable;
        reply->data3 = config.ttl_settle_channel;
    }
}

static void make_cancel_reply(const nclp_main_command_t *command,
                              nclp_main_reply_t *reply)
{
    uint32_t token = 0U;
    uint32_t result_id = 0U;
    uint32_t generation = 0U;
    uint32_t ack_token = 0U;
    int result;

    make_reply(command, reply, NCLP_COMMAND_STATUS_OK);
    result = nclp_shared_request_impedance_cancel(
        &token, &result_id, &generation, &ack_token);
    if (result == NCLP_SHARED_CANCEL_NOT_ACTIVE) {
        reply->status = NCLP_COMMAND_STATUS_NOT_ACTIVE;
    } else if (result == NCLP_SHARED_CANCEL_NOT_READY) {
        reply->status = NCLP_COMMAND_STATUS_NETWORK;
    } else if (result != NCLP_SHARED_CANCEL_ACCEPTED) {
        reply->status = NCLP_COMMAND_STATUS_SNAPSHOT_BUSY;
    }
    reply->data0 = token;
    reply->data1 = result_id;
    reply->data2 = generation;
    reply->data3 = ack_token;
}

static void make_result_reply(const nclp_main_command_t *command,
                              nclp_main_reply_t *reply)
{
    nclp_result_header_t header;
    nclp_result_record_t record;
    uint32_t type = command->args[0];
    uint32_t result_id = command->args[1];
    uint32_t index = command->args[2];
    int result;

    make_reply(command, reply, NCLP_COMMAND_STATUS_OK);
    if (command->args[3] != 0U ||
        (type != NCLP_RESULT_TYPE_SCAN &&
         type != NCLP_RESULT_TYPE_INIT &&
         type != NCLP_RESULT_TYPE_IMPEDANCE)) {
        reply->status = NCLP_COMMAND_STATUS_ARGUMENT;
        return;
    }
    result = nclp_result_read(type, result_id, index, &header, &record);
    if (result == NCLP_RESULT_READ_NONE) {
        reply->status = NCLP_COMMAND_STATUS_NO_RESULT;
    } else if (result == NCLP_RESULT_READ_STALE) {
        reply->status = NCLP_COMMAND_STATUS_STALE_RESULT;
    } else if (result == NCLP_RESULT_READ_RANGE) {
        reply->status = NCLP_COMMAND_STATUS_RESULT_RANGE;
    } else if (result == NCLP_RESULT_READ_BUSY) {
        reply->status = NCLP_COMMAND_STATUS_SNAPSHOT_BUSY;
    } else if (result != NCLP_RESULT_READ_OK) {
        reply->status = NCLP_COMMAND_STATUS_ARGUMENT;
    }
    if (reply->status != NCLP_COMMAND_STATUS_OK) {
        return;
    }
    if (index == NCLP_RESULT_INDEX_METADATA) {
        reply->data0 = header.result_id;
        reply->data1 = (header.state & 0xFFU) |
                       ((header.result_type & 0xFFU) << 8) |
                       ((header.operation_status & 0xFFFFU) << 16);
        reply->data2 = header.available_records;
        reply->data3 = (header.total_records & 0xFFFFU) |
                       ((header.record_bytes & 0xFFFFU) << 16);
        reply->fail_count = header.fail_count;
    } else {
        reply->data0 = record.words[0];
        reply->data1 = record.words[1];
        reply->data2 = record.words[2];
        reply->data3 = record.words[3];
        reply->fail_count = header.fail_count;
    }
}

/* Returns one only when a hardware command was accepted for later reply. */
int nclp_command_service_submit(const nclp_main_command_t *command,
                                nclp_command_reply_fn send_reply, uint32_t token)
{
    nclp_main_reply_t reply;
    int submit;

    if (command == NULL || send_reply == NULL) return -1;
    make_reply(command, &reply, NCLP_COMMAND_STATUS_OK);
    if (command->command == NCLP_CMD_PING) {
        make_ping_reply(command, &reply);
        (void)send_reply(token, &reply);
        return 0;
    }
    if (command->command == NCLP_CMD_GET_STATUS) {
        make_status_reply(command, &reply);
        (void)send_reply(token, &reply);
        return 0;
    }
    if (command->command == NCLP_CMD_GET_CONFIG) {
        make_config_reply(command, &reply);
        (void)send_reply(token, &reply);
        return 0;
    }
    if (command->command == NCLP_CMD_GET_PROGRESS) {
        make_progress_reply(command, &reply);
        (void)send_reply(token, &reply);
        return 0;
    }
    if (command->command == NCLP_CMD_GET_RESULT) {
        make_result_reply(command, &reply);
        (void)send_reply(token, &reply);
        return 0;
    }
    if (command->command == NCLP_CMD_CANCEL) {
        make_cancel_reply(command, &reply);
        (void)send_reply(token, &reply);
        return 0;
    }
    if (g_pending_command.send_reply != NULL) {
        reply.status = NCLP_COMMAND_STATUS_BUSY;
        (void)send_reply(token, &reply);
        return 0;
    }

    submit = nclp_shared_try_submit_command(command);
    if (submit != NCLP_SHARED_SUBMIT_ACCEPTED) {
        reply.status = submit == NCLP_SHARED_SUBMIT_BUSY ?
                       NCLP_COMMAND_STATUS_BUSY :
                       NCLP_COMMAND_STATUS_NETWORK;
        (void)send_reply(token, &reply);
        return 0;
    }

    memset(&g_pending_command, 0, sizeof(g_pending_command));
    g_pending_command.send_reply = send_reply;
    g_pending_command.token = token;
    g_pending_command.command = *command;
    return 1;
}

void nclp_command_service_poll(void)
{
    int poll;
    if (g_pending_command.send_reply == NULL) return;
    if (g_pending_command.reply_ready == 0U) {
        poll = nclp_shared_poll_reply(&g_pending_command.reply);
        if (poll == NCLP_SHARED_POLL_PENDING) return;
        if (poll != NCLP_SHARED_POLL_COMPLETE)
            make_reply(&g_pending_command.command, &g_pending_command.reply,
                       NCLP_COMMAND_STATUS_NETWORK);
        g_pending_command.reply_ready = 1U;
    }
    if (g_pending_command.send_reply(g_pending_command.token,
                                     &g_pending_command.reply) == 0)
        memset(&g_pending_command, 0, sizeof(g_pending_command));
}
