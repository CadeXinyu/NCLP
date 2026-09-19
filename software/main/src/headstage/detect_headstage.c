#include "headstage_internal.h"

nclp_headstage_context_t nclp_headstage = {
    .best_lane_phases_a = 0U,
    .best_lane_phases_b = 0U,
    .scan_capture_debug_printed = 0U,
    .scan_progress_base = 0U,
};

const char * const nclp_stream_names[SPI_SCAN_STREAMS] = {"A1", "A2", "B1", "B2", "C1", "C2", "D1", "D2"};
#include "phase_selection.h"

static uint16_t scan_aux3_command(uint32_t index)
{
    static const uint8_t read_regs[] = {
        40, 41, 42, 43, 44, 63, 62, 63, 63
    };

    if (index >= sizeof(read_regs) / sizeof(read_regs[0])) {
        return rhd_read_reg_cmd(63U);
    }

    return rhd_read_reg_cmd(read_regs[index]);
}

int program_phase_scan_aux_bank(uint32_t reg59_only)
{
    xil_printf("\r\n[3] Write %s phase-scan commands to Aux3 RAM bank 0\r\n",
               reg59_only != 0U ? "RHD2164 Reg59" : "dummy-mode identity");

    stop_spi_safely();

    /* Scan mode selects bank 0 on every connector.  INIT_DUMMY forces the
     * channel-32/33 Aux1/Aux2 slots to READ(63), so only Aux3 RAM is consumed.
     */
    for (uint32_t index = 0U; index <= AUX_SCAN_END_INDEX; ++index) {
        uint16_t scan = reg59_only != 0U ? rhd_read_reg_cmd(59U) :
                                           scan_aux3_command(index);

        intan_aux_command_write(INTAN_AUX3_WINDOW_OFFSET, AUX_SCAN_BANK,
                                index, scan);
    }

    intan_register_write(INTAN_REG_AUX1_BANK_SELECT,
                         AUX_BANK_SELECT_SCAN_VALUE);
    intan_register_write(INTAN_REG_AUX2_BANK_SELECT,
                         AUX_BANK_SELECT_SCAN_VALUE);
    intan_register_write(INTAN_REG_AUX3_BANK_SELECT,
                         AUX_BANK_SELECT_SCAN_VALUE);
    intan_register_write(INTAN_REG_AUX1_END_INDEX, AUX_SCAN_END_INDEX);
    intan_register_write(INTAN_REG_AUX2_END_INDEX, AUX_SCAN_END_INDEX);
    intan_register_write(INTAN_REG_AUX3_END_INDEX, AUX_SCAN_END_INDEX);
    intan_register_write(INTAN_REG_AUX1_LOOP_INDEX, AUX_SCAN_LOOP_INDEX);
    intan_register_write(INTAN_REG_AUX2_LOOP_INDEX, AUX_SCAN_LOOP_INDEX);
    intan_register_write(INTAN_REG_AUX3_LOOP_INDEX, AUX_SCAN_LOOP_INDEX);

    if (reg59_only != 0U) {
        xil_printf("  PASS Reg59 table written: %lu repeated READ(59) replies per phase\r\n",
                   (unsigned long)SPI_SCAN_CAPTURE_FRAMES);
    } else {
        xil_printf("  PASS identity table written: INTAN + chip-ID + num-amps + dummy\r\n");
    }
    DEBUG_PRINT("  INFO init dummy mode sends READ63 on channels 0..33; only channel 34 sends Aux3\r\n");
    return 0;
}

static int configure_scan_trial(uint32_t packed_lane_phases_a,
                                uint32_t packed_lane_phases_b)
{
    intan_register_write(INTAN_REG_LOGICAL_STREAM_ENABLE, 0U);
    if (write_lane_phases_verified(packed_lane_phases_a,
                                   packed_lane_phases_b) != 0) {
        return -1;
    }
    intan_register_write(INTAN_REG_AUX1_BANK_SELECT,
                         AUX_BANK_SELECT_SCAN_VALUE);
    intan_register_write(INTAN_REG_AUX2_BANK_SELECT,
                         AUX_BANK_SELECT_SCAN_VALUE);
    intan_register_write(INTAN_REG_AUX3_BANK_SELECT,
                         AUX_BANK_SELECT_SCAN_VALUE);
    intan_register_write(INTAN_REG_AUX1_END_INDEX, AUX_SCAN_END_INDEX);
    intan_register_write(INTAN_REG_AUX2_END_INDEX, AUX_SCAN_END_INDEX);
    intan_register_write(INTAN_REG_AUX3_END_INDEX, AUX_SCAN_END_INDEX);
    intan_register_write(INTAN_REG_AUX1_LOOP_INDEX, AUX_SCAN_LOOP_INDEX);
    intan_register_write(INTAN_REG_AUX2_LOOP_INDEX, AUX_SCAN_LOOP_INDEX);
    intan_register_write(INTAN_REG_AUX3_LOOP_INDEX, AUX_SCAN_LOOP_INDEX);
    intan_register_write(INTAN_REG_FINITE_FRAME_COUNT, SPI_SCAN_TIMESTEPS);
    intan_register_write(INTAN_REG_ACQUISITION_CONFIG,
                         INTAN_ACQUISITION_CONFIG_INIT_DUMMY);
    intan_register_write(INTAN_REG_LOGICAL_STREAM_ENABLE,
                         SPI_SCAN_STREAM_MASK);
    return start_intan_capture();
}

static uint32_t packed_lane_phase_set(uint32_t packed, uint32_t lane,
                                      uint32_t phase)
{
    uint32_t shift = 4U * (lane & 0x7U);

    return (packed & ~(0xFU << shift)) | ((phase & 0xFU) << shift);
}

