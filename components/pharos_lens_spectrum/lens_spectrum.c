/* Pharos lens: Spectrum - the 2.4 GHz airtime waterfall
 *
 * The orientation tool. Before you judge anything, you look: which channels
 * are busy, where the beacons are, where the noise is. It is the map the rest
 * of the lenses are read against.
 *
 * It also carries the single most important honest disclaimer in the product,
 * on screen, permanently: this radio hears 2.4 GHz only. Most modern office
 * and home traffic has moved to 5 and 6 GHz, where this device is deaf. A
 * quiet waterfall is not a quiet building.
 */
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "pharos_bus.h"
#include "pharos_lens.h"
#include "pharos_radio.h"
#include "pharos_region.h"

#define SPEC_RING 512
#define SPEC_HISTORY 64 /* waterfall rows */

static pharos_event_t s_slots[SPEC_RING];
static pharos_bus_t s_bus;
static SemaphoreHandle_t s_lock;

typedef struct {
    uint16_t frames;
    int8_t peak_rssi;
    int8_t noise_floor;
    uint16_t beacons;
} spec_cell_t;

static spec_cell_t s_now[PHAROS_CHAN_MAX + 1];
static uint8_t s_waterfall[SPEC_HISTORY][PHAROS_CHAN_MAX + 1]; /* busy 0..255 */
static uint8_t s_row;

static bool spectrum_mount(void)
{
    memset(s_now, 0, sizeof(s_now));
    memset(s_waterfall, 0, sizeof(s_waterfall));
    s_row = 0;
    for (unsigned c = 0; c <= PHAROS_CHAN_MAX; c++) {
        s_now[c].peak_rssi = -128;
        s_now[c].noise_floor = -95;
    }
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    return s_lock && pharos_bus_init(&s_bus, s_slots, SPEC_RING);
}

static bool spectrum_start(void)
{
    pharos_scan_plan_t plan = pharos_scan_plan_survey();
    plan.dwell_ms = 120; /* quick sweeps keep the waterfall moving */
    plan.want_mgmt = true;
    plan.want_data = true;
    return pharos_radio_rx_start(&plan, &s_bus);
}

static void spectrum_stop(void) { pharos_radio_rx_stop(); }

static void spectrum_event(const pharos_event_t *ev)
{
    if (!ev || ev->type != PHAROS_EV_DOT11) {
        return;
    }
    const uint8_t ch = ev->u.dot11.channel;
    if (ch > PHAROS_CHAN_MAX) {
        return;
    }
    spec_cell_t *c = &s_now[ch];
    c->frames++;
    if (ev->u.dot11.rssi > c->peak_rssi) {
        c->peak_rssi = ev->u.dot11.rssi;
    }
    if (ev->u.dot11.subtype == PHAROS_ST_BEACON) {
        c->beacons++;
    }
}

static void spectrum_tick(uint32_t dt_ms)
{
    (void)dt_ms;
    if (xSemaphoreTake(s_lock, 0) != pdTRUE) {
        return;
    }
    /* Fold the current per-channel counts into a new waterfall row, then
     * decay the live counts so the next window starts fresh. Busy is a log-ish
     * compression of frame count so one loud channel does not saturate the
     * whole strip. */
    for (unsigned c = 1; c <= PHAROS_CHAN_MAX; c++) {
        uint32_t f = s_now[c].frames;
        uint8_t busy = (f == 0) ? 0
                     : (f < 4) ? 40
                     : (f < 16) ? 90
                     : (f < 64) ? 150
                     : (f < 256) ? 210 : 255;
        s_waterfall[s_row][c] = busy;
        s_now[c].frames = (uint16_t)(s_now[c].frames / 2);
        s_now[c].beacons = (uint16_t)(s_now[c].beacons * 3 / 4);
        if (s_now[c].peak_rssi > -128) {
            s_now[c].peak_rssi = (int8_t)(s_now[c].peak_rssi - 1); /* slow decay */
        }
    }
    s_row = (uint8_t)((s_row + 1) % SPEC_HISTORY);
    xSemaphoreGive(s_lock);
}

/* UI reads: the current per-channel bars and the scrolling history. */
unsigned pharos_lens_spectrum_bars(uint8_t *busy_out, int8_t *peak_out, unsigned max)
{
    if (!s_lock || xSemaphoreTake(s_lock, pdMS_TO_TICKS(5)) != pdTRUE) {
        return 0;
    }
    const uint8_t hi = pharos_region_max_channel();
    unsigned n = 0;
    for (unsigned c = 1; c <= hi && n < max; c++, n++) {
        if (busy_out) busy_out[n] = s_waterfall[(s_row + SPEC_HISTORY - 1) % SPEC_HISTORY][c];
        if (peak_out) peak_out[n] = s_now[c].peak_rssi;
    }
    xSemaphoreGive(s_lock);
    return n;
}

static struct pharos_bus *spectrum_ingest(void) { return &s_bus; }

static const pharos_lens_t k_spectrum = {
    .id = "wifi.spectrum",
    .name = "Spectrum",
    .summary = "2.4 GHz airtime waterfall - the map you read the others against",
    .glyph = "waves",
    .kind = PHAROS_LENS_OBSERVE,
    .caps = PHAROS_CAP_WIFI_RX | PHAROS_CAP_WIFI_CHAN,
    .budget_ma = 128,
    .on_mount = spectrum_mount,
    .on_start = spectrum_start,
    .on_stop = spectrum_stop,
    .on_tick = spectrum_tick,
    .on_event = spectrum_event,
    .ingest = spectrum_ingest,
};

PHAROS_LENS_REGISTER(&k_spectrum);
