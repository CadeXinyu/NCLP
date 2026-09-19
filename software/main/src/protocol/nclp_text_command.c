#include "nclp_text_command.h"

#include <stddef.h>
#include <string.h>

typedef struct {
    const char *name;
    uint32_t value;
} nclp_named_uint_t;

static int hex_digit_value(char character)
{
    if (character >= '0' && character <= '9') {
        return character - '0';
    }
    if (character >= 'a' && character <= 'f') {
        return character - 'a' + 10;
    }
    if (character >= 'A' && character <= 'F') {
        return character - 'A' + 10;
    }
    return -1;
}

/* Parse exactly one unsigned token while leaving the cursor at its trailing
 * space or NUL.  Text controls accept decimal and explicit 0x hexadecimal;
 * signs, empty 0x prefixes, trailing junk, and uint32 overflow are rejected. */
static int parse_uint_token(const char **text_inout, uint32_t *value_out)
{
    const char *text = *text_inout;
    uint32_t base = 10U;
    uint32_t value = 0U;
    uint32_t digits = 0U;

    while (*text == ' ') {
        ++text;
    }
    if (text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        base = 16U;
        text += 2;
    }
    for (;;) {
        int parsed = hex_digit_value(*text);
        uint32_t digit;

        if (parsed < 0 || (uint32_t)parsed >= base) {
            break;
        }
        digit = (uint32_t)parsed;
        if (value > (UINT32_MAX - digit) / base) {
            return -1;
        }
        value = value * base + digit;
        ++digits;
        ++text;
    }
    if (digits == 0U || (*text != ' ' && *text != '\0')) {
        return -1;
    }
    *text_inout = text;
    *value_out = value;
    return 0;
}

static int parse_uint_list(const char *text, uint32_t *values,
                           uint32_t capacity, uint32_t *count_out)
{
    uint32_t count = 0U;

    for (;;) {
        while (*text == ' ') {
            ++text;
        }
        if (*text == '\0') {
            *count_out = count;
            return 0;
        }
        if (count >= capacity ||
            parse_uint_token(&text, &values[count]) != 0) {
            return -1;
        }
        ++count;
    }
}

static int parse_exact_uints(const char *text, uint32_t *values,
                             uint32_t expected_count)
{
    uint32_t count = 0U;

    return parse_uint_list(text, values, expected_count, &count) == 0 &&
           count == expected_count ? 0 : -1;
}

static int parse_uint(const char *text, uint32_t *value_out)
{
    return parse_exact_uints(text, value_out, 1U);
}

static int parse_named_uint_token(const char **text_inout,
                                  const nclp_named_uint_t *names,
                                  uint32_t name_count, uint32_t *value_out)
{
    const char *text = *text_inout;
    const char *token;
    const char *end;

    while (*text == ' ') {
        ++text;
    }
    token = text;
    while (*text != '\0' && *text != ' ') {
        ++text;
    }
    end = text;
    if (end == token) {
        return -1;
    }
    for (uint32_t index = 0U; index < name_count; ++index) {
        size_t length = strlen(names[index].name);

        if ((size_t)(end - token) == length &&
            strncmp(token, names[index].name, length) == 0) {
            *text_inout = end;
            *value_out = names[index].value;
            return 0;
        }
    }

    text = token;
    if (parse_uint_token(&text, value_out) != 0 || text != end) {
        return -1;
    }
    *text_inout = end;
    return 0;
}

/* Human-facing DSP cutoffs are entered in Hz.  Keep the binary protocol and
 * shared state integer-only by converting at most three decimal places to
 * millihertz here (for example, "1.25" -> 1250 mHz). */
