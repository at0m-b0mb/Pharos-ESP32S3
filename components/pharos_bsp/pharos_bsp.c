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
/* sdkconfig.h FIRST, and this is not a style preference.
 *
 * The #if chain below tests CONFIG_PHAROS_HAS_VENDOR_BSP. ESP-IDF does not
 * force-include sdkconfig.h; a translation unit only sees CONFIG_* macros once
 * something has included it. Every ESP header in this file is included INSIDE
 * one of the branches - i.e. after the test has already been evaluated - so
 * without this line the macro is undefined at that moment, !defined() is true,
 * and the SIMULATED board path is compiled regardless of what sdkconfig says.
 *
 * That is exactly what shipped from v1.4.0 to v1.7.0: the flag was set to y,
 * the build honoured it everywhere except here, and the panel was never
 * touched. tools/check_display.sh now fails the build if the simulated path
 * ends up in a release image. */
#include "sdkconfig.h"

#include "pharos_bsp.h"

#include "board_pins.h"

#if defined(PHAROS_HOST)

static pharos_bsp_status_t s_last;
bool pharos_bsp_init(pharos_bsp_status_t *out) { (void)out; return false; }
void pharos_bsp_last_status(pharos_bsp_status_t *out) { if (out) *out = s_last; }
bool pharos_bsp_battery(pwr_battery_t *out) { (void)out; return false; }
void pharos_bsp_brightness(uint8_t level) { (void)level; }
bool pharos_bsp_rotate(int degrees) { (void)degrees; return false; }
int pharos_bsp_rotation(void) { return 0; }
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
static pharos_bsp_status_t s_last;

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
    st.disp_result = PHAROS_DISP_UNTRIED;
    ESP_LOGW(TAG, "vendor BSP disabled - simulated board, screen will stay dark. "
                  "psram_free=%uKB", (unsigned)(st.psram_free / 1024));
    s_last = st;
    if (out) {
        *out = st;
    }
    return true;
}

void pharos_bsp_last_status(pharos_bsp_status_t *out) { if (out) *out = s_last; }

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

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"

#include "lvgl.h"
#include "bsp/esp-bsp.h"
#include "bsp/display.h"
/* esp_lv_adapter_start() - the step the vendor skips when touch fails. It is
 * pulled in transitively by the board header, but name it so the dependency
 * is visible rather than accidental. */
#include "esp_lv_adapter.h"
#include "nvs.h"

static const char *TAG = "bsp";

static pharos_bsp_status_t s_last;

bool pharos_bsp_init(pharos_bsp_status_t *out)
{
    pharos_bsp_status_t st;
    memset(&st, 0, sizeof(st));

    /* One call is supposed to bring up the QSPI CO5300 panel, the CST9217
     * touch controller and LVGL, and start the BSP's LVGL task.
     *
     * THE RETURN VALUE MATTERS, and throwing it away is what kept the screen
     * black. bsp_display_start_with_config() runs five steps in order:
     *
     *   1. esp_lv_adapter_init()
     *   2. bsp_display_lcd_init()      <- REGISTERS THE LVGL DISPLAY
     *   3. bsp_display_indev_init()    <- touch; returns NULL if it fails
     *   4. bsp_display_brightness_init()  (backlight on)
     *   5. esp_lv_adapter_start()         (starts the LVGL flush task)
     *
     * Step 3 aborts the function with `return NULL`. But step 2 has already
     * registered the display with LVGL - so lv_display_get_default() answers
     * "yes, there is a display" while steps 4 and 5 never ran. The panel is
     * therefore unlit and nothing is ever flushed to it, and every lv_* call
     * still succeeds because it only mutates objects in RAM.
     *
     * That is exactly the reported symptom: a healthy boot, lenses ticking,
     * `ui: active: wifi.spectrum` forever, and a black screen. Asking LVGL
     * instead of reading the return value could not distinguish it. */
    lv_display_t *disp = bsp_display_start();

    if (disp != NULL) {
        st.disp_result = PHAROS_DISP_OK;
        st.touch_ok = true;
    } else {
        /* The vendor aborted part-way. If the PANEL registered, the only
         * things skipped are the backlight and the LVGL task - finish them
         * ourselves. A working screen with dead touch beats a black screen,
         * and this device is usable from the console regardless. */
        disp = lv_display_get_default();
        if (disp != NULL) {
            ESP_LOGW(TAG, "bsp_display_start() aborted after the panel came up "
                          "(touch controller is the usual cause) - finishing "
                          "the sequence without it");
            const esp_err_t b = bsp_display_brightness_init();
            const esp_err_t a = esp_lv_adapter_start();
            if (b == ESP_OK && a == ESP_OK) {
                st.disp_result = PHAROS_DISP_REPAIRED;
            } else {
                st.disp_result = PHAROS_DISP_FAILED;
                ESP_LOGE(TAG, "repair failed: brightness=%s adapter_start=%s",
                         esp_err_to_name(b), esp_err_to_name(a));
            }
            st.touch_ok = false;
        } else {
            st.disp_result = PHAROS_DISP_FAILED;
            ESP_LOGE(TAG, "the panel never registered with LVGL - check the "
                          "QSPI wiring and that this really is a 1.75C board");
        }
    }

    st.display_ok = (disp != NULL) && (st.disp_result != PHAROS_DISP_FAILED);
    st.pmu_ok = true;
    st.psram_free = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    st.rtc_present = PHAROS_BOARD_HAS_DISCRETE_RTC ? true : false;
    st.sd_present = PHAROS_BOARD_HAS_SD ? true : false;

    if (disp) {
        st.disp_w = (int16_t)lv_display_get_horizontal_resolution(disp);
        st.disp_h = (int16_t)lv_display_get_vertical_resolution(disp);
    }

    if (st.display_ok) {
        /* Idempotent, and cheap insurance: on an AMOLED brightness IS emission,
         * so a panel at zero is indistinguishable from a broken one. */
        bsp_display_brightness_set(100);
        rotation_restore();
        ESP_LOGI(TAG, "panel %s: %dx%d, touch=%d, psram_free=%uKB",
                 pharos_disp_result_name(st.disp_result),
                 st.disp_w, st.disp_h, st.touch_ok,
                 (unsigned)(st.psram_free / 1024));
    }

    s_last = st;
    if (out) {
        *out = st;
    }
    return st.display_ok;
}

