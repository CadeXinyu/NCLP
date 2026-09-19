#include "stim_control.h"

#include "../../../common/nclp_pl_registers.h"

#include "xil_io.h"
#include "xil_printf.h"

#include <stddef.h>
#include <stdint.h>

#define STIM_CTRL_BASE ((uintptr_t)NCLP_STIM_BASE_DEFAULT)
#define STIM_INIT_IDLE_POLL_LIMIT 100000U
#define STIM_COMMAND_POLL_LIMIT 100000U
#define STIM_CLOCK_POLL_LIMIT 1000000U
#define STIM_DAC_CANONICAL_WORD_MASK 0x0FFF0FFFU
#define STIM_DAC_ENGINE_MAX_MHZ 40U
#define STIM_INIT_QUIESCE_STATUS_MASK \
    (STIM_STATUS_ARMED | STIM_STATUS_BUSY | STIM_STATUS_TTL_ACTIVE | \
     STIM_STATUS_DAC_RUNNING | STIM_STATUS_DAC_BUSY | \
     STIM_STATUS_DAC_CLOCK_PROGRAM_BUSY | STIM_STATUS_DAC_PRIME_BUSY | \
     STIM_STATUS_CONFIG_LOCKED)
#define STIM_INIT_FINAL_ERROR_STATUS_MASK \
    (STIM_STATUS_ERROR_PENDING | STIM_STATUS_FAULT_IRQ)

static uint32_t g_stim_initialized;
static uint32_t g_stim_reported_errors;
static uint32_t g_preset_valid;
static nclp_stim_preset_config_t g_preset;

static uint32_t read_stim_register(uint32_t offset)
{
    return Xil_In32((UINTPTR)(STIM_CTRL_BASE + offset));
}

static void write_stim_register(uint32_t offset, uint32_t value)
{
    Xil_Out32((UINTPTR)(STIM_CTRL_BASE + offset), value);
}

static int stim_ready(void)
{
    return g_stim_initialized != 0U ? NCLP_STIM_OK :
                                      NCLP_STIM_ERROR_NOT_READY;
}

static int configuration_access_allowed(void)
{
    uint32_t status;

    if (stim_ready() != NCLP_STIM_OK) {
        return NCLP_STIM_ERROR_NOT_READY;
    }
    status = read_stim_register(STIM_REG_STATUS);
    /* CONFIG_LOCKED is the RTL's admission contract.  Do not reject the
     * aggregate BUSY bit: it deliberately remains high while a mandatory
     * A/B-zero obligation waits for an unavailable clock, during which OFF,
     * TTL pulse, DAC, and RAM configuration remain writable. */
    if ((status & (STIM_STATUS_ARMED |
                   STIM_STATUS_CONFIG_LOCKED)) != 0U) {
        return NCLP_STIM_ERROR_BUSY;
    }
    return NCLP_STIM_OK;
}

static int register_write_verified(uint32_t offset, uint32_t value)
{
    write_stim_register(offset, value);
    return read_stim_register(offset) == value ? NCLP_STIM_OK :
                                                NCLP_STIM_ERROR_HARDWARE;
}

static uint32_t crc32_update_word(uint32_t crc, uint32_t word)
{
    for (uint32_t byte_index = 0U; byte_index < 4U; ++byte_index) {
        crc ^= (word >> (8U * byte_index)) & 0xFFU;
        for (uint32_t bit_index = 0U; bit_index < 8U; ++bit_index) {
            uint32_t reflected_polynomial =
                (uint32_t)-(int32_t)(crc & 1U) & 0xEDB88320U;
            crc = (crc >> 1U) ^ reflected_polynomial;
        }
    }
    return crc;
}

int nclp_stim_action_valid(const nclp_stim_action_config_t *config)
{
    return config != NULL && config->mode <= STIM_OUTPUT_MODE_DAC &&
           config->intan_marker_mask <= 0x3FFFU &&
           config->external_trigger_enable <= 1U &&
           (config->mode != STIM_OUTPUT_MODE_TTL ||
            config->ttl_pulse_width_axi_cycles != 0U);
}

static int dac_config_valid(const nclp_stim_dac_config_t *config)
{
    return config != NULL &&
           config->channel_mask >= NCLP_DAC_CHANNEL_A &&
           config->channel_mask <= NCLP_DAC_CHANNEL_BOTH &&
           config->update_period_clocks != 0U &&
           config->start_index < STIM_RAM_DEPTH_WORDS &&
           config->loop_index < STIM_RAM_DEPTH_WORDS &&
           config->end_index < STIM_RAM_DEPTH_WORDS &&
           config->start_index <= config->loop_index &&
           config->loop_index <= config->end_index &&
           config->finite_update_count != 0U;
}

