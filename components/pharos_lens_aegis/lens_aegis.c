/* Pharos lens: Aegis - the one screen that tells the story
 *
 * Every other lens answers one question and then forgets. Aegis holds the
 * accumulated picture: which stages of an attack have fired, how long ago, and
 * whether they arrived in an order that makes them an operation rather than a
 * noisy afternoon.
 *
 * It holds NO radio. It cannot, and should not: the findings are pushed to it
 * by whichever lens is active, through pharos_lens_t::stage_report, and the UI
 * loop does the forwarding. That is what makes the latch work across lens
 * changes - and across the operator not looking, which is the normal case for
 * a device with one small round screen.
 *
 * The state itself lives in pharos_ui, because the UI loop is the one thing
 * that runs continuously and always knows which lens is active. This file is
 * the lens that displays it and the operator's acknowledge button.
 */
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "pharos_aegis.h"
#include "pharos_lens.h"
#include "pharos_report.h"
#include "pharos_ui.h"

static const char *TAG = "lens.aegis";

static pa_verdict_t s_verdict;
static pa_band_t s_last_band;

static bool aegis_mount(void)
{
    memset(&s_verdict, 0, sizeof(s_verdict));
    s_last_band = PA_BAND_CLEAR;
    return true;
}

static void aegis_tick(uint32_t dt_ms)
{
    (void)dt_ms;
    if (!pharos_ui_aegis_snapshot(&s_verdict)) {
        return;
    }
    if (s_verdict.band != s_last_band) {
        ESP_LOGI(TAG, "%s score=%u/%u raised=%u live=%u worst=%s(%u, %us ago) \"%s\"",
                 pa_band_name(s_verdict.band), s_verdict.score, s_verdict.ceiling,
                 s_verdict.n_raised, s_verdict.n_live,
                 pa_stage_name(s_verdict.worst), s_verdict.worst_peak,
                 (unsigned)s_verdict.worst_age_s, s_verdict.headline);
        s_last_band = s_verdict.band;
    }
}

bool pharos_lens_aegis_snapshot(pa_verdict_t *out)
{
    if (!out) {
        return false;
    }
    return pharos_ui_aegis_snapshot(out);
}

/* The operator has seen it and is starting a fresh watch. Deliberate, like
 * adopting a baseline: the device never clears its own memory of a finding. */
void pharos_lens_aegis_acknowledge(void)
{
    pharos_ui_aegis_ack();
    memset(&s_verdict, 0, sizeof(s_verdict));
    s_last_band = PA_BAND_CLEAR;
}

bool pharos_lens_aegis_report(char *buf, size_t cap, prt_redact_t redact, uint32_t salt)
{
    pa_verdict_t v;
    if (!pharos_ui_aegis_snapshot(&v)) {
        memset(&v, 0, sizeof(v));
    }
    prt_t w;
    prt_init(&w, buf, cap, redact, salt);
    prt_obj_begin(&w, NULL);
    prt_str(&w, "tool", "pharos");
    prt_str(&w, "lens", "sys.aegis");
    prt_u32(&w, "uptime_ms", (uint32_t)(esp_timer_get_time() / 1000));
    prt_str(&w, "band", pa_band_name(v.band));
    prt_u32(&w, "score", v.score);
    prt_u32(&w, "ceiling", v.ceiling);
    prt_u32(&w, "stages_raised", v.n_raised);
    prt_u32(&w, "stages_live", v.n_live);
    prt_u32(&w, "stage_mask", v.stage_mask);
    prt_str(&w, "worst_stage", pa_stage_name(v.worst));
    prt_u32(&w, "worst_peak", v.worst_peak);
    prt_u32(&w, "worst_age_s", v.worst_age_s);
    prt_bool(&w, "latched", (v.notes & PA_NOTE_LATCHED) != 0);
    prt_bool(&w, "sequence", (v.notes & PA_NOTE_SEQUENCE) != 0);
    prt_str(&w, "advice", pa_band_advice(v.band));
    prt_obj_end(&w);
    return prt_finish(&w);
}

static const pharos_lens_t k_aegis = {
    .id = "sys.aegis",
    .name = "Aegis",
    .summary = "Every finding so far, and what it adds up to",
    .glyph = "shield",
    .kind = PHAROS_LENS_ANALYSE,
    /* No radio at all. Aegis reasons about what the other lenses already
     * heard; it never listens for itself. */
    .caps = PHAROS_CAP_STORAGE_W,
    .budget_ma = 25,
    .on_mount = aegis_mount,
    .on_tick = aegis_tick,
};

PHAROS_LENS_REGISTER(&k_aegis);
