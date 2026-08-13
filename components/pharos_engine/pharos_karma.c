#include "pharos_karma.h"

#include <string.h>

static uint32_t clamp_u32(uint32_t v, uint32_t lo, uint32_t hi)
{
    return v < lo ? lo : (v > hi ? hi : v);
}

static bool name_eq(const pk_ssid_t *s, const char *ssid, uint8_t len)
{
    return s->len == len && memcmp(s->name, ssid, len) == 0;
}

static bool mac_eq(const uint8_t *a, const uint8_t *b)
{
    return memcmp(a, b, 6) == 0;
}

static bool mac_is_zero(const uint8_t *a)
{
    for (int i = 0; i < 6; i++) {
        if (a[i]) return false;
    }
    return true;
}

void pk_reset(pk_engine_t *e)
{
    if (e) {
        memset(e, 0, sizeof(*e));
    }
}

static pk_responder_t *responder_admit(pk_engine_t *e, const uint8_t bssid[6])
{
    for (unsigned i = 0; i < e->n_responders; i++) {
        if (e->responders[i].in_use && mac_eq(e->responders[i].bssid, bssid)) {
            return &e->responders[i];
        }
    }
    if (e->n_responders >= PK_MAX_RESPONDERS) {
        return NULL;
    }
    pk_responder_t *r = &e->responders[e->n_responders++];
    memset(r, 0, sizeof(*r));
    memcpy(r->bssid, bssid, 6);
    r->in_use = true;
    return r;
}

static pk_ssid_t *ssid_slot(pk_responder_t *r, const char *ssid, uint8_t len)
{
    if (len == 0 || len > PK_SSID_MAX) {
        return NULL;
    }
    for (unsigned i = 0; i < r->n_ssids; i++) {
        if (name_eq(&r->ssids[i], ssid, len)) {
            return &r->ssids[i];
        }
    }
    if (r->n_ssids >= PK_MAX_SSIDS) {
        /* Out of slots. A KARMA responder will blow past this quickly, which
         * is itself informative, so record the overflow rather than dropping
         * the fact on the floor. */
        r->overflow = true;
        return NULL;
    }
    pk_ssid_t *s = &r->ssids[r->n_ssids++];
    memset(s, 0, sizeof(*s));
    memcpy(s->name, ssid, len);
    s->name[len] = '\0';
    s->len = len;
    return s;
}

void pk_observe_probe(pk_engine_t *e, const char *ssid, uint8_t len, uint64_t t_us)
{
    if (!e) {
        return;
    }
    e->probes_seen++;
    if (!ssid || len == 0 || len > PK_SSID_MAX) {
        return; /* wildcard probe: names nothing, so nothing to echo */
    }
    pk_recent_t *slot = &e->recent[e->recent_head % PK_MAX_RECENT];
    memcpy(slot->name, ssid, len);
    slot->name[len] = '\0';
    slot->len = len;
    slot->t_us = t_us;
    e->recent_head++;
}

/* Was this exact name asked for shortly before t_us? */
static bool recently_probed(const pk_engine_t *e, const char *ssid, uint8_t len,
                            uint64_t t_us)
{
    for (unsigned i = 0; i < PK_MAX_RECENT; i++) {
        const pk_recent_t *r = &e->recent[i];
        if (r->len != len || r->len == 0) {
            continue;
        }
        if (memcmp(r->name, ssid, len) != 0) {
            continue;
        }
        if (t_us >= r->t_us && (t_us - r->t_us) <= PK_ECHO_WINDOW_US) {
            return true;
        }
    }
    return false;
}

void pk_observe_beacon(pk_engine_t *e, const uint8_t bssid[6], const char *ssid,
                       uint8_t len, int8_t rssi, uint8_t channel, uint64_t t_us)
{
    (void)t_us;
    if (!e || !bssid || mac_is_zero(bssid)) {
        return;
    }
    pk_responder_t *r = responder_admit(e, bssid);
    if (!r) {
        return;
    }
    r->beacons++;
    r->rssi = rssi;
    r->channel = channel;
    pk_ssid_t *s = ssid_slot(r, ssid, len);
    if (s) {
        /* Announced unprompted. This is the thing a KARMA responder cannot
         * do for a name it has not been asked about. */
        s->beaconed = true;
    }
}

