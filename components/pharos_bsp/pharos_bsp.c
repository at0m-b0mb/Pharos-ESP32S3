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
/* No IMU off the real board. ABSENT, not STILL: pharos_motion.h explains why
 * "nothing is measuring" must never be reported as "nothing is moving". */
bool pharos_bsp_imu_present(void) { return false; }
bool pharos_bsp_imu_read(int32_t *x, int32_t *y, int32_t *z)
{
    (void)x; (void)y; (void)z;
    return false;
}
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
/* No IMU off the real board. ABSENT, not STILL: pharos_motion.h explains why
 * "nothing is measuring" must never be reported as "nothing is moving". */
bool pharos_bsp_imu_present(void) { return false; }
bool pharos_bsp_imu_read(int32_t *x, int32_t *y, int32_t *z)
{
    (void)x; (void)y; (void)z;
    return false;
}
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
#include "driver/i2c_master.h"
#include "nvs.h"

static const char *TAG = "bsp";

static pharos_bsp_status_t s_last;

/* Defined below, next to the rest of the rotation handling; called from
 * pharos_bsp_init() once the panel is confirmed up. */
static void rotation_restore(void);

/* ---- draw buffers in INTERNAL RAM -----------------------------------
 *
 * THE PANEL DROPPING FRAMES, AND WHY IT ONLY EVER HAPPENED WHILE NAVIGATING.
 *
 *     panel_io_spi_tx_color(395): spi transmit (queue) color failed
 *     panel_co5300_draw_bitmap(292): send color data failed
 *     esp_lvgl:bridge_v9: Draw bitmap failed: ESP_ERR_NO_MEM
 *
 * A rendered frame that never reaches the glass, which from the outside is a
 * screen that tears, stalls and shows stale content - reported, accurately, as
 * "screen glitches".
 *
 * It is NOT the transaction queue. esp_lcd calls spi_device_queue_trans() with
 * portMAX_DELAY, so a full queue BLOCKS; it cannot return this error. The
 * failure is one layer further down. The vendor BSP puts LVGL's draw buffers
 * in PSRAM, and the SPI master cannot always DMA straight out of PSRAM - when
 * the source is not cache-line aligned in BOTH address and length it allocates
 * a bounce buffer, the size of the whole transfer, from INTERNAL DMA-capable
 * RAM, and copies through it. LVGL flushes whatever rectangle happens to be
 * dirty, so those lengths are arbitrary and almost every flush bounces.
 *
 * Internal RAM is the scarce one on this board. Bringing the Wi-Fi driver up
 * or down allocates tens of kilobytes of it at once - dynamic rx buffers,
 * static rx buffers at 1600 bytes each, tx buffers, the driver task stack -
 * and Pharos tears the radio down and back up on EVERY lens change. So while
 * the operator taps through the dial, the bounce buffer loses a race against
 * the Wi-Fi driver and the frame is dropped.
 *
 * That is not a theory. Of 207 dropped frames captured from the device while
 * the dial was being navigated by hand, 193 - ninety-three percent - fell
 * within 400 ms of a radio init or deinit, and none at all appeared while a
 * lens simply ran. Switching lenses slowly from the console never reproduced
 * it, which is exactly why this survived the earlier testing.
 *
 * The fix is to stop needing a bounce buffer at all. Draw buffers allocated in
 * internal DMA-capable RAM are a legal DMA source as they are, so the flush
 * goes straight out of them: no allocation on the flush path, nothing to fail,
 * and nothing Wi-Fi can take away.
 *
 * The cost is internal RAM, so the size is negotiated rather than assumed: try
 * a comfortable buffer, fall back through smaller ones, and if none of them
 * fit keep the vendor's PSRAM pair rather than leaving the display worse off.
 * Fewer rows only means more transfers per repaint, and transfers are cheap -
 * it is the ALLOCATION that was failing.
 *
 * The vendor's PSRAM buffers stay allocated; LVGL does not own them and there
 * is no supported way to hand them back. That leaks about 93 KB of PSRAM once,
 * at boot, out of roughly 6 MB - deliberately, and far cheaper than forking a
 * managed component. */