static void live_snapshot(uint32_t words[4])
{
    if (words == NULL) {
        return;
    }
    words[0] = read_stim_register(STIM_REG_STATUS);
    words[1] = read_stim_register(STIM_REG_DAC_PRIME_STATUS);
    words[2] = read_stim_register(STIM_REG_ACCEPTED_TRIGGER_COUNT);
    words[3] = read_stim_register(STIM_REG_UNSERVED_TRIGGER_COUNT);
}

int nclp_stim_init(void)
{
    uint32_t block_id;
    uint32_t abi_version;
    uint32_t capabilities;
    uint32_t info;
    uint32_t status;
    uint32_t error_status;
    uint32_t action_config;
    uint32_t ttl_pulse_width;
    uint32_t intan_stim_marker_mask;
    uint32_t fault_irq_enable;
    uint32_t poll_count;

    g_stim_initialized = 0U;
    g_preset_valid = 0U;
    g_preset = (nclp_stim_preset_config_t){0};
    g_stim_reported_errors = 0U;
    block_id = read_stim_register(STIM_REG_BLOCK_ID);
    abi_version = read_stim_register(STIM_REG_ABI_VERSION);
    capabilities = read_stim_register(STIM_REG_CAPABILITIES);
    info = read_stim_register(STIM_REG_INFO);
    if (block_id != STIM_BLOCK_ID_EXPECTED ||
        (abi_version & NCLP_ABI_MAJOR_MASK) !=
            (STIM_ABI_VERSION_EXPECTED & NCLP_ABI_MAJOR_MASK) ||
        (capabilities & STIM_CAPABILITIES_REQUIRED) !=
            STIM_CAPABILITIES_REQUIRED ||
        info != STIM_INFO_EXPECTED) {
        xil_printf("  FAIL PL Stim identity id=0x%08lx abi=0x%08lx caps=0x%08lx info=0x%08lx\r\n",
                   (unsigned long)block_id,
                   (unsigned long)abi_version,
                   (unsigned long)capabilities,
                   (unsigned long)info);
        return -1;
    }

    /* A firmware restart must never inherit an armed or active output.  The
     * mandatory reset/clock-epoch A/B-zero obligation is included in the RTL
     * controller BUSY state, so this bounded wait also proves physical zeroing
     * completed before configuration is changed. */
    write_stim_register(STIM_REG_COMMAND,
                        STIM_COMMAND_DISARM | STIM_COMMAND_STOP_ACTIVITY);
    status = 0U;
    for (poll_count = 0U; poll_count < STIM_INIT_IDLE_POLL_LIMIT; ++poll_count) {
        status = read_stim_register(STIM_REG_STATUS);
        if ((status & STIM_INIT_QUIESCE_STATUS_MASK) == 0U) {
            break;
        }
    }
    if (poll_count == STIM_INIT_IDLE_POLL_LIMIT ||
        (status & STIM_INIT_QUIESCE_STATUS_MASK) != 0U) {
        xil_printf("  FAIL PL Stim quiesce timeout status=0x%08lx\r\n",
                   (unsigned long)status);
        return -1;
    }

    /* Configuration writes are rejected while STATUS.CONFIG_LOCKED is set.
     * Apply the fail-low preset only after quiescence, then prove that every
     * field reached the PS-visible register bank. */
    write_stim_register(STIM_REG_ACTION_CONFIG, STIM_OUTPUT_MODE_OFF);
    write_stim_register(STIM_REG_TTL_PULSE_WIDTH_AXI_CYCLES, 0U);
    write_stim_register(STIM_REG_INTAN_STIM_MARKER_MASK, 0U);

    action_config = read_stim_register(STIM_REG_ACTION_CONFIG);
    ttl_pulse_width = read_stim_register(STIM_REG_TTL_PULSE_WIDTH_AXI_CYCLES);
    intan_stim_marker_mask =
        read_stim_register(STIM_REG_INTAN_STIM_MARKER_MASK);
    if (action_config != STIM_OUTPUT_MODE_OFF ||
        ttl_pulse_width != 0U || intan_stim_marker_mask != 0U) {
        xil_printf("  FAIL PL Stim fail-low readback action=0x%08lx width=0x%08lx marker=0x%08lx\r\n",
                   (unsigned long)action_config,
                   (unsigned long)ttl_pulse_width,
                   (unsigned long)intan_stim_marker_mask);
        return -1;
    }

    write_stim_register(STIM_REG_FAULT_IRQ_ENABLE,
                        STIM_FAULT_IRQ_ENABLE_DEFAULT);
    write_stim_register(STIM_REG_COMMAND, STIM_COMMAND_CLEAR_DIAGNOSTICS);
    write_stim_register(STIM_REG_ERROR_STATUS, STIM_ERROR_ALL_MASK);
    fault_irq_enable = read_stim_register(STIM_REG_FAULT_IRQ_ENABLE);
    error_status = read_stim_register(STIM_REG_ERROR_STATUS);
    status = read_stim_register(STIM_REG_STATUS);
    if (fault_irq_enable != STIM_FAULT_IRQ_ENABLE_DEFAULT ||
        error_status != 0U ||
        (status & (STIM_INIT_QUIESCE_STATUS_MASK |
                   STIM_INIT_FINAL_ERROR_STATUS_MASK)) != 0U) {
        xil_printf("  FAIL PL Stim unsafe final state status=0x%08lx errors=0x%08lx irq_mask=0x%08lx\r\n",
                   (unsigned long)status,
                   (unsigned long)error_status,
                   (unsigned long)fault_irq_enable);
        return -1;
    }

    g_stim_initialized = 1U;
    xil_printf("  PASS PL Stim controller      ABI v3 depth=%lu words\r\n",
               (unsigned long)(info & STIM_INFO_RAM_DEPTH_MASK));
    return 0;
}

