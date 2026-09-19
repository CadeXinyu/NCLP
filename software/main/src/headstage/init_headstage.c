#include "headstage_internal.h"

#define INIT_TO_IMPEDANCE_SETTLE_US 100000U

/* Register 1 (ADC buffer bias) and register 2 (MUX bias/load) depend on
 * sample rate.  acquisition_config.c selects these values before building an
 * INIT command list.  Keep the power-on values equal to the 30 kS/s profile
 * so initialization starts with a known profile before configuration is
 * initialized. */

static const init_analog_upper_cutoff_t g_init_analog_upper_cutoffs[] = {
    {20000U, 8U, 0U, 4U, 0U},
    {15000U, 11U, 0U, 8U, 0U},
    {10000U, 17U, 0U, 16U, 0U},
    {7500U, 22U, 0U, 23U, 0U},
    {5000U, 33U, 0U, 37U, 0U},
    {3000U, 3U, 1U, 13U, 1U},
    {2500U, 13U, 1U, 25U, 1U},
    {2000U, 27U, 1U, 44U, 1U},
    {1500U, 1U, 2U, 23U, 2U},
    {1000U, 46U, 2U, 30U, 3U},
    {750U, 41U, 3U, 36U, 4U},
    {500U, 30U, 5U, 43U, 6U},
    {300U, 6U, 9U, 2U, 11U},
    {250U, 42U, 10U, 5U, 13U},
    {200U, 24U, 13U, 7U, 16U},
    {150U, 44U, 17U, 8U, 21U},
    {100U, 38U, 26U, 5U, 31U}
};

static const init_analog_lower_cutoff_t g_init_analog_lower_cutoffs[] = {
    {500000U, 13U, 0U, 0U},
    {300000U, 15U, 0U, 0U},
    {250000U, 17U, 0U, 0U},
    {200000U, 18U, 0U, 0U},
    {150000U, 21U, 0U, 0U},
    {100000U, 25U, 0U, 0U},
    {75000U, 28U, 0U, 0U},
    {50000U, 34U, 0U, 0U},
    {30000U, 44U, 0U, 0U},
    {25000U, 48U, 0U, 0U},
    {20000U, 54U, 0U, 0U},
    {15000U, 62U, 0U, 0U},
    {10000U, 5U, 1U, 0U},
    {7500U, 18U, 1U, 0U},
    {5000U, 40U, 1U, 0U},
    {3000U, 20U, 2U, 0U},
    {2500U, 42U, 2U, 0U},
    {2000U, 8U, 3U, 0U},
    {1500U, 9U, 4U, 0U},
    {1000U, 44U, 6U, 0U},
    {750U, 49U, 9U, 0U},
    {500U, 35U, 17U, 0U},
    {300U, 1U, 40U, 0U},
    {250U, 56U, 54U, 0U},
    {100U, 16U, 60U, 1U}
};

const init_analog_upper_cutoff_t * init_find_analog_upper_cutoff(uint32_t analog_upper_cutoff_hz)
{
    for (uint32_t i = 0U; i < (sizeof(g_init_analog_upper_cutoffs) / sizeof(g_init_analog_upper_cutoffs[0])); ++i) {
        if (g_init_analog_upper_cutoffs[i].analog_upper_cutoff_hz == analog_upper_cutoff_hz) {
            return &g_init_analog_upper_cutoffs[i];
        }
    }

    return 0;
}

const init_analog_lower_cutoff_t * init_find_analog_lower_cutoff(uint32_t analog_lower_cutoff_millihz)
{
    for (uint32_t i = 0U; i < (sizeof(g_init_analog_lower_cutoffs) / sizeof(g_init_analog_lower_cutoffs[0])); ++i) {
        if (g_init_analog_lower_cutoffs[i].analog_lower_cutoff_millihz == analog_lower_cutoff_millihz) {
            return &g_init_analog_lower_cutoffs[i];
        }
    }

    return 0;
}

uint32_t aux_bank_select_value(uint32_t aux_bank)
{
    uint32_t bank = aux_bank & INTAN_AUX_BANK_SELECT_FIELD_MASK;

    return INTAN_AUX_BANK_SELECT_PACK(bank, bank, bank, bank);
}

static int aux_bank_select_write_check(const char *name, uint32_t reg_offset,
                                       uint32_t expected)
{
    uint32_t readback = 0U;

    intan_register_write(reg_offset, expected);
    if (intan_reg_read(reg_offset, &readback) != 0) {
        xil_printf("  FAIL %s readback\r\n", name);
        return -1;
    }

    readback &= 0xFFU;
    if (readback != expected) {
        xil_printf("  FAIL %s readback got=0x%02lx expected=0x%02lx\r\n",
                   name,
                   (unsigned long)readback,
                   (unsigned long)expected);
        return -1;
    }

    return 0;
}

