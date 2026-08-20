/* Pharos lens: Footprint - the red team's mirror, on the real air
 *
 * "How loud am I?" - answered by measuring, not by replaying a script.
 *
 * ---------------------------------------------------------------------------
 * WHY IT USED TO SHOW A SIMULATION, AND WHY THAT WAS NOT GOOD ENOUGH
 *
 * This lens grades an attack the way a defender's watchtower would, and the
 * first version did it by playing a synthesised scenario through the real
 * Watch engine. The arithmetic was genuine; the traffic was not. So it wore a
 * SIMULATION banner - which was honest, and which also meant the one lens
 * aimed at somebody testing their own tooling could not be pointed at their
 * own tooling. It was reported exactly that way: a sensor that only shows
 * test data.
 *
 * It now holds a receiver. Run your attack, point this at it, and it reports
 * what a defender standing on that channel would have seen - the grade, the
 * single family that gave you away, and whether a defender who merely HOPS
 * would have noticed at all.
 *
 * ---------------------------------------------------------------------------
 * THE TWO POSTURES, FROM ONE CAPTURE
 *
 * The trick that makes this honest is that both gradings come from the SAME
 * frames. The engine's confidence ceiling is a function of how much of the
 * channel the receiver heard (see pharos_watch.h), so the same evidence graded
 * against a camped context and a hopping one produces the two numbers this
 * lens exists to compare:
 *
 *   camped  - what somebody standing on your channel sees. The worst case for
 *             whoever is running the attack, and the only number worth
 *             planning against.
 *   hopping - what a defender sweeping thirteen channels sees. Usually much
 *             less, and the gap between the two is the stealth an attacker is
 *             relying on whether they know it or not.
 *
 * Nothing here helps anybody attack anything: it is the defender's own engine,
 * pointed at the room, reporting what the defender would conclude. Knowing
 * your own signature is what makes an authorised operator careful, and it is
 * exactly what a blue team wants a red team to internalise.
 *
 * It still cannot transmit. It never could.
 */
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "pharos_bus.h"
#include "pharos_lens.h"
#include "pharos_opsec.h"
#include "pharos_radio.h"
#include "pharos_range.h"
#include "pharos_watch.h"

static const char *TAG = "lens.footprint";

#define FP_RING 512 /* a burst is exactly when the ring matters */

EXT_RAM_BSS_ATTR static pharos_event_t s_slots[FP_RING];
static pharos_bus_t s_bus;

/* Kept only so the drill mode still has a scenario to name; live capture does
 * not use it. */
static pr_scenario_t s_scenario = PR_SCENARIO_DEAUTH_FLOOD;

/* LIVE UNLESS THERE IS NOTHING TO MEASURE.
 *
 * A lens that shows a drill while claiming to measure the room is the failure
 * this rewrite exists to fix, so the rule is one-directional: it starts live,
 * and only says SIMULATION when the operator has explicitly asked for a
 * scenario. Silence on the air is reported as silence, never quietly filled
 * in with a rehearsal. */
static bool s_drill;
static uint32_t s_frames;

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
/* GRADE WHAT IS ACTUALLY IN THE AIR.
 *
 * Both postures come from the same captured frames - that is what makes the
 * comparison meaningful rather than two separate guesses. Only the receiver
 * context differs: one describes a defender standing on this channel, the
 * other one sweeping thirteen. */
static void assess_live(void)
{
    const uint64_t now = (uint64_t)esp_timer_get_time();
    const uint8_t chan = pharos_radio_channel();

    /* The camped view uses the dwell this receiver ACTUALLY achieved, not a
     * hopeful 100%. Claiming a full-airtime ceiling for a capture that only
     * heard a fraction of the channel would flatter the attacker's score in
     * the one direction that gets somebody caught. */
    pw_context_t camped = {
        .dwell_permil = pharos_radio_is_camped()
                            ? pharos_radio_dwell_permil(chan)
                            : 1000,
        .bus_yield_permil = pharos_bus_yield_permil(&s_bus),
        .window_ms = 15000,
    };
    pw_context_t hopping = { .dwell_permil = 71,
                             .bus_yield_permil = pharos_bus_yield_permil(&s_bus),
                             .window_ms = 15000 };
    pw_evaluate(&s_engine, now, &camped, &s_camped);
    pw_evaluate(&s_engine, now, &hopping, &s_hopping);
    po_assess(&s_camped, &s_hopping, &s_report);
}

/* The drill, kept for the training case and for when there is no radio. Same
 * engine, synthesised traffic - and the display says so unmistakably. */
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
    s_drill = true;

    ESP_LOGI(TAG, "DRILL %s: %s  tell=%s  camped=%u hopping=%u",
             pr_scenario_name(scenario), po_grade_name(s_report.grade),
             s_report.tell_name, s_report.camped_score, s_report.hopping_score);
}

