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

/* Why the panel is in the state it is in. Reported by the `diag` console
 * command, because "the screen is black" is not a diagnosis and the device
 * should be able to tell you which of these happened. */
typedef enum {
    PHAROS_DISP_UNTRIED = 0, /* vendor BSP not compiled in                */
    PHAROS_DISP_OK,          /* bsp_display_start() returned a display    */
    PHAROS_DISP_REPAIRED,    /* it aborted mid-sequence; we finished it   */
    PHAROS_DISP_FAILED,      /* the panel itself never registered         */
} pharos_disp_result_t;

typedef struct {
    bool display_ok;
    bool touch_ok;
    bool imu_ok;
    bool pmu_ok;
    bool rtc_present;
    bool sd_present;
    uint32_t psram_free;
    pharos_disp_result_t disp_result;
    int16_t disp_w, disp_h;
} pharos_bsp_status_t;

bool pharos_bsp_init(pharos_bsp_status_t *out);

/* The status recorded by the last pharos_bsp_init(), for the CLI. */
void pharos_bsp_last_status(pharos_bsp_status_t *out);
const char *pharos_disp_result_name(pharos_disp_result_t r);

/* Battery telemetry from the AXP2101, for the power planner and the rim
 * gauge. Returns false if the PMU did not come up. */
bool pharos_bsp_battery(pwr_battery_t *out);

/* ---- the motion sensor ----
 *
 * The board carries a QMI8658 six-axis IMU on the same I2C bus as the touch
 * controller and the PMU, and the vendor BSP declares BSP_CAPS_IMU 0 - there
 * is no driver for it. This is that driver, deliberately minimal: the
 * accelerometer, at 50 Hz, and nothing else. The gyroscope is left powered
 * down because nothing here needs angular rate and it costs current.
 *
 * Returns false when no IMU answered, which callers must treat as UNKNOWN
 * rather than as "not moving" - see pharos_motion.h. */
bool pharos_bsp_imu_present(void);

/* One acceleration sample in milli-g. False when there is no IMU or the read
 * failed; the values are then untouched. */
bool pharos_bsp_imu_read(int32_t *x_mg, int32_t *y_mg, int32_t *z_mg);

/* Set panel brightness, 0..255. AMOLED, so 0 is truly off. */
void pharos_bsp_brightness(uint8_t level);

/* Panel rotation in degrees: 0, 90, 180 or 270.
 *
 * Which one is "upright" depends on how you hold a round device, so this is a
 * runtime setting rather than a build constant. It is persisted, so the board
 * comes back the way you left it. Returns false for an unsupported angle. */
bool pharos_bsp_rotate(int degrees);
int pharos_bsp_rotation(void);

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
