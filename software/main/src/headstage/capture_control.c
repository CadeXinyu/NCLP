#include "headstage_internal.h"

static int ddr_capture_start(uint32_t block_bytes);

nclp_capture_context_t nclp_capture = {
    .ddr_capture_armed = 0U,
    .ddr_capture_active = 0U,
    .capture_valid_bytes = 0U,
    .capture_valid_words32 = 0U,
    .capture_block_bytes = SPI_CAPTURE_BLOCK_BYTES,
    .capture_ring_bytes = SPI_DDR_RING_BYTES,
    .capture_low_half_first = 0U,
};

uint16_t get_capture16(const uint32_t *words32, uint32_t index16, uint32_t low_half_first)
{
    uint32_t word = words32[index16 >> 1];

    if (low_half_first != 0U) {
        return (index16 & 1U) ? (uint16_t)(word >> 16) : (uint16_t)(word & 0xFFFFU);
    }

    return (index16 & 1U) ? (uint16_t)(word & 0xFFFFU) : (uint16_t)(word >> 16);
}

int magic_at_capture(const uint32_t *words32, uint32_t index16, uint32_t low_half_first)
{
    return (get_capture16(words32, index16 + 0U, low_half_first) == 0x1942U) &&
           (get_capture16(words32, index16 + 1U, low_half_first) == 0x2702U) &&
           (get_capture16(words32, index16 + 2U, low_half_first) == 0x1999U) &&
           (get_capture16(words32, index16 + 3U, low_half_first) == 0xC691U);
}

static uint32_t timestamp_at_capture(const uint32_t *words32, uint32_t index16, uint32_t low_half_first)
{
    uint32_t ts_lo = get_capture16(words32, index16 + 4U, low_half_first);
    uint32_t ts_hi = get_capture16(words32, index16 + 5U, low_half_first);

    return ts_lo | (ts_hi << 16);
}

void print_capture_word_order_debug(const char *label, uint32_t max_words32)
{
    uint32_t probe_words32 = (max_words32 > 8U) ? 8U : max_words32;
    uint32_t produced = reg_read(SPI_DDR_WRITER_BASE, DDRW_REG_PRODUCED_BLOCK_COUNT);
    uint32_t outstanding = reg_read(SPI_DDR_WRITER_BASE, DDRW_REG_OUTSTANDING_BLOCK_COUNT);
    uint32_t last_len = reg_read(SPI_DDR_WRITER_BASE, DDRW_REG_LAST_PRODUCED_BLOCK_SIZE_BYTES);
    uint32_t status = reg_read(SPI_DDR_WRITER_BASE, DDRW_REG_STATUS);
    uint32_t error = reg_read(SPI_DDR_WRITER_BASE, DDRW_REG_ERROR_STATUS);

    xil_printf("      INFO %s valid_words32=%lu valid_bytes=%lu order_guess=%lu\r\n",
               label,
               (unsigned long)probe_words32,
               (unsigned long)nclp_capture.capture_valid_bytes,
               (unsigned long)nclp_capture.capture_low_half_first);
    xil_printf("      INFO DDRW status=0x%08lx error=0x%08lx produced=%lu outstanding=%lu last_len=%lu\r\n",
               (unsigned long)status,
               (unsigned long)error,
               (unsigned long)produced,
               (unsigned long)outstanding,
               (unsigned long)last_len);
    for (uint32_t i = 0U; i < probe_words32; ++i) {
        xil_printf("      INFO cap32[%lu]=0x%08lx\r\n",
                   (unsigned long)i,
                   (unsigned long)nclp_capture.capture_ring_words[i]);
    }

    for (uint32_t order = 0U; order < 2U; ++order) {
        xil_printf("      INFO order=%lu magic=%lu ts=%lu words16=%04x %04x %04x %04x %04x %04x\r\n",
                   (unsigned long)order,
                   (unsigned long)magic_at_capture(nclp_capture.capture_ring_words, 0U, order),
                   (unsigned long)timestamp_at_capture(nclp_capture.capture_ring_words, 0U, order),
                   (unsigned int)get_capture16(nclp_capture.capture_ring_words, 0U, order),
                   (unsigned int)get_capture16(nclp_capture.capture_ring_words, 1U, order),
                   (unsigned int)get_capture16(nclp_capture.capture_ring_words, 2U, order),
                   (unsigned int)get_capture16(nclp_capture.capture_ring_words, 3U, order),
                   (unsigned int)get_capture16(nclp_capture.capture_ring_words, 4U, order),
                   (unsigned int)get_capture16(nclp_capture.capture_ring_words, 5U, order));
    }
}

uint32_t find_best_capture_order_and_offset(uint32_t stream_count,
                                                   uint32_t *best_order_out,
                                                   uint32_t *best_offset16_out)
{
    uint32_t words32_count = nclp_capture.capture_valid_words32;
    uint32_t frame_words16 = frame_words_for_stream_count(stream_count);
    uint32_t best_matches = 0U;
    uint32_t best_order = 0U;
    uint32_t best_offset16 = 0U;

    for (uint32_t order = 0U; order < 2U; ++order) {
        for (uint32_t offset16 = 0U; offset16 < frame_words16; ++offset16) {
            uint32_t matches = 0U;

            for (uint32_t frame = 0U; ; ++frame) {
                uint32_t index16 = offset16 + (frame * frame_words16);
                if ((index16 + frame_words16) > (words32_count * 2U)) {
                    break;
                }
                if (magic_at_capture(nclp_capture.capture_ring_words, index16, order)) {
                    matches++;
                }
            }

            if (matches > best_matches) {
                best_matches = matches;
                best_order = order;
                best_offset16 = offset16;
            }
        }
    }

    if (best_order_out != 0) {
        *best_order_out = best_order;
    }
    if (best_offset16_out != 0) {
        *best_offset16_out = best_offset16;
    }
    return best_matches;
}

uint32_t reg_read(uintptr_t base, uint32_t offset)
{
    return Xil_In32((UINTPTR)(base + offset));
}

void reg_write(uintptr_t base, uint32_t offset, uint32_t value)
{
    Xil_Out32((UINTPTR)(base + offset), value);
}

uint16_t rhd_convert_cmd(uint32_t channel)
{
    return (uint16_t)((channel & 0x3FU) << 8);
}

uint16_t rhd_read_reg_cmd(uint32_t reg)
{
    return (uint16_t)(0xC000U | ((reg & 0x3FU) << 8));
}

uint16_t rhd_write_reg_cmd(uint32_t reg, uint32_t value)
{
    return (uint16_t)(0x8000U | ((reg & 0x3FU) << 8) | (value & 0xFFU));
}

uint16_t rhd_calibrate_cmd(void)
{
    return 0x5500U;
}