void pharos_bsp_last_status(pharos_bsp_status_t *out) { if (out) *out = s_last; }

bool pharos_bsp_display_lock(int timeout_ms)
{
    /* CRITICAL: bsp_display_lock() wraps esp_lv_adapter_lock(), which returns
     * an esp_err_t - ESP_OK (== 0) means the LVGL mutex was ACQUIRED. Returning
     * that value straight as a bool inverts it: a successful lock reads as
     * false, every caller (the splash that creates the HUD, and every repaint)
     * bails out under `if (!lock)`, and the panel - fully powered, backlight at
     * 100%, LVGL task running - is simply never drawn to. That is the black
     * screen. Compare against ESP_OK so success is true. A negative timeout is
     * the vendor's "wait forever" sentinel, so it is passed through as-is. */
    return bsp_display_lock((uint32_t)timeout_ms) == ESP_OK;
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

static int s_rotation;

int pharos_bsp_rotation(void) { return s_rotation; }

bool pharos_bsp_rotate(int degrees)
{
    bsp_display_rotation_t r;
    switch (degrees) {
    case 0:   r = BSP_DISPLAY_ROTATE_0;   break;
    case 90:  r = BSP_DISPLAY_ROTATE_90;  break;
    case 180: r = BSP_DISPLAY_ROTATE_180; break;
    case 270: r = BSP_DISPLAY_ROTATE_270; break;
    default:  return false;
    }
    if (bsp_display_rotation_set(r) != ESP_OK) {
        return false;
    }
    s_rotation = degrees;

    /* Remember it: which way is up is a property of how this particular
     * operator holds the thing, not of the build. */
    nvs_handle_t h;
    if (nvs_open("pharos", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i32(h, "rot", degrees);
        nvs_commit(h);
        nvs_close(h);
    }
    ESP_LOGI(TAG, "panel rotation set to %d degrees", degrees);
    return true;
}

static void rotation_restore(void)
{
    nvs_handle_t h;
    int32_t deg = -1;
    if (nvs_open("pharos", NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_i32(h, "rot", &deg) != ESP_OK) {
            deg = -1;
        }
        nvs_close(h);
    }
    if (deg == 0 || deg == 90 || deg == 180 || deg == 270) {
        pharos_bsp_rotate((int)deg);
    }
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

/* ---- diagnosis, for the console ------------------------------------- */

const char *pharos_disp_result_name(pharos_disp_result_t r)
{
    switch (r) {
    case PHAROS_DISP_UNTRIED:  return "not attempted (vendor BSP off)";
    case PHAROS_DISP_OK:       return "up";
    case PHAROS_DISP_REPAIRED: return "up (repaired; touch failed)";
    case PHAROS_DISP_FAILED:   return "FAILED";
    default:                   return "?";
    }
}