static void display_buffers_to_internal(lv_display_t *disp)
{
    if (!disp) {
        return;
    }
    /* Descending ladder. 466 px * 2 bytes = 932 bytes a row. */
    static const int rows[] = { 24, 20, 16, 12 };

    const size_t before =
        heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);

    for (unsigned i = 0; i < sizeof(rows) / sizeof(rows[0]); i++) {
        const size_t bytes = (size_t)BSP_LCD_H_RES * (size_t)rows[i] * 2u;
        /* THE RESERVE EXISTS BECAUSE THE BLUETOOTH CONTROLLER DOES.
         *
         * Moving the draw buffers into internal RAM fixed the dropped frames,
         * and then took 87 KB of the scarcest memory on the chip to do it. The
         * BLE controller wants a large contiguous block of the same memory,
         * and Vigil is the only lens that starts it - so nothing noticed until
         * that one lens was opened, at which point:
         *
         *     BLE_INIT: Malloc failed
         *     BLE assert emi.c 164
         *     Guru Meditation Error: Core 0 panic'ed (Interrupt wdt timeout)
         *
         * A display optimisation that silently breaks a detector is a bad
         * trade at any frame rate. The reserve is large enough for the
         * controller, the audio codec and the Wi-Fi driver together, and the
         * buffers shrink to fit around them.
         *
         * The number was raised again after Vigil finally ran: with the Wi-Fi
         * sniffer AND the BLE observer both up - which only Vigil does -
         * internal free fell to 19 KB, under the threshold this firmware's own
         * System screen paints red. Nothing had failed yet, and waiting for it
         * to would have been a poor way to find out.
         *
         * That costs nothing worth having. These buffers live in internal RAM
         * precisely so the SPI master never needs a bounce buffer, and without
         * a bounce buffer there is no allocation on the flush path to fail -
         * so a smaller buffer only means more transfers per repaint, and
         * transfers were never the problem. */
        if (before < (bytes * 2u) + 140u * 1024u) {
            continue;
        }
        void *b1 = heap_caps_aligned_alloc(64, bytes,
                                           MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
        void *b2 = heap_caps_aligned_alloc(64, bytes,
                                           MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA);
        if (!b1 || !b2) {
            if (b1) heap_caps_free(b1);
            if (b2) heap_caps_free(b2);
            continue;
        }
        lv_display_set_buffers(disp, b1, b2, bytes, LV_DISPLAY_RENDER_MODE_PARTIAL);
        ESP_LOGI(TAG,
                 "draw buffers: 2 x %ux%u in INTERNAL DMA RAM (%u KB each); "
                 "flushes no longer bounce, so a busy radio cannot drop a frame. "
                 "internal free %u -> %u KB",
                 (unsigned)BSP_LCD_H_RES, (unsigned)rows[i],
                 (unsigned)(bytes / 1024), (unsigned)(before / 1024),
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL |
                                                    MALLOC_CAP_DMA) / 1024));
        return;
    }
    ESP_LOGW(TAG, "not enough internal DMA RAM for draw buffers (%u KB free) - "
                  "keeping the vendor's PSRAM pair; expect dropped frames while "
                  "the radio restarts", (unsigned)(before / 1024));
}

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
        /* Before anything is drawn: move the draw buffers into internal DMA
         * RAM so a flush never has to allocate. Under the LVGL lock, because
         * the adapter's task is already running. */
        if (bsp_display_lock(1000) == ESP_OK) {
            display_buffers_to_internal(disp);
            bsp_display_unlock();
        } else {
            ESP_LOGW(TAG, "could not take the LVGL lock to move the draw "
                          "buffers; keeping the vendor's PSRAM pair");
        }
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

/* ---- AXP2101 -------------------------------------------------------
 *
 * The power-management chip, on the same I2C bus as the touch controller and
 * the IMU, at 0x34. The vendor BSP brings the bus up and hands out a handle
 * but does not talk to the PMU at all, so this does.
 *
 * Everything here is PROBED, never assumed. The chip ID is read first and a
 * wrong answer means the reading is reported ABSENT rather than guessed at -
 * a fabricated battery percentage on a security tool is exactly the kind of
 * small lie that makes an operator stop believing the large truths. The old
 * implementation was honest about this in the only way it could be: it
 * returned false and said so in a comment. Now it can do better.
 *
 * Register map, from the AXP2101 datasheet:
 *   0x00 STATUS1  bit3 = battery present
 *   0x01 STATUS2  bits[6:5] = 0 standby, 1 charging, 2 discharging
 *   0x03 CHIP_ID  = 0x4A on this part
 *   0x30 ADC_EN   bit0 = VBAT channel
 *   0x34/0x35     VBAT, 14-bit, already in millivolts
 *   0xA4 SOC      fuel-gauge percentage, 0..100
 */
