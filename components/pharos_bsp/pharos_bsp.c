/* Pharos - board bring-up over the Waveshare managed BSP.
 *
 * Three build paths:
 *
 *   - HOST: no hardware (test/host). Stub.
 *   - TARGET with CONFIG_PHAROS_HAS_VENDOR_BSP=y (the DEFAULT): drives the real
 *     panel through the Waveshare component, exactly as their own LVGL example
 *     does. This is what makes the screen light up.
 *   - TARGET with the flag off: compiles and links with a simulated board, for
 *     CI or for bringing the logic up on a bare module with no panel attached.
 *
 * The vendor API, taken from Waveshare's examples/esp-idf/02_lvgl_demo_v9:
 *
 *     bsp_display_start();      // panel + touch + LVGL, all of it
 *     bsp_display_lock(-1);     // take the LVGL mutex before touching widgets
 *     ... lv_* calls ...
 *     bsp_display_unlock();
 *
 * bsp_display_start() spins up the BSP's own LVGL task, so every widget call
 * anywhere in Pharos must be bracketed by that lock. pharos_bsp_display_lock()
 * below is the single place that is expressed, so the rest of the firmware
 * never has to know whose mutex it is.
 */
#include "pharos_bsp.h"

#include "board_pins.h"

#if defined(PHAROS_HOST)

bool pharos_bsp_init(pharos_bsp_status_t *out) { (void)out; return false; }
bool pharos_bsp_battery(pwr_battery_t *out) { (void)out; return false; }
void pharos_bsp_brightness(uint8_t level) { (void)level; }
bool pharos_bsp_orientation(int16_t *p, int16_t *r) { (void)p; (void)r; return false; }
bool pharos_bsp_display_lock(int timeout_ms) { (void)timeout_ms; return false; }
void pharos_bsp_display_unlock(void) {}

#elif !defined(CONFIG_PHAROS_HAS_VENDOR_BSP)

/* Target build, simulated board: compiles and links for ESP32-S3 with no panel
 * driven. Used by CI and by anyone bringing the logic up on a bare module. */
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "bsp";

bool pharos_bsp_init(pharos_bsp_status_t *out)
{
    pharos_bsp_status_t st;
    memset(&st, 0, sizeof(st));
    st.display_ok = false; /* honest: no panel is being driven */
    st.touch_ok = false;
    st.pmu_ok = true;
    st.rtc_present = PHAROS_BOARD_HAS_DISCRETE_RTC ? true : false;
    st.sd_present = PHAROS_BOARD_HAS_SD ? true : false;
    st.psram_free = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGW(TAG, "vendor BSP disabled - simulated board, screen will stay dark. "
                  "psram_free=%uKB", (unsigned)(st.psram_free / 1024));
    if (out) {
        *out = st;
    }
    return true;
}

bool pharos_bsp_battery(pwr_battery_t *out)
{
    if (!out) return false;
    memset(out, 0, sizeof(*out));
    out->present = false;
    out->capacity_mah = PHAROS_BOARD_BATTERY_MAH;
    return false;
}

void pharos_bsp_brightness(uint8_t level) { (void)level; }
bool pharos_bsp_orientation(int16_t *p, int16_t *r)
{
    if (p) *p = 0;
    if (r) *r = 0;
    return false;
}
bool pharos_bsp_display_lock(int timeout_ms) { (void)timeout_ms; return false; }
void pharos_bsp_display_unlock(void) {}

#else /* CONFIG_PHAROS_HAS_VENDOR_BSP - the real panel */

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"

static const char *TAG = "bsp";

bool pharos_bsp_init(pharos_bsp_status_t *out)
{
    pharos_bsp_status_t st;
    memset(&st, 0, sizeof(st));

    /* One call brings up the QSPI CO5300 panel, the CST9217 touch controller
     * and LVGL, and starts the BSP's LVGL task. Matches the vendor example. */
    bsp_display_start();

    /* Ask LVGL whether a display actually registered, rather than assuming a
     * particular return type from the vendor call - that keeps this source
     * compiling across BSP revisions. */
    lv_display_t *disp = lv_display_get_default();
    st.display_ok = (disp != NULL);
    st.touch_ok = st.display_ok; /* the same call brings touch up */
    st.pmu_ok = true;

    st.psram_free = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    st.rtc_present = PHAROS_BOARD_HAS_DISCRETE_RTC ? true : false;
    st.sd_present = PHAROS_BOARD_HAS_SD ? true : false;

    if (st.display_ok) {
        ESP_LOGI(TAG, "panel up: %dx%d, psram_free=%uKB",
                 (int)lv_display_get_horizontal_resolution(disp),
                 (int)lv_display_get_vertical_resolution(disp),
                 (unsigned)(st.psram_free / 1024));
    } else {
        ESP_LOGE(TAG, "bsp_display_start() did not register an LVGL display");
    }

    if (out) {
        *out = st;
    }
    return st.display_ok;
}

bool pharos_bsp_display_lock(int timeout_ms)
{
    return bsp_display_lock(timeout_ms);
}

void pharos_bsp_display_unlock(void)
{
    bsp_display_unlock();
}

bool pharos_bsp_battery(pwr_battery_t *out)
{
    if (!out) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    /* AXP2101 telemetry lands with the PMU driver; until then the power
     * planner is told the reading is absent rather than given a fabricated
     * charge. */
    out->present = false;
    out->capacity_mah = PHAROS_BOARD_BATTERY_MAH;
    return false;
}

void pharos_bsp_brightness(uint8_t level)
{
    /* The vendor BSP sets a sane default at start; this is the operator's
     * dimmer. Percent, not 0..255. */
    bsp_display_brightness_set((int)(((int)level * 100) / 255));
}

bool pharos_bsp_orientation(int16_t *pitch_cdeg, int16_t *roll_cdeg)
{
    /* QMI8658 read lands with the IMU driver at M1. */
    if (pitch_cdeg) *pitch_cdeg = 0;
    if (roll_cdeg) *roll_cdeg = 0;
    return false;
}

#endif /* build path */
