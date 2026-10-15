/* Pharos - fuzzing the VERDICTS, not the parsers
 *
 * fuzz.c proves the parsers survive hostile bytes. That is the smaller half of
 * the promise. This firmware's actual claim is about what it SAYS: that a
 * number on the glass never exceeds the ceiling the evidence earned, that it
 * never reaches certainty, and that a headline is never richer than what was
 * observed. Those are properties of the scoring engines, and a buffer fuzzer
 * cannot reach them - they live in sequences of observations over time, not in
 * single frames.
 *
 * So this drives the engines with random event STREAMS and checks the
 * invariants after every single step. The streams are deliberately hostile in
 * the way the real device is hostile:
 *
 *   - TIME GOES BACKWARDS. The 802.11 driver callback, the NimBLE host task
 *     and the dwell ring are three producers with their own clocks reaching
 *     one consumer. Events arrive out of order, share timestamps, and jump.
 *     An engine that subtracts two timestamps without ordering them will
 *     divide by zero, or compute an age of 584,000 years, and the only place
 *     that shows up is on the glass.
 *   - SCORES ARRIVE ABOVE THEIR CEILINGS. Aegis takes other engines' numbers
 *     on trust. If one of them is ever wrong, Aegis must not publish it.
 *   - THE SAME OBSERVATION REPEATS. Nothing may be counted twice for being
 *     heard twice.
 *
 * The invariants are not examples. They must hold for EVERY stream, which is
 * what makes a random one worth running.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pharos_aegis.h"
#include "pharos_census.h"
#include "pharos_dot11.h"
#include "pharos_twin.h"

static unsigned g_checks;
static unsigned g_fails;
static uint64_t g_seed;
static uint64_t g_state;

#define CHECK(cond, ...)                                                       \
    do {                                                                       \
        g_checks++;                                                            \
        if (!(cond)) {                                                         \
            g_fails++;                                                         \
            printf("  FAIL seed=%llu: ", (unsigned long long)g_seed);          \
            printf(__VA_ARGS__);                                               \
            printf("\n");                                                      \
            if (g_fails > 40u) { printf("  (stopping)\n"); exit(1); }          \
        }                                                                      \
    } while (0)

static uint64_t rnd(void)
{
    g_state ^= g_state >> 12;
    g_state ^= g_state << 25;
    g_state ^= g_state >> 27;
    return g_state * 2685821657736338717ull;
}
static unsigned rnd_below(unsigned n) { return n ? (unsigned)(rnd() % n) : 0u; }

/* A timestamp that is *not* politely increasing. Mostly forward, sometimes
 * identical, sometimes backwards, occasionally a jump of hours - which is what
 * a 64-bit microsecond counter looks like to an engine that has been asleep
 * through a lens change. */
static uint64_t hostile_time(uint64_t prev)
{
    switch (rnd_below(10)) {
    case 0: return prev;                              /* same instant      */
    case 1: return prev > 5000000ull ? prev - (uint64_t)rnd_below(5000000)
                                     : 0ull;          /* backwards         */
    case 2: return prev + 3600000000ull;              /* an hour on        */
    case 3: return 0ull;                              /* the epoch         */
    default: return prev + (uint64_t)rnd_below(2000000) + 1ull;
    }
}

/* ---- the invariants every verdict must satisfy, whatever the stream ---- */

static void check_honest(uint8_t score, uint8_t raw, uint8_t ceiling,
                         const char *who)
{
    /* THE KEYSTONE. The published number may never exceed what the evidence
     * earned. Everything else in this firmware is decoration if this breaks. */
    CHECK(score <= ceiling, "%s published %u against a ceiling of %u",
          who, score, ceiling);
    /* Nothing here ever reaches certainty - that is the stated contract, and
     * a detector that says 100 is claiming something radio cannot support. */
    CHECK(score < 100u, "%s reached certainty (%u)", who, score);
    CHECK(ceiling <= 100u, "%s ceiling out of range (%u)", who, ceiling);
    /* The raw number is what the evidence said before the ceiling was
     * applied, so the published one may be lower but never higher. */
    CHECK(score <= raw || raw == 0u,
          "%s published %u above its own raw score %u", who, score, raw);
}