#define AXP_ADDR      0x34
#define AXP_REG_ST1   0x00
#define AXP_REG_ST2   0x01
#define AXP_REG_ID    0x03
#define AXP_REG_ADCEN 0x30
#define AXP_REG_VBATH 0x34
#define AXP_REG_SOC   0xA4
#define AXP_CHIP_ID   0x4A

static i2c_master_dev_handle_t s_axp;
static bool s_axp_probed;
static bool s_axp_present;

static bool axp_rd(uint8_t reg, uint8_t *val, size_t n)
{
    if (!s_axp) {
        return false;
    }
    return i2c_master_transmit_receive(s_axp, &reg, 1, val, n,
                                       pdMS_TO_TICKS(50)) == ESP_OK;
}

static bool axp_wr(uint8_t reg, uint8_t val)
{
    if (!s_axp) {
        return false;
    }
    const uint8_t b[2] = { reg, val };
    return i2c_master_transmit(s_axp, b, sizeof(b), pdMS_TO_TICKS(50)) == ESP_OK;
}

/* ---- QMI8658 six-axis IMU ------------------------------------------
 *
 * The vendor BSP says BSP_CAPS_IMU 0 and ships no driver, but its own header
 * lists an IMU on the shared bus and the board's documentation names the part.
 * So: probe both of its possible addresses, verify WHO_AM_I, and refuse to
 * report anything if the answer is wrong.
 *
 * That last part matters more here than usual. Something else lives at 0x6A on
 * plenty of boards, and reading a stranger's registers as acceleration would
 * produce a confident stream of numbers that mean nothing - which, fed into
 * the motion engine, would become confident statements about whether somebody
 * walked. An unverified chip is treated as no chip. */
#define QMI_ADDR_LO 0x6A
#define QMI_ADDR_HI 0x6B
#define QMI_REG_WHOAMI 0x00
#define QMI_REG_CTRL1  0x02
#define QMI_REG_CTRL2  0x03
#define QMI_REG_CTRL7  0x08
#define QMI_REG_AX_L   0x35
#define QMI_REG_RESET  0x60
#define QMI_REG_STATUS0 0x2E
#define QMI_WHOAMI_VAL 0x05

static i2c_master_dev_handle_t s_imu;
static bool s_imu_probed;
static bool s_imu_present;

/* Only for the log line below - the engine does its own. */
static int32_t isqrt_i32(int32_t v)
{
    if (v <= 0) {
        return 0;
    }
    int32_t r = 0, b = 1 << 15;
    while (b > v) b >>= 2;
    while (b) {
        if (v >= r + b) { v -= r + b; r = (r >> 1) + b; }
        else            { r >>= 1; }
        b >>= 2;
    }
    return r;
}

static bool imu_rd(uint8_t reg, uint8_t *val, size_t n)
{
    if (!s_imu) {
        return false;
    }
    return i2c_master_transmit_receive(s_imu, &reg, 1, val, n,
                                       pdMS_TO_TICKS(50)) == ESP_OK;
}

static bool imu_wr(uint8_t reg, uint8_t val)
{
    if (!s_imu) {
        return false;
    }
    const uint8_t b[2] = { reg, val };
    return i2c_master_transmit(s_imu, b, sizeof(b), pdMS_TO_TICKS(50)) == ESP_OK;
}

static bool imu_try(uint8_t addr)
{
    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (!bus) {
        return false;
    }
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = 100000,
    };
    if (i2c_master_bus_add_device(bus, &cfg, &s_imu) != ESP_OK) {
        return false;
    }
    uint8_t who = 0;
    if (!imu_rd(QMI_REG_WHOAMI, &who, 1) || who != QMI_WHOAMI_VAL) {
        i2c_master_bus_rm_device(s_imu);
        s_imu = NULL;
        return false;
    }
    ESP_LOGI(TAG, "QMI8658 IMU found at 0x%02X", addr);
    return true;
}

