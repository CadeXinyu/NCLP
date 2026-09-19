#ifndef NCLP_MAIN_PS_ETHERNET_H
#define NCLP_MAIN_PS_ETHERNET_H

#include <stdint.h>

int nclp_ps_ethernet_init(void);
/* Disables RJ45 command execution and UDP application data without stopping
 * the TCP listener or lwIP link/timer maintenance. New connections receive the
 * fixed SFP-owner response and close. A reply already owned by TCP is allowed
 * to enter lwIP before the existing client is retired. */
void nclp_ps_ethernet_set_application_enabled(uint32_t enabled);
int nclp_ps_ethernet_control_idle(void);
/* Drops a muted client's completed-but-unsent software reply and closes the
 * old connection. The caller must first prove the command service is idle. */
int nclp_ps_ethernet_force_retire_control(void);
void nclp_ps_ethernet_poll(void);
int nclp_ps_ethernet_send_datagram(const void *data, uint16_t bytes);
int nclp_ps_ethernet_set_udp_destination(uint32_t ipv4, uint32_t port);

#endif
