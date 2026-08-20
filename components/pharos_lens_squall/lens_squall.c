/* Pharos lens: Squall - is the air busy, broken, or being denied?
 *
 * The only lens that reasons about the frames that did NOT arrive. It consumes
 * PHAROS_EV_DWELL - one summary per channel visit - rather than individual
 * frames, so its whole view of the world is "what did this channel sound
 * like", which is exactly the question jamming poses.
 *
 * All judgement lives in the pure, host-tested engine (pharos_squall). This
 * file only forwards dwell summaries and hands the verdict out.
 */
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "pharos_bus.h"
#include "pharos_lens.h"
#include "pharos_radio.h"
#include "pharos_report.h"
#include "pharos_squall.h"

static const char *TAG = "lens.squall";

#define SQUALL_RING 256

EXT_RAM_BSS_ATTR static pharos_event_t s_slots[SQUALL_RING];
static pharos_bus_t s_bus;

EXT_RAM_BSS_ATTR static pq_state_table_t s_table;
static pq_verdict_t s_verdict;
static SemaphoreHandle_t s_lock;
static pq_state_t s_last_state;

static bool squall_mount(void)
{
    pq_reset(&s_table);
    memset(&s_verdict, 0, sizeof(s_verdict));
    s_last_state = PQ_STATE_UNKNOWN;
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    return s_lock != NULL && pharos_bus_init(&s_bus, s_slots, SQUALL_RING);
}

static bool squall_start(void)
{
    pharos_scan_plan_t plan = pharos_scan_plan_survey();
    /* Long enough per visit that a dwell summary means something, short enough
     * that the whole band is covered in a few seconds - jamming is a condition
     * to be mapped across channels, not chased on one. */
    plan.dwell_ms = 500;
    plan.want_mgmt = true;
    plan.want_data = true; /* retries are mostly data frames */
    return pharos_radio_rx_start(&plan, &s_bus);
}

static void squall_stop(void)
{
    pharos_radio_rx_stop();
}

static void squall_event(const pharos_event_t *ev)
{
    if (!ev || ev->type != PHAROS_EV_DWELL) {
        return; /* this lens listens to visits, not to frames */
    }
    const pharos_ev_dwell_t *d = &ev->u.dwell;
    pq_dwell_t in = {
        .channel = d->channel,
        .dwell_ms = d->dwell_ms,
        .frames = d->frames,
        .retries = d->retries,
        .noise_floor = d->noise_floor,
        .peak_rssi = d->peak_rssi,
        .busy_permil = d->busy_permil,
    };
    pq_observe(&s_table, &in);
}

static void squall_tick(uint32_t dt_ms)
{
    (void)dt_ms;
    if (xSemaphoreTake(s_lock, 0) != pdTRUE) {
        return;
    }
    pq_context_t ctx = {
        .dwell_permil = (uint16_t)pharos_radio_dwell_permil(pharos_radio_channel()),
        .camped = pharos_radio_is_camped(),
    };
    pq_evaluate(&s_table, &ctx, &s_verdict);

    if (s_verdict.worst != s_last_state) {
        ESP_LOGI(TAG, "%s ch%u score=%u/%u graded=%u denial=%u retry=%u%% \"%s\"",
                 pq_state_name(s_verdict.worst), s_verdict.worst_channel,
                 s_verdict.score, s_verdict.ceiling, s_verdict.n_graded,
                 s_verdict.n_denial, s_verdict.retry_permil / 10,
                 s_verdict.headline);
        s_last_state = s_verdict.worst;
    }
    xSemaphoreGive(s_lock);
}

bool pharos_lens_squall_snapshot(pq_verdict_t *out)
{
    if (!out || !s_lock) {
        return false;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(5)) != pdTRUE) {
        return false;
    }
    *out = s_verdict;
    xSemaphoreGive(s_lock);
    return true;
}

bool pharos_lens_squall_report(char *buf, size_t cap, prt_redact_t redact, uint32_t salt)
{
    prt_t w;
    prt_init(&w, buf, cap, redact, salt);
    prt_obj_begin(&w, NULL);
    prt_str(&w, "tool", "pharos");
    prt_str(&w, "lens", "wifi.squall");
    prt_u32(&w, "uptime_ms", (uint32_t)(esp_timer_get_time() / 1000));
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
        prt_str(&w, "state", pq_state_name(s_verdict.worst));
        prt_u32(&w, "worst_channel", s_verdict.worst_channel);
        prt_u32(&w, "score", s_verdict.score);
        prt_u32(&w, "ceiling", s_verdict.ceiling);
        prt_u32(&w, "families", s_verdict.families);
        prt_u32(&w, "notes", s_verdict.notes);
        prt_u32(&w, "channels_graded", s_verdict.n_graded);
        prt_u32(&w, "channels_denial", s_verdict.n_denial);
        prt_u32(&w, "channels_congested", s_verdict.n_congested);
        prt_u32(&w, "retry_permil", s_verdict.retry_permil);
        prt_str(&w, "advice", pq_state_advice(s_verdict.worst));
        xSemaphoreGive(s_lock);
    }
    prt_obj_end(&w);
    return prt_finish(&w);
}