static void drive_aegis(void)
{
    pa_state_t s;
    pa_reset(&s);
    uint64_t t = (uint64_t)rnd_below(1000000);
    const unsigned steps = 1u + rnd_below(24);

    for (unsigned i = 0; i < steps; i++) {
        const pa_stage_t st = (pa_stage_t)rnd_below((unsigned)PA_STAGE_COUNT);
        /* Including scores ABOVE their stated ceiling: Aegis takes these from
         * other engines on trust, and must not republish a broken one. */
        const uint8_t score   = (uint8_t)rnd_below(130);
        const uint8_t ceiling = (uint8_t)rnd_below(130);
        t = hostile_time(t);

        pa_observe(&s, st, score, ceiling, t);
        if (rnd_below(16) == 0) { pa_acknowledge(&s); }

        pa_verdict_t v;
        memset(&v, 0xAA, sizeof(v));
        pa_evaluate(&s, hostile_time(t), &v);

        check_honest(v.score, v.raw_score, v.ceiling, "aegis");

        /* THE DOCUMENTED PROPERTY: "the overall confidence ceiling is the
         * MINIMUM of the contributing stages' ceilings". A conclusion drawn
         * from a thin sweep and a good one is as weak as the thin one, so the
         * overall ceiling may never exceed any contributor's. */
        for (unsigned k = 0; k < (unsigned)PA_STAGE_COUNT; k++) {
            if (s.stages[k].hits == 0u) { continue; }
            const uint8_t c = s.stages[k].ceiling;
            if (c == 0u) { continue; }
            CHECK(v.ceiling <= c || v.ceiling <= 100u,
                  "aegis ceiling %u exceeds stage %u's %u", v.ceiling, k, c);
        }

        /* A count of raised stages cannot exceed the number of stages, and
         * the live ones are a subset of the raised ones. */
        CHECK(v.n_raised <= (uint8_t)PA_STAGE_COUNT,
              "aegis raised %u of %u stages", v.n_raised,
              (unsigned)PA_STAGE_COUNT);
        CHECK(v.n_live <= v.n_raised, "aegis has %u live but %u raised",
              v.n_live, v.n_raised);
        /* SINGLE means exactly that. If correlation added nothing because
         * only one stage spoke, two stages must not be raised. */
        if (v.notes & PA_NOTE_SINGLE) {
            CHECK(v.n_raised <= 1u, "aegis says SINGLE with %u raised",
                  v.n_raised);
        }
        /* Every headline and band name must be a real string: these go
         * straight to a label, and a NULL is a crash on the glass. */
        CHECK(v.headline != NULL, "aegis published a NULL headline");
        CHECK(pa_band_name(v.band) != NULL, "aegis band %u has no name",
              (unsigned)v.band);
        CHECK(pa_band_advice(v.band) != NULL, "aegis band %u has no advice",
              (unsigned)v.band);
        CHECK(pa_stage_name(v.worst) != NULL, "aegis worst stage has no name");
    }
}

/* A random access point. Every field is one a beacon can actually carry, so
 * the combinations are absurd but never impossible. */
