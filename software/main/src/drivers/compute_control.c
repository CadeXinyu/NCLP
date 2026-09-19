#include "compute_control.h"
#include "../../../common/nclp_pl_registers.h"
#include "xil_io.h"

#include <stddef.h>
#include <stdint.h>

static uint32_t g_compute_fabric_ready;
static uint32_t g_sfp_mailbox_ready;

static uint32_t fabric_read(uint32_t offset)
{
    return Xil_In32((UINTPTR)(NCLP_COMPUTE_FABRIC_BASE_DEFAULT + offset));
}

static void fabric_write(uint32_t offset, uint32_t value)
{
    Xil_Out32((UINTPTR)(NCLP_COMPUTE_FABRIC_BASE_DEFAULT + offset), value);
}

static uint32_t mailbox_read(uint32_t offset)
{
    return Xil_In32((UINTPTR)(NCLP_SFP_MAILBOX_BASE_DEFAULT + offset));
}

static int mailbox_identity_valid(void)
{
    return mailbox_read(SFP_MAILBOX_REG_BLOCK_ID) ==
               SFP_MAILBOX_BLOCK_ID_EXPECTED &&
           mailbox_read(SFP_MAILBOX_REG_ABI_VERSION) ==
               SFP_MAILBOX_ABI_VERSION_EXPECTED &&
           (mailbox_read(SFP_MAILBOX_REG_CAPABILITIES) &
            SFP_MAILBOX_CAPABILITIES_REQUIRED) ==
               SFP_MAILBOX_CAPABILITIES_REQUIRED &&
           mailbox_read(SFP_MAILBOX_REG_INFO) == SFP_MAILBOX_INFO_EXPECTED;
}

int nclp_compute_refresh_sfp_mailbox_identity(void)
{
    if (g_compute_fabric_ready == 0U) {
        g_sfp_mailbox_ready = 0U;
        return -1;
    }
    g_sfp_mailbox_ready = mailbox_identity_valid() != 0 ? 1U : 0U;
    return g_sfp_mailbox_ready != 0U ? 0 : -1;
}

uint32_t nclp_compute_sfp_mode_selected(void)
{
    if (g_compute_fabric_ready == 0U) {
        return 0U;
    }
    return (fabric_read(COMPUTE_FABRIC_REG_CONTROL) &
            COMPUTE_FABRIC_CONTROL_SFP_MODE_SELECT) != 0U ? 1U : 0U;
}

int nclp_compute_sfp_link_ready(void)
{
    uint32_t link_status;

    if (g_compute_fabric_ready == 0U || g_sfp_mailbox_ready == 0U) {
        return 0;
    }
    link_status = fabric_read(COMPUTE_FABRIC_REG_LINK_STATUS);
    return (link_status & (COMPUTE_FABRIC_LINK_STATUS_UP |
                           COMPUTE_FABRIC_LINK_STATUS_FAULT)) ==
                          COMPUTE_FABRIC_LINK_STATUS_UP &&
           (mailbox_read(SFP_MAILBOX_REG_STATUS) &
            SFP_MAILBOX_STATUS_LINK_UP) != 0U;
}

int nclp_compute_select_sfp_mode(uint32_t sfp_mode_selected)
{
    uint32_t control;
    uint32_t status;

    if (g_compute_fabric_ready == 0U || sfp_mode_selected > 1U) {
        return -1;
    }
    status = fabric_read(COMPUTE_FABRIC_REG_STATUS);
    if ((status & COMPUTE_FABRIC_STATUS_CONFIG_IDLE) == 0U ||
        (sfp_mode_selected != 0U && nclp_compute_sfp_link_ready() == 0)) {
        return -1;
    }

    control = fabric_read(COMPUTE_FABRIC_REG_CONTROL) &
              (COMPUTE_FABRIC_CONTROL_PHY_ENABLE |
               COMPUTE_FABRIC_CONTROL_FAULT_IRQ_ENABLE);
    if (sfp_mode_selected != 0U) {
        control |= COMPUTE_FABRIC_CONTROL_SFP_MODE_SELECT;
    }
    fabric_write(COMPUTE_FABRIC_REG_CONTROL, control);
    return nclp_compute_sfp_mode_selected() == sfp_mode_selected ? 0 : -1;
}

int nclp_compute_sync_transport_mode(void)
{
    uint32_t sfp_link_ready;

    if (g_compute_fabric_ready == 0U) {
        return -1;
    }
    if (g_sfp_mailbox_ready == 0U) {
        (void)nclp_compute_refresh_sfp_mailbox_identity();
    }
    sfp_link_ready = nclp_compute_sfp_link_ready() != 0 ? 1U : 0U;
    if (nclp_compute_sfp_mode_selected() == sfp_link_ready) {
        return 0;
    }
    return nclp_compute_select_sfp_mode(sfp_link_ready);
}

int nclp_compute_set_local_stream_mask(uint32_t stream_mask)
{
    if (g_compute_fabric_ready == 0U || stream_mask == 0U ||
        (stream_mask & 0xFFFF0000U) != 0U) {
        return -1;
    }
    if ((fabric_read(COMPUTE_FABRIC_REG_STATUS) &
         COMPUTE_FABRIC_STATUS_CONFIG_IDLE) == 0U) {
        return -1;
    }
    fabric_write(COMPUTE_FABRIC_REG_SOURCE_STREAM_MASK, stream_mask);
    return fabric_read(COMPUTE_FABRIC_REG_SOURCE_STREAM_MASK) == stream_mask ?
        0 : -1;
}

