#ifndef NCLP_MAIN_TTL_ROUTER_H
#define NCLP_MAIN_TTL_ROUTER_H
#include <stdint.h>
#define NCLP_TTL_ROUTER_OK 0
#define NCLP_TTL_ROUTER_ERROR_HARDWARE (-1)
#define NCLP_TTL_ROUTER_ERROR_ARGUMENT (-2)
#define NCLP_TTL_ROUTER_ERROR_BUSY (-3)
#define NCLP_TTL_ROUTER_ERROR_NOT_READY (-4)
/* Pure validation; duplicate non-OFF sources are invalid. */
int nclp_ttl_router_route_valid(uint32_t ttl0_source, uint32_t ttl1_source);
/* Call after the owning controller has been disarmed/quiesced. Identity must
 * match before any write; success proves OFF/OFF and cleared diagnostics. */
int nclp_ttl_router_init(void);
int nclp_ttl_router_set_route(uint32_t ttl0_source, uint32_t ttl1_source);
int nclp_ttl_router_get_route(uint32_t *route);
int nclp_ttl_router_get_status(uint32_t *status, uint32_t *errors);
int nclp_ttl_router_clear_errors(uint32_t mask);
#endif