uint16_t rhd_clear_cal_cmd(void)
{
    return 0x6A00U;
}

void intan_register_write(uint32_t reg_offset, uint32_t value)
{
    reg_write(INTAN_BASE, reg_offset, value);
}

int start_intan_capture(void)
{
    uint32_t status = 0U;
    uint32_t error_status = 0U;
    uint32_t end_markers_before;
    uint32_t end_markers_after;
    uint32_t saw_pending = 0U;

    if (nclp_capture.ddr_capture_armed == 0U) {
        xil_printf("  FAIL Intan START without armed DDR capture\r\n");
        nclp_diagnostics.fail_count++;
        if (nclp_capture.ddr_capture_active != 0U) {
            (void)ddrw_abort_active_session("DDRW duplicate Intan START abort");
            nclp_capture.ddr_capture_active = 0U;
        }
        return -1;
    }
    if (ddr_capture_start(nclp_capture.capture_block_bytes) != 0) {
        (void)ddrw_abort_active_session("DDRW START failure quiesce");
        nclp_capture.ddr_capture_armed = 0U;
        nclp_capture.ddr_capture_active = 0U;
        nclp_capture.capture_valid_bytes = 0U;
        nclp_capture.capture_valid_words32 = 0U;
        return -1;
    }

    /* DDR preparation clears the previous session diagnostics.  Treat any
     * error accumulated while programming this session as a real setup
     * failure instead of erasing it immediately before START. */
    error_status = reg_read(INTAN_BASE, INTAN_REG_ERROR_STATUS) &
                   INTAN_ERROR_IMPLEMENTED_MASK;
    if (error_status != 0U) {
        xil_printf("  FAIL Intan pre-START error_status=0x%08lx\r\n",
                   (unsigned long)error_status);
        nclp_diagnostics.fail_count++;
        (void)ddrw_abort_active_session("DDRW Intan pre-START error abort");
        nclp_capture.ddr_capture_active = 0U;
        return -1;
    }

    end_markers_before = reg_read(
        INTAN_BASE, INTAN_REG_RECORDING_END_OF_SESSION_MARKER_COUNT);
    reg_write(INTAN_BASE, INTAN_REG_ERROR_STATUS,
              INTAN_ERROR_START_REJECTED |
              INTAN_ERROR_CONTROL_WRITE_REJECTED);
    intan_register_write(INTAN_REG_ACQUISITION_COMMAND,
                         INTAN_ACQUISITION_COMMAND_START);

    for (uint32_t elapsed = 0U; elapsed < INTAN_START_TIMEOUT_US;
         elapsed += INTAN_START_POLL_INTERVAL_US) {
        status = reg_read(INTAN_BASE, INTAN_REG_ACQUISITION_STATUS);
        error_status = reg_read(INTAN_BASE, INTAN_REG_ERROR_STATUS) &
                       INTAN_ERROR_IMPLEMENTED_MASK;
        end_markers_after = reg_read(
            INTAN_BASE, INTAN_REG_RECORDING_END_OF_SESSION_MARKER_COUNT);

        if ((status & INTAN_ACQUISITION_STATUS_START_REJECTED) != 0U ||
            error_status != 0U) {
            break;
        }
        if ((status & INTAN_ACQUISITION_STATUS_RUNNING) != 0U) {
            return 0;
        }
        if ((status & INTAN_ACQUISITION_STATUS_START_PENDING) != 0U) {
            saw_pending = 1U;
        } else if (saw_pending != 0U ||
                   end_markers_after != end_markers_before) {
            /* A very short finite session can complete between AXI-Lite
             * polls.  A completed pending handshake or a new end marker is
             * still positive evidence that START was accepted. */
            return 0;
        }
        usleep(INTAN_START_POLL_INTERVAL_US);
    }

    xil_printf("  FAIL Intan START status=0x%08lx error_status=0x%08lx pending_seen=%lu end_markers=%lu->%lu\r\n",
               (unsigned long)status,
               (unsigned long)error_status,
               (unsigned long)saw_pending,
               (unsigned long)end_markers_before,
               (unsigned long)reg_read(
                   INTAN_BASE, INTAN_REG_RECORDING_END_OF_SESSION_MARKER_COUNT));
    nclp_diagnostics.fail_count++;
    (void)ddrw_abort_active_session("DDRW Intan START failure abort");
    nclp_capture.ddr_capture_active = 0U;
    return -1;
}

int intan_reg_read(uint32_t reg_offset, uint32_t *value)
{
    if (value == NULL) {
        return -1;
    }
    *value = reg_read(INTAN_BASE, reg_offset);
    return 0;
}

void intan_aux_command_write(uint32_t window_offset, uint32_t bank,
                                    uint32_t index, uint32_t value)
{
    reg_write(INTAN_BASE,
              INTAN_AUX_COMMAND_OFFSET(window_offset, bank, index),
              value & 0xFFFFU);
}

void stop_spi_safely(void)
{
    /* Clearing CONTINUOUS requests the ordered frame-boundary stop. */
    intan_register_write(INTAN_REG_ACQUISITION_CONFIG, 0U);
    usleep(1000U);
    intan_register_write(INTAN_REG_FINITE_FRAME_COUNT, 0U);
    intan_register_write(INTAN_REG_LOGICAL_STREAM_ENABLE, 0U);
}

static int enable_intan_output(uint32_t output_config)
{
    uint32_t error_status;

    reg_write(INTAN_BASE, INTAN_REG_DIAGNOSTIC_COMMAND,
              INTAN_DIAGNOSTIC_COMMAND_CLEAR);
    reg_write(INTAN_BASE, INTAN_REG_ERROR_STATUS,
              INTAN_ERROR_IMPLEMENTED_MASK);
    error_status = reg_read(INTAN_BASE, INTAN_REG_ERROR_STATUS) &
                   INTAN_ERROR_IMPLEMENTED_MASK;
    if (error_status != 0U) {
        xil_printf("  FAIL Intan diagnostic reset error_status=0x%08lx\r\n",
                   (unsigned long)error_status);
        nclp_diagnostics.fail_count++;
        return -1;
    }
    reg_write(INTAN_BASE, INTAN_REG_OUTPUT_CONFIG,
              output_config);
    if (reg_read(INTAN_BASE, INTAN_REG_OUTPUT_CONFIG) != output_config) {
        nclp_diagnostics.fail_count++;
        return -1;
    }
    return 0;
}

int enable_intan_recording_output(void)
{
    return enable_intan_output(INTAN_OUTPUT_CONFIG_RECORDING_ENABLE);
}

int enable_intan_compute_output(void)
{
    return enable_intan_output(INTAN_OUTPUT_CONFIG_COMPUTE_ENABLE);
}