static int uniform_aux_bank_select_write_check(const char *name,
                                               uint32_t reg_offset,
                                               uint32_t aux_bank)
{
    return aux_bank_select_write_check(name, reg_offset,
                                       aux_bank_select_value(aux_bank));
}

static uint32_t init_max_write_reg_for_chip(uint8_t chip_id)
{
    return (chip_channel_count(chip_id) == 64U) ? 21U : 17U;
}

static uint32_t init_last_write_index_for_chip(uint8_t chip_id)
{
    return 2U + init_max_write_reg_for_chip(chip_id);
}

static int init_write_value(uint32_t reg, uint8_t *value, uint32_t vdd_sense_enable,
                            uint32_t dsp_enable, uint32_t dsp_cutoff_freq,
                            const init_analog_upper_cutoff_t *analog_upper_cutoff,
                            const init_analog_lower_cutoff_t *analog_lower_cutoff)
{
    static const uint8_t values[22] = {
        0xDEU, 0x42U, 0x04U, 0x00U, 0x80U, 0x00U, 0x80U, 0x00U,
        0x16U, 0x80U, 0x17U, 0x80U, 0x2CU, 0x86U, 0xFFU, 0xFFU,
        0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU, 0xFFU
    };

    if (reg >= 22U) {
        return 0;
    }

    *value = values[reg];
    if (reg == 1U) {
        *value = nclp_operation.init_rhd_reg1_value;
        if (vdd_sense_enable == 0U) {
            *value = (uint8_t)(*value & ~(1U << 6));
        }
    }
    if (reg == 2U) {
        *value = nclp_operation.init_rhd_reg2_value;
    }
    if (reg == 4U) {
        *value = (uint8_t)((*value & 0xE0U) |
                           ((dsp_enable != 0U) ? 0x10U : 0x00U) |
                           (dsp_cutoff_freq & 0x0FU));
    }
    if (reg == 8U) {
        *value = analog_upper_cutoff->rh1_dac1;
    }
    if (reg == 9U) {
        *value = (uint8_t)(0x80U | (analog_upper_cutoff->rh1_dac2 & 0x1FU));
    }
    if (reg == 10U) {
        *value = analog_upper_cutoff->rh2_dac1;
    }
    if (reg == 11U) {
        *value = (uint8_t)(0x80U | (analog_upper_cutoff->rh2_dac2 & 0x1FU));
    }
    if (reg == 12U) {
        *value = analog_lower_cutoff->rl_dac1;
    }
    if (reg == 13U) {
        *value = (uint8_t)(0x80U |
                           ((analog_lower_cutoff->rl_dac3 & 0x01U) << 6) |
                           (analog_lower_cutoff->rl_dac2 & 0x3FU));
    }
    return 1;
}

static uint16_t init_aux3_command(uint32_t index, uint8_t chip_id, uint32_t vdd_sense_enable,
                                  uint32_t dsp_enable, uint32_t dsp_cutoff_freq,
                                  const init_analog_upper_cutoff_t *analog_upper_cutoff,
                                  const init_analog_lower_cutoff_t *analog_lower_cutoff)
{
    uint32_t max_write_reg = init_max_write_reg_for_chip(chip_id);
    uint8_t value = 0U;

    if (index == 0U) {
        return rhd_clear_cal_cmd();
    }

    if (index == 1U) {
        return rhd_read_reg_cmd(63U);
    }

    if (index >= 2U && (index - 2U) <= max_write_reg &&
        init_write_value(index - 2U, &value, vdd_sense_enable, dsp_enable, dsp_cutoff_freq,
                         analog_upper_cutoff, analog_lower_cutoff)) {
        return rhd_write_reg_cmd(index - 2U, value);
    }

    if (index == AUX_INIT_CALIBRATE_INDEX) {
        return rhd_calibrate_cmd();
    }

    /* Indexes 20..23 are connector-profile fillers for RHD2132/RHD2216 and
     * WRITE18..21 for RHD2164.  Indexes 24..26 are the common pre-calibration
     * guard, and index 28 is the common post-calibration proof command. */
    return rhd_read_reg_cmd(63U);
}

static int init_expected_write_echo(uint32_t command_index, uint8_t profile_chip_id,
                                    uint32_t vdd_sense_enable, uint32_t dsp_enable,
                                    uint32_t dsp_cutoff_freq,
                                    const init_analog_upper_cutoff_t *analog_upper_cutoff,
                                    const init_analog_lower_cutoff_t *analog_lower_cutoff,
                                    uint8_t *expected)
{
    uint32_t max_write_index = init_last_write_index_for_chip(profile_chip_id);
    uint16_t cmd;

    if (command_index < 2U || command_index > max_write_index) {
        return 0;
    }

    cmd = init_aux3_command(command_index, profile_chip_id, vdd_sense_enable, dsp_enable, dsp_cutoff_freq,
                            analog_upper_cutoff, analog_lower_cutoff);
    *expected = (uint8_t)(cmd & 0x00FFU);
    return 1;
}