int nclp_compute_fault_pending(void)
{
    uint32_t fabric_fault;
    uint32_t selected_link_fault = 0U;

    if (g_compute_fabric_ready == 0U) {
        return 0;
    }
    fabric_fault = fabric_read(COMPUTE_FABRIC_REG_STATUS) &
                   COMPUTE_FABRIC_STATUS_FAULT_MASK;
    if (nclp_compute_sfp_mode_selected() != 0U) {
        selected_link_fault =
            fabric_read(COMPUTE_FABRIC_REG_LINK_STATUS) &
            COMPUTE_FABRIC_LINK_STATUS_FAULT;
    }
    return (fabric_fault | selected_link_fault) != 0U;
}

int nclp_compute_clear_fault(void)
{
    uint32_t status;

    if (g_compute_fabric_ready == 0U) {
        return -1;
    }
    fabric_write(COMPUTE_FABRIC_REG_COMMAND,
                 COMPUTE_FABRIC_COMMAND_CLEAR_DIAGNOSTICS);
    status = fabric_read(COMPUTE_FABRIC_REG_STATUS);
    if ((status & COMPUTE_FABRIC_STATUS_FAULT_MASK) != 0U) {
        return -1;
    }
    if (nclp_compute_sfp_mode_selected() != 0U &&
        (fabric_read(COMPUTE_FABRIC_REG_LINK_STATUS) &
         COMPUTE_FABRIC_LINK_STATUS_FAULT) != 0U) {
        return -1;
    }
    return 0;
}

void nclp_compute_read_status(uint32_t words[4])
{
    if (words == NULL) {
        return;
    }
    words[0] = fabric_read(COMPUTE_FABRIC_REG_STATUS);
    words[1] = fabric_read(COMPUTE_FABRIC_REG_LINK_STATUS);
    words[2] = fabric_read(COMPUTE_FABRIC_REG_RX_MALFORMED_PACKET_COUNT);
    words[3] = fabric_read(COMPUTE_FABRIC_REG_RX_STIM_TRIGGER_COUNT);
}

uint32_t nclp_compute_fabric_status(void)
{
    return fabric_read(COMPUTE_FABRIC_REG_STATUS);
}

int nclp_compute_clear_intan_end_of_stream(void)
{
    uint32_t status;

    if (g_compute_fabric_ready == 0U) {
        return -1;
    }
    status = fabric_read(COMPUTE_FABRIC_REG_STATUS);
    if ((status & COMPUTE_FABRIC_STATUS_CONFIG_IDLE) == 0U) {
        return -1;
    }
    fabric_write(COMPUTE_FABRIC_REG_COMMAND,
                 COMPUTE_FABRIC_COMMAND_CLEAR_INTAN_EOS);
    status = fabric_read(COMPUTE_FABRIC_REG_STATUS);
    return (status & (COMPUTE_FABRIC_STATUS_CONFIG_IDLE |
                      COMPUTE_FABRIC_STATUS_INTAN_EOS_SEEN)) ==
                     COMPUTE_FABRIC_STATUS_CONFIG_IDLE ? 0 : -1;
}

int nclp_compute_init(void)
{
    const uint32_t initial_control =
        COMPUTE_FABRIC_CONTROL_PHY_ENABLE |
        COMPUTE_FABRIC_CONTROL_FAULT_IRQ_ENABLE;

    g_compute_fabric_ready = 0U;
    g_sfp_mailbox_ready = 0U;
    if (fabric_read(COMPUTE_FABRIC_REG_BLOCK_ID) !=
            COMPUTE_FABRIC_BLOCK_ID_EXPECTED ||
        (fabric_read(COMPUTE_FABRIC_REG_ABI_VERSION) &
         NCLP_ABI_MAJOR_MASK) !=
            (COMPUTE_FABRIC_ABI_VERSION_EXPECTED & NCLP_ABI_MAJOR_MASK) ||
        (fabric_read(COMPUTE_FABRIC_REG_CAPABILITIES) &
         COMPUTE_FABRIC_CAPABILITIES_REQUIRED) !=
            COMPUTE_FABRIC_CAPABILITIES_REQUIRED) {
        return -1;
    }
    if ((fabric_read(COMPUTE_FABRIC_REG_STATUS) &
         COMPUTE_FABRIC_STATUS_CONFIG_IDLE) == 0U) {
        return -1;
    }

    g_compute_fabric_ready = 1U;
    /* A broken SFP mailbox must not block the local Ethernet path. It simply
     * makes the automatic SFP route ineligible until a later refresh. */
    (void)nclp_compute_refresh_sfp_mailbox_identity();
    fabric_write(COMPUTE_FABRIC_REG_CONTROL, initial_control);
    if ((fabric_read(COMPUTE_FABRIC_REG_CONTROL) &
         (COMPUTE_FABRIC_CONTROL_PHY_ENABLE |
          COMPUTE_FABRIC_CONTROL_SFP_MODE_SELECT |
          COMPUTE_FABRIC_CONTROL_FAULT_IRQ_ENABLE)) != initial_control ||
        nclp_compute_clear_intan_end_of_stream() != 0 ||
        nclp_compute_clear_fault() != 0) {
        g_compute_fabric_ready = 0U;
        return -1;
    }
    return 0;
}