/* An SFP session has no PS recording sink and must never arm DDRW.
 * START acknowledgement comes from acquisition state or the SFP stream's
 * completed EOS for a finite session shorter than the software poll interval. */
int start_intan_sfp_capture(void)
{
    uint32_t status = 0U;
    uint32_t errors = 0U;
    uint32_t saw_pending = 0U;

    if (reg_read(INTAN_BASE, INTAN_REG_OUTPUT_CONFIG) !=
            INTAN_OUTPUT_CONFIG_COMPUTE_ENABLE ||
        (nclp_compute_fabric_status() &
         COMPUTE_FABRIC_STATUS_INTAN_EOS_SEEN) != 0U) {
        return -1;
    }
    errors = reg_read(INTAN_BASE, INTAN_REG_ERROR_STATUS) &
             INTAN_ERROR_IMPLEMENTED_MASK;
    if (errors != 0U) {
        return -1;
    }
    reg_write(INTAN_BASE, INTAN_REG_ERROR_STATUS,
              INTAN_ERROR_START_REJECTED | INTAN_ERROR_CONTROL_WRITE_REJECTED);
    intan_register_write(INTAN_REG_ACQUISITION_COMMAND,
                         INTAN_ACQUISITION_COMMAND_START);
    for (uint32_t elapsed = 0U; elapsed < INTAN_START_TIMEOUT_US;
         elapsed += INTAN_START_POLL_INTERVAL_US) {
        status = reg_read(INTAN_BASE, INTAN_REG_ACQUISITION_STATUS);
        errors = reg_read(INTAN_BASE, INTAN_REG_ERROR_STATUS) &
                 INTAN_ERROR_IMPLEMENTED_MASK;
        if ((status & INTAN_ACQUISITION_STATUS_START_REJECTED) != 0U ||
            errors != 0U) {
            break;
        }
        if ((status & INTAN_ACQUISITION_STATUS_RUNNING) != 0U) {
            return 0;
        }
        if ((status & INTAN_ACQUISITION_STATUS_START_PENDING) != 0U) {
            saw_pending = 1U;
        } else if (saw_pending != 0U ||
                   (nclp_compute_fabric_status() &
                    COMPUTE_FABRIC_STATUS_INTAN_EOS_SEEN) != 0U) {
            return 0;
        }
        usleep(INTAN_START_POLL_INTERVAL_US);
    }
    xil_printf("  FAIL SFP stream START status=0x%08lx error=0x%08lx\r\n",
               (unsigned long)status, (unsigned long)errors);
    nclp_diagnostics.fail_count++;
    return -1;
}

int wait_spi_stopped(uint32_t timeout_loops)
{
    uint32_t running = 0U;

    for (uint32_t i = 0U; i < timeout_loops; ++i) {
        if (intan_reg_read(INTAN_REG_ACQUISITION_STATUS, &running) != 0) {
            return -1;
        }
        if ((running & INTAN_ACQUISITION_STATUS_RUNNING) == 0U) {
            return 0;
        }
        usleep(10U);
    }

    xil_printf("  FAIL SPI stop timeout running=0x%08lx\r\n", (unsigned long)running);
    nclp_diagnostics.fail_count++;
    return -1;
}

int ddrw_wait_for_state(uint32_t expected_state,
                               uint32_t required_status,
                               uint32_t forbidden_status,
                               uint32_t timeout_us,
                               const char *label)
{
    uint32_t state = 0U;
    uint32_t status = 0U;

    for (uint32_t elapsed = 0U; elapsed < timeout_us; elapsed += 10U) {
        state = reg_read(SPI_DDR_WRITER_BASE, DDRW_REG_SESSION_STATE);
        status = reg_read(SPI_DDR_WRITER_BASE, DDRW_REG_STATUS);
        if (state == expected_state &&
            (status & required_status) == required_status &&
            (status & forbidden_status) == 0U) {
            return 0;
        }
        usleep(10U);
    }

    xil_printf("  FAIL %s state=%lu expected=%lu status=0x%08lx error=0x%08lx\r\n",
               label,
               (unsigned long)state,
               (unsigned long)expected_state,
               (unsigned long)status,
               (unsigned long)reg_read(SPI_DDR_WRITER_BASE,
                                        DDRW_REG_ERROR_STATUS));
    nclp_diagnostics.fail_count++;
    return -1;
}

static uint64_t read_split_counter(uintptr_t base,
                                   uint32_t low_offset,
                                   uint32_t high_offset)
{
    uint32_t high_before;
    uint32_t high_after;
    uint32_t low;

    do {
        high_before = reg_read(base, high_offset);
        low = reg_read(base, low_offset);
        high_after = reg_read(base, high_offset);
    } while (high_before != high_after);

    return ((uint64_t)high_after << 32) | low;
}

static uint64_t ddrw_committed_bytes(void)
{
    return read_split_counter(SPI_DDR_WRITER_BASE,
                              DDRW_REG_DDR_COMMITTED_BYTE_COUNT_LO,
                              DDRW_REG_DDR_COMMITTED_BYTE_COUNT_HI);
}

static uint64_t intan_recording_payload_words(void)
{
    return read_split_counter(INTAN_BASE,
                              INTAN_REG_RECORDING_PAYLOAD_WORD_COUNT_LO,
                              INTAN_REG_RECORDING_PAYLOAD_WORD_COUNT_HI);
}