static bool footprint_mount(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    pw_reset(&s_engine);
    memset(&s_camped, 0, sizeof(s_camped));
    memset(&s_hopping, 0, sizeof(s_hopping));
    memset(&s_report, 0, sizeof(s_report));
    s_drill = false;
    s_frames = 0;
    return s_lock && pharos_bus_init(&s_bus, s_slots, FP_RING);
}

static bool footprint_start(void)
{
    pharos_scan_plan_t plan = pharos_scan_plan_survey();
    plan.want_mgmt = true;
    return pharos_radio_rx_start(&plan, &s_bus);
}

static void footprint_stop(void) { pharos_radio_rx_stop(); }

/* Analytics core. */
static void footprint_event(const pharos_event_t *ev)
{
    if (!ev || ev->type != PHAROS_EV_DOT11) {
        return;
    }
    s_frames++;
    pw_observe(&s_engine, &ev->u.dot11, ev->t_us);
}

static struct pharos_bus *footprint_ingest(void) { return &s_bus; }

/* Re-assess periodically so the reading breathes; the scenario is fixed until
 * the operator picks another. */
static void footprint_tick(uint32_t dt_ms)
{
    static uint32_t acc;
    acc += dt_ms;
    if (acc < 1000) {
        return;
    }
    acc = 0;
    if (xSemaphoreTake(s_lock, 0) != pdTRUE) {
        return;
    }
    /* Live unless somebody asked for a drill. Never the other way round. */
    if (!s_drill) {
        assess_live();
    }
    xSemaphoreGive(s_lock);
}

void pharos_lens_footprint_select(pr_scenario_t scenario);

/* Back to measuring the room. */
void pharos_lens_footprint_live(void)
{
    if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
        s_drill = false;
        pw_reset(&s_engine);
        s_frames = 0;
        xSemaphoreGive(s_lock);
        ESP_LOGI(TAG, "footprint: measuring the real air");
    }
}

/* The centre tap: drill off, drill on. Somebody who wants to see what a known
 * attack shape looks like can still have it - explicitly, and labelled. */
static void footprint_select(void)
{
    if (s_drill) {
        pharos_lens_footprint_live();
    } else {
        pharos_lens_footprint_select(s_scenario);
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
    const bool drill = s_drill;
    const uint32_t frames = s_frames;
    xSemaphoreGive(s_lock);

    snprintf(o->big, sizeof(o->big), "%u", r.camped_score);
    snprintf(o->band, sizeof(o->band), "%s", po_grade_name(r.grade));

    if (drill) {
        snprintf(o->detail, sizeof(o->detail), "%s", "drill, not this room");
        snprintf(o->why, sizeof(o->why), "%.14s - a hopper scores %u",
                 pr_scenario_name(sc), r.hopping_score);
    } else if (frames < 20u) {
        /* NOTHING HEARD IS A RESULT, NOT A GAP TO FILL.
         *
         * The old version played a scenario whenever it had nothing, which is
         * how a lens ends up showing test data forever. Quiet air is reported
         * as quiet air. */
        snprintf(o->detail, sizeof(o->detail), "%s", "listening to this room");
        snprintf(o->why, sizeof(o->why), "%u frames so far", (unsigned)frames);
    } else {
        snprintf(o->detail, sizeof(o->detail), "%s", "to a defender who camps");
        snprintf(o->why, sizeof(o->why), "a hopper scores %u of %u",
                 r.hopping_score, r.camped_score);
    }

    snprintf(o->advice, sizeof(o->advice), "%s",
             (!drill && frames < 20u)
                 ? "Run your tooling; this grades what it hears."
                 : (r.invisible_to_hoppers ? "A hopping defender would miss it."
                                           : "A defender sees this either way."));
    o->score = r.camped_score;
    o->ceiling = c.ceiling ? c.ceiling : 100;
    o->has_score = true;
    /* ONLY when it really is one. This lens measures the room by default now,
     * and the banner has to mean something. */
    o->simulated = drill;
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
        /* The tell goes in the LEFT column: it is a phrase, and eleven
         * characters of one is not a finding. */
        snprintf(out->left, sizeof(out->left), "%.25s",
                 r.tell_name ? r.tell_name : "no single tell");
        snprintf(out->right, sizeof(out->right), "loudest");
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
    .summary = "How loud is what you are running? The red team's mirror",
    .glyph = "eye",
    /* OBSERVE, not TRAIN: it measures the room. */
    .kind = PHAROS_LENS_OBSERVE,
    .caps = PHAROS_CAP_WIFI_RX | PHAROS_CAP_WIFI_CHAN,
    .budget_ma = 130,
    .on_mount = footprint_mount,
    .on_start = footprint_start,
    .on_stop = footprint_stop,
    .on_event = footprint_event,
    .ingest = footprint_ingest,
    .on_select = footprint_select,
    .on_tick = footprint_tick,
    .display = k_footprint_display,
    .row = k_footprint_row,
    .row_head_left = "DETECTABILITY",
    .row_head_right = "VALUE",
};

PHAROS_LENS_REGISTER(&k_footprint);
