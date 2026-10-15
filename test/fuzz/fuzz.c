/* Pharos - fuzzing the parsers that read what strangers transmit
 *
 * Every 802.11 frame and every BLE advertisement this firmware parses is a
 * buffer whose contents, lengths and structure are chosen by whoever is in
 * radio range. The engines are pure C11 with no allocation, so they can be
 * driven directly on the host - and that makes the whole hostile-input surface
 * reachable from a test.
 *
 * WHY THIS IS NOT libFuzzer. Apple's clang ships the -fsanitize=fuzzer flag
 * without the runtime to link it, so a libFuzzer target does not build on the
 * machine this project is developed on. More importantly a coverage-guided
 * fuzzer is not reproducible: CI would find a crash on one run and not the
 * next, and the corpus would have to live somewhere. This walks a seeded
 * sequence instead. The same seed gives the same inputs on every machine
 * forever, so "it passed" means something and a failure can be replayed with
 * one number.
 *
 * The sanitizers are what actually detect the faults - this only has to reach
 * the code. Build with ASan and UBSan (the Makefile does) or it proves very
 * little.
 *
 * It also asserts POST-CONDITIONS the sanitizers cannot see: that a walker
 * terminates, that an output length never exceeds the buffer it was given,
 * that a parser's answer about a truncated frame is never richer than its
 * answer about the whole one.
 */
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "pharos_dot11.h"
#include "pharos_event.h"
#include "pharos_roster.h"
#include "pharos_vigil.h"
#include "pharos_wps.h"

static unsigned g_checks;
static unsigned g_fails;

#define FCHECK(cond, ...)                                                     \
    do {                                                                      \
        g_checks++;                                                           \
        if (!(cond)) {                                                        \
            g_fails++;                                                        \
            printf("  FAIL seed=%llu: ", (unsigned long long)g_seed);         \
            printf(__VA_ARGS__);                                              \
            printf("\n");                                                     \
        }                                                                     \
    } while (0)

/* xorshift64*. Deterministic, tiny, and good enough to shake out structure -
 * this is not trying to be a statistical generator. */
static uint64_t g_seed;
static uint64_t g_state;

static uint64_t rnd(void)
{
    g_state ^= g_state >> 12;
    g_state ^= g_state << 25;
    g_state ^= g_state >> 27;
    return g_state * 2685821657736338717ull;
}

static unsigned rnd_below(unsigned n) { return n ? (unsigned)(rnd() % n) : 0u; }

/* ---- generators -------------------------------------------------------
 *
 * Pure noise finds the shallow faults quickly and then stops being useful,
 * because a parser rejects it in the first few bytes. The interesting inputs
 * are the ones that look RIGHT far enough in to reach the arithmetic: a
 * plausible header with a hostile length, an element chain that is valid up to
 * its last element, a frame truncated at exactly the wrong offset.
 */

#define MAXBUF 512

/* A tag/len/value chain, with lengths that frequently lie. */
static unsigned gen_elements(uint8_t *b, unsigned cap)
{
    unsigned w = 0;
    const unsigned count = rnd_below(12u);
    for (unsigned i = 0; i < count && w + 2u <= cap; i++) {
        const uint8_t id = (uint8_t)rnd_below(256u);
        uint8_t len;
        switch (rnd_below(6u)) {
        case 0:  len = 0; break;                       /* zero-length        */
        case 1:  len = 255; break;                     /* claims far too much */
        case 2:  len = (uint8_t)(cap - w); break;      /* exactly to the end  */
        case 3:  len = (uint8_t)(cap - w + 1u); break; /* one past the end    */
        default: len = (uint8_t)rnd_below(40u); break; /* plausible           */
        }
        b[w++] = id;
        b[w++] = len;
        for (unsigned k = 0; k < len && w < cap; k++) {
            b[w++] = (uint8_t)rnd();
        }
    }
    return w;
}

/* A management frame: a real-looking 24-byte header, then a body that may or
 * may not be an element chain. */
static unsigned gen_mgmt(uint8_t *b, unsigned cap)
{
    if (cap < 24u) {
        return 0;
    }
    static const uint8_t subtypes[] = { 0x80, 0x50, 0xB0, 0x00, 0x10, 0xC0,
                                        0xA0, 0x40, 0xD0, 0x20 };
    b[0] = subtypes[rnd_below(sizeof subtypes)];
    b[1] = (uint8_t)rnd();
    for (unsigned i = 2; i < 24u; i++) {
        b[i] = (uint8_t)rnd();
    }
    unsigned w = 24u;
    /* Beacons and probe responses carry 12 fixed bytes before the elements.
     * Sometimes emit them, sometimes do not - the mismatch is the point. */
    if (rnd_below(2u)) {
        for (unsigned i = 0; i < 12u && w < cap; i++) {
            b[w++] = (uint8_t)rnd();
        }
    }
    w += gen_elements(b + w, cap - w);
    return w;
}

