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


/* Build an Apple Nearby Action advertisement - the payload a phone raises a
 * pairing dialog for, and the one every BLE-spam tool broadcasts. */
static uint8_t g_pair[16];
static const uint8_t *apple_pair(uint8_t model, uint8_t *len)
{
    g_pair[0] = 0x02; g_pair[1] = 0x01; g_pair[2] = 0x06;
    g_pair[3] = 0x07;              /* length */
    g_pair[4] = 0xFF;              /* manufacturer specific */
    g_pair[5] = 0x4C; g_pair[6] = 0x00;  /* Apple, little-endian */
    g_pair[7] = 0x0F;              /* Nearby Action */
    g_pair[8] = 0x05;
    g_pair[9] = model;             /* the action/model code */
    g_pair[10] = 0x00; g_pair[11] = 0x00;
    *len = 12;
    return g_pair;
}

/* PAIRING-POPUP SPAM is the attack every one of these tools ships with, and it
 * is detected by DIVERSITY rather than volume: a real room has a few
 * accessories each advertising the one model they are, while a spammer cycles
 * through dozens of codes from rotating addresses. That distinction is what
 * keeps it quiet in an airport. */
static void test_rival_pairing_spam(void)
{
    banner("rival: pairing-popup spam is model diversity, not volume");
    uint8_t alen = 0;
    uint8_t addr[6] = { 0x02, 0, 0, 0, 0, 0 };

    /* The attack: many models, rotating addresses, in a few seconds. */
    {
        prv_state_t s; prv_verdict_t v;
        prv_reset(&s);
        for (int i = 0; i < 40; i++) {
            addr[5] = (uint8_t)i;
            const uint8_t *a = apple_pair((uint8_t)(0x01 + (i % 12)), &alen);
            prv_observe_ble_adv(&s, addr, NULL, a, alen, -45,
                                T0 + (uint64_t)i * 50000ull);
        }
        prv_evaluate(&s, T0 + 2000000ull, &v);
        CHECK(v.pair_models >= PRV_SPAM_MODELS, "model diversity seen (%u)",
              v.pair_models);
        CHECK(v.notes & PRV_NOTE_PAIR_SPAM, "named as pairing spam");
        CHECK(v.families & PRV_FAM_ACTIVE, "which is something being DONE");
        CHECK_EQ(v.band, PRV_BAND_ACTIVE);
    }

    /* THE ROOM THAT MUST STAY QUIET. Three real accessories, each advertising
     * the ONE model it actually is, as often as they like. Volume alone is not
     * the signal and a busy cafe must not read as an attack. */
    {
        prv_state_t s; prv_verdict_t v;
        prv_reset(&s);
        for (int i = 0; i < 90; i++) {
            const uint8_t which = (uint8_t)(i % 3);
            addr[5] = which;                     /* stable addresses */
            const uint8_t *a = apple_pair((uint8_t)(0x20 + which), &alen);
            prv_observe_ble_adv(&s, addr, NULL, a, alen, -55,
                                T0 + (uint64_t)i * 40000ull);
        }
        prv_evaluate(&s, T0 + 4000000ull, &v);
        CHECK(v.pair_advs >= PRV_SPAM_ADVS, "plenty of advertisements (%u)",
              v.pair_advs);
        CHECK(v.pair_models < PRV_SPAM_MODELS,
              "but only three models (%u)", v.pair_models);
        CHECK((v.notes & PRV_NOTE_PAIR_SPAM) == 0,
              "so it is NOT called spam");
        CHECK(v.band < PRV_BAND_ACTIVE, "and does not alarm");
    }

    /* Google Fast Pair carries the same kind of code and must count too. */
    {
        prv_state_t s; prv_verdict_t v;
        prv_reset(&s);
        uint8_t g[12] = { 0x02, 0x01, 0x06, 0x06, 0x16, 0x2C, 0xFE, 0, 0, 0, 0, 0 };
        for (int i = 0; i < 40; i++) {
            addr[5] = (uint8_t)i;
            g[9] = (uint8_t)(0x40 + (i % 10)); /* model byte */
            prv_observe_ble_adv(&s, addr, NULL, g, 11, -45,
                                T0 + (uint64_t)i * 50000ull);
        }
        prv_evaluate(&s, T0 + 2000000ull, &v);
        CHECK(v.notes & PRV_NOTE_PAIR_SPAM, "Fast Pair spam counted too");
    }
}


/* SWITCHING IT OFF MUST SWITCH IT OFF EVERYWHERE.
 *
 * Reported from the room: "I turned off my Flipper Zero but it still shows one
 * detected." The list had already dropped it - that path was made stale-aware
 * when this bug was fixed the first time - but the COUNT above the list had
 * not, so the screen said "hardware identified: 1" over an empty list.
 *
 * A screen that contradicts itself is worse than either half alone: the
 * operator has to pick which line to believe and has nothing to pick with. So
 * the invariant is not "the count expires" - it is that the count and the list
 * agree, always, and that is what this asserts. */
