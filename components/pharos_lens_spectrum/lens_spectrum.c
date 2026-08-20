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

#include "esp_attr.h"
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

EXT_RAM_BSS_ATTR static pharos_event_t s_slots[SPEC_RING];
static pharos_bus_t s_bus;
static SemaphoreHandle_t s_lock;

typedef struct {
    uint16_t frames;
    int8_t peak_rssi;
    int8_t noise_floor;
    uint16_t beacons;
} spec_cell_t;

/* TWO TABLES, AND THE DIFFERENCE IS THE WHOLE FIX.
 *
 * s_cur accumulates while the receiver is ON a channel. s_last is what it
 * heard during that channel's most recent COMPLETED visit, and it is what the
 * map and the waterfall read.
 *
 * The first attempt kept one table and decayed it on a timer, which cannot
 * work: a channel is only visited for 120 ms out of every 2.6 second sweep, so
 * any decay fast enough to be current is fast enough to erase a channel before
 * the receiver returns to it. Worse, the decay was integer - (n * 7) / 8 turns
 * 1 into 0 - so a channel with one or two beacons was wiped on the very first
 * fold and only the single loudest channel ever had a value. The map showed
 * one bar and thirteen empty rows while Census was listing access points on
 * six different channels.
 *
 * The radio already publishes PHAROS_EV_DWELL when it leaves a channel,
 * carrying exactly what that visit heard. So the map now reports the last
 * visit rather than a decaying blend of all of them - which is also the more
 * honest statement: "when I was last on channel 6, this is what was there". */
static spec_cell_t s_cur[PHAROS_CHAN_MAX + 1];
static spec_cell_t s_now[PHAROS_CHAN_MAX + 1];
EXT_RAM_BSS_ATTR static uint8_t s_waterfall[SPEC_HISTORY][PHAROS_CHAN_MAX + 1]; /* busy 0..255 */
static uint8_t s_row;

static bool spectrum_mount(void)
{
    memset(s_cur, 0, sizeof(s_cur));
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
    if (!ev) {
        return;
    }
    /* The receiver has left a channel: that visit is now a complete
     * observation, so publish it and start the next one clean. */
    if (ev->type == PHAROS_EV_DWELL) {
        const uint8_t dc = ev->u.dwell.channel;
        if (dc >= 1 && dc <= PHAROS_CHAN_MAX) {
            s_now[dc] = s_cur[dc];
            memset(&s_cur[dc], 0, sizeof(s_cur[dc]));
        }
        return;
    }
    if (ev->type != PHAROS_EV_DOT11) {
        return;
    }
    const uint8_t ch = ev->u.dot11.channel;
    if (ch > PHAROS_CHAN_MAX) {
        return;
    }
    spec_cell_t *c = &s_cur[ch];
    c->frames++;
    if (c->frames == 1 || ev->u.dot11.rssi > c->peak_rssi) {
        c->peak_rssi = ev->u.dot11.rssi;
    }
    if (ev->u.dot11.subtype == PHAROS_ST_BEACON) {
        c->beacons++;
    }
}

/* THE DECAY IS PER SWEEP, NOT PER TICK.
 *
 * This ran on the UI tick - about twenty times a second - and halved every
 * channel's frame count each time. The radio dwells 120-200 ms on a channel
 * and a full sweep of thirteen takes some 2.6 seconds, so by the time it came
 * back to channel 2 that channel had been halved fifty times and read zero.
 * The result was a "channel map" that only ever showed the channel the
 * receiver happened to be sitting on, and a waterfall that scrolled its entire
 * history in under a second.
 *
 * Both are only useful if a channel REMEMBERS its last visit until the next
 * one. So the fold happens once a second and the decay is gentle: a channel
 * keeps most of its value across a sweep and fades over tens of seconds if
 * the traffic really has stopped. */
#define SPEC_FOLD_MS 1000u

static uint32_t s_fold_accum;

static void spectrum_tick(uint32_t dt_ms)
{
    s_fold_accum += dt_ms;
    if (s_fold_accum < SPEC_FOLD_MS) {
        return;
    }
    s_fold_accum = 0;

    if (xSemaphoreTake(s_lock, 0) != pdTRUE) {
        return;
    }
    /* One waterfall row a second, from the last completed visit to each
     * channel. Nothing is decayed here any more: a channel's value is
     * replaced when the receiver next visits it, which is the only moment
     * there is anything new to say about it. Busy is a log-ish compression of
     * the frame count so one loud channel cannot saturate the strip. */
    for (unsigned c = 1; c <= PHAROS_CHAN_MAX; c++) {
        const uint32_t f = s_now[c].frames;
        const uint8_t busy = (f == 0) ? 0
                           : (f < 4) ? 40
                           : (f < 16) ? 90
                           : (f < 64) ? 150
                           : (f < 256) ? 210 : 255;
        s_waterfall[s_row][c] = busy;
    }
    s_row = (uint8_t)((s_row + 1) % SPEC_HISTORY);
    xSemaphoreGive(s_lock);
}