static int parse_dsp_hz(const char *text, uint32_t values[2])
{
    uint32_t enable = 0U;
    uint32_t whole = 0U;
    uint32_t fraction = 0U;
    uint32_t fraction_digits = 0U;
    uint32_t digits = 0U;

    while (*text == ' ') {
        ++text;
    }
    while (*text >= '0' && *text <= '9') {
        uint32_t digit = (uint32_t)(*text - '0');
        if (enable > (UINT32_MAX - digit) / 10U) {
            return -1;
        }
        enable = (enable * 10U) + digit;
        ++text;
        ++digits;
    }
    if (digits == 0U || enable > 1U || *text != ' ') {
        return -1;
    }
    while (*text == ' ') {
        ++text;
    }
    digits = 0U;
    while (*text >= '0' && *text <= '9') {
        uint32_t digit = (uint32_t)(*text - '0');
        if (whole > (UINT32_MAX - digit) / 10U) {
            return -1;
        }
        whole = (whole * 10U) + digit;
        ++text;
        ++digits;
    }
    if (*text == '.') {
        ++text;
        while (*text >= '0' && *text <= '9' && fraction_digits < 3U) {
            fraction = (fraction * 10U) + (uint32_t)(*text - '0');
            ++text;
            ++fraction_digits;
        }
        if (*text >= '0' && *text <= '9') {
            return -1;
        }
    }
    if (digits == 0U || (whole == 0U && fraction == 0U)) {
        return -1;
    }
    while (fraction_digits < 3U) {
        fraction *= 10U;
        ++fraction_digits;
    }
    if (whole > (UINT32_MAX - fraction) / 1000U) {
        return -1;
    }
    while (*text == ' ') {
        ++text;
    }
    if (*text != '\0') {
        return -1;
    }
    values[0] = enable;
    values[1] = whole * 1000U + fraction;
    return 0;
}

static int parse_ipv4_destination(const char *text, uint32_t *ipv4_out,
                                  uint32_t *port_out)
{
    uint32_t octets[4];
    uint32_t port = NCLP_DATA_PORT;

    for (uint32_t i = 0U; i < 4U; ++i) {
        uint32_t value = 0U;
        uint32_t digits = 0U;

        while (*text >= '0' && *text <= '9') {
            uint32_t digit = (uint32_t)(*text - '0');

            if (value > (255U - digit) / 10U) {
                return -1;
            }
            value = (value * 10U) + digit;
            ++text;
            ++digits;
        }
        if (digits == 0U || value > 255U ||
            (i < 3U && *text != '.')) {
            return -1;
        }
        octets[i] = value;
        if (i < 3U) {
            ++text;
        }
    }
    while (*text == ' ') {
        ++text;
    }
    if (*text != '\0') {
        if (parse_uint(text, &port) != 0 || port == 0U || port > 65535U) {
            return -1;
        }
    }
    *ipv4_out = (octets[0] << 24) | (octets[1] << 16) |
                (octets[2] << 8) | octets[3];
    *port_out = port;
    return 0;
}