uint32_t packed_lane_phase_get(uint32_t packed, uint32_t lane)
{
    return (packed >> (4U * (lane & 0x7U))) & 0xFU;
}

static int scan_expected_low_byte(uint32_t command_index, uint8_t *expected)
{
    switch (command_index) {
    case 0U:
        *expected = (uint8_t)'I';
        return 1;
    case 1U:
        *expected = (uint8_t)'N';
        return 1;
    case 2U:
        *expected = (uint8_t)'T';
        return 1;
    case 3U:
        *expected = (uint8_t)'A';
        return 1;
    case 4U:
        *expected = (uint8_t)'N';
        return 1;
    default:
        return 0;
    }
}

static uint32_t score_identity_reply(uint16_t word, uint32_t command_index)
{
    uint8_t low = (uint8_t)(word & 0x00FFU);
    uint8_t expected = 0U;

    if (scan_expected_low_byte(command_index, &expected)) {
        return (low == expected) ? 8U : 0U;
    }

    if (command_index == 5U) {
        return (low == 1U || low == 2U || low == 4U) ? 8U : 0U;
    }

    if (command_index == 6U) {
        return (low == 16U || low == 32U || low == 64U) ? 8U : 0U;
    }

    return 0U;
}

static uint8_t chip_id_from_aux_word(uint16_t word)
{
    uint8_t low = (uint8_t)(word & 0x00FFU);

    return (low == 1U || low == 2U || low == 4U) ? low : 0U;
}

static const char *chip_type_name(uint8_t chip_id)
{
    switch (chip_id) {
    case 1U:
        return "RHD2132";
    case 2U:
        return "RHD2216";
    case 4U:
        return "RHD2164";
    default:
        return "unknown";
    }
}

uint32_t chip_channel_count(uint8_t chip_id)
{
    switch (chip_id) {
    case 1U:
        return 32U;
    case 2U:
        return 16U;
    case 4U:
        return 64U;
    default:
        return 0U;
    }
}

uint8_t selected_stream_mask(void)
{
    uint8_t mask = 0U;

    for (uint32_t stream = 0U; stream < SPI_SCAN_STREAMS; ++stream) {
        uint8_t chip_id = nclp_headstage.best_stream_chip_id[stream];
        uint32_t expected_amps = chip_channel_count(chip_id);
        uint32_t supported_lane =
            (chip_id != 4U) || (nclp_headstage.best_stream_reg59[stream] == 53U);

        if ((nclp_headstage.best_stream_score[stream] >= SPI_SCAN_PASS_SCORE) &&
            (expected_amps != 0U) &&
            (nclp_headstage.best_stream_num_amps[stream] == expected_amps) &&
            supported_lane) {
            mask |= (uint8_t)(1U << stream);
        }
    }

    return mask;
}

uint32_t first_selected_stream(void)
{
    for (uint32_t stream = 0U; stream < SPI_SCAN_STREAMS; ++stream) {
        uint8_t chip_id = nclp_headstage.best_stream_chip_id[stream];
        uint32_t expected_amps = chip_channel_count(chip_id);
        uint32_t supported_lane =
            (chip_id != 4U) || (nclp_headstage.best_stream_reg59[stream] == 53U);

        if ((nclp_headstage.best_stream_score[stream] >= SPI_SCAN_PASS_SCORE) &&
            (expected_amps != 0U) &&
            (nclp_headstage.best_stream_num_amps[stream] == expected_amps) &&
            supported_lane) {
            return stream;
        }
    }

    return 0U;
}

uint32_t primary_stream_mask(uint32_t stream)
{
    uint32_t primary;

    stream &= 0x7U;
    primary = 2U * stream;
    if (nclp_headstage.best_stream_chip_id[stream] == 4U) {
        /* Each physical lane has adjacent logical stream IDs.  RHD2164 uses
         * independently phased MISO A (2*lane) and MISO B (2*lane+1).
         */
        return (1U << primary) | (1U << (primary + 1U));
    }
    return 1U << primary;
}

uint32_t selected_logical_stream_mask(void)
{
    uint32_t logical_mask = 0U;
    uint32_t physical_mask = selected_stream_mask();

    for (uint32_t stream = 0U; stream < SPI_SCAN_STREAMS; ++stream) {
        if ((physical_mask & (1U << stream)) != 0U) {
            logical_mask |= primary_stream_mask(stream);
        }
    }
    return logical_mask;
}