static void imu_probe(void)
{
    if (s_imu_probed) {
        return;
    }
    s_imu_probed = true;

    if (!imu_try(QMI_ADDR_LO) && !imu_try(QMI_ADDR_HI)) {
        ESP_LOGW(TAG, "no QMI8658 answered; motion sensing unavailable");
        return;
    }

    /* A SOFT RESET FIRST.
     *
     * Without it the part can come up in whatever state the previous firmware
     * left it, and the configuration below then lands on top of that rather
     * than replacing it. That is what happened here: WHO_AM_I answered, the
     * writes appeared to succeed, and every axis read railed at full scale.
     * The datasheet's init sequence starts with a reset for exactly this
     * reason. */
    imu_wr(QMI_REG_RESET, 0xB0);
    vTaskDelay(pdMS_TO_TICKS(50));

    /* CTRL1: address auto-increment for block reads, little-endian.
     * CTRL2: bits [6:4] are full scale (001 = +/-4 g), bits [3:0] the output
     *        rate (0110 = 125 Hz). This was 0x24 - which is 010, +/-8 g - so
     *        every reading came back at half its true value and a device flat
     *        on a desk measured 473 mg of gravity instead of 1000. Nothing
     *        crashed; the swing thresholds simply became twice as hard to
     *        reach, which would have quietly made walking hard to detect.
     * CTRL7: accelerometer on, GYROSCOPE OFF - nothing here needs angular
     *        rate, and leaving it running would cost current on a device that
     *        may be in somebody's pocket for hours. */
    imu_wr(QMI_REG_CTRL1, 0x40);
    imu_wr(QMI_REG_CTRL2, 0x16);
    imu_wr(QMI_REG_CTRL7, 0x01);
    vTaskDelay(pdMS_TO_TICKS(50));

    /* READ THE CONFIGURATION BACK.
     *
     * A write that silently did not land is indistinguishable from one that
     * did, right up until the data is nonsense - which is how a railed
     * accelerometer got as far as the motion engine. Reading the control
     * registers back turns "I asked for this" into "it is set to this". */
    {
        uint8_t c1 = 0, c2 = 0, c7 = 0, st0 = 0;
        imu_rd(QMI_REG_CTRL1, &c1, 1);
        imu_rd(QMI_REG_CTRL2, &c2, 1);
        imu_rd(QMI_REG_CTRL7, &c7, 1);
        imu_rd(QMI_REG_STATUS0, &st0, 1);
        ESP_LOGI(TAG, "QMI8658 ctrl1=0x%02X ctrl2=0x%02X ctrl7=0x%02X st0=0x%02X",
                 c1, c2, c7, st0);
        if (c2 != 0x16 || !(c7 & 0x01)) {
            ESP_LOGW(TAG, "QMI8658 did not take its configuration");
        }
    }

    /* Prove it is actually producing data before claiming it works: a chip
     * that answers WHO_AM_I but was never configured would stream zeroes,
     * and zeroes read as "perfectly still" forever. */
    int32_t x = 0, y = 0, z = 0;
    s_imu_present = true;
    if (!pharos_bsp_imu_read(&x, &y, &z)) {
        s_imu_present = false;
        ESP_LOGW(TAG, "QMI8658 did not return a sample; motion unavailable");
        return;
    }
    /* GRAVITY HAS TO READ AS GRAVITY.
     *
     * Whatever way up the board is, the magnitude at rest is one g. Checking
     * only that it is "not near zero" let a wrong full-scale setting through:
     * the part answered, the numbers looked plausible, and every reading was
     * half its true value - so the swing thresholds became twice as hard to
     * reach and walking would have gone undetected for a reason nothing on the
     * device could report.
     *
     * A band around one g catches a mis-scaled range, a part left asleep, and
     * a chip that is not a QMI8658 at all. It is also a real self-test: if
     * this passes, the units downstream are the units the engine expects. */
    const int32_t mag2 = x * x + y * y + z * z;
    if (mag2 < 700L * 700L || mag2 > 1400L * 1400L) {
        s_imu_present = false;
        ESP_LOGW(TAG,
                 "QMI8658 reads %ld,%ld,%ld mg - |a|=%ld, expected ~1000 at "
                 "rest; scale or range is wrong, motion disabled",
                 (long)x, (long)y, (long)z, (long)isqrt_i32(mag2));
        return;
    }
    ESP_LOGI(TAG, "motion sensing live (%ld,%ld,%ld mg at rest)", (long)x,
             (long)y, (long)z);
}

bool pharos_bsp_imu_present(void)
{
    imu_probe();
    return s_imu_present;
}

