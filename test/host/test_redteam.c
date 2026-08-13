/* Pharos host tests, part seven: the red-team-facing engines.
 *
 *  - flood: detecting the beacon/SSID-spam attack the ESP32 world is known
 *    for, while refusing to cry wolf at a genuinely busy city.
 *  - opsec: the footprint scorer, driven by the REAL Watch engine, that tells
 *    an operator how detectable their attack is and what gives it away.
 */
#include "pharos_flood.h"
#include "pharos_opsec.h"
#include "pharos_range.h"
#include "pharos_watch.h"
#include "test_support.h"

/* ----------------------------------------------------------------- flood */

static void bssid_seq(uint8_t out[6], unsigned i, bool local)
{
    out[0] = local ? 0x02 : 0x3C; /* local bit set, or a real-looking OUI */
    out[1] = 0xAB;
    out[2] = 0xCD;
    out[3] = (uint8_t)(i >> 8);
    out[4] = (uint8_t)i;
    out[5] = 0x01;
}

static void real_bssid(uint8_t out[6], unsigned vendor, unsigned dev)
{
    /* A plausible globally-administered address: distinct real vendors. */
    static const uint8_t ouis[6][3] = {
        { 0x3C, 0x22, 0xFB }, { 0x00, 0x1A, 0x11 }, { 0xB8, 0x27, 0xEB },
        { 0xAC, 0xDE, 0x48 }, { 0x00, 0x50, 0x56 }, { 0xF0, 0x9F, 0xC2 },
    };
    memcpy(out, ouis[vendor % 6], 3);
    out[3] = 0x10; out[4] = (uint8_t)vendor; out[5] = (uint8_t)dev;
}

static pf_context_t fctx(uint16_t dwell)
{
    pf_context_t c = { .dwell_permil = dwell, .bus_yield_permil = 1000,
                       .window_ms = 12000 };
    return c;
}