void pk_observe_response(pk_engine_t *e, const uint8_t bssid[6], const char *ssid,
                         uint8_t len, int8_t rssi, uint8_t channel, uint64_t t_us)
{
    if (!e || !bssid || mac_is_zero(bssid)) {
        return;
    }
    e->responses_seen++;
    pk_responder_t *r = responder_admit(e, bssid);
    if (!r) {
        return;
    }
    r->responses++;
    r->rssi = rssi;
    r->channel = channel;
    pk_ssid_t *s = ssid_slot(r, ssid, len);
    if (!s) {
        return;
    }
    s->answered = true;
    if (recently_probed(e, ssid, len, t_us)) {
        s->echoed = true;
    }
}

uint8_t pk_ceiling(const pk_context_t *ctx)
{
    const uint32_t dwell = clamp_u32(ctx ? ctx->dwell_permil : 1000, 1, 1000);
    const uint32_t yield = clamp_u32(ctx ? ctx->bus_yield_permil : 1000, 1, 1000);
    /* Same shape as the other engines: hopping cannot reach the alarm band,
     * because "it never beaconed that name" is exactly the claim a hopping
     * receiver is least entitled to make. */
    uint32_t c = 56u + (38u * dwell) / 1000u;
    uint32_t pen = (yield >= 900u) ? 0u : (900u - yield) / 40u;
    if (pen > 15u) pen = 15u;
    c = (c > pen) ? c - pen : 0u;
    return (uint8_t)clamp_u32(c, 45u, 94u);
}

static pk_band_t band_of(uint8_t score)
{
    if (score >= 70) return PK_BAND_KARMA_LIKELY;
    if (score >= 45) return PK_BAND_SUSPICIOUS;
    if (score >= 20) return PK_BAND_MIXED;
    return PK_BAND_NORMAL;
}

