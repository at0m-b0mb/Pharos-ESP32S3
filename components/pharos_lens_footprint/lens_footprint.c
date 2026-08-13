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

    pw_engine_t eng;
    pw_reset(&eng);
    pharos_event_t ev;
    uint64_t last = 0;
    while (pr_range_next(&r, &ev)) {
        if (ev.type == PHAROS_EV_DOT11) {
            pw_observe(&eng, &ev.u.dot11, ev.t_us);
        }
        last = ev.t_us;
    }

    pw_context_t camped = { .dwell_permil = 1000, .bus_yield_permil = 1000,
                            .window_ms = 12000 };
    pw_context_t hopping = { .dwell_permil = 71, .bus_yield_permil = 1000,
                             .window_ms = 12000 };
    pw_evaluate(&eng, last, &camped, &s_camped);
    pw_evaluate(&eng, last, &hopping, &s_hopping);
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
};

PHAROS_LENS_REGISTER(&k_footprint);