uint32_t nclp_stim_service(void)
{
    uint32_t enabled_errors;

    if (g_stim_initialized == 0U) {
        return 0U;
    }
    enabled_errors = read_stim_register(STIM_REG_ERROR_STATUS) &
                     read_stim_register(STIM_REG_FAULT_IRQ_ENABLE);
    if (enabled_errors != 0U && enabled_errors != g_stim_reported_errors) {
        /* A detected stimulation fault must fail the physical path safe even
         * if the application subsequently enters its global FAULT state.
         * DISARM drops TTL immediately and STOP retains the DAC A/B-zero
         * obligation until the serial clock can complete it. */
        write_stim_register(STIM_REG_COMMAND,
                            STIM_COMMAND_DISARM |
                            STIM_COMMAND_STOP_ACTIVITY);
        xil_printf("  FAIL PL Stim fault errors=0x%08lx last=0x%08lx count=%lu\r\n",
                   (unsigned long)enabled_errors,
                   (unsigned long)read_stim_register(STIM_REG_LAST_ERROR_VECTOR),
                   (unsigned long)read_stim_register(STIM_REG_ERROR_INCIDENT_CYCLE_COUNT));
    }
    g_stim_reported_errors = enabled_errors;
    return enabled_errors;
}

void nclp_stim_emergency_stop(void)
{
    if (g_stim_initialized != 0U) {
        write_stim_register(STIM_REG_COMMAND,
                            STIM_COMMAND_DISARM |
                            STIM_COMMAND_STOP_ACTIVITY);
    }
}

int nclp_stim_get_section(uint32_t section, uint32_t words[4])
{
    if (words == NULL || section >= NCLP_STIM_SECTION_COUNT) {
        return NCLP_STIM_ERROR_ARGUMENT;
    }
    if (stim_ready() != NCLP_STIM_OK) {
        return NCLP_STIM_ERROR_NOT_READY;
    }

    switch (section) {
    case NCLP_STIM_SECTION_IDENTITY:
        words[0] = read_stim_register(STIM_REG_BLOCK_ID);
        words[1] = read_stim_register(STIM_REG_ABI_VERSION);
        words[2] = read_stim_register(STIM_REG_CAPABILITIES);
        words[3] = read_stim_register(STIM_REG_INFO);
        break;
    case NCLP_STIM_SECTION_LIVE:
        words[0] = read_stim_register(STIM_REG_STATUS);
        words[1] = read_stim_register(STIM_REG_SAFETY_STATUS);
        words[2] = read_stim_register(STIM_REG_ACCEPTED_TRIGGER_COUNT);
        words[3] = read_stim_register(STIM_REG_UNSERVED_TRIGGER_COUNT);
        break;
    case NCLP_STIM_SECTION_ACTION:
        words[0] = read_stim_register(STIM_REG_ACTION_CONFIG);
        words[1] = 0U; /* Protocol layer fills the independent router view. */
        words[2] = read_stim_register(STIM_REG_TTL_PULSE_WIDTH_AXI_CYCLES);
        words[3] = read_stim_register(STIM_REG_INTAN_STIM_MARKER_MASK);
        break;
    case NCLP_STIM_SECTION_DAC_CONFIG_0:
        words[0] = read_stim_register(STIM_REG_DAC_PLAYBACK_CONFIG);
        words[1] = read_stim_register(STIM_REG_DAC_UPDATE_PERIOD_CLOCKS);
        words[2] = read_stim_register(STIM_REG_DAC_START_INDEX);
        words[3] = read_stim_register(STIM_REG_DAC_LOOP_INDEX);
        break;
    case NCLP_STIM_SECTION_DAC_CONFIG_1:
        words[0] = read_stim_register(STIM_REG_DAC_END_INDEX);
        words[1] = read_stim_register(STIM_REG_DAC_FINITE_UPDATE_COUNT);
        words[2] = read_stim_register(STIM_REG_DAC_CLOCK_CONFIG);
        words[3] = read_stim_register(STIM_REG_DAC_CLOCK_STATUS);
        break;
    case NCLP_STIM_SECTION_DAC_RUNTIME:
        words[0] = read_stim_register(STIM_REG_DAC_PRIME_STATUS);
        /* CURRENT latches the coherent playback snapshot consumed by the
         * following COMPLETED read in the PL register bank. */
        words[1] = read_stim_register(STIM_REG_DAC_CURRENT_WAVEFORM_INDEX);
        words[2] = read_stim_register(STIM_REG_DAC_COMPLETED_UPDATE_COUNT);
        words[3] = read_stim_register(STIM_REG_SAFE_OFF_COUNT);
        break;
    case NCLP_STIM_SECTION_ERRORS:
        words[0] = read_stim_register(STIM_REG_ERROR_STATUS);
        words[1] = read_stim_register(STIM_REG_FAULT_IRQ_ENABLE);
        words[2] = read_stim_register(STIM_REG_ERROR_INCIDENT_CYCLE_COUNT);
        words[3] = read_stim_register(STIM_REG_LAST_ERROR_VECTOR);
        break;
    case NCLP_STIM_SECTION_PRESET_0:
        words[0] = g_preset_valid; words[1] = g_preset.kind;
        words[2] = g_preset.update_rate_hz; words[3] = g_preset.parameter;
        break;
    case NCLP_STIM_SECTION_PRESET_1:
        words[0] = g_preset.minimum_a_uv; words[1] = g_preset.maximum_a_uv;
        words[2] = g_preset.minimum_b_uv; words[3] = g_preset.maximum_b_uv;
        break;
    case NCLP_STIM_SECTION_PRESET_2:
        words[0] = g_preset.channel_mask; words[1] = g_preset.repeat_count;
        words[2] = 0U; words[3] = 0U;
        break;
    default:
        return NCLP_STIM_ERROR_ARGUMENT;
    }
    return NCLP_STIM_OK;
}

