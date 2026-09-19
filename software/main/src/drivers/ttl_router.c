#include "ttl_router.h"

#include "../../../common/nclp_pl_registers.h"
#include "xil_io.h"

#include <stddef.h>

static int initialized;

static uint32_t read_reg(uint32_t offset)
{
    return Xil_In32((UINTPTR)(NCLP_TTL_ROUTER_BASE_DEFAULT + offset));
}

static void write_reg(uint32_t offset, uint32_t value)
{
    Xil_Out32((UINTPTR)(NCLP_TTL_ROUTER_BASE_DEFAULT + offset), value);
}

int nclp_ttl_router_route_valid(uint32_t ttl0_source, uint32_t ttl1_source)
{
    return ttl0_source <= TTL_ROUTER_SOURCE_TRIGGER_MONITOR &&
           ttl1_source <= TTL_ROUTER_SOURCE_TRIGGER_MONITOR &&
           (ttl0_source == TTL_ROUTER_SOURCE_OFF || ttl0_source != ttl1_source);
}

static int store_route(uint32_t route)
{
    if (read_reg(TTL_ROUTER_REG_STATUS) & TTL_ROUTER_STATUS_ROUTE_WRITE_LOCKED)
        return NCLP_TTL_ROUTER_ERROR_BUSY;
    write_reg(TTL_ROUTER_REG_ROUTE, route);
    if (read_reg(TTL_ROUTER_REG_ROUTE) != route ||
        (read_reg(TTL_ROUTER_REG_STATUS) &
         (TTL_ROUTER_STATUS_ROUTE_CONFLICT | TTL_ROUTER_STATUS_ROUTE_WRITE_LOCKED)))
        return NCLP_TTL_ROUTER_ERROR_HARDWARE;
    return NCLP_TTL_ROUTER_OK;
}

int nclp_ttl_router_init(void)
{
    int result;
    initialized = 0;
    if (read_reg(TTL_ROUTER_REG_BLOCK_ID) != TTL_ROUTER_BLOCK_ID_EXPECTED ||
        (read_reg(TTL_ROUTER_REG_ABI_VERSION) & NCLP_ABI_MAJOR_MASK) !=
        (TTL_ROUTER_ABI_VERSION_EXPECTED & NCLP_ABI_MAJOR_MASK) ||
        (read_reg(TTL_ROUTER_REG_CAPABILITIES) & TTL_ROUTER_CAPABILITIES_REQUIRED) !=
        TTL_ROUTER_CAPABILITIES_REQUIRED ||
        read_reg(TTL_ROUTER_REG_INFO) != TTL_ROUTER_INFO_EXPECTED)
        return NCLP_TTL_ROUTER_ERROR_HARDWARE;
    result = store_route(0);
    if (result != NCLP_TTL_ROUTER_OK) return result;
    write_reg(TTL_ROUTER_REG_ERROR_STATUS, TTL_ROUTER_ERROR_ALL_MASK);
    if (read_reg(TTL_ROUTER_REG_ERROR_STATUS) != 0)
        return NCLP_TTL_ROUTER_ERROR_HARDWARE;
    initialized = 1;
    return NCLP_TTL_ROUTER_OK;
}

int nclp_ttl_router_set_route(uint32_t ttl0_source, uint32_t ttl1_source)
{
    if (!nclp_ttl_router_route_valid(ttl0_source, ttl1_source))
        return NCLP_TTL_ROUTER_ERROR_ARGUMENT;
    if (!initialized) return NCLP_TTL_ROUTER_ERROR_NOT_READY;
    return store_route((ttl0_source << TTL_ROUTER_TTL0_SOURCE_SHIFT) |
                       (ttl1_source << TTL_ROUTER_TTL1_SOURCE_SHIFT));
}

int nclp_ttl_router_get_route(uint32_t *route)
{
    if (route == NULL) return NCLP_TTL_ROUTER_ERROR_ARGUMENT;
    if (!initialized) return NCLP_TTL_ROUTER_ERROR_NOT_READY;
    *route = read_reg(TTL_ROUTER_REG_ROUTE);
    return NCLP_TTL_ROUTER_OK;
}

int nclp_ttl_router_get_status(uint32_t *status, uint32_t *errors)
{
    if (status == NULL || errors == NULL) return NCLP_TTL_ROUTER_ERROR_ARGUMENT;
    if (!initialized) return NCLP_TTL_ROUTER_ERROR_NOT_READY;
    *status = read_reg(TTL_ROUTER_REG_STATUS);
    *errors = read_reg(TTL_ROUTER_REG_ERROR_STATUS);
    return NCLP_TTL_ROUTER_OK;
}

int nclp_ttl_router_clear_errors(uint32_t mask)
{
    if (mask & ~TTL_ROUTER_ERROR_ALL_MASK) return NCLP_TTL_ROUTER_ERROR_ARGUMENT;
    if (!initialized) return NCLP_TTL_ROUTER_ERROR_NOT_READY;
    write_reg(TTL_ROUTER_REG_ERROR_STATUS, mask);
    return (read_reg(TTL_ROUTER_REG_ERROR_STATUS) & mask) ?
        NCLP_TTL_ROUTER_ERROR_HARDWARE : NCLP_TTL_ROUTER_OK;
}
