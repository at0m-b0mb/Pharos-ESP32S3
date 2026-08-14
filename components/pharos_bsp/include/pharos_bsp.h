/* Pharos - board bring-up
 *
 * One call brings the board to life: PMU, I2C bus, display, touch, IMU. It
 * wraps the Waveshare managed BSP rather than re-driving the CO5300 and
 * AXP2101 by hand, because the vendor component is the authority on this
 * panel's init sequence and getting an AMOLED init wrong is a good way to
 * cook a panel.
 *
 * Everything here is target-only; the host tests never call it.
 */
#ifndef PHAROS_BSP_H
#define PHAROS_BSP_H

#include <stdbool.h>
#include <stdint.h>

#include "pharos_power.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    bool display_ok;
    bool touch_ok;
    bool imu_ok;
    bool pmu_ok;
    bool rtc_present;
    bool sd_present;
    uint32_t psram_free;
} pharos_bsp_status_t;

bool pharos_bsp_init(pharos_bsp_status_t *out);

/* Battery telemetry from the AXP2101, for the power planner and the rim
 * gauge. Returns false if the PMU did not come up. */
bool pharos_bsp_battery(pwr_battery_t *out);

/* Set panel brightness, 0..255. AMOLED, so 0 is truly off. */
void pharos_bsp_brightness(uint8_t level);

/* Latest IMU sample, in centi-degrees, for the wrist-raise and still-detect
 * that drive the Sentry lens. */
bool pharos_bsp_orientation(int16_t *pitch_cdeg, int16_t *roll_cdeg);

/* The LVGL mutex. bsp_display_start() runs LVGL on its own task, so EVERY
 * lv_* call anywhere in the firmware must be bracketed by these. Wrapping the
 * vendor's lock here means the rest of Pharos never needs to know whose mutex
 * it is - and that there even is one.
 *
 *   if (pharos_bsp_display_lock(50)) { ...lv_* calls... ; pharos_bsp_display_unlock(); }
 *
 * timeout_ms < 0 waits forever. Returns false if the lock was not taken (or
 * there is no display on this build), in which case do NOT touch LVGL. */
bool pharos_bsp_display_lock(int timeout_ms);
void pharos_bsp_display_unlock(void);

#ifdef __cplusplus
}
#endif

#endif /* PHAROS_BSP_H */