static void print_detection_summary(void)
{
    uint32_t chip_count = 0U;
    uint32_t channel_count = 0U;

    xil_printf("\r\n[5] Detection summary\r\n");
    for (uint32_t stream = 0U; stream < SPI_SCAN_STREAMS; ++stream) {
        uint8_t chip_id = nclp_headstage.best_stream_chip_id[stream];
        uint8_t observed_amps = nclp_headstage.best_stream_num_amps[stream];
        uint32_t expected_amps = chip_channel_count(chip_id);
        uint32_t supported_lane =
                            (chip_id != 4U) || (nclp_headstage.best_stream_reg59[stream] == 53U);
        uint32_t detected = (nclp_headstage.best_stream_score[stream] >= SPI_SCAN_PASS_SCORE) &&
                            (expected_amps != 0U) &&
                            (observed_amps == expected_amps) &&
                            supported_lane;
        uint32_t mismatch = (nclp_headstage.best_stream_score[stream] >= SPI_SCAN_PASS_SCORE) &&
                            (expected_amps != 0U) &&
                            (observed_amps != 0U) &&
                            (observed_amps != expected_amps);

        if (detected) {
            uint32_t phase_b = packed_lane_phase_get(nclp_headstage.best_lane_phases_b,
                                                     stream);

            chip_count++;
            channel_count += expected_amps;
            xil_printf("  %s: DETECTED phaseA=%lu phaseB=%lu score=%lu chip=%s amps=%lu reg59=%lu\r\n",
                       nclp_stream_names[stream],
                       (unsigned long)nclp_headstage.best_stream_phase[stream],
                       (unsigned long)phase_b,
                       (unsigned long)nclp_headstage.best_stream_score[stream],
                       chip_type_name(chip_id),
                       (unsigned long)observed_amps,
                       (unsigned long)nclp_headstage.best_stream_reg59[stream]);
        } else if (NCLP_SCAN_DEBUG != 0U) {
            xil_printf("  %s: missing phase=%lu score=%lu chip=%s amps=%lu\r\n",
                       nclp_stream_names[stream],
                       (unsigned long)nclp_headstage.best_stream_phase[stream],
                       (unsigned long)nclp_headstage.best_stream_score[stream],
                       chip_type_name(chip_id),
                       (unsigned long)observed_amps);
        }

        if (mismatch) {
            xil_printf("  ERROR %s chip=%s expects %lu amps but ROM62 returned %lu\r\n",
                       nclp_stream_names[stream],
                       chip_type_name(chip_id),
                       (unsigned long)expected_amps,
                       (unsigned long)observed_amps);
            nclp_diagnostics.fail_count++;
        }
    }

    xil_printf("  TOTAL detected chips=%lu channels=%lu\r\n",
               (unsigned long)chip_count,
               (unsigned long)channel_count);
}

static uint32_t score_identity_frame(const uint16_t *frame_words,
                                     uint32_t frame_index,
                                     uint32_t stream)
{
    uint32_t command_count = AUX_SCAN_END_INDEX + 1U;
    uint32_t command_index = frame_index % command_count;
    uint16_t aux3_word;

    aux3_word = frame_word_for_stream(frame_words, SPI_FRAME_AUX3_SLOT, stream);

    return score_identity_reply(aux3_word, command_index);
}

static void debug_print_scan_frame(const uint16_t *frame_words, uint32_t frame_index)
{
    uint32_t command_count = AUX_SCAN_END_INDEX + 1U;
    uint32_t command_index;
    uint32_t ts;

    command_index = frame_index % command_count;

    ts = frame_timestamp32(frame_words);
    xil_printf("      frame=%lu ts=%lu cmd_idx=%lu aux3:",
               (unsigned long)frame_index,
               (unsigned long)ts,
               (unsigned long)command_index);

    for (uint32_t stream = 0U; stream < SPI_SCAN_STREAMS; ++stream) {
        uint16_t aux3_word = frame_word_for_stream(frame_words, SPI_FRAME_AUX3_SLOT,
                                                   2U * stream);
        xil_printf(" %s=0x%04lx",
                   nclp_stream_names[stream],
                   (unsigned long)aux3_word);
    }

    xil_printf(" expected=");
    if (command_index <= 4U) {
        static const char expected_chars[5] = {'I', 'N', 'T', 'A', 'N'};
        xil_printf("'%c'", expected_chars[command_index]);
    } else if (command_index == 5U) {
        xil_printf("chip_id");
    } else if (command_index == 6U) {
        xil_printf("num_amps");
    } else {
        xil_printf("dummy_read63");
    }

    xil_printf("\r\n");
}

static void print_phase_miso_values(uint32_t phase,
                                    const uint16_t aux3_words[SPI_SCAN_STREAMS][AUX_SCAN_END_INDEX + 1U],
                                    const uint32_t stream_scores[SPI_SCAN_STREAMS])
{
    xil_printf("      MISO identity_phase=%lu all8:", (unsigned long)phase);
    for (uint32_t stream = 0U; stream < SPI_SCAN_STREAMS; ++stream) {
        xil_printf(" %s score=%lu aux3=",
                   nclp_stream_names[stream],
                   (unsigned long)stream_scores[stream]);
        for (uint32_t cmd = 0U; cmd <= 7U; ++cmd) {
            xil_printf("%04lx", (unsigned long)aux3_words[stream][cmd]);
            if (cmd != 7U) {
                xil_printf(",");
            }
        }
    }
    xil_printf("\r\n");
}