void test_flood(void)
{
    banner("flood: beacon / SSID spam");
    pf_engine_t e;
    pf_verdict_t v;
    pf_context_t camped = fctx(1000);

    /* Quiet: a handful of persistent networks. */
    pf_reset(&e);
    for (unsigned round = 0; round < 30; round++) {
        for (unsigned i = 0; i < 4; i++) {
            uint8_t b[6];
            real_bssid(b, i, 1);
            char name[16];
            snprintf(name, sizeof(name), "Home-%u", i);
            pf_observe(&e, b, name, (uint8_t)strlen(name), 1000000ull * round + i);
        }
    }
    pf_evaluate(&e, &camped, &v);
    CHECK(v.band <= PF_BAND_BUSY, "four persistent networks are quiet (got %s)",
          pf_band_name(v.band));
    CHECK_EQ(v.distinct_ssids, 4);

    /* THE false positive: a dense city rooftop - 60 real, persistent networks
     * from many real vendors. Lots of names, but they do not churn and they
     * are not synthetic. Must not alarm. */
    pf_reset(&e);
    for (unsigned round = 0; round < 20; round++) {
        for (unsigned i = 0; i < 60; i++) {
            uint8_t b[6];
            real_bssid(b, i, i % 4);
            char name[24];
            snprintf(name, sizeof(name), "Resident_%02u_5G", i);
            pf_observe(&e, b, name, (uint8_t)strlen(name),
                       1000000ull * round + i * 1000ull);
        }
    }
    pf_evaluate(&e, &camped, &v);
    CHECK(v.band <= PF_BAND_BUSY, "a dense city is BUSY, not a flood (got %s @ %u)",
          pf_band_name(v.band), v.score);
    CHECK(v.notes & PF_NOTE_URBAN, "recognised as urban density");
    CHECK(!(v.families & PF_FAM_EPHEMERAL), "persistent names are not ephemeral");
    CHECK(v.score <= 44, "volume alone cannot pass BUSY (got %u)", v.score);

    /* The attack: hundreds of fabricated names, each beaconed once or twice,
     * from software BSSIDs sharing a prefix. */
    pf_reset(&e);
    static const char *const words[] = { "Free", "Guest", "WiFi", "Net", "Fast",
                                         "Home", "Fibre", "5G", "Café", "Pub" };
    for (unsigned i = 0; i < 300; i++) {
        uint8_t b[6];
        bssid_seq(b, i, true);
        char name[24];
        snprintf(name, sizeof(name), "%s_%s_%03u", words[i % 10], words[(i / 10) % 10], i);
        /* Each name beaconed just twice across a short, fast window. */
        pf_observe(&e, b, name, (uint8_t)strlen(name), 5000000ull + i * 20000ull);
        pf_observe(&e, b, name, (uint8_t)strlen(name), 5000000ull + i * 20000ull + 500);
    }
    pf_evaluate(&e, &camped, &v);
    CHECK_EQ(v.band, PF_BAND_FLOOD_LIKELY);
    CHECK(v.families & PF_FAM_VOLUME, "volume family");
    CHECK(v.families & PF_FAM_EPHEMERAL, "ephemeral family");
    CHECK(v.families & PF_FAM_SYNTHETIC, "synthetic family");
    CHECK(v.notes & PF_NOTE_TABLE_FULL, "the flood overran the table, and it is noted");
    CHECK(v.score <= v.ceiling, "within ceiling");
    CHECK(v.synthetic_permil > 900, "nearly all names on software BSSIDs");

    /* Same flood, hopping: the churn claim is weakened, so it must not reach
     * the alarm band on a hopping receiver. */
    pf_verdict_t vh;
    pf_context_t hopping = fctx(71);
    pf_evaluate(&e, &hopping, &vh);
    CHECK(vh.band < PF_BAND_FLOOD_LIKELY, "hopping cannot alarm a flood (got %s)",
          pf_band_name(vh.band));
    CHECK(pf_ceiling(&hopping) < pf_ceiling(&camped), "ceiling follows dwell");
    CHECK(pf_ceiling(&camped) < 100, "nothing certain");

    /* A short burst is not a rate. */
    pf_reset(&e);
    for (unsigned i = 0; i < 40; i++) {
        uint8_t b[6];
        bssid_seq(b, i, true);
        char name[16];
        snprintf(name, sizeof(name), "burst_%03u", i);
        pf_observe(&e, b, name, (uint8_t)strlen(name), 1000000ull + i * 5000ull);
    }
    pf_evaluate(&e, &camped, &v);
    CHECK(v.notes & PF_NOTE_SHORT, "a sub-second burst is disclosed as short");
    CHECK(v.score <= 49, "cannot alarm on a burst (got %u)", v.score);

    /* Hidden / empty SSIDs are ignored, not counted as names. */
    pf_reset(&e);
    uint8_t b[6];
    real_bssid(b, 0, 0);
    pf_observe(&e, b, "", 0, 1000);
    pf_observe(&e, b, NULL, 0, 2000);
    pf_evaluate(&e, &camped, &v);
    CHECK_EQ(v.distinct_ssids, 0);
    CHECK_EQ(v.band, PF_BAND_QUIET);

    /* Vocabulary. */
    for (int band = PF_BAND_QUIET; band <= PF_BAND_FLOOD_LIKELY; band++) {
        const char *nm = pf_band_name((pf_band_t)band);
        const char *ad = pf_band_advice((pf_band_t)band);
        CHECK(nm && *nm && ad && *ad, "band %d described", band);
        CHECK(strstr(nm, "SAFE") == NULL, "no band claims safety");
    }
}

/* ----------------------------------------------------------------- opsec */

/* Grade the range's flood scenario with the real Watch engine, camped and
 * hopping, then assess the footprint - exactly as the Footprint lens will. */
static void watch_pair(pw_verdict_t *camped, pw_verdict_t *hopping, uint16_t intensity)
{
    pr_range_t r;
    pr_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.scenario = PR_SCENARIO_DEAUTH_FLOOD;
    cfg.seed = 0xABCD;
    cfg.intensity = intensity;
    pr_range_init(&r, &cfg);
    pw_engine_t eng;
    pw_reset(&eng);
    pharos_event_t ev;
    uint64_t last = 0;
    while (pr_range_next(&r, &ev)) {
        pw_observe(&eng, &ev.u.dot11, ev.t_us);
        last = ev.t_us;
    }
    pw_context_t c = { .dwell_permil = 1000, .bus_yield_permil = 1000, .window_ms = 12000 };
    pw_context_t h = { .dwell_permil = 71, .bus_yield_permil = 1000, .window_ms = 12000 };
    pw_evaluate(&eng, last, &c, camped);
    pw_evaluate(&eng, last, &h, hopping);
}

