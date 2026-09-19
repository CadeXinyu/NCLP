#include "headstage_internal.h"

static int reset_and_check_recording_output(void)
{
    uint32_t output_config = 0U;
    uint32_t status = 0U;
    uint32_t intan_error_status = 0U;
    uint32_t end_of_session_marker_count = 0U;
    uint32_t unaccepted_source_event_count = 0U;
    uint32_t stall_cycle_count = 0U;

    if (enable_intan_recording_output() != 0) {
        return -1;
    }
    for (uint32_t attempt = 0U; attempt < 1000U; ++attempt) {
        output_config = reg_read(INTAN_BASE, INTAN_REG_OUTPUT_CONFIG);
        status = reg_read(INTAN_BASE, INTAN_REG_OUTPUT_STATUS);
        intan_error_status = reg_read(INTAN_BASE, INTAN_REG_ERROR_STATUS) &
                             INTAN_ERROR_IMPLEMENTED_MASK;
        end_of_session_marker_count = reg_read(
            INTAN_BASE, INTAN_REG_RECORDING_END_OF_SESSION_MARKER_COUNT);
        unaccepted_source_event_count = reg_read(
            INTAN_BASE, INTAN_REG_UNACCEPTED_SOURCE_EVENT_COUNT);
        stall_cycle_count = reg_read(INTAN_BASE,
                                     INTAN_REG_RECORDING_STALL_CYCLE_COUNT);
        if ((output_config & INTAN_OUTPUT_CONFIG_RECORDING_ENABLE) != 0U &&
            (status & (INTAN_OUTPUT_STATUS_RECORDING_END_OF_SESSION_SEEN |
                       INTAN_OUTPUT_STATUS_RECORDING_ACTIVE |
                       INTAN_OUTPUT_STATUS_ERROR_MASK)) == 0U &&
            intan_error_status == 0U &&
            end_of_session_marker_count == 0U &&
            unaccepted_source_event_count == 0U &&
            stall_cycle_count == 0U) {
            return 0;
        }
        usleep(10U);
    }

    xil_printf("  FAIL recording output reset config=0x%08lx status=0x%08lx error_status=0x%08lx end_markers=%lu unaccepted_source_events=%lu stall_cycles=%lu\r\n",
               (unsigned long)output_config, (unsigned long)status,
               (unsigned long)intan_error_status,
               (unsigned long)end_of_session_marker_count,
               (unsigned long)unaccepted_source_event_count,
               (unsigned long)stall_cycle_count);
    nclp_diagnostics.fail_count++;
    return -1;
}

static int reset_and_check_sfp_output(void)
{
    if ((reg_read(INTAN_BASE, INTAN_REG_ACQUISITION_STATUS) &
         (INTAN_ACQUISITION_STATUS_RUNNING |
          INTAN_ACQUISITION_STATUS_START_PENDING |
          INTAN_ACQUISITION_STATUS_ACCESS_LOCKED)) != 0U) {
        return -1;
    }
    if (enable_intan_compute_output() != 0 ||
        nclp_compute_clear_intan_end_of_stream() != 0 ||
        reg_read(INTAN_BASE,
                 INTAN_REG_UNACCEPTED_SOURCE_EVENT_COUNT) != 0U) {
        return -1;
    }
    return 0;
}

static int program_stream_aux_bank(uint32_t aux_mode)
{
    uint16_t dummy = rhd_read_reg_cmd(63U);
    uint16_t aux1;
    uint16_t aux2;
    uint16_t aux3;

    if (aux_mode == NCLP_STREAM_AUX_INPUTS) {
        aux1 = rhd_convert_cmd(32U);
        aux2 = rhd_convert_cmd(33U);
        aux3 = rhd_convert_cmd(34U);
    } else if (aux_mode == NCLP_STREAM_AUX_VDD) {
        aux1 = dummy;
        aux2 = dummy;
        aux3 = rhd_convert_cmd(48U);
    } else {
        xil_printf("  FAIL unsupported stream AUX mode=%lu\r\n",
                   (unsigned long)aux_mode);
        nclp_diagnostics.fail_count++;
        return -1;
    }

    /* The SPI core tags replies into the current frame's AUX rows 32..34.  On
     * RHD2164, only the primary (MISO A) stream has a valid VDD sensor. */
    intan_aux_command_write(INTAN_AUX1_WINDOW_OFFSET, AUX_VDD_BANK, 0U, aux1);
    intan_aux_command_write(INTAN_AUX2_WINDOW_OFFSET, AUX_VDD_BANK, 0U, aux2);
    intan_aux_command_write(INTAN_AUX3_WINDOW_OFFSET, AUX_VDD_BANK, 0U, aux3);

    return 0;
}

