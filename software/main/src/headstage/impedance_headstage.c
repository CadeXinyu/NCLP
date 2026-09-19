/*
 * SPDX-License-Identifier: GPL-3.0-or-later
 *
 * Portions adapted from the Open Ephys RHD Recording Controller impedance
 * implementation, Copyright (C) 2021 Open Ephys.
 * NCLP modifications Copyright (c) 2026 CadeXinyu.
 *
 * This program is free software: you can redistribute it and/or modify it
 * under the terms of the GNU General Public License as published by the Free
 * Software Foundation, either version 3 of the License, or (at your option)
 * any later version.
 */

#include "headstage_internal.h"

/* RHD impedance test helpers.
 *
 * Open Ephys model:
 *   Reg5 selects/enable Zcheck and capacitor scale.
 *   Reg7 selects one amplifier channel.
 *   Reg6 loops a sine-wave DAC command list while amplifier data is captured.
 */

#include "impedance_cancel.h"

#define IMP_SAMPLE_RATE_HZ              30000U
#define IMP_TEST_FREQ_HZ                1000U
#define IMP_SINE_PERIOD_FRAMES          (IMP_SAMPLE_RATE_HZ / IMP_TEST_FREQ_HZ)
#define IMP_SETTLE_PERIODS              2U
#define IMP_MEASURE_PERIODS             20U
#define IMP_SETTLE_FRAMES               (IMP_SETTLE_PERIODS * IMP_SINE_PERIOD_FRAMES)
#define IMP_MEASURE_FRAMES              (IMP_MEASURE_PERIODS * IMP_SINE_PERIOD_FRAMES)
#define IMP_CAPTURE_FRAMES              (IMP_SETTLE_FRAMES + IMP_MEASURE_FRAMES)
#define IMP_CLEANUP_FRAMES              4U
#define IMP_TOTAL_FRAMES                (IMP_CAPTURE_FRAMES + IMP_CLEANUP_FRAMES)
#define IMP_REG6_BANK                   1U
#define IMP_REG57_BANK                  2U
#define IMP_DAC_MIDPOINT                128U
#define IMP_DAC_AMPLITUDE               128U
#define IMP_DAC_VOLTAGE_AMPLITUDE       ((double)IMP_DAC_AMPLITUDE * (1.225 / 256.0))
#define IMP_ADC_UV_PER_COUNT            0.195
#define IMP_PARASITIC_CAP_F             14.0e-12

#define IMP_REG5_DAC_POWER              0x40U
#define IMP_REG5_SCALE_SHIFT            3U
#define IMP_REG5_ENABLE                 0x01U

#define IMP_CLEANUP_VERIFY_FRAMES        7U

#ifndef NCLP_CAPTURE_CANCELLED
#define NCLP_CAPTURE_CANCELLED           (-2)
#endif

static uint32_t g_impedance_stream_count = 1U;

static int impedance_cancel_requested(void)
{
    return nclp_impedance_cancel_requested() != 0;
}

static void impedance_cancel_acknowledge(void)
{
    nclp_impedance_cancel_acknowledge();
}

/* 30-sample sine for 1 kHz at 30 kS/s, amplitude 128 around code 128. */
static const uint8_t g_imp_dac_sine_30[IMP_SINE_PERIOD_FRAMES] = {
    128U, 155U, 180U, 203U, 223U, 239U, 250U, 255U, 255U, 250U,
    239U, 223U, 203U, 180U, 155U, 128U, 101U,  76U,  53U,  33U,
     17U,   6U,   1U,   1U,   6U,  17U,  33U,  53U,  76U, 101U
};

static uint8_t impedance_reg5_value(uint32_t cap_range)
{
    uint8_t cap_bits;

    switch (cap_range) {
    case 0U:
        cap_bits = 0x00U; /* 0.1 pF */
        break;
    case 1U:
        cap_bits = 0x01U; /* 1.0 pF */
        break;
    default:
        cap_bits = 0x03U; /* 10.0 pF */
        break;
    }

    /* Intan Reg5: DAC power[6], dummy-load[5]=0, scale[4:3],
     * connect-all[2]=0, polarity[1]=0, Zcheck-enable[0].
     */
    return (uint8_t)(IMP_REG5_DAC_POWER |
                     (cap_bits << IMP_REG5_SCALE_SHIFT) |
                     IMP_REG5_ENABLE);
}

static const char *impedance_cap_name(uint32_t cap_range)
{
    switch (cap_range) {
    case 0U:
        return "0.1pF";
    case 1U:
        return "1pF";
    default:
        return "10pF";
    }
}

static uint16_t impedance_aux1_command(uint32_t index)
{
    if (index < IMP_SINE_PERIOD_FRAMES) {
        return rhd_write_reg_cmd(6U, g_imp_dac_sine_30[index]);
    }

    return rhd_write_reg_cmd(6U, IMP_DAC_MIDPOINT);
}