static int capture_and_score_identity(uint32_t phase,
                                      uint32_t stream_scores[SPI_SCAN_STREAMS],
                                      uint8_t chip_ids[SPI_SCAN_STREAMS],
                                      uint8_t num_amps[SPI_SCAN_STREAMS])
{
    uint16_t word_buffer[SPI_FRAME_BUFFER_WORDS];
    uint32_t timestamps[SPI_SCAN_CAPTURE_FRAMES];
    timestamp_check_result_t ts_check;
    uint32_t frames_seen = 0U;
    uint32_t best_score = 0U;
    uint32_t sync_losses = 0U;
    uint32_t best_order = 0U;
    uint32_t best_offset16 = 0U;
    uint32_t best_matches = 0U;
    uint32_t frame_words16 = SPI_FRAME_WORDS;
    uint32_t words32_count;
    uint16_t aux3_words[SPI_SCAN_STREAMS][AUX_SCAN_END_INDEX + 1U];

    for (uint32_t i = 0U; i < SPI_SCAN_STREAMS; ++i) {
        stream_scores[i] = 0U;
        chip_ids[i] = 0U;
        num_amps[i] = 0U;
        for (uint32_t cmd = 0U; cmd <= AUX_SCAN_END_INDEX; ++cmd) {
            aux3_words[i][cmd] = 0xFFFFU;
        }
    }

    if (ddr_capture_collect(SPI_CAPTURE_BLOCK_BYTES) != 0) {
        return -1;
    }

    words32_count = nclp_capture.capture_valid_words32;
    best_matches = find_best_capture_order_and_offset(SPI_SCAN_CAPTURE_STREAMS,
                                                       &best_order, &best_offset16);

    if (NCLP_SCAN_DEBUG != 0U) {
        xil_printf("      INFO phase=%lu bytes=%lu matches=%lu order=%lu ddr=0x%08lx write_offset=%lu\r\n",
                   (unsigned long)phase,
                   (unsigned long)nclp_capture.capture_valid_bytes,
                   (unsigned long)best_matches,
                   (unsigned long)best_order,
                   (unsigned long)reg_read(SPI_DDR_WRITER_BASE, DDRW_REG_STATUS),
                   (unsigned long)reg_read(SPI_DDR_WRITER_BASE, DDRW_REG_CURRENT_WRITE_OFFSET_BYTES));
    }

    if (best_matches == 0U) {
        if (NCLP_SCAN_DEBUG != 0U) {
            print_capture_word_order_debug("phase-scan", words32_count);
        }
        return -1;
    }

    for (uint32_t frame = 0U; frame < SPI_SCAN_CAPTURE_FRAMES; ++frame) {
        uint32_t start16 = best_offset16 + (frame * frame_words16);
        uint32_t command_index;

        if ((start16 + frame_words16) > (words32_count * 2U)) {
            break;
        }
        if (!magic_at_capture(nclp_capture.capture_ring_words, start16, best_order)) {
            sync_losses++;
            continue;
        }

        for (uint32_t w = 0U; w < frame_words16; ++w) {
            word_buffer[w] = get_capture16(nclp_capture.capture_ring_words, start16 + w, best_order);
        }
        timestamps[frames_seen] = frame_timestamp32(word_buffer);

        if (SPI_SCAN_SHOULD_PRINT_FRAME(frame)) {
            debug_print_scan_frame(word_buffer, frame);
        }

        for (uint32_t stream = 0U; stream < SPI_SCAN_STREAMS; ++stream) {
            stream_scores[stream] += score_identity_frame(word_buffer, frame,
                                                           2U * stream);
        }

        command_index = frame % (AUX_SCAN_END_INDEX + 1U);
        if (command_index <= 6U) {
            for (uint32_t stream = 0U; stream < SPI_SCAN_STREAMS; ++stream) {
                uint32_t primary = 2U * stream;
                uint16_t aux3_word = frame_word_for_stream(word_buffer,
                                                            SPI_FRAME_AUX3_SLOT,
                                                            primary);
                uint8_t low = (uint8_t)(aux3_word & 0x00FFU);

                aux3_words[stream][command_index] = aux3_word;
                if (command_index == 5U) {
                    uint8_t chip_id = chip_id_from_aux_word(aux3_word);
                    if (chip_ids[stream] == 0U && chip_id != 0U) {
                        chip_ids[stream] = chip_id;
                    }
                } else if (command_index == 6U) {
                    if (num_amps[stream] == 0U &&
                        (low == 16U || low == 32U || low == 64U)) {
                        num_amps[stream] = low;
                    }
                }
            }
        }

        frames_seen++;
    }

    check_timestamp_sequence(timestamps, frames_seen, &ts_check);
    if (ts_check.errors != 0U) {
        xil_printf("      FAIL timestamp gap phase=%lu frame=%lu expected=%lu actual=%lu errors=%lu\r\n",
                   (unsigned long)phase,
                   (unsigned long)ts_check.first_bad_frame,
                   (unsigned long)ts_check.expected_timestamp,
                   (unsigned long)ts_check.actual_timestamp,
                   (unsigned long)ts_check.errors);
    } else if ((frames_seen != 0U) && (NCLP_SCAN_DEBUG != 0U)) {
        xil_printf("      PASS timestamp check phase=%lu frames=%lu first=%lu last=%lu\r\n",
                   (unsigned long)phase,
                   (unsigned long)ts_check.frames,
                   (unsigned long)ts_check.first_timestamp,
                   (unsigned long)ts_check.last_timestamp);
    }

    for (uint32_t stream = 0U; stream < SPI_SCAN_STREAMS; ++stream) {
        if (stream_scores[stream] > best_score) {
            best_score = stream_scores[stream];
        }
    }

    if (NCLP_DEBUG != 0U || best_score == 0U) {
        print_phase_miso_values(phase, aux3_words, stream_scores);
    }
    if (NCLP_SCAN_DEBUG != 0U) {
        xil_printf("      INFO summary phase=%lu frames=%lu score=%lu\r\n",
                   (unsigned long)phase,
                   (unsigned long)frames_seen,
                   (unsigned long)best_score);
    }
    if (SPI_SCAN_VERBOSE != 0U) {
        xil_printf("      scores:");
        for (uint32_t stream = 0U; stream < SPI_SCAN_STREAMS; ++stream) {
            xil_printf(" %s=%lu", nclp_stream_names[stream], (unsigned long)stream_scores[stream]);
        }
        xil_printf("\r\n");
    }

    if (frames_seen != SPI_SCAN_CAPTURE_FRAMES || sync_losses != 0U ||
        ts_check.errors != 0U) {
        xil_printf("      FAIL identity capture phase=%lu frames=%lu/%lu sync_losses=%lu\r\n",
                   (unsigned long)phase,
                   (unsigned long)frames_seen,
                   (unsigned long)SPI_SCAN_CAPTURE_FRAMES,
                   (unsigned long)sync_losses);
        return -1;
    }

    return 0;
}