static const char *stream_aux_mode_name(uint32_t aux_mode)
{
    if (aux_mode == NCLP_STREAM_AUX_INPUTS) {
        return "AUX1/2/3";
    }
    if (aux_mode == NCLP_STREAM_AUX_VDD) {
        return "VDD";
    }
    return "UNKNOWN";
}

static int configure_stream_mask_run_common(uint32_t stream_mask,
                                            uint32_t mode,
                                            uint32_t frame_count,
                                            uint32_t aux_mode,
                                            uint32_t start_spi,
                                            uint32_t sfp_destination)
{
    uint32_t acquisition_config =
        (mode == NCLP_STREAM_MODE_CONT) ?
            INTAN_ACQUISITION_CONFIG_CONTINUOUS : 0U;

    stop_spi_safely();
    if (sfp_destination != 0U) {
        if (reset_and_check_sfp_output() != 0) {
            return -1;
        }
    } else {
        if (reset_and_check_recording_output() != 0 ||
            ddr_capture_prepare(SPI_CAPTURE_BLOCK_BYTES) != 0) {
            return -1;
        }
    }

    /* Only the local decoder needs the exact logical-stream mask. SFP receives
     * the unchanged Intan frame and its peer decodes the frame magic. */
    if (sfp_destination == 0U &&
        nclp_compute_set_local_stream_mask(stream_mask) != 0) {
        xil_printf("  FAIL local decoder stream mask=0x%04lx\r\n",
                   (unsigned long)stream_mask);
        nclp_diagnostics.fail_count++;
        return -1;
    }

    if (program_stream_aux_bank(aux_mode) != 0) {
        return -1;
    }

    if (write_lane_phases_verified(nclp_headstage.best_lane_phases_a,
                                   nclp_headstage.best_lane_phases_b) != 0) {
        xil_printf("  FAIL stream setup\r\n");
        nclp_diagnostics.fail_count++;
        return -1;
    }
    intan_register_write(INTAN_REG_AUX1_BANK_SELECT,
                         aux_bank_select_value(AUX_VDD_BANK));
    intan_register_write(INTAN_REG_AUX2_BANK_SELECT,
                         aux_bank_select_value(AUX_VDD_BANK));
    intan_register_write(INTAN_REG_AUX3_BANK_SELECT,
                         aux_bank_select_value(AUX_VDD_BANK));
    intan_register_write(INTAN_REG_AUX1_END_INDEX, 0U);
    intan_register_write(INTAN_REG_AUX2_END_INDEX, 0U);
    intan_register_write(INTAN_REG_AUX3_END_INDEX, 0U);
    intan_register_write(INTAN_REG_AUX1_LOOP_INDEX, 0U);
    intan_register_write(INTAN_REG_AUX2_LOOP_INDEX, 0U);
    intan_register_write(INTAN_REG_AUX3_LOOP_INDEX, 0U);
    intan_register_write(INTAN_REG_FINITE_FRAME_COUNT, frame_count);
    intan_register_write(INTAN_REG_ACQUISITION_CONFIG, acquisition_config);
    intan_register_write(INTAN_REG_LOGICAL_STREAM_ENABLE, stream_mask);

    /* SFP compute receives the original compact stream without starting
     * the DDR writer. Local recording enables both identical destinations. */
    const uint32_t output_config = INTAN_OUTPUT_CONFIG_COMPUTE_ENABLE |
        (sfp_destination != 0U ? 0U : INTAN_OUTPUT_CONFIG_RECORDING_ENABLE);
    reg_write(INTAN_BASE, INTAN_REG_OUTPUT_CONFIG, output_config);
    if (reg_read(INTAN_BASE, INTAN_REG_OUTPUT_CONFIG) != output_config) {
        nclp_diagnostics.fail_count++;
        return -1;
    }

    if (start_spi != 0U) {
        if ((sfp_destination != 0U ? start_intan_sfp_capture() :
                                     start_intan_capture()) != 0) {
            xil_printf("  FAIL stream source start\r\n");
            return -1;
        }
    }

    nclp_operation.streaming = (start_spi != 0U) ? 1U : 0U;
    nclp_operation.stream_aux_mode = aux_mode;
    xil_printf("  PASS stream %s mask=0x%04lx output=%s mode=%s frame_count=%lu aux=%s bank=%lu\r\n",
               (start_spi != 0U) ? "started" : "armed",
               (unsigned long)stream_mask,
               sfp_destination != 0U ? "SFP" : "RECORDING+LOCAL_COMPUTE",
               (mode == NCLP_STREAM_MODE_CONT) ? "continuous" : "finite",
               (unsigned long)frame_count,
               stream_aux_mode_name(aux_mode),
               (unsigned long)AUX_VDD_BANK);
    return 0;
}