void pk_evaluate(const pk_engine_t *e, const pk_context_t *ctx, pk_verdict_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->headline = "";
    if (!e || !ctx) {
        return;
    }

    out->ceiling = pk_ceiling(ctx);
    const uint32_t dwell = clamp_u32(ctx->dwell_permil, 1, 1000);
    if (dwell < 500) {
        out->notes |= PK_NOTE_THIN_DWELL;
    }
    if (e->probes_seen == 0) {
        out->notes |= PK_NOTE_NO_PROBES;
    }

    /* Grade each responder; keep the worst. */
    uint32_t best_score = 0;
    for (unsigned i = 0; i < e->n_responders; i++) {
        const pk_responder_t *r = &e->responders[i];
        if (!r->in_use) {
            continue;
        }

        unsigned answered = 0, unannounced = 0, echoed = 0;
        for (unsigned k = 0; k < r->n_ssids; k++) {
            const pk_ssid_t *s = &r->ssids[k];
            if (!s->answered) {
                continue;
            }
            answered++;
            if (!s->beaconed) {
                unannounced++;
                if (s->echoed) {
                    echoed++;
                }
            }
        }
        if (answered == 0) {
            continue;
        }

        /* --- BREADTH: how many names does one radio answer for? --------
         * Counted over the *unannounced* ones only. A corporate AP carrying
         * four announced SSIDs scores nothing here, which is the point. */
        uint32_t breadth = 0;
        if (unannounced >= 3) {
            breadth = 16 + (unannounced - 3) * 6;
        } else if (unannounced == 2) {
            breadth = 8;
        }
        if (r->overflow) {
            breadth += 8; /* it ran us out of slots, which is itself a signal */
        }
        breadth = clamp_u32(breadth, 0, 40);

        /* --- ABSENCE: the defining trait, scaled by how much we heard ----
         * The claim "it never announced this name" is only as strong as our
         * chance of having heard the announcement. Camped, that is a real
         * observation; hopping, it is mostly an admission of not listening. */
        uint32_t absence = 0;
        if (unannounced > 0) {
            const uint32_t share = (unannounced * 1000u) / answered;
            absence = (share * 30u) / 1000u;              /* 0..30 by ratio  */
            absence = (absence * dwell) / 1000u;          /* scaled by dwell */
        }
        absence = clamp_u32(absence, 0, 30);

        /* --- ECHO: answers that arrive right after somebody asked -------- */
        uint32_t echo = 0;
        if (echoed >= 1) {
            echo = 12 + (echoed - 1) * 9;
        }
        echo = clamp_u32(echo, 0, 30);

        uint32_t raw = breadth + absence + echo;
        raw = clamp_u32(raw, 0, 100);
        uint32_t score = raw;

        uint8_t families = 0;
        if (breadth >= 12) families |= PK_FAM_BREADTH;
        if (absence >= 10) families |= PK_FAM_ABSENCE;
        if (echo >= 12) families |= PK_FAM_ECHO;

        unsigned family_count = 0;
        for (unsigned b = 0; b < 3; b++) {
            if (families & (1u << b)) family_count++;
        }

        /* An alarm requires the ABSENCE family specifically. Answering for
         * several networks is what a multi-SSID access point does all day;
         * it only becomes impersonation when the radio will not say those
         * names unless asked. */
        if (!(families & PK_FAM_ABSENCE) && score > 44) {
            score = 44;
        }
        if (family_count < 2 && score > 44) {
            score = 44;
        }
        if (score > out->ceiling) {
            score = out->ceiling;
        }

        if (score >= best_score) {
            best_score = score;
            out->score = (uint8_t)score;
            out->raw_score = (uint8_t)raw;
            out->families = families;
            out->c_breadth = (uint8_t)breadth;
            out->c_absence = (uint8_t)absence;
            out->c_echo = (uint8_t)echo;
            out->answered_ssids = (uint8_t)answered;
            out->unannounced = (uint8_t)unannounced;
            out->echoed = (uint8_t)echoed;
            memcpy(out->suspect, r->bssid, 6);
            if (r->overflow) {
                out->notes |= PK_NOTE_TABLE_FULL;
            }
            if (r->bssid[0] & 0x02) {
                out->notes |= PK_NOTE_LOCAL_MAC;
            }
            if (unannounced == 0 && answered >= 2) {
                out->notes |= PK_NOTE_MULTI_SSID;
            }
        }
    }

    out->band = band_of(out->score);

    switch (out->band) {
    case PK_BAND_KARMA_LIKELY:
        out->headline = "One radio answers to names it never announces";
        break;
    case PK_BAND_SUSPICIOUS:
        out->headline = "A radio is answering for networks it does not advertise";
        break;
    case PK_BAND_MIXED:
        out->headline = (out->notes & PK_NOTE_MULTI_SSID)
                            ? "Several networks here, all properly announced"
                            : "Nothing yet that looks like impersonation";
        break;
    case PK_BAND_NORMAL:
    default:
        out->headline = (out->notes & PK_NOTE_NO_PROBES)
                            ? "No probe requests heard yet - nothing to answer"
                            : "Access points answer only for what they announce";
        break;
    }
}

const char *pk_band_name(pk_band_t band)
{
    switch (band) {
    case PK_BAND_NORMAL:       return "NORMAL";
    case PK_BAND_MIXED:        return "MIXED";
    case PK_BAND_SUSPICIOUS:   return "SUSPICIOUS";
    case PK_BAND_KARMA_LIKELY: return "KARMA LIKELY";
    default:                   return "?";
    }
}

const char *pk_band_advice(pk_band_t band)
{
    switch (band) {
    case PK_BAND_NORMAL:
        return "Radios here answer only for networks they also announce. That "
               "is what an honest access point looks like.";
    case PK_BAND_MIXED:
        return "One radio carries several networks and announces all of them - "
               "normal for a corporate or guest deployment.";
    case PK_BAND_SUSPICIOUS:
        return "A radio answered for a network it has never announced. Camp on "
               "this channel to be sure the announcement was not simply missed.";
    case PK_BAND_KARMA_LIKELY:
        return "A radio is agreeing to be whatever passing devices ask for. Do "
               "not let clients associate; preserve the log and locate it.";
    default:
        return "";
    }
}