static void test_rival_switched_off_everywhere(void)
{
    banner("rival: the count and the list never disagree");
    prv_state_t s; prv_verdict_t v;
    prv_reset(&s);

    const uint8_t addr[6] = { 0x02, 0x11, 0x22, 0x33, 0x44, 0x55 };
    uint8_t alen = 0;
    /* The real capture, from the test above: a renamed unit, found by its
     * advertised UUID. */
    static const uint8_t fz[] = {
        0x02, 0x01, 0x06,
        0x07, 0x09, 0x52, 0x33, 0x67, 0x68, 0x6f, 0x6e, /* "R3ghon" */
        0x03, 0x02, 0x82, 0x30,                          /* UUID 0x3082 */
        0x02, 0x0a, 0x00,
    };
    alen = (uint8_t)sizeof(fz);
    for (int i = 0; i < 10; i++) {
        prv_observe_ble_adv(&s, addr, "R3ghon", fz, alen, -57,
                            T0 + (uint64_t)i * 200000ull);
    }
    const uint64_t last = T0 + 9ull * 200000ull;

    /* While it is on. */
    prv_evaluate(&s, last, &v);
    CHECK_EQ(v.n_devices, 1);
    CHECK_EQ(v.n_flipper, 1);
    {
        prv_device_t d;
        CHECK(prv_device_at_now(&s, 0, last, &d), "and the list has a row");
        CHECK_EQ(d.kind, PRV_KIND_FLIPPER);
    }

    /* Just inside this device's own window: still both. The window is no
     * longer a constant - it follows the cadence the device was heard at - so
     * the test asks for it rather than assuming thirty seconds. */
    const uint64_t win = prv_expiry_us(&s.dev[0]);
    const uint64_t warm = last + win - 1000000ull;
    prv_evaluate(&s, warm, &v);
    CHECK_EQ(v.n_devices, 1);
    {
        prv_device_t d;
        CHECK(prv_device_at_now(&s, 0, warm, &d), "list still has it");
    }

    /* Switched off. Past the window, BOTH must let go - and the score must
     * come back down, because the score is what raises the alarm. Listening
     * throughout: that is what makes the silence mean something. */
    const uint64_t cold = last + win + 1000000ull;
    prv_listen(&s, win + 1000000ull);
    prv_evaluate(&s, cold, &v);
    CHECK_EQ(v.n_devices, 0);
    CHECK_EQ(v.n_flipper, 0);
    CHECK_EQ(v.worst_kind, PRV_KIND_NONE);
    CHECK_EQ(v.band, PRV_BAND_CLEAR);
    {
        prv_device_t d;
        CHECK(!prv_device_at_now(&s, 0, cold, &d), "and the list is empty");
    }

    /* The general form, swept across the whole life of the sighting: at no
     * instant may the count claim hardware the list cannot show. */
    for (uint64_t t = last; t <= last + PRV_STALE_US * 2ull; t += 500000ull) {
        prv_evaluate(&s, t, &v);
        unsigned rows = 0;
        prv_device_t d;
        while (prv_device_at_now(&s, rows, t, &d)) {
            rows++;
        }
        CHECK(rows == v.n_devices,
              "count %u == list %u at t+%llus", v.n_devices, rows,
              (unsigned long long)((t - last) / 1000000ull));
    }
}

/* HOW LONG SILENCE HAS TO LAST BEFORE IT MEANS SOMETHING.
 *
 * A flat thirty seconds was the answer to the wrong question. It is the right
 * patience for the quietest thing this engine can see, and it was being spent
 * on a Flipper that had been advertising twice a second - so switching one off
 * left the screen claiming it for half a minute. Reported as "it took some time
 * to remove the Flipper Zero after I closed it".
 *
 * Silence is only evidence in proportion to how talkative the thing was. */
static void test_rival_expiry_follows_cadence(void)
{
    banner("rival: a chatty device is dropped sooner than a quiet one");

    static const uint8_t fz[] = {
        0x02, 0x01, 0x06,
        0x03, 0x02, 0x82, 0x30, /* Flipper, UUID 0x3082 */
    };
    const uint8_t addr[6] = { 0x02, 0x11, 0x22, 0x33, 0x44, 0x55 };

    /* THE CHATTY ONE: heard twice a second for a minute. */
    {
        prv_state_t s; prv_verdict_t v;
        prv_reset(&s);
        for (int i = 0; i < 120; i++) {
            /* Listening throughout, which is what the wall clock in these
             * fixtures is standing in for. Staleness is measured on the
             * listening clock now, so a test that does not advance it is
             * modelling a receiver with its ears shut. */
            prv_listen(&s, 500000ull);
            prv_observe_ble_adv(&s, addr, "R3ghon", fz, (uint8_t)sizeof(fz), -57,
                                T0 + (uint64_t)i * 500000ull);
        }
        const uint64_t last = T0 + 119ull * 500000ull;

        prv_evaluate(&s, last, &v);
        CHECK_EQ(v.n_devices, 1);

        /* Five seconds of silence from something heard twice a second is
         * eight missed advertisements over. It is gone - but only because we
         * were still listening through those five seconds. */
        prv_listen(&s, 6000000ull);
        prv_evaluate(&s, last + 6000000ull, &v);
        CHECK_EQ(v.n_devices, 0);
        prv_device_t d;
        CHECK(!prv_device_at_now(&s, 0, last + 6000000ull, &d),
              "and the list agrees");

        /* But not so twitchy that a single missed advert drops it. Re-heard,
         * so the clocks line up again. */
        prv_observe_ble_adv(&s, addr, "R3ghon", fz, (uint8_t)sizeof(fz), -57,
                            last + 6500000ull);
        prv_listen(&s, 2000000ull);
        prv_evaluate(&s, last + 8500000ull, &v);
        CHECK_EQ(v.n_devices, 1);
    }

    /* THE QUIET ONE: a beacon heard once every ten seconds. Dropping this at
     * five seconds would make it flicker in and out of the list forever, so it
     * keeps the full ceiling. This is the case the flat constant was for, and
     * it must not regress. */
    {
        prv_state_t s; prv_verdict_t v;
        prv_reset(&s);
        const uint8_t bssid[6] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xDE, 0xAD };
        for (int i = 0; i < 6; i++) {
            prv_observe_beacon(&s, bssid, NULL, 0, true, -70,
                               T0 + (uint64_t)i * 10000000ull);
        }
        const uint64_t last = T0 + 50000000ull;

        prv_listen(&s, 12000000ull);
        prv_evaluate(&s, last + 12000000ull, &v);
        CHECK_EQ(v.n_devices, 1);
        CHECK_EQ(v.n_pwnagotchi, 1);
        CHECK(prv_expiry_us(&s.dev[0]) == PRV_STALE_US,
              "a ten-second cadence keeps the full window");

        /* And it does eventually go. */
        prv_listen(&s, PRV_STALE_US + 1000000ull);
        prv_evaluate(&s, last + PRV_STALE_US + 1000000ull, &v);
        CHECK_EQ(v.n_devices, 0);
    }

    /* Too few sightings to measure a cadence at all: the patient ceiling, not
     * the twitchy floor. The less it knows, the longer it waits. */
    {
        prv_state_t s;
        prv_reset(&s);
        prv_observe_ble_adv(&s, addr, NULL, fz, (uint8_t)sizeof(fz), -60, T0);
        CHECK(prv_expiry_us(&s.dev[0]) == PRV_STALE_US,
              "one sighting is not a cadence");
        prv_observe_ble_adv(&s, addr, NULL, fz, (uint8_t)sizeof(fz), -60,
                            T0 + 100000ull);
        CHECK(prv_expiry_us(&s.dev[0]) == PRV_STALE_US,
              "and neither is two");
    }

    /* The window is bounded at both ends, whatever the cadence. */
    {
        prv_state_t s;
        prv_reset(&s);
        /* Absurdly fast: 1 ms apart. The floor must hold. */
        for (int i = 0; i < 50; i++) {
            prv_observe_ble_adv(&s, addr, NULL, fz, (uint8_t)sizeof(fz), -60,
                                T0 + (uint64_t)i * 1000ull);
        }
        const uint64_t w = prv_expiry_us(&s.dev[0]);
        CHECK(w >= PRV_STALE_MIN_US, "never twitchier than the floor");
        CHECK(w <= PRV_STALE_US, "never more patient than the ceiling");
    }

    /* AND THE INVARIANT THAT MATTERS, re-asserted against the new rule: the
     * count and the list agree at every instant, whatever window applies. */
    {
        prv_state_t s; prv_verdict_t v;
        prv_reset(&s);
        for (int i = 0; i < 60; i++) {
            prv_observe_ble_adv(&s, addr, "R3ghon", fz, (uint8_t)sizeof(fz), -57,
                                T0 + (uint64_t)i * 400000ull);
        }
        const uint64_t last = T0 + 59ull * 400000ull;
        for (uint64_t t = last; t <= last + 40000000ull; t += 500000ull) {
            prv_evaluate(&s, t, &v);
            unsigned rows = 0;
            prv_device_t d;
            while (prv_device_at_now(&s, rows, t, &d)) {
                rows++;
            }
            CHECK(rows == v.n_devices, "count %u == list %u at t+%llums",
                  v.n_devices, rows,
                  (unsigned long long)((t - last) / 1000ull));
        }
    }
}

