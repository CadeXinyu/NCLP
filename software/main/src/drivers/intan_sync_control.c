#include "intan_sync_control.h"
#include "../../../common/nclp_pl_registers.h"
#include "xil_io.h"
#include "xparameters.h"
#include <stddef.h>
#define INTAN_BASE ((uintptr_t)XPAR_NCLP_INTAN_SPI_MODULE_BD_0_BASEADDR)
static uint32_t reg_read(uintptr_t base, uint32_t offset)
{ return Xil_In32((UINTPTR)(base + offset)); }
static void reg_write(uintptr_t base, uint32_t offset, uint32_t value)
{ Xil_Out32((UINTPTR)(base + offset), value); }

void nclp_intan_sync_read(uint32_t words[4])
{
    if (words == NULL) return;
    words[0] = reg_read(INTAN_BASE, INTAN_REG_SYNC_MODE);
    words[1] = reg_read(INTAN_BASE, INTAN_REG_SYNC_PERIOD_FRAMES);
    words[2] = reg_read(INTAN_BASE, INTAN_REG_SYNC_HIGH_FRAMES);
    words[3] = reg_read(INTAN_BASE, INTAN_REG_ACQUISITION_STATUS);
}

int nclp_intan_sync_set(uint32_t mode, uint32_t period_frames,
                                 uint32_t high_frames, uint32_t words[4])
{
    uint32_t acquisition_status = reg_read(
        INTAN_BASE, INTAN_REG_ACQUISITION_STATUS);
    uint32_t stored_period = period_frames;
    uint32_t stored_high = high_frames;

    if (words == NULL || mode > INTAN_SYNC_MODE_RECORDING_GATE) {
        return NCLP_INTAN_SYNC_ERROR_ARGUMENT;
    }
    if (mode == INTAN_SYNC_MODE_PERIODIC) {
        if (period_frames == 0U || period_frames > 0xFFFFU ||
            high_frames == 0U || high_frames > period_frames) {
            return NCLP_INTAN_SYNC_ERROR_ARGUMENT;
        }
    } else {
        /* Non-periodic timing arguments are deliberately canonical on the
         * wire.  The hardware stores 1/1 so a later periodic transition can
         * never expose an intermediate divide-by-zero configuration. */
        if (period_frames != 0U || high_frames != 0U) {
            return NCLP_INTAN_SYNC_ERROR_ARGUMENT;
        }
        stored_period = 1U;
        stored_high = 1U;
    }
    if ((acquisition_status &
         (INTAN_ACQUISITION_STATUS_RUNNING |
          INTAN_ACQUISITION_STATUS_START_PENDING |
          INTAN_ACQUISITION_STATUS_ACCESS_LOCKED)) != 0U) {
        return NCLP_INTAN_SYNC_ERROR_BUSY;
    }

    /* Keep the tuple valid throughout the three separate AXI writes.  The
     * SPI domain snapshots it only on the next acquisition START. */
    reg_write(INTAN_BASE, INTAN_REG_SYNC_MODE, INTAN_SYNC_MODE_OFF);
    reg_write(INTAN_BASE, INTAN_REG_SYNC_PERIOD_FRAMES, stored_period);
    reg_write(INTAN_BASE, INTAN_REG_SYNC_HIGH_FRAMES, stored_high);
    reg_write(INTAN_BASE, INTAN_REG_SYNC_MODE, mode);
    nclp_intan_sync_read(words);
    if (words[0] != mode || words[1] != stored_period ||
        words[2] != stored_high) {
        return NCLP_INTAN_SYNC_ERROR_HARDWARE;
    }
    return NCLP_INTAN_SYNC_OK;
}