static uint16_t impedance_aux3_command(uint32_t index, uint32_t cap_range, uint32_t channel)
{
    if (index == 0U) {
        return rhd_write_reg_cmd(5U, impedance_reg5_value(cap_range));
    }

    if (index == 1U) {
        return rhd_write_reg_cmd(7U, channel & 0x3FU);
    }

    if (index == (IMP_TOTAL_FRAMES - 4U)) {
        return rhd_write_reg_cmd(5U, 0x00U);       /* Zcheck off. */
    }

    if (index == (IMP_TOTAL_FRAMES - 3U)) {
        return rhd_write_reg_cmd(7U, 0x00U);       /* Safe channel. */
    }

    if (index == (IMP_TOTAL_FRAMES - 2U)) {
        return rhd_read_reg_cmd(63U);              /* Expose Reg7 reply. */
    }

    return rhd_read_reg_cmd(63U);                  /* Even-frame dummy. */
}

static int program_impedance_aux_banks(uint32_t cap_range, uint32_t channel)
{
    uint16_t dummy = rhd_read_reg_cmd(63U);

    for (uint32_t index = 0U; index < IMP_TOTAL_FRAMES; ++index) {
        if (impedance_cancel_requested()) {
            return NCLP_IMPEDANCE_CANCELLED;
        }
        intan_aux_command_write(
            INTAN_AUX1_WINDOW_OFFSET, IMP_REG6_BANK, index,
            impedance_aux1_command(index % IMP_SINE_PERIOD_FRAMES));
        intan_aux_command_write(INTAN_AUX2_WINDOW_OFFSET, IMP_REG6_BANK,
                                index, dummy);
        intan_aux_command_write(
            INTAN_AUX3_WINDOW_OFFSET, IMP_REG57_BANK, index,
            impedance_aux3_command(index, cap_range, channel));
    }

    return impedance_cancel_requested() ? NCLP_IMPEDANCE_CANCELLED :
                                          NCLP_IMPEDANCE_OK;
}

static void stop_impedance_trial(void)
{
    stop_spi_safely();
}

static int configure_impedance_trial(uint32_t stream_mask,
                                     uint32_t packed_lane_phases_a,
                                     uint32_t packed_lane_phases_b)
{
    if (impedance_cancel_requested()) {
        return NCLP_IMPEDANCE_CANCELLED;
    }
    stop_impedance_trial();

    if (ddr_capture_prepare(SPI_CAPTURE_BLOCK_BYTES) != 0 ||
        write_lane_phases_verified(packed_lane_phases_a,
                                   packed_lane_phases_b) != 0) {
        return NCLP_IMPEDANCE_ERROR;
    }
    intan_register_write(INTAN_REG_AUX1_BANK_SELECT,
                         aux_bank_select_value(IMP_REG6_BANK));
    intan_register_write(INTAN_REG_AUX2_BANK_SELECT,
                         aux_bank_select_value(IMP_REG6_BANK));
    intan_register_write(INTAN_REG_AUX3_BANK_SELECT,
                         aux_bank_select_value(IMP_REG57_BANK));
    intan_register_write(INTAN_REG_AUX1_END_INDEX,
                         IMP_SINE_PERIOD_FRAMES - 1U);
    intan_register_write(INTAN_REG_AUX2_END_INDEX,
                         IMP_SINE_PERIOD_FRAMES - 1U);
    intan_register_write(INTAN_REG_AUX3_END_INDEX,
                         IMP_TOTAL_FRAMES - 1U);
    intan_register_write(INTAN_REG_AUX1_LOOP_INDEX, 0U);
    intan_register_write(INTAN_REG_AUX2_LOOP_INDEX, 0U);
    intan_register_write(INTAN_REG_AUX3_LOOP_INDEX, 0U);
    intan_register_write(INTAN_REG_FINITE_FRAME_COUNT, IMP_TOTAL_FRAMES);
    intan_register_write(INTAN_REG_ACQUISITION_CONFIG, 0U);
    intan_register_write(INTAN_REG_LOGICAL_STREAM_ENABLE, stream_mask);
    if (start_intan_capture() != 0) {
        return NCLP_IMPEDANCE_ERROR;
    }

    return impedance_cancel_requested() ? NCLP_IMPEDANCE_CANCELLED :
                                          NCLP_IMPEDANCE_OK;
}

static uint16_t impedance_channel_word(const uint16_t *frame_words, uint32_t channel)
{
    /* RHD2164 CONVERT(X) returns module A channel X on MISO A (SDR) and
     * module B channel X+32 on MISO B (DDR).  The compact frame stores those
     * as stream 0 channels 0..31 and stream 1 channels 32..63.
     */
    uint32_t stream_index = channel / 32U;
    uint32_t amplifier_slot = channel % 32U;

    return frame_word_for_stream_count(frame_words, amplifier_slot,
                                       stream_index,
                                       g_impedance_stream_count);
}

typedef struct {
    uint32_t frames;
    uint32_t timestamp_errors;
    uint32_t verify_errors;
    double magnitude_uv;
    double phase_deg;
} impedance_result_t;

