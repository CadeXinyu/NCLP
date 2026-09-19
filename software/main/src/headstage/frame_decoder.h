#ifndef NCLP_FRAME_DECODER_H
#define NCLP_FRAME_DECODER_H
#include <stdint.h>

#define SPI_FRAME_MAGIC_WORDS          4U
#define SPI_FRAME_TIMESTAMP_WORDS      2U
#define SPI_FRAME_CHANNEL_SLOTS        35U
#define SPI_FRAME_AUX1_SLOT            32U
#define SPI_FRAME_AUX2_SLOT            33U
#define SPI_FRAME_AUX3_SLOT            34U
#define SPI_FRAME_TTL_WORDS            1U
#define SPI_FRAME_DATA_BASE            (SPI_FRAME_MAGIC_WORDS + SPI_FRAME_TIMESTAMP_WORDS)

typedef struct {
    uint32_t frames;
    uint32_t errors;
    uint32_t first_timestamp;
    uint32_t last_timestamp;
    uint32_t first_bad_frame;
    uint32_t expected_timestamp;
    uint32_t actual_timestamp;
} timestamp_check_result_t;

uint32_t frame_words_for_stream_count(uint32_t stream_count);
uint32_t frame_timestamp32(const uint16_t *frame_words);
void check_timestamp_sequence(const uint32_t *timestamps,
                                     uint32_t count,
                                     timestamp_check_result_t *result);
uint16_t frame_word_for_stream_count(const uint16_t *frame_words,
                                            uint32_t logical_slot,
                                            uint32_t stream_index,
                                            uint32_t stream_count);
uint16_t frame_word_for_stream(const uint16_t *frame_words, uint32_t logical_slot, uint32_t stream);
#endif
