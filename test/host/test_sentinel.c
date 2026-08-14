/* Pharos host tests, part ten: Sentinel - the site baseline and its diff. */
#include "pharos_sentinel.h"
#include "test_support.h"

static pc_ap_t ap_make(const char *ssid, uint8_t last, uint8_t chan, bool sae,
                       bool mfp, bool open)
{
    pc_ap_t a;
    memset(&a, 0, sizeof(a));
    const uint8_t b[6] = { 0xAC, 0x11, 0x22, 0x33, 0x44, last };
    memcpy(a.bssid, b, 6);
    a.ssid_len = (uint8_t)strlen(ssid);
    memcpy(a.ssid, ssid, a.ssid_len);
    a.channel = chan;
    a.rssi = -50;
    a.beacons = 20;
    a.beacon_ms = 100;
    if (!open) {
        a.privacy = true;
        a.ccmp_pairwise = true;
        a.rsn.has_rsn = true;
        if (sae) a.rsn.has_sae = true; else a.rsn.has_psk = true;
        a.rsn.mfp_capable = mfp;
        a.rsn.mfp_required = mfp;
    }
    return a;
}

static ps_context_t ctx_of(uint16_t dwell)
{
    ps_context_t c = { .dwell_permil = dwell, .sweep_ms = 20000 };
    return c;
}

static const ps_finding_t *find_change(const ps_verdict_t *v, ps_change_t c)
{
    for (unsigned i = 0; i < v->n_findings; i++) {
        if (v->findings[i].change == c) return &v->findings[i];
    }
    return NULL;
}

