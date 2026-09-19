#ifndef NCLP_COMPUTE_CONTROL_H
#define NCLP_COMPUTE_CONTROL_H

#include <stdint.h>

/* A53-0 owns the Intan destination and local decoder stream mask.  The PL
 * validates SFP trigger packets and presents only a pulse to stimulation;
 * DAC configuration remains in the stimulation AXI-Lite block. */
int nclp_compute_init(void);
/* Revalidate only the SFP mailbox ABI without resetting compute-fabric
 * routing, masks, counters, or diagnostics. */
int nclp_compute_refresh_sfp_mailbox_identity(void);

/* Mode and stream configuration. */
uint32_t nclp_compute_sfp_mode_selected(void);
int nclp_compute_sfp_link_ready(void);
int nclp_compute_select_sfp_mode(uint32_t sfp_mode_selected);
/* Make the compute destination follow the usable physical SFP link.  This is
 * allowed only while the compute fabric reports CONFIG_IDLE. */
int nclp_compute_sync_transport_mode(void);
int nclp_compute_set_local_stream_mask(uint32_t stream_mask);

/* Live status and diagnostics. */
uint32_t nclp_compute_fabric_status(void);
/* STATUS, LINK_STATUS, malformed RX packets, accepted stimulation triggers. */
void nclp_compute_read_status(uint32_t words[4]);
int nclp_compute_fault_pending(void);
int nclp_compute_clear_fault(void);
int nclp_compute_clear_intan_end_of_stream(void);

#endif
