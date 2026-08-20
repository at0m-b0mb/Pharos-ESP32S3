/* Pharos host tests, part five: the training range.
 *
 * These are the tests that make the range trustworthy as a teaching tool. A
 * scenario is only honest if, fed through the real engines, it produces the
 * verdict the lesson claims it will - and reproducibly, from a seed. So the
 * range is driven here exactly as the live lens drives it, and the engine's
 * verdict is asserted. If a future change makes the "flood" scenario stop
 * reaching FLOOD LIKELY when camped, this fails, and the lesson is not a lie.
 */
#include "pharos_census.h"
#include "pharos_probe.h"
#include "pharos_range.h"
#include "pharos_twin.h"
#include "pharos_watch.h"
#include "test_support.h"

static pr_config_t cfg(pr_scenario_t s, uint16_t dwell, uint16_t intensity)
{
    pr_config_t c;
    memset(&c, 0, sizeof(c));
    c.scenario = s;
    c.seed = 0xBEEF;
    c.dwell_permil = dwell;
    c.intensity = intensity;
    return c;
}

void test_range_determinism(void)
{
    banner("range: deterministic from a seed");
    pr_range_t a, b;
    pr_config_t c = cfg(PR_SCENARIO_DEAUTH_FLOOD, 1000, 600);
    pr_range_init(&a, &c);
    pr_range_init(&b, &c);

    pharos_event_t ea, eb;
    unsigned n = 0;
    bool ha, hb;
    do {
        ha = pr_range_next(&a, &ea);
        hb = pr_range_next(&b, &eb);
        CHECK_EQ(ha, hb);
        if (ha) {
            CHECK(memcmp(&ea, &eb, sizeof(ea)) == 0, "event %u identical", n);
        }
        n++;
    } while (ha && n < 5000);
    CHECK(n > 40, "scenario produced a meaningful number of events (%u)", n);
}

void test_range_flood(void)
{
    banner("range: the flood scenario drives the real Watch engine");

    /* Camped: the lesson claims this reaches FLOOD LIKELY. */
    pr_range_t r;
    pr_config_t c = cfg(PR_SCENARIO_DEAUTH_FLOOD, 1000, 800);
    pr_range_init(&r, &c);
    pw_engine_t eng;
    pw_reset(&eng);
    pharos_event_t ev;
    uint64_t last = 0;
    while (pr_range_next(&r, &ev)) {
        pw_observe(&eng, &ev.u.dot11, ev.t_us);
        last = ev.t_us;
    }
    pw_context_t camped = { .dwell_permil = 1000, .bus_yield_permil = 1000,
                            .window_ms = 12000 };
    pw_verdict_t v;
    pw_evaluate(&eng, last, &camped, &v);
    CHECK_EQ(v.band, PW_BAND_LIKELY);
    CHECK(v.families & PW_FAM_RATE, "rate family in the range flood");

    /* Same events, hopping receiver: the lesson claims this stops at
     * SUSPICIOUS. This is the whole point of the range - the ceiling is
     * demonstrated with the identical stream. */
    pw_context_t hopping = { .dwell_permil = 71, .bus_yield_permil = 1000,
                             .window_ms = 12000 };
    pw_verdict_t vh;
    pw_evaluate(&eng, last, &hopping, &vh);
    CHECK(vh.band <= PW_BAND_SUSPICIOUS,
          "hopping caps the range flood below the alarm (got %s)",
          pw_band_name(vh.band));
    CHECK(v.score > vh.score, "camping earns more than hopping on the same stream");
}

/* The companion to test_range_flood, and the point of the pair.
 *
 * Same kind of attack, but against a network that REQUIRES protected
 * management frames. The unprotected disconnects are then a contradiction
 * rather than a measurement, and a contradiction does not get weaker because
 * the receiver only visited for 200 ms. This is the scenario where a hopping
 * receiver is entitled to alarm - and the flood scenario, whose attacker rode
 * the access point's own sequence counter and gave away only its position, is
 * the one where it is not. */
void test_range_proven(void)
{
    banner("range: a contradiction may alarm even while hopping");

    pr_range_t r;
    pr_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.scenario = PR_SCENARIO_DEAUTH_PROVEN;
    cfg.seed = 0xBEEF;
    cfg.intensity = 800;
    pr_range_init(&r, &cfg);

    pw_engine_t eng;
    pw_reset(&eng);
    pharos_event_t ev;
    uint64_t last = 0;
    while (pr_range_next(&r, &ev)) {
        pw_observe(&eng, &ev.u.dot11, ev.t_us);
        last = ev.t_us;
    }

    pw_context_t hopping = { .dwell_permil = 77, .bus_yield_permil = 1000,
                             .window_ms = 12000 };
    pw_verdict_t vh;
    pw_evaluate(&eng, last, &hopping, &vh);

    CHECK(vh.forgery & PW_FORGE_MFP_PROOF,
          "the 802.11w contradiction is found (forgery=0x%02x)", vh.forgery);
    CHECK(vh.notes & PW_NOTE_HARD, "and it counts as hard evidence");
    CHECK_EQ(vh.ceiling, PW_CEILING_HARD_EVIDENCE);
    CHECK(vh.ceiling > pw_ceiling(&hopping),
          "the ceiling rose above what dwell alone would allow (%u vs %u)",
          vh.ceiling, pw_ceiling(&hopping));
    CHECK(vh.rejoins_after > 0, "the clients came back (%u of %u)",
          vh.rejoins_after, vh.rejoins);
    CHECK(vh.score <= vh.ceiling, "and it still respects its own ceiling");
}