static int capture_impedance_frames(uint32_t primary_stream, uint32_t cap_range,
                                    uint32_t channel, impedance_result_t *result)
{
    uint16_t word_buffer[SPI_FRAME_BUFFER_WORDS];
    uint32_t timestamps[IMP_TOTAL_FRAMES];
    timestamp_check_result_t ts_check;
    uint32_t frames_seen = 0U;
    uint32_t frame_words = frame_words_for_stream_count(g_impedance_stream_count);
    uint32_t best_order = 0U;
    uint32_t best_offset16 = 0U;
    uint32_t best_matches = 0U;
    double i_accum = 0.0;
    double q_accum = 0.0;
    uint32_t verify_errors = 0U;
    uint8_t expected_reg5 = impedance_reg5_value(cap_range);
    int capture_status;

    memset(result, 0, sizeof(*result));
    if (impedance_cancel_requested()) {
        return NCLP_IMPEDANCE_CANCELLED;
    }
    capture_status = ddr_capture_collect(SPI_CAPTURE_BLOCK_BYTES);
    if (capture_status == NCLP_CAPTURE_CANCELLED) {
        return NCLP_IMPEDANCE_CANCELLED;
    }
    if (capture_status != 0) {
        result->frames = 0U;
        result->timestamp_errors = 1U;
        result->verify_errors = 0U;
        result->magnitude_uv = 0.0;
        result->phase_deg = 0.0;
        return NCLP_IMPEDANCE_ERROR;
    }
    if (impedance_cancel_requested()) {
        return NCLP_IMPEDANCE_CANCELLED;
    }

    best_matches = find_best_capture_order_and_offset(g_impedance_stream_count,
                                                       &best_order,
                                                       &best_offset16);
    if (best_matches == 0U) {
        if (NCLP_SCAN_DEBUG != 0U) {
            print_capture_word_order_debug("impedance", nclp_capture.capture_valid_words32);
        }
        xil_printf("  FAIL impedance capture sync matches=0 bytes=%lu\r\n",
                   (unsigned long)nclp_capture.capture_valid_bytes);
        result->frames = 0U;
        result->timestamp_errors = 1U;
        result->verify_errors = 0U;
        result->magnitude_uv = 0.0;
        result->phase_deg = 0.0;
        return NCLP_IMPEDANCE_ERROR;
    }

    for (uint32_t frame = 0U; frame < IMP_TOTAL_FRAMES; ++frame) {
        uint32_t start16 = best_offset16 + (frame * frame_words);

        if (impedance_cancel_requested()) {
            return NCLP_IMPEDANCE_CANCELLED;
        }
        if ((start16 + frame_words) > (nclp_capture.capture_valid_words32 * 2U)) {
            break;
        }
        if (!magic_at_capture(nclp_capture.capture_ring_words, start16, best_order)) {
            continue;
        }

        for (uint32_t w = 0U; w < frame_words; ++w) {
            word_buffer[w] = get_capture16(nclp_capture.capture_ring_words, start16 + w, best_order);
        }
        timestamps[frames_seen] = frame_timestamp32(word_buffer);

        if (frames_seen == 0U) {
            uint16_t aux3_word = frame_word_for_stream_count(
                word_buffer, SPI_FRAME_AUX3_SLOT, 0U,
                g_impedance_stream_count);
            uint16_t expected = (uint16_t)(0xFF00U | expected_reg5);
            if (aux3_word != expected) {
                xil_printf("  MISO frame=0 Reg5 got=0x%04lx exp=0x%04lx\r\n",
                           (unsigned long)aux3_word,
                           (unsigned long)expected);
                verify_errors++;
            }
        } else if (frames_seen == 1U) {
            uint16_t aux3_word = frame_word_for_stream_count(
                word_buffer, SPI_FRAME_AUX3_SLOT, 0U,
                g_impedance_stream_count);
            uint16_t expected = (uint16_t)(0xFF00U | (channel & 0x3FU));
            if (aux3_word != expected) {
                xil_printf("  MISO frame=1 Reg7 got=0x%04lx exp=0x%04lx\r\n",
                           (unsigned long)aux3_word,
                           (unsigned long)expected);
                verify_errors++;
            }
        } else if (frames_seen == (IMP_TOTAL_FRAMES - 4U)) {
            uint16_t aux3_word = frame_word_for_stream_count(
                word_buffer, SPI_FRAME_AUX3_SLOT, 0U,
                g_impedance_stream_count);
            if (aux3_word != 0xFF00U) {
                xil_printf("  MISO cleanup Reg5-off frame=%lu got=0x%04lx exp=0xFF00 cmd_idx=%lu\r\n",
                           (unsigned long)frames_seen,
                           (unsigned long)aux3_word,
                           (unsigned long)(IMP_TOTAL_FRAMES - 4U));
                verify_errors++;
            }
        } else if (frames_seen == (IMP_TOTAL_FRAMES - 3U)) {
            uint16_t aux3_word = frame_word_for_stream_count(
                word_buffer, SPI_FRAME_AUX3_SLOT, 0U,
                g_impedance_stream_count);
            if (aux3_word != 0xFF00U) {
                xil_printf("  MISO cleanup Reg7-safe frame=%lu got=0x%04lx exp=0xFF00 cmd_idx=%lu\r\n",
                           (unsigned long)frames_seen,
                           (unsigned long)aux3_word,
                           (unsigned long)(IMP_TOTAL_FRAMES - 3U));
                verify_errors++;
            }
        } else if (frames_seen >= (IMP_TOTAL_FRAMES - 2U)) {
            uint16_t aux3_word = frame_word_for_stream_count(
                word_buffer, SPI_FRAME_AUX3_SLOT, 0U,
                g_impedance_stream_count);
            uint16_t expected = (uint16_t)nclp_headstage.best_stream_chip_id[primary_stream];
            if ((aux3_word & 0x00FFU) != expected) {
                xil_printf("  MISO cleanup READ63 frame=%lu got=0x%04lx exp_low=0x%02lx cmd_idx=%lu\r\n",
                           (unsigned long)frames_seen,
                           (unsigned long)aux3_word,
                           (unsigned long)expected,
                           (unsigned long)frames_seen);
                verify_errors++;
            }
        }

        if (frames_seen >= IMP_SETTLE_FRAMES && frames_seen < IMP_CAPTURE_FRAMES) {
            uint32_t sine_index = frames_seen % IMP_SINE_PERIOD_FRAMES;
            double sample_uv = IMP_ADC_UV_PER_COUNT *
                               ((double)((int32_t)impedance_channel_word(word_buffer, channel) - 32768));
            double angle = 2.0 * 3.14159265358979323846 *
                           (double)sine_index / (double)IMP_SINE_PERIOD_FRAMES;

            i_accum += sample_uv * cos(angle);
            q_accum += sample_uv * -sin(angle);
        }

        frames_seen++;
    }

    if (frames_seen != IMP_TOTAL_FRAMES) {
        xil_printf("  FAIL impedance timeout frames=%lu expected=%lu bytes=%lu\r\n",
                   (unsigned long)frames_seen,
                   (unsigned long)IMP_TOTAL_FRAMES,
                   (unsigned long)nclp_capture.capture_valid_bytes);
        result->frames = frames_seen;
        result->timestamp_errors = 1U;
        result->verify_errors = verify_errors;
        result->magnitude_uv = 0.0;
        result->phase_deg = 0.0;
        return NCLP_IMPEDANCE_ERROR;
    }

    check_timestamp_sequence(timestamps, frames_seen, &ts_check);
    result->frames = frames_seen;
    result->timestamp_errors = ts_check.errors;
    result->verify_errors = verify_errors;
    result->magnitude_uv = 2.0 * sqrt((i_accum * i_accum) + (q_accum * q_accum)) /
                           (double)IMP_MEASURE_FRAMES;
    result->phase_deg = atan2(q_accum, i_accum) * (180.0 / 3.14159265358979323846);
    return (ts_check.errors == 0U && verify_errors == 0U) ?
           NCLP_IMPEDANCE_OK : NCLP_IMPEDANCE_ERROR;
}