static int capture_reg59_marker_counts(uint32_t phase,
                                       uint32_t stream_view,
                                       uint8_t expected_marker,
                                       uint8_t marker_counts[SPI_SCAN_STREAMS])
{
    uint16_t word_buffer[SPI_FRAME_BUFFER_WORDS];
    uint32_t timestamps[SPI_SCAN_CAPTURE_FRAMES] = {0U};
    timestamp_check_result_t ts_check;
    uint32_t frames_seen = 0U;
    uint32_t sync_losses = 0U;
    uint32_t best_order = 0U;
    uint32_t best_offset16 = 0U;
    uint32_t best_matches;
    uint32_t words32_count;

    if (stream_view > 1U) {
        return -1;
    }
    for (uint32_t lane = 0U; lane < SPI_SCAN_STREAMS; ++lane) {
        marker_counts[lane] = 0U;
    }

    if (ddr_capture_collect(SPI_CAPTURE_BLOCK_BYTES) != 0) {
        return -1;
    }

    words32_count = nclp_capture.capture_valid_words32;
    best_matches = find_best_capture_order_and_offset(SPI_SCAN_CAPTURE_STREAMS,
                                                       &best_order,
                                                       &best_offset16);
    if (best_matches == 0U) {
        if (NCLP_SCAN_DEBUG != 0U) {
            print_capture_word_order_debug("Reg59-scan", words32_count);
        }
        return -1;
    }

    for (uint32_t frame = 0U; frame < SPI_SCAN_CAPTURE_FRAMES; ++frame) {
        uint32_t start16 = best_offset16 + (frame * SPI_FRAME_WORDS);

        if ((start16 + SPI_FRAME_WORDS) > (words32_count * 2U)) {
            break;
        }
        if (!magic_at_capture(nclp_capture.capture_ring_words, start16, best_order)) {
            sync_losses++;
            continue;
        }

        for (uint32_t word = 0U; word < SPI_FRAME_WORDS; ++word) {
            word_buffer[word] = get_capture16(nclp_capture.capture_ring_words,
                                              start16 + word,
                                              best_order);
        }
        timestamps[frames_seen] = frame_timestamp32(word_buffer);

        for (uint32_t lane = 0U; lane < SPI_SCAN_STREAMS; ++lane) {
            uint32_t logical_stream = (2U * lane) + stream_view;
            uint16_t reply = frame_word_for_stream(word_buffer,
                                                    SPI_FRAME_AUX3_SLOT,
                                                    logical_stream);

            if (nclp_reg59_marker_matches(reply, expected_marker)) {
                marker_counts[lane]++;
            }
        }
        frames_seen++;
    }

    check_timestamp_sequence(timestamps, frames_seen, &ts_check);
    if (frames_seen != SPI_SCAN_CAPTURE_FRAMES || sync_losses != 0U ||
        ts_check.errors != 0U) {
        xil_printf("      FAIL Reg59 capture view=%c phase=%lu frames=%lu/%lu sync_losses=%lu timestamp_errors=%lu\r\n",
                   stream_view == 0U ? 'A' : 'B',
                   (unsigned long)phase,
                   (unsigned long)frames_seen,
                   (unsigned long)SPI_SCAN_CAPTURE_FRAMES,
                   (unsigned long)sync_losses,
                   (unsigned long)ts_check.errors);
        return -1;
    }

    if (NCLP_SCAN_DEBUG != 0U) {
        xil_printf("      INFO Reg59 view=%c phase=%lu expected=%lu counts:",
                   stream_view == 0U ? 'A' : 'B',
                   (unsigned long)phase,
                   (unsigned long)expected_marker);
        for (uint32_t lane = 0U; lane < SPI_SCAN_STREAMS; ++lane) {
            xil_printf(" %s=%lu", nclp_stream_names[lane],
                       (unsigned long)marker_counts[lane]);
        }
        xil_printf("\r\n");
    }
    return 0;
}

static int scan_reg59_view(uint32_t stream_view,
                           uint8_t expected_marker,
                           uint8_t active_lane_mask,
                           uint32_t fixed_lane_phases_a,
                           uint32_t fixed_lane_phases_b,
                           uint32_t progress_offset,
                           uint16_t accepted_masks[SPI_SCAN_STREAMS])
{
    uint32_t scan_errors = 0U;

    xil_printf("  Scan RHD2164 Reg59 MISO %c, expected marker=%lu\r\n",
               stream_view == 0U ? 'A' : 'B',
               (unsigned long)expected_marker);
    for (uint32_t lane = 0U; lane < SPI_SCAN_STREAMS; ++lane) {
        accepted_masks[lane] = 0U;
    }

    for (uint32_t phase = SPI_SCAN_PHASE_FIRST;
         phase <= SPI_SCAN_PHASE_LAST;
         ++phase) {
        uint8_t marker_counts[SPI_SCAN_STREAMS];
        uint32_t phase_index = phase - SPI_SCAN_PHASE_FIRST;
        uint32_t trial_lane_phases_a = fixed_lane_phases_a;
        uint32_t trial_lane_phases_b = fixed_lane_phases_b;
        uint32_t phase_status = 0U;

        for (uint32_t lane = 0U; lane < SPI_SCAN_STREAMS; ++lane) {
            if ((active_lane_mask & (uint8_t)(1U << lane)) == 0U) {
                continue;
            }
            if (stream_view == 0U) {
                trial_lane_phases_a = packed_lane_phase_set(
                    trial_lane_phases_a, lane, phase);
            } else {
                trial_lane_phases_b = packed_lane_phase_set(
                    trial_lane_phases_b, lane, phase);
            }
        }

        stop_spi_safely();
        if (ddr_capture_prepare(SPI_CAPTURE_BLOCK_BYTES) != 0 ||
            configure_scan_trial(trial_lane_phases_a,
                                 trial_lane_phases_b) != 0 ||
            capture_reg59_marker_counts(phase, stream_view, expected_marker,
                                        marker_counts) != 0) {
            xil_printf("      FAIL Reg59 view=%c phase=%lu setup/capture\r\n",
                       stream_view == 0U ? 'A' : 'B',
                       (unsigned long)phase);
            nclp_diagnostics.fail_count++;
            scan_errors++;
            phase_status = 1U;
        } else {
            for (uint32_t lane = 0U; lane < SPI_SCAN_STREAMS; ++lane) {
                if ((active_lane_mask & (uint8_t)(1U << lane)) != 0U &&
                    marker_counts[lane] == SPI_REG59_REQUIRED_REPLIES) {
                    accepted_masks[lane] |= (uint16_t)(1U << phase_index);
                }
            }
        }
        stop_spi_safely();
        nclp_progress_update(nclp_headstage.scan_progress_base + progress_offset +
                                 phase_index + 1U,
                             0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU,
                             phase_status, nclp_diagnostics.fail_count);
    }

    return scan_errors == 0U ? 0 : -1;
}