int ddrw_check_terminal(uint32_t block_bytes,
                               uint64_t *payload_bytes_out,
                               uint32_t *produced_out,
                               uint32_t *final_length_out,
                               uint32_t *transfer_error_out)
{
    uint32_t status;
    uint32_t status_after_ack;
    uint32_t error_flags;
    uint32_t input_protocol_error_count;
    uint32_t last_bresp;
    uint32_t produced;
    uint32_t last_length;
    uint32_t final_length;
    uint32_t stream_status;
    uint32_t intan_error_status;
    uint32_t end_of_session_markers;
    uint32_t unaccepted_source_events;
    uint32_t stall_cycles;
    uint32_t failed = 0U;
    uint64_t committed;
    uint64_t recording_payload_words;
    uint64_t expected_blocks;
    uint32_t expected_final_length;

    if (block_bytes == 0U) {
        return -1;
    }
    if (ddrw_wait_for_state(DDRW_STATE_DONE,
                            DDRW_STATUS_EOS_SEEN |
                            DDRW_STATUS_EOS_COMMITTED,
                            DDRW_STATUS_RUNNING |
                            DDRW_STATUS_DATA_PATH_BUSY |
                            DDRW_STATUS_FIFO_NONEMPTY |
                            DDRW_STATUS_ERROR_MASK,
                            (SPI_CAPTURE_WAIT_TIMEOUT_MS + 1000U) * 1000U,
                            "DDRW ordered EOS terminal") != 0) {
        if (transfer_error_out != NULL) {
            *transfer_error_out = reg_read(SPI_DDR_WRITER_BASE,
                                         DDRW_REG_STATUS);
        }
        return -1;
    }

    status = reg_read(SPI_DDR_WRITER_BASE, DDRW_REG_STATUS);
    error_flags = reg_read(SPI_DDR_WRITER_BASE, DDRW_REG_ERROR_STATUS);
    input_protocol_error_count = reg_read(
        SPI_DDR_WRITER_BASE, DDRW_REG_INPUT_PROTOCOL_ERROR_COUNT);
    last_bresp = reg_read(SPI_DDR_WRITER_BASE, DDRW_REG_LAST_AXI_BRESP) & 0x3U;
    produced = reg_read(SPI_DDR_WRITER_BASE,
                        DDRW_REG_PRODUCED_BLOCK_COUNT);
    last_length = reg_read(SPI_DDR_WRITER_BASE,
                           DDRW_REG_LAST_PRODUCED_BLOCK_SIZE_BYTES);
    final_length = reg_read(SPI_DDR_WRITER_BASE,
                            DDRW_REG_FINAL_BLOCK_SIZE_BYTES);
    committed = ddrw_committed_bytes();
    recording_payload_words = intan_recording_payload_words();
    stream_status = reg_read(INTAN_BASE, INTAN_REG_OUTPUT_STATUS);
    intan_error_status = reg_read(INTAN_BASE, INTAN_REG_ERROR_STATUS) &
                         INTAN_ERROR_IMPLEMENTED_MASK;
    end_of_session_markers = reg_read(
        INTAN_BASE, INTAN_REG_RECORDING_END_OF_SESSION_MARKER_COUNT);
    unaccepted_source_events = reg_read(
        INTAN_BASE, INTAN_REG_UNACCEPTED_SOURCE_EVENT_COUNT);
    stall_cycles = reg_read(INTAN_BASE,
                            INTAN_REG_RECORDING_STALL_CYCLE_COUNT);

    expected_blocks = (committed == 0U) ? 0U :
        ((committed + block_bytes - 1U) / block_bytes);
    expected_final_length = (committed == 0U) ? 0U :
        (uint32_t)(((committed - 1U) % block_bytes) + 1U);

    if ((status & (DDRW_STATUS_EOS_SEEN |
                   DDRW_STATUS_EOS_COMMITTED)) !=
        (DDRW_STATUS_EOS_SEEN |
         DDRW_STATUS_EOS_COMMITTED) ||
        (status & (DDRW_STATUS_RUNNING |
                   DDRW_STATUS_DATA_PATH_BUSY |
                   DDRW_STATUS_FIFO_NONEMPTY |
                   DDRW_STATUS_ERROR_MASK)) != 0U ||
        error_flags != 0U || input_protocol_error_count != 0U || last_bresp != 0U ||
        produced != (uint32_t)expected_blocks ||
        last_length != expected_final_length ||
        final_length != expected_final_length ||
        committed != (recording_payload_words * 2U) ||
        end_of_session_markers != 1U ||
        intan_error_status != 0U ||
        (stream_status & (INTAN_OUTPUT_STATUS_RECORDING_END_OF_SESSION_SEEN |
                          INTAN_OUTPUT_STATUS_RECORDING_ACTIVE |
                          INTAN_OUTPUT_STATUS_ERROR_MASK)) !=
            INTAN_OUTPUT_STATUS_RECORDING_END_OF_SESSION_SEEN ||
        unaccepted_source_events != 0U) {
        failed = 1U;
    }

    if ((status & DDRW_STATUS_IRQ_PENDING) == 0U) {
        failed = 1U;
    }
    reg_write(SPI_DDR_WRITER_BASE, DDRW_REG_COMMAND,
              DDRW_COMMAND_IRQ_ACK);
    status_after_ack = reg_read(SPI_DDR_WRITER_BASE, DDRW_REG_STATUS);
    if ((status_after_ack & DDRW_STATUS_IRQ_PENDING) != 0U ||
        (status_after_ack & (DDRW_STATUS_EOS_SEEN |
                             DDRW_STATUS_EOS_COMMITTED)) !=
            (DDRW_STATUS_EOS_SEEN | DDRW_STATUS_EOS_COMMITTED)) {
        failed = 1U;
    }

    if (payload_bytes_out != NULL) {
        *payload_bytes_out = committed;
    }
    if (produced_out != NULL) {
        *produced_out = produced;
    }
    if (final_length_out != NULL) {
        *final_length_out = final_length;
    }
    if (transfer_error_out != NULL) {
        if ((status & DDRW_STATUS_ERROR_MASK) != 0U ||
            error_flags != 0U || input_protocol_error_count != 0U || last_bresp != 0U) {
            *transfer_error_out = status;
        } else if (intan_error_status != 0U) {
            *transfer_error_out = 0x80000000U | intan_error_status;
        } else if ((stream_status & INTAN_OUTPUT_STATUS_ERROR_MASK) != 0U ||
                   unaccepted_source_events != 0U) {
            *transfer_error_out = 0x80000000U | stream_status;
        } else {
            *transfer_error_out = failed;
        }
    }

    if (failed != 0U) {
        xil_printf("  FAIL DDRW terminal contract status=0x%08lx error=0x%08lx input_protocol_errors=%lu bresp=%lu produced=%lu expected=%lu last=%lu final=%lu expected_final=%lu\r\n",
                   (unsigned long)status,
                   (unsigned long)error_flags,
                   (unsigned long)input_protocol_error_count,
                   (unsigned long)last_bresp,
                   (unsigned long)produced,
                   (unsigned long)((uint32_t)expected_blocks),
                   (unsigned long)last_length,
                   (unsigned long)final_length,
                   (unsigned long)expected_final_length);
        xil_printf("  FAIL SPI stream terminal status=0x%08lx error_status=0x%08lx end_markers=%lu unaccepted_source_events=%lu stall_cycles=%lu committed=%08lx%08lx payload_words=%08lx%08lx\r\n",
                   (unsigned long)stream_status,
                   (unsigned long)intan_error_status,
                   (unsigned long)end_of_session_markers,
                   (unsigned long)unaccepted_source_events,
                   (unsigned long)stall_cycles,
                   (unsigned long)(committed >> 32),
                   (unsigned long)committed,
                   (unsigned long)(recording_payload_words >> 32),
                   (unsigned long)recording_payload_words);
        nclp_diagnostics.fail_count++;
        return -1;
    }
    return 0;
}

