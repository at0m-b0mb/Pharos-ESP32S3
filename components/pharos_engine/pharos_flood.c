#include "pharos_flood.h"

#include <string.h>

static uint32_t clamp_u32(uint32_t v, uint32_t lo, uint32_t hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static uint32_t interp(uint32_t x, const uint32_t *xs, const uint32_t *ys, unsigned n)
{
    if (x <= xs[0]) return ys[0];
    for (unsigned i = 1; i < n; i++) {
        if (x <= xs[i]) {
            const uint32_t span = xs[i] - xs[i - 1];
            const uint32_t rise = ys[i] - ys[i - 1];
            return ys[i - 1] + (span ? ((x - xs[i - 1]) * rise) / span : 0);
        }
    }
    return ys[n - 1];
}

void pf_reset(pf_engine_t *e)
{
    if (e) {
        memset(e, 0, sizeof(*e));
    }
}

static pf_oui_t *oui_admit(pf_engine_t *e, const uint8_t bssid[6])
{
    for (unsigned i = 0; i < e->n_ouis; i++) {
        if (e->ouis[i].in_use && memcmp(e->ouis[i].oui, bssid, 3) == 0) {
            return &e->ouis[i];
        }
    }
    if (e->n_ouis >= PF_MAX_OUI) {
        return NULL;
    }
    pf_oui_t *o = &e->ouis[e->n_ouis++];
    memset(o, 0, sizeof(*o));
    memcpy(o->oui, bssid, 3);
    o->local = (bssid[0] & 0x02) != 0;
    o->in_use = true;
    return o;
}

static pf_ssid_t *ssid_find(pf_engine_t *e, const char *ssid, uint8_t len)
{
    for (unsigned i = 0; i < e->n_ssids; i++) {
        if (e->ssids[i].in_use && e->ssids[i].len == len &&
            memcmp(e->ssids[i].name, ssid, len) == 0) {
            return &e->ssids[i];
        }
    }
    return NULL;
}

void pf_observe(pf_engine_t *e, const uint8_t bssid[6], const char *ssid,
                uint8_t len, uint64_t t_us)
{
    if (!e || !bssid || !ssid || len == 0 || len > PF_SSID_MAX) {
        return; /* a hidden/empty SSID tells us nothing about a name flood */
    }
    if (e->total_beacons == 0) {
        e->first_us = t_us;
    }
    e->total_beacons++;
    e->last_us = t_us;

    pf_ssid_t *s = ssid_find(e, ssid, len);
    if (s) {
        s->beacons++;
        s->last_us = t_us;
        return;
    }

    /* A new name. Its arrival rate is the whole VOLUME family, so novelty is
     * counted even when the table is full - we just cannot keep the detail. */
    e->distinct_created++;

    pf_oui_t *o = oui_admit(e, bssid);
    if (o) {
        o->names++;
    }

    if (e->n_ssids >= PF_MAX_SSIDS) {
        /* Evict the least recently heard. A flood fills this instantly, which
         * is itself a signal, so it is recorded rather than hidden. */
        e->overflow = true;
        pf_ssid_t *oldest = &e->ssids[0];
        for (unsigned i = 1; i < PF_MAX_SSIDS; i++) {
            if (e->ssids[i].last_us < oldest->last_us) {
                oldest = &e->ssids[i];
            }
        }
        s = oldest;
        e->evictions++;
    } else {
        s = &e->ssids[e->n_ssids++];
    }
    memset(s, 0, sizeof(*s));
    memcpy(s->name, ssid, len);
    s->name[len] = '\0';
    s->len = len;
    memcpy(s->bssid, bssid, 6);
    s->beacons = 1;
    s->first_us = t_us;
    s->last_us = t_us;
    s->in_use = true;
}

uint8_t pf_ceiling(const pf_context_t *ctx)
{
    const uint32_t dwell = clamp_u32(ctx ? ctx->dwell_permil : 1000, 1, 1000);
    const uint32_t yield = clamp_u32(ctx ? ctx->bus_yield_permil : 1000, 1, 1000);
    uint32_t c = 56u + (38u * dwell) / 1000u;
    uint32_t pen = (yield >= 900u) ? 0u : (900u - yield) / 40u;
    if (pen > 15u) pen = 15u;
    c = (c > pen) ? c - pen : 0u;
    return (uint8_t)clamp_u32(c, 45u, 95u);
}

static pf_band_t band_of(uint8_t score)
{
    if (score >= 70) return PF_BAND_FLOOD_LIKELY;
    if (score >= 45) return PF_BAND_SUSPICIOUS;
    if (score >= 20) return PF_BAND_BUSY;
    return PF_BAND_QUIET;
}

void pf_evaluate(const pf_engine_t *e, const pf_context_t *ctx, pf_verdict_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->headline = "";
    if (!e || !ctx) {
        return;
    }
    out->ceiling = pf_ceiling(ctx);

    const uint32_t dwell = clamp_u32(ctx->dwell_permil, 1, 1000);
    const uint32_t yield = clamp_u32(ctx->bus_yield_permil, 1, 1000);
    if (dwell < 500) out->notes |= PF_NOTE_THIN_DWELL;
    if (e->overflow) out->notes |= PF_NOTE_TABLE_FULL;

    out->distinct_ssids = (uint16_t)(e->n_ssids > 0xFFFF ? 0xFFFF : e->n_ssids);

    if (e->distinct_created == 0) {
        out->band = PF_BAND_QUIET;
        out->headline = "No network names in view";
        return;
    }

    /* --- VOLUME: how fast do brand-new names appear? --------------------
     * Corrected for dwell and yield so a hopping receiver's thin sample is
     * extrapolated to a true rate, then held honest by the ceiling. */
    uint32_t elapsed_ms = ctx->window_ms ? ctx->window_ms : 10000u;
    if (e->last_us > e->first_us) {
        const uint64_t span = (e->last_us - e->first_us) / 1000ull;
        if (span > 0 && span < elapsed_ms) {
            elapsed_ms = (uint32_t)span;
        }
    }
    /* A RATE NEEDS A REAL DENOMINATOR.
     *
     * Narrowing the time base to the span between the first and last name
     * heard is right for catching a burst inside a long window, and wrong the
     * moment the sample is thin: three names that happened to arrive within
     * 800 ms became "227.2 new names a minute" on a quiet street, which is a
     * number built almost entirely out of division. The same shape of bug put
     * "33600 frames a second" on the Watch face.
     *
     * So the span may sharpen the reading, never invent one. Below the floor
     * the window is what gets used, and the verdict says the window was
     * short. */
    if (elapsed_ms < PF_MIN_WINDOW_MS) {
        elapsed_ms = (ctx->window_ms > PF_MIN_WINDOW_MS) ? ctx->window_ms
                                                         : PF_MIN_WINDOW_MS;
        out->notes |= PF_NOTE_SHORT;
    }

    /* And the duty correction has the same failure. dwell_permil is clamped
     * to 1 above, so a receiver reporting a near-zero dwell would multiply
     * everything it saw by a thousand. This one hops thirteen channels, so
     * roughly 77 permil is the floor an honest measurement can reach, and
     * anything under it is a measurement fault rather than a thin sample. */
    const uint32_t duty = (dwell < PF_MIN_DWELL_PERMIL) ? PF_MIN_DWELL_PERMIL
                                                        : dwell;
    if (dwell < PF_MIN_DWELL_PERMIL) {
        out->notes |= PF_NOTE_SHORT;
    }

    /* new names per minute, x10, duty-corrected */
    {
        const uint64_t num = (uint64_t)e->distinct_created * 600ull * 1000ull * 1000ull;
        const uint64_t den = (uint64_t)elapsed_ms * (uint64_t)duty * (uint64_t)yield / 1000ull;
        uint32_t rate = den ? (uint32_t)(num / den) : 0u;

        /* Last guard, on the extrapolation itself. Multiplying four names by
         * thirteen is arithmetic, not evidence; until enough distinct names
         * have actually been created the reading is held to what a
         * hop-corrected reading of THIS sample can support. */
        if (e->distinct_created < PF_MIN_NAMES_TO_EXTRAPOLATE) {
            const uint32_t plain =
                (uint32_t)(((uint64_t)e->distinct_created * 600ull * 1000ull) /
                           (uint64_t)elapsed_ms);
            const uint32_t bound = plain * PF_THIN_SAMPLE_FACTOR;
            if (rate > bound) {
                rate = bound;
            }
            out->notes |= PF_NOTE_SHORT;
        }
        out->new_per_min_x10 = (uint16_t)clamp_u32(rate, 0, 0xFFFF);
    }
    static const uint32_t vx[] = { 30, 120, 300, 900, 3000, 12000 };
    static const uint32_t vy[] = { 0, 8, 16, 26, 34, 40 };
    out->c_volume = (uint8_t)interp(out->new_per_min_x10, vx, vy, 6);

    /* --- EPHEMERAL: share of names heard once or twice ------------------ */
    unsigned ephemeral = 0, considered = 0;
    for (unsigned i = 0; i < e->n_ssids; i++) {
        if (!e->ssids[i].in_use) continue;
        considered++;
        if (e->ssids[i].beacons <= 2) ephemeral++;
    }
    out->ephemeral_permil = considered ? (uint16_t)((ephemeral * 1000u) / considered) : 0;
    /* Only meaningful once we have seen a few names; two ephemeral names out
     * of two is noise, not a flood. */
    if (considered >= 8) {
        static const uint32_t ex[] = { 300, 500, 700, 850, 1000 };
        static const uint32_t ey[] = { 0, 6, 16, 24, 30 };
        out->c_ephemeral = (uint8_t)interp(out->ephemeral_permil, ex, ey, 5);
    }

    /* --- SYNTHETIC: software BSSIDs and prefix reuse -------------------- */
    unsigned local_names = 0;
    uint8_t widest = 0;
    for (unsigned i = 0; i < e->n_ouis; i++) {
        if (!e->ouis[i].in_use) continue;
        if (e->ouis[i].local) local_names += e->ouis[i].names;
        if (e->ouis[i].names > widest) widest = (uint8_t)clamp_u32(e->ouis[i].names, 0, 255);
    }
    out->widest_oui_names = widest;
    out->synthetic_permil = e->distinct_created
                                ? (uint16_t)((local_names * 1000u) / e->distinct_created)
                                : 0;
    {
        uint32_t synth = 0;
        static const uint32_t sx[] = { 200, 400, 700, 1000 };
        static const uint32_t sy[] = { 0, 8, 18, 24 };
        synth = interp(out->synthetic_permil, sx, sy, 4);
        /* Many distinct names under one prefix is the signature of a single
         * flooding tool cycling its list. */
        if (widest >= 8) synth += 6;
        out->c_synthetic = (uint8_t)clamp_u32(synth, 0, 30);
    }

    /* --- families ------------------------------------------------------- */
    if (out->c_volume >= 12) out->families |= PF_FAM_VOLUME;
    if (out->c_ephemeral >= 10) out->families |= PF_FAM_EPHEMERAL;
    if (out->c_synthetic >= 10) out->families |= PF_FAM_SYNTHETIC;

    uint32_t raw = (uint32_t)out->c_volume + out->c_ephemeral + out->c_synthetic;
    raw = clamp_u32(raw, 0, 100);
    out->raw_score = (uint8_t)raw;
    uint32_t score = raw;

    unsigned family_count = 0;
    for (unsigned b = 0; b < 3; b++) {
        if (out->families & (1u << b)) family_count++;
    }

    /* The urban guard: a lot of persistent, real-vendor networks is a city,
     * not an attack. VOLUME on its own cannot pass BUSY. */
    if (family_count < 2) {
        if (out->families == PF_FAM_VOLUME) {
            out->notes |= PF_NOTE_URBAN;
        }
        if (score > 44) score = 44;
    }
    if ((out->notes & PF_NOTE_SHORT) && score > 49) {
        score = 49; /* a burst in half a second is not a rate */
    }
    if (score > out->ceiling) {
        score = out->ceiling;
    }

    out->score = (uint8_t)score;
    out->band = band_of(out->score);

    switch (out->band) {
    case PF_BAND_FLOOD_LIKELY:
        out->headline = "Fabricated network names appearing faster than any real place";
        break;
    case PF_BAND_SUSPICIOUS:
        out->headline = "Network names are churning - camp to confirm they never return";
        break;
    case PF_BAND_BUSY:
        out->headline = (out->notes & PF_NOTE_URBAN)
                            ? "Many networks, but they persist - a busy place, not a flood"
                            : "More names than usual, nothing yet that says attack";
        break;
    case PF_BAND_QUIET:
    default:
        out->headline = "Network names appear at an ordinary pace";
        break;
    }
}

const char *pf_band_name(pf_band_t band)
{
    switch (band) {
    case PF_BAND_QUIET:        return "QUIET";
    case PF_BAND_BUSY:         return "BUSY AIRSPACE";
    case PF_BAND_SUSPICIOUS:   return "SUSPICIOUS";
    case PF_BAND_FLOOD_LIKELY: return "FLOOD LIKELY";
    default:                   return "?";
    }
}

const char *pf_band_advice(pf_band_t band)
{
    switch (band) {
    case PF_BAND_QUIET:
        return "New network names appear at the pace a real environment sets. "
               "Nothing here is spamming the air.";
    case PF_BAND_BUSY:
        return "A lot of networks, but they persist and come from real vendors. "
               "Density is not an attack.";
    case PF_BAND_SUSPICIOUS:
        return "Names are appearing and vanishing. Camp on this channel to be "
               "sure they never beacon again - that is what a flood does.";
    case PF_BAND_FLOOD_LIKELY:
        return "One radio is inventing network names faster than any place "
               "gains them, and they do not persist. A beacon flood. Locate it.";
    default:
        return "";
    }
}