/* THE SPAM THE DIVERSITY TEST CANNOT SEE.
 *
 * Several of the common tools do not cycle payloads at all: they pick the one
 * dialog that is most annoying on the target and repeat it as fast as the
 * radio allows. One model code, hundreds of advertisements. Diversity counts
 * one, and the raw-rate note only fires above sixty a second, so a steady
 * twenty-a-second single-payload flood produced NO finding whatsoever. That is
 * the gap this covers, and it was reported from the room rather than found
 * here - the tests below are what stops it coming back. */
static void test_rival_single_payload_spam(void)
{
    banner("rival: one payload from a hundred addresses is still spam");
    uint8_t alen = 0;
    uint8_t addr[6] = { 0x02, 0, 0, 0, 0, 0 };

    {
        prv_state_t s; prv_verdict_t v;
        prv_reset(&s);
        /* ONE model code throughout - and a fresh address each time, which is
         * what these tools do so a phone cannot dismiss them permanently. */
        for (int i = 0; i < 40; i++) {
            addr[4] = (uint8_t)(i >> 8);
            addr[5] = (uint8_t)i;
            const uint8_t *a = apple_pair(0x0B, &alen);
            prv_observe_ble_adv(&s, addr, NULL, a, alen, -45,
                                T0 + (uint64_t)i * 50000ull);
        }
        prv_evaluate(&s, T0 + 2000000ull, &v);
        CHECK(v.pair_models < PRV_SPAM_MODELS,
              "diversity alone sees nothing (%u models)", v.pair_models);
        CHECK(v.peak_adv_per_s < 60u, "and the raw-rate note does not fire (%u/s)",
              v.peak_adv_per_s);
        CHECK(v.pair_addrs >= PRV_SPAM_ADDRS,
              "but the addresses churn (%u)", v.pair_addrs);
        CHECK(v.notes & PRV_NOTE_PAIR_SPAM, "so it IS named as spam");
        CHECK_EQ(v.band, PRV_BAND_ACTIVE);
    }

    /* THE ROOM THAT MUST STAY QUIET, again - because a new family is a new way
     * to be wrong. Real accessories advertise one model each AND keep their
     * address, so neither half of the test may fire. */
    {
        prv_state_t s; prv_verdict_t v;
        prv_reset(&s);
        for (int i = 0; i < 120; i++) {
            const uint8_t which = (uint8_t)(i % 4);
            addr[4] = 0;
            addr[5] = which;                     /* four stable accessories */
            const uint8_t *a = apple_pair((uint8_t)(0x30 + which), &alen);
            prv_observe_ble_adv(&s, addr, NULL, a, alen, -55,
                                T0 + (uint64_t)i * 30000ull);
        }
        prv_evaluate(&s, T0 + 3000000ull, &v);
        CHECK(v.pair_advs >= PRV_SPAM_ADVS, "plenty of advertisements (%u)",
              v.pair_advs);
        CHECK(v.pair_addrs < PRV_SPAM_ADDRS,
              "from four addresses, not forty (%u)", v.pair_addrs);
        CHECK((v.notes & PRV_NOTE_PAIR_SPAM) == 0, "so it is NOT called spam");
        CHECK(v.band < PRV_BAND_ACTIVE, "and does not alarm");
    }

    /* A spammer that stops must stop being reported: the addresses expire on
     * their own stamps, the same way the models do. */
    {
        prv_state_t s; prv_verdict_t v;
        prv_reset(&s);
        for (int i = 0; i < 40; i++) {
            addr[4] = (uint8_t)(i >> 8);
            addr[5] = (uint8_t)i;
            const uint8_t *a = apple_pair(0x0B, &alen);
            prv_observe_ble_adv(&s, addr, NULL, a, alen, -45,
                                T0 + (uint64_t)i * 50000ull);
        }
        prv_evaluate(&s, T0 + 2000000ull, &v);
        CHECK(v.notes & PRV_NOTE_PAIR_SPAM, "spam while it is running");
        prv_evaluate(&s, T0 + 60000000ull, &v);
        CHECK(v.pair_addrs == 0, "addresses expire out of the window");
        CHECK((v.notes & PRV_NOTE_PAIR_SPAM) == 0,
              "and it stops being called spam once it stops");
    }
}

/* THE FAMILIES THAT WERE NOT BEING PARSED AT ALL.
 *
 * Samsung's EasySetup advertisement is what the Android-facing tools reach
 * for, and this engine could not see it: not scored low, not seen. Microsoft's
 * is the opposite problem - company 0x0006 alone is every Microsoft-adjacent
 * device announcing something unrelated, so it had to be narrowed to the Swift
 * Pair beacon ID or ordinary traffic would inflate the diversity count. */