int ddrw_abort_active_session(const char *label)
{
    uint32_t state = reg_read(SPI_DDR_WRITER_BASE,
                              DDRW_REG_SESSION_STATE);
    uint32_t status = reg_read(SPI_DDR_WRITER_BASE, DDRW_REG_STATUS);

    if (state == DDRW_STATE_CAPTURING || state == DDRW_STATE_FULL_WAIT ||
        state == DDRW_STATE_DRAINING) {
        reg_write(SPI_DDR_WRITER_BASE, DDRW_REG_COMMAND,
                  DDRW_COMMAND_ABORT);
        stop_spi_safely();
        (void)wait_spi_stopped(10000U);
        for (uint32_t elapsed = 0U; elapsed < 1000000U; elapsed += 10U) {
            state = reg_read(SPI_DDR_WRITER_BASE,
                             DDRW_REG_SESSION_STATE);
            status = reg_read(SPI_DDR_WRITER_BASE, DDRW_REG_STATUS);
            if (state == DDRW_STATE_ABORT_DONE &&
                (status & DDRW_STATUS_ABORT_DONE) != 0U &&
                (status & (DDRW_STATUS_RUNNING |
                           DDRW_STATUS_DATA_PATH_BUSY |
                           DDRW_STATUS_FIFO_NONEMPTY |
                           DDRW_STATUS_FAULT)) == 0U) {
                return 0;
            }
            /* A graceful EOS may win the AXI-Lite ABORT race.  DONE is an
             * equally safe terminal state and must not be turned into a
             * spurious abort timeout. */
            if (state == DDRW_STATE_DONE &&
                (status & (DDRW_STATUS_EOS_SEEN |
                           DDRW_STATUS_EOS_COMMITTED)) ==
                    (DDRW_STATUS_EOS_SEEN |
                     DDRW_STATUS_EOS_COMMITTED) &&
                (status & (DDRW_STATUS_RUNNING |
                           DDRW_STATUS_DATA_PATH_BUSY |
                           DDRW_STATUS_FIFO_NONEMPTY |
                           DDRW_STATUS_ERROR_MASK)) == 0U) {
                return 0;
            }
            usleep(10U);
        }
        xil_printf("  FAIL %s state=%lu status=0x%08lx error=0x%08lx\r\n",
                   label,
                   (unsigned long)state,
                   (unsigned long)status,
                   (unsigned long)reg_read(SPI_DDR_WRITER_BASE,
                                            DDRW_REG_ERROR_STATUS));
        nclp_diagnostics.fail_count++;
        return -1;
    }

    stop_spi_safely();
    (void)wait_spi_stopped(10000U);
    if (state == DDRW_STATE_FAULT_DRAIN) {
        return ddrw_wait_for_state(DDRW_STATE_FAULT,
                                   DDRW_STATUS_FAULT,
                                   DDRW_STATUS_RUNNING |
                                   DDRW_STATUS_DATA_PATH_BUSY |
                                   DDRW_STATUS_FIFO_NONEMPTY,
                                   1000000U, label);
    }
    if (state == DDRW_STATE_IDLE || state == DDRW_STATE_DONE ||
        state == DDRW_STATE_ABORT_DONE || state == DDRW_STATE_FAULT) {
        return 0;
    }

    xil_printf("  FAIL %s unexpected DDRW state=%lu status=0x%08lx\r\n",
               label, (unsigned long)state, (unsigned long)status);
    nclp_diagnostics.fail_count++;
    return -1;
}

int ddr_read_produced_snapshot(uint32_t *produced_out,
                                      uint32_t *last_len_out)
{
    uint32_t before = 0U;
    uint32_t after = 0U;
    uint32_t last_len = 0U;

    if (produced_out == NULL || last_len_out == NULL) {
        return -1;
    }
    for (uint32_t attempt = 0U; attempt < 1000U; ++attempt) {
        before = reg_read(SPI_DDR_WRITER_BASE,
                          DDRW_REG_PRODUCED_BLOCK_COUNT);
        last_len = reg_read(SPI_DDR_WRITER_BASE,
                            DDRW_REG_LAST_PRODUCED_BLOCK_SIZE_BYTES);
        after = reg_read(SPI_DDR_WRITER_BASE,
                         DDRW_REG_PRODUCED_BLOCK_COUNT);
        if (before == after) {
            *produced_out = after;
            *last_len_out = last_len;
            return 0;
        }
    }

    xil_printf("  FAIL DDR produced-count snapshot unstable before=%lu after=%lu\r\n",
               (unsigned long)before, (unsigned long)after);
    nclp_diagnostics.fail_count++;
    return -1;
}

static int ddr_collect_produced_blocks(uint32_t block_bytes,
                                       uint32_t max_collect_bytes,
                                       uint32_t *consumed_blocks,
                                       uint32_t *collect_bytes,
                                       uint32_t *collect_overflow,
                                       uint32_t *last_produced_snapshot)
{
    uint32_t active_ring_bytes;
    uint32_t produced;
    uint32_t last_len;

    if (block_bytes == 0U || consumed_blocks == NULL ||
        collect_bytes == NULL || collect_overflow == NULL ||
        last_produced_snapshot == NULL) {
        return -1;
    }
    active_ring_bytes = nclp_capture.capture_ring_bytes;
    if (active_ring_bytes == 0U ||
        (active_ring_bytes % block_bytes) != 0U) {
        return -1;
    }
    if (ddr_read_produced_snapshot(&produced, &last_len) != 0) {
        return -1;
    }

    *last_produced_snapshot = produced;
    while (*consumed_blocks < produced) {
        uint32_t capacity_blocks = active_ring_bytes / block_bytes;
        uint32_t block_offset =
            ((*consumed_blocks) % capacity_blocks) * block_bytes;
        uint32_t block_len = block_bytes;
        uint8_t *destination = (uint8_t *)nclp_capture.capture_collect_words;
        const uint8_t *source = (const uint8_t *)nclp_capture.capture_ring_words;

        if (((*consumed_blocks) + 1U) == produced && last_len != 0U && last_len < block_bytes) {
            block_len = last_len;
        }

        Xil_DCacheInvalidateRange((INTPTR)&source[block_offset], block_len);

        if ((*collect_bytes + block_len) > max_collect_bytes) {
            *collect_overflow = 1U;
            return 0;
        }

        for (uint32_t i = 0U; i < block_len; ++i) {
            destination[*collect_bytes + i] = source[block_offset + i];
        }
        *collect_bytes += block_len;

        (*consumed_blocks)++;
        reg_write(SPI_DDR_WRITER_BASE, DDRW_REG_CONSUMED_BLOCK_COUNT,
                  *consumed_blocks);
    }
    return 0;
}