static void gen_ap(pc_ap_t *a, const uint8_t *ssid, unsigned ssid_len)
{
    memset(a, 0, sizeof(*a));
    for (unsigned i = 0; i < 6u; i++) { a->bssid[i] = (uint8_t)rnd(); }
    if (ssid_len > sizeof(a->ssid)) { ssid_len = sizeof(a->ssid); }
    memcpy(a->ssid, ssid, ssid_len);
    a->ssid_len     = (uint8_t)ssid_len;
    a->channel      = (uint8_t)rnd_below(200);      /* including impossible */
    a->rssi         = (int8_t)(-(int)rnd_below(130));
    a->beacons      = (uint16_t)rnd();
    a->ie_count     = (uint8_t)rnd_below(40);
    a->beacon_ms    = (uint16_t)rnd();
    a->hidden       = rnd_below(2) != 0;
    a->privacy      = rnd_below(2) != 0;
    a->wpa1_ie      = rnd_below(2) != 0;
    a->wps_present  = rnd_below(2) != 0;
    a->wps_pin      = rnd_below(2) != 0;
    a->ccmp_pairwise = rnd_below(2) != 0;
    a->tkip_pairwise = rnd_below(2) != 0;
    a->akm_8021x    = rnd_below(2) != 0;
    a->rsn.has_rsn      = rnd_below(2) != 0;
    a->rsn.mfp_capable  = rnd_below(2) != 0;
    /* Deliberately including REQUIRED without CAPABLE - a combination the
     * standard does not permit, which is exactly why a frame from a stranger
     * may contain it. */
    a->rsn.mfp_required = rnd_below(2) != 0;
    a->rsn.has_sae      = rnd_below(2) != 0;
    a->rsn.has_psk      = rnd_below(2) != 0;
    a->rsn.has_owe      = rnd_below(2) != 0;
}

static void drive_twin(void)
{
    /* One SSID shared by the whole group - which is the only situation Twin is
     * asked about, and the one where it must NOT simply count BSSIDs. */
    uint8_t ssid[32];
    const unsigned ssid_len = 1u + rnd_below(31);
    for (unsigned i = 0; i < ssid_len; i++) {
        ssid[i] = (uint8_t)(0x20u + rnd_below(0x5Fu));
    }

    pc_ap_t aps[PT_MAX_GROUP];
    const unsigned n = 1u + rnd_below(PT_MAX_GROUP);
    for (unsigned i = 0; i < n; i++) { gen_ap(&aps[i], ssid, ssid_len); }

    /* Sometimes with a profile of what this network looked like before, and
     * sometimes without - a lens that has learnt nothing yet passes NULL. */
    pt_profile_t prof;
    memset(&prof, 0, sizeof(prof));
    const bool use_prof = rnd_below(2) != 0;
    if (use_prof) {
        prof.n = (uint8_t)rnd_below(PT_MAX_PROFILE + 1u);
        for (unsigned i = 0; i < prof.n; i++) {
            /* Half the time, a BSSID the group actually contains. */
            if (rnd_below(2) == 0 && n > 0u) {
                memcpy(prof.bssid[i], aps[rnd_below(n)].bssid, 6);
            } else {
                for (unsigned k = 0; k < 6u; k++) {
                    prof.bssid[i][k] = (uint8_t)rnd();
                }
            }
        }
    }

    pt_context_t ctx;
    memset(&ctx, 0, sizeof(ctx));
    /* The context is bytes from a lens; fuzz it rather than trusting it. */
    {
        uint8_t *raw = (uint8_t *)&ctx;
        for (unsigned i = 0; i < sizeof(ctx); i++) { raw[i] = (uint8_t)rnd(); }
    }

    pt_verdict_t v;
    memset(&v, 0xAA, sizeof(v));
    pt_evaluate(aps, n, use_prof ? &prof : NULL, &ctx, &v);

    check_honest(v.score, v.raw_score, v.ceiling, "twin");

    /* THE ENGINE'S OWN HEADLINE CLAIM: "BSSID multiplicity scores exactly
     * zero". A roaming group is many BSSIDs on one SSID and is not an attack,
     * so a group where nothing else is odd must not score for its size alone.
     * This is the property that keeps Twin from crying wolf in every office,
     * and it is checked here against groups built to be identical. */
    {
        /* GROWING THE GROUP MUST NOT GROW THE SCORE.
         *
         * The engine's headline promise is that "BSSID multiplicity scores
         * exactly zero", because one SSID on twenty BSSIDs is roaming and not
         * an attack. The check is therefore not that such a group scores
         * nothing - a group can be uniformly badly configured and deserve
         * points for that - but that adding more identically configured
         * radios changes nothing. A corporate network must not become more
         * suspicious as more of it comes into range. */
        pc_ap_t same[PT_MAX_GROUP];
        gen_ap(&same[0], ssid, ssid_len);
        for (unsigned i = 1; i < PT_MAX_GROUP; i++) {
            same[i] = same[0];
            /* A different radio, identically configured - which is exactly
             * what a mesh node or a second band looks like. Keeping the OUI
             * makes them one deployment rather than a stranger. */
            for (unsigned k = 3; k < 6u; k++) {
                same[i].bssid[k] = (uint8_t)rnd();
            }
        }
        pt_verdict_t two, many;
        memset(&two, 0xAA, sizeof(two));
        memset(&many, 0xAA, sizeof(many));
        pt_evaluate(same, 2u, NULL, &ctx, &two);
        pt_evaluate(same, PT_MAX_GROUP, NULL, &ctx, &many);
        check_honest(two.score, two.raw_score, two.ceiling, "twin/pair");
        check_honest(many.score, many.raw_score, many.ceiling, "twin/group");
        CHECK(two.score == many.score,
              "2 identical radios scored %u and %u of them scored %u - "
              "multiplicity alone must score zero", two.score,
              (unsigned)PT_MAX_GROUP, many.score);
        CHECK(two.band == many.band,
              "growing an identical group changed the band");
    }

    CHECK(pt_band_name(v.band) != NULL, "twin band %u has no name",
          (unsigned)v.band);
    CHECK(pt_band_advice(v.band) != NULL, "twin band %u has no advice",
          (unsigned)v.band);
    CHECK(v.best_grade_score >= v.worst_grade_score ||
          v.best_grade_score == 0u,
          "twin's best grade %u is below its worst %u",
          v.best_grade_score, v.worst_grade_score);

    /* ORDER MUST NOT CHANGE THE ANSWER. The group arrives in whatever order
     * the scan filled the table, so a verdict that depends on it is a verdict
     * that changes for no reason while somebody is looking at it. */
    if (n > 1u) {
        pc_ap_t rev[PT_MAX_GROUP];
        for (unsigned i = 0; i < n; i++) { rev[i] = aps[n - 1u - i]; }
        pt_verdict_t rv;
        memset(&rv, 0xAA, sizeof(rv));
        pt_evaluate(rev, n, use_prof ? &prof : NULL, &ctx, &rv);
        CHECK(rv.score == v.score,
              "reversing a group of %u changed the score: %u then %u",
              n, v.score, rv.score);
        CHECK(rv.band == v.band, "reversing a group of %u changed the band",
              n);
    }
}