static void test_rival_spam_families(void)
{
    banner("rival: Samsung counts, and Microsoft only when it is Swift Pair");
    uint8_t addr[6] = { 0x02, 0, 0, 0, 0, 0 };

    {
        prv_state_t s; prv_verdict_t v;
        prv_reset(&s);
        /* len, 0xFF, 75 00, then payload; byte p[4] is the model. */
        uint8_t sam[10] = { 0x02, 0x01, 0x06, 0x06, 0xFF, 0x75, 0x00, 0x42, 0x00, 0x00 };
        for (int i = 0; i < 40; i++) {
            addr[5] = (uint8_t)i;
            sam[9] = (uint8_t)(0x50 + (i % 10)); /* p[4] of the 0xFF block */
            prv_observe_ble_adv(&s, addr, NULL, sam, 10, -45,
                                T0 + (uint64_t)i * 50000ull);
        }
        prv_evaluate(&s, T0 + 2000000ull, &v);
        CHECK(v.pair_advs > 0, "Samsung EasySetup is parsed at all (%u)",
              v.pair_advs);
        CHECK(v.notes & PRV_NOTE_PAIR_SPAM, "and Samsung spam is named");
    }

    {
        prv_state_t s; prv_verdict_t v;
        prv_reset(&s);
        /* Company 0x0006 with beacon ID 0x03 - genuine Swift Pair. */
        uint8_t ms[11] = { 0x02, 0x01, 0x06, 0x07, 0xFF, 0x06, 0x00, 0x03, 0x00, 0x00, 0x00 };
        for (int i = 0; i < 40; i++) {
            addr[5] = (uint8_t)i;
            ms[10] = (uint8_t)(0x60 + (i % 10));
            prv_observe_ble_adv(&s, addr, NULL, ms, 11, -45,
                                T0 + (uint64_t)i * 50000ull);
        }
        prv_evaluate(&s, T0 + 2000000ull, &v);
        CHECK(v.notes & PRV_NOTE_PAIR_SPAM, "Swift Pair spam is named");
    }

    {
        prv_state_t s; prv_verdict_t v;
        prv_reset(&s);
        /* Company 0x0006 with a DIFFERENT beacon ID: ordinary Microsoft
         * traffic, from stable addresses. Must not be counted as a pairing
         * popup at all - this is the false positive the narrowing prevents. */
        uint8_t ms[11] = { 0x02, 0x01, 0x06, 0x07, 0xFF, 0x06, 0x00, 0x01, 0x00, 0x00, 0x00 };
        for (int i = 0; i < 40; i++) {
            addr[5] = (uint8_t)(i % 3);
            ms[10] = (uint8_t)(0x60 + (i % 10));
            prv_observe_ble_adv(&s, addr, NULL, ms, 11, -45,
                                T0 + (uint64_t)i * 50000ull);
        }
        prv_evaluate(&s, T0 + 2000000ull, &v);
        CHECK(v.pair_advs == 0, "non-Swift-Pair Microsoft data is not a popup");
        CHECK((v.notes & PRV_NOTE_PAIR_SPAM) == 0, "and is not called spam");
    }
}

/* A RENAMED PINEAPPLE IS STILL A PINEAPPLE.
 *
 * Detection was a match on the word "pineapple" in the network name, which
 * survives exactly as long as it takes somebody to change it - and changing it
 * is step one of using the thing. The OUI is what the vendor shipped. */
static void test_rival_pineapple(void)
{
    banner("rival: the rogue-AP appliance, by name and by OUI");

    {
        prv_state_t s; prv_verdict_t v;
        prv_reset(&s);
        const uint8_t bssid[6] = { 0x00, 0x13, 0x37, 0x11, 0x22, 0x33 };
        /* A name that gives nothing away - "Guest WiFi" is the whole point. */
        for (int i = 0; i < 6; i++) {
            prv_observe_beacon(&s, bssid, "Guest WiFi", 10, false, -50,
                               T0 + (uint64_t)i * 300000ull);
        }
        prv_evaluate(&s, T0 + 2000000ull, &v);
        CHECK_EQ(v.worst_kind, PRV_KIND_PINEAPPLE);
        CHECK(v.n_wifi_tools >= 1, "counted as a Wi-Fi tool");
        CHECK(v.score <= PRV_CAP_PRESENCE_ONLY,
              "and still capped: owning one is not an offence (%u)", v.score);
    }

    {
        prv_state_t s; prv_verdict_t v;
        prv_reset(&s);
        /* The default management network, on a vendor-neutral address. */
        const uint8_t bssid[6] = { 0xAA, 0xBB, 0xCC, 0x01, 0x02, 0x03 };
        for (int i = 0; i < 6; i++) {
            prv_observe_beacon(&s, bssid, "MK7_1A2B", 8, false, -60,
                               T0 + (uint64_t)i * 300000ull);
        }
        prv_evaluate(&s, T0 + 2000000ull, &v);
        CHECK_EQ(v.worst_kind, PRV_KIND_PINEAPPLE);
    }

    {
        /* And the ordinary network that must not be swept up. */
        prv_state_t s; prv_verdict_t v;
        prv_reset(&s);
        const uint8_t bssid[6] = { 0x3C, 0x37, 0x86, 0x01, 0x02, 0x03 };
        for (int i = 0; i < 6; i++) {
            prv_observe_beacon(&s, bssid, "Sunshine 12", 11, false, -50,
                               T0 + (uint64_t)i * 300000ull);
        }
        prv_evaluate(&s, T0 + 2000000ull, &v);
        CHECK_EQ(v.n_wifi_tools, 0);
        CHECK_EQ(v.band, PRV_BAND_CLEAR);
    }

    CHECK(prv_is_hak5_oui((const uint8_t[]){ 0x00, 0x13, 0x37, 0, 0, 0 }),
          "the OUI itself");
    CHECK(!prv_is_hak5_oui((const uint8_t[]){ 0x00, 0x13, 0x38, 0, 0, 0 }),
          "and only that OUI");
}