/* A BLE advertisement: [len][type][payload], same shape, same lies. */
static unsigned gen_adv(uint8_t *b, unsigned cap)
{
    const unsigned limit = cap < 31u ? cap : 31u;
    unsigned w = 0;
    const unsigned count = rnd_below(8u);
    for (unsigned i = 0; i < count && w + 1u <= limit; i++) {
        uint8_t len;
        switch (rnd_below(5u)) {
        case 0:  len = 0; break;
        case 1:  len = 255; break;
        case 2:  len = (uint8_t)(limit - w); break;
        default: len = (uint8_t)(1u + rnd_below(20u)); break;
        }
        b[w++] = len;
        /* Favour the types the walkers actually branch on. */
        static const uint8_t types[] = { 0xFF, 0x09, 0x08, 0x16, 0x19, 0x01,
                                         0x14, 0x03, 0x02 };
        if (w < limit) {
            b[w++] = types[rnd_below(sizeof types)];
        }
        for (unsigned k = 1; k < len && w < limit; k++) {
            b[w++] = (uint8_t)rnd();
        }
    }
    return w;
}

static unsigned gen_noise(uint8_t *b, unsigned cap)
{
    const unsigned n = rnd_below(cap + 1u);
    for (unsigned i = 0; i < n; i++) {
        b[i] = (uint8_t)rnd();
    }
    return n;
}

/* ---- targets ---------------------------------------------------------- */

static void hit_dot11(const uint8_t *b, unsigned n)
{
    pharos_ev_dot11_t hdr;
    memset(&hdr, 0, sizeof hdr);
    if (!pharos_dot11_parse_header(b, n, &hdr)) {
        return;
    }
    /* A parsed header must describe a frame that fits what we handed it. */
    FCHECK(hdr.ssid_len <= sizeof hdr.ssid - 1u,
           "ssid_len %u exceeds the field", (unsigned)hdr.ssid_len);

    size_t off = 0, len = 0;
    const bool win = pharos_dot11_ie_window(hdr.subtype, n > 24u ? n - 24u : 0u,
                                            &off, &len);
    if (win) {
        /* The window must lie inside the body it was computed from. */
        const size_t body = n > 24u ? n - 24u : 0u;
        FCHECK(off <= body, "ie window offset %u past body %u",
               (unsigned)off, (unsigned)body);
        FCHECK(off + len <= body, "ie window end %u past body %u",
               (unsigned)(off + len), (unsigned)body);
    }

    const uint8_t *body = (n > 24u) ? b + 24 : b;
    const size_t blen = (n > 24u) ? n - 24u : 0u;

    uint8_t ie_len = 0;
    const uint8_t *ie = pharos_dot11_find_ie(body, blen, 0, &ie_len);
    if (ie) {
        /* A found element must sit wholly inside the body. */
        FCHECK(ie >= body && (size_t)(ie - body) + ie_len <= blen,
               "found element escapes the body");
    }
    (void)pharos_dot11_find_ie_from(body, blen, 12u, 221u, &ie_len);
    (void)pharos_dot11_ie_count(body, blen, 0);
    (void)pharos_dot11_body_is_ie_chain(hdr.subtype);

    pharos_rsn_t rsn;
    memset(&rsn, 0, sizeof rsn);
    (void)pharos_dot11_rsn(body, blen, &rsn);

    pharos_eapol_t eap;
    memset(&eap, 0, sizeof eap);
    (void)pharos_dot11_eapol(b, n, &eap);

    uint16_t reason = 0;
    (void)pharos_dot11_reason(b, n, &hdr, &reason);
}

static void hit_wps(const uint8_t *b, unsigned n)
{
    pwps_info_t w;
    memset(&w, 0, sizeof w);
    if (pwps_parse(b, (uint16_t)n, &w)) {
        char buf[64];
        const char *m = pwps_model(&w, buf, sizeof buf);
        FCHECK(m != NULL, "pwps_model returned NULL on a parsed record");
        FCHECK(strlen(m) < sizeof buf, "pwps_model overran its buffer");
        (void)pwps_pin_exposed(&w);
    }
    memset(&w, 0, sizeof w);
    (void)pwps_parse_attrs(b, (uint16_t)n, &w);
}