void test_sentinel(void)
{
    banner("sentinel: what changed since the baseline");
    ps_baseline_t base;
    ps_verdict_t v;
    ps_context_t camped = ctx_of(1000);

    /* A three-AP estate, all healthy WPA3 + MFP. */
    pc_ap_t est[3] = {
        ap_make("Acme-Staff", 0x01, 1, true, true, false),
        ap_make("Acme-Guest", 0x02, 6, true, true, false),
        ap_make("Acme-IoT",   0x03, 11, true, true, false),
    };

    /* No baseline: says so, does not invent findings. */
    ps_reset(&base);
    ps_compare(&base, est, 3, &camped, &v);
    CHECK(v.notes & PS_NOTE_NO_BASELINE, "no baseline disclosed");
    CHECK_EQ(v.band, PS_BAND_UNCHANGED);
    CHECK_EQ(v.n_findings, 0);
    CHECK(strstr(v.headline, "No baseline") != NULL, "headline says so");

    /* Adopt, then sweep the identical estate: nothing changed. */
    CHECK_EQ(ps_adopt(&base, est, 3, 1000), 3);
    CHECK(base.adopted, "baseline adopted");
    ps_compare(&base, est, 3, &camped, &v);
    CHECK_EQ(v.band, PS_BAND_UNCHANGED);
    CHECK_EQ(v.n_findings, 0);
    CHECK_EQ(v.score, 0);
    CHECK(strstr(ps_band_advice(v.band), "one channel at a time") != NULL,
          "even 'unchanged' admits the receiver's limit");

    /* THE finding that matters: an AP drops from WPA3+MFP to open. */
    {
        pc_ap_t now[3];
        memcpy(now, est, sizeof(est));
        now[1] = ap_make("Acme-Guest", 0x02, 6, false, false, true); /* open! */
        ps_compare(&base, now, 3, &camped, &v);
        CHECK_EQ(v.band, PS_BAND_INVESTIGATE);
        CHECK_EQ(v.n_downgrade, 1);
        const ps_finding_t *f = find_change(&v, PS_CHANGE_DOWNGRADE);
        CHECK(f != NULL, "downgrade recorded");
        CHECK(f && f->severity >= 85, "open-now is the top severity (got %u)",
              f ? f->severity : 0);
        CHECK(f && f->was > f->now, "grade actually fell (%u -> %u)",
              f ? f->was : 0, f ? f->now : 0);
        CHECK(strstr(v.headline, "weaker") != NULL, "headline: %s", v.headline);
    }

    /* Losing 802.11w alone is a downgrade too - that is the weakness the Watch
     * lens detects being exploited. */
    {
        pc_ap_t now[3];
        memcpy(now, est, sizeof(est));
        now[0] = ap_make("Acme-Staff", 0x01, 1, true, false, false); /* mfp gone */
        ps_compare(&base, now, 3, &camped, &v);
        CHECK_EQ(v.n_downgrade, 1);
        const ps_finding_t *f = find_change(&v, PS_CHANGE_DOWNGRADE);
        CHECK(f && f->severity >= 70, "losing MFP scores high (got %u)",
              f ? f->severity : 0);
        CHECK_EQ(v.band, PS_BAND_INVESTIGATE);
    }

    /* A brand-new, unremarkable AP is ordinary churn, not an alarm. */
    {
        pc_ap_t now[4];
        memcpy(now, est, sizeof(est));
        now[3] = ap_make("Neighbour-5G", 0x77, 3, true, true, false);
        ps_compare(&base, now, 4, &camped, &v);
        CHECK_EQ(v.n_new, 1);
        CHECK(v.band <= PS_BAND_DRIFT, "a healthy newcomer is drift, not alarm (got %s)",
              ps_band_name(v.band));
    }

    /* But a newcomer wearing an SSID the estate already owns is the shape of
     * an impersonator, and it escalates. */
    {
        pc_ap_t now[4];
        memcpy(now, est, sizeof(est));
        now[3] = ap_make("Acme-Guest", 0x99, 3, false, false, true); /* open twin */
        ps_compare(&base, now, 4, &camped, &v);
        CHECK_EQ(v.n_new, 1);
        CHECK(v.notes & PS_NOTE_SSID_REUSE, "SSID reuse noted");
        CHECK(v.band >= PS_BAND_NOTABLE, "open twin of a known SSID escalates (got %s)",
              ps_band_name(v.band));
        const ps_finding_t *f = find_change(&v, PS_CHANGE_NEW);
        CHECK(f && f->severity >= 60, "severity reflects both tells (got %u)",
              f ? f->severity : 0);
    }

    /* MISSING is the lowest-severity finding, because this receiver hears one
     * channel at a time - and it is discounted further on a thin sweep. */
    {
        ps_verdict_t vc, vh;
        ps_context_t hopping = ctx_of(71);
        ps_compare(&base, est, 2, &camped, &vc);   /* third AP not heard */
        CHECK_EQ(vc.n_missing, 1);
        CHECK(vc.band <= PS_BAND_DRIFT, "a missing AP alone is not an alarm");
        const ps_finding_t *fc = find_change(&vc, PS_CHANGE_MISSING);
        CHECK(fc && fc->severity <= 10, "missing scores low (got %u)",
              fc ? fc->severity : 0);

        ps_compare(&base, est, 2, &hopping, &vh);
        const ps_finding_t *fh = find_change(&vh, PS_CHANGE_MISSING);
        CHECK(vh.notes & PS_NOTE_THIN_SWEEP, "thin sweep disclosed");
        CHECK(fh && fc && fh->severity < fc->severity,
              "a hopping sweep trusts MISSING even less (%u < %u)",
              fh ? fh->severity : 0, fc ? fc->severity : 0);
    }

    /* Channel move and rename are detected and separated. */
    {
        pc_ap_t now[3];
        memcpy(now, est, sizeof(est));
        now[2].channel = 9;
        ps_compare(&base, now, 3, &camped, &v);
        CHECK_EQ(v.n_moved, 1);
        const ps_finding_t *f = find_change(&v, PS_CHANGE_MOVED);
        CHECK(f && f->channel_was == 11 && f->channel_now == 9,
              "move records both channels");

        memcpy(now, est, sizeof(est));
        now[0] = ap_make("Acme-Corp", 0x01, 1, true, true, false);
        ps_compare(&base, now, 3, &camped, &v);
        CHECK_EQ(v.n_renamed, 1);
        CHECK(find_change(&v, PS_CHANGE_RENAMED) != NULL, "rename recorded");
    }

    /* An upgrade is still a change, but it is good news and scores as such. */
    {
        ps_baseline_t weak;
        pc_ap_t before[1] = { ap_make("Acme-Staff", 0x01, 1, false, false, false) };
        pc_ap_t after[1]  = { ap_make("Acme-Staff", 0x01, 1, true,  true,  false) };
        ps_adopt(&weak, before, 1, 1000);
        ps_compare(&weak, after, 1, &camped, &v);
        CHECK_EQ(v.n_upgrade, 1);
        CHECK_EQ(v.n_downgrade, 0);
        CHECK(v.band <= PS_BAND_DRIFT, "an upgrade is not an alarm");
    }

    /* Ceilings behave like every other engine. */
    ps_context_t thin_ctx = ctx_of(71);
    CHECK(ps_ceiling(&camped) > ps_ceiling(&thin_ctx), "camping buys confidence");
    CHECK(ps_ceiling(&camped) < 100, "nothing here is certain");

    /* Baseline capacity is bounded, not overrun. */
    {
        ps_baseline_t big;
        pc_ap_t many[PS_MAX_BASELINE + 8];
        for (unsigned i = 0; i < PS_MAX_BASELINE + 8; i++) {
            many[i] = ap_make("Net", (uint8_t)i, 6, true, true, false);
        }
        CHECK_EQ(ps_adopt(&big, many, PS_MAX_BASELINE + 8, 1), PS_MAX_BASELINE);
    }

    /* Vocabulary: no band or advice may promise safety. */
    for (int b = PS_BAND_UNCHANGED; b <= PS_BAND_INVESTIGATE; b++) {
        const char *nm = ps_band_name((ps_band_t)b);
        const char *ad = ps_band_advice((ps_band_t)b);
        CHECK(nm && *nm && ad && *ad, "band %d described", b);
        CHECK(strstr(nm, "SAFE") == NULL && strstr(nm, "SECURE") == NULL,
              "no band claims safety");
        CHECK(strstr(ad, " is safe") == NULL, "advice never says safe");
    }
    for (int c = PS_CHANGE_NONE; c < PS_CHANGE_COUNT; c++) {
        CHECK(ps_change_name((ps_change_t)c)[0] != '\0', "change %d named", c);
    }
}
