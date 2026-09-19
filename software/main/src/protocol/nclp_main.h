#ifndef NCLP_MAIN_H
#define NCLP_MAIN_H

#include <stddef.h>
#include <stdint.h>
#include "../../../common/nclp_wire.h"

/* A53-0 executes board commands. Shared wire constants live in nclp_wire.h.
 * This decoded command omits the magic/version already checked by transport. */

typedef struct {
    uint32_t command;
    uint32_t sequence;
    uint32_t args[NCLP_CMD_WORDS - 4U];
} nclp_main_command_t;

typedef struct {
    uint32_t magic;
    uint32_t version;
    uint32_t command;
    uint32_t sequence;
    uint32_t status;
    uint32_t data0;
    uint32_t data1;
    uint32_t data2;
    uint32_t data3;
    uint32_t fail_count;
} nclp_main_reply_t;

int nclp_main_execute_command(const nclp_main_command_t *command,
                              nclp_main_reply_t *reply);

#endif