int ddr_capture_prepare(uint32_t block_bytes)
{
    UINTPTR ring_addr = (UINTPTR)nclp_capture.capture_ring_words;
    uint32_t capacity_blocks;
    uint32_t buffer_bytes;
    uint32_t status;

    if (block_bytes == 0U || block_bytes > SPI_CAPTURE_BLOCK_BYTES) {
        xil_printf("  FAIL SPI DDR writer invalid block bytes=%lu\r\n", (unsigned long)block_bytes);
        nclp_diagnostics.fail_count++;
        return -1;
    }

    capacity_blocks = SPI_DDR_RING_BYTES / block_bytes;
    if (capacity_blocks == 0U) {
        capacity_blocks = 1U;
    }
    buffer_bytes = capacity_blocks * block_bytes;
    nclp_capture.capture_block_bytes = block_bytes;
    nclp_capture.capture_ring_bytes = buffer_bytes;

    if (ddrw_abort_active_session("DDRW prepare quiesce") != 0) {
        return -1;
    }
    reg_write(SPI_DDR_WRITER_BASE, DDRW_REG_COMMAND, DDRW_COMMAND_SOFT_RESET);
    if (ddrw_wait_for_state(DDRW_STATE_IDLE, 0U,
                            DDRW_STATUS_RUNNING |
                            DDRW_STATUS_DATA_PATH_BUSY |
                            DDRW_STATUS_ERROR_MASK,
                            100000U, "DDRW reset to IDLE") != 0) {
        return -1;
    }

    for (uint32_t i = 0U; i < SPI_DDR_RING_WORDS32; ++i) {
        nclp_capture.capture_ring_words[i] = 0xA5A50000U | (i & 0xFFFFU);
    }
    Xil_DCacheFlushRange((INTPTR)nclp_capture.capture_ring_words, SPI_DDR_RING_BYTES);

    reg_write(SPI_DDR_WRITER_BASE, DDRW_REG_RING_BASE_ADDR_LO, (uint32_t)ring_addr);
    reg_write(SPI_DDR_WRITER_BASE, DDRW_REG_RING_BASE_ADDR_HI, (uint32_t)(((uint64_t)ring_addr) >> 32));
    reg_write(SPI_DDR_WRITER_BASE, DDRW_REG_RING_SIZE_BYTES, buffer_bytes);
    reg_write(SPI_DDR_WRITER_BASE, DDRW_REG_BLOCK_SIZE_BYTES, block_bytes);
    reg_write(SPI_DDR_WRITER_BASE, DDRW_REG_RING_CAPACITY_BLOCKS, capacity_blocks);
    reg_write(SPI_DDR_WRITER_BASE, DDRW_REG_IRQ_COMPLETION_INTERVAL_BLOCKS,
              DDRW_IRQ_COMPLETION_INTERVAL_BLOCKS);
    /* ERROR_STATUS is W1C.  Configuration remains passive until the explicit
     * COMMAND.START pulse in ddr_capture_start(). */
    reg_write(SPI_DDR_WRITER_BASE, DDRW_REG_ERROR_STATUS,
              DDRW_ERROR_IMPLEMENTED_MASK);
    reg_write(SPI_DDR_WRITER_BASE, DDRW_REG_COMMAND,
              DDRW_COMMAND_IRQ_ACK);

    status = reg_read(SPI_DDR_WRITER_BASE, DDRW_REG_STATUS);
    if ((status & (DDRW_STATUS_RUNNING | DDRW_STATUS_ERROR_MASK)) != 0U ||
        reg_read(SPI_DDR_WRITER_BASE,
                 DDRW_REG_SESSION_STATE) != DDRW_STATE_IDLE) {
        xil_printf("  FAIL SPI DDR writer config status=0x%08lx error=0x%08lx\r\n",
                   (unsigned long)status,
                   (unsigned long)reg_read(SPI_DDR_WRITER_BASE, DDRW_REG_ERROR_STATUS));
        nclp_diagnostics.fail_count++;
        return -1;
    }

    DEBUG_PRINT("  INFO SPI DDR writer prepared buffer_bytes=%lu allocation_bytes=%lu block_bytes=%lu capacity_blocks=%lu\r\n",
                (unsigned long)buffer_bytes,
                (unsigned long)SPI_DDR_RING_BYTES,
                (unsigned long)block_bytes,
                (unsigned long)capacity_blocks);

    if (enable_intan_recording_output() != 0) {
        return -1;
    }

    nclp_capture.ddr_capture_armed = 1U;
    nclp_capture.ddr_capture_active = 0U;
    nclp_capture.capture_valid_bytes = 0U;
    nclp_capture.capture_valid_words32 = 0U;
    nclp_headstage.scan_capture_debug_printed = 0U;
    return 0;
}

static int ddr_capture_start(uint32_t block_bytes)
{
    uint32_t session_before;
    uint32_t session_after;
    uint32_t status;

    if (nclp_capture.ddr_capture_active != 0U) {
        return 0;
    }

    if (block_bytes == 0U || block_bytes > SPI_CAPTURE_BLOCK_BYTES) {
        xil_printf("  FAIL SPI DDR writer start block bytes=%lu\r\n", (unsigned long)block_bytes);
        nclp_diagnostics.fail_count++;
        return -1;
    }

    status = reg_read(SPI_DDR_WRITER_BASE, DDRW_REG_STATUS);
    if ((status & (DDRW_STATUS_RUNNING |
                   DDRW_STATUS_ERROR_MASK)) != 0U ||
        reg_read(SPI_DDR_WRITER_BASE,
                 DDRW_REG_SESSION_STATE) != DDRW_STATE_IDLE) {
        xil_printf("  FAIL SPI DDR writer not armed status=0x%08lx state=%lu\r\n",
                   (unsigned long)status,
                   (unsigned long)reg_read(SPI_DDR_WRITER_BASE,
                                            DDRW_REG_SESSION_STATE));
        nclp_diagnostics.fail_count++;
        return -1;
    }

    session_before = reg_read(SPI_DDR_WRITER_BASE, DDRW_REG_SESSION_ID);
    reg_write(SPI_DDR_WRITER_BASE, DDRW_REG_COMMAND,
              DDRW_COMMAND_START);
    if (ddrw_wait_for_state(DDRW_STATE_CAPTURING,
                            DDRW_STATUS_RUNNING,
                            DDRW_STATUS_ERROR_MASK,
                            100000U, "DDRW explicit START") != 0) {
        return -1;
    }
    session_after = reg_read(SPI_DDR_WRITER_BASE, DDRW_REG_SESSION_ID);
    if (session_after != (session_before + 1U)) {
        xil_printf("  FAIL DDRW session ID before=%lu after=%lu\r\n",
                   (unsigned long)session_before,
                   (unsigned long)session_after);
        nclp_diagnostics.fail_count++;
        return -1;
    }

    nclp_capture.ddr_capture_armed = 0U;
    nclp_capture.ddr_capture_active = 1U;
    nclp_capture.capture_valid_bytes = 0U;
    nclp_capture.capture_valid_words32 = 0U;
    return 0;
}