static uint8_t init_connector_profile_chip_id(uint32_t connector, uint32_t physical_stream_mask)
{
    uint8_t profile_chip_id = 1U;

    /* A connector has two physical MISO lanes but one MOSI command wire.  If
     * either attached chip is an RHD2164, use the 64-channel command profile;
     * WRITE18..21 are specified no-ops on an RHD2132/RHD2216 sharing it. */
    for (uint32_t lane = 0U; lane < SPI_LANES_PER_CONNECTOR; ++lane) {
        uint32_t stream = (SPI_LANES_PER_CONNECTOR * connector) + lane;

        if ((physical_stream_mask & (1U << stream)) != 0U) {
            uint8_t chip_id = nclp_headstage.best_stream_chip_id[stream];

            if (chip_channel_count(chip_id) == 64U) {
                return chip_id;
            }
            if (chip_channel_count(chip_id) != 0U) {
                profile_chip_id = chip_id;
            }
        }
    }

    return profile_chip_id;
}

static uint32_t init_aux3_connector_banks_value(void)
{
    uint32_t packed = 0U;

    for (uint32_t connector = 0U; connector < SPI_CONNECTOR_COUNT; ++connector) {
        packed |= connector << (2U * connector);
    }
    return packed;
}

static int program_init_aux_banks(uint32_t physical_stream_mask,
                                  uint32_t vdd_sense_enable,
                                  uint32_t dsp_enable, uint32_t dsp_cutoff_freq,
                                  const init_analog_upper_cutoff_t *analog_upper_cutoff,
                                  const init_analog_lower_cutoff_t *analog_lower_cutoff)
{
    uint16_t filler = rhd_read_reg_cmd(63U);

    xil_printf("\r\n[3] Write one 29-frame mixed-headstage init schedule\r\n");
    stop_spi_safely();

    /* INIT_DUMMY overrides slots 0..33 with READ63, so Aux1/Aux2 only need a
     * valid index-zero word and may remain fixed at MAX=0 for the session. */
    intan_aux_command_write(INTAN_AUX1_WINDOW_OFFSET, 0U, 0U, filler);
    intan_aux_command_write(INTAN_AUX2_WINDOW_OFFSET, 0U, 0U, filler);

    for (uint32_t connector = 0U; connector < SPI_CONNECTOR_COUNT; ++connector) {
        uint8_t profile_chip_id =
            init_connector_profile_chip_id(connector, physical_stream_mask);

        for (uint32_t index = 0U; index < AUX_INIT_COMMAND_COUNT; ++index) {
            uint16_t init = init_aux3_command(
                index, profile_chip_id, vdd_sense_enable, dsp_enable,
                dsp_cutoff_freq, analog_upper_cutoff, analog_lower_cutoff);

            intan_aux_command_write(INTAN_AUX3_WINDOW_OFFSET, connector,
                                    index, init);
        }

        DEBUG_PRINT("  INFO connector %c bank=%lu profile=%s indexes 0..28\r\n",
                    (int)('A' + connector),
                    (unsigned long)connector,
                    chip_channel_count(profile_chip_id) == 64U ?
                        "WRITE0..21" : "WRITE0..17+READ63x4");
    }

    xil_printf("  PASS Aux3 banks A/B/C/D=0/1/2/3, common CALIBRATE index 27, final READ63 index 28\r\n");
    DEBUG_PRINT("  INFO init dummy mode makes CS slots 0..33 send READ63; only slot 34 sends Aux3 command\r\n");
    return 0;
}

