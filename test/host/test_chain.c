/* Pharos host tests, part six: SHA-256, the evidence chain, and KARMA. */
#include <stdlib.h>

#include "pharos_chain.h"
#include "pharos_karma.h"
#include "pharos_sha256.h"
#include "test_support.h"

/* ---------------------------------------------------------------- sha256 */

static const char *hexof(const void *data, size_t len)
{
    static char buf[65];
    uint8_t d[PHAROS_SHA256_SIZE];
    pharos_sha256(data, len, d);
    pharos_sha256_hex(d, buf);
    return buf;
}

void test_sha256(void)
{
    banner("sha256: NIST vectors");

    /* The standard vectors. If these pass, the chain below means something. */
    CHECK(strcmp(hexof("", 0),
                 "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855") == 0,
          "empty: %s", hexof("", 0));
    CHECK(strcmp(hexof("abc", 3),
                 "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad") == 0,
          "abc: %s", hexof("abc", 3));
    CHECK(strcmp(hexof("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56),
                 "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1") == 0,
          "448-bit: %s", hexof("abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56));

    /* A million 'a' - exercises the length counter past 2^20 bits. */
    {
        pharos_sha256_t c;
        pharos_sha256_init(&c);
        char chunk[1000];
        memset(chunk, 'a', sizeof(chunk));
        for (int i = 0; i < 1000; i++) {
            pharos_sha256_update(&c, chunk, sizeof(chunk));
        }
        uint8_t d[PHAROS_SHA256_SIZE];
        char hex[65];
        pharos_sha256_final(&c, d);
        pharos_sha256_hex(d, hex);
        CHECK(strcmp(hex, "cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0") == 0,
              "million a: %s", hex);
    }

    /* Streaming in arbitrary chunk sizes must equal the one-shot digest: the
     * chain feeds this incrementally, so a buffering bug would be silent. */
    const char *msg = "the quick brown fox jumps over the lazy dog, repeatedly and at length";
    const size_t len = strlen(msg);
    uint8_t once[PHAROS_SHA256_SIZE];
    pharos_sha256(msg, len, once);
    for (size_t chunk = 1; chunk <= len; chunk++) {
        pharos_sha256_t c;
        pharos_sha256_init(&c);
        for (size_t off = 0; off < len; off += chunk) {
            const size_t n = (off + chunk > len) ? (len - off) : chunk;
            pharos_sha256_update(&c, msg + off, n);
        }
        uint8_t d[PHAROS_SHA256_SIZE];
        pharos_sha256_final(&c, d);
        CHECK(memcmp(d, once, PHAROS_SHA256_SIZE) == 0,
              "streaming at chunk %zu matches one-shot", chunk);
    }

    /* Block-boundary lengths, where padding logic goes wrong if it is going
     * to go wrong at all. */
    for (size_t n = 54; n <= 66; n++) {
        char b[80];
        memset(b, 'x', sizeof(b));
        uint8_t a1[PHAROS_SHA256_SIZE], a2[PHAROS_SHA256_SIZE];
        pharos_sha256(b, n, a1);
        pharos_sha256_t c;
        pharos_sha256_init(&c);
        pharos_sha256_update(&c, b, n / 2);
        pharos_sha256_update(&c, b + n / 2, n - n / 2);
        pharos_sha256_final(&c, a2);
        CHECK(memcmp(a1, a2, PHAROS_SHA256_SIZE) == 0, "boundary len %zu", n);
    }
}

/* ----------------------------------------------------------------- chain */

static void build_chain(phc_chain_t *c, phc_record_t *recs, unsigned *n)
{
    phc_open(c, "pharos-test-device", 18, 0xDEADBEEFCAFEull);
    unsigned i = 0;
    phc_commit(c, PHC_KIND_SESSION_OPEN, 1000, "open", 4, &recs[i++]);
    phc_commit(c, PHC_KIND_FENCE_STATUS, 2000, "fence:clean", 11, &recs[i++]);
    phc_commit(c, PHC_KIND_LENS_START, 3000, "wifi.watch", 10, &recs[i++]);
    phc_commit(c, PHC_KIND_VERDICT, 4000, "{\"band\":\"SUSPICIOUS\"}", 21, &recs[i++]);
    phc_commit(c, PHC_KIND_VERDICT, 5000, "{\"band\":\"FLOOD LIKELY\"}", 23, &recs[i++]);
    phc_commit(c, PHC_KIND_REPORT, 6000, "{\"aps\":[]}", 10, &recs[i++]);
    phc_close(c, 7000, &recs[i++]);
    *n = i;
}

void test_chain(void)
{
    banner("chain: tamper-evident evidence");
    phc_chain_t c;
    phc_record_t recs[16];
    unsigned n = 0;
    unsigned bad = 0;

    build_chain(&c, recs, &n);
    CHECK_EQ(n, 7);
    CHECK_EQ(c.seq, 7);
    CHECK(!c.open, "closed after phc_close");
    CHECK(phc_verify(c.root, recs, n, &bad), "an untouched chain verifies");
    CHECK_EQ(bad, n);

    /* The root must depend on both the nonce and the device identity, or two
     * devices in one room would produce interchangeable chains. */
    phc_chain_t a, b;
    phc_open(&a, "device-A", 8, 42);
    phc_open(&b, "device-B", 8, 42);
    CHECK(memcmp(a.root, b.root, PHAROS_SHA256_SIZE) != 0, "device id changes the root");
    phc_open(&b, "device-A", 8, 43);
    CHECK(memcmp(a.root, b.root, PHAROS_SHA256_SIZE) != 0, "nonce changes the root");
    phc_open(&b, "device-A", 8, 42);
    CHECK(memcmp(a.root, b.root, PHAROS_SHA256_SIZE) == 0, "same inputs, same root");

    /* MODIFICATION: change one byte of one payload hash. */
    {
        phc_record_t t[16];
        memcpy(t, recs, sizeof(recs));
        t[3].payload_hash[0] ^= 0x01;
        CHECK(!phc_verify(c.root, t, n, &bad), "a modified payload is caught");
        CHECK_EQ(bad, 3);
    }
    /* ...and changing the recorded head to match does not save it, because
     * the next link no longer reproduces. */
    {
        phc_record_t t[16];
        memcpy(t, recs, sizeof(recs));
        t[3].payload_hash[0] ^= 0x01;
        phc_link(t[2].head, &t[3], t[3].head); /* forge this link */
        CHECK(!phc_verify(c.root, t, n, &bad), "re-linking one record is not enough");
        CHECK_EQ(bad, 4);
    }

    /* DELETION: lift a record out of the middle. */
    {
        phc_record_t t[16];
        unsigned m = 0;
        for (unsigned i = 0; i < n; i++) {
            if (i == 2) continue;
            t[m++] = recs[i];
        }
        CHECK(!phc_verify(c.root, t, m, &bad), "a deleted record is caught");
        CHECK_EQ(bad, 2);
    }

    /* TRUNCATION: drop the tail. The remaining prefix still verifies - that
     * is inherent to a hash chain - which is exactly why the head digest must
     * be published somewhere the holder of the file does not control. */
    {
        CHECK(phc_verify(c.root, recs, n - 2, &bad),
              "a truncated prefix verifies on its own");
        CHECK(memcmp(recs[n - 3].head, recs[n - 1].head, PHAROS_SHA256_SIZE) != 0,
              "but its head differs from the published one, which is the tell");
    }

    /* INSERTION: splice a fabricated record in. */
    {
        phc_record_t t[16];
        memcpy(t, recs, sizeof(recs));
        for (unsigned i = n; i > 4; i--) {
            t[i] = t[i - 1];
        }
        memset(&t[4], 0, sizeof(t[4]));
        t[4].seq = 4;
        t[4].t_us = 4500;
        t[4].kind = PHC_KIND_OPERATOR_NOTE;
        pharos_sha256("fabricated", 10, t[4].payload_hash);
        phc_link(t[3].head, &t[4], t[4].head);
        CHECK(!phc_verify(c.root, t, n + 1, &bad), "an inserted record is caught");
    }

    /* REORDER: swap two adjacent records. */
    {
        phc_record_t t[16];
        memcpy(t, recs, sizeof(recs));
        phc_record_t tmp = t[3]; t[3] = t[4]; t[4] = tmp;
        CHECK(!phc_verify(c.root, t, n, &bad), "a reordered chain is caught");
    }

    /* A different root must not verify the same records. */
    {
        phc_chain_t other;
        phc_open(&other, "someone-else", 12, 99);
        CHECK(!phc_verify(other.root, recs, n, &bad), "wrong root does not verify");
        CHECK_EQ(bad, 0);
    }

    /* Committing to a closed chain must fail rather than silently continue. */
    {
        phc_record_t r;
        CHECK(!phc_commit(&c, PHC_KIND_VERDICT, 9000, "late", 4, &r),
              "a closed chain accepts nothing further");
    }

    /* The payload hash is a real commitment: same bytes, same hash. */
    {
        phc_chain_t x, y;
        phc_record_t rx, ry;
        phc_open(&x, "d", 1, 7);
        phc_open(&y, "d", 1, 7);
        phc_commit(&x, PHC_KIND_VERDICT, 100, "payload", 7, &rx);
        phc_commit(&y, PHC_KIND_VERDICT, 100, "payload", 7, &ry);
        CHECK(memcmp(rx.head, ry.head, PHAROS_SHA256_SIZE) == 0, "deterministic");
        phc_commit(&y, PHC_KIND_VERDICT, 100, "payloae", 7, &ry);
        CHECK(memcmp(rx.head, ry.head, PHAROS_SHA256_SIZE) != 0, "one byte changes it");
    }

    for (int k = PHC_KIND_SESSION_OPEN; k <= PHC_KIND_SESSION_CLOSE; k++) {
        CHECK(phc_kind_name((phc_kind_t)k)[0] != '?', "kind %d named", k);
    }
}

/* ----------------------------------------------------------------- karma */

static const uint8_t AP_HONEST[6] = { 0xAC, 0x11, 0x22, 0x00, 0x00, 0x01 };
static const uint8_t AP_ROGUE[6]  = { 0x02, 0x66, 0x6E, 0x00, 0x00, 0x02 };

static void probe_and_answer(pk_engine_t *e, const uint8_t *bssid, const char *ssid,
                             uint64_t t, bool prompt)
{
    const uint8_t len = (uint8_t)strlen(ssid);
    pk_observe_probe(e, ssid, len, t);
    pk_observe_response(e, bssid, ssid, len, -45, 6, prompt ? t + 100000 : t + 5000000);
}

void test_karma(void)
{
    banner("karma: a radio that answers to any name");
    pk_context_t camped = { .dwell_permil = 1000, .bus_yield_permil = 1000 };
    pk_verdict_t v;
    pk_engine_t e;

    /* Nothing heard at all. */
    pk_reset(&e);
    pk_evaluate(&e, &camped, &v);
    CHECK_EQ(v.band, PK_BAND_NORMAL);
    CHECK(v.notes & PK_NOTE_NO_PROBES, "no probes disclosed");
    CHECK_EQ(v.score, 0);

    /* THE false positive to defeat: a corporate AP carrying four networks. It
     * answers for all of them - and beacons all of them. Must score ~nothing. */
    pk_reset(&e);
    static const char *const carried[] = { "Acme-Staff", "Acme-Guest", "Acme-IoT", "Acme-Voice" };
    for (unsigned round = 0; round < 6; round++) {
        for (unsigned i = 0; i < 4; i++) {
            pk_observe_beacon(&e, AP_HONEST, carried[i], (uint8_t)strlen(carried[i]),
                              -50, 6, 1000000ull * round);
        }
    }
    for (unsigned i = 0; i < 4; i++) {
        probe_and_answer(&e, AP_HONEST, carried[i], 7000000ull + i, true);
    }
    pk_evaluate(&e, &camped, &v);
    CHECK_EQ(v.band, PK_BAND_NORMAL);
    CHECK_EQ(v.unannounced, 0);
    CHECK_EQ(v.answered_ssids, 4);
    CHECK(v.notes & PK_NOTE_MULTI_SSID, "recognised as a multi-SSID deployment");
    CHECK(v.score < 20, "a legitimate multi-SSID AP must not alarm (got %u)", v.score);

    /* The attack: a radio that answers for whatever it hears asked for, and
     * announces none of it. */
    pk_reset(&e);
    pk_observe_beacon(&e, AP_HONEST, "Acme-Staff", 10, -50, 6, 1000);
    static const char *const asked[] = {
        "HomeNet_5G", "Starbucks WiFi", "Heathrow_Free", "TheRobinsons", "eduroam"
    };
    for (unsigned i = 0; i < 5; i++) {
        probe_and_answer(&e, AP_ROGUE, asked[i], 2000000ull + i * 200000ull, true);
    }
    pk_evaluate(&e, &camped, &v);
    CHECK_EQ(v.band, PK_BAND_KARMA_LIKELY);
    CHECK(memcmp(v.suspect, AP_ROGUE, 6) == 0, "the rogue is identified");
    CHECK_EQ(v.unannounced, 5);
    CHECK(v.families & PK_FAM_ABSENCE, "absence family");
    CHECK(v.families & PK_FAM_BREADTH, "breadth family");
    CHECK(v.families & PK_FAM_ECHO, "echo family");
    CHECK(v.notes & PK_NOTE_LOCAL_MAC, "software address noted");
    CHECK(v.score <= v.ceiling, "within ceiling");

    /* Same traffic, hopping receiver: "it never beaconed that" is exactly the
     * claim a hopping receiver is least entitled to make, so it must not alarm. */
    pk_context_t hopping = { .dwell_permil = 71, .bus_yield_permil = 1000 };
    pk_verdict_t vh;
    pk_evaluate(&e, &hopping, &vh);
    CHECK(vh.band < PK_BAND_KARMA_LIKELY,
          "hopping cannot raise a karma alarm (got %s)", pk_band_name(vh.band));
    CHECK(vh.notes & PK_NOTE_THIN_DWELL, "thin dwell disclosed");
    CHECK(vh.score < v.score, "camping earns more on identical traffic");
    CHECK(pk_ceiling(&hopping) < pk_ceiling(&camped), "ceiling follows dwell");
    CHECK(pk_ceiling(&camped) < 100, "nothing here is certain");

    /* Breadth without absence: a radio answering for two names it *does*
     * beacon is a normal small deployment, not an attack. */
    pk_reset(&e);
    pk_observe_beacon(&e, AP_HONEST, "Net-A", 5, -50, 6, 1000);
    pk_observe_beacon(&e, AP_HONEST, "Net-B", 5, -50, 6, 2000);
    probe_and_answer(&e, AP_HONEST, "Net-A", 3000000, true);
    probe_and_answer(&e, AP_HONEST, "Net-B", 3100000, true);
    pk_evaluate(&e, &camped, &v);
    CHECK(!(v.families & PK_FAM_ABSENCE), "no absence when everything is announced");
    CHECK(v.score <= 44, "without the absence family it cannot pass MIXED");

    /* A slow answer is not an echo: corroboration must be time-bounded. */
    pk_reset(&e);
    for (unsigned i = 0; i < 5; i++) {
        probe_and_answer(&e, AP_ROGUE, asked[i], 2000000ull + i * 200000ull, false);
    }
    pk_evaluate(&e, &camped, &v);
    CHECK_EQ(v.echoed, 0);
    CHECK(!(v.families & PK_FAM_ECHO), "a late answer does not count as an echo");

    /* Wildcard probes name nothing and must not be treated as an ask. */
    pk_reset(&e);
    pk_observe_probe(&e, NULL, 0, 1000);
    pk_observe_probe(&e, "", 0, 2000);
    pk_observe_response(&e, AP_ROGUE, "Anything", 8, -40, 6, 1100);
    pk_evaluate(&e, &camped, &v);
    CHECK_EQ(v.echoed, 0);
    CHECK_EQ(e.probes_seen, 2);

    /* Table limits are graceful. */
    pk_reset(&e);
    for (unsigned i = 0; i < PK_MAX_SSIDS + 6; i++) {
        char name[24];
        snprintf(name, sizeof(name), "net-%02u", i);
        probe_and_answer(&e, AP_ROGUE, name, 1000000ull + i * 10000ull, true);
    }
    pk_evaluate(&e, &camped, &v);
    CHECK(e.responders[0].overflow, "overflow recorded, not silently dropped");
    CHECK(v.notes & PK_NOTE_TABLE_FULL, "overflow surfaced in the verdict");
    CHECK_EQ(v.band, PK_BAND_KARMA_LIKELY);

    /* Vocabulary. */
    for (int b = PK_BAND_NORMAL; b <= PK_BAND_KARMA_LIKELY; b++) {
        const char *nm = pk_band_name((pk_band_t)b);
        const char *ad = pk_band_advice((pk_band_t)b);
        CHECK(nm && *nm && ad && *ad, "band %d described", b);
        CHECK(strstr(nm, "SAFE") == NULL, "no band claims safety");
        CHECK(strstr(ad, " is safe") == NULL, "advice never says safe");
    }
}