static double impedance_cap_farads(uint32_t cap_range)
{
    switch (cap_range) {
    case 0U:
        return 0.1e-12;
    case 1U:
        return 1.0e-12;
    default:
        return 10.0e-12;
    }
}

static void factor_out_parallel_capacitance(double *magnitude, double *phase_deg)
{
    double phase_rad = (*phase_deg) * (3.14159265358979323846 / 180.0);
    double measured_r = (*magnitude) * cos(phase_rad);
    double measured_x = (*magnitude) * sin(phase_rad);
    double cap_term = 2.0 * 3.14159265358979323846 * (double)IMP_TEST_FREQ_HZ * IMP_PARASITIC_CAP_F;
    double x_term = cap_term * ((measured_r * measured_r) + (measured_x * measured_x));
    double denominator = (cap_term * x_term) + (2.0 * cap_term * measured_x) + 1.0;
    double true_r = measured_r / denominator;
    double true_x = (measured_x + x_term) / denominator;

    *magnitude = sqrt((true_r * true_r) + (true_x * true_x));
    *phase_deg = atan2(true_x, true_r) * (180.0 / 3.14159265358979323846);
}

static void empirical_resistance_correction(double *magnitude, double *phase_deg)
{
    double phase_rad = (*phase_deg) * (3.14159265358979323846 / 180.0);
    double r = (*magnitude) * cos(phase_rad);
    double x = (*magnitude) * sin(phase_rad);

    r /= 10.0 * exp(-(double)IMP_SAMPLE_RATE_HZ / 2500.0) *
         cos(2.0 * 3.14159265358979323846 * (double)IMP_SAMPLE_RATE_HZ / 15000.0) + 1.0;

    *magnitude = sqrt((r * r) + (x * x));
    *phase_deg = atan2(x, r) * (180.0 / 3.14159265358979323846);
}

