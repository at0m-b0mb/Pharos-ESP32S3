/* Pharos lens: Range - the training simulator
 *
 * A red-and-blue teaching lens that holds no radio capability at all. It plays
 * a synthesised scenario through the real detection engines and narrates the
 * verdict as it forms, so a learner can see - on the exact same gauge they use
 * in the field - how an attack earns its score and where detection refuses to
 * be certain.
 *
 * Because it declares no wifi.rx and no ble.scan, the HAL would refuse it a
 * radio even if it asked. The events come from pharos_range, the arithmetic
 * from pharos_watch and friends; there is no path from this lens to an antenna.
 */
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "pharos_lens.h"
#include "pharos_range.h"
#include "pharos_watch.h"

static const char *TAG = "lens.range";

static pr_range_t s_range;
EXT_RAM_BSS_ATTR static pw_engine_t s_engine;
static pw_verdict_t s_verdict;
static pr_scenario_t s_scenario = PR_SCENARIO_DEAUTH_FLOOD;
static SemaphoreHandle_t s_lock;
static bool s_running;

static void load_scenario(pr_scenario_t s)
{
    pr_config_t cfg = {
        .scenario = s,
        .seed = (uint32_t)esp_timer_get_time(),
        .dwell_permil = 1000, /* the learner toggles camped/hopping in the UI */
        .intensity = 700,
    };
    pr_range_init(&s_range, &cfg);
    pw_reset(&s_engine);
    memset(&s_verdict, 0, sizeof(s_verdict));
    s_scenario = s;
    ESP_LOGI(TAG, "scenario: %s - %s", pr_scenario_name(s), pr_scenario_teaches(s));
}

static bool range_mount(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    load_scenario(s_scenario);
    return s_lock != NULL;
}

static bool range_start(void)
{
    s_running = true;
    return true; /* no radio: this is the whole point of the range */
}

static void range_stop(void) { s_running = false; }

/* Advance the virtual scenario at a human-watchable pace and re-grade. */
static void range_tick(uint32_t dt_ms)
{
    (void)dt_ms;
    if (!s_running || xSemaphoreTake(s_lock, 0) != pdTRUE) {
        return;
    }
    pharos_event_t ev;
    for (int i = 0; i < 8; i++) { /* a few events per frame */
        if (!pr_range_next(&s_range, &ev)) {
            /* Scenario finished; loop it so a learner can keep watching. */
            load_scenario(s_scenario);
            break;
        }
        if (ev.type == PHAROS_EV_DOT11) {
            pw_observe(&s_engine, &ev.u.dot11, ev.t_us);
        }
    }
    pw_context_t ctx = { .dwell_permil = s_range.cfg.dwell_permil,
                         .bus_yield_permil = 1000, .window_ms = 12000 };
    pw_evaluate(&s_engine, s_range.t_us, &ctx, &s_verdict);
    xSemaphoreGive(s_lock);
}

void pharos_lens_range_select(pr_scenario_t s)
{
    if (s < PR_SCENARIO_COUNT && xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
        load_scenario(s);
        xSemaphoreGive(s_lock);
    }
}

void pharos_lens_range_set_dwell(uint16_t dwell_permil)
{
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
        s_range.cfg.dwell_permil = dwell_permil ? dwell_permil : 71;
        xSemaphoreGive(s_lock);
    }
}

bool pharos_lens_range_snapshot(pw_verdict_t *out, pr_scenario_t *scenario)
{
    if (!out || !s_lock || xSemaphoreTake(s_lock, pdMS_TO_TICKS(5)) != pdTRUE) {
        return false;
    }
    *out = s_verdict;
    if (scenario) *scenario = s_scenario;
    xSemaphoreGive(s_lock);
    return true;
}

/* The training range played scenarios through the real engines and showed a
 * frame counter, so the one lens whose entire purpose is TEACHING what a
 * verdict means displayed the least meaningful number on the device.
 *
 * It now shows the verdict the scenario is producing, live, next to the name
 * of the scenario producing it - which is the lesson. */