static int init_expected_aux3_reply(uint32_t command_index, uint8_t reply_chip_id,
                                    uint8_t profile_chip_id,
                                    uint32_t vdd_sense_enable, uint32_t dsp_enable,
                                    uint32_t dsp_cutoff_freq,
                                    const init_analog_upper_cutoff_t *analog_upper_cutoff,
                                    const init_analog_lower_cutoff_t *analog_lower_cutoff,
                                    uint16_t *expected, uint16_t *mask)
{
    uint8_t write_echo = 0U;

    if (command_index == 0U) {
        *expected = 0U;
        *mask = 0x7FFFU;
        return 1;
    }

    if (command_index == 1U) {
        *expected = reply_chip_id;
        *mask = 0x00FFU;
        return 1;
    }

    if (command_index > init_last_write_index_for_chip(profile_chip_id) &&
        command_index < AUX_INIT_CALIBRATE_INDEX) {
        *expected = reply_chip_id;
        *mask = 0x00FFU;
        return 1;
    }

    /* The final Aux3 READ(63) is preceded by all 34 benign commands in the
     * post-CALIBRATE frame.  Besides satisfying the nine-dummy-command rule,
     * its returned chip ID proves that the final scheduled frame executed. */
    if (command_index == AUX_INIT_LAST_INDEX) {
        *expected = reply_chip_id;
        *mask = 0x00FFU;
        return 1;
    }

    if (init_expected_write_echo(command_index, profile_chip_id, vdd_sense_enable, dsp_enable,
                                 dsp_cutoff_freq, analog_upper_cutoff, analog_lower_cutoff, &write_echo)) {
        /* WRITE(R,D) returns exactly 0xFF00 | D, including writes to
         * read-only or non-existent registers.  Check the complete response
         * so a malformed reply cannot pass merely by matching D. */
        *expected = (uint16_t)(0xFF00U | write_echo);
        *mask = 0xFFFFU;
        return 1;
    }

    if (command_index == AUX_INIT_CALIBRATE_INDEX) {
        *expected = 0U;
        *mask = 0x7FFFU;
        return 1;
    }

    return 0;
}

static int verify_init_aux3_result(uint32_t command_index, uint16_t word,
                                   uint8_t reply_chip_id, uint8_t profile_chip_id,
                                   uint32_t vdd_sense_enable, uint32_t dsp_enable,
                                   uint32_t dsp_cutoff_freq,
                                   const init_analog_upper_cutoff_t *analog_upper_cutoff,
                                   const init_analog_lower_cutoff_t *analog_lower_cutoff)
{
    uint16_t expected = 0U;
    uint16_t mask = 0U;

    if (!init_expected_aux3_reply(command_index, reply_chip_id, profile_chip_id,
                                  vdd_sense_enable, dsp_enable,
                                  dsp_cutoff_freq, analog_upper_cutoff, analog_lower_cutoff, &expected, &mask)) {
        return 1;
    }

    /* READ(63) must return the exact chip ID recorded by SCAN.  Accepting any
     * supported ID here can hide a swapped or misrouted headstage. */
    return ((word & mask) == (expected & mask));
}

static int configure_init_verify_trial(uint32_t stream_mask,
                                       uint32_t packed_lane_phases_a,
                                       uint32_t packed_lane_phases_b)
{
    uint32_t aux3_banks = init_aux3_connector_banks_value();

    intan_register_write(INTAN_REG_LOGICAL_STREAM_ENABLE, 0U);
    if ((write_lane_phases_verified(packed_lane_phases_a,
                                    packed_lane_phases_b) != 0) ||
        (uniform_aux_bank_select_write_check("AUX1_BANK_SELECT",
                                             INTAN_REG_AUX1_BANK_SELECT, 0U) != 0) ||
        (uniform_aux_bank_select_write_check("AUX2_BANK_SELECT",
                                             INTAN_REG_AUX2_BANK_SELECT, 0U) != 0) ||
        (aux_bank_select_write_check("AUX3_BANK_SELECT",
                                     INTAN_REG_AUX3_BANK_SELECT, aux3_banks) != 0)) {
        return -1;
    }
    intan_register_write(INTAN_REG_AUX1_END_INDEX, 0U);
    intan_register_write(INTAN_REG_AUX2_END_INDEX, 0U);
    intan_register_write(INTAN_REG_AUX3_END_INDEX, AUX_INIT_LAST_INDEX);
    intan_register_write(INTAN_REG_AUX1_LOOP_INDEX, AUX_INIT_LOOP_INDEX);
    intan_register_write(INTAN_REG_AUX2_LOOP_INDEX, AUX_INIT_LOOP_INDEX);
    intan_register_write(INTAN_REG_AUX3_LOOP_INDEX, AUX_INIT_LOOP_INDEX);
    intan_register_write(INTAN_REG_FINITE_FRAME_COUNT, AUX_INIT_TIMESTEPS);
    intan_register_write(INTAN_REG_ACQUISITION_CONFIG,
                         INTAN_ACQUISITION_CONFIG_INIT_DUMMY);
    intan_register_write(INTAN_REG_LOGICAL_STREAM_ENABLE, stream_mask);
    return start_intan_capture();
}

typedef struct {
    uint32_t checks;
    uint32_t errors;
    uint32_t timestamp_errors;
    uint32_t sync_losses;
} init_verify_summary_t;

static uint32_t init_primary_logical_stream_mask(uint32_t physical_stream_mask)
{
    uint32_t logical_mask = 0U;

    for (uint32_t stream = 0U; stream < SPI_SCAN_STREAMS; ++stream) {
        if ((physical_stream_mask & (1U << stream)) != 0U) {
            logical_mask |= 1U << (2U * stream);
        }
    }
    return logical_mask;
}

