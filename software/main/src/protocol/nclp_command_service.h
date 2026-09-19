#ifndef NCLP_COMMAND_SERVICE_H
#define NCLP_COMMAND_SERVICE_H
#include "nclp_main.h"
/* A53-1 command arbitration and snapshot access, shared by TCP and SFP.
 * A sink returns 0 after accepting a reply, -1 to retry without re-execution.
 * token is transport-owned correlation data; the service does not interpret it. */
typedef int (*nclp_command_reply_fn)(uint32_t token, const nclp_main_reply_t *reply);
void nclp_command_service_init(void);
int nclp_command_service_submit(const nclp_main_command_t *command,
                                nclp_command_reply_fn send_reply, uint32_t token);
void nclp_command_service_poll(void);
int nclp_command_service_pending(void);
#endif