int nclp_stim_set_recorded_ttl(uint32_t logical_ttl)
{
    int access;
    uint32_t mask;
    if (logical_ttl == 1U || logical_ttl > 15U) return NCLP_STIM_ERROR_ARGUMENT;
    access = configuration_access_allowed();
    if (access != NCLP_STIM_OK) return access;
    mask = logical_ttl == 0U ? 0U : 1U << (logical_ttl - 2U);
    return register_write_verified(STIM_REG_INTAN_STIM_MARKER_MASK, mask);
}

int nclp_stim_set_action(const nclp_stim_action_config_t *config)
{
    uint32_t action;
    int access;

    if (!nclp_stim_action_valid(config)) {
        return NCLP_STIM_ERROR_ARGUMENT;
    }
    access = configuration_access_allowed();
    if (access != NCLP_STIM_OK) {
        return access;
    }

    action = config->mode |
             (config->external_trigger_enable != 0U ?
              STIM_ACTION_CONFIG_EXTERNAL_TRIGGER_ENABLE : 0U);
    /* Any ACTION_CONFIG write invalidates PRIME in PL, even an identical value.
     * TTL pulse width/recording changes do not change the prepared waveform.
     * Only write this word when mode or trigger admission actually changes. */
    if ((read_stim_register(STIM_REG_ACTION_CONFIG) != action &&
         register_write_verified(STIM_REG_ACTION_CONFIG, action) != NCLP_STIM_OK) ||
        register_write_verified(STIM_REG_TTL_PULSE_WIDTH_AXI_CYCLES,
                                config->ttl_pulse_width_axi_cycles) !=
            NCLP_STIM_OK ||
        register_write_verified(STIM_REG_INTAN_STIM_MARKER_MASK,
                                config->intan_marker_mask) != NCLP_STIM_OK) {
        return NCLP_STIM_ERROR_HARDWARE;
    }
    return NCLP_STIM_OK;
}

int nclp_stim_set_dac(const nclp_stim_dac_config_t *config)
{
    int access;

    if (!dac_config_valid(config)) {
        return NCLP_STIM_ERROR_ARGUMENT;
    }
    access = configuration_access_allowed();
    if (access != NCLP_STIM_OK) {
        return access;
    }
    g_preset_valid = 0U;
    if (register_write_verified(STIM_REG_DAC_PLAYBACK_CONFIG, config->channel_mask) !=
            NCLP_STIM_OK ||
        register_write_verified(STIM_REG_DAC_UPDATE_PERIOD_CLOCKS,
                                config->update_period_clocks) !=
            NCLP_STIM_OK ||
        register_write_verified(STIM_REG_DAC_START_INDEX,
                                config->start_index) != NCLP_STIM_OK ||
        register_write_verified(STIM_REG_DAC_LOOP_INDEX,
                                config->loop_index) != NCLP_STIM_OK ||
        register_write_verified(STIM_REG_DAC_END_INDEX,
                                config->end_index) != NCLP_STIM_OK ||
        register_write_verified(STIM_REG_DAC_FINITE_UPDATE_COUNT,
                                config->finite_update_count) != NCLP_STIM_OK) {
        return NCLP_STIM_ERROR_HARDWARE;
    }
    return NCLP_STIM_OK;
}

