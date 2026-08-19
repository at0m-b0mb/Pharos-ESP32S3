/* Pharos lens: Footprint - the red team's mirror
 *
 * A training lens with no radio at all. It takes an attack scenario, grades it
 * with the REAL Watch engine against two receiver postures - a camped defender
 * and a hopping one - and reports how detectable it is: the grade, the family
 * that gives it away, and whether a defender who only hops would even notice.
 *
 * This is how a receive-only tool serves a red team honestly. It cannot run
 * the attack; it can show, from the watchtower's side, exactly what the attack
 * looks like and what makes it loud. Understanding your own signature is
 * tradecraft - and it is precisely what a blue team wants a red team to learn.
 *
 * All the reasoning is in pharos_opsec.c and pharos_watch.c, both pure and
 * host-tested. This lens is the loop that plays the scenario and snapshots the
 * assessment for the UI. It declares no capabilities, so the HAL would refuse
 * it a radio even if it asked.
 */
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "pharos_lens.h"
#include "pharos_opsec.h"
#include "pharos_range.h"
#include "pharos_watch.h"

static const char *TAG = "lens.footprint";

static pr_scenario_t s_scenario = PR_SCENARIO_DEAUTH_FLOOD;

/* THE ENGINE IS A STATIC IN PSRAM, NOT A LOCAL, AND THE REASON IS A CRASH.
 *
 * pw_engine_t is 10,720 bytes - an AP table, a 256-entry disconnect ring and
 * the per-second slots. assess() used to declare one on the STACK, and
 * pharos_ui_run() is called straight from app_main(), so this code runs on the
 * main task with CONFIG_ESP_MAIN_TASK_STACK_SIZE = 8192 bytes.
 *
 * It fit, barely, while the engine was about 6 KB. The v2 rewrite grew it past
 * the stack and the device died the moment this lens was opened:
 *
 *     ***ERROR*** A stack overflow in task main has been detected.
 *
 * A ten-kilobyte automatic is not a near miss to be tuned around; it does not
 * belong on any task's stack. It lives in .bss in PSRAM instead, like every
 * other large lens buffer in this project (see the note on EXT_RAM_BSS_ATTR in
 * the other lenses). Only one lens is active at a time and assess() runs on
 * the UI task alone, so a single shared instance is safe.
 *
 * tools/check_sources.sh now fails the build if any lens declares one of these
 * on the stack again. */
EXT_RAM_BSS_ATTR static pw_engine_t s_engine;

static pw_verdict_t s_camped, s_hopping;
static po_report_t s_report;
static SemaphoreHandle_t s_lock;

/* Play the whole scenario through the Watch engine, then grade it twice. This
 * is the identical code path the field lens uses; only the receiver posture
 * differs between the two gradings. */
static void assess(pr_scenario_t scenario)
{
    pr_config_t cfg = {
        .scenario = scenario,
        .seed = (uint32_t)esp_timer_get_time(),
        .dwell_permil = 1000,
        .intensity = 750,
    };
    pr_range_t r;
    pr_range_init(&r, &cfg);

    pw_reset(&s_engine);
    pharos_event_t ev;
    uint64_t last = 0;
    while (pr_range_next(&r, &ev)) {
        if (ev.type == PHAROS_EV_DOT11) {
            pw_observe(&s_engine, &ev.u.dot11, ev.t_us);
        }
        last = ev.t_us;
    }

    pw_context_t camped = { .dwell_permil = 1000, .bus_yield_permil = 1000,
                            .window_ms = 12000 };
    pw_context_t hopping = { .dwell_permil = 71, .bus_yield_permil = 1000,
                             .window_ms = 12000 };
    pw_evaluate(&s_engine, last, &camped, &s_camped);
    pw_evaluate(&s_engine, last, &hopping, &s_hopping);
    po_assess(&s_camped, &s_hopping, &s_report);
    s_scenario = scenario;

    ESP_LOGI(TAG, "%s: %s  tell=%s  camped=%u hopping=%u gap=%u%s",
             pr_scenario_name(scenario), po_grade_name(s_report.grade),
             s_report.tell_name, s_report.camped_score, s_report.hopping_score,
             s_report.stealth_gap,
             s_report.invisible_to_hoppers ? "  (hoppers miss it)" : "");
}

static bool footprint_mount(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    assess(s_scenario);
    return s_lock != NULL;
}

static bool footprint_start(void) { return true; } /* no radio, by design */

/* Re-assess periodically so the reading breathes; the scenario is fixed until
 * the operator picks another. */
static void footprint_tick(uint32_t dt_ms)
{
    static uint32_t acc;
    acc += dt_ms;
    if (acc < 2000) {
        return;
    }
    acc = 0;
    if (xSemaphoreTake(s_lock, 0) == pdTRUE) {
        assess(s_scenario);
        xSemaphoreGive(s_lock);
    }
}

