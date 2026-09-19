#ifndef NCLP_OUTPUT_COMMANDS_H
#define NCLP_OUTPUT_COMMANDS_H
#include "nclp_main.h"

/* A53-0 supplies one admission snapshot. The output dispatcher owns no
 * acquisition state and leaves reply identity/fail_count to its caller. */
typedef struct {
    uint32_t streaming;
    uint32_t operation_state;
    int stopped_configuration_allowed;
    uint32_t physical_chip_mask;
    uint32_t packed_chip_ids;
    uint32_t local_detector_stream_active;
} nclp_output_context_t;

/* Quiesce STIM, then verify and initialize the independent TTL router OFF. */
int nclp_output_init(void);

/* Returns 1 for a handled command, including a rejected command; 0 otherwise. */
int nclp_output_execute(const nclp_main_command_t *command,
                         nclp_main_reply_t *reply,
                         const nclp_output_context_t *context);
#endif