static int identity_lane_selection(
    uint32_t lane,
    const uint32_t phase_stream_scores[SPI_SCAN_PHASE_COUNT][SPI_SCAN_STREAMS],
    const uint8_t phase_chip_ids[SPI_SCAN_PHASE_COUNT][SPI_SCAN_STREAMS],
    const uint8_t phase_num_amps[SPI_SCAN_PHASE_COUNT][SPI_SCAN_STREAMS],
    uint16_t *identity_mask,
    nclp_phase_window_t *identity_window,
    uint8_t *chip_id)
{
    static const uint8_t supported_chip_ids[] = {1U, 2U, 4U};
    uint32_t best_length = 0U;
    uint32_t tied = 0U;

    *identity_mask = 0U;
    *chip_id = 0U;
    identity_window->first = 0U;
    identity_window->last = 0U;
    identity_window->middle = 0U;
    identity_window->length = 0U;

    for (uint32_t type = 0U;
         type < sizeof(supported_chip_ids) / sizeof(supported_chip_ids[0]);
         ++type) {
        uint8_t candidate_id = supported_chip_ids[type];
        uint32_t expected_amps = chip_channel_count(candidate_id);
        uint16_t candidate_mask = 0U;
        nclp_phase_window_t candidate_window;

        for (uint32_t phase_index = 0U;
             phase_index < SPI_SCAN_PHASE_COUNT;
             ++phase_index) {
            if (phase_stream_scores[phase_index][lane] >= SPI_SCAN_PASS_SCORE &&
                phase_chip_ids[phase_index][lane] == candidate_id &&
                phase_num_amps[phase_index][lane] == expected_amps) {
                candidate_mask |= (uint16_t)(1U << phase_index);
            }
        }

        if (nclp_phase_longest_window(candidate_mask, &candidate_window) != 0) {
            continue;
        }
        if (candidate_window.length > best_length) {
            best_length = candidate_window.length;
            *identity_mask = candidate_mask;
            *identity_window = candidate_window;
            *chip_id = candidate_id;
            tied = 0U;
        } else if (candidate_window.length == best_length) {
            tied = 1U;
        }
    }

    if (best_length == 0U) {
        return 0;
    }
    if (tied != 0U) {
        xil_printf("  FAIL %s ambiguous chip identity across phase windows\r\n",
                   nclp_stream_names[lane]);
        return -1;
    }
    return 1;
}