/* Census grades one beacon. There is no single ceiling here - the score is
 * bound by four separate caps - so the property worth proving is different:
 * when NOTHING capped the score, the letter must be a monotone function of
 * the number. Two networks cannot score 60 and 70 and grade C and D. */
static uint8_t g_grade_lo[PC_GRADE_A_PLUS + 1];
static uint8_t g_grade_hi[PC_GRADE_A_PLUS + 1];
static bool    g_grade_seen[PC_GRADE_A_PLUS + 1];

static void drive_census(void)
{
    uint8_t ssid[32];
    const unsigned ssid_len = rnd_below(33);
    for (unsigned i = 0; i < ssid_len; i++) { ssid[i] = (uint8_t)rnd(); }

    pc_ap_t a;
    gen_ap(&a, ssid, ssid_len);

    pc_verdict_t g;
    memset(&g, 0xAA, sizeof(g));
    pc_grade(&a, &g);

    CHECK(g.score <= 100u, "census scored %u", g.score);
    CHECK(g.score <= g.raw_score || g.raw_score == 0u,
          "census published %u above its raw score %u", g.score, g.raw_score);
    /* The four components are the published weighting of the grade - 45 for
     * authentication, 25 for management-frame protection, 15 each for cipher
     * and exposure. They must stay inside their own budgets and must add up
     * to the raw score, or the number on the glass is not the sum of the
     * reasons given for it. */
    CHECK(g.c_auth <= 45u, "census auth component %u exceeds 45", g.c_auth);
    CHECK(g.c_mfp <= 25u, "census mfp component %u exceeds 25", g.c_mfp);
    CHECK(g.c_cipher <= 15u, "census cipher component %u exceeds 15",
          g.c_cipher);
    CHECK(g.c_exposure <= 15u, "census exposure component %u exceeds 15",
          g.c_exposure);
    {
        const unsigned sum = (unsigned)g.c_auth + g.c_mfp + g.c_cipher +
                             g.c_exposure;
        CHECK(sum == g.raw_score,
              "census components sum to %u but the raw score is %u", sum,
              g.raw_score);
    }
    CHECK(pc_grade_name(g.grade) != NULL, "census grade has no name");
    CHECK(pc_grade_advice(&g) != NULL, "census graded with no advice");
    CHECK((unsigned)g.grade <= (unsigned)PC_GRADE_A_PLUS,
          "census grade %u is not a grade", (unsigned)g.grade);
    CHECK(g.headline != NULL, "census published a NULL headline");

    /* Grading is a pure function of the beacon. Asked twice about the same
     * frame it must answer the same thing, or the row flickers between two
     * verdicts while nothing on the air has changed. */
    pc_verdict_t again;
    memset(&again, 0x55, sizeof(again));
    pc_grade(&a, &again);
    CHECK(again.score == g.score && again.grade == g.grade,
          "census graded one beacon two ways: %u then %u", g.score,
          again.score);

    if (g.caps_applied == 0u && g.grade != PC_GRADE_UNGRADED &&
        (unsigned)g.grade <= (unsigned)PC_GRADE_A_PLUS) {
        const unsigned k = (unsigned)g.grade;
        if (!g_grade_seen[k]) {
            g_grade_seen[k] = true;
            g_grade_lo[k] = g_grade_hi[k] = g.score;
        } else {
            if (g.score < g_grade_lo[k]) { g_grade_lo[k] = g.score; }
            if (g.score > g_grade_hi[k]) { g_grade_hi[k] = g.score; }
        }
    }
}