/* ONE FLIPPER RUNNING SPAM IS ONE FLIPPER.
 *
 * A Flipper broadcasting pairing spam uses a fresh random address AND a fresh
 * junk-suffixed name for every advertisement. A table keyed on address showed
 * twenty-four Flippers in a room containing one - the same mistake as counting
 * rows instead of devices, one layer down. */
static void test_rival_address_rotation_is_one_device(void)
{
    banner("rival: a device that rotates its address is still one device");
    prv_state_t s;
    prv_verdict_t v;
    prv_reset(&s);

    uint8_t addr[6] = { 0x02, 0x00, 0x00, 0x00, 0x00, 0x00 };
    uint8_t adv[16] = { 0x02, 0x01, 0x06, 0x03, 0x02, 0x82, 0x30, 0x00 };

    /* The steady advertisement: one address, its real name, seen often. */
    static const uint8_t REAL[6] = { 0x88, 0x27, 0x90, 0x26, 0xe1, 0x80 };
    for (int i = 0; i < 30; i++) {
        prv_observe_ble_adv(&s, REAL, "R3ghon", adv, 7, -52,
                            T0 + (uint64_t)i * 100000ull);
    }
    /* The spam: twenty-four rotating addresses, each seen once, each carrying
     * a name with unprintable junk appended. */
    for (int i = 0; i < 24; i++) {
        addr[5] = (uint8_t)(0x40 + i);
        char junk[16];
        snprintf(junk, sizeof(junk), "Flipper %c%c", (char)0x01, (char)(0x80 + i));
        prv_observe_ble_adv(&s, addr, junk, adv, 7, -54,
                            T0 + 3000000ull + (uint64_t)i * 20000ull);
    }
    prv_evaluate(&s, T0 + 5000000ull, &v);

    CHECK_EQ(v.n_flipper, 1);
    CHECK_EQ(v.n_devices, 1);
    CHECK(v.n_addresses >= 20, "but the addresses are counted (%u)",
          v.n_addresses);
    /* The steadiest address supplies the name, so the real one survives the
     * flood of spoofed ones. */
    CHECK(strcmp(v.worst_name, "R3ghon") == 0,
          "the steady name wins (got \"%s\")", v.worst_name);

    prv_device_t d;
    CHECK(prv_device_at(&s, 0, &d), "one entry in the list");
    CHECK_EQ(d.kind, PRV_KIND_FLIPPER);
    CHECK(d.addresses >= 20, "carrying the address count (%u)", d.addresses);
    CHECK(!prv_device_at(&s, 1, &d), "and nothing after it");

    /* Names are sanitised: no unprintable byte reaches a label. */
    for (const char *c = d.name; *c; c++) {
        CHECK((unsigned char)*c >= 0x20 && (unsigned char)*c < 0x7F,
              "name is printable");
    }
}




/* A steady attack must produce a steady reading. The first window
 * implementation reset its counters every four seconds, so continuous spam
 * made the verdict fall out of IN USE and climb back in over and over while
 * nothing in the room had changed. */
static void test_rival_steady_spam_reads_steady(void)
{
    banner("rival: continuous spam does not make the verdict oscillate");
    prv_state_t s;
    prv_reset(&s);
    uint8_t alen = 0;
    uint8_t addr[6] = { 0x02, 0, 0, 0, 0, 0 };

    /* Twenty seconds of unbroken spam - five times the window - sampled every
     * half second throughout. Every sample after the first second must agree. */
    unsigned samples = 0, active = 0;
    for (int i = 0; i < 400; i++) {
        const uint64_t t = T0 + (uint64_t)i * 50000ull; /* 20 adverts a second */
        addr[5] = (uint8_t)i;
        addr[4] = (uint8_t)(i >> 8);
        const uint8_t *a = apple_pair((uint8_t)(0x01 + (i % 14)), &alen);
        prv_observe_ble_adv(&s, addr, NULL, a, alen, -45, t);

        if (i > 20 && (i % 10) == 0) {
            prv_verdict_t v;
            prv_evaluate(&s, t, &v);
            samples++;
            if (v.band == PRV_BAND_ACTIVE) {
                active++;
            }
        }
    }
    CHECK(samples > 20, "plenty of samples (%u)", samples);
    CHECK_EQ(active, samples); /* every single one, not most of them */
}

/* THE MARAUDER SITTING THERE AS A ROGUE ACCESS POINT.
 *
 * A Marauder doing a deauthentication flood is Watch's problem, and doing
 * beacon spam is Mirage's. Standing up an evil portal it was invisible: it
 * looked like any other network, because nothing here read the one part of the
 * frame that gives it away - the radio's own address.
 *
 * Nearly every cheap Wi-Fi attack tool is an ESP32, and no manufactured router
 * carries an Espressif OUI. */