bool pharos_bsp_imu_read(int32_t *x_mg, int32_t *y_mg, int32_t *z_mg)
{
    if (!s_imu) {
        return false;
    }
    uint8_t raw[6];
    if (!imu_rd(QMI_REG_AX_L, raw, sizeof(raw))) {
        return false;
    }
    /* Little-endian signed 16-bit per axis. At +/-4 g full scale the LSB is
     * 4000 mg / 32768 - done as a multiply and shift so the hot path stays
     * integer. */
    for (unsigned i = 0; i < 3; i++) {
        const int16_t v = (int16_t)((uint16_t)raw[i * 2] |
                                    ((uint16_t)raw[i * 2 + 1] << 8));
        const int32_t mg = ((int32_t)v * 4000) / 32768;
        if (i == 0 && x_mg) *x_mg = mg;
        if (i == 1 && y_mg) *y_mg = mg;
        if (i == 2 && z_mg) *z_mg = mg;
    }
    return true;
}

static void axp_probe(void)
{
    if (s_axp_probed) {
        return;
    }
    s_axp_probed = true;

    i2c_master_bus_handle_t bus = bsp_i2c_get_handle();
    if (!bus) {
        ESP_LOGW(TAG, "no I2C bus handle; battery telemetry unavailable");
        return;
    }
    const i2c_device_config_t cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AXP_ADDR,
        .scl_speed_hz = 100000,
    };
    if (i2c_master_bus_add_device(bus, &cfg, &s_axp) != ESP_OK) {
        ESP_LOGW(TAG, "could not attach to the PMU at 0x%02X", AXP_ADDR);
        return;
    }
    uint8_t id = 0;
    if (!axp_rd(AXP_REG_ID, &id, 1)) {
        ESP_LOGW(TAG, "PMU at 0x%02X did not answer; battery unavailable", AXP_ADDR);
        return;
    }
    if (id != AXP_CHIP_ID) {
        /* Something is there, but it is not the part this code understands.
         * Reporting its registers as a battery percentage would be inventing
         * a number, so it does not. */
        ESP_LOGW(TAG, "0x%02X answered with chip id 0x%02X, expected 0x%02X - "
                      "not reading it as a battery", AXP_ADDR, id, AXP_CHIP_ID);
        return;
    }
    /* Turn the battery-voltage ADC channel on; without it VBAT reads zero and
     * a zero would look like a flat pack rather than a disabled converter. */
    uint8_t adcen = 0;
    if (axp_rd(AXP_REG_ADCEN, &adcen, 1)) {
        axp_wr(AXP_REG_ADCEN, (uint8_t)(adcen | 0x01));
    }
    s_axp_present = true;
    ESP_LOGI(TAG, "AXP2101 found at 0x%02X; battery telemetry live", AXP_ADDR);
}

bool pharos_bsp_battery(pwr_battery_t *out)
{
    if (!out) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    out->capacity_mah = PHAROS_BOARD_BATTERY_MAH;

    axp_probe();
    if (!s_axp_present) {
        out->present = false;
        return false;
    }

    uint8_t st1 = 0, st2 = 0, soc = 0, vb[2] = { 0, 0 };
    if (!axp_rd(AXP_REG_ST1, &st1, 1) || !axp_rd(AXP_REG_ST2, &st2, 1)) {
        return false;
    }
    out->present = (st1 & 0x08) != 0;
    /* bits[6:5]: 0 standby, 1 charging, 2 discharging */
    out->charging = (((st2 >> 5) & 0x03) == 0x01);

    if (axp_rd(AXP_REG_VBATH, vb, 2)) {
        /* 14 bits across two registers, already in millivolts. */
        out->mv = (uint16_t)((((uint16_t)vb[0] & 0x3F) << 8) | vb[1]);
    }
    if (axp_rd(AXP_REG_SOC, &soc, 1) && soc <= 100u) {
        out->soc_pct = soc;
    } else if (out->mv) {
        /* No fuel gauge answer: fall back to the pack curve, and be clear in
         * the only way code can be - a linear 3.3-4.2 V map is a rough guide,
         * not a gauge. Callers see a percentage either way; the difference is
         * that this one cannot be better than approximate. */
        const int mv = (int)out->mv;
        int pct = (mv - 3300) * 100 / (4200 - 3300);
        if (pct < 0) pct = 0;
        if (pct > 100) pct = 100;
        out->soc_pct = (uint8_t)pct;
    }
    return out->present;
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
