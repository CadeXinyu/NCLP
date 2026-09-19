#include "led_control.h"

#include "../../../common/nclp_pl_registers.h"

#include "xil_io.h"
#include "xil_printf.h"

#include <stdint.h>

#define LED_CTRL_BASE ((uintptr_t)NCLP_LED_BASE_DEFAULT)

#define LED_PHYSICAL_PORT_A_MASK 0x03U
#define LED_PHYSICAL_PORT_B_MASK 0x0CU
#define LED_PHYSICAL_PORT_C_MASK 0x30U
#define LED_PHYSICAL_PORT_D_MASK 0xC0U

static uint32_t g_led_initialized;
static uint32_t g_led_on_mask;

static uint32_t read_led_register(uint32_t offset)
{
    return Xil_In32((UINTPTR)(LED_CTRL_BASE + offset));
}

static void write_led_register(uint32_t offset, uint32_t value)
{
    Xil_Out32((UINTPTR)(LED_CTRL_BASE + offset), value);
}

static uint32_t make_led_on_mask(uint8_t physical_chip_mask,
                                 uint32_t init_valid, uint32_t fault)
{
    uint32_t mask = 0U;

    if (init_valid != 0U) {
        if ((physical_chip_mask & LED_PHYSICAL_PORT_A_MASK) != 0U) {
            mask |= LED_ON_A_MASK;
        }
        if ((physical_chip_mask & LED_PHYSICAL_PORT_B_MASK) != 0U) {
            mask |= LED_ON_B_MASK;
        }
        if ((physical_chip_mask & LED_PHYSICAL_PORT_C_MASK) != 0U) {
            mask |= LED_ON_C_MASK;
        }
        if ((physical_chip_mask & LED_PHYSICAL_PORT_D_MASK) != 0U) {
            mask |= LED_ON_D_MASK;
        }
    }
    if (fault != 0U) {
        mask |= LED_ON_ERROR_MASK;
    }
    return mask;
}

int nclp_led_init(void)
{
    uint32_t block_id;
    uint32_t abi_version;
    uint32_t capabilities;
    uint32_t info;
    uint32_t readback;

    g_led_initialized = 0U;
    g_led_on_mask = 0U;
    block_id = read_led_register(LED_REG_BLOCK_ID);
    abi_version = read_led_register(LED_REG_ABI_VERSION);
    capabilities = read_led_register(LED_REG_CAPABILITIES);
    info = read_led_register(LED_REG_INFO);
    if (block_id != LED_BLOCK_ID_EXPECTED ||
        (abi_version & NCLP_ABI_MAJOR_MASK) !=
            (LED_ABI_VERSION_EXPECTED & NCLP_ABI_MAJOR_MASK) ||
        (capabilities & LED_CAPABILITIES_REQUIRED) !=
            LED_CAPABILITIES_REQUIRED ||
        info != LED_INFO_EXPECTED) {
        xil_printf("  FAIL PL LED identity id=0x%08lx abi=0x%08lx caps=0x%08lx info=0x%08lx\r\n",
                   (unsigned long)block_id,
                   (unsigned long)abi_version,
                   (unsigned long)capabilities,
                   (unsigned long)info);
        return -1;
    }

    write_led_register(LED_REG_SOFTWARE_ON_MASK, 0U);
    readback = read_led_register(LED_REG_SOFTWARE_ON_MASK) & LED_ON_ALL_MASK;
    if (readback != 0U) {
        xil_printf("  FAIL PL LED clear readback=0x%02lx\r\n",
                   (unsigned long)readback);
        return -1;
    }

    g_led_initialized = 1U;
    xil_printf("  PASS PL LED control          abi=0x%08lx\r\n",
               (unsigned long)abi_version);
    return 0;
}

void nclp_led_update(uint8_t physical_chip_mask, uint32_t init_valid,
                     uint32_t fault)
{
    uint32_t desired_mask;
    uint32_t readback;

    if (g_led_initialized == 0U) {
        return;
    }

    desired_mask = make_led_on_mask(physical_chip_mask, init_valid, fault);
    if (desired_mask == g_led_on_mask) {
        return;
    }

    write_led_register(LED_REG_SOFTWARE_ON_MASK, desired_mask);
    readback = read_led_register(LED_REG_SOFTWARE_ON_MASK) & LED_ON_ALL_MASK;
    g_led_on_mask = desired_mask;
    if (readback != desired_mask) {
        xil_printf("  FAIL PL LED write mask=0x%02lx readback=0x%02lx\r\n",
                   (unsigned long)desired_mask,
                   (unsigned long)readback);
    }
}