static uint32_t init_physical_stream_count(uint32_t physical_stream_mask)
{
    uint32_t count = 0U;

    for (uint32_t stream = 0U; stream < SPI_SCAN_STREAMS; ++stream) {
        if ((physical_stream_mask & (1U << stream)) != 0U) {
            count++;
        }
    }
    return count;
}

static uint32_t init_expected_capture_bytes(uint32_t stream_count)
{
    return 2U * frame_words_for_stream_count(stream_count) *
           AUX_INIT_CAPTURE_FRAMES;
}

static void init_mark_selected_summary_error(
    uint32_t physical_stream_mask,
    init_verify_summary_t summaries[SPI_SCAN_STREAMS])
{
    for (uint32_t stream = 0U; stream < SPI_SCAN_STREAMS; ++stream) {
        if ((physical_stream_mask & (1U << stream)) != 0U) {
            summaries[stream].errors = 1U;
        }
    }
}

static int capture_and_verify_init_streams(
    uint32_t physical_stream_mask,
    uint32_t vdd_sense_enable,
    uint32_t dsp_enable, uint32_t dsp_cutoff_freq,
    const init_analog_upper_cutoff_t *analog_upper_cutoff,
    const init_analog_lower_cutoff_t *analog_lower_cutoff,
    init_verify_summary_t summaries[SPI_SCAN_STREAMS])
{
    uint16_t word_buffer[SPI_FRAME_BUFFER_WORDS];
    uint32_t timestamps[AUX_INIT_CAPTURE_FRAMES];
    timestamp_check_result_t ts_check;
    uint32_t frames_seen = 0U;
    uint32_t sync_losses = 0U;
    uint32_t init_stream_count = init_physical_stream_count(physical_stream_mask);
    uint32_t init_frame_words = frame_words_for_stream_count(init_stream_count);
    uint32_t expected_capture_bytes =
        init_expected_capture_bytes(init_stream_count);
    uint32_t words32_count;
    uint32_t best_order = 0U;
    uint32_t best_offset16 = 0U;
    uint32_t best_matches = 0U;
    uint32_t all_ok = 1U;

    memset(summaries, 0, sizeof(init_verify_summary_t) * SPI_SCAN_STREAMS);
    if (init_stream_count == 0U) {
        return -1;
    }
    if (expected_capture_bytes > SPI_DDR_RING_BYTES ||
        expected_capture_bytes > nclp_capture.capture_ring_bytes ||
        expected_capture_bytes > SPI_DDR_COLLECT_BYTES) {
        xil_printf("  FAIL mixed init capture capacity bytes=%lu ring=%lu active=%lu collect=%lu\r\n",
                   (unsigned long)expected_capture_bytes,
                   (unsigned long)SPI_DDR_RING_BYTES,
                   (unsigned long)nclp_capture.capture_ring_bytes,
                   (unsigned long)SPI_DDR_COLLECT_BYTES);
        init_mark_selected_summary_error(physical_stream_mask, summaries);
        nclp_diagnostics.fail_count++;
        return -1;
    }

    if (ddr_capture_collect(SPI_CAPTURE_BLOCK_BYTES) != 0) {
        init_mark_selected_summary_error(physical_stream_mask, summaries);
        return -1;
    }
    if (nclp_capture.capture_valid_bytes != expected_capture_bytes) {
        xil_printf("  FAIL mixed init capture length bytes=%lu expected=%lu blocks=%lu tail=%lu\r\n",
                   (unsigned long)nclp_capture.capture_valid_bytes,
                   (unsigned long)expected_capture_bytes,
                   (unsigned long)((expected_capture_bytes +
                                    SPI_CAPTURE_BLOCK_BYTES - 1U) /
                                   SPI_CAPTURE_BLOCK_BYTES),
                   (unsigned long)(((expected_capture_bytes - 1U) %
                                    SPI_CAPTURE_BLOCK_BYTES) + 1U));
        init_mark_selected_summary_error(physical_stream_mask, summaries);
        nclp_diagnostics.fail_count++;
        return -1;
    }

    words32_count = nclp_capture.capture_valid_words32;
    best_matches = find_best_capture_order_and_offset(init_stream_count, &best_order, &best_offset16);
    if (best_matches == 0U) {
        if (NCLP_SCAN_DEBUG != 0U) {
            print_capture_word_order_debug("init-verify", words32_count);
        }
        xil_printf("  FAIL init capture sync matches=0 bytes=%lu\r\n",
                   (unsigned long)nclp_capture.capture_valid_bytes);
        for (uint32_t stream = 0U; stream < SPI_SCAN_STREAMS; ++stream) {
            if ((physical_stream_mask & (1U << stream)) != 0U) {
                summaries[stream].sync_losses = 1U;
            }
        }
        nclp_diagnostics.fail_count++;
        return -1;
    }

    for (uint32_t frame = 0U; frame < AUX_INIT_CAPTURE_FRAMES; ++frame) {
        uint32_t start16 = best_offset16 + (frame * init_frame_words);

        if ((start16 + init_frame_words) > (words32_count * 2U)) {
            break;
        }
        if (!magic_at_capture(nclp_capture.capture_ring_words, start16, best_order)) {
            sync_losses++;
            continue;
        }

        for (uint32_t w = 0U; w < init_frame_words; ++w) {
            word_buffer[w] = get_capture16(nclp_capture.capture_ring_words, start16 + w, best_order);
        }
        timestamps[frames_seen] = frame_timestamp32(word_buffer);

        {
            uint32_t command_index = timestamps[frames_seen];
            uint32_t capture_stream = 0U;

            for (uint32_t stream = 0U; stream < SPI_SCAN_STREAMS; ++stream) {
                if ((physical_stream_mask & (1U << stream)) != 0U) {
                    uint8_t chip_id = nclp_headstage.best_stream_chip_id[stream];
                    uint8_t profile_chip_id = init_connector_profile_chip_id(
                        stream / SPI_LANES_PER_CONNECTOR,
                        physical_stream_mask);
                    uint16_t aux3_word = frame_word_for_stream_count(
                        word_buffer, SPI_FRAME_AUX3_SLOT,
                        capture_stream, init_stream_count);
                    summaries[stream].checks++;
                    if (command_index >= AUX_INIT_COMMAND_COUNT) {
                        xil_printf("  FAIL init %s unexpected timestamp/index=%lu\r\n",
                                   nclp_stream_names[stream],
                                   (unsigned long)command_index);
                        summaries[stream].errors++;
                        capture_stream++;
                        continue;
                    }

                    if (!verify_init_aux3_result(
                            command_index, aux3_word, chip_id,
                            profile_chip_id, vdd_sense_enable, dsp_enable,
                            dsp_cutoff_freq, analog_upper_cutoff,
                            analog_lower_cutoff)) {
                        uint16_t expected = 0U;
                        uint16_t mask = 0U;

                        (void)init_expected_aux3_reply(
                            command_index, chip_id, profile_chip_id,
                            vdd_sense_enable, dsp_enable, dsp_cutoff_freq,
                            analog_upper_cutoff, analog_lower_cutoff,
                            &expected, &mask);
                        xil_printf("  FAIL init %s cmd idx=%lu sent=0x%04x got=0x%04x expected=0x%04x mask=0x%04x frame=%lu\r\n",
                                   nclp_stream_names[stream],
                                   (unsigned long)command_index,
                                   (unsigned int)init_aux3_command(
                                       command_index, profile_chip_id,
                                       vdd_sense_enable, dsp_enable,
                                       dsp_cutoff_freq, analog_upper_cutoff,
                                       analog_lower_cutoff),
                                   (unsigned int)aux3_word,
                                   (unsigned int)expected,
                                   (unsigned int)mask,
                                   (unsigned long)frames_seen);
                        summaries[stream].errors++;
                    }
                    capture_stream++;
                }
            }
        }

        frames_seen++;
    }

    check_timestamp_sequence(timestamps, frames_seen, &ts_check);
    for (uint32_t stream = 0U; stream < SPI_SCAN_STREAMS; ++stream) {
        if ((physical_stream_mask & (1U << stream)) != 0U) {
            summaries[stream].timestamp_errors = ts_check.errors;
            summaries[stream].sync_losses = sync_losses;
        }
    }
    if (ts_check.errors != 0U) {
        xil_printf("  FAIL init timestamp gap frame=%lu expected=%lu actual=%lu errors=%lu\r\n",
                   (unsigned long)ts_check.first_bad_frame,
                   (unsigned long)ts_check.expected_timestamp,
                   (unsigned long)ts_check.actual_timestamp,
                   (unsigned long)ts_check.errors);
        nclp_diagnostics.fail_count++;
    } else {
        DEBUG_PRINT("  PASS init timestamp check frames=%lu first=%lu last=%lu\r\n",
                    (unsigned long)ts_check.frames,
                    (unsigned long)ts_check.first_timestamp,
                    (unsigned long)ts_check.last_timestamp);
    }

    if (frames_seen != AUX_INIT_COMMAND_COUNT ||
        ts_check.errors != 0U || sync_losses != 0U) {
        all_ok = 0U;
    }
    for (uint32_t stream = 0U; stream < SPI_SCAN_STREAMS; ++stream) {
        if ((physical_stream_mask & (1U << stream)) != 0U) {
            if (summaries[stream].errors == 0U &&
                summaries[stream].checks == AUX_INIT_COMMAND_COUNT &&
                ts_check.errors == 0U && sync_losses == 0U) {
                xil_printf("  PASS init %s MISO verification checks=%lu\r\n",
                           nclp_stream_names[stream],
                           (unsigned long)summaries[stream].checks);
            } else {
                xil_printf("  FAIL init %s MISO verification checks=%lu expected=%lu errors=%lu frames=%lu sync_losses=%lu\r\n",
                           nclp_stream_names[stream],
                           (unsigned long)summaries[stream].checks,
                           (unsigned long)AUX_INIT_COMMAND_COUNT,
                           (unsigned long)summaries[stream].errors,
                           (unsigned long)frames_seen,
                           (unsigned long)sync_losses);
                all_ok = 0U;
            }
        }
    }

    if (all_ok != 0U) {
        xil_printf("  PASS mixed init capture streams=%lu frames=%lu frame_words=%lu\r\n",
                   (unsigned long)init_stream_count,
                   (unsigned long)frames_seen,
                   (unsigned long)init_frame_words);
        return 0;
    }

    nclp_diagnostics.fail_count++;
    return -1;
}

