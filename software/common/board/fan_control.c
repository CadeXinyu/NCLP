#include "fan_control.h"

#include "sleep.h"
#include "xil_io.h"
#include "xil_printf.h"
#include "xiltimer.h"
#include "xparameters.h"
#include "xstatus.h"
#include "xsysmonpsu.h"
#include "xttcps.h"

#include <stdint.h>

/* TTC0 channel 2 drives the board fan through the PL at 25 kHz. Both the
 * production and diagnostic images use this driver and its thermal policy. */
#define FAN_PWM_HZ              25000U
#define FAN_PWM_MATCH_INDEX     0U
#define FAN_STARTUP_US          500000U
#define FAN_SERVICE_TICKS       ((XTime)COUNTS_PER_SECOND)
#define FAN_SYSMON_TEMP_ADDR    (XPAR_XSYSMONPSU_0_BASEADDR + XPS_BA_OFFSET)
#define FAN_STOP_TEMP_MC        42000L
#define FAN_RESTART_TEMP_MC     46000L
#define FAN_MIN_DUTY            10U

static XTtcPs g_fan_ttc;
static XInterval g_fan_period;
static XTime g_last_service;
static int32_t g_filtered_temp_mc;
static uint32_t g_current_duty;
static uint32_t g_have_temperature;
static uint32_t g_initialized;
static uint32_t g_restart_kick;

static uint32_t interpolate_duty(int32_t temperature_mc,
                                 int32_t low_mc, int32_t high_mc,
                                 uint32_t low_duty, uint32_t high_duty)
{
    return low_duty +
           (uint32_t)(((int64_t)(temperature_mc - low_mc) *
                       (int64_t)(high_duty - low_duty)) /
                      (int64_t)(high_mc - low_mc));
}

static uint32_t fan_curve_duty(int32_t temperature_mc)
{
    if ((g_current_duty == 0U && temperature_mc < FAN_RESTART_TEMP_MC) ||
        (g_current_duty != 0U && temperature_mc <= FAN_STOP_TEMP_MC)) {
        return 0U;
    }
    if (temperature_mc < 50000L) {
        return FAN_MIN_DUTY;
    }
    if (temperature_mc < 60000L) {
        return interpolate_duty(temperature_mc, 50000L, 60000L,
                                FAN_MIN_DUTY, 40U);
    }
    if (temperature_mc < 68000L) {
        return interpolate_duty(temperature_mc, 60000L, 68000L, 40U, 65U);
    }
    if (temperature_mc < 75000L) {
        return interpolate_duty(temperature_mc, 68000L, 75000L, 65U, 100U);
    }
    return 100U;
}

static void apply_duty(uint32_t duty)
{
    uint32_t match;

    if (duty > 100U) {
        duty = 100U;
    }
    match = (uint32_t)(((uint64_t)g_fan_period * duty + 50U) / 100U);
    XTtcPs_SetMatchValue(&g_fan_ttc, FAN_PWM_MATCH_INDEX,
                         (XMatchRegValue)match);
    g_current_duty = duty;
}

static int read_temperature_mc(int32_t *temperature_mc)
{
    uint16_t raw = (uint16_t)Xil_In32(FAN_SYSMON_TEMP_ADDR);
    float temperature_c = XSysMonPsu_RawToTemperature_OnChip(raw);
    int32_t value_mc = (int32_t)(temperature_c * 1000.0f);

    if (value_mc < 0L || value_mc > 125000L) {
        return -1;
    }
    *temperature_mc = value_mc;
    return 0;
}

void nclp_fan_service(void)
{
    XTime now;
    int32_t measured_mc;
    uint32_t target_duty;
    uint32_t first_sample;

    if (g_initialized == 0U) {
        return;
    }
    XTime_GetTime(&now);
    if (g_last_service != 0U &&
        (now - g_last_service) < FAN_SERVICE_TICKS) {
        return;
    }
    g_last_service = now;

    if (read_temperature_mc(&measured_mc) != 0) {
        apply_duty(100U);
        return;
    }
    first_sample = (g_have_temperature == 0U) ? 1U : 0U;
    if (first_sample != 0U) {
        g_filtered_temp_mc = measured_mc;
        g_have_temperature = 1U;
    } else if (measured_mc > g_filtered_temp_mc) {
        g_filtered_temp_mc = measured_mc;
    } else {
        g_filtered_temp_mc =
            (int32_t)(((int64_t)g_filtered_temp_mc * 7 + measured_mc) / 8);
    }

    target_duty = fan_curve_duty(g_filtered_temp_mc);
    if (target_duty > 0U && g_current_duty == 0U) {
        apply_duty(100U);
        g_restart_kick = 1U;
        return;
    }
    if (g_restart_kick != 0U) {
        g_restart_kick = 0U;
    } else if (target_duty > 0U && first_sample == 0U &&
               target_duty + 5U < g_current_duty) {
        /* The startup path already supplied its 500 ms spin-up kick.  Rate
         * limit only later cooling changes, not the first curve decision. */
        target_duty = g_current_duty - 5U;
    }
    if (target_duty != g_current_duty) {
        apply_duty(target_duty);
    }
}

int nclp_fan_init(void)
{
    XTtcPs_Config *config;
    u8 prescaler;

    config = XTtcPs_LookupConfig((u32)XPAR_XTTCPS_2_BASEADDR);
    if (config == NULL ||
        XTtcPs_CfgInitialize(&g_fan_ttc, config, config->BaseAddress) !=
            XST_SUCCESS) {
        xil_printf("  FAIL KR260 fan TTC0 channel 2 initialization\r\n");
        return -1;
    }
    XTtcPs_Stop(&g_fan_ttc);
    if (XTtcPs_SetOptions(&g_fan_ttc,
                          XTTCPS_OPTION_INTERVAL_MODE |
                          XTTCPS_OPTION_MATCH_MODE) != XST_SUCCESS) {
        return -1;
    }
    XTtcPs_CalcIntervalFromFreq(&g_fan_ttc, FAN_PWM_HZ,
                                &g_fan_period, &prescaler);
    XTtcPs_SetPrescaler(&g_fan_ttc, prescaler);
    XTtcPs_SetInterval(&g_fan_ttc, g_fan_period);
    apply_duty(100U);
    XTtcPs_ResetCounterValue(&g_fan_ttc);
    XTtcPs_Start(&g_fan_ttc);
    g_initialized = 1U;
    xil_printf("  PASS KR260 fan PWM           TTC0 ch2 25 kHz quiet curve\r\n");
    usleep(FAN_STARTUP_US);
    nclp_fan_service();
    return 0;
}