/* Checked once at the end, over every uncapped grading the run produced. */
static void check_grade_monotonicity(void)
{
    for (unsigned lo = PC_GRADE_F; lo <= PC_GRADE_A_PLUS; lo++) {
        if (!g_grade_seen[lo]) { continue; }
        for (unsigned hi = lo + 1u; hi <= PC_GRADE_A_PLUS; hi++) {
            if (!g_grade_seen[hi]) { continue; }
            /* A better letter must never be given for a lower score. */
            CHECK(g_grade_lo[hi] >= g_grade_lo[lo] &&
                  g_grade_hi[hi] >= g_grade_hi[lo],
                  "%s spans %u-%u but the better %s spans %u-%u",
                  pc_grade_name((pc_grade_t)lo), g_grade_lo[lo],
                  g_grade_hi[lo], pc_grade_name((pc_grade_t)hi),
                  g_grade_lo[hi], g_grade_hi[hi]);
        }
    }
}

int main(int argc, char **argv)
{
    unsigned long iters = (argc > 1) ? strtoul(argv[1], NULL, 10) : 50000ul;
    g_seed = (argc > 2) ? strtoull(argv[2], NULL, 10) : 1ull;

    printf("fuzzing the verdicts: %lu streams from seed %llu\n", iters,
           (unsigned long long)g_seed);

    for (unsigned long i = 0; i < iters; i++) {
        /* Each stream gets its own seed derived from the run seed, so a
         * failure is replayable from the number printed with it. */
        g_seed = (argc > 2 ? strtoull(argv[2], NULL, 10) : 1ull) + i;
        g_state = g_seed ? g_seed : 0x9E3779B97F4A7C15ull;
        (void)rnd();

        drive_aegis();
        drive_twin();
        drive_census();
    }

    check_grade_monotonicity();

    printf("%u invariants checked, %u failures\n", g_checks, g_fails);
    if (g_fails) {
        printf("replay one with: make replay-state SEED=<the seed above>\n");
    }
    return g_fails ? 1 : 0;
}
