/* Pharos host tests - Rival, the lens that looks for TOOLS rather than attacks.
 *
 * The dangerous failure mode here is not missing a Flipper. It is calling
 * somebody's headphones an attack tool: this lens points at PEOPLE in a room,
 * and a false positive is an accusation. So most of what follows is the list
 * of things it must refuse to flag.
 */
#include <string.h>

#include "pharos_rival.h"
#include "test_support.h"

#define T0 1000000ull

static const uint8_t A1[6] = { 0x0C, 0xFA, 0x22, 0x11, 0x22, 0x33 };
static const uint8_t A2[6] = { 0x02, 0x11, 0x22, 0x33, 0x44, 0x55 };

static void test_rival_names(void)
{
    banner("rival: what announces itself, and what must never be flagged");

    CHECK_EQ(prv_classify_name("Flipper Zaqp", 12, true), PRV_KIND_FLIPPER);
    /* Case must not matter - firmware forks rename things. */
    CHECK_EQ(prv_classify_name("flipper bob", 11, true), PRV_KIND_FLIPPER);
    CHECK_EQ(prv_classify_name("pwnagotchi_ab12", 15, false), PRV_KIND_PWNAGOTCHI);
    CHECK_EQ(prv_classify_name("Pineapple_1234", 14, false), PRV_KIND_PINEAPPLE);
    CHECK_EQ(prv_classify_name("ESP32-Marauder", 14, true), PRV_KIND_DEAUTHER);
    CHECK_EQ(prv_classify_name("HC-05", 5, true), PRV_KIND_SERIAL_BRIDGE);
    CHECK_EQ(prv_classify_name("ESP32_1A2B", 10, true), PRV_KIND_DEV_BOARD);

    /* THE REFUSALS. Every one of these is a real product name that a naive
     * substring match would flag, and flagging one is an accusation aimed at
     * whoever is holding it. */
    static const char *innocent[] = {
        "JBL Flip 5",        /* "Flip" is not "Flipper"            */
        "Flip Phone",
        "AirPods Pro",
        "Galaxy Buds",
        "MX Master 3",
        "Bose QC45",
        "Tile",
        "My Passport",
        "Pixel Buds",
        "Apple Watch",
    };
    for (unsigned i = 0; i < sizeof(innocent) / sizeof(innocent[0]); i++) {
        const prv_kind_t k =
            prv_classify_name(innocent[i], (uint8_t)strlen(innocent[i]), true);
        CHECK(k == PRV_KIND_NONE, "\"%s\" must not be flagged (got %s)",
              innocent[i], prv_kind_name(k));
    }

    /* A Flipper name over WI-FI is not a Flipper: the Bluetooth name is the
     * signal, and an access point somebody called "Flipper" is a network. */
    CHECK_EQ(prv_classify_name("Flipper Zaqp", 12, false), PRV_KIND_NONE);

    /* Empty and NULL must be safe. */
    CHECK_EQ(prv_classify_name(NULL, 0, true), PRV_KIND_NONE);
    CHECK_EQ(prv_classify_name("", 0, true), PRV_KIND_NONE);
}

/* Owning a tool is not an offence. A Flipper sitting in somebody's pocket must
 * be reported and must NOT alarm, or the operator learns to ignore the lens. */
static void test_rival_presence_is_capped(void)
{
    banner("rival: presence alone never reaches the alarm band");
    prv_state_t s;
    prv_verdict_t v;
    prv_reset(&s);

    for (int i = 0; i < 40; i++) {
        prv_observe_ble(&s, A1, "Flipper Zaqp", -40,
                        T0 + (uint64_t)i * 1000000ull);
    }
    prv_evaluate(&s, T0 + 40000000ull, &v);

    CHECK_EQ(v.worst_kind, PRV_KIND_FLIPPER);
    CHECK_EQ(v.n_flipper, 1);
    CHECK(v.families & PRV_FAM_PRESENT, "presence is reported");
    CHECK((v.families & PRV_FAM_ACTIVE) == 0, "nothing is being DONE with it");
    CHECK(v.score <= PRV_CAP_PRESENCE_ONLY,
          "presence is capped at %u (got %u)", PRV_CAP_PRESENCE_ONLY, v.score);
    CHECK(v.band < PRV_BAND_ACTIVE, "and it does not alarm");
}