void pharos_lens_footprint_select(pr_scenario_t scenario)
{
    if (scenario < PR_SCENARIO_COUNT && s_lock &&
        xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
        assess(scenario);
        xSemaphoreGive(s_lock);
    }
}

bool pharos_lens_footprint_snapshot(po_report_t *out, pr_scenario_t *scenario)
{
    if (!out || !s_lock || xSemaphoreTake(s_lock, pdMS_TO_TICKS(5)) != pdTRUE) {
        return false;
    }
    *out = s_report;
    if (scenario) *scenario = s_scenario;
    xSemaphoreGive(s_lock);
    return true;
}

/* Footprint had no display, so the red-team OPSEC lens showed a frame counter
 * while holding a full detectability report. The headline is the grade a
 * CAMPED defender would give this attack - the worst case for whoever is
 * running it, which is the only number worth planning against. */
static bool k_footprint_display(struct pharos_lens_display *o)
{
    po_report_t r;
    pw_verdict_t c, h;
    if (!s_lock || xSemaphoreTake(s_lock, pdMS_TO_TICKS(5)) != pdTRUE) {
        return false;
    }
    r = s_report; c = s_camped; h = s_hopping;
    const pr_scenario_t sc = s_scenario;
    xSemaphoreGive(s_lock);

    snprintf(o->big, sizeof(o->big), "%u", r.camped_score);
    snprintf(o->band, sizeof(o->band), "%s", po_grade_name(r.grade));
    snprintf(o->detail, sizeof(o->detail), "%.14s  hop %u", pr_scenario_name(sc),
             r.hopping_score);
    snprintf(o->advice, sizeof(o->advice), "%s",
             r.invisible_to_hoppers ? "A hopping defender would miss it."
                                    : "A defender sees this either way.");
    snprintf(o->why, sizeof(o->why), "%.47s", r.tell_name ? r.tell_name : "");
    o->score = r.camped_score;
    o->ceiling = c.ceiling ? c.ceiling : 100;
    o->has_score = true;
    (void)h;
    return true;
}

static bool k_footprint_row(unsigned index, struct pharos_lens_row *out)
{
    po_report_t r;
    if (!s_lock || xSemaphoreTake(s_lock, pdMS_TO_TICKS(5)) != pdTRUE) {
        return false;
    }
    r = s_report;
    xSemaphoreGive(s_lock);

    switch (index) {
    case 0:
        snprintf(out->left, sizeof(out->left), "camped defender");
        snprintf(out->right, sizeof(out->right), "%u", r.camped_score);
        out->tone = (r.camped_score >= 75) ? PHAROS_TONE_BAD : PHAROS_TONE_WARN;
        return true;
    case 1:
        snprintf(out->left, sizeof(out->left), "hopping defender");
        snprintf(out->right, sizeof(out->right), "%u", r.hopping_score);
        out->tone = PHAROS_TONE_NEUTRAL; return true;
    case 2:
        snprintf(out->left, sizeof(out->left), "stealth gap");
        snprintf(out->right, sizeof(out->right), "%u", r.stealth_gap);
        /* The operationally useful number: how much a defender loses by
         * refusing to stand still. */
        out->tone = PHAROS_TONE_WARN; return true;
    case 3:
        snprintf(out->left, sizeof(out->left), "families lit");
        snprintf(out->right, sizeof(out->right), "%u", r.families_lit);
        out->tone = PHAROS_TONE_DIM; return true;
    case 4:
        snprintf(out->left, sizeof(out->left), "loudest tell");
        snprintf(out->right, sizeof(out->right), "%.11s",
                 r.tell_name ? r.tell_name : "--");
        out->tone = PHAROS_TONE_BAD; return true;
    case 5:
        snprintf(out->left, sizeof(out->left), "hoppers miss it");
        snprintf(out->right, sizeof(out->right), "%s",
                 r.invisible_to_hoppers ? "yes" : "no");
        out->tone = r.invisible_to_hoppers ? PHAROS_TONE_WARN : PHAROS_TONE_GOOD;
        return true;
    default: return false;
    }
}

static const pharos_lens_t k_footprint = {
    .id = "train.footprint",
    .name = "Footprint",
    .summary = "How loud is your attack? The red team's mirror - never transmits",
    .glyph = "eye",
    .kind = PHAROS_LENS_TRAIN,
    .caps = PHAROS_CAP_NONE,
    .budget_ma = 45,
    .on_mount = footprint_mount,
    .on_start = footprint_start,
    .on_tick = footprint_tick,
    .display = k_footprint_display,
    .row = k_footprint_row,
    .row_head_left = "DETECTABILITY",
    .row_head_right = "VALUE",
};

PHAROS_LENS_REGISTER(&k_footprint);
