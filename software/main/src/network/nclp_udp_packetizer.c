#include "nclp_udp_packetizer.h"

#include <string.h>

/* Intan serializes 0xc691199927021942 least-significant 16-bit word first. */
static const uint8_t nclp_intan_frame_magic[8] = {
    0x42U, 0x19U, 0x02U, 0x27U, 0x99U, 0x19U, 0x91U, 0xc6U
};

#define NCLP_INTAN_FRAME_PREFIX_BYTES 12U
#define NCLP_INTAN_FRAME_STREAM_ROWS  35U
#define NCLP_INTAN_FRAME_TAIL_BYTES   2U
#define NCLP_INTAN_SAMPLE_WORD_BYTES  2U

static void put_le16(uint8_t *destination, uint16_t value)
{
    destination[0] = (uint8_t)value;
    destination[1] = (uint8_t)(value >> 8);
}

static void put_le32(uint8_t *destination, uint32_t value)
{
    destination[0] = (uint8_t)value;
    destination[1] = (uint8_t)(value >> 8);
    destination[2] = (uint8_t)(value >> 16);
    destination[3] = (uint8_t)(value >> 24);
}

static void put_le64(uint8_t *destination, uint64_t value)
{
    put_le32(destination, (uint32_t)value);
    put_le32(destination + 4U, (uint32_t)(value >> 32));
}

static uint32_t get_le32(const uint8_t *source)
{
    return ((uint32_t)source[0]) |
           ((uint32_t)source[1] << 8) |
           ((uint32_t)source[2] << 16) |
           ((uint32_t)source[3] << 24);
}

static uint16_t stream_count(uint16_t mask)
{
    uint16_t count = 0U;

    while (mask != 0U) {
        count = (uint16_t)(count + (mask & 1U));
        mask = (uint16_t)(mask >> 1);
    }
    return count;
}

static uint16_t sample_rate_flag(uint32_t sample_rate_hz)
{
    switch (sample_rate_hz) {
    case 5000U:
        return NCLP_UDP_FLAG_RATE_5KHZ;
    case 10000U:
        return NCLP_UDP_FLAG_RATE_10KHZ;
    case 15000U:
        return NCLP_UDP_FLAG_RATE_15KHZ;
    case 20000U:
        return NCLP_UDP_FLAG_RATE_20KHZ;
    case 25000U:
        return NCLP_UDP_FLAG_RATE_25KHZ;
    case 30000U:
        return NCLP_UDP_FLAG_RATE_30KHZ;
    default:
        return 0U;
    }
}

uint16_t nclp_udp_packetizer_input_frame_bytes(
    uint16_t logical_stream_mask)
{
    uint16_t count = stream_count(logical_stream_mask);

    if (count == 0U) {
        return 0U;
    }
    return (uint16_t)(NCLP_PL_FRAME_BASE_BYTES +
                      (NCLP_PL_FRAME_STREAM_BYTES * count));
}

static uint64_t extend_timestamp(nclp_udp_packetizer_t *packetizer,
                                 uint32_t timestamp)
{
    if (packetizer->timestamp_initialized != 0U &&
        timestamp < packetizer->last_timestamp_low &&
        (packetizer->last_timestamp_low - timestamp) > 0x80000000UL) {
        packetizer->timestamp_epoch += (UINT64_C(1) << 32);
    }

    packetizer->timestamp_initialized = 1U;
    packetizer->last_timestamp_low = timestamp;
    return packetizer->timestamp_epoch + timestamp;
}

static void build_header(nclp_udp_packetizer_t *packetizer)
{
    uint8_t *header = packetizer->datagram;

    put_le32(header + 0U, NCLP_UDP_MAGIC);
    header[4] = NCLP_UDP_VERSION;
    header[5] = NCLP_UDP_HEADER_BYTES;
    put_le16(header + 6U,
             (uint16_t)packetizer->config.sample_mode |
             packetizer->sample_rate_flag);
    put_le32(header + 8U, packetizer->config.session_id);
    put_le32(header + 12U, packetizer->sequence);
    put_le64(header + 16U, packetizer->datagram_timestamp);
    put_le16(header + 24U, packetizer->config.logical_stream_mask);
    put_le16(header + 26U, NCLP_UDP_FIXED_FRAME_BYTES);
    put_le16(header + 28U, 1U);
    put_le16(header + 30U, packetizer->config.layout_id);
}

