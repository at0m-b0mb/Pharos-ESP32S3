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

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "pharos_lens.h"
#include "pharos_range.h"
#include "pharos_watch.h"

static const char *TAG = "lens.range";

static pr_range_t s_range;
static pw_engine_t s_engine;
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
};

PHAROS_LENS_REGISTER(&k_range);
