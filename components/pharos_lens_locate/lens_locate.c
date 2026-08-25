/* Pharos lens: Locate - walk toward a flagged transmitter
 *
 * The follow-up to a detection. When Watch, Karma or Mirage names a suspect,
 * the operator hands that BSSID to Locate, which camps on its channel and
 * plays a hotter/colder game from the suspect's own RSSI. It is the physical
 * half of an investigation - and, being receive-only, it finds a transmitter
 * without ever becoming one.
 *
 * Judgement is in pharos_locate.c, pure and host-tested. This file feeds the
 * engine the RSSI of frames from the target and hands the UI a snapshot. The
 * target is set by the operator via pharos_lens_locate_set_target(); with no
 * target the lens listens and says so.
 */
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "pharos_bus.h"
#include "pharos_dot11.h"
#include "pharos_lens.h"
#include "pharos_locate.h"
#include "pharos_radio.h"

static const char *TAG = "lens.locate";

#define LOCATE_RING 256

EXT_RAM_BSS_ATTR static pharos_event_t s_slots[LOCATE_RING];
static pharos_bus_t s_bus;
static pl_engine_t s_engine;
static pl_verdict_t s_verdict;
static uint8_t s_target[6];
static bool s_has_target;
static uint8_t s_channel;
static SemaphoreHandle_t s_lock;

static bool locate_mount(void)
{
    pl_reset(&s_engine, s_has_target ? s_target : NULL);
    memset(&s_verdict, 0, sizeof(s_verdict));
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    return s_lock && pharos_bus_init(&s_bus, s_slots, LOCATE_RING);
}

static bool locate_start(void)
{
    /* Camp on the target's channel: a hotter/colder game is meaningless while
     * hopping, because most samples would be of a channel the target is not
     * on. If no channel is known yet, survey until the target is seen. */
    pharos_scan_plan_t plan = s_channel ? pharos_scan_plan_camp(s_channel)
                                        : pharos_scan_plan_survey();
    plan.want_mgmt = true;
    plan.want_data = true; /* more frames from the target = a smoother needle */
    return pharos_radio_rx_start(&plan, &s_bus);
}

static void locate_stop(void) { pharos_radio_rx_stop(); }

static void locate_event(const pharos_event_t *ev)
{
    if (!ev || ev->type != PHAROS_EV_DOT11 || !s_has_target) {
        return;
    }
    if (xSemaphoreTake(s_lock, 0) != pdTRUE) {
        return;
    }
    /* Feed transmitter-address RSSI to the engine; it ignores anything that is
     * not the target. */
    pl_observe(&s_engine, ev->u.dot11.a2, ev->u.dot11.rssi, ev->t_us);
    xSemaphoreGive(s_lock);
}

static void locate_tick(uint32_t dt_ms)
{
    (void)dt_ms;
    if (xSemaphoreTake(s_lock, 0) != pdTRUE) {
        return;
    }
    pl_verdict_t v;
    pl_evaluate(&s_engine, &v);
    if (v.trend != s_verdict.trend) {
        ESP_LOGI(TAG, "%s  rssi=%d peak=%d close=%u%% conf=%u%%",
                 pl_trend_name(v.trend), v.rssi_smoothed, v.rssi_peak,
                 v.closeness, v.confidence);
    }
    s_verdict = v;
    xSemaphoreGive(s_lock);
}

/* Hand Locate a suspect (typically from another lens' verdict) and the channel
 * it was seen on. Re-arms the engine. */
void pharos_lens_locate_set_target(const uint8_t bssid[6], uint8_t channel)
{
    if (!bssid) {
        return;
    }
    memcpy(s_target, bssid, 6);
    s_has_target = true;
    s_channel = channel;
    if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
        pl_reset(&s_engine, s_target);
        xSemaphoreGive(s_lock);
    }
    /* If we are the active lens, re-camp on the new channel. */
    if (pharos_lens_active() && strcmp(pharos_lens_active()->id, "wifi.locate") == 0) {
        pharos_radio_rx_stop();
        locate_start();
    }
    ESP_LOGI(TAG, "target %02X:%02X:%02X:%02X:%02X:%02X ch%u",
             bssid[0], bssid[1], bssid[2], bssid[3], bssid[4], bssid[5], channel);
}

bool pharos_lens_locate_snapshot(pl_verdict_t *out)
{
    if (!out || !s_lock || xSemaphoreTake(s_lock, pdMS_TO_TICKS(5)) != pdTRUE) {
        return false;
    }
    *out = s_verdict;
    xSemaphoreGive(s_lock);
    return true;
}

static struct pharos_bus *locate_ingest(void) { return &s_bus; }


/* ---- Locate had no display at all -------------------------------------
 *
 * The lens that exists to walk you toward a transmitter was showing a frame
 * counter, which is worse than nothing: the ONE number an operator needs while
 * moving is whether they are getting warmer, and a counter that always rises
 * looks exactly like getting warmer.
 *
 * The headline is the trend word. The gauge is closeness on a fixed scale -
 * never a distance, because RSSI is not one and pretending otherwise is how
 * people walk confidently into the wrong room. */