static int emit_packet(nclp_udp_packetizer_t *packetizer,
                       nclp_udp_emit_fn emit,
                       void *emit_context)
{
    if (packetizer->datagram_pending == 0U) {
        return NCLP_UDP_PACKETIZER_OK;
    }
    if (emit == NULL) {
        return NCLP_UDP_PACKETIZER_BAD_ARGUMENT;
    }

    build_header(packetizer);
    if (emit(emit_context, packetizer->datagram,
             NCLP_UDP_FIXED_DATAGRAM_BYTES) != 0) {
        packetizer->stats.send_failures++;
        return NCLP_UDP_PACKETIZER_SEND_FAILED;
    }

    packetizer->sequence++;
    packetizer->stats.emitted_packets++;
    packetizer->stats.emitted_frames++;
    packetizer->datagram_pending = 0U;
    packetizer->datagram_timestamp = 0U;
    return NCLP_UDP_PACKETIZER_OK;
}

static void normalize_complete_frame(nclp_udp_packetizer_t *packetizer)
{
    uint8_t *output = packetizer->datagram + NCLP_UDP_HEADER_BYTES;
    const uint8_t *input = packetizer->input_frame;
    uint16_t mask = packetizer->config.logical_stream_mask;

    memset(output, 0, NCLP_UDP_FIXED_FRAME_BYTES);
    memcpy(output, input, NCLP_INTAN_FRAME_PREFIX_BYTES);

    for (uint16_t row = 0U; row < NCLP_INTAN_FRAME_STREAM_ROWS; ++row) {
        const uint8_t *source_row = input + NCLP_INTAN_FRAME_PREFIX_BYTES +
            (row * packetizer->input_stream_count *
             NCLP_INTAN_SAMPLE_WORD_BYTES);
        uint8_t *destination_row = output + NCLP_INTAN_FRAME_PREFIX_BYTES +
            (row * NCLP_PL_MAX_LOGICAL_STREAMS *
             NCLP_INTAN_SAMPLE_WORD_BYTES);
        uint16_t source_stream = 0U;

        for (uint16_t logical_slot = 0U;
             logical_slot < NCLP_PL_MAX_LOGICAL_STREAMS;
             ++logical_slot) {
            if ((mask & (uint16_t)(1U << logical_slot)) != 0U) {
                memcpy(destination_row +
                           (logical_slot * NCLP_INTAN_SAMPLE_WORD_BYTES),
                       source_row +
                           (source_stream * NCLP_INTAN_SAMPLE_WORD_BYTES),
                       NCLP_INTAN_SAMPLE_WORD_BYTES);
                source_stream++;
            }
        }
    }

    memcpy(output + NCLP_UDP_FIXED_FRAME_BYTES - NCLP_INTAN_FRAME_TAIL_BYTES,
           input + packetizer->input_frame_bytes -
               NCLP_INTAN_FRAME_TAIL_BYTES,
           NCLP_INTAN_FRAME_TAIL_BYTES);
}

static int append_complete_frame(nclp_udp_packetizer_t *packetizer,
                                 nclp_udp_emit_fn emit,
                                 void *emit_context)
{
    uint64_t timestamp;

    timestamp = extend_timestamp(packetizer,
                                 get_le32(packetizer->input_frame + 8U));
    packetizer->datagram_timestamp = timestamp;
    normalize_complete_frame(packetizer);
    packetizer->datagram_pending = 1U;
    packetizer->stats.accepted_frames++;
    return emit_packet(packetizer, emit, emit_context);
}

int nclp_udp_packetizer_init(nclp_udp_packetizer_t *packetizer,
                             const nclp_udp_packetizer_config_t *config)
{
    uint16_t input_frame_bytes;

    if (packetizer == NULL || config == NULL) {
        return NCLP_UDP_PACKETIZER_BAD_ARGUMENT;
    }
    if (config->logical_stream_mask == 0U ||
        sample_rate_flag(config->sample_rate_hz) == 0U ||
        (config->sample_mode != NCLP_UDP_MODE_AUX &&
         config->sample_mode != NCLP_UDP_MODE_VDD)) {
        return NCLP_UDP_PACKETIZER_BAD_CONFIG;
    }

    input_frame_bytes = nclp_udp_packetizer_input_frame_bytes(
        config->logical_stream_mask);

    memset(packetizer, 0, sizeof(*packetizer));
    packetizer->config = *config;
    packetizer->input_frame_bytes = input_frame_bytes;
    packetizer->input_stream_count = stream_count(
        config->logical_stream_mask);
    packetizer->sample_rate_flag = sample_rate_flag(config->sample_rate_hz);
    return NCLP_UDP_PACKETIZER_OK;
}

