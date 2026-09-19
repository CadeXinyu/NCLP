#include "frame_decoder.h"
#include <stddef.h>

/* Dynamic Intan frame decoder helpers. */

uint32_t frame_words_for_stream_count(uint32_t stream_count)
{
    return SPI_FRAME_DATA_BASE +
           (SPI_FRAME_CHANNEL_SLOTS * stream_count) +
           SPI_FRAME_TTL_WORDS;
}

uint32_t frame_timestamp32(const uint16_t *frame_words)
{
    return ((uint32_t)frame_words[5] << 16) | frame_words[4];
}

void check_timestamp_sequence(const uint32_t *timestamps,
                                     uint32_t count,
                                     timestamp_check_result_t *result)
{
    result->frames = count;
    result->errors = 0U;
    result->first_timestamp = (count == 0U) ? 0U : timestamps[0];
    result->last_timestamp = (count == 0U) ? 0U : timestamps[count - 1U];
    result->first_bad_frame = 0U;
    result->expected_timestamp = 0U;
    result->actual_timestamp = 0U;

    for (uint32_t i = 1U; i < count; ++i) {
        uint32_t expected = timestamps[i - 1U] + 1U;

        if (timestamps[i] != expected) {
            if (result->errors == 0U) {
                result->first_bad_frame = i;
                result->expected_timestamp = expected;
                result->actual_timestamp = timestamps[i];
            }
            result->errors++;
        }
    }
}

uint16_t frame_word_for_stream_count(const uint16_t *frame_words,
                                            uint32_t logical_slot,
                                            uint32_t stream_index,
                                            uint32_t stream_count)
{
    if (frame_words == NULL || stream_count == 0U || stream_count > 16U ||
        stream_index >= stream_count || logical_slot >= SPI_FRAME_CHANNEL_SLOTS) {
        return 0U;
    }
    uint32_t word_index = SPI_FRAME_DATA_BASE +
                          (logical_slot * stream_count) +
                          stream_index;
    return frame_words[word_index];
}

uint16_t frame_word_for_stream(const uint16_t *frame_words, uint32_t logical_slot, uint32_t stream)
{
    return frame_word_for_stream_count(frame_words, logical_slot, stream, 16U);
}