int arm_stream_mask_run(uint32_t stream_mask, uint32_t mode,
                        uint32_t frame_count, uint32_t aux_mode)
{
    return configure_stream_mask_run_common(stream_mask, mode, frame_count,
                                            aux_mode, 0U, 0U);
}

int start_armed_stream(void)
{
    if (start_intan_capture() != 0) {
        xil_printf("  FAIL armed stream writer/SPI start\r\n");
        return -1;
    }
    nclp_operation.streaming = 1U;
    return 0;
}

static int wait_stream_frame_end_and_writer_idle(void)
{
    uint32_t stream_status;
    uint32_t writer_status;
    uint32_t writer_state;
    uint32_t produced;
    uint32_t intan_error_status;
    uint32_t end_of_session_marker_count;
    uint32_t unaccepted_source_event_count;
    uint32_t stall_cycle_count;
    uint32_t required = DDRW_STATUS_EOS_SEEN |
                        DDRW_STATUS_EOS_COMMITTED;
    uint32_t forbidden = DDRW_STATUS_RUNNING | DDRW_STATUS_DATA_PATH_BUSY |
                         DDRW_STATUS_FIFO_NONEMPTY | DDRW_STATUS_ERROR_MASK;

    if (ddrw_wait_for_state(DDRW_STATE_DONE, required, forbidden,
                            1000000U, "stream clean EOS") != 0) {
        return -1;
    }

    stream_status = reg_read(INTAN_BASE, INTAN_REG_OUTPUT_STATUS);
    writer_state = reg_read(SPI_DDR_WRITER_BASE, DDRW_REG_SESSION_STATE);
    writer_status = reg_read(SPI_DDR_WRITER_BASE, DDRW_REG_STATUS);
    produced = reg_read(SPI_DDR_WRITER_BASE,
                        DDRW_REG_PRODUCED_BLOCK_COUNT);
    intan_error_status = reg_read(INTAN_BASE, INTAN_REG_ERROR_STATUS) &
                         INTAN_ERROR_IMPLEMENTED_MASK;
    end_of_session_marker_count = reg_read(
        INTAN_BASE, INTAN_REG_RECORDING_END_OF_SESSION_MARKER_COUNT);
    unaccepted_source_event_count = reg_read(
        INTAN_BASE, INTAN_REG_UNACCEPTED_SOURCE_EVENT_COUNT);
    stall_cycle_count = reg_read(INTAN_BASE,
                                 INTAN_REG_RECORDING_STALL_CYCLE_COUNT);
    if ((stream_status & INTAN_OUTPUT_STATUS_RECORDING_END_OF_SESSION_SEEN) != 0U &&
        (stream_status & (INTAN_OUTPUT_STATUS_RECORDING_ACTIVE |
                          INTAN_OUTPUT_STATUS_ERROR_MASK)) == 0U &&
        end_of_session_marker_count != 0U &&
        intan_error_status == 0U &&
        unaccepted_source_event_count == 0U &&
        stall_cycle_count == 0U &&
        reg_read(SPI_DDR_WRITER_BASE, DDRW_REG_ERROR_STATUS) == 0U) {
        return 0;
    }

    xil_printf("  FAIL stream terminal output=0x%08lx intan_error=0x%08lx writer_state=%lu writer=0x%08lx produced=%lu end_markers=%lu unaccepted_source_events=%lu stall_cycles=%lu\r\n",
               (unsigned long)stream_status,
               (unsigned long)intan_error_status,
               (unsigned long)writer_state,
               (unsigned long)writer_status,
               (unsigned long)produced,
               (unsigned long)end_of_session_marker_count,
               (unsigned long)unaccepted_source_event_count,
               (unsigned long)stall_cycle_count);
    nclp_diagnostics.fail_count++;
    return -1;
}

static int wait_compute_fabric_frame_end_and_idle(uint32_t was_streaming)
{
    uint32_t status = 0U;

    for (uint32_t elapsed = 0U; elapsed < 1000000U; elapsed += 10U) {
        status = nclp_compute_fabric_status();
        if ((status & COMPUTE_FABRIC_STATUS_CONFIG_IDLE) != 0U &&
            (was_streaming == 0U ||
             (status & COMPUTE_FABRIC_STATUS_INTAN_EOS_SEEN) != 0U)) {
            if ((status & COMPUTE_FABRIC_STATUS_FAULT_MASK) == 0U)
                return 0;
            break;
        }
        usleep(10U);
    }

    xil_printf("  FAIL local compute drain status=0x%08lx\r\n",
               (unsigned long)status);
    nclp_diagnostics.fail_count++;
    return -1;
}