int nclp_command_parse_text(const char *text, nclp_main_command_t *command)
{
    static const nclp_named_uint_t preset_names[] = {
        { "SINE", NCLP_DAC_PRESET_SINE },
        { "GAUSSIAN", NCLP_DAC_PRESET_GAUSSIAN },
        { "CONSTANT", NCLP_DAC_PRESET_CONSTANT }
    };
    static const nclp_named_uint_t control_names[] = {
        { "ARM", NCLP_STIM_ACTION_ARM },
        { "DISARM", NCLP_STIM_ACTION_DISARM },
        { "TRIGGER", NCLP_STIM_ACTION_TRIGGER },
        { "CLEAR", NCLP_STIM_ACTION_CLEAR },
        { "PRIME", NCLP_STIM_ACTION_PRIME }
    };
    static const nclp_named_uint_t ripple_control_names[] = {
        { "STOP", NCLP_RIPPLE_ACTION_STOP },
        { "START", NCLP_RIPPLE_ACTION_START }
    };
    static const nclp_named_uint_t ripple_baseline_names[] = {
        { "CANCEL", NCLP_RIPPLE_BASELINE_CANCEL },
        { "START", NCLP_RIPPLE_BASELINE_START }
    };

    memset(command, 0, sizeof(*command));
    command->sequence = 0U;

    if (strcmp(text, "PING") == 0) {
        command->command = NCLP_CMD_PING;
    } else if (strcmp(text, "COMPUTE_STATUS") == 0) {
        command->command = NCLP_CMD_GET_COMPUTE_STATUS;
    } else if (strcmp(text, "STATUS") == 0) {
        command->command = NCLP_CMD_GET_STATUS;
    } else if (strcmp(text, "GET_PROGRESS") == 0) {
        command->command = NCLP_CMD_GET_PROGRESS;
    } else if (strncmp(text, "GET_RESULT ", 11U) == 0) {
        command->command = NCLP_CMD_GET_RESULT;
        if (parse_exact_uints(text + 11U, command->args, 3U) != 0) {
            return -1;
        }
    } else if (strcmp(text, "GET_CONFIG") == 0) {
        command->command = NCLP_CMD_GET_CONFIG;
        command->args[0] = NCLP_CONFIG_SECTION_CORE;
    } else if (strncmp(text, "GET_CONFIG ", 11U) == 0) {
        command->command = NCLP_CMD_GET_CONFIG;
        if (parse_uint(text + 11U, &command->args[0]) != 0) {
            return -1;
        }
    } else if (strncmp(text, "SET_RATE ", 9U) == 0) {
        command->command = NCLP_CMD_SET_RATE;
        if (parse_uint(text + 9U, &command->args[0]) != 0) {
            return -1;
        }
    } else if (strncmp(text, "SET_BANDWIDTH ", 14U) == 0) {
        command->command = NCLP_CMD_SET_BANDWIDTH;
        if (parse_exact_uints(text + 14U, command->args, 2U) != 0) {
            return -1;
        }
    } else if (strncmp(text, "SET_DSP ", 8U) == 0) {
        command->command = NCLP_CMD_SET_DSP;
        if (parse_dsp_hz(text + 8U, command->args) != 0) {
            return -1;
        }
    } else if (strncmp(text, "SET_TTL_SETTLE ", 15U) == 0) {
        command->command = NCLP_CMD_SET_TTL_SETTLE;
        if (parse_exact_uints(text + 15U, command->args, 2U) != 0 ||
            command->args[0] > 1U || command->args[1] > 15U) {
            return -1;
        }
    } else if (strcmp(text, "SCAN") == 0) {
        command->command = NCLP_CMD_SCAN;
    } else if (strcmp(text, "INIT") == 0) {
        command->command = NCLP_CMD_INIT;
        command->args[0] = NCLP_INIT_FIRST_STREAM;
        command->args[1] = 1U;
    } else if (strcmp(text, "IMPEDANCE") == 0) {
        command->command = NCLP_CMD_IMPEDANCE;
    } else if (strcmp(text, "START AUX") == 0) {
        command->command = NCLP_CMD_START_STREAM;
        command->args[4] = NCLP_SAMPLE_MODE_AUX;
    } else if (strcmp(text, "START VDD") == 0) {
        command->command = NCLP_CMD_START_STREAM;
        command->args[4] = NCLP_SAMPLE_MODE_VDD;
    } else if (strcmp(text, "STOP") == 0) {
        command->command = NCLP_CMD_STOP_STREAM;
    } else if (strcmp(text, "RESET") == 0) {
        command->command = NCLP_CMD_RESET;
    } else if (strcmp(text, "CANCEL") == 0) {
        command->command = NCLP_CMD_CANCEL;
    } else if (strcmp(text, "STIM_STATUS") == 0) {
        command->command = NCLP_CMD_STIM_GET;
        command->args[0] = NCLP_STIM_SECTION_LIVE;
    } else if (strncmp(text, "STIM_STATUS ",
                       sizeof("STIM_STATUS ") - 1U) == 0) {
        command->command = NCLP_CMD_STIM_GET;
        if (parse_uint(text + sizeof("STIM_STATUS ") - 1U,
                       &command->args[0]) != 0) {
            return -1;
        }
    } else if (strncmp(text, "STIM_SET_ACTION ",
                       sizeof("STIM_SET_ACTION ") - 1U) == 0) {
        command->command = NCLP_CMD_STIM_SET_ACTION;
        if (parse_exact_uints(text + sizeof("STIM_SET_ACTION ") - 1U,
                              command->args, 6U) != 0) {
            return -1;
        }
    } else if (strncmp(text, "DAC_SET_CONFIG ",
                       sizeof("DAC_SET_CONFIG ") - 1U) == 0) {
        command->command = NCLP_CMD_DAC_SET_CONFIG;
        if (parse_exact_uints(text + sizeof("DAC_SET_CONFIG ") - 1U,
                              command->args, 6U) != 0) {
            return -1;
        }
    } else if (strncmp(text, "DAC_SET_INTAN_TTL ",
                       sizeof("DAC_SET_INTAN_TTL ") - 1U) == 0) {
        command->command = NCLP_CMD_DAC_SET_INTAN_TTL;
        if (parse_uint(text + sizeof("DAC_SET_INTAN_TTL ") - 1U, &command->args[0]) != 0 ||
            command->args[0] == 1U || command->args[0] > 15U) return -1;
    } else if (strncmp(text, "DAC_SET_CLOCK ",
                       sizeof("DAC_SET_CLOCK ") - 1U) == 0) {
        command->command = NCLP_CMD_DAC_SET_CLOCK;
        if (parse_exact_uints(text + sizeof("DAC_SET_CLOCK ") - 1U,
                              command->args, 3U) != 0) {
            return -1;
        }
    } else if (strncmp(text, "DAC_WRITE ",
                       sizeof("DAC_WRITE ") - 1U) == 0) {
        uint32_t count = 0U;

        command->command = NCLP_CMD_DAC_WRITE;
        if (parse_uint_list(text + sizeof("DAC_WRITE ") - 1U,
                            command->args, NCLP_CMD_WORDS - 4U,
                            &count) != 0 ||
            count < 3U || command->args[1] == 0U ||
            command->args[1] > 10U || count != command->args[1] + 2U) {
            return -1;
        }
    } else if (strncmp(text, "DAC_READ ",
                       sizeof("DAC_READ ") - 1U) == 0) {
        command->command = NCLP_CMD_DAC_READ;
        if (parse_exact_uints(text + sizeof("DAC_READ ") - 1U,
                              command->args, 2U) != 0) {
            return -1;
        }
    } else if (strncmp(text, "DAC_PRESET ",
                       sizeof("DAC_PRESET ") - 1U) == 0) {
        const char *arguments = text + sizeof("DAC_PRESET ") - 1U;

        command->command = NCLP_CMD_DAC_PRESET;
        if (parse_named_uint_token(&arguments, preset_names,
                                   sizeof(preset_names) /
                                   sizeof(preset_names[0]),
                                   &command->args[0]) != 0 ||
            parse_exact_uints(arguments, &command->args[1], 9U) != 0) {
            return -1;
        }
    } else if (strncmp(text, "STIM_CONTROL ",
                       sizeof("STIM_CONTROL ") - 1U) == 0) {
        const char *arguments = text + sizeof("STIM_CONTROL ") - 1U;

        command->command = NCLP_CMD_STIM_CONTROL;
        if (parse_named_uint_token(&arguments, control_names,
                                   sizeof(control_names) /
                                   sizeof(control_names[0]),
                                   &command->args[0]) != 0) {
            return -1;
        }
        while (*arguments == ' ') {
            ++arguments;
        }
        if (*arguments != '\0' ||
            (command->args[0] != NCLP_STIM_ACTION_ARM &&
             command->args[0] != NCLP_STIM_ACTION_DISARM &&
             command->args[0] != NCLP_STIM_ACTION_TRIGGER &&
             command->args[0] != NCLP_STIM_ACTION_CLEAR &&
             command->args[0] != NCLP_STIM_ACTION_PRIME)) {
            return -1;
        }
    } else if (strncmp(text, "STIM_DIAGNOSTICS ",
                       sizeof("STIM_DIAGNOSTICS ") - 1U) == 0) {
        command->command = NCLP_CMD_STIM_DIAGNOSTICS;
        if (parse_exact_uints(text + sizeof("STIM_DIAGNOSTICS ") - 1U,
                              command->args, 2U) != 0) {
            return -1;
        }
    } else if (strncmp(text, "SET_INTAN_SYNC ",
                       sizeof("SET_INTAN_SYNC ") - 1U) == 0) {
        command->command = NCLP_CMD_SET_INTAN_SYNC;
        if (parse_exact_uints(text + sizeof("SET_INTAN_SYNC ") - 1U,
                              command->args, 3U) != 0) {
            return -1;
        }
    } else if (strcmp(text, "GET_INTAN_SYNC") == 0) {
        command->command = NCLP_CMD_GET_INTAN_SYNC;
    } else if (strncmp(text, "RIPPLE_GET_CONFIG ", sizeof("RIPPLE_GET_CONFIG ") - 1U) == 0) {
        command->command = NCLP_CMD_RIPPLE_GET_CONFIG;
        if (parse_exact_uints(text + sizeof("RIPPLE_GET_CONFIG ") - 1U, command->args, 1U)) return -1;
    } else if (strncmp(text, "RIPPLE_STATUS ", sizeof("RIPPLE_STATUS ") - 1U) == 0) {
        command->command = NCLP_CMD_RIPPLE_STATUS;
        if (parse_exact_uints(text + sizeof("RIPPLE_STATUS ") - 1U, command->args, 1U)) return -1;
    } else if (strncmp(text, "RIPPLE_APPLY_PROFILE ",
                       sizeof("RIPPLE_APPLY_PROFILE ") - 1U) == 0) {
        command->command = NCLP_CMD_RIPPLE_APPLY_PROFILE;
        if (parse_exact_uints(
                text + sizeof("RIPPLE_APPLY_PROFILE ") - 1U,
                command->args, 10U)) return -1;
    } else if (strncmp(text, "RIPPLE_SET_K ", sizeof("RIPPLE_SET_K ") - 1U) == 0) {
        command->command = NCLP_CMD_RIPPLE_SET_K;
        if (parse_exact_uints(text + sizeof("RIPPLE_SET_K ") - 1U, command->args, 1U)) return -1;
    } else if (strncmp(text, "RIPPLE_BASELINE ",
                       sizeof("RIPPLE_BASELINE ") - 1U) == 0) {
        const char *arguments = text + sizeof("RIPPLE_BASELINE ") - 1U;
        command->command = NCLP_CMD_RIPPLE_BASELINE;
        if (parse_named_uint_token(&arguments, ripple_baseline_names,
                sizeof(ripple_baseline_names) / sizeof(ripple_baseline_names[0]),
                &command->args[0])) return -1;
        while (*arguments == ' ') ++arguments;
        if (command->args[0] == NCLP_RIPPLE_BASELINE_START) {
            if (parse_exact_uints(arguments, &command->args[1], 1U)) return -1;
        } else if (*arguments != '\0') {
            return -1;
        }
    } else if (strncmp(text, "RIPPLE_CONTROL ", sizeof("RIPPLE_CONTROL ") - 1U) == 0) {
        const char *arguments = text + sizeof("RIPPLE_CONTROL ") - 1U;
        command->command = NCLP_CMD_RIPPLE_CONTROL;
        if (parse_named_uint_token(&arguments, ripple_control_names,
                sizeof(ripple_control_names) / sizeof(ripple_control_names[0]), &command->args[0])) return -1;
        while (*arguments == ' ') ++arguments;
        if (*arguments != '\0' || command->args[0] > NCLP_RIPPLE_ACTION_START) return -1;
    } else if (strncmp(text, "SET_UDP_DEST ", 13U) == 0) {
        const char *destination = text + 13U;
        command->command = NCLP_CMD_SET_UDP_DEST;
        if (parse_ipv4_destination(destination, &command->args[0],
                                   &command->args[1]) != 0) {
            return -1;
        }
    } else {
        return -1;
    }
    return 0;
}
