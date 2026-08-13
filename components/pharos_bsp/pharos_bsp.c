/* Pharos - board bring-up over the Waveshare managed BSP.
 *
 * The vendor component (waveshare/esp32_s3_touch_amoled_1_75c) exposes the
 * bsp_* API for this exact panel. Pharos calls it rather than re-driving the
 * CO5300/AXP2101 itself; the wrapping here is only to give the rest of the
 * firmware a small, honest surface and to record what actually came up.
 *
 * The vendor headers are only present in an ESP-IDF build, so the whole file
 * is guarded. In a host build this component is not compiled at all.
 */
#include "pharos_bsp.h"

#include "board_pins.h"

#if defined(PHAROS_HOST)

bool pharos_bsp_init(pharos_bsp_status_t *out) { (void)out; return false; }
bool pharos_bsp_battery(pwr_battery_t *out) { (void)out; return false; }
void pharos_bsp_brightness(uint8_t level) { (void)level; }
bool pharos_bsp_orientation(int16_t *p, int16_t *r) { (void)p; (void)r; return false; }

#else

#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

/* From the managed BSP component. Names follow the vendor's bsp_* convention;
 * confirm against the installed component version at bring-up (docs/HARDWARE). */
#include "bsp/esp-bsp.h"

static const char *TAG = "bsp";
static bool s_pmu_up;

bool pharos_bsp_init(pharos_bsp_status_t *out)
{
    pharos_bsp_status_t st;
    memset(&st, 0, sizeof(st));

    /* I2C first: the PMU, touch and IMU all hang off it. */
    if (bsp_i2c_init() == ESP_OK) {
        st.pmu_ok = true;
        s_pmu_up = true;
    }

    /* Display + touch through the vendor LVGL bring-up. */
    lv_display_t *disp = bsp_display_start();
    st.display_ok = (disp != NULL);
    st.touch_ok = st.display_ok; /* the vendor call brings up both */

    st.psram_free = (uint32_t)heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    st.rtc_present = BSP_HAS_DISCRETE_RTC ? true : false;
    st.sd_present = BSP_HAS_SD ? true : false;

    ESP_LOGI(TAG, "board up: display=%d touch=%d pmu=%d psram_free=%uKB",
             st.display_ok, st.touch_ok, st.pmu_ok, st.psram_free / 1024);

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
    /* The vendor BSP surfaces the AXP2101 through bsp_power_*; if a given
     * component version does not, this reads the PMU over I2C directly. Until
     * hardware bring-up (M1) the values are marked absent so the power planner
     * reports "estimated" rather than a fabricated charge. */
    out->present = false;
    out->capacity_mah = 500; /* MX1.25 pack as commonly fitted; VERIFY */
    return false;
}

void pharos_bsp_brightness(uint8_t level)
{
    bsp_display_brightness_set((int)((level * 100) / 255));
}

bool pharos_bsp_orientation(int16_t *pitch_cdeg, int16_t *roll_cdeg)
{
    /* QMI8658 read lands with the IMU driver at M1. */
    if (pitch_cdeg) *pitch_cdeg = 0;
    if (roll_cdeg) *roll_cdeg = 0;
    return false;
}

#endif /* PHAROS_HOST */