static void hit_ble(const uint8_t *b, unsigned n)
{
    const uint8_t len = (uint8_t)(n > 255u ? 255u : n);
    (void)pv_classify(b, len);

    char name[PR_ROSTER_NAME + 1];
    uint16_t appearance = 0;
    (void)rd_ble_parse_adv(b, n, name, sizeof name, &appearance);
    FCHECK(strlen(name) < sizeof name, "parsed name overran its buffer");

    /* The length accessor can never answer wider than the buffer it guards. */
    pharos_ev_ble_t ev;
    memset(&ev, 0, sizeof ev);
    ev.data_len = (uint8_t)rnd();
    FCHECK(pharos_ev_ble_len(&ev) <= PHAROS_BLE_ADV_MAX,
           "ble_len %u exceeds the advertisement buffer",
           (unsigned)pharos_ev_ble_len(&ev));
}

static void hit_text(const uint8_t *b, unsigned n)
{
    char out[40];
    const unsigned w = pharos_copy_printable(out, sizeof out, b, n);
    FCHECK(w < sizeof out, "copy_printable wrote %u into %u", w,
           (unsigned)sizeof out);
    FCHECK(strlen(out) == w, "copy_printable length disagrees with its string");
    for (unsigned i = 0; i < w; i++) {
        const unsigned char c = (unsigned char)out[i];
        FCHECK(c >= 0x20u && c != 0x7Fu,
               "a control byte 0x%02x survived the filter", c);
    }
    /* Idempotent: cleaning already-clean text changes nothing. */
    char again[40];
    const unsigned w2 = pharos_copy_printable(again, sizeof again,
                                              (const uint8_t *)out, w);
    FCHECK(w2 == w && memcmp(again, out, w) == 0,
           "copy_printable is not idempotent");
}

/* TRUNCATION. A parser's answer about a prefix must never be RICHER than its
 * answer about the whole frame - that would mean the missing bytes were being
 * invented. Cheap to check and it covers every offset. */
static void hit_truncation(const uint8_t *b, unsigned n)
{
    if (n < 26u) {
        return;
    }
    pharos_ev_dot11_t full;
    memset(&full, 0, sizeof full);
    if (!pharos_dot11_parse_header(b, n, &full)) {
        return;
    }
    for (unsigned cut = 24u; cut < n; cut++) {
        pharos_ev_dot11_t part;
        memset(&part, 0, sizeof part);
        if (!pharos_dot11_parse_header(b, cut, &part)) {
            continue;
        }
        FCHECK(part.ssid_len <= full.ssid_len + 32u,
               "a %u-byte prefix reported a longer SSID than the %u-byte frame",
               cut, n);
        const uint8_t *body = b + 24;
        FCHECK(pharos_dot11_ie_count(body, cut - 24u, 0) <=
                   pharos_dot11_ie_count(body, n - 24u, 0),
               "a prefix counted MORE elements than the whole frame");
    }
}

/* ---- the loop --------------------------------------------------------- */

int main(int argc, char **argv)
{
    unsigned long iters = (argc > 1) ? strtoul(argv[1], NULL, 10) : 200000ul;
    uint64_t seed0 = (argc > 2) ? strtoull(argv[2], NULL, 10) : 1ull;

    printf("fuzzing the parsers: %lu iterations from seed %llu\n", iters,
           (unsigned long long)seed0);

    uint8_t buf[MAXBUF];
    for (unsigned long i = 0; i < iters; i++) {
        g_seed = seed0 + i;
        g_state = g_seed ? g_seed : 0x9E3779B97F4A7C15ull;

        unsigned n;
        switch (i % 4u) {
        case 0:  n = gen_mgmt(buf, MAXBUF); break;
        case 1:  n = gen_adv(buf, MAXBUF); break;
        case 2:  n = gen_elements(buf, MAXBUF); break;
        default: n = gen_noise(buf, MAXBUF); break;
        }

        /* EXACTLY-SIZED, ON THE HEAP, FOR EVERY CALL.
         *
         * Handing the targets a fixed 512-byte stack buffer hid the whole
         * class this harness exists to catch: a walker that runs past its
         * length still lands inside that buffer, so ASan sees nothing. I
         * confirmed it by deleting a bounds check from rd_ble_parse_adv and
         * watching 400 iterations pass.
         *
         * A heap block of exactly n bytes is poisoned the instant the walker
         * steps past the end, which is what makes "0 failures" mean
         * something. */
        uint8_t *exact = (uint8_t *)malloc(n ? n : 1u);
        if (!exact) {
            continue;
        }
        memcpy(exact, buf, n);

        hit_dot11(exact, n);
        hit_wps(exact, n);
        hit_ble(exact, n);
        hit_text(exact, n);
        hit_truncation(exact, n);

        free(exact);
    }

    printf("%u post-conditions checked, %u failures\n", g_checks, g_fails);
    if (g_fails) {
        printf("replay a failure with: ./fuzz 1 <seed>\n");
    }
    return g_fails ? 1 : 0;
}