static int run_post_detection_init_session(
    uint32_t vdd_sense_enable,
    uint32_t dsp_enable,
    uint32_t dsp_cutoff_freq,
    uint32_t analog_upper_cutoff_hz,
    uint32_t analog_lower_cutoff_millihz,
    init_verify_summary_t summaries[SPI_SCAN_STREAMS])
{
    uint32_t physical_stream_mask = (uint32_t)selected_stream_mask();
    uint32_t logical_stream_mask =
        init_primary_logical_stream_mask(physical_stream_mask);
    const init_analog_upper_cutoff_t *analog_upper_cutoff = init_find_analog_upper_cutoff(analog_upper_cutoff_hz);
    const init_analog_lower_cutoff_t *analog_lower_cutoff = init_find_analog_lower_cutoff(analog_lower_cutoff_millihz);

    memset(summaries, 0, sizeof(init_verify_summary_t) * SPI_SCAN_STREAMS);

    if (analog_upper_cutoff == 0) {
        xil_printf("\r\n[6] FAIL init: unsupported upper bandwidth %lu Hz\r\n",
                   (unsigned long)analog_upper_cutoff_hz);
        init_mark_selected_summary_error(physical_stream_mask, summaries);
        nclp_diagnostics.fail_count++;
        return -1;
    }
    if (analog_lower_cutoff == 0) {
        xil_printf("\r\n[6] FAIL init: unsupported lower bandwidth %lu mHz\r\n",
                   (unsigned long)analog_lower_cutoff_millihz);
        init_mark_selected_summary_error(physical_stream_mask, summaries);
        nclp_diagnostics.fail_count++;
        return -1;
    }

    if (physical_stream_mask == 0U || logical_stream_mask == 0U) {
        xil_printf("\r\n[6] Skip init: no supported RHD stream detected\r\n");
        return -1;
    }

    xil_printf("\r\n[6] Configure all detected chips in one init dummy session physical=0x%02lx logical-primary=0x%04lx\r\n",
               (unsigned long)physical_stream_mask,
               (unsigned long)logical_stream_mask);
    if (program_init_aux_banks(
            physical_stream_mask, vdd_sense_enable, dsp_enable,
            dsp_cutoff_freq, analog_upper_cutoff,
            analog_lower_cutoff) != 0) {
        init_mark_selected_summary_error(physical_stream_mask, summaries);
        return -1;
    }

    xil_printf("  INIT verify all primary MISOs lane_phases_A=0x%08lx lane_phases_B=0x%08lx\r\n",
               (unsigned long)nclp_headstage.best_lane_phases_a,
               (unsigned long)nclp_headstage.best_lane_phases_b);
    stop_spi_safely();
    if (ddr_capture_prepare(SPI_CAPTURE_BLOCK_BYTES) != 0 ||
        configure_init_verify_trial(logical_stream_mask,
                                    nclp_headstage.best_lane_phases_a,
                                    nclp_headstage.best_lane_phases_b) != 0) {
        xil_printf("  FAIL mixed init verify setup\r\n");
        nclp_diagnostics.fail_count++;
        init_mark_selected_summary_error(physical_stream_mask, summaries);
        stop_spi_safely();
        return -1;
    }
    if (capture_and_verify_init_streams(
            physical_stream_mask, vdd_sense_enable, dsp_enable,
            dsp_cutoff_freq, analog_upper_cutoff, analog_lower_cutoff,
            summaries) != 0) {
        xil_printf("  FAIL mixed init MISO verification\r\n");
        stop_spi_safely();
        return -1;
    }

    stop_spi_safely();
    usleep(INIT_TO_IMPEDANCE_SETTLE_US);
    xil_printf("  PASS one mixed init sequence verified on every detected chip, waited 0.1 s\r\n");
    return 0;
}