void test_range_calm_and_roaming(void)
{
    banner("range: calm stays calm, roaming is not a twin");

    pr_range_t r;
    pr_config_t c = cfg(PR_SCENARIO_CALM, 1000, 0);
    pr_range_init(&r, &c);
    pw_engine_t eng;
    pw_reset(&eng);
    pharos_event_t ev;
    uint64_t last = 0;
    while (pr_range_next(&r, &ev)) {
        pw_observe(&eng, &ev.u.dot11, ev.t_us);
        last = ev.t_us;
    }
    pw_context_t ctx = { .dwell_permil = 1000, .bus_yield_permil = 1000,
                         .window_ms = 12000 };
    pw_verdict_t v;
    pw_evaluate(&eng, last, &ctx, &v);
    CHECK(v.band <= PW_BAND_BACKGROUND, "the calm scenario must not alarm (got %s)",
          pw_band_name(v.band));

    /* Roaming: build a census table from the beacons, then run Twin. */
    pr_config_t rc = cfg(PR_SCENARIO_ROAMING, 1000, 0);
    pr_range_init(&r, &rc);
    pc_ap_t aps[PT_MAX_GROUP];
    unsigned n = 0;
    while (pr_range_next(&r, &ev) && n < PT_MAX_GROUP) {
        const uint8_t *bssid = ev.u.dot11.a2;
        /* find or add */
        unsigned k = 0;
        for (; k < n; k++) {
            if (memcmp(aps[k].bssid, bssid, 6) == 0) break;
        }
        if (k == n) {
            memset(&aps[n], 0, sizeof(aps[n]));
            memcpy(aps[n].bssid, bssid, 6);
            aps[n].privacy = true;
            aps[n].ccmp_pairwise = true;
            aps[n].rsn.has_rsn = true;
            aps[n].rsn.has_sae = true;
            aps[n].rsn.mfp_required = true;
            aps[n].rsn.mfp_capable = true;
            aps[n].channel = 6;
            aps[n].rssi = ev.u.dot11.rssi;
            n++;
        }
        aps[k < n ? k : n - 1].beacons++;
    }
    CHECK(n >= 5, "roaming scenario produced several BSSIDs (%u)", n);
    pt_context_t tctx = { .dwell_permil = 1000 };
    pt_verdict_t tv;
    pt_evaluate(aps, n, NULL, &tctx, &tv);
    CHECK_EQ(tv.band, PT_BAND_CONSISTENT);
    CHECK(tv.notes & PT_NOTE_ROAMING, "range roaming is recognised as roaming");
}

void test_range_probe_leak(void)
{
    banner("range: the probe-leak scenario defeats randomisation");
    pr_range_t r;
    pr_config_t c = cfg(PR_SCENARIO_PROBE_LEAK, 1000, 0);
    pr_range_init(&r, &c);
    pp_engine_t eng;
    pp_reset(&eng);
    pharos_event_t ev;
    while (pr_range_next(&r, &ev)) {
        pp_probe_t p;
        memset(&p, 0, sizeof(p));
        memcpy(p.addr, ev.u.dot11.a2, 6);
        p.seq = ev.u.dot11.seq;
        p.rssi = ev.u.dot11.rssi;
        p.t_us = ev.t_us;
        p.fingerprint = 0xF1DE; /* one chipset across the address change */
        pp_observe(&eng, &p);
    }
    /* The address changed halfway, but the fingerprint and sequence continuity
     * should fold both identities into one tracked device. */
    CHECK_EQ(eng.n_devices, 1);
    CHECK(eng.devices[0].identities >= 2, "the address change was seen through");
}

void test_range_vocabulary(void)
{
    banner("range: every scenario is named and teaches something");
    for (int s = 0; s < PR_SCENARIO_COUNT; s++) {
        const char *nm = pr_scenario_name((pr_scenario_t)s);
        const char *te = pr_scenario_teaches((pr_scenario_t)s);
        CHECK(nm && *nm, "scenario %d named", s);
        CHECK(te && *te, "scenario %d teaches something", s);
        const pr_beat_t *beats = NULL;
        const unsigned nb = pr_range_beats((pr_scenario_t)s, &beats);
        CHECK(beats != NULL, "scenario %d has narration", s);
        for (unsigned i = 0; i < nb; i++) {
            CHECK(beats[i].text && *beats[i].text, "beat text present");
        }
    }
}