static bool k_locate_display(struct pharos_lens_display *o)
{
    if (!s_lock || xSemaphoreTake(s_lock, 0) != pdTRUE) {
        return false;
    }
    const pl_verdict_t v = s_verdict;
    const bool have = s_has_target;
    const uint8_t ch = s_channel;
    uint8_t t[6];
    memcpy(t, s_target, 6);
    xSemaphoreGive(s_lock);

    if (!have) {
        snprintf(o->big, sizeof(o->big), "--");
        snprintf(o->band, sizeof(o->band), "NO TARGET");
        snprintf(o->detail, sizeof(o->detail), "set one from another lens");
        snprintf(o->advice, sizeof(o->advice), "Nothing to walk toward yet.");
        o->has_score = false;
        return true;
    }
    snprintf(o->big, sizeof(o->big), "%u", v.closeness);
    snprintf(o->band, sizeof(o->band), "%s", pl_trend_name(v.trend));
    snprintf(o->detail, sizeof(o->detail), "%02x:%02x:%02x ch%u  %d dBm",
             t[3], t[4], t[5], (unsigned)ch, (int)v.rssi_smoothed);
    snprintf(o->advice, sizeof(o->advice), "%s",
             v.locked ? "Walk slowly; watch the trend."
                      : "Hold still to settle the reading.");
    if (!v.locked) {
        snprintf(o->why, sizeof(o->why), "not enough samples to trust yet");
    }
    o->score = v.closeness;
    /* Confidence IS the ceiling here: a trend from six samples is a guess and
     * the arc should show how much of the scale this reading has earned. */
    o->ceiling = v.confidence;
    o->has_score = true;

    /* CLOSENESS IS NOT A THREAT SCALE.
     *
     * Without this the face colours 81% closeness with the threat palette and
     * the whole screen goes red as you walk TOWARD the thing you are looking
     * for - which reads as "danger" at exactly the moment it means "found it".
     * Locate is pointed at a transmitter another lens already flagged; getting
     * nearer is the goal, not a finding. So the reading is declared as
     * carrying no alarm and the ring fills green as you close in.
     *
     * (The old face had the same bug and it mattered less, because a thin arc
     * is easy to ignore. An aura is not.) */
    o->alert = 0;
    o->has_alert = true;
    return true;
}

static bool k_locate_row(unsigned index, struct pharos_lens_row *out)
{
    if (!s_lock || xSemaphoreTake(s_lock, 0) != pdTRUE) {
        return false;
    }
    const pl_verdict_t v = s_verdict;
    uint8_t t[6];
    memcpy(t, s_target, 6);
    const bool have = s_has_target;
    const uint8_t ch = s_channel;
    xSemaphoreGive(s_lock);

    switch (index) {
    case 0:
        snprintf(out->left, sizeof(out->left), "target");
        if (have) {
            snprintf(out->right, sizeof(out->right), "%02x:%02x:%02x",
                     t[3], t[4], t[5]);
            out->tone = PHAROS_TONE_NEUTRAL;
        } else {
            snprintf(out->right, sizeof(out->right), "none");
            out->tone = PHAROS_TONE_DIM;
        }
        return true;
    case 1:
        snprintf(out->left, sizeof(out->left), "channel");
        snprintf(out->right, sizeof(out->right), "%u", (unsigned)ch);
        out->tone = PHAROS_TONE_DIM;
        return true;
    case 2:
        snprintf(out->left, sizeof(out->left), "trend");
        snprintf(out->right, sizeof(out->right), "%.11s", pl_trend_name(v.trend));
        out->tone = v.locked ? PHAROS_TONE_WARN : PHAROS_TONE_DIM;
        return true;
    case 3:
        snprintf(out->left, sizeof(out->left), "signal now");
        snprintf(out->right, sizeof(out->right), "%d dBm", (int)v.rssi_now);
        out->tone = PHAROS_TONE_NEUTRAL;
        return true;
    case 4:
        snprintf(out->left, sizeof(out->left), "smoothed");
        snprintf(out->right, sizeof(out->right), "%d dBm", (int)v.rssi_smoothed);
        out->tone = PHAROS_TONE_NEUTRAL;
        return true;
    case 5:
        snprintf(out->left, sizeof(out->left), "best seen");
        snprintf(out->right, sizeof(out->right), "%d dBm", (int)v.rssi_peak);
        out->tone = PHAROS_TONE_NEUTRAL;
        return true;
    case 6:
        snprintf(out->left, sizeof(out->left), "confidence");
        snprintf(out->right, sizeof(out->right), "%u%%", v.confidence);
        out->tone = (v.confidence >= 60) ? PHAROS_TONE_GOOD : PHAROS_TONE_WARN;
        return true;
    case 7:
        snprintf(out->left, sizeof(out->left), "samples");
        snprintf(out->right, sizeof(out->right), "%u", (unsigned)v.samples);
        out->tone = PHAROS_TONE_DIM;
        return true;
    case 8:
        /* Said out loud, because it is the single most misread thing here. */
        snprintf(out->left, sizeof(out->left), "distance");
        snprintf(out->right, sizeof(out->right), "not shown");
        out->tone = PHAROS_TONE_WARN;
        return true;
    default:
        return false;
    }
}

static const pharos_lens_t k_locate = {
    .id = "wifi.locate",
    .purpose = "walk to a transmitter",
    .name = "Locate",
    .summary = "Walk toward a flagged transmitter - hotter, colder, here",
    .glyph = "compass",
    .kind = PHAROS_LENS_ANALYSE,
    .caps = PHAROS_CAP_WIFI_RX | PHAROS_CAP_WIFI_CHAN,
    .budget_ma = 130,
    .on_mount = locate_mount,
    .on_start = locate_start,
    .on_stop = locate_stop,
    .on_tick = locate_tick,
    .on_event = locate_event,
    .ingest = locate_ingest,
    .display = k_locate_display,
    .row = k_locate_row,
    .row_head_left = "READING",
    .row_head_right = "VALUE",
};

PHAROS_LENS_REGISTER(&k_locate);