/* The one thing that DOES justify raising a voice: an advertisement flood,
 * which is a burst of distinct advertisers no ordinary room produces. */
static void test_rival_spam_is_active(void)
{
    banner("rival: an advertisement flood is something being DONE");
    prv_state_t s;
    prv_verdict_t v;
    prv_reset(&s);

    uint8_t addr[6] = { 0x02, 0, 0, 0, 0, 0 };
    for (int i = 0; i < 120; i++) {
        addr[4] = (uint8_t)(i >> 8);
        addr[5] = (uint8_t)i;
        /* Spam advertisers carry junk names, not tool names. */
        prv_observe_ble(&s, addr, NULL, -50, T0 + (uint64_t)i * 5000ull);
    }
    prv_evaluate(&s, T0 + 1000000ull, &v);

    CHECK(v.peak_adv_per_s >= 60, "the flood was counted (%u/s)", v.peak_adv_per_s);
    CHECK(v.notes & PRV_NOTE_SPAM, "and named as spam");
    CHECK(v.families & PRV_FAM_ACTIVE, "which is the ACTIVE family");
    CHECK_EQ(v.band, PRV_BAND_ACTIVE);
}

/* An ordinary busy room must not read as a flood. */
static void test_rival_busy_room_is_not_a_flood(void)
{
    banner("rival: a busy room is not an advertisement flood");
    prv_state_t s;
    prv_verdict_t v;
    prv_reset(&s);

    uint8_t addr[6] = { 0x02, 0, 0, 0, 0, 0 };
    /* Twelve phones and earbuds, each advertising a few times a second. */
    for (int sec = 0; sec < 6; sec++) {
        for (int d = 0; d < 12; d++) {
            addr[5] = (uint8_t)d;
            prv_observe_ble(&s, addr, "AirPods Pro", -60,
                            T0 + (uint64_t)sec * 1000000ull + (uint64_t)d * 20000ull);
        }
    }
    prv_evaluate(&s, T0 + 6000000ull, &v);

    CHECK((v.notes & PRV_NOTE_SPAM) == 0, "not called spam (peak %u/s)",
          v.peak_adv_per_s);
    CHECK((v.families & PRV_FAM_ACTIVE) == 0, "and not called active");
    CHECK_EQ(v.n_devices, 0); /* AirPods are not this lens' business */
}

static void test_rival_listing_and_vocabulary(void)
{
    banner("rival: the list is capability-ordered and everything names itself");
    prv_state_t s;
    prv_verdict_t v;
    prv_reset(&s);

    prv_observe_ble(&s, A2, "ESP32_1A2B", -30, T0);          /* weak       */
    prv_observe_ble(&s, A1, "Flipper Zaqp", -80, T0 + 1000); /* capable    */
    prv_evaluate(&s, T0 + 2000000ull, &v);

    prv_device_t d;
    CHECK(prv_device_at(&s, 0, &d), "there is a first device");
    CHECK_EQ(d.kind, PRV_KIND_FLIPPER);
    CHECK(prv_device_at(&s, 1, &d), "and a second");
    CHECK_EQ(d.kind, PRV_KIND_DEV_BOARD);
    CHECK(!prv_device_at(&s, 2, &d), "and the list terminates");

    /* Deafness is on EVERY verdict, not in a footnote: a quiet screen here
     * must never be read as "there is nothing here". */
    CHECK(v.notes & PRV_NOTE_BREDR_BLIND, "classic Bluetooth deafness declared");
    CHECK(v.notes & PRV_NOTE_SUBGHZ_BLIND, "sub-GHz deafness declared");

    for (int k = 0; k < PRV_KIND_COUNT; k++) {
        CHECK(prv_kind_name((prv_kind_t)k) != NULL, "kind %d names itself", k);
        CHECK(prv_kind_note((prv_kind_t)k) != NULL, "kind %d explains itself", k);
    }
    for (int b = 0; b < PRV_BAND_COUNT; b++) {
        const char *n = prv_band_name((prv_band_t)b);
        const char *a = prv_band_advice((prv_band_t)b);
        CHECK(n && *n, "band %d named", b);
        CHECK(a && *a, "band %d advised", b);
        CHECK(strstr(n, "HOSTILE") == NULL, "no band calls anybody hostile");
        CHECK(strstr(a, " safe") == NULL, "no band claims safety");
    }
}