static void test_rival_devboard_access_points(void)
{
    banner("rival: a dev board running an access point is not a router");

    /* 24:0A:C4 - Espressif. This is an ESP32 beaconing. */
    const uint8_t esp[6] = { 0x24, 0x0A, 0xC4, 0x11, 0x22, 0x33 };
    /* 00:1D:7E - Cisco-Linksys. A real router. */
    const uint8_t real[6] = { 0x00, 0x1D, 0x7E, 0x44, 0x55, 0x66 };

    CHECK(prv_is_devboard_oui(esp), "an Espressif prefix is recognised");
    CHECK(!prv_is_devboard_oui(real), "a router vendor's is not");
    CHECK(!prv_is_devboard_oui(NULL), "and NULL is survivable");

    /* A LOCALLY-ADMINISTERED address is not a manufacturer assignment at all,
     * so an OUI table says nothing about it. Every one of these tools can set
     * one, and matching anyway would produce confident nonsense. */
    const uint8_t laa[6] = { 0x26, 0x0A, 0xC4, 0x11, 0x22, 0x33 };
    CHECK(!prv_is_devboard_oui(laa), "a locally-administered address is refused");

    /* AN ENCRYPTED dev-board network is a smart plug, a sensor, a hobby
     * project. Worth noting, and no more - this is the false positive that
     * would otherwise be somebody's furniture. */
    {
        prv_state_t s; prv_verdict_t v;
        prv_reset(&s);
        for (int i = 0; i < 20; i++) {
            prv_observe_beacon_ex(&s, esp, "MyPlug", 6, false, false, -50,
                                  T0 + (uint64_t)i * 100000ull);
        }
        prv_evaluate(&s, T0 + 2000000ull, &v);
        CHECK_EQ(v.n_devices, 1);
        CHECK(v.worst_kind == PRV_KIND_DEV_BOARD,
              "a closed dev-board network is just a dev board");
        CHECK(v.score <= PRV_CAP_PRESENCE_ONLY,
              "and presence alone stays capped (%u)", (unsigned)v.score);
    }

    /* AN OPEN one is the shape a captive portal needs and a finished smart
     * plug does not. */
    {
        prv_state_t s; prv_verdict_t v;
        prv_reset(&s);
        for (int i = 0; i < 20; i++) {
            prv_observe_beacon_ex(&s, esp, "Free WiFi", 9, false, true, -50,
                                  T0 + (uint64_t)i * 100000ull);
        }
        prv_evaluate(&s, T0 + 2000000ull, &v);
        CHECK(v.worst_kind == PRV_KIND_ROGUE_AP,
              "an open dev-board network is a portal's shape");
        CHECK(v.score > 0, "and it is reported");
        CHECK(v.score <= PRV_CAP_PRESENCE_ONLY,
              "still capped - a shape is not a confession (%u)",
              (unsigned)v.score);
    }

    /* A REAL ROUTER, open or not, is not this engine's business. An open
     * guest network is a configuration finding for Census, and if Rival
     * reported every cafe hotspot as attack hardware nobody would use it. */
    {
        prv_state_t s; prv_verdict_t v;
        prv_reset(&s);
        for (int i = 0; i < 20; i++) {
            prv_observe_beacon_ex(&s, real, "Cafe Guest", 10, false, true, -50,
                                  T0 + (uint64_t)i * 100000ull);
        }
        prv_evaluate(&s, T0 + 2000000ull, &v);
        CHECK_EQ(v.n_devices, 0);
        CHECK(v.band == PRV_BAND_CLEAR, "an open cafe hotspot is not a finding");
    }

    /* A board that ANNOUNCES itself keeps the more specific classification -
     * the OUI test must not overwrite a name that already said more. */
    {
        prv_state_t s; prv_verdict_t v;
        prv_reset(&s);
        for (int i = 0; i < 20; i++) {
            prv_observe_beacon_ex(&s, esp, "pwned", 5, false, true, -50,
                                  T0 + (uint64_t)i * 100000ull);
        }
        prv_evaluate(&s, T0 + 2000000ull, &v);
        CHECK(v.worst_kind == PRV_KIND_DEAUTHER,
              "a board that names the attack keeps that name (got %s)",
              prv_kind_name(v.worst_kind));
    }
}

/* THE MODULE THAT HAS BEEN RENAMED.
 *
 * Every BLE serial bridge in the skimmer literature ships with a factory name,
 * and changing it is a single AT command - so a name list catches the careless
 * and nobody else. The GATT profile is what the module IS, and changing that
 * means reflashing it. */
static void test_rival_serial_bridge_by_profile(void)
{
    banner("rival: a serial bridge is caught by its profile, not its name");

    const uint8_t addr[6] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 };

    /* Advertising FFE0 - the transparent-UART service - under a name that
     * gives nothing away. */
    static const uint8_t ffe0[] = {
        0x02, 0x01, 0x06,
        0x03, 0x03, 0xE0, 0xFF,             /* complete 16-bit UUID: FFE0 */
        0x05, 0x09, 'P', 'u', 'm', 'p',     /* an innocuous name          */
    };
    prv_state_t s; prv_verdict_t v;
    prv_reset(&s);
    for (int i = 0; i < 20; i++) {
        prv_observe_ble_adv(&s, addr, "Pump", ffe0, (uint8_t)sizeof(ffe0), -55,
                            T0 + (uint64_t)i * 100000ull);
    }
    prv_evaluate(&s, T0 + 2000000ull, &v);
    CHECK_EQ(v.n_devices, 1);
    CHECK(v.worst_kind == PRV_KIND_SERIAL_BRIDGE,
          "the profile gives it away despite the name (got %s)",
          prv_kind_name(v.worst_kind));

    /* FFF0, the other common clone family. */
    static const uint8_t fff0[] = {
        0x02, 0x01, 0x06,
        0x03, 0x03, 0xF0, 0xFF,
    };
    prv_state_t s2;
    prv_reset(&s2);
    for (int i = 0; i < 20; i++) {
        prv_observe_ble_adv(&s2, addr, NULL, fff0, (uint8_t)sizeof(fff0), -55,
                            T0 + (uint64_t)i * 100000ull);
    }
    prv_evaluate(&s2, T0 + 2000000ull, &v);
    CHECK(v.worst_kind == PRV_KIND_SERIAL_BRIDGE, "so does FFF0");

    /* AND IT IS STILL ONLY A SERIAL BRIDGE. The identical module is inside
     * scoreboards, door locks and a thousand hobby projects - calling it a
     * skimmer on this evidence would be inventing intent, and at a fuel pump
     * that is the difference between a warning and an accusation. */
    CHECK(v.score <= PRV_CAP_PRESENCE_ONLY,
          "presence stays capped (%u)", (unsigned)v.score);

    /* An ordinary BLE device with neither the name nor the profile is not
     * swept up. */
    static const uint8_t plain[] = {
        0x02, 0x01, 0x06,
        0x03, 0x03, 0x0D, 0x18,             /* heart rate service         */
    };
    prv_state_t s3;
    prv_reset(&s3);
    for (int i = 0; i < 20; i++) {
        prv_observe_ble_adv(&s3, addr, "Band", plain, (uint8_t)sizeof(plain),
                            -55, T0 + (uint64_t)i * 100000ull);
    }
    prv_evaluate(&s3, T0 + 2000000ull, &v);
    CHECK_EQ(v.n_devices, 0);
}

