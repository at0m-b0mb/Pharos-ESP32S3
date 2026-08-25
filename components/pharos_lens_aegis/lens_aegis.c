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
#include <stdio.h>
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

static bool k_aegis_display(struct pharos_lens_display *o)
{
    pa_verdict_t v;
    if (!pharos_ui_aegis_snapshot(&v)) {
        return false;
    }
    snprintf(o->big, sizeof(o->big), "%u", v.score);
    snprintf(o->band, sizeof(o->band), "%s", pa_band_name(v.band));
    if (v.worst_peak) {
        snprintf(o->detail, sizeof(o->detail), "%s %u, %us ago",
                 pa_stage_name(v.worst), v.worst_peak, (unsigned)v.worst_age_s);
    } else {
        snprintf(o->detail, sizeof(o->detail), "%u raised  %u live",
                 v.n_raised, v.n_live);
    }
    snprintf(o->advice, sizeof(o->advice), "%s", v.headline ? v.headline : "");
    o->score = v.score; o->ceiling = v.ceiling; o->has_score = true;
    return true;
}

/* Aegis is the only lens that can see the whole engagement, because it is the
 * one thing that keeps running while the operator moves between the others.
 * Its detail page is therefore the closest this device has to a case summary. */
static bool k_aegis_row(unsigned index, struct pharos_lens_row *out)
{
    pa_verdict_t v;
    if (!pharos_lens_aegis_snapshot(&v)) return false;
    switch (index) {
    case 0:
        snprintf(out->left, sizeof(out->left), "stages raised");
        snprintf(out->right, sizeof(out->right), "%u", (unsigned)v.n_raised);
        out->tone = v.n_raised ? PHAROS_TONE_BAD : PHAROS_TONE_GOOD; return true;
    case 1:
        snprintf(out->left, sizeof(out->left), "still current");
        snprintf(out->right, sizeof(out->right), "%u", (unsigned)v.n_live);
        out->tone = v.n_live ? PHAROS_TONE_BAD : PHAROS_TONE_NEUTRAL; return true;
    case 2:
        snprintf(out->left, sizeof(out->left), "worst peak");
        snprintf(out->right, sizeof(out->right), "%u", (unsigned)v.worst_peak);
        out->tone = (v.worst_peak >= 75) ? PHAROS_TONE_BAD
                  : (v.worst_peak >= 40) ? PHAROS_TONE_WARN : PHAROS_TONE_DIM;
        return true;
    case 3: {
        snprintf(out->left, sizeof(out->left), "how long ago");
        /* Clamped so the formatted result cannot outgrow the 12-byte column
         * on a device that has been running for a day. */
        uint32_t s_ago = v.worst_age_s;
        if (s_ago >= 60u) {
            uint32_t m = s_ago / 60u;
            if (m > 999u) m = 999u;
            snprintf(out->right, sizeof(out->right), "%um ago", (unsigned)m);
        } else {
            snprintf(out->right, sizeof(out->right), "%us ago", (unsigned)s_ago);
        }
        out->tone = PHAROS_TONE_DIM; return true;
    }
    case 4:
        snprintf(out->left, sizeof(out->left), "earned / allowed");
        snprintf(out->right, sizeof(out->right), "%u/%u", v.raw_score, v.ceiling);
        out->tone = PHAROS_TONE_DIM; return true;
    default: return false;
    }
}

static const pharos_lens_t k_aegis = {
    .id = "sys.aegis",
    .purpose = "the whole picture",
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
    .display = k_aegis_display,
    .row = k_aegis_row,
    .row_head_left = "ENGAGEMENT",
    .row_head_right = "VALUE",
};

PHAROS_LENS_REGISTER(&k_aegis);