int run_post_detection_init_all(uint32_t vdd_sense_enable, uint32_t dsp_enable,
                                       uint32_t dsp_cutoff_freq, uint32_t analog_upper_cutoff_hz,
                                       uint32_t analog_lower_cutoff_millihz)
{
    uint32_t stream_mask = (uint32_t)selected_stream_mask();
    init_verify_summary_t verifications[SPI_SCAN_STREAMS];
    int session_status;
    uint32_t result_failure = 0U;

    if (stream_mask == 0U) {
        xil_printf("\r\n[6] Skip init: no supported RHD stream detected\r\n");
        return -1;
    }

    session_status = run_post_detection_init_session(
        vdd_sense_enable, dsp_enable, dsp_cutoff_freq,
        analog_upper_cutoff_hz, analog_lower_cutoff_millihz,
        verifications);

    for (uint32_t stream = 0U; stream < SPI_SCAN_STREAMS; ++stream) {
        if ((stream_mask & (1U << stream)) != 0U) {
            nclp_result_record_t record = {0};
            init_verify_summary_t verification = verifications[stream];
            uint32_t record_index =
                nclp_result_staged_count(NCLP_RESULT_TYPE_INIT);
            uint32_t final_phase = nclp_headstage.best_stream_phase[stream];
            uint32_t aggregate_errors = verification.errors +
                                        verification.timestamp_errors +
                                        verification.sync_losses;
            int status = (session_status == 0 &&
                          verification.checks == AUX_INIT_COMMAND_COUNT &&
                          aggregate_errors == 0U) ? 0 : -1;

            if (status != 0 && aggregate_errors == 0U) {
                aggregate_errors = 1U;
            }
            if (aggregate_errors > 0xFFFFU) {
                aggregate_errors = 0xFFFFU;
            }

            record.init.info = (stream & 0x7U) |
                               (1U << 3) |
                               ((status == 0 ? 1U : 0U) << 4) |
                               ((vdd_sense_enable & 0x1U) << 5) |
                               ((dsp_enable & 0x1U) << 6) |
                               ((dsp_cutoff_freq & 0xFU) << 8) |
                               ((final_phase & 0xFU) << 12) |
                               ((uint32_t)nclp_headstage.best_stream_chip_id[stream] << 16) |
                               (((status == 0) ? 0U : 1U) << 24);
            record.init.checks_errors =
                (verification.checks > 0xFFFFU ? 0xFFFFU :
                                                 verification.checks) |
                (aggregate_errors << 16);
            record.init.analog_upper_hz = analog_upper_cutoff_hz;
            record.init.analog_lower_millihz =
                analog_lower_cutoff_millihz;
            if (nclp_result_write_record(NCLP_RESULT_TYPE_INIT,
                                         record_index, &record) != 0) {
                xil_printf("  FAIL init result storage on %s\r\n",
                           nclp_stream_names[stream]);
                result_failure = 1U;
            }
            nclp_progress_update(record_index + 1U, stream,
                                 0xFFFFFFFFU, 0xFFFFFFFFU,
                                 (status == 0) ? 0U : 1U,
                                 nclp_diagnostics.fail_count);
        }
    }

    return (session_status == 0 && result_failure == 0U) ? 0 : -1;
}

/* Apply an INIT configuration to every detected chip without publishing an
 * INIT result generation.  This is intentionally separate from the normal
 * user INIT path: temporary impedance settings and their restoration must not
 * overwrite the user's last explicit INIT result. */
int run_post_detection_init_all_no_results(
    uint32_t vdd_sense_enable,
    uint32_t dsp_enable,
    uint32_t dsp_cutoff_freq,
    uint32_t analog_upper_cutoff_hz,
    uint32_t analog_lower_cutoff_millihz)
{
    uint32_t stream_mask = (uint32_t)selected_stream_mask();
    init_verify_summary_t verifications[SPI_SCAN_STREAMS];

    if (stream_mask == 0U) {
        xil_printf("  FAIL init apply: no supported RHD stream detected\r\n");
        return -1;
    }

    /* MOSI/CS are shared, so one session applies the configuration to every
     * chip even if one returned stream subsequently fails verification. */
    return run_post_detection_init_session(
        vdd_sense_enable, dsp_enable, dsp_cutoff_freq,
        analog_upper_cutoff_hz, analog_lower_cutoff_millihz,
        verifications);
}
