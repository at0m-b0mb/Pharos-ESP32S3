#include "pharos_range.h"

#include <string.h>

/* Deterministic PRNG: xorshift32. Reproducibility is the point - a scenario
 * must play identically from the same seed so a lesson can be paused,
 * rewound, and asserted in a test. */
static uint32_t xs(pr_range_t *r)
{
    uint32_t x = r->rng ? r->rng : 0x1234567u;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    r->rng = x;
    return x;
}

static int8_t jitter(pr_range_t *r, int8_t base, int8_t spread)
{
    if (spread <= 0) {
        return base;
    }
    const int delta = (int)(xs(r) % (uint32_t)(2 * spread + 1)) - spread;
    int v = base + delta;
    if (v > 0) v = 0;
    if (v < -110) v = -110;
    return (int8_t)v;
}

static void set_mac(uint8_t m[6], uint8_t a, uint8_t b, uint8_t c, uint8_t d,
                    uint8_t e, uint8_t f)
{
    m[0] = a; m[1] = b; m[2] = c; m[3] = d; m[4] = e; m[5] = f;
}

void pr_range_init(pr_range_t *r, const pr_config_t *cfg)
{
    if (!r || !cfg) {
        return;
    }
    memset(r, 0, sizeof(*r));
    r->cfg = *cfg;
    r->rng = cfg->seed ? cfg->seed : 0xC0FFEEu;
    r->t_us = 1000000ull; /* start at 1 s, clear of any bucket epoch */

    /* A believable estate: one real AP, a twin on a software address, a
     * victim client, an attacker, and a chatty phone. */
    set_mac(r->ap_bssid,   0xAC, 0x22, 0x0B, 0x10, 0x20, 0x30);
    set_mac(r->twin_bssid, 0x02, 0xDE, 0xAD, 0xBE, 0xEF, 0x01); /* locally admin */
    set_mac(r->victim,     0x3C, 0x71, 0xBF, 0xAA, 0xBB, 0xCC);
    set_mac(r->attacker,   0x02, 0x66, 0x6E, 0x00, 0x00, 0x99);
    set_mac(r->phone,      0x9A, 0x11, 0x22, 0x33, 0x44, 0x55); /* randomised */
}

/* Fill a management-frame event. Helper keeps the scenario code readable. */
static void mk_mgmt(pharos_event_t *ev, uint64_t t_us, uint8_t subtype,
                    const uint8_t *a1, const uint8_t *a2, int8_t rssi,
                    uint16_t reason, uint8_t flags)
{
    memset(ev, 0, sizeof(*ev));
    ev->t_us = t_us;
    ev->type = PHAROS_EV_DOT11;
    ev->u.dot11.type = PHAROS_FT_MGMT;
    ev->u.dot11.subtype = subtype;
    memcpy(ev->u.dot11.a1, a1, 6);
    memcpy(ev->u.dot11.a2, a2, 6);
    memcpy(ev->u.dot11.a3, a2, 6);
    ev->u.dot11.rssi = rssi;
    ev->u.dot11.channel = 6;
    ev->u.dot11.reason_or_status = reason;
    ev->u.dot11.flags = flags;
}