int ddr_capture_collect(uint32_t block_bytes)
{
    uint32_t consumed = 0U;
    uint32_t collect_bytes = 0U;
    uint32_t collect_overflow = 0U;
    uint32_t produced = 0U;
    uint32_t terminal_produced = 0U;
    uint32_t terminal_final_length = 0U;
    uint32_t terminal_transfer_error = 0U;
    uint32_t source_stopped = 0U;
    uint64_t terminal_payload_bytes = 0U;

    if (block_bytes != nclp_capture.capture_block_bytes) {
        xil_printf("  FAIL DDR wait block mismatch requested=%lu active=%lu\r\n",
                   (unsigned long)block_bytes,
                   (unsigned long)nclp_capture.capture_block_bytes);
        nclp_diagnostics.fail_count++;
        return -1;
    }

    for (uint32_t i = 0U; i < SPI_DDR_COLLECT_WORDS32; ++i) {
        nclp_capture.capture_collect_words[i] = 0U;
    }

    for (uint32_t loop = 0U; loop < SPI_CAPTURE_WAIT_TIMEOUT_MS; ++loop) {
        uint32_t running = 0U;

        if ((nclp_operation.operation_state == NCLP_STATE_IMPEDANCE ||
             nclp_operation.operation_state == NCLP_STATE_CANCELLING) &&
            nclp_impedance_cancel_requested() != 0) {
            if (ddrw_abort_active_session("DDRW impedance cancel") != 0) {
                return -1;
            }
            return NCLP_CAPTURE_CANCELLED;
        }

        if (ddr_collect_produced_blocks(nclp_capture.capture_block_bytes,
                                        SPI_DDR_COLLECT_BYTES,
                                        &consumed,
                                        &collect_bytes,
                                        &collect_overflow,
                                        &produced) != 0) {
            return -1;
        }
        if (collect_overflow != 0U) {
            break;
        }
        if (intan_reg_read(INTAN_REG_ACQUISITION_STATUS, &running) != 0) {
            return -1;
        }
        if ((running & INTAN_ACQUISITION_STATUS_RUNNING) == 0U) {
            source_stopped = 1U;
            break;
        }
        nclp_fan_service();
        usleep(1000U);
    }

    stop_spi_safely();
    (void)wait_spi_stopped(10000U);
    if (source_stopped == 0U) {
        xil_printf("  FAIL SPI capture timeout before finite session stopped\r\n");
        (void)ddrw_abort_active_session("DDRW capture timeout abort");
        nclp_diagnostics.fail_count++;
        return -1;
    }
    if (ddrw_check_terminal(nclp_capture.capture_block_bytes,
                            &terminal_payload_bytes,
                            &terminal_produced,
                            &terminal_final_length,
                            &terminal_transfer_error) != 0) {
        return -1;
    }
    if (terminal_payload_bytes > SPI_DDR_COLLECT_BYTES) {
        xil_printf("  FAIL DDR terminal payload exceeds collection buffer bytes=%08lx%08lx capacity=%lu\r\n",
                   (unsigned long)(terminal_payload_bytes >> 32),
                   (unsigned long)terminal_payload_bytes,
                   (unsigned long)SPI_DDR_COLLECT_BYTES);
        nclp_diagnostics.fail_count++;
        return -1;
    }
    if (ddr_collect_produced_blocks(nclp_capture.capture_block_bytes,
                                    SPI_DDR_COLLECT_BYTES,
                                    &consumed,
                                    &collect_bytes,
                                    &collect_overflow,
                                    &produced) != 0) {
        return -1;
    }

    if (collect_overflow != 0U) {
        xil_printf("  FAIL DDR collect overflow collected=%lu capacity=%lu\r\n",
                   (unsigned long)collect_bytes,
                   (unsigned long)SPI_DDR_COLLECT_BYTES);
        nclp_diagnostics.fail_count++;
        return -1;
    }

    if ((uint64_t)collect_bytes != terminal_payload_bytes ||
        produced != terminal_produced || consumed != terminal_produced ||
        reg_read(SPI_DDR_WRITER_BASE,
                 DDRW_REG_OUTSTANDING_BLOCK_COUNT) != 0U) {
        xil_printf("  FAIL DDR exact collection bytes=%lu expected=%08lx%08lx produced=%lu terminal=%lu consumed=%lu outstanding=%lu final=%lu transfer_error=0x%08lx\r\n",
                   (unsigned long)collect_bytes,
                   (unsigned long)(terminal_payload_bytes >> 32),
                   (unsigned long)terminal_payload_bytes,
                   (unsigned long)produced,
                   (unsigned long)terminal_produced,
                   (unsigned long)consumed,
                   (unsigned long)reg_read(SPI_DDR_WRITER_BASE,
                                            DDRW_REG_OUTSTANDING_BLOCK_COUNT),
                   (unsigned long)terminal_final_length,
                   (unsigned long)terminal_transfer_error);
        nclp_diagnostics.fail_count++;
        return -1;
    }

    if (collect_bytes == 0U) {
        xil_printf("  FAIL DDR collect captured no data status=0x%08lx produced=%lu outstanding=%lu\r\n",
                   (unsigned long)reg_read(SPI_DDR_WRITER_BASE, DDRW_REG_STATUS),
                   (unsigned long)reg_read(SPI_DDR_WRITER_BASE, DDRW_REG_PRODUCED_BLOCK_COUNT),
                   (unsigned long)reg_read(SPI_DDR_WRITER_BASE, DDRW_REG_OUTSTANDING_BLOCK_COUNT));
        nclp_diagnostics.fail_count++;
        return -1;
    }

    for (uint32_t i = 0U; i < ((collect_bytes + 3U) / 4U); ++i) {
        nclp_capture.capture_ring_words[i] = nclp_capture.capture_collect_words[i];
    }

    nclp_capture.capture_low_half_first = 0U;
    if (!magic_at_capture(nclp_capture.capture_ring_words, 0U, nclp_capture.capture_low_half_first)) {
        nclp_capture.capture_low_half_first = 1U;
    }

    nclp_capture.ddr_capture_active = 0U;
    nclp_capture.capture_valid_bytes = collect_bytes;
    nclp_capture.capture_valid_words32 = (collect_bytes + 3U) / 4U;

    DEBUG_PRINT("  INFO DDR collect bytes=%lu blocks_produced=%lu blocks_consumed=%lu write_offset=%lu\r\n",
                (unsigned long)collect_bytes,
                (unsigned long)produced,
                (unsigned long)consumed,
                (unsigned long)reg_read(SPI_DDR_WRITER_BASE, DDRW_REG_CURRENT_WRITE_OFFSET_BYTES));
    if ((NCLP_SCAN_DEBUG != 0U) && (nclp_headstage.scan_capture_debug_printed == 0U)) {
        print_capture_word_order_debug("scan-capture", (collect_bytes + 3U) / 4U);
        nclp_headstage.scan_capture_debug_printed = 1U;
    }
    return 0;
}