/* Aegis: denial of service is a DISRUPT-stage finding, same family as a deauth
 * flood - somebody is stopping the network working. */
static bool k_squall_stage(uint8_t *stage, uint8_t *score, uint8_t *ceiling)
{
    *stage = 2; /* PA_STAGE_DISRUPT */
    *score = s_verdict.score;
    *ceiling = s_verdict.ceiling;
    return true;
}

static struct pharos_bus *squall_ingest(void) { return &s_bus; }

static bool k_squall_display(struct pharos_lens_display *o)
{
    pq_verdict_t v = s_verdict;
    snprintf(o->big, sizeof(o->big), "%u", v.score);
    snprintf(o->band, sizeof(o->band), "%s", pq_state_name(v.worst));
    snprintf(o->detail, sizeof(o->detail), "ch%u  %u graded  retry %u%%",
             v.worst_channel, v.n_graded, v.retry_permil / 10u);
    snprintf(o->advice, sizeof(o->advice), "%s", v.headline ? v.headline : "");
    o->score = v.score; o->ceiling = v.ceiling; o->has_score = true;
    return true;
}

/* Squall grades the air per channel; the headline can only carry the worst
 * one. This says which, and how many others are in the same state. */
static bool k_squall_row(unsigned index, struct pharos_lens_row *out)
{
    pq_verdict_t v;
    if (!pharos_lens_squall_snapshot(&v)) {
        return false;
    }
    switch (index) {
    case 0:
        snprintf(out->left, sizeof(out->left), "worst channel");
        snprintf(out->right, sizeof(out->right), "ch %u", (unsigned)v.worst_channel);
        out->tone = PHAROS_TONE_WARN;
        return true;
    case 1:
        snprintf(out->left, sizeof(out->left), "channels graded");
        snprintf(out->right, sizeof(out->right), "%u", (unsigned)v.n_graded);
        out->tone = PHAROS_TONE_DIM;
        return true;
    case 2:
        snprintf(out->left, sizeof(out->left), "showing denial");
        snprintf(out->right, sizeof(out->right), "%u", (unsigned)v.n_denial);
        out->tone = v.n_denial ? PHAROS_TONE_BAD : PHAROS_TONE_GOOD;
        return true;
    case 3:
        snprintf(out->left, sizeof(out->left), "merely congested");
        snprintf(out->right, sizeof(out->right), "%u", (unsigned)v.n_congested);
        /* Congested is not attacked, and conflating them is how a busy office
         * gets reported as a jamming incident. */
        out->tone = PHAROS_TONE_NEUTRAL;
        return true;
    case 4:
        snprintf(out->left, sizeof(out->left), "retransmissions");
        snprintf(out->right, sizeof(out->right), "%u%%",
                 (unsigned)(v.retry_permil / 10u));
        out->tone = (v.retry_permil >= 400u) ? PHAROS_TONE_BAD
                  : (v.retry_permil >= 200u) ? PHAROS_TONE_WARN
                                             : PHAROS_TONE_GOOD;
        return true;
    default:
        return false;
    }
}

static const pharos_lens_t k_squall = {
    .id = "wifi.squall",
    .name = "Squall",
    .summary = "Busy, broken, or jammed - tells the three apart",
    .glyph = "storm",
    .kind = PHAROS_LENS_OBSERVE,
    .caps = PHAROS_CAP_WIFI_RX | PHAROS_CAP_WIFI_CHAN | PHAROS_CAP_STORAGE_W,
    .budget_ma = 135,
    .on_mount = squall_mount,
    .on_start = squall_start,
    .on_stop = squall_stop,
    .on_tick = squall_tick,
    .on_event = squall_event,
    .ingest = squall_ingest,
    .stage_report = k_squall_stage,
    .display = k_squall_display,
    .row = k_squall_row,
    .row_head_left = "AIR QUALITY",
    .row_head_right = "VALUE",
};

PHAROS_LENS_REGISTER(&k_squall);