/* A Pwnagotchi is NOT found by name, and the first version of this lens tried
 * to. Its advertisement is a beacon with NO SSID at all, carrying a chunked
 * JSON payload in information elements 222 and 224-226, sent from a hardcoded
 * de:ad:be:ef:de:ad. Two independent signals; either is sufficient. */
static void test_rival_pwnagotchi(void)
{
    banner("rival: a Pwnagotchi has no SSID to match on");
    static const uint8_t PWN[6]  = { 0xDE, 0xAD, 0xBE, 0xEF, 0xDE, 0xAD };
    static const uint8_t OTHER[6] = { 0x02, 0x99, 0x88, 0x77, 0x66, 0x55 };

    CHECK(prv_is_pwnagotchi_addr(PWN), "the advertisement address is known");
    CHECK(!prv_is_pwnagotchi_addr(OTHER), "and nothing else matches it");

    /* Signal one: the address, with no SSID and no whisper flag. */
    {
        prv_state_t s; prv_verdict_t v;
        prv_reset(&s);
        for (int i = 0; i < 8; i++) {
            prv_observe_beacon(&s, PWN, NULL, 0, false, -55,
                               T0 + (uint64_t)i * 500000ull);
        }
        prv_evaluate(&s, T0 + 4000000ull, &v);
        CHECK_EQ(v.n_pwnagotchi, 1);
        CHECK_EQ(v.worst_kind, PRV_KIND_PWNAGOTCHI);
    }

    /* Signal two: the whisper elements, from a DIFFERENT address - which is
     * what a fork that changed the constant would look like. */
    {
        prv_state_t s; prv_verdict_t v;
        prv_reset(&s);
        for (int i = 0; i < 8; i++) {
            prv_observe_beacon(&s, OTHER, "Sleepy", 6, true, -55,
                               T0 + (uint64_t)i * 500000ull);
        }
        prv_evaluate(&s, T0 + 4000000ull, &v);
        CHECK_EQ(v.n_pwnagotchi, 1);
        /* The unit's own name, lifted out of the whisper payload by the radio,
         * is what gets shown - a beacon with no SSID leaves the field free. */
        CHECK(strcmp(v.worst_name, "Sleepy") == 0,
              "the unit's name is carried through (got \"%s\")", v.worst_name);
    }

    /* An ORDINARY beacon from an ordinary access point must not be either. */
    {
        prv_state_t s; prv_verdict_t v;
        prv_reset(&s);
        for (int i = 0; i < 8; i++) {
            prv_observe_beacon(&s, OTHER, "Sunset12", 8, false, -55,
                               T0 + (uint64_t)i * 500000ull);
        }
        prv_evaluate(&s, T0 + 4000000ull, &v);
        CHECK_EQ(v.n_pwnagotchi, 0);
        CHECK_EQ(v.n_devices, 0);
    }

    /* The deauther's documented default access-point name. */
    {
        prv_state_t s; prv_verdict_t v;
        prv_reset(&s);
        prv_observe_beacon(&s, OTHER, "pwned", 5, false, -40, T0);
        prv_evaluate(&s, T0 + 1000000ull, &v);
        CHECK_EQ(v.worst_kind, PRV_KIND_DEAUTHER);
    }
}


/* A Flipper Zero cannot be found by name and never could: Pharos scans
 * PASSIVELY, so it sees the advertisement and never the scan response where
 * the name lives. Measured in a real room, 19 of 23 advertisers were nameless.
 *
 * What the advertisement does carry is a 128-bit service UUID whose bytes
 * identify the device and its shell colour. */