static void calculate_impedance(const impedance_result_t measured[IMP_CAP_RANGES],
                                double *impedance_ohms,
                                double *phase_deg,
                                uint32_t *best_cap)
{
    double best_distance = 1.0e99;
    uint32_t best = 0U;
    double measured_mag_uv;
    double current;
    double relative_freq = (double)IMP_TEST_FREQ_HZ / (double)IMP_SAMPLE_RATE_HZ;

    for (uint32_t cap = 0U; cap < IMP_CAP_RANGES; ++cap) {
        double mag = measured[cap].magnitude_uv;
        if (mag > 0.0) {
            double distance = fabs(log(mag / 250.0));
            if (distance < best_distance) {
                best_distance = distance;
                best = cap;
            }
        }
    }

    measured_mag_uv = measured[best].magnitude_uv;
    current = 2.0 * 3.14159265358979323846 * (double)IMP_TEST_FREQ_HZ *
              IMP_DAC_VOLTAGE_AMPLITUDE * impedance_cap_farads(best);

    if (current <= 0.0 || measured_mag_uv <= 0.0) {
        *impedance_ohms = 0.0;
        *phase_deg = 0.0;
        *best_cap = best;
        return;
    }

    *impedance_ohms = 1.0e-6 * (measured_mag_uv / current) *
                      ((18.0 * relative_freq * relative_freq) + 1.0);
    *phase_deg = measured[best].phase_deg + (360.0 * (3.0 / (double)IMP_SINE_PERIOD_FRAMES));

    factor_out_parallel_capacitance(impedance_ohms, phase_deg);
    empirical_resistance_correction(impedance_ohms, phase_deg);
    *best_cap = best;
}

static int store_impedance_channel_result(
    uint32_t stream, uint32_t global_channel_base, uint32_t channel,
    const impedance_result_t measured[IMP_CAP_RANGES])
{
    nclp_result_record_t record = {0};
    double impedance_ohms;
    double phase_deg;
    uint32_t best_cap;
    uint32_t timestamp_error = 0U;
    uint32_t verify_error = 0U;
    uint32_t saturated = 0U;
    uint64_t magnitude_milliohms;
    int64_t phase_microdegrees;

    calculate_impedance(measured, &impedance_ohms, &phase_deg, &best_cap);
    for (uint32_t cap = 0U; cap < IMP_CAP_RANGES; ++cap) {
        timestamp_error |= measured[cap].timestamp_errors != 0U;
        verify_error |= measured[cap].verify_errors != 0U;
    }
    if (impedance_ohms <= 0.0) {
        magnitude_milliohms = 0U;
    } else if (impedance_ohms >= 18446744073709548.0) {
        magnitude_milliohms = UINT64_MAX;
        saturated = 1U;
    } else {
        magnitude_milliohms = (uint64_t)(impedance_ohms * 1000.0 + 0.5);
    }
    phase_microdegrees = (int64_t)(phase_deg * 1000000.0 +
        ((phase_deg >= 0.0) ? 0.5 : -0.5));
    if (phase_microdegrees > INT32_MAX) {
        phase_microdegrees = INT32_MAX;
        saturated = 1U;
    } else if (phase_microdegrees < INT32_MIN) {
        phase_microdegrees = INT32_MIN;
        saturated = 1U;
    }

    record.impedance.info = ((global_channel_base + channel) & 0x1FFU) |
                            ((channel & 0x3FU) << 9) |
                            ((stream & 0x7U) << 15) |
                            ((best_cap & 0x3U) << 18) |
                            (1U << 20) |
                            ((timestamp_error & 0x1U) << 21) |
                            ((verify_error & 0x1U) << 22) |
                            ((saturated & 0x1U) << 23);
    record.impedance.magnitude_milliohms_low =
        (uint32_t)magnitude_milliohms;
    record.impedance.magnitude_milliohms_high =
        (uint32_t)(magnitude_milliohms >> 32);
    record.impedance.phase_microdegrees = (int32_t)phase_microdegrees;
    return nclp_result_write_record(NCLP_RESULT_TYPE_IMPEDANCE,
                                    global_channel_base + channel,
                                    &record);
}

static int prepare_impedance_data_path(void)
{
    DEBUG_PRINT("  INFO impedance data path = Intan AXIS -> DDR capture ring\r\n");
    return ddr_capture_prepare(SPI_CAPTURE_BLOCK_BYTES);
}

static uint16_t impedance_cleanup_aux1_command(uint32_t index)
{
    return index == 0U ? rhd_write_reg_cmd(6U, IMP_DAC_MIDPOINT) :
                         rhd_read_reg_cmd(63U);
}

static uint16_t impedance_cleanup_aux3_command(uint32_t index)
{
    switch (index) {
    case 0U:
        return rhd_write_reg_cmd(5U, 0U);
    case 1U:
        return rhd_write_reg_cmd(7U, 0U);
    case 2U:
        return rhd_read_reg_cmd(5U);
    case 3U:
        return rhd_read_reg_cmd(6U);
    case 4U:
        return rhd_read_reg_cmd(7U);
    default:
        return rhd_read_reg_cmd(63U);
    }
}