/* THE CABLE WITH A RADIO IN IT. */
static void test_rival_implant(void)
{
    banner("rival: an O.MG cable's access point is recognised");

    /* DE:4F:22 - the locally-administered form of Espressif's DC:4F:22, and
     * the documented O.MG indicator. */
    const uint8_t omg[6]  = { 0xDE, 0x4F, 0x22, 0xAA, 0xBB, 0xCC };
    /* DC:4F:22 - the ordinary Espressif assignment for the same part. */
    const uint8_t esp[6]  = { 0xDC, 0x4F, 0x22, 0xAA, 0xBB, 0xCC };

    CHECK(prv_is_implant_oui(omg), "DE:4F:22 is recognised");
    CHECK(!prv_is_implant_oui(esp), "DC:4F:22 is not - that is any ESP8266");
    CHECK(!prv_is_implant_oui(NULL), "NULL is survivable");

    /* AND THE REASON IT NEEDS ITS OWN TEST: the dev-board table refuses
     * locally-administered addresses, so this one would otherwise be missed
     * entirely - the general rule is right, and this is its exception. */
    CHECK(!prv_is_devboard_oui(omg),
          "the dev-board test still refuses a made-up address");
    CHECK(prv_is_devboard_oui(esp), "and still accepts the real assignment");

    prv_state_t s; prv_verdict_t v;
    prv_reset(&s);
    for (int i = 0; i < 20; i++) {
        prv_observe_beacon_ex(&s, omg, "iPhone", 6, false, false, -40,
                              T0 + (uint64_t)i * 100000ull);
    }
    prv_evaluate(&s, T0 + 2000000ull, &v);
    CHECK_EQ(v.n_devices, 1);
    CHECK(v.worst_kind == PRV_KIND_IMPLANT,
          "an implant, not a dev board (got %s)", prv_kind_name(v.worst_kind));

    /* Encrypted or open, it is the same finding - unlike a dev board, where
     * open is what distinguishes a portal from a smart plug. A cable has no
     * innocent reason to be broadcasting at all. */
    prv_state_t o;
    prv_reset(&o);
    for (int i = 0; i < 20; i++) {
        prv_observe_beacon_ex(&o, omg, NULL, 0, false, true, -40,
                              T0 + (uint64_t)i * 100000ull);
    }
    prv_evaluate(&o, T0 + 2000000ull, &v);
    CHECK(v.worst_kind == PRV_KIND_IMPLANT, "open too");

    /* Still capped at presence. The address is one anything could have set, so
     * this is a strong hint and not an identification. */
    CHECK(v.score <= PRV_CAP_PRESENCE_ONLY, "presence stays capped (%u)",
          (unsigned)v.score);
}

static void test_rival_payload_independent_spam(void)
{
    banner("rival: one radio wearing many addresses, whatever it advertises");

    /* THE ATTACK THE OTHER TWO TESTS CANNOT SEE.
     *
     * Measured on hardware: a name-rotation flood produced 244 distinct
     * addresses and "popup models / advs" of 0/0, because it carries no Apple
     * pairing payload at all. Both existing tests need one. This one reads no
     * payload: the attacker writes the address, the world writes the level. */
    prv_state_t s;
    prv_reset(&s);

    static const char *k_names[4] = { "MNTM-FW", "YAPYAPYAP", "Flipper", "hi" };
    uint64_t t = 1000000ull;
    for (unsigned i = 0; i < 30u; i++) {
        uint8_t a[6] = { 0x02, 0x00, 0x00, 0x00, (uint8_t)(i >> 8), (uint8_t)i };
        /* One radio: every advertisement lands within a couple of dB. */
        const int8_t rssi = (int8_t)(-52 + (int)(i % 3u));
        prv_observe_ble_adv(&s, a, k_names[i % 4u], 0, 0, rssi, t);
        t += 120000ull; /* ~8 a second */
    }

    prv_verdict_t v;
    prv_evaluate(&s, t, &v);

    CHECK(v.notes & PRV_NOTE_COHERENT, "a coherent address flood is seen");
    CHECK(v.families & PRV_FAM_ONE_RADIO, "and attributed to one radio");
    CHECK(v.families & PRV_FAM_ACTIVE, "which is something being DONE");
    CHECK(v.notes & PRV_NOTE_MANY_NAMES, "wearing several names");
    CHECK(v.cohere_addrs >= PRV_COHERE_ADDRS, "the count is reported");
    CHECK(v.score >= 74, "and it reaches the ACTIVE band");
    CHECK(v.band == PRV_BAND_ACTIVE, "explicitly");
}

static void test_rival_a_busy_room_is_not_a_spammer(void)
{
    banner("rival: many accessories at many distances is not one radio");

    /* THE NEGATIVE THIS EXISTS FOR. A cafe full of headphones, watches and
     * beacons also produces a lot of addresses - but they are at different
     * DISTANCES, so their levels are spread across tens of dB. Accusing that
     * room of running an attack is how an operator learns to ignore the
     * screen. */
    prv_state_t s;
    prv_reset(&s);

    uint64_t t = 1000000ull;
    for (unsigned i = 0; i < 30u; i++) {
        uint8_t a[6] = { 0x02, 0x00, 0x00, 0x00, (uint8_t)(i >> 8), (uint8_t)i };
        /* -30 down to -88: a real room, not one transmitter. */
        const int8_t rssi = (int8_t)(-30 - (int)(i * 2u));
        prv_observe_ble_adv(&s, a, "Headphones", 0, 0, rssi, t);
        t += 120000ull;
    }

    prv_verdict_t v;
    prv_evaluate(&s, t, &v);

    CHECK(!(v.notes & PRV_NOTE_COHERENT), "a spread of levels is not a flood");
    CHECK(!(v.families & PRV_FAM_ONE_RADIO), "and is not blamed on one radio");
    CHECK(v.cohere_addrs < PRV_COHERE_ADDRS, "the cluster never forms");
}

static void test_rival_one_address_repeating_is_not_a_flood(void)
{
    banner("rival: a chatty beacon is not a rotation flood");

    /* A single accessory advertising fast keeps ONE address. The test counts
     * DISTINCT addresses, so it must be unmoved however talkative it is. */
    prv_state_t s;
    prv_reset(&s);

    uint8_t a[6] = { 0x02, 0x11, 0x22, 0x33, 0x44, 0x55 };
    uint64_t t = 1000000ull;
    for (unsigned i = 0; i < 60u; i++) {
        prv_observe_ble_adv(&s, a, "Beacon", 0, 0, -52, t);
        t += 60000ull;
    }

    prv_verdict_t v;
    prv_evaluate(&s, t, &v);
    CHECK(!(v.notes & PRV_NOTE_COHERENT), "one address is not many");
    CHECK(v.cohere_addrs <= 1, "and counts as one");
}

