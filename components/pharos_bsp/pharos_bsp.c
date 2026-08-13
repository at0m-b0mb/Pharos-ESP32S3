/* Pharos - board bring-up over the Waveshare managed BSP.
 *
 * Three build paths, chosen so the firmware compiles and links everywhere:
 *
 *   - HOST: no hardware at all (test/host). Stub.
 *   - TARGET without CONFIG_PHAROS_HAS_VENDOR_BSP (the default): compiles for
 *     ESP32-S3 and links the whole architecture, but uses a *simulated* board.
 *     This is what CI builds. It proves every Pharos component compiles and
 *     links for the target without asserting anything about the vendor's BSP
 *     API, which is unverified until someone has the board in hand (M1).
 *   - TARGET with CONFIG_PHAROS_HAS_VENDOR_BSP=y: the real path, calling the
 *     Waveshare managed BSP. Enabled at hardware bring-up, when the vendor's
 *     bsp_* function names can be confirmed against the installed component
 *     version and the REQUIRES list extended to pull it in.
 *
 * The gate is honest, not a fudge: the project's own docs say BSP bring-up is
 * milestone M1, and this is exactly that boundary made mechanical.
 */
#include "pharos_bsp.h"

#include "board_pins.h"

#if defined(PHAROS_HOST)

bool pharos_bsp_init(pharos_bsp_status_t *out) { (void)out; return false; }
bool pharos_bsp_battery(pwr_battery_t *out) { (void)out; return false; }
void pharos_bsp_brightness(uint8_t level) { (void)level; }
bool pharos_bsp_orientation(int16_t *p, int16_t *r) { (void)p; (void)r; return false; }

#elif !defined(CONFIG_PHAROS_HAS_VENDOR_BSP)

/* Target build, simulated board. Compiles and links for ESP32-S3; brings the
 * runtime up so the whole firmware is exercisable over serial before the
 * vendor BSP is wired at M1. */
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "bsp";

bool pharos_bsp_init(pharos_bsp_status_t *out)
{
    pharos_bsp_status_t st;
    memset(&st, 0, sizeof(st));
    st.display_ok = true;  /* simulated: no panel driven yet (M1) */
    st.touch_ok = true;
    st.imu_ok = false;
    st.pmu_ok = true;
    st.rtc_present = BSP_HAS_DISCRETE_RTC ? true : false;
    st.sd_present = BSP_HAS_SD ? true : false;
    st.psram_free = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    ESP_LOGW(TAG, "vendor BSP not enabled (CONFIG_PHAROS_HAS_VENDOR_BSP=n); "
                  "running with a simulated board. psram_free=%uKB",
             (unsigned)(st.psram_free / 1024));
    if (out) {
        *out = st;
    }
    return true;
}

bool pharos_bsp_battery(pwr_battery_t *out)
{
    if (!out) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->present = false; /* power planner reports "estimated" */
    out->capacity_mah = 500;
    return false;
}

void pharos_bsp_brightness(uint8_t level) { (void)level; }

bool pharos_bsp_orientation(int16_t *pitch_cdeg, int16_t *roll_cdeg)
{
    if (pitch_cdeg) *pitch_cdeg = 0;
    if (roll_cdeg) *roll_cdeg = 0;
    return false;
}

#else /* CONFIG_PHAROS_HAS_VENDOR_BSP: the real path, enabled at M1 */

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

/* From the Waveshare managed component. Add it to this component's REQUIRES
 * when enabling the flag; confirm the bsp_* names against the installed
 * version (docs/HARDWARE.md §bring-up). */
#include "bsp/esp-bsp.h"

static const char *TAG = "bsp";
static bool s_pmu_up;

bool pharos_bsp_init(pharos_bsp_status_t *out)
{
    pharos_bsp_status_t st;
    memset(&st, 0, sizeof(st));

    if (bsp_i2c_init() == ESP_OK) {
        st.pmu_ok = true;
        s_pmu_up = true;
    }

    lv_display_t *disp = bsp_display_start();
    st.display_ok = (disp != NULL);
    st.touch_ok = st.display_ok;

    st.psram_free = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    st.rtc_present = BSP_HAS_DISCRETE_RTC ? true : false;
    st.sd_present = BSP_HAS_SD ? true : false;

    ESP_LOGI(TAG, "board up: display=%d touch=%d pmu=%d psram_free=%uKB",
             st.display_ok, st.touch_ok, st.pmu_ok,
             (unsigned)(st.psram_free / 1024));

    if (out) {
        *out = st;
    }
    return st.display_ok && st.pmu_ok;
}

bool pharos_bsp_battery(pwr_battery_t *out)
{
    if (!out || !s_pmu_up) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->present = false;
    out->capacity_mah = 500; /* VERIFY against the fitted pack */
    return false;
}

void pharos_bsp_brightness(uint8_t level)
{
    bsp_display_brightness_set((int)((level * 100) / 255));
}

bool pharos_bsp_orientation(int16_t *pitch_cdeg, int16_t *roll_cdeg)
{
    if (pitch_cdeg) *pitch_cdeg = 0;
    if (roll_cdeg) *roll_cdeg = 0;
    return false;
}

#endif /* build path */