int nclp_stim_set_clock(uint32_t output_divide, uint32_t input_divide,
                        uint32_t feedback_multiply,
                        uint32_t *clock_status_out)
{
    uint32_t config_word;
    uint32_t status = 0U;
    uint32_t vco_numerator;
    int access;

    if (output_divide < 1U || output_divide > 128U ||
        input_divide < 1U || input_divide > 2U ||
        feedback_multiply < 2U ||
        feedback_multiply > STIM_DAC_CLOCK_CONFIG_PLL_M_MAX) {
        return NCLP_STIM_ERROR_ARGUMENT;
    }
    vco_numerator = 140U * feedback_multiply;
    if (vco_numerator < 750U * input_divide ||
        vco_numerator > 1500U * input_divide ||
        vco_numerator > STIM_DAC_ENGINE_MAX_MHZ * input_divide *
                        output_divide) {
        return NCLP_STIM_ERROR_ARGUMENT;
    }
    access = stim_ready();
    if (access != NCLP_STIM_OK) {
        return access;
    }
    /* Unlike ordinary configuration, a clock request must remain available
     * while STATUS.BUSY represents a mandatory A/B-zero request waiting for
     * clock recovery.  CONFIG_LOCKED still protects an active transaction. */
    status = read_stim_register(STIM_REG_STATUS);
    if ((status & (STIM_STATUS_ARMED | STIM_STATUS_CONFIG_LOCKED |
                   STIM_STATUS_DAC_CLOCK_PROGRAM_BUSY |
                   STIM_STATUS_DAC_PRIME_BUSY)) != 0U) {
        if (clock_status_out != NULL) {
            *clock_status_out = read_stim_register(
                STIM_REG_DAC_CLOCK_STATUS);
        }
        return NCLP_STIM_ERROR_BUSY;
    }
    status = read_stim_register(STIM_REG_DAC_CLOCK_STATUS);
    if ((status & (STIM_DAC_CLOCK_STATUS_READY |
                   STIM_DAC_CLOCK_STATUS_PROGRAM_BUSY)) !=
        STIM_DAC_CLOCK_STATUS_READY) {
        if (clock_status_out != NULL) {
            *clock_status_out = status;
        }
        return NCLP_STIM_ERROR_BUSY;
    }

    config_word = (output_divide << STIM_DAC_CLOCK_CONFIG_O_SHIFT) |
                  (input_divide << STIM_DAC_CLOCK_CONFIG_D_SHIFT) |
                  (feedback_multiply << STIM_DAC_CLOCK_CONFIG_M_SHIFT);
    g_preset_valid = 0U;
    if (register_write_verified(STIM_REG_DAC_CLOCK_CONFIG, config_word) !=
        NCLP_STIM_OK) {
        return NCLP_STIM_ERROR_HARDWARE;
    }
    write_stim_register(STIM_REG_DAC_CLOCK_COMMAND,
                        STIM_DAC_CLOCK_COMMAND_PROGRAM);
    for (uint32_t poll = 0U; poll < STIM_CLOCK_POLL_LIMIT; ++poll) {
        status = read_stim_register(STIM_REG_DAC_CLOCK_STATUS);
        if ((status & STIM_DAC_CLOCK_STATUS_ERROR) != 0U) {
            break;
        }
        if ((status & STIM_DAC_CLOCK_STATUS_PROGRAM_BUSY) == 0U &&
            (status & (STIM_DAC_CLOCK_STATUS_LOCKED |
                       STIM_DAC_CLOCK_STATUS_READY)) ==
                (STIM_DAC_CLOCK_STATUS_LOCKED |
                 STIM_DAC_CLOCK_STATUS_READY)) {
            if (clock_status_out != NULL) {
                *clock_status_out = status;
            }
            return NCLP_STIM_OK;
        }
    }
    if (clock_status_out != NULL) {
        *clock_status_out = status;
    }
    return NCLP_STIM_ERROR_HARDWARE;
}