static bool k_range_display(struct pharos_lens_display *o)
{
    pw_verdict_t v;
    pr_scenario_t sc;
    if (!pharos_lens_range_snapshot(&v, &sc)) {
        return false;
    }
    snprintf(o->big, sizeof(o->big), "%u", v.score);
    snprintf(o->band, sizeof(o->band), "%s", pw_band_name(v.band));
    /* "FLOOD LIKELY 77" on a screen in a quiet building was read, reasonably,
     * as an attack in progress. The banner above says SIMULATION; this line
     * says the same thing in the grammar of the reading itself, so the number
     * cannot be taken for a measurement even at a glance. */
    snprintf(o->detail, sizeof(o->detail), "if real: %.20s", pr_scenario_name(sc));
    snprintf(o->advice, sizeof(o->advice), "%.34s", pr_scenario_teaches(sc));
    o->families = v.families;
    o->fam_label[0] = "RATE";
    o->fam_label[1] = "SHAPE";
    o->fam_label[2] = "FORGE";
    o->fam_label[3] = "AFTER";
    o->score = v.score;
    o->raw_score = v.raw_score;
    o->ceiling = v.ceiling;
    o->has_score = true;
    /* A drill, not the room. See pharos_lens_display::simulated. */
    o->simulated = true;
    return true;
}

static bool k_range_row(unsigned index, struct pharos_lens_row *out)
{
    pw_verdict_t v;
    pr_scenario_t sc;
    if (!pharos_lens_range_snapshot(&v, &sc)) {
        return false;
    }
    if (index == 0) {
        snprintf(out->left, sizeof(out->left), "scenario");
        snprintf(out->right, sizeof(out->right), "%.11s", pr_scenario_name(sc));
        out->tone = PHAROS_TONE_NEUTRAL; return true;
    }
    if (index == 1) {
        snprintf(out->left, sizeof(out->left), "teaches");
        snprintf(out->right, sizeof(out->right), "%s", "see below");
        out->tone = PHAROS_TONE_DIM; return true;
    }
    /* The rest is the same evidence breakdown the live Watch lens shows, so
     * that what a learner reads here is literally what they will read in the
     * field rather than a simplified teaching version of it. */
    switch (index - 2) {
    case 0:
        snprintf(out->left, sizeof(out->left), "rate");
        snprintf(out->right, sizeof(out->right), "%u/34", v.c_rate);
        out->tone = (v.families & PW_FAM_RATE) ? PHAROS_TONE_WARN : PHAROS_TONE_DIM;
        return true;
    case 1:
        snprintf(out->left, sizeof(out->left), "targeting shape");
        snprintf(out->right, sizeof(out->right), "%u/22", v.c_shape);
        out->tone = (v.families & PW_FAM_SHAPE) ? PHAROS_TONE_WARN : PHAROS_TONE_DIM;
        return true;
    case 2:
        snprintf(out->left, sizeof(out->left), "forgery");
        snprintf(out->right, sizeof(out->right), "%u/30", v.c_forgery);
        out->tone = (v.families & PW_FAM_FORGERY) ? PHAROS_TONE_BAD : PHAROS_TONE_DIM;
        return true;
    case 3:
        snprintf(out->left, sizeof(out->left), "aftermath");
        snprintf(out->right, sizeof(out->right), "%u/18", v.c_aftermath);
        out->tone = (v.families & PW_FAM_AFTERMATH) ? PHAROS_TONE_BAD : PHAROS_TONE_DIM;
        return true;
    case 4:
        snprintf(out->left, sizeof(out->left), "earned / allowed");
        snprintf(out->right, sizeof(out->right), "%u/%u", v.raw_score, v.ceiling);
        /* The whole lesson in one row: what the evidence was worth, and what
         * this receiver was entitled to claim from it. */
        out->tone = (v.raw_score > v.score) ? PHAROS_TONE_WARN : PHAROS_TONE_DIM;
        return true;
    default: return false;
    }
}

static const pharos_lens_t k_range = {
    .id = "train.range",
    .name = "Range",
    .summary = "Practise reading attacks on the real gauge - never transmits",
    .glyph = "target",
    .kind = PHAROS_LENS_TRAIN,
    .caps = PHAROS_CAP_NONE, /* no radio, by design */
    .budget_ma = 45,
    .on_mount = range_mount,
    .on_start = range_start,
    .on_stop = range_stop,
    .on_tick = range_tick,
    .display = k_range_display,
    .row = k_range_row,
    .row_head_left = "LESSON",
    .row_head_right = "VALUE",
};

PHAROS_LENS_REGISTER(&k_range);