void test_opsec(void)
{
    banner("opsec: how detectable is this attack?");
    pw_verdict_t camped, hopping;
    po_report_t r;

    watch_pair(&camped, &hopping, 800);
    po_assess(&camped, &hopping, &r);

    /* The range flood is broadcast + spoofed + high-rate, so a camped defender
     * alarms and the footprint is loud. */
    CHECK(camped.band == PW_BAND_LIKELY, "the range flood alarms a camped defender");
    CHECK(r.grade >= PO_GRADE_LOUD, "a broadcast flood is loud (got %s)",
          po_grade_name(r.grade));
    CHECK_EQ(r.camped_score, camped.score);
    CHECK_EQ(r.hopping_score, hopping.score);

    /* The single most useful OPSEC fact: loud when watched, missed when the
     * defender is hopping. */
    CHECK(hopping.band <= PW_BAND_SUSPICIOUS, "hopping does not alarm on it");
    CHECK(r.invisible_to_hoppers, "footprint flags that hoppers miss it");
    CHECK(r.stealth_gap > 0, "camping costs the attacker real visibility");
    CHECK_EQ(r.stealth_gap, camped.score - hopping.score);

    /* The dominant tell must be one an operator can act on, and the guidance
     * must name it. */
    CHECK(r.dominant_tell != PO_TELL_NONE, "a dominant tell is identified");
    CHECK(r.tell_name && *r.tell_name, "the tell is named: %s", r.tell_name);
    CHECK(r.guidance && strlen(r.guidance) > 20, "actionable guidance present");

    /* Monotonicity: a louder attack is never assessed as quieter. */
    pw_verdict_t c2, h2;
    po_report_t r2;
    watch_pair(&c2, &h2, 200); /* gentler */
    po_assess(&c2, &h2, &r2);
    CHECK(r2.camped_score <= r.camped_score, "gentler attack scores no higher");
    CHECK(r2.grade <= r.grade, "gentler attack is no louder");

    /* A ghost: nothing observed at all grades GHOST, not an error. */
    pw_verdict_t empty;
    memset(&empty, 0, sizeof(empty));
    po_assess(&empty, &empty, &r2);
    CHECK_EQ(r2.grade, PO_GRADE_GHOST);
    CHECK(!r2.invisible_to_hoppers, "a ghost is not 'invisible to hoppers' - it is invisible to all");

    /* Null hopping side is handled: worst-case still assessable. */
    po_assess(&camped, NULL, &r2);
    CHECK_EQ(r2.grade, r.grade);
    CHECK_EQ(r2.hopping_score, 0);
    CHECK(!r2.invisible_to_hoppers, "cannot claim hopper-invisibility without the hopping view");

    /* Null camped side is refused honestly. */
    po_assess(NULL, &hopping, &r2);
    CHECK(strstr(r2.headline, "cannot judge") != NULL, "no camped view is disclosed");

    /* Vocabulary. */
    for (int g = PO_GRADE_GHOST; g <= PO_GRADE_BLARING; g++) {
        CHECK(po_grade_name((po_grade_t)g)[0] != '?', "grade %d named", g);
    }
    for (int t = PO_TELL_NONE; t <= PO_TELL_REASON; t++) {
        CHECK(po_tell_name((po_tell_t)t)[0] != '\0', "tell %d named", t);
    }
    /* OPSEC guidance is defensive - it explains detectability, never how to
     * evade. A greppable guard against the tool's purpose drifting. */
    static const po_tell_t tells[] = { PO_TELL_RATE, PO_TELL_TARGET,
                                       PO_TELL_IDENTITY, PO_TELL_REASON, PO_TELL_NONE };
    for (unsigned i = 0; i < 5; i++) {
        pw_verdict_t fake;
        memset(&fake, 0, sizeof(fake));
        fake.score = 80;
        fake.families = 0x7;
        switch (tells[i]) {
        case PO_TELL_RATE:     fake.c_rate = 40; break;
        case PO_TELL_TARGET:   fake.c_target = 22; break;
        case PO_TELL_IDENTITY: fake.c_identity = 22; break;
        case PO_TELL_REASON:   fake.c_reason = 12; break;
        default: break;
        }
        po_assess(&fake, NULL, &r2);
        CHECK(strstr(r2.guidance, "evade") == NULL, "guidance never coaches evasion");
        CHECK(strstr(r2.guidance, "avoid detection") == NULL, "guidance is defensive");
    }
}
