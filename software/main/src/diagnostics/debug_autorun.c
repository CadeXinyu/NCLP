#include "debug_autorun.h"

#include "../protocol/nclp_main.h"

#include "xil_printf.h"

#include <string.h>

#define NCLP_DEBUG_OPERATION_OK       0
#define NCLP_DEBUG_OPERATION_NO_CHIP  1
#define NCLP_DEBUG_OPERATION_FAILED  (-1)

static int run_debug_command(uint32_t command_id, uint32_t sequence)
{
    nclp_main_command_t command;
    nclp_main_reply_t reply;

    memset(&command, 0, sizeof(command));
    command.command = command_id;
    command.sequence = sequence;
    if (command_id == NCLP_CMD_INIT) {
        command.args[0] = NCLP_INIT_FIRST_STREAM;
        command.args[1] = 1U;
    }
    if (nclp_main_execute_command(&command, &reply) != 0) {
        if (reply.status == NCLP_COMMAND_STATUS_NO_CHIP_DETECTED) {
            xil_printf("  INFO debug SCAN found no supported RHD chip; controller remains IDLE\r\n");
            return NCLP_DEBUG_OPERATION_NO_CHIP;
        }
        xil_printf("  FAIL debug operation cmd=0x%02lx status=%lu failures=%lu\r\n",
                   (unsigned long)command_id,
                   (unsigned long)reply.status,
                   (unsigned long)reply.fail_count);
        return NCLP_DEBUG_OPERATION_FAILED;
    }
    xil_printf("  PASS debug operation cmd=0x%02lx result=%lu count=%lu\r\n",
               (unsigned long)command_id,
               (unsigned long)reply.data0,
               (unsigned long)reply.data1);
    return NCLP_DEBUG_OPERATION_OK;
}

int nclp_debug_autorun(void)
{
    int scan_status;

    xil_printf("\r\nNCLP debug mode 1: automatic scan -> init -> impedance\r\n");
    scan_status = run_debug_command(NCLP_CMD_SCAN, 1U);
    if (scan_status == NCLP_DEBUG_OPERATION_NO_CHIP) {
        xil_printf("RESULT NO CHIP: debug autorun stopped without hardware fault\r\n");
        return 0;
    }
    if (scan_status != NCLP_DEBUG_OPERATION_OK ||
        run_debug_command(NCLP_CMD_INIT, 2U) != 0 ||
        run_debug_command(NCLP_CMD_IMPEDANCE, 3U) != 0) {
        xil_printf("RESULT FAIL: debug autorun stopped\r\n");
        return -1;
    }
    xil_printf("RESULT PASS: debug autorun complete; TCP results remain available\r\n");
    return 0;
}
