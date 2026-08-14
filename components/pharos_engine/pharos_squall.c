#include "pharos_squall.h"

#include <string.h>

static uint32_t clamp_u32(uint32_t v, uint32_t lo, uint32_t hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

void pq_reset(pq_state_table_t *t)
{
    if (t) {
        memset(t, 0, sizeof(*t));
    }
}

void pq_observe(pq_state_table_t *t, const pq_dwell_t *d)
{
    if (!t || !d || d->channel == 0 || d->channel > PQ_MAX_CHANNELS) {
        return;
    }
    pq_channel_t *c = &t->ch[d->channel];
    if (!c->in_use) {
        c->in_use = true;
        c->channel = d->channel;
        t->n_seen++;
    }
    c->visits++;
    c->frames_total += d->frames;
    c->retries_total += d->retries;
    c->dwell_ms_total += d->dwell_ms;
    c->busy_sum += d->busy_permil;
    if (d->noise_floor != 0) {
        c->floor_sum += d->noise_floor;
        c->floor_n++;
    }
}

uint8_t pq_ceiling(const pq_context_t *ctx)
{
    if (!ctx) {
        return 55;
    }
    const uint32_t dwell = clamp_u32(ctx->dwell_permil, 1, 1000);

    /* Deliberately the harshest ceiling curve in Pharos. Every other engine
     * reasons about frames that ARRIVED, and can extrapolate a rate from a
     * sample. This one reasons about frames that did NOT arrive - and "no
     * frames on channel 11" is indistinguishable from "we were not listening
     * to channel 11". A hopping receiver therefore cannot honestly claim much
     * about denial at all. */
    uint32_t c = 45u + (45u * dwell) / 1000u;
    if (!ctx->camped) {
        c = c > 12u ? c - 12u : 1u;
    }
    return (uint8_t)clamp_u32(c, 30u, 92u);
}

/* Classify one channel from its accumulated visits. */
static void classify(pq_channel_t *c)
{
    if (c->visits < PQ_MIN_SAMPLES) {
        c->state = PQ_STATE_UNKNOWN;
        c->severity = 0;
        return;
    }

    const uint32_t busy = c->busy_sum / c->visits;                 /* per mille */
    const uint32_t secs = c->dwell_ms_total ? c->dwell_ms_total : 1;
    /* Frames per second of actual listening. */
    const uint32_t fps = (c->frames_total * 1000u) / secs;
    const uint32_t retry_permil =
        c->frames_total ? (c->retries_total * 1000u) / c->frames_total : 0;

    /* "Loud" is judged from the airtime the radio reported busy, which is the
     * measure that does not depend on anything decoding. */
    const bool loud = busy >= 400;
    const bool very_loud = busy >= 650;
    const bool productive = fps >= 20;

    /* "Barren" is anchored to something physical rather than picked: a single
     * access point beacons roughly ten times a second. Hearing fewer than ten
     * frames a second therefore means we cannot decode even ONE beaconing AP -
     * which, on a channel the radio reports as busy, is precisely the
     * condition worth naming. */
    const bool barren = fps < 10;

    if (very_loud && barren) {
        /* Power in the band that will not resolve into frames. */
        c->state = PQ_STATE_DENIAL;
        c->severity = (uint8_t)clamp_u32(70u + (busy - 650u) / 12u, 70, 95);
    } else if (loud && productive) {
        c->state = PQ_STATE_CONGESTED;
        c->severity = (uint8_t)clamp_u32(25u + (busy - 400u) / 20u, 25, 45);
    } else if (loud && barren) {
        c->state = PQ_STATE_DENIAL;
        c->severity = (uint8_t)clamp_u32(55u + (busy - 400u) / 10u, 55, 75);
    } else if (retry_permil >= 400 && !barren) {
        /* Usable but fighting for airtime. */
        c->state = PQ_STATE_DEGRADED;
        c->severity = (uint8_t)clamp_u32(30u + (retry_permil - 400u) / 20u, 30, 55);
    } else if (barren && !loud) {
        c->state = PQ_STATE_QUIET;
        c->severity = 0;
    } else {
        c->state = PQ_STATE_HEALTHY;
        c->severity = 0;
    }
}

void pq_evaluate(const pq_state_table_t *t, const pq_context_t *ctx, pq_verdict_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->headline = "";
    out->worst = PQ_STATE_UNKNOWN;
    if (!t || !ctx) {
        return;
    }
    out->ceiling = pq_ceiling(ctx);
    if (ctx->dwell_permil < 500) {
        out->notes |= PQ_NOTE_THIN;
    }

    /* Work on a local copy so the table itself stays const to the caller. */
    pq_channel_t local[PQ_MAX_CHANNELS + 1];
    memcpy(local, t->ch, sizeof(local));

    uint32_t frames = 0, retries = 0;
    uint8_t worst_sev = 0;
    bool any_floor = false;

    for (unsigned i = 1; i <= PQ_MAX_CHANNELS; i++) {
        pq_channel_t *c = &local[i];
        if (!c->in_use) {
            continue;
        }
        if (c->floor_n) {
            any_floor = true;
        }
        classify(c);
        if (c->state == PQ_STATE_UNKNOWN) {
            continue;
        }
        out->n_graded++;
        frames += c->frames_total;
        retries += c->retries_total;
        if (c->state == PQ_STATE_DENIAL) {
            out->n_denial++;
        }
        if (c->state == PQ_STATE_CONGESTED) {
            out->n_congested++;
        }
        if (c->severity > worst_sev) {
            worst_sev = c->severity;
            out->worst = c->state;
            out->worst_channel = c->channel;
        }
    }

    if (out->n_graded == 0) {
        out->notes |= PQ_NOTE_FEW;
        out->headline = "Not enough dwells yet to judge the air";
        return;
    }
    if (!any_floor) {
        out->notes |= PQ_NOTE_NOFLOOR;
    }
    out->retry_permil = (uint16_t)(frames ? (retries * 1000u) / frames : 0);

    /* If nothing was graded worse than healthy, the worst state may still be
     * QUIET/HEALTHY - report it rather than leaving it UNKNOWN. */
    if (out->worst == PQ_STATE_UNKNOWN) {
        for (unsigned i = 1; i <= PQ_MAX_CHANNELS; i++) {
            if (local[i].in_use && local[i].state != PQ_STATE_UNKNOWN) {
                out->worst = local[i].state;
                out->worst_channel = local[i].channel;
                break;
            }
        }
    }

    uint32_t score = worst_sev;

    /* --- families ------------------------------------------------------ */
    if (out->n_denial) {
        out->families |= PQ_FAM_ENERGY;
    }
    if (out->retry_permil >= 350) {
        out->families |= PQ_FAM_RETRIES;
        score += clamp_u32((out->retry_permil - 350u) / 25u, 0, 14);
    }
    if (out->n_denial >= 3) {
        /* A jammer usually covers a band, not one channel. A single bad
         * channel is far more likely to be one noisy device. */
        out->families |= PQ_FAM_SPREAD;
        score += 10;
    } else if (out->n_denial == 1) {
        out->notes |= PQ_NOTE_NARROW;
    }

    /* --- honesty caps --------------------------------------------------- */

    /* THE cap. Energy alone is a microwave oven, a video sender, a neighbour's
     * outdoor bridge. DENIAL is only claimable when the senders are visibly
     * suffering too, or when it spans the band. One family is a strong hint
     * and is reported as DEGRADED-grade suspicion, never as denial. */
    const bool energy_only = (out->families == PQ_FAM_ENERGY);
    if (energy_only && score > 62) {
        score = 62;
    }
    if (out->families == 0 && score > 45) {
        score = 45;
    }

    /* A single suspicious channel could be one piece of equipment. Real denial
     * is rarely that tidy. */
    if ((out->notes & PQ_NOTE_NARROW) && score > 74) {
        score = 74;
    }

    /* Hopping cannot tell "no frames" from "not listening". */
    if ((out->notes & PQ_NOTE_THIN) && score > 66) {
        score = 66;
    }

    score = clamp_u32(score, 0, 100);
    out->raw_score = (uint8_t)score;
    if (score > out->ceiling) {
        score = out->ceiling;
    }
    out->score = (uint8_t)score;

    /* Downgrade the reported state if the caps stopped us short of claiming
     * denial - the word and the number must never disagree. */
    if (out->worst == PQ_STATE_DENIAL && out->score < 70) {
        out->worst = PQ_STATE_DEGRADED;
    }

    switch (out->worst) {
    case PQ_STATE_DENIAL:
        out->headline = "The band is loud and nothing is decoding - this is what a jam looks like";
        break;
    case PQ_STATE_DEGRADED:
        out->headline = energy_only
                            ? "Energy here that will not decode - could be a device, not an attack"
                            : "Frames are suffering; the cause is not established";
        break;
    case PQ_STATE_CONGESTED:
        out->headline = "Busy, and working: a loud building rather than a problem";
        break;
    case PQ_STATE_QUIET:
        out->headline = "Very little here - quiet air, or a radio hearing nothing";
        break;
    case PQ_STATE_HEALTHY:
    default:
        out->headline = "Energy and traffic in proportion on what was visited";
        break;
    }
}

const char *pq_state_name(pq_state_t s)
{
    switch (s) {
    case PQ_STATE_UNKNOWN:   return "UNKNOWN";
    case PQ_STATE_QUIET:     return "QUIET";
    case PQ_STATE_HEALTHY:   return "HEALTHY";
    case PQ_STATE_CONGESTED: return "CONGESTED";
    case PQ_STATE_DEGRADED:  return "DEGRADED";
    case PQ_STATE_DENIAL:    return "DENIAL";
    default:                 return "?";
    }
}

const char *pq_state_advice(pq_state_t s)
{
    switch (s) {
    case PQ_STATE_UNKNOWN:
        return "Not enough visits to this channel to say anything. Camp on it "
               "if you want an answer about it specifically.";
    case PQ_STATE_QUIET:
        return "Almost nothing is arriving and the air is not loud. That is an "
               "empty channel - or a receiver that is not hearing. Check the "
               "other channels read differently before trusting it.";
    case PQ_STATE_HEALTHY:
        return "Energy and decoded traffic are in proportion. Normal air, on "
               "the channels actually visited in the last few seconds.";
    case PQ_STATE_CONGESTED:
        return "Loud AND productive - a busy building, not an attack. Users "
               "will feel this as slowness. The fix is capacity and channel "
               "planning, not a security response.";
    case PQ_STATE_DEGRADED:
        return "Traffic is suffering here. Common innocent causes first: a "
               "microwave oven, a video sender, a neighbour's outdoor bridge, "
               "a failing radio. Camp on the channel to raise confidence, and "
               "use Locate to walk toward whatever is loudest.";
    case PQ_STATE_DENIAL:
        return "The band is full of power that will not resolve into frames, "
               "and the senders are suffering with it. That is the shape of "
               "jamming. Treat it as a physical problem: it has a location and "
               "somebody has to walk to it. Capture a report now - this is a "
               "condition that stops the moment the source is switched off.";
    default:
        return "";
    }
}