int nclp_stim_write_ram(uint32_t start_index, const uint32_t *words,
                        uint32_t count, uint32_t *crc32_out)
{
    uint32_t crc = 0xFFFFFFFFU;
    int access;

    if (words == NULL || count == 0U ||
        start_index >= STIM_RAM_DEPTH_WORDS ||
        count > STIM_RAM_DEPTH_WORDS - start_index) {
        return NCLP_STIM_ERROR_ARGUMENT;
    }
    for (uint32_t index = 0U; index < count; ++index) {
        if ((words[index] & ~STIM_DAC_CANONICAL_WORD_MASK) != 0U) {
            return NCLP_STIM_ERROR_ARGUMENT;
        }
    }
    access = configuration_access_allowed();
    if (access != NCLP_STIM_OK) {
        return access;
    }
    g_preset_valid = 0U;
    for (uint32_t index = 0U; index < count; ++index) {
        uint32_t offset = STIM_RAM_WORD_OFFSET(start_index + index);

        write_stim_register(offset, words[index]);
        if (read_stim_register(offset) != words[index]) {
            return NCLP_STIM_ERROR_HARDWARE;
        }
        crc = crc32_update_word(crc, words[index]);
    }
    if (crc32_out != NULL) {
        *crc32_out = crc ^ 0xFFFFFFFFU;
    }
    return NCLP_STIM_OK;
}

int nclp_stim_read_ram(uint32_t start_index, uint32_t *words,
                       uint32_t count)
{
    if (stim_ready() != NCLP_STIM_OK) {
        return NCLP_STIM_ERROR_NOT_READY;
    }
    if (words == NULL || count == 0U ||
        start_index >= STIM_RAM_DEPTH_WORDS ||
        count > STIM_RAM_DEPTH_WORDS - start_index) {
        return NCLP_STIM_ERROR_ARGUMENT;
    }
    for (uint32_t index = 0U; index < count; ++index) {
        words[index] = read_stim_register(
            STIM_RAM_WORD_OFFSET(start_index + index));
    }
    return NCLP_STIM_OK;
}

int nclp_stim_load_preset(const nclp_stim_preset_config_t *config,
                          nclp_dac_preset_plan_t *plan_out,
                          uint32_t *crc32_out)
{
    nclp_stim_action_config_t action;
    nclp_stim_dac_config_t dac;
    nclp_dac_preset_plan_t plan;
    const uint32_t clock = read_stim_register(STIM_REG_DAC_CLOCK_CONFIG);
    const uint32_t o = clock & 255U, d = (clock >> 8) & 15U, m = (clock >> 12) & 255U;
    const uint32_t engine_hz = o && d ? (uint32_t)(140000000ULL * m / (o * d)) : 0U;
    uint32_t crc = 0xFFFFFFFFU;
    uint32_t live[4];
    int access;
    int result;

    if (!nclp_dac_plan_preset(config, engine_hz, &plan)) {
        return NCLP_STIM_ERROR_ARGUMENT;
    }
    access = configuration_access_allowed();
    if (access != NCLP_STIM_OK) {
        return access;
    }

    /* Physical pin routing is independently owned by ttl_router. */
    action.mode = STIM_OUTPUT_MODE_DAC;
    action.ttl_pulse_width_axi_cycles =
        read_stim_register(STIM_REG_TTL_PULSE_WIDTH_AXI_CYCLES);
    action.intan_marker_mask =
        read_stim_register(STIM_REG_INTAN_STIM_MARKER_MASK) & 0x3FFFU;
    action.external_trigger_enable = config->external_trigger_enable;
    if (!nclp_stim_action_valid(&action)) {
        return NCLP_STIM_ERROR_ARGUMENT;
    }

    dac.channel_mask = config->channel_mask;
    dac.update_period_clocks = plan.update_period_clocks;
    dac.start_index = 0U;
    dac.loop_index = 0U;
    dac.end_index = plan.sample_count - 1U;
    dac.finite_update_count = plan.finite_update_count;
    if (!dac_config_valid(&dac)) {
        return NCLP_STIM_ERROR_ARGUMENT;
    }

    g_preset_valid = 0U;
    for (uint32_t index = 0U; index < plan.sample_count; ++index) {
        uint32_t word = nclp_dac_preset_word(config, &plan, index);
        uint32_t offset = STIM_RAM_WORD_OFFSET(index);

        write_stim_register(offset, word);
        if (read_stim_register(offset) != word) {
            return NCLP_STIM_ERROR_HARDWARE;
        }
        crc = crc32_update_word(crc, word);
    }
    result = nclp_stim_set_action(&action);
    if (result != NCLP_STIM_OK) {
        return result;
    }
    result = nclp_stim_set_dac(&dac);
    if (result != NCLP_STIM_OK) {
        return result;
    }
    result = nclp_stim_control(NCLP_STIM_ACTION_PRIME, live);
    if (result != NCLP_STIM_OK) {
        return result;
    }
    g_preset = *config;
    g_preset_valid = 1U;
    if (plan_out != NULL) *plan_out = plan;
    if (crc32_out != NULL) {
        *crc32_out = crc ^ 0xFFFFFFFFU;
    }
    return NCLP_STIM_OK;
}