int stop_command_stream(void)
{
    uint32_t was_streaming = nclp_operation.streaming;

    stop_spi_safely();
    if (wait_spi_stopped(10000U) != 0 ||
        (was_streaming != 0U &&
         (wait_stream_frame_end_and_writer_idle() != 0 ||
          wait_compute_fabric_frame_end_and_idle(was_streaming) != 0))) {
        return -1;
    }
    reg_write(INTAN_BASE, INTAN_REG_OUTPUT_CONFIG,
              INTAN_OUTPUT_CONFIG_RECORDING_ENABLE);
    nclp_operation.streaming = 0U;
    return 0;
}

int arm_sfp_stream_run(uint32_t stream_mask, uint32_t mode,
                        uint32_t frame_count, uint32_t aux_mode)
{
    return configure_stream_mask_run_common(stream_mask, mode, frame_count,
                                            aux_mode, 0U, 1U);
}

int start_armed_sfp_stream(void)
{
    if (start_intan_sfp_capture() != 0) {
        return -1;
    }
    nclp_operation.streaming = 1U;
    return 0;
}

static int finish_intan_compute_stream(uint32_t forced_stop)
{
    uint32_t acquisition_status = 0U;
    uint32_t compute_status = 0U;
    uint32_t completed = 0U;
    uint32_t eos_cleared = 0U;
    uint32_t was_streaming = nclp_operation.streaming;

    /* Request the frame-boundary stop first. Do not clear masks or output
     * selection while a stopped producer still owes buffered data/EOS. */
    intan_register_write(INTAN_REG_ACQUISITION_CONFIG, 0U);
    for (uint32_t elapsed = 0U; elapsed < 1000000U; elapsed += 10U) {
        acquisition_status = reg_read(INTAN_BASE, INTAN_REG_ACQUISITION_STATUS);
        compute_status = nclp_compute_fabric_status();
        if ((acquisition_status & (INTAN_ACQUISITION_STATUS_RUNNING |
             INTAN_ACQUISITION_STATUS_START_PENDING |
             INTAN_ACQUISITION_STATUS_ACCESS_LOCKED)) == 0U) {
            if (forced_stop != 0U && eos_cleared == 0U) {
                if (nclp_compute_clear_intan_end_of_stream() != 0) {
                    usleep(10U);
                    continue;
                }
                eos_cleared = 1U;
                compute_status = nclp_compute_fabric_status();
            }
            if ((compute_status & COMPUTE_FABRIC_STATUS_CONFIG_IDLE) != 0U &&
                ((forced_stop != 0U && eos_cleared != 0U) ||
                 was_streaming == 0U ||
                 (compute_status &
                  COMPUTE_FABRIC_STATUS_INTAN_EOS_SEEN) != 0U)) {
                completed = 1U;
                break;
            }
        }
        usleep(10U);
    }
    if (completed == 0U) {
        xil_printf("  FAIL compute drain acquisition=0x%08lx compute=0x%08lx\r\n",
                   (unsigned long)acquisition_status,
                   (unsigned long)compute_status);
        nclp_diagnostics.fail_count++;
        return -1;
    }
    intan_register_write(INTAN_REG_FINITE_FRAME_COUNT, 0U);
    intan_register_write(INTAN_REG_LOGICAL_STREAM_ENABLE, 0U);
    reg_write(INTAN_BASE, INTAN_REG_OUTPUT_CONFIG, 0U);
    if (reg_read(INTAN_BASE, INTAN_REG_OUTPUT_CONFIG) != 0U) {
        return -1;
    }
    nclp_operation.streaming = 0U;
    if (forced_stop == 0U &&
        (((compute_status & COMPUTE_FABRIC_STATUS_FAULT_MASK) != 0U) ||
         (reg_read(INTAN_BASE, INTAN_REG_ERROR_STATUS) &
          INTAN_ERROR_IMPLEMENTED_MASK) != 0U ||
         reg_read(INTAN_BASE,
                  INTAN_REG_UNACCEPTED_SOURCE_EVENT_COUNT) != 0U)) {
        return -1;
    }
    return 0;
}

int stop_sfp_stream(void)
{
    return finish_intan_compute_stream(0U);
}

int abort_intan_compute_stream(void)
{
    return finish_intan_compute_stream(1U);
}