static void test_rival_ios_flood_varies_nothing(void)
{
    banner("rival: an iOS flood varies neither payload nor address");

    /* MEASURED ON HARDWARE AND SCORED CLEAR. Three action codes from one
     * address defeats both diversity tests; eighty-eight popup-triggering
     * advertisements in four seconds is the attack regardless. */
    prv_state_t s;
    prv_reset(&s);

    const uint8_t one[6] = { 0x7c, 0x33, 0xe0, 0x7e, 0xbc, 0x1f };
    uint64_t t = 1000000ull;
    for (unsigned i = 0; i < 90u; i++) {
        /* company 0x004C, type 0x0F (Nearby Action), three action codes */
        uint8_t adv[8] = { 0x07, 0xFF, 0x4C, 0x00, 0x0F,
                           (uint8_t)(0x05 + (i % 3u)), 0x00, 0x00 };
        prv_observe_ble_adv(&s, one, "", adv, 8, -45, t);
        t += 45000ull; /* ~22 a second */
    }

    prv_verdict_t v;
    prv_evaluate(&s, t, &v);

    CHECK(v.pair_worst_addr >= PRV_SPAM_PAIR_RATE,
          "one address is doing all of it (%u)", (unsigned)v.pair_worst_addr);
    CHECK(v.pair_models < PRV_SPAM_MODELS, "payload diversity alone says no");
    CHECK(v.pair_addrs < PRV_SPAM_ADDRS, "address diversity alone says no");
    CHECK(v.notes & PRV_NOTE_PAIR_SPAM, "but the flood is still seen");
    CHECK(v.families & PRV_FAM_ACTIVE, "as something being DONE");
    CHECK(v.score >= 70, "and it reaches the ACTIVE band");
}

static void test_rival_an_accessory_burst_is_not_a_flood(void)
{
    banner("rival: a case being opened is not an attack");

    /* THE NEGATIVE. Real AirPods burst proximity-pairing when the case opens
     * and then STOP. The test is a sustained rate across the window, so a
     * short burst must not reach it. */
    prv_state_t s;
    prv_reset(&s);

    const uint8_t pods[6] = { 0x4c, 0x00, 0x11, 0x22, 0x33, 0x44 };
    uint64_t t = 1000000ull;
    for (unsigned i = 0; i < 12u; i++) {
        uint8_t adv[8] = { 0x07, 0xFF, 0x4C, 0x00, 0x07, 0x0E, 0x20, 0x00 };
        prv_observe_ble_adv(&s, pods, "", adv, 8, -55, t);
        t += 50000ull;
    }
    t += 4000000ull; /* then silence, as a real case does */

    prv_verdict_t v;
    prv_evaluate(&s, t, &v);
    CHECK(!(v.notes & PRV_NOTE_PAIR_SPAM), "a short burst is not a flood");
}

static void test_rival_a_turn_on_another_band_is_not_absence(void)
{
    banner("rival: silence while not listening is not evidence of absence");

    /* THE BUG, AS REPORTED: "my flipper zero is turned on but it sometimes
     * says there is nothing in the room".
     *
     * One radio serves every watch. On the home ring the watchtower hands it
     * round, so while another watch has it this lens hears nothing at all -
     * and staleness was measured on the WALL clock, so a Flipper sitting on
     * the desk was declared gone after ten seconds of somebody else's turn.
     * Absence of evidence taken as evidence of absence, by a lens that had
     * not noticed its own ears were shut. */
    /* The real advertisement: a Flipper is recognised by the Flipper service
     * UUID 0x3082, not by its name - which is exactly why a renamed one is
     * still found. */
    static const uint8_t fz[] = {
        0x02, 0x01, 0x06,
        0x07, 0x09, 0x52, 0x33, 0x67, 0x68, 0x6f, 0x6e, /* "R3ghon" */
        0x03, 0x02, 0x82, 0x30,                          /* UUID 0x3082 */
    };
    const uint8_t addr[6] = { 0x88, 0x27, 0x90, 0x26, 0xe1, 0x80 };

    prv_state_t s;
    prv_verdict_t v;
    prv_reset(&s);

    /* Heard twice a second for a minute while this lens holds the radio. */
    uint64_t t = 1000000ull;
    for (unsigned i = 0; i < 120u; i++) {
        prv_listen(&s, 500000ull);
        prv_observe_ble_adv(&s, addr, "R3ghon", fz, (uint8_t)sizeof(fz), -53, t);
        t += 500000ull;
    }
    prv_evaluate(&s, t, &v);
    CHECK_EQ(v.n_flipper, 1);

    /* Now a full minute passes on ANOTHER watch's band. The wall clock runs;
     * the listening clock does not, because nothing was received. */
    t += 60000000ull;
    prv_evaluate(&s, t, &v);
    CHECK_EQ(v.n_flipper, 1);
    CHECK(v.n_devices == 1, "the Flipper is still in the room");
    {
        prv_device_t d;
        CHECK(prv_device_at_now(&s, 0, t, &d), "and still on the list");
    }

    /* But once this lens IS listening again and still hears nothing, it lets
     * go exactly as before. The rule did not get weaker, it got honest. */
    prv_listen(&s, 30000000ull);
    prv_evaluate(&s, t + 30000000ull, &v);
    CHECK_EQ(v.n_flipper, 0);
}

void test_rival(void)
{
    test_rival_devboard_access_points();
    test_rival_implant();
    test_rival_serial_bridge_by_profile();
    test_rival_names();
    test_rival_pwnagotchi();
    test_rival_flipper_advertisement();
    test_rival_pairing_spam();
    test_rival_switched_off_everywhere();
    test_rival_expiry_follows_cadence();
    test_rival_single_payload_spam();
    test_rival_spam_families();
    test_rival_pineapple();
    test_rival_steady_spam_reads_steady();
    test_rival_address_rotation_is_one_device();
    test_rival_presence_is_capped();
    test_rival_spam_is_active();
    test_rival_busy_room_is_not_a_flood();
    test_rival_listing_and_vocabulary();    test_rival_payload_independent_spam();
    test_rival_a_busy_room_is_not_a_spammer();
    test_rival_one_address_repeating_is_not_a_flood();    test_rival_ios_flood_varies_nothing();
    test_rival_an_accessory_burst_is_not_a_flood();    test_rival_a_turn_on_another_band_is_not_absence();
}