int nclp_stim_control(uint32_t action, uint32_t words[4])
{
    uint32_t status;
    uint32_t prime_status;
    uint32_t accepted_before;
    uint32_t unserved_before;
    uint32_t errors_before;

    if (stim_ready() != NCLP_STIM_OK) {
        return NCLP_STIM_ERROR_NOT_READY;
    }
    if (action != NCLP_STIM_ACTION_ARM &&
        action != NCLP_STIM_ACTION_DISARM &&
        action != NCLP_STIM_ACTION_TRIGGER &&
        action != NCLP_STIM_ACTION_CLEAR &&
        action != NCLP_STIM_ACTION_PRIME) {
        return NCLP_STIM_ERROR_ARGUMENT;
    }

    if ((action == NCLP_STIM_ACTION_ARM ||
         action == NCLP_STIM_ACTION_TRIGGER ||
         action == NCLP_STIM_ACTION_PRIME) &&
        (read_stim_register(STIM_REG_ERROR_STATUS) &
         read_stim_register(STIM_REG_FAULT_IRQ_ENABLE) &
         STIM_ERROR_ALL_MASK) != 0U) {
        /* Close the service-loop race: an enabled asynchronous fault that
         * arrives after service but before this mailbox command must never
         * permit a new physical action.  Disabled diagnostic bits may remain
         * sticky and are handled by the per-command new-error baseline. */
        return NCLP_STIM_ERROR_NOT_READY;
    }

    status = read_stim_register(STIM_REG_STATUS);
    prime_status = read_stim_register(STIM_REG_DAC_PRIME_STATUS);
    if (action == NCLP_STIM_ACTION_PRIME) {
        if ((status & (STIM_STATUS_ARMED | STIM_STATUS_BUSY |
                       STIM_STATUS_CONFIG_LOCKED)) != 0U) {
            return NCLP_STIM_ERROR_BUSY;
        }
        if ((status & STIM_STATUS_CONFIG_VALID) == 0U ||
            (status & STIM_STATUS_DAC_CLOCK_READY) == 0U ||
            (read_stim_register(STIM_REG_ACTION_CONFIG) & 0x3U) !=
                STIM_OUTPUT_MODE_DAC ||
            (read_stim_register(STIM_REG_SAFETY_STATUS) &
             STIM_SAFETY_STATUS_SAFE_OFF_ACTIVE) != 0U) {
            return NCLP_STIM_ERROR_NOT_READY;
        }
        errors_before = read_stim_register(STIM_REG_ERROR_STATUS) &
                        STIM_ERROR_ALL_MASK;
        write_stim_register(STIM_REG_COMMAND, STIM_COMMAND_PRIME_DAC);
        for (uint32_t poll = 0U; poll < STIM_COMMAND_POLL_LIMIT; ++poll) {
            prime_status = read_stim_register(STIM_REG_DAC_PRIME_STATUS);
            status = read_stim_register(STIM_REG_STATUS);
            if ((prime_status & STIM_DAC_PRIME_STATUS_FAULT) != 0U ||
                (read_stim_register(STIM_REG_ERROR_STATUS) &
                 STIM_ERROR_ALL_MASK & ~errors_before) != 0U) {
                break;
            }
            if ((prime_status & STIM_DAC_PRIME_STATUS_PRIMED) != 0U &&
                (status & (STIM_STATUS_BUSY |
                           STIM_STATUS_DAC_PRIME_BUSY)) == 0U) {
                live_snapshot(words);
                return NCLP_STIM_OK;
            }
        }
        live_snapshot(words);
        return NCLP_STIM_ERROR_HARDWARE;
    }

    if (action == NCLP_STIM_ACTION_ARM) {
        if ((status & (STIM_STATUS_BUSY | STIM_STATUS_CONFIG_LOCKED)) != 0U) {
            return NCLP_STIM_ERROR_BUSY;
        }
        if ((status & STIM_STATUS_CONFIG_VALID) == 0U ||
            (read_stim_register(STIM_REG_SAFETY_STATUS) &
             STIM_SAFETY_STATUS_SAFE_OFF_ACTIVE) != 0U ||
            (((read_stim_register(STIM_REG_ACTION_CONFIG) & 0x3U) ==
               STIM_OUTPUT_MODE_DAC) &&
             ((prime_status & STIM_DAC_PRIME_STATUS_PRIMED) == 0U))) {
            return NCLP_STIM_ERROR_NOT_READY;
        }
        errors_before = read_stim_register(STIM_REG_ERROR_STATUS) &
                        STIM_ERROR_ALL_MASK;
        write_stim_register(STIM_REG_COMMAND, STIM_COMMAND_ARM);
        for (uint32_t poll = 0U; poll < STIM_COMMAND_POLL_LIMIT; ++poll) {
            status = read_stim_register(STIM_REG_STATUS);
            if ((status & STIM_STATUS_ARMED) != 0U) {
                live_snapshot(words);
                return NCLP_STIM_OK;
            }
            if ((read_stim_register(STIM_REG_ERROR_STATUS) &
                 STIM_ERROR_ALL_MASK & ~errors_before) != 0U) {
                break;
            }
        }
        live_snapshot(words);
        return NCLP_STIM_ERROR_NOT_READY;
    }

    if (action == NCLP_STIM_ACTION_TRIGGER) {
        if ((status & STIM_STATUS_ARMED) == 0U ||
            (read_stim_register(STIM_REG_SAFETY_STATUS) &
             STIM_SAFETY_STATUS_SAFE_OFF_ACTIVE) != 0U) {
            return NCLP_STIM_ERROR_NOT_READY;
        }
        accepted_before = read_stim_register(
            STIM_REG_ACCEPTED_TRIGGER_COUNT);
        unserved_before = read_stim_register(
            STIM_REG_UNSERVED_TRIGGER_COUNT);
        errors_before = read_stim_register(STIM_REG_ERROR_STATUS) &
                        STIM_ERROR_ALL_MASK;
        write_stim_register(STIM_REG_COMMAND, STIM_COMMAND_SOFTWARE_TRIGGER);
        for (uint32_t poll = 0U; poll < STIM_COMMAND_POLL_LIMIT; ++poll) {
            if (read_stim_register(STIM_REG_ACCEPTED_TRIGGER_COUNT) !=
                accepted_before) {
                live_snapshot(words);
                return NCLP_STIM_OK;
            }
            if (read_stim_register(STIM_REG_UNSERVED_TRIGGER_COUNT) !=
                    unserved_before ||
                (read_stim_register(STIM_REG_ERROR_STATUS) &
                 STIM_ERROR_ALL_MASK & ~errors_before) != 0U) {
                break;
            }
        }
        live_snapshot(words);
        return NCLP_STIM_ERROR_NOT_READY;
    }

    if (action == NCLP_STIM_ACTION_CLEAR) {
        write_stim_register(STIM_REG_COMMAND,
                            STIM_COMMAND_CLEAR_DIAGNOSTICS);
        write_stim_register(STIM_REG_ERROR_STATUS, STIM_ERROR_ALL_MASK);
        g_stim_reported_errors = 0U;
        if ((read_stim_register(STIM_REG_ERROR_STATUS) &
             STIM_ERROR_ALL_MASK) != 0U) {
            live_snapshot(words);
            return NCLP_STIM_ERROR_HARDWARE;
        }
        live_snapshot(words);
        return NCLP_STIM_OK;
    }

    write_stim_register(STIM_REG_COMMAND, STIM_COMMAND_DISARM);
    for (uint32_t poll = 0U; poll < STIM_COMMAND_POLL_LIMIT; ++poll) {
        status = read_stim_register(STIM_REG_STATUS);
        if ((status & (STIM_STATUS_BUSY | STIM_STATUS_TTL_ACTIVE |
                       STIM_STATUS_DAC_RUNNING | STIM_STATUS_DAC_BUSY |
                       STIM_STATUS_DAC_PRIME_BUSY)) == 0U &&
            (status & (STIM_STATUS_CONFIG_LOCKED | STIM_STATUS_ARMED)) == 0U) {
            live_snapshot(words);
            return NCLP_STIM_OK;
        }
    }
    live_snapshot(words);
    return NCLP_STIM_ERROR_HARDWARE;
}