static struct pharos_bus *spectrum_ingest(void) { return &s_bus; }

/* ---- what this lens actually knows -----------------------------------
 *
 * Spectrum had no display and no detail page, so the generic face showed a
 * FRAME COUNTER: a number that climbed forever and told the operator nothing
 * about the air. It has a per-channel table and a waterfall the whole time.
 *
 * The headline is the BUSIEST channel, because that is the actionable fact -
 * it is where to camp, and it is where a flood would be hiding. The detail
 * page is the channel map: every channel, what is on it, and how loud. */
static bool k_spectrum_display(struct pharos_lens_display *o)
{
    if (!s_lock || xSemaphoreTake(s_lock, 0) != pdTRUE) {
        return false;
    }
    unsigned busiest = 0, total_frames = 0, total_beacons = 0;
    uint16_t best = 0;
    for (unsigned c = 1; c <= PHAROS_CHAN_MAX; c++) {
        total_frames += s_now[c].frames;
        total_beacons += s_now[c].beacons;
        if (s_now[c].frames > best) {
            best = s_now[c].frames;
            busiest = c;
        }
    }
    const int8_t peak = busiest ? s_now[busiest].peak_rssi : 0;
    xSemaphoreGive(s_lock);

    if (!total_frames) {
        snprintf(o->big, sizeof(o->big), "--");
        snprintf(o->band, sizeof(o->band), "listening");
        snprintf(o->detail, sizeof(o->detail), "no traffic heard yet");
        snprintf(o->advice, sizeof(o->advice), "Sweeping 2.4 GHz.");
        o->has_score = false;
        return true;
    }
    snprintf(o->big, sizeof(o->big), "%u", busiest);
    snprintf(o->band, sizeof(o->band), "BUSIEST CH");
    snprintf(o->detail, sizeof(o->detail), "%u frames  %u beacons  %d dBm",
             total_frames, total_beacons, (int)peak);
    snprintf(o->advice, sizeof(o->advice), "Camp here to hear it properly.");
    /* The gauge is this channel's share of everything heard - a crowded band
     * and one loud channel are different pictures and the arc should say
     * which. */
    o->score = (uint8_t)((total_frames ? (best * 100u) / total_frames : 0u));
    o->ceiling = 100;
    o->has_score = true;

    /* A CROWDED BAND IS NOT AN ATTACK.
     *
     * This score is one channel's share of the airtime, and in any ordinary
     * flat it is high - the Wi-Fi is simply busy. Read as a threat it put an
     * orange dot on the home ring and made the whole screen say "something is
     * up" about a working router. Spectrum is a picture to go and look at, not
     * a watch that raises anything. See pharos_lens_display::alert. */
    o->has_alert = true;
    o->alert = (o->score >= 80u) ? 1u : 0u;
    return true;
}

/* The channel map. This is the "what is actually out there" the operator
 * came for, and it is one row per channel rather than a number to trust. */
static bool k_spectrum_row(unsigned index, struct pharos_lens_row *out)
{
    if (index >= PHAROS_CHAN_MAX) {
        return false;
    }
    if (!s_lock || xSemaphoreTake(s_lock, 0) != pdTRUE) {
        return false;
    }
    const unsigned c = index + 1u;
    const spec_cell_t cell = s_now[c];
    uint16_t busiest = 0;
    for (unsigned k = 1; k <= PHAROS_CHAN_MAX; k++) {
        if (s_now[k].frames > busiest) busiest = s_now[k].frames;
    }
    xSemaphoreGive(s_lock);

    /* A tiny bar so the map reads as a spectrum at a glance rather than as a
     * column of numbers. Eight steps is all a 26-character cell can carry and
     * all the eye needs to find the peak. */
    char bar[9];
    const unsigned fill = busiest ? (cell.frames * 8u) / busiest : 0u;
    for (unsigned i = 0; i < 8; i++) {
        bar[i] = (i < fill) ? '#' : '.';
    }
    bar[8] = '\0';

    snprintf(out->left, sizeof(out->left), "ch %-2u %s %3u ap", c, bar,
             (unsigned)(cell.beacons > 999 ? 999 : cell.beacons));
    if (cell.frames) {
        snprintf(out->right, sizeof(out->right), "%d dBm", (int)cell.peak_rssi);
    } else {
        snprintf(out->right, sizeof(out->right), "quiet");
    }
    out->tone = !cell.frames                 ? PHAROS_TONE_DIM
              : (busiest && cell.frames == busiest) ? PHAROS_TONE_WARN
                                                    : PHAROS_TONE_NEUTRAL;
    return true;
}

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
    .display = k_spectrum_display,
    .row = k_spectrum_row,
    .row_head_left = "CHANNEL  ACTIVITY",
    .row_head_right = "PEAK",
};

PHAROS_LENS_REGISTER(&k_spectrum);