static void program_impedance_cleanup_banks(void)
{
    uint16_t dummy = rhd_read_reg_cmd(63U);

    for (uint32_t index = 0U; index < IMP_CLEANUP_VERIFY_FRAMES; ++index) {
        intan_aux_command_write(INTAN_AUX1_WINDOW_OFFSET,
                                IMP_REG6_BANK, index,
                                impedance_cleanup_aux1_command(index));
        intan_aux_command_write(INTAN_AUX2_WINDOW_OFFSET,
                                IMP_REG6_BANK, index, dummy);
        intan_aux_command_write(INTAN_AUX3_WINDOW_OFFSET,
                                IMP_REG57_BANK, index,
                                impedance_cleanup_aux3_command(index));
    }
}

static uint32_t configure_impedance_cleanup_trial(uint32_t stream_mask,
                                                  uint32_t packed_lane_phases_a,
                                                  uint32_t packed_lane_phases_b)
{
    uint32_t failures = 0U;
    uint32_t last_index = IMP_CLEANUP_VERIFY_FRAMES - 1U;

    stop_impedance_trial();
    failures += ddr_capture_prepare(SPI_CAPTURE_BLOCK_BYTES) != 0;
    failures += write_lane_phases_verified(packed_lane_phases_a,
                                           packed_lane_phases_b) != 0;
    intan_register_write(INTAN_REG_AUX1_BANK_SELECT,
                         aux_bank_select_value(IMP_REG6_BANK));
    intan_register_write(INTAN_REG_AUX2_BANK_SELECT,
                         aux_bank_select_value(IMP_REG6_BANK));
    intan_register_write(INTAN_REG_AUX3_BANK_SELECT,
                         aux_bank_select_value(IMP_REG57_BANK));
    intan_register_write(INTAN_REG_AUX1_END_INDEX, last_index);
    intan_register_write(INTAN_REG_AUX2_END_INDEX, last_index);
    intan_register_write(INTAN_REG_AUX3_END_INDEX, last_index);
    intan_register_write(INTAN_REG_AUX1_LOOP_INDEX, 0U);
    intan_register_write(INTAN_REG_AUX2_LOOP_INDEX, 0U);
    intan_register_write(INTAN_REG_AUX3_LOOP_INDEX, 0U);
    intan_register_write(INTAN_REG_FINITE_FRAME_COUNT,
                         IMP_CLEANUP_VERIFY_FRAMES);
    intan_register_write(INTAN_REG_ACQUISITION_CONFIG, 0U);
    intan_register_write(INTAN_REG_LOGICAL_STREAM_ENABLE, stream_mask);
    if (failures == 0U) {
        failures += start_intan_capture() != 0;
    }
    return failures;
}

static uint32_t capture_and_verify_impedance_cleanup(uint32_t physical_stream)
{
    uint16_t word_buffer[SPI_FRAME_BUFFER_WORDS];
    uint32_t frame_words = frame_words_for_stream_count(g_impedance_stream_count);
    uint32_t best_order = 0U;
    uint32_t best_offset16 = 0U;
    uint32_t best_matches;
    uint32_t frames_seen = 0U;
    uint32_t failures = 0U;

    if (ddr_capture_collect(SPI_CAPTURE_BLOCK_BYTES) != 0) {
        return 1U;
    }
    best_matches = find_best_capture_order_and_offset(g_impedance_stream_count,
                                                       &best_order,
                                                       &best_offset16);
    if (best_matches == 0U) {
        return 1U;
    }

    for (uint32_t frame = 0U; frame < IMP_CLEANUP_VERIFY_FRAMES; ++frame) {
        uint32_t start16 = best_offset16 + (frame * frame_words);

        if ((start16 + frame_words) > (nclp_capture.capture_valid_words32 * 2U)) {
            break;
        }
        if (!magic_at_capture(nclp_capture.capture_ring_words, start16, best_order)) {
            continue;
        }
        for (uint32_t word = 0U; word < frame_words; ++word) {
            word_buffer[word] = get_capture16(nclp_capture.capture_ring_words,
                                              start16 + word,
                                              best_order);
        }
        for (uint32_t logical = 0U; logical < g_impedance_stream_count;
             ++logical) {
            uint16_t aux1 = frame_word_for_stream_count(
                word_buffer, SPI_FRAME_AUX1_SLOT, logical,
                g_impedance_stream_count);
            uint16_t aux3 = frame_word_for_stream_count(
                word_buffer, SPI_FRAME_AUX3_SLOT, logical,
                g_impedance_stream_count);

            if (frames_seen == 0U) {
                failures += (aux1 & 0x00FFU) != IMP_DAC_MIDPOINT;
                failures += (aux3 & 0x00FFU) != 0U;
            } else if (frames_seen == 1U || frames_seen == 2U ||
                       frames_seen == 4U) {
                failures += (aux3 & 0x00FFU) != 0U;
            } else if (frames_seen == 3U) {
                failures += (aux3 & 0x00FFU) != IMP_DAC_MIDPOINT;
            } else if (frames_seen >= 5U && logical == 0U) {
                failures += (aux3 & 0x00FFU) !=
                            nclp_headstage.best_stream_chip_id[physical_stream];
            }
        }
        frames_seen++;
    }
    if (frames_seen != IMP_CLEANUP_VERIFY_FRAMES) {
        failures++;
    }
    return failures;
}

