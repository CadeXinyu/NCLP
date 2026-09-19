#ifndef NCLP_SFP_CONTROL_H
#define NCLP_SFP_CONTROL_H

#include <stdint.h>

/* A53-1 fixed binary command/reply transport through the PS/SFP AXI-Lite
 * mailbox. The mailbox owns packet retention and interrupted-TX replay. */
int nclp_sfp_control_init(void);
/* Returns true only when the mailbox ABI is valid and both compute-fabric and
 * mailbox status report a usable physical SFP link. */
int nclp_sfp_link_usable(void);
void nclp_sfp_control_set_command_ingress_enabled(uint32_t enabled);
int nclp_sfp_control_path_idle(void);
void nclp_sfp_control_poll(void);

#endif
