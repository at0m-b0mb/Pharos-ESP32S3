#include "pharos_aegis.h"

#include <string.h>

static uint32_t clamp_u32(uint32_t v, uint32_t lo, uint32_t hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

void pa_reset(pa_state_t *s)
{
    if (s) {
        memset(s, 0, sizeof(*s));
    }
}

void pa_observe(pa_state_t *s, pa_stage_t stage, uint8_t score, uint8_t ceiling,
                uint64_t t_us)
{
    if (!s || stage < 0 || stage >= PA_STAGE_COUNT) {
        return;
    }
    pa_stage_state_t *st = &s->stages[stage];
    st->current = score;
    st->last_us = t_us;
    if (t_us > s->now_us) {
        s->now_us = t_us;
    }

    if (score >= PA_STAGE_ALARM) {
        if (!st->raised) {
            st->raised = true;
            st->first_us = t_us;
        }
        st->hits++;
    }
    /* The high-water mark, and the ceiling that came with it. Keeping the
     * ceiling alongside the peak is what stops a later, better-quality sweep
     * from lending its confidence to an old, thin observation. */
    if (score > st->peak) {
        st->peak = score;
        st->ceiling = ceiling;
        st->peak_us = t_us;
    }
}

void pa_acknowledge(pa_state_t *s)
{
    if (!s) {
        return;
    }
    const uint64_t now = s->now_us;
    memset(s->stages, 0, sizeof(s->stages));
    s->now_us = now;
}

/* Stage weights. Reconnaissance is common and cheap - every phone in the room
 * probes - so it is worth very little on its own. Impersonation and collection
 * are deliberate acts that need a human's attention. */
static uint32_t stage_weight_permil(pa_stage_t st)
{
    switch (st) {
    case PA_STAGE_RECON:       return 600;
    case PA_STAGE_IMPERSONATE: return 1000;
    case PA_STAGE_DISRUPT:     return 950;
    case PA_STAGE_HARVEST:     return 1000;
    case PA_STAGE_DRIFT:       return 800;
    default:                   return 800;
    }
}

static pa_band_t band_of(uint8_t score)
{
    if (score >= 70) return PA_BAND_INCIDENT;
    if (score >= 45) return PA_BAND_ELEVATED;
    if (score >= 20) return PA_BAND_NOTED;
    return PA_BAND_CLEAR;
}

void pa_evaluate(const pa_state_t *s, uint64_t now_us, pa_verdict_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->headline = "";
    out->worst = PA_STAGE_RECON;
    if (!s) {
        return;
    }
    if (now_us < s->now_us) {
        now_us = s->now_us;
    }

    /* The ceiling is the MINIMUM across contributing stages: a conclusion is
     * only as trustworthy as its least trustworthy input. Starts at 100 and is
     * pulled down by whatever actually contributed. */
    uint32_t ceiling = 100;
    uint32_t best_weighted = 0;
    uint8_t raised_peak = 0;
    uint64_t prev_first = 0;
    bool ordered = true;
    bool any_contrib = false;

    for (int i = 0; i < PA_STAGE_COUNT; i++) {
        const pa_stage_state_t *st = &s->stages[i];
        if (st->peak == 0) {
            continue;
        }
        any_contrib = true;
        if (st->ceiling && st->ceiling < ceiling) {
            ceiling = st->ceiling;
        }
        if (st->peak > out->worst_peak) {
            out->worst_peak = st->peak;
            out->worst = (pa_stage_t)i;
            out->worst_age_s = (uint32_t)((now_us - st->peak_us) / 1000000ull);
        }
        const uint32_t w = (st->peak * stage_weight_permil((pa_stage_t)i)) / 1000u;
        if (w > best_weighted) {
            best_weighted = w;
        }
        if (st->raised) {
            out->n_raised++;
            out->stage_mask |= (uint8_t)(1u << i);
            if (st->peak > raised_peak) {
                raised_peak = st->peak;
            }
            /* Did the stages arrive in the attacker's own order? The loop runs
             * in stage order, so first-seen times must be non-decreasing. */
            if (st->first_us < prev_first) {
                ordered = false;
            }
            prev_first = st->first_us;
            if ((now_us - st->last_us) <= PA_FRESH_US && st->current >= PA_STAGE_ALARM) {
                out->n_live++;
            }
        }
    }

    if (!any_contrib) {
        out->ceiling = 100;
        out->band = PA_BAND_CLEAR;
        out->headline = "Nothing raised. No lens has reported a finding.";
        return;
    }
    out->ceiling = (uint8_t)ceiling;

    uint32_t score;
    if (out->n_raised == 0) {
        /* Background only: report the loudest thing, weighted, and nothing
         * more. Correlation has no business amplifying quiet readings. */
        score = best_weighted;
        out->notes |= PA_NOTE_SINGLE;
    } else if (out->n_raised == 1) {
        /* THE rule that keeps this honest: with one stage raised there is
         * nothing to correlate, so Aegis reports that stage's own score
         * unchanged. Fusion may add confidence only when there is genuinely
         * more than one independent thing to fuse. */
        score = raised_peak;
        out->notes |= PA_NOTE_SINGLE;
    } else {
        /* Several independent stages. Start from the worst and add for each
         * additional one - a lookalike access point AND clients being knocked
         * onto it is a materially different situation from either alone. */
        score = raised_peak + clamp_u32((out->n_raised - 1u) * 9u, 0, 27);

        /* And if they arrived in the attacker's own order, that is a sequence
         * rather than a noisy room. */
        if (ordered && out->n_raised >= 3) {
            out->notes |= PA_NOTE_SEQUENCE;
            score += 8;
        }
    }

    /* Age. A peak from twenty minutes ago is a real fact about the building,
     * but it is not the present tense and the device must not imply it is. */
    const pa_stage_state_t *w = &s->stages[out->worst];
    if ((now_us - w->peak_us) > PA_FRESH_US) {
        out->notes |= PA_NOTE_LATCHED;
    }
    bool any_fresh = false;
    for (int i = 0; i < PA_STAGE_COUNT; i++) {
        if (s->stages[i].peak && (now_us - s->stages[i].last_us) <= PA_FRESH_US) {
            any_fresh = true;
            break;
        }
    }
    if (!any_fresh) {
        out->notes |= PA_NOTE_STALE;
    }

    score = clamp_u32(score, 0, 100);
    out->raw_score = (uint8_t)score;
    if (score > out->ceiling) {
        score = out->ceiling;
    }
    out->score = (uint8_t)score;
    out->band = band_of(out->score);

    switch (out->band) {
    case PA_BAND_INCIDENT:
        if (out->notes & PA_NOTE_SEQUENCE) {
            out->headline = "Several stages, in attack order: treat as an operation";
        } else if (out->notes & PA_NOTE_LATCHED) {
            /* The score is high but the peak is history. Saying "this is an
             * incident" in the present tense would send the operator hunting
             * for something that stopped happening ten minutes ago. */
            out->headline = "Something serious happened here while you were not watching";
        } else {
            out->headline = "Several findings at once - this is an incident";
        }
        break;
    case PA_BAND_ELEVATED:
        out->headline = (out->notes & PA_NOTE_LATCHED)
                            ? "Something happened here while you were not watching"
                            : "One real finding is live right now";
        break;
    case PA_BAND_NOTED:
        out->headline = "Background activity worth knowing about";
        break;
    case PA_BAND_CLEAR:
    default:
        out->headline = "Nothing raised in what has been swept so far";
        break;
    }
}

const char *pa_stage_name(pa_stage_t st)
{
    switch (st) {
    case PA_STAGE_RECON:       return "RECON";
    case PA_STAGE_IMPERSONATE: return "IMPERSONATE";
    case PA_STAGE_DISRUPT:     return "DISRUPT";
    case PA_STAGE_HARVEST:     return "HARVEST";
    case PA_STAGE_DRIFT:       return "DRIFT";
    default:                   return "?";
    }
}

const char *pa_stage_meaning(pa_stage_t st)
{
    switch (st) {
    case PA_STAGE_RECON:
        return "Somebody is looking: probing, or cataloguing what is here.";
    case PA_STAGE_IMPERSONATE:
        return "A radio is wearing a name that belongs to somebody else.";
    case PA_STAGE_DISRUPT:
        return "Clients are being pushed off, or the air is being filled.";
    case PA_STAGE_HARVEST:
        return "Handshakes are being collected for offline attack.";
    case PA_STAGE_DRIFT:
        return "The estate is not configured the way your baseline recorded.";
    default:
        return "";
    }
}

const char *pa_band_name(pa_band_t b)
{
    switch (b) {
    case PA_BAND_CLEAR:    return "CLEAR";
    case PA_BAND_NOTED:    return "NOTED";
    case PA_BAND_ELEVATED: return "ELEVATED";
    case PA_BAND_INCIDENT: return "INCIDENT";
    default:               return "?";
    }
}

const char *pa_band_advice(pa_band_t b)
{
    switch (b) {
    case PA_BAND_CLEAR:
        return "No lens has raised anything. This receiver hears one channel "
               "at a time and only reports what it was pointed at, so this is "
               "the absence of a finding rather than a clean bill of health.";
    case PA_BAND_NOTED:
        return "There is background activity - probing, or small changes. "
               "Normal for a live building. Worth a skim, not a response.";
    case PA_BAND_ELEVATED:
        return "One finding is real enough to look at. Open the lens named "
               "below and read its own verdict, which carries the detail and "
               "the reasoning this summary does not.";
    case PA_BAND_INCIDENT:
        return "More than one independent stage has fired. Capture evidence "
               "now - write a report while the detail is still in memory - and "
               "escalate to whoever owns this network. Note the times: the "
               "peaks below are latched and each says how long ago it was.";
    default:
        return "";
    }
}