int nclp_stim_set_diagnostics(uint32_t irq_enable_mask,
                              uint32_t error_clear_mask,
                              uint32_t words[4])
{
    if (stim_ready() != NCLP_STIM_OK) {
        return NCLP_STIM_ERROR_NOT_READY;
    }
    if ((irq_enable_mask & ~STIM_ERROR_ALL_MASK) != 0U ||
        (error_clear_mask & ~STIM_ERROR_ALL_MASK) != 0U) {
        return NCLP_STIM_ERROR_ARGUMENT;
    }
    if (register_write_verified(STIM_REG_FAULT_IRQ_ENABLE,
                                irq_enable_mask) != NCLP_STIM_OK) {
        return NCLP_STIM_ERROR_HARDWARE;
    }
    if (error_clear_mask != 0U) {
        write_stim_register(STIM_REG_ERROR_STATUS, error_clear_mask);
        g_stim_reported_errors &= ~error_clear_mask;
    }
    if (words != NULL) {
        words[0] = read_stim_register(STIM_REG_ERROR_STATUS);
        words[1] = read_stim_register(STIM_REG_FAULT_IRQ_ENABLE);
        words[2] = read_stim_register(
            STIM_REG_ERROR_INCIDENT_CYCLE_COUNT);
        words[3] = read_stim_register(STIM_REG_LAST_ERROR_VECTOR);
    }
    return (read_stim_register(STIM_REG_ERROR_STATUS) & error_clear_mask) ==
           0U ? NCLP_STIM_OK : NCLP_STIM_ERROR_HARDWARE;
}