bool pr_range_next(pr_range_t *r, pharos_event_t *out)
{
    if (!r || !out) {
        return false;
    }
    static const uint8_t BCAST[6] = { 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    const uint16_t inten = r->cfg.intensity ? r->cfg.intensity : 500;

    switch (r->cfg.scenario) {
    case PR_SCENARIO_CALM:
        /* Beacons, plus the occasional honest roaming disconnect. Teaches the
         * baseline: this is what "nothing is wrong" looks like. */
        if (r->emitted >= 120) return false;
        r->t_us += 90000; /* ~100 ms beacon interval */
        if ((r->emitted % 20) == 19) {
            mk_mgmt(out, r->t_us, PHAROS_ST_DEAUTH, r->victim, r->ap_bssid,
                    jitter(r, -50, 3), 8, 0);
        } else {
            mk_mgmt(out, r->t_us, PHAROS_ST_BEACON, BCAST, r->ap_bssid,
                    jitter(r, -50, 3), 0, PHAROS_DOT11_F_MFP_SEEN);
        }
        r->emitted++;
        return true;

    case PR_SCENARIO_ROAMING:
        /* One SSID on six BSSIDs. The classic false positive: the range shows
         * the learner that Twin scores this at zero on purpose. */
        if (r->emitted >= 90) return false;
        r->t_us += 80000;
        {
            uint8_t bssid[6];
            memcpy(bssid, r->ap_bssid, 6);
            bssid[5] = (uint8_t)(0x30 + (r->emitted % 6)); /* same OUI, six radios */
            mk_mgmt(out, r->t_us, PHAROS_ST_BEACON, BCAST, bssid,
                    jitter(r, -55, 6), 0, PHAROS_DOT11_F_MFP_SEEN);
        }
        r->emitted++;
        return true;

    case PR_SCENARIO_DEAUTH_FLOOD:
        /* Phase 0: establish the real AP so identity has something to compare
         * against. Phase 1: the attacker sprays broadcast deauths, spoofing
         * the AP's address but arriving at a different signal level. */
        if (r->phase == 0) {
            r->t_us += 100000;
            mk_mgmt(out, r->t_us, PHAROS_ST_BEACON, BCAST, r->ap_bssid,
                    jitter(r, -45, 2), 0, 0 /* no MFP: the flood will work */);
            if (++r->emitted >= 12) { r->phase = 1; r->emitted = 0; }
            return true;
        }
        {
            const uint32_t total = 40 + (uint32_t)inten;
            if (r->emitted >= total) return false;
            /* Frames per step scale with intensity; the attacker claims the
             * AP's address but is heard ~20 dB louder (it is in the room). */
            r->t_us += 8000;
            mk_mgmt(out, r->t_us, PHAROS_ST_DEAUTH, BCAST, r->ap_bssid,
                    jitter(r, -68, 3), 7, 0);
            r->emitted++;
            return true;
        }

    case PR_SCENARIO_EVIL_TWIN:
        /* Real AP (protected) and a twin (open, software address, louder) both
         * beacon. Feed these into the census/twin table, not the watch. */
        if (r->emitted >= 60) return false;
        r->t_us += 90000;
        if (r->emitted & 1) {
            mk_mgmt(out, r->t_us, PHAROS_ST_BEACON, BCAST, r->ap_bssid,
                    jitter(r, -60, 3), 0, PHAROS_DOT11_F_MFP_SEEN | PHAROS_DOT11_F_PROTECTED);
        } else {
            mk_mgmt(out, r->t_us, PHAROS_ST_BEACON, BCAST, r->twin_bssid,
                    jitter(r, -38, 3), 0, 0 /* open: undercuts its sibling */);
        }
        r->emitted++;
        return true;

    case PR_SCENARIO_PROBE_LEAK:
        /* A phone directed-probing for a revealing history, then changing its
         * address but keeping the sequence counter running. Feed to probe. */
        if (r->emitted >= 40) return false;
        r->t_us += 120000;
        {
            uint8_t addr[6];
            memcpy(addr, r->phone, 6);
            if (r->emitted >= 20) {
                addr[1] = 0xEE; /* new random address, second identity */
            }
            mk_mgmt(out, r->t_us, PHAROS_ST_PROBE_REQ, BCAST, addr,
                    jitter(r, -55, 4), 0, 0);
            out->u.dot11.seq = (uint16_t)(100 + r->emitted); /* keeps counting */
        }
        r->emitted++;
        return true;

    default:
        return false;
    }
}

/* ---- narration ------------------------------------------------------- */

static const pr_beat_t k_flood_beats[] = {
    { 0,    "A healthy network. Watch the gauge sit in BACKGROUND." },
    { 1200, "Deauth frames begin. Rate climbs; one family lights up." },
    { 2600, "Broadcast target + spoofed identity: three families now agree." },
    { 4000, "Camped, this reaches FLOOD LIKELY. Hopping, it stops at SUSPICIOUS." },
    { 5200, "That gap is the ceiling. Confidence is earned by standing still." },
};

static const pr_beat_t k_twin_beats[] = {
    { 0,    "Two radios answer to one name. Multiplicity alone scores zero." },
    { 1500, "One is open while its sibling is protected: the posture family." },
    { 3000, "Software address, own channel, far louder: identity + behaviour." },
    { 4200, "All three agree -> TWIN LIKELY. Do not join the loud one." },
};

static const pr_beat_t k_probe_beats[] = {
    { 0,    "A phone announces the networks it remembers, to everyone." },
    { 1800, "It names a workplace and a home - that identifies a person." },
    { 3200, "It changes address. The sequence counter gives it away anyway." },
    { 4200, "Grade: the fix is forgetting old networks, not the address." },
};

static const pr_beat_t k_calm_beats[] = {
    { 0,    "This is the baseline. Learn it, so you know when it breaks." },
    { 2000, "Roaming and idle timeouts produce a little deauth. That is normal." },
};

static const pr_beat_t k_roaming_beats[] = {
    { 0,    "One SSID, six BSSIDs. A naive tool screams evil twin here." },
    { 1800, "Pharos does not: same vendor, same posture. This is roaming." },
    { 3200, "Multiplicity is not evidence. The false positive is the lesson." },
};

unsigned pr_range_beats(pr_scenario_t s, const pr_beat_t **beats)
{
    switch (s) {
    case PR_SCENARIO_DEAUTH_FLOOD: *beats = k_flood_beats;   return 5;
    case PR_SCENARIO_EVIL_TWIN:    *beats = k_twin_beats;    return 4;
    case PR_SCENARIO_PROBE_LEAK:   *beats = k_probe_beats;   return 4;
    case PR_SCENARIO_CALM:         *beats = k_calm_beats;    return 2;
    case PR_SCENARIO_ROAMING:      *beats = k_roaming_beats; return 3;
    default:                       *beats = k_calm_beats;    return 0;
    }
}

const char *pr_scenario_name(pr_scenario_t s)
{
    switch (s) {
    case PR_SCENARIO_CALM:         return "Calm network";
    case PR_SCENARIO_ROAMING:      return "Roaming estate";
    case PR_SCENARIO_DEAUTH_FLOOD: return "Deauth flood";
    case PR_SCENARIO_EVIL_TWIN:    return "Evil twin";
    case PR_SCENARIO_PROBE_LEAK:   return "Probe leak";
    default:                       return "?";
    }
}

const char *pr_scenario_teaches(pr_scenario_t s)
{
    switch (s) {
    case PR_SCENARIO_CALM:
        return "What normal looks like, so a break in it stands out.";
    case PR_SCENARIO_ROAMING:
        return "Why many BSSIDs on one SSID is not an attack.";
    case PR_SCENARIO_DEAUTH_FLOOD:
        return "How a flood earns three families - and why hopping caps it.";
    case PR_SCENARIO_EVIL_TWIN:
        return "Why an alarm needs the posture gap, not just an odd radio.";
    case PR_SCENARIO_PROBE_LEAK:
        return "How randomisation is defeated, and what actually fixes it.";
    default:
        return "";
    }
}