static void seek_magic_byte(nclp_udp_packetizer_t *packetizer, uint8_t byte)
{
    if (byte == nclp_intan_frame_magic[
                    packetizer->magic_prefix_bytes_matched]) {
        packetizer->magic_prefix_bytes_matched++;
        if (packetizer->magic_prefix_bytes_matched ==
            sizeof(nclp_intan_frame_magic)) {
            memcpy(packetizer->input_frame, nclp_intan_frame_magic,
                   sizeof(nclp_intan_frame_magic));
            packetizer->input_frame_bytes_collected =
                (uint16_t)sizeof(nclp_intan_frame_magic);
            packetizer->magic_prefix_bytes_matched = 0U;
            if (packetizer->resynchronizing_after_discard != 0U) {
                packetizer->stats.resync_count++;
                packetizer->resynchronizing_after_discard = 0U;
            }
        }
        return;
    }

    packetizer->stats.discarded_bytes +=
        packetizer->magic_prefix_bytes_matched;
    if (byte == nclp_intan_frame_magic[0]) {
        packetizer->magic_prefix_bytes_matched = 1U;
    } else {
        packetizer->stats.discarded_bytes++;
        packetizer->magic_prefix_bytes_matched = 0U;
    }
    packetizer->resynchronizing_after_discard = 1U;
}

int nclp_udp_packetizer_feed(nclp_udp_packetizer_t *packetizer,
                             const void *data,
                             size_t bytes,
                             nclp_udp_emit_fn emit,
                             void *emit_context,
                             size_t *consumed)
{
    const uint8_t *input = (const uint8_t *)data;
    size_t position = 0U;
    int result;

    if (consumed != NULL) {
        *consumed = 0U;
    }
    if (packetizer == NULL || emit == NULL ||
        (data == NULL && bytes != 0U)) {
        return NCLP_UDP_PACKETIZER_BAD_ARGUMENT;
    }

    if (packetizer->datagram_pending != 0U) {
        result = emit_packet(packetizer, emit, emit_context);
        if (result != NCLP_UDP_PACKETIZER_OK) {
            return result;
        }
    }

    while (position < bytes) {
        if (packetizer->input_frame_bytes_collected == 0U) {
            seek_magic_byte(packetizer, input[position]);
            position++;
        } else {
            size_t needed = packetizer->input_frame_bytes -
                            packetizer->input_frame_bytes_collected;
            size_t available = bytes - position;
            size_t copy_bytes = (available < needed) ? available : needed;

            memcpy(packetizer->input_frame +
                       packetizer->input_frame_bytes_collected,
                   input + position, copy_bytes);
            packetizer->input_frame_bytes_collected =
                (uint16_t)(packetizer->input_frame_bytes_collected +
                           copy_bytes);
            position += copy_bytes;

            if (packetizer->input_frame_bytes_collected ==
                packetizer->input_frame_bytes) {
                packetizer->input_frame_bytes_collected = 0U;
                result = append_complete_frame(packetizer, emit, emit_context);
                if (result != NCLP_UDP_PACKETIZER_OK) {
                    if (consumed != NULL) {
                        *consumed = position;
                    }
                    return result;
                }
            }
        }
    }

    if (consumed != NULL) {
        *consumed = position;
    }
    return NCLP_UDP_PACKETIZER_OK;
}

int nclp_udp_packetizer_flush(nclp_udp_packetizer_t *packetizer,
                              nclp_udp_emit_fn emit,
                              void *emit_context)
{
    if (packetizer == NULL || emit == NULL) {
        return NCLP_UDP_PACKETIZER_BAD_ARGUMENT;
    }
    return emit_packet(packetizer, emit, emit_context);
}