int run_phase_scan(void)
{
    uint32_t phase_stream_scores[SPI_SCAN_PHASE_COUNT][SPI_SCAN_STREAMS] = {{0U}};
    uint8_t phase_chip_ids[SPI_SCAN_PHASE_COUNT][SPI_SCAN_STREAMS] = {{0U}};
    uint8_t phase_num_amps[SPI_SCAN_PHASE_COUNT][SPI_SCAN_STREAMS] = {{0U}};
    uint16_t identity_masks[SPI_SCAN_STREAMS] = {0U};
    uint16_t reg59_a_masks[SPI_SCAN_STREAMS] = {0U};
    uint16_t reg59_b_masks[SPI_SCAN_STREAMS] = {0U};
    nclp_phase_window_t identity_windows[SPI_SCAN_STREAMS];
    uint8_t identity_chip_ids[SPI_SCAN_STREAMS] = {0U};
    uint8_t rhd2164_lane_mask = 0U;
    uint8_t rhd2164_a_valid_mask = 0U;
    uint32_t scan_errors = 0U;

    xil_printf("\r\n[4] Scan identity, then independent RHD2164 Reg59 A/B phase windows\r\n");
    DEBUG_PRINT("  INFO identity Aux3 loop: READ(40..44), READ(63), READ(62), READ(63), READ(63)\r\n");
    DEBUG_PRINT("  INFO fixed IDs: MISO A=2*lane, MISO B=2*lane+1; both use absolute phase taps\r\n");

    for (uint32_t stream = 0U; stream < SPI_SCAN_STREAMS; ++stream) {
        nclp_headstage.best_stream_score[stream] = 0U;
        nclp_headstage.best_stream_phase[stream] = SPI_SCAN_PHASE_FIRST;
        nclp_headstage.best_stream_chip_id[stream] = 0U;
        nclp_headstage.best_stream_num_amps[stream] = 0U;
        nclp_headstage.best_stream_reg59[stream] = 0U;
    }

    for (uint32_t phase = SPI_SCAN_PHASE_FIRST; phase <= SPI_SCAN_PHASE_LAST; ++phase) {
        uint32_t phase_index = phase - SPI_SCAN_PHASE_FIRST;
        uint32_t phase_status = 0U;
        uint32_t packed_lane_phases_a = 0U;
        uint32_t packed_lane_phases_b = 0U;

        for (uint32_t lane = 0U; lane < SPI_SCAN_STREAMS; ++lane) {
            packed_lane_phases_a = packed_lane_phase_set(
                packed_lane_phases_a, lane, phase);
            /* Identity is read from MISO A.  Mirror A into B during this pass
             * so non-RHD2164 lanes retain a deterministic unused B setting.
             */
            packed_lane_phases_b = packed_lane_phase_set(
                packed_lane_phases_b, lane, phase);
        }
        if (SPI_SCAN_VERBOSE != 0U) {
            DEBUG_PRINT("\r\n  identity phase=%lu lane_phases_A=0x%08lx lane_phases_B=0x%08lx\r\n",
                        (unsigned long)phase,
                        (unsigned long)packed_lane_phases_a,
                        (unsigned long)packed_lane_phases_b);
        }

        stop_spi_safely();
        if (ddr_capture_prepare(SPI_CAPTURE_BLOCK_BYTES) != 0 ||
            configure_scan_trial(packed_lane_phases_a,
                                 packed_lane_phases_b) != 0) {
            xil_printf("      FAIL phase %lu setup\r\n", (unsigned long)phase);
            nclp_diagnostics.fail_count++;
            scan_errors++;
            nclp_progress_update(nclp_headstage.scan_progress_base + phase_index + 1U,
                                 0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU, 1U,
                                 nclp_diagnostics.fail_count);
            continue;
        }
        if (capture_and_score_identity(
                phase, phase_stream_scores[phase_index],
                phase_chip_ids[phase_index],
                phase_num_amps[phase_index]) != 0) {
            xil_printf("      FAIL identity phase %lu capture\r\n",
                       (unsigned long)phase);
            nclp_diagnostics.fail_count++;
            scan_errors++;
            phase_status = 1U;
        }
        stop_spi_safely();
        nclp_progress_update(nclp_headstage.scan_progress_base + phase_index + 1U,
                             0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU,
                             phase_status, nclp_diagnostics.fail_count);
    }

    nclp_headstage.best_lane_phases_a = 0U;
    nclp_headstage.best_lane_phases_b = 0U;
    for (uint32_t lane = 0U; lane < SPI_SCAN_STREAMS; ++lane) {
        int selection = identity_lane_selection(
            lane, phase_stream_scores, phase_chip_ids, phase_num_amps,
            &identity_masks[lane], &identity_windows[lane],
            &identity_chip_ids[lane]);

        if (selection < 0) {
            nclp_diagnostics.fail_count++;
            scan_errors++;
            continue;
        }
        if (selection == 0) {
            if (NCLP_SCAN_DEBUG != 0U) {
                xil_printf("  Lane %s: no supported identity window\r\n",
                           nclp_stream_names[lane]);
            }
            continue;
        }

        uint32_t best_phase = SPI_SCAN_PHASE_FIRST +
                              identity_windows[lane].middle;
        uint32_t selected = best_phase - SPI_SCAN_PHASE_FIRST;

        nclp_headstage.best_lane_phases_a = packed_lane_phase_set(nclp_headstage.best_lane_phases_a,
                                                      lane, best_phase);
        nclp_headstage.best_lane_phases_b = packed_lane_phase_set(nclp_headstage.best_lane_phases_b,
                                                      lane, best_phase);
        nclp_headstage.best_stream_score[lane] = phase_stream_scores[selected][lane];
        nclp_headstage.best_stream_phase[lane] = best_phase;
        nclp_headstage.best_stream_chip_id[lane] = identity_chip_ids[lane];
        nclp_headstage.best_stream_num_amps[lane] = phase_num_amps[selected][lane];

        xil_printf("  Lane %s identity chip=%s mask=0x%04lx range=%lu..%lu middle=%lu\r\n",
                   nclp_stream_names[lane],
                   chip_type_name(identity_chip_ids[lane]),
                   (unsigned long)identity_masks[lane],
                   (unsigned long)identity_windows[lane].first,
                   (unsigned long)identity_windows[lane].last,
                   (unsigned long)best_phase);
        if (identity_chip_ids[lane] == 4U) {
            rhd2164_lane_mask |= (uint8_t)(1U << lane);
        }
    }

    if (rhd2164_lane_mask != 0U) {
        if (program_phase_scan_aux_bank(1U) != 0) {
            scan_errors++;
            nclp_progress_update(nclp_headstage.scan_progress_base + SPI_SCAN_PHASE_STEPS,
                                 0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU,
                                 1U, nclp_diagnostics.fail_count);
        } else {
            if (scan_reg59_view(0U, 53U, rhd2164_lane_mask,
                                nclp_headstage.best_lane_phases_a,
                                nclp_headstage.best_lane_phases_b,
                                SPI_SCAN_PHASE_COUNT,
                                reg59_a_masks) != 0) {
                scan_errors++;
            }

            /* MISO A must satisfy both the ROM identity and Reg59 marker. */
            for (uint32_t lane = 0U; lane < SPI_SCAN_STREAMS; ++lane) {
                uint8_t lane_bit = (uint8_t)(1U << lane);
                uint16_t accepted_a_mask;
                nclp_phase_window_t window_a;
                uint32_t selected_a;
                uint32_t selected_index;

                if ((rhd2164_lane_mask & lane_bit) == 0U) {
                    continue;
                }

                accepted_a_mask = identity_masks[lane] &
                                  reg59_a_masks[lane];
                if (nclp_phase_longest_window(accepted_a_mask,
                                              &window_a) != 0) {
                    xil_printf("  FAIL %s RHD2164 MISO A has no identity/Reg59 window identity=0x%04lx A53=0x%04lx\r\n",
                               nclp_stream_names[lane],
                               (unsigned long)identity_masks[lane],
                               (unsigned long)reg59_a_masks[lane]);
                    nclp_diagnostics.fail_count++;
                    scan_errors++;
                    continue;
                }

                selected_a = SPI_SCAN_PHASE_FIRST + window_a.middle;
                selected_index = selected_a - SPI_SCAN_PHASE_FIRST;
                nclp_headstage.best_lane_phases_a = packed_lane_phase_set(
                    nclp_headstage.best_lane_phases_a, lane, selected_a);
                nclp_headstage.best_stream_score[lane] =
                    phase_stream_scores[selected_index][lane];
                nclp_headstage.best_stream_phase[lane] = selected_a;
                nclp_headstage.best_stream_chip_id[lane] = 4U;
                nclp_headstage.best_stream_num_amps[lane] =
                    phase_num_amps[selected_index][lane];
                rhd2164_a_valid_mask |= lane_bit;

                xil_printf("  PASS %s RHD2164 MISO A identity=0x%04lx A53=0x%04lx accepted=0x%04lx range=%lu..%lu middle=%lu\r\n",
                           nclp_stream_names[lane],
                           (unsigned long)identity_masks[lane],
                           (unsigned long)reg59_a_masks[lane],
                           (unsigned long)accepted_a_mask,
                           (unsigned long)window_a.first,
                           (unsigned long)window_a.last,
                           (unsigned long)selected_a);
            }

            /* MISO B has its own absolute phase register, so sweep and select
             * it independently while holding the chosen MISO A phases fixed.
             */
            if (scan_reg59_view(1U, 58U, rhd2164_lane_mask,
                                nclp_headstage.best_lane_phases_a,
                                nclp_headstage.best_lane_phases_b,
                                2U * SPI_SCAN_PHASE_COUNT,
                                reg59_b_masks) != 0) {
                scan_errors++;
            }

            for (uint32_t lane = 0U; lane < SPI_SCAN_STREAMS; ++lane) {
                uint8_t lane_bit = (uint8_t)(1U << lane);
                nclp_phase_window_t window_b;
                uint32_t selected_b;

                if ((rhd2164_lane_mask & lane_bit) == 0U) {
                    continue;
                }
                if (nclp_phase_longest_window(reg59_b_masks[lane],
                                              &window_b) != 0) {
                    xil_printf("  FAIL %s RHD2164 MISO B has no Reg59 window B58=0x%04lx\r\n",
                               nclp_stream_names[lane],
                               (unsigned long)reg59_b_masks[lane]);
                    nclp_diagnostics.fail_count++;
                    scan_errors++;
                    continue;
                }

                selected_b = SPI_SCAN_PHASE_FIRST + window_b.middle;
                if ((rhd2164_a_valid_mask & lane_bit) == 0U) {
                    continue;
                }
                if (nclp_rhd2164_phase_order_valid(
                        nclp_headstage.best_stream_phase[lane], selected_b) == 0) {
                    xil_printf("  FAIL %s RHD2164 phase order requires B>A: A=%lu B=%lu identity=0x%04lx A53=0x%04lx B58=0x%04lx\r\n",
                               nclp_stream_names[lane],
                               (unsigned long)nclp_headstage.best_stream_phase[lane],
                               (unsigned long)selected_b,
                               (unsigned long)identity_masks[lane],
                               (unsigned long)reg59_a_masks[lane],
                               (unsigned long)reg59_b_masks[lane]);
                    nclp_diagnostics.fail_count++;
                    scan_errors++;
                    continue;
                }

                nclp_headstage.best_lane_phases_b = packed_lane_phase_set(
                    nclp_headstage.best_lane_phases_b, lane, selected_b);
                nclp_headstage.best_stream_score[lane] += 8U;
                nclp_headstage.best_stream_reg59[lane] = 53U;

                xil_printf("  PASS %s RHD2164 absolute phases A=%lu B=%lu B58=0x%04lx B-range=%lu..%lu\r\n",
                           nclp_stream_names[lane],
                           (unsigned long)nclp_headstage.best_stream_phase[lane],
                           (unsigned long)selected_b,
                           (unsigned long)reg59_b_masks[lane],
                           (unsigned long)window_b.first,
                           (unsigned long)window_b.last);
            }
        }
    } else {
        /* No RHD2164 needs the two marker sweeps, but expose the same complete
         * 48-step progress contract to the command/status interface.
         */
        nclp_progress_update(nclp_headstage.scan_progress_base + SPI_SCAN_PHASE_STEPS,
                             0xFFFFFFFFU, 0xFFFFFFFFU, 0xFFFFFFFFU,
                             0U, nclp_diagnostics.fail_count);
    }

    stop_spi_safely();
    if (write_lane_phases_verified(nclp_headstage.best_lane_phases_a,
                                   nclp_headstage.best_lane_phases_b) != 0) {
        scan_errors++;
    }

    DEBUG_PRINT("\r\n  FINAL lane_phases_A=0x%08lx lane_phases_B=0x%08lx\r\n",
                (unsigned long)nclp_headstage.best_lane_phases_a,
                (unsigned long)nclp_headstage.best_lane_phases_b);
    print_detection_summary();
    return (scan_errors == 0U) ? 0 : -1;
}
