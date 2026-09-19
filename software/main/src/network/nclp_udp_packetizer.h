#ifndef NCLP_UDP_PACKETIZER_H
#define NCLP_UDP_PACKETIZER_H

#include <stddef.h>
#include <stdint.h>
#include "../../../common/nclp_wire.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * UDP sample datagram (all header fields are little-endian):
 *
 *   0x00  u32 magic
 *   0x04  u8  version
 *   0x05  u8  header_bytes
 *   0x06  u16 flags (AUX/VDD plus one sample-rate code)
 *   0x08  u32 session_id
 *   0x0c  u32 sequence
 *   0x10  u64 first_timestamp
 *   0x18  u16 logical_stream_mask
 *   0x1a  u16 frame_bytes (always 1134)
 *   0x1c  u16 frame_count (always 1)
 *   0x1e  u16 layout_id
 *   0x20  one normalized 16-logical-slot Intan frame
 *
 * NCLP_UDP_MAGIC is chosen so the four serialized bytes spell "NCLP".
 * Every emitted datagram is exactly 1166 bytes. The Intan recording frame
 * remains compressed to the enabled logical streams. The packetizer expands
 * its 35 row-major stream groups into 16 logical slots, placing active words
 * according to logical_stream_mask and zero-filling inactive slots. The fixed
 * 12-byte magic/timestamp prefix and final 2-byte TTL word retain their Intan
 * byte order.
 */

typedef enum {
    NCLP_UDP_MODE_AUX = NCLP_UDP_FLAG_MODE_AUX,
    NCLP_UDP_MODE_VDD = NCLP_UDP_FLAG_MODE_VDD
} nclp_udp_sample_mode_t;

typedef enum {
    NCLP_UDP_PACKETIZER_OK = 0,
    NCLP_UDP_PACKETIZER_BAD_ARGUMENT = -1,
    NCLP_UDP_PACKETIZER_BAD_CONFIG = -2,
    NCLP_UDP_PACKETIZER_SEND_FAILED = -3
} nclp_udp_packetizer_result_t;

typedef struct {
    uint32_t session_id;
    uint32_t sample_rate_hz;
    uint16_t logical_stream_mask;
    uint16_t layout_id;
    nclp_udp_sample_mode_t sample_mode;
} nclp_udp_packetizer_config_t;

/*
 * The callback must synchronously consume or copy the datagram. The storage is
 * owned by the packetizer and may be reused as soon as the callback returns.
 * Return zero on success and nonzero to leave the datagram queued for retry.
 */
typedef int (*nclp_udp_emit_fn)(void *context,
                                const uint8_t *datagram,
                                uint16_t datagram_bytes);

typedef struct {
    uint64_t discarded_bytes;
    uint64_t accepted_frames;
    uint64_t emitted_packets;
    uint64_t emitted_frames;
    uint32_t resync_count;
    uint32_t send_failures;
} nclp_udp_packetizer_stats_t;

/*
 * Public only to permit static allocation on the standalone target. Callers
 * must treat every member as private and initialize with init().
 */
typedef struct {
    nclp_udp_packetizer_config_t config;
    nclp_udp_packetizer_stats_t stats;
    uint32_t sequence;
    uint16_t input_frame_bytes;
    uint16_t input_stream_count;
    uint16_t sample_rate_flag;
    uint16_t input_frame_bytes_collected;
    uint8_t datagram_pending;
    uint8_t magic_prefix_bytes_matched;
    uint8_t resynchronizing_after_discard;
    uint8_t timestamp_initialized;
    uint32_t last_timestamp_low;
    uint64_t timestamp_epoch;
    uint64_t datagram_timestamp;
    uint8_t input_frame[NCLP_PL_MAX_FRAME_BYTES];
    uint8_t datagram[NCLP_UDP_FIXED_DATAGRAM_BYTES];
} nclp_udp_packetizer_t;

/* Starts a new session and resets sequence, carry, timestamp, and statistics. */
int nclp_udp_packetizer_init(nclp_udp_packetizer_t *packetizer,
                             const nclp_udp_packetizer_config_t *config);

/* Returns the compressed Intan frame size, 14 + 70 * popcount(mask), or zero. */
uint16_t nclp_udp_packetizer_input_frame_bytes(
    uint16_t logical_stream_mask);

/*
 * Ingests an arbitrary DDR byte chunk. Each complete, magic-aligned compressed
 * input frame emits one fixed 1166-byte datagram. A partial input frame is held
 * across calls and is never emitted by flush().
 *
 * consumed receives the number of input bytes committed to internal state. If
 * emission fails, retry later with data + *consumed and bytes - *consumed.
 */
int nclp_udp_packetizer_feed(nclp_udp_packetizer_t *packetizer,
                             const void *data,
                             size_t bytes,
                             nclp_udp_emit_fn emit,
                             void *emit_context,
                             size_t *consumed);

/* Retries a queued complete datagram; it never emits a partial input frame. */
int nclp_udp_packetizer_flush(nclp_udp_packetizer_t *packetizer,
                              nclp_udp_emit_fn emit,
                              void *emit_context);

#ifdef __cplusplus
}
#endif

#endif