/* Always call this after an impedance operation.  It ignores a latched
 * cancellation request, stops the active trial, and verifies Reg5=0,
 * Reg6=128, Reg7=0 independently on every detected physical chip.
 * The return value is the accumulated number of cleanup failures.
 */
static uint32_t impedance_cleanup_all_detected_streams(void)
{
    uint32_t detected_mask = (uint32_t)selected_stream_mask();
    uint32_t failures = 0U;

    /* Cleanup capture must not be aborted by the cancellation it is
     * servicing.  The operation return code remains CANCELLED.
     */
    impedance_cancel_acknowledge();
    stop_impedance_trial();
    program_impedance_cleanup_banks();
    for (uint32_t stream = 0U; stream < SPI_SCAN_STREAMS; ++stream) {
        uint32_t setup_failures;

        if ((detected_mask & (1U << stream)) == 0U) {
            continue;
        }
        g_impedance_stream_count =
            nclp_headstage.best_stream_chip_id[stream] == 4U ? 2U : 1U;
        setup_failures = configure_impedance_cleanup_trial(
            primary_stream_mask(stream), nclp_headstage.best_lane_phases_a,
            nclp_headstage.best_lane_phases_b);
        failures += setup_failures;
        if (setup_failures == 0U) {
            failures += capture_and_verify_impedance_cleanup(stream);
        }
        stop_impedance_trial();
    }
    if (failures != 0U) {
        xil_printf("  FAIL impedance cleanup errors=%lu\r\n",
                   (unsigned long)failures);
    } else {
        DEBUG_PRINT("  PASS impedance cleanup Reg5=0 Reg6=128 Reg7=0\r\n");
    }
    return failures;
}

static int run_impedance_test_stream(uint32_t stream, uint32_t global_channel_base)
{
    uint32_t stream_mask;
    uint8_t chip_id;
    uint32_t channels;
    uint32_t detected_mask = (uint32_t)selected_stream_mask();

    if (stream >= SPI_SCAN_STREAMS || (detected_mask & (1U << stream)) == 0U) {
        xil_printf("\r\n[7] Skip impedance: stream %lu is not a supported detected chip\r\n",
                   (unsigned long)stream);
        return NCLP_IMPEDANCE_ERROR;
    }

    chip_id = nclp_headstage.best_stream_chip_id[stream];
    channels = chip_channel_count(chip_id);
    stream_mask = primary_stream_mask(stream);
    g_impedance_stream_count = (chip_id == 4U) ? 2U : 1U;

    xil_printf("\r\n[7] Impedance test on %s: local=0..%lu global=%lu..%lu, streams=%lu mask=0x%04lx, %lu frames/trial\r\n",
               nclp_stream_names[stream],
               (unsigned long)(channels - 1U),
               (unsigned long)global_channel_base,
               (unsigned long)(global_channel_base + channels - 1U),
               (unsigned long)g_impedance_stream_count,
               (unsigned long)stream_mask,
               (unsigned long)IMP_TOTAL_FRAMES);

    if (prepare_impedance_data_path() != 0) {
        return NCLP_IMPEDANCE_ERROR;
    }

    DEBUG_PRINT("  INFO 30kS/s 1kHz: sine=%lu frames, settle=%lu, measure=%lu, cleanup=%lu\r\n",
                (unsigned long)IMP_SINE_PERIOD_FRAMES,
                (unsigned long)IMP_SETTLE_FRAMES,
                (unsigned long)IMP_MEASURE_FRAMES,
                (unsigned long)IMP_CLEANUP_FRAMES);
    DEBUG_PRINT("  INFO Aux1 Reg6 sine bank=%lu loop=0..%lu; Aux3 Reg5/7 bank=%lu end=0..%lu\r\n",
                (unsigned long)IMP_REG6_BANK,
                (unsigned long)(IMP_SINE_PERIOD_FRAMES - 1U),
                (unsigned long)IMP_REG57_BANK,
                (unsigned long)(IMP_TOTAL_FRAMES - 1U));

    for (uint32_t ch = 0U; ch < channels; ++ch) {
        impedance_result_t cap_results[IMP_CAP_RANGES];

        if (impedance_cancel_requested()) {
            return NCLP_IMPEDANCE_CANCELLED;
        }
        for (uint32_t cap = 0U; cap < IMP_CAP_RANGES; ++cap) {
            impedance_result_t result;
            int status;

            DEBUG_PRINT("  START %s cap=%s local=%lu global=%lu\r\n",
                        nclp_stream_names[stream],
                        impedance_cap_name(cap),
                        (unsigned long)ch,
                        (unsigned long)(global_channel_base + ch));

            status = program_impedance_aux_banks(cap, ch);
            if (status != NCLP_IMPEDANCE_OK) {
                return status;
            }

            status = configure_impedance_trial(stream_mask,
                                               nclp_headstage.best_lane_phases_a,
                                               nclp_headstage.best_lane_phases_b);
            if (status == NCLP_IMPEDANCE_CANCELLED) {
                stop_impedance_trial();
                return status;
            }
            if (status != NCLP_IMPEDANCE_OK) {
                xil_printf("  FAIL impedance setup cap=%s ch=%lu\r\n",
                           impedance_cap_name(cap),
                           (unsigned long)ch);
                nclp_diagnostics.fail_count++;
                return NCLP_IMPEDANCE_ERROR;
            }

            status = capture_impedance_frames(stream, cap, ch, &result);
            stop_impedance_trial();
            if (status == NCLP_IMPEDANCE_CANCELLED) {
                return status;
            }
            if (status != NCLP_IMPEDANCE_OK) {
                xil_printf("  FAIL impedance capture cap=%s ch=%lu ts_errors=%lu verify_errors=%lu\r\n",
                           impedance_cap_name(cap),
                           (unsigned long)ch,
                           (unsigned long)result.timestamp_errors,
                           (unsigned long)result.verify_errors);
                nclp_diagnostics.fail_count++;
                return NCLP_IMPEDANCE_ERROR;
            }

            cap_results[cap] = result;
            nclp_fan_service();
            nclp_progress_advance(stream, ch, cap, 0U, nclp_diagnostics.fail_count);
            DEBUG_PRINT("  IMP %s cap=%s local=%lu global=%lu frames=%lu mag_uV=%lu phase_mdeg=%ld\r\n",
                        nclp_stream_names[stream],
                        impedance_cap_name(cap),
                        (unsigned long)ch,
                        (unsigned long)(global_channel_base + ch),
                        (unsigned long)result.frames,
                        (unsigned long)result.magnitude_uv,
                        (long)(result.phase_deg * 1000.0));
        }
        if (store_impedance_channel_result(stream, global_channel_base, ch,
                                           cap_results) != 0) {
            xil_printf("  FAIL impedance result storage on %s channel=%lu\r\n",
                       nclp_stream_names[stream], (unsigned long)ch);
            return NCLP_IMPEDANCE_ERROR;
        }
        if (impedance_cancel_requested()) {
            return NCLP_IMPEDANCE_CANCELLED;
        }
    }

    xil_printf("  PASS impedance complete      %s channels=%lu\r\n",
               nclp_stream_names[stream], (unsigned long)channels);
    return NCLP_IMPEDANCE_OK;
}