static void test_rival_flipper_advertisement(void)
{
    banner("rival: the Flipper is found by its advertised UUID, not its name");
    const char *colour = NULL;

    /* THE REAL THING, captured off a Flipper on a desk:
     *   0201 06 | 07 09 "R3ghon" | 03 02 82 30 | 02 0a 00
     * The signature is a 16-bit service UUID (AD type 0x02), NOT a 128-bit
     * one - and the unit is RENAMED, so no name match could ever fire. */
    static const uint8_t real[] = {
        0x02, 0x01, 0x06,
        0x07, 0x09, 0x52, 0x33, 0x67, 0x68, 0x6f, 0x6e, /* "R3ghon" */
        0x03, 0x02, 0x82, 0x30,                          /* UUID 0x3082 */
        0x02, 0x0a, 0x00,
    };
    CHECK_EQ(prv_classify_adv(real, sizeof(real), &colour), PRV_KIND_FLIPPER);
    CHECK(colour && strcmp(colour, "white") == 0,
          "shell colour read from the UUID (got \"%s\")", colour ? colour : "");

    /* The other two shells. */
    uint8_t adv[8] = { 0x02, 0x01, 0x06, 0x03, 0x02, 0x81, 0x30, 0x00 };
    CHECK_EQ(prv_classify_adv(adv, 7, &colour), PRV_KIND_FLIPPER);
    CHECK(strcmp(colour, "black") == 0, "black");
    adv[5] = 0x83;
    CHECK_EQ(prv_classify_adv(adv, 7, &colour), PRV_KIND_FLIPPER);
    CHECK(strcmp(colour, "transparent") == 0, "transparent");

    /* THE SAME BYTES ANYWHERE ELSE MUST NOT MATCH. Scanning the payload for
     * two loose bytes - which is what some tools do - would collide by chance
     * across a room of advertisers, and every false positive on this lens is
     * an accusation pointed at a person. Real Apple manufacturer data from the
     * same capture, doctored to contain the pair: */
    uint8_t mfg[20] = { 0 };
    mfg[0] = 17; mfg[1] = 0xFF; mfg[2] = 0x4C; mfg[3] = 0x00;
    mfg[4] = 0x82; mfg[5] = 0x30;
    CHECK_EQ(prv_classify_adv(mfg, 18, &colour), PRV_KIND_NONE);

    /* A neighbouring 16-bit UUID must not match either. */
    uint8_t near[8] = { 0x02, 0x01, 0x06, 0x03, 0x02, 0x84, 0x30, 0x00 };
    CHECK_EQ(prv_classify_adv(near, 7, &colour), PRV_KIND_NONE);

    /* A nameless ordinary advertisement is not a Flipper. */
    uint8_t plain[8] = { 0x02, 0x01, 0x06, 0x03, 0x03, 0x0F, 0x18, 0x00 };
    CHECK_EQ(prv_classify_adv(plain, 7, &colour), PRV_KIND_NONE);

    CHECK_EQ(prv_classify_adv(NULL, 0, &colour), PRV_KIND_NONE);

    /* End to end: a nameless Flipper advertisement must still be found, and
     * the colour used as its label since no name is obtainable passively. */
    prv_state_t st;
    prv_verdict_t v;
    prv_reset(&st);
    adv[5] = 0x81;
    static const uint8_t F[6] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 };
    for (int i = 0; i < 6; i++) {
        prv_observe_ble_adv(&st, F, NULL, adv, 18, -45,
                            T0 + (uint64_t)i * 500000ull);
    }
    prv_evaluate(&st, T0 + 3000000ull, &v);
    CHECK_EQ(v.n_flipper, 1);
    CHECK_EQ(v.worst_kind, PRV_KIND_FLIPPER);
    CHECK(strcmp(v.worst_name, "black") == 0,
          "labelled by shell colour when no name is obtainable (got \"%s\")",
          v.worst_name);
}

void test_rival(void)
{
    test_rival_names();
    test_rival_pwnagotchi();
    test_rival_flipper_advertisement();
    test_rival_presence_is_capped();
    test_rival_spam_is_active();
    test_rival_busy_room_is_not_a_flood();
    test_rival_listing_and_vocabulary();
}