int reset_board_path(void)
{
    DEBUG_PRINT("\r\n[1] Stop and re-arm SPI/DDRW path\r\n");

    if (ddrw_abort_active_session("DDRW reset-path quiesce") != 0) {
        return -1;
    }
    reg_write(INTAN_BASE, INTAN_REG_DIAGNOSTIC_COMMAND,
              INTAN_DIAGNOSTIC_COMMAND_CLEAR);
    reg_write(INTAN_BASE, INTAN_REG_OUTPUT_CONFIG,
              INTAN_OUTPUT_CONFIG_RECORDING_ENABLE);
    /* START resets the SPI timestamp/AUX runtime state.  Keep the global PL
     * reset out of normal recovery so CDC reset release cannot synthesize an
     * AXIS payload between otherwise orderly capture sessions. */
    return ddr_capture_prepare(SPI_CAPTURE_BLOCK_BYTES);
}

int print_board_identity(void)
{
    uint32_t intan_block_id;
    uint32_t intan_abi_version;
    uint32_t intan_capabilities;
    uint32_t intan_info;
    uint32_t ddrw_block_id;
    uint32_t ddrw_abi_version;
    uint32_t ddrw_capabilities;
    uint32_t ddrw_info;
    uint32_t value = 0U;

    DEBUG_PRINT("\r\n[2] Read board identity\r\n");
    intan_block_id = reg_read(INTAN_BASE, INTAN_REG_BLOCK_ID);
    intan_abi_version = reg_read(INTAN_BASE, INTAN_REG_ABI_VERSION);
    intan_capabilities = reg_read(INTAN_BASE, INTAN_REG_CAPABILITIES);
    intan_info = reg_read(INTAN_BASE, INTAN_REG_INFO);
    ddrw_block_id = reg_read(SPI_DDR_WRITER_BASE, DDRW_REG_BLOCK_ID);
    ddrw_abi_version = reg_read(SPI_DDR_WRITER_BASE,
                                DDRW_REG_ABI_VERSION);
    ddrw_capabilities = reg_read(SPI_DDR_WRITER_BASE,
                                 DDRW_REG_CAPABILITIES);
    ddrw_info = reg_read(SPI_DDR_WRITER_BASE, DDRW_REG_INFO);
    DEBUG_PRINT("  INFO Intan id=0x%08lx ABI=0x%08lx info=0x%08lx caps=0x%08lx\r\n",
                (unsigned long)intan_block_id,
                (unsigned long)intan_abi_version,
                (unsigned long)intan_info,
                (unsigned long)intan_capabilities);
    DEBUG_PRINT("  INFO DDR writer id=0x%08lx ABI=0x%08lx info=0x%08lx\r\n",
                (unsigned long)ddrw_block_id,
                (unsigned long)ddrw_abi_version,
                (unsigned long)ddrw_info);

    if (intan_block_id != INTAN_BLOCK_ID_EXPECTED ||
        (intan_abi_version & NCLP_ABI_MAJOR_MASK) !=
            (INTAN_ABI_VERSION_EXPECTED & NCLP_ABI_MAJOR_MASK) ||
        (intan_capabilities & INTAN_CAPABILITIES_REQUIRED) !=
            INTAN_CAPABILITIES_REQUIRED ||
        intan_info != INTAN_INFO_EXPECTED ||
        ddrw_block_id != DDRW_BLOCK_ID_EXPECTED ||
        (ddrw_abi_version & NCLP_ABI_MAJOR_MASK) !=
            (DDRW_ABI_VERSION_EXPECTED & NCLP_ABI_MAJOR_MASK) ||
        (ddrw_capabilities & DDRW_CAPABILITIES_REQUIRED) !=
            DDRW_CAPABILITIES_REQUIRED ||
        ddrw_info != DDRW_INFO_EXPECTED) {
        xil_printf("  FAIL PL map Intan=0x%08lx caps=0x%08lx "
                   "DDRW=0x%08lx caps=0x%08lx\r\n",
                   (unsigned long)intan_block_id,
                   (unsigned long)intan_capabilities,
                   (unsigned long)ddrw_block_id,
                   (unsigned long)ddrw_capabilities);
        return -1;
    }
    xil_printf("  PASS Intan flat AXI ABI v3 capabilities 0x%08lx\r\n",
               (unsigned long)intan_capabilities);
    reg_write(INTAN_BASE, INTAN_REG_ERROR_STATUS,
              INTAN_ERROR_IMPLEMENTED_MASK);
    reg_write(INTAN_BASE, INTAN_REG_ERROR_ENABLE,
              INTAN_ERROR_IMPLEMENTED_MASK);
    xil_printf("  PASS DDR writer v2 capabilities 0x%08lx\r\n",
               (unsigned long)ddrw_capabilities);

    if (intan_reg_read(INTAN_REG_STREAM_SOURCE_MAP_LO, &value) != 0 ||
        value != INTAN_STREAM_SOURCE_MAP_LO_EXPECTED) {
        xil_printf("  FAIL fixed stream source map LO=0x%08lx\r\n",
                   (unsigned long)value);
        return -1;
    }
    if (intan_reg_read(INTAN_REG_STREAM_SOURCE_MAP_HI, &value) != 0 ||
        value != INTAN_STREAM_SOURCE_MAP_HI_EXPECTED) {
        xil_printf("  FAIL fixed stream source map HI=0x%08lx\r\n",
                   (unsigned long)value);
        return -1;
    }
    xil_printf("  PASS fixed stream maps 9180/B3A2/D5C4/F7E6\r\n");

    if (intan_reg_read(INTAN_REG_SAMPLE_CLOCK_STATUS, &value) == 0) {
        DEBUG_PRINT("  INFO sample clock status 0x%08lx locked=%lu ready=%lu busy=%lu\r\n",
                    (unsigned long)value,
                    (unsigned long)((value & INTAN_SAMPLE_CLOCK_STATUS_LOCKED) != 0U),
                    (unsigned long)((value & INTAN_SAMPLE_CLOCK_STATUS_READY) != 0U),
                    (unsigned long)((value & INTAN_SAMPLE_CLOCK_STATUS_BUSY) != 0U));
    }

    return 0;
}