int run_impedance_test_all(void)
{
    uint32_t detected_mask = (uint32_t)selected_stream_mask();
    uint32_t global_channel_base = 0U;
    uint32_t cleanup_failures;
    int status = NCLP_IMPEDANCE_OK;

    if (detected_mask == 0U) {
        xil_printf("\r\n[7] Skip impedance: no detected stream\r\n");
        return NCLP_IMPEDANCE_ERROR;
    }

    xil_printf("\r\n[7] Impedance chip order and global channel map\r\n");
    for (uint32_t stream = 0U; stream < SPI_SCAN_STREAMS; ++stream) {
        if ((detected_mask & (1U << stream)) != 0U) {
            uint32_t channels = chip_channel_count(nclp_headstage.best_stream_chip_id[stream]);
            xil_printf("  %s: global %lu..%lu (%lu channels)\r\n",
                       nclp_stream_names[stream],
                       (unsigned long)global_channel_base,
                       (unsigned long)(global_channel_base + channels - 1U),
                       (unsigned long)channels);
            global_channel_base += channels;
        }
    }

    global_channel_base = 0U;
    for (uint32_t stream = 0U; stream < SPI_SCAN_STREAMS; ++stream) {
        if ((detected_mask & (1U << stream)) != 0U) {
            uint32_t channels = chip_channel_count(nclp_headstage.best_stream_chip_id[stream]);
            status = run_impedance_test_stream(stream, global_channel_base);
            if (status != NCLP_IMPEDANCE_OK) {
                break;
            }
            global_channel_base += channels;
        }
    }

    cleanup_failures = impedance_cleanup_all_detected_streams();
    if (cleanup_failures != 0U) {
        nclp_diagnostics.fail_count += cleanup_failures;
        status = NCLP_IMPEDANCE_ERROR;
    }
    if (status == NCLP_IMPEDANCE_CANCELLED) {
        xil_printf("\r\n  CANCELLED impedance complete_channels=%lu\r\n",
                   (unsigned long)nclp_result_staged_count(
                       NCLP_RESULT_TYPE_IMPEDANCE));
    } else if (status == NCLP_IMPEDANCE_OK) {
        xil_printf("\r\n  PASS all-chip impedance test channels=0..%lu\r\n",
                   (unsigned long)(global_channel_base - 1U));
    }
    return status;
}
