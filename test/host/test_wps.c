/* Host tests for the WPS information element parser.
 *
 * These bytes come off the air from strangers, and the failure modes are
 * asymmetric. A miss costs a finding. A MISPARSE produces a confident model
 * string assembled from somebody else's bytes - which then goes on the glass,
 * into a report, and into whatever the operator looks up next. So the tests
 * are byte-level fixtures shaped exactly like real beacons, plus a set of
 * malformed ones that must not walk off the end of the buffer.
 */
#include <string.h>

#include "pharos_wps.h"
#include "test_support.h"

/* A WPS vendor IE as it appears in a beacon: id 221, length, MS OUI 00:50:F2,
 * type 0x04, then attributes. */
static const uint8_t k_archer[] = {
    /* an SSID element first, so the parser has to walk past something */
    0x00, 0x04, 'H', 'o', 'm', 'e',
    /* supported rates, likewise */
    0x01, 0x04, 0x82, 0x84, 0x8B, 0x96,
    /* the WPS element */
    0xDD, 0x32, 0x00, 0x50, 0xF2, 0x04,
    0x10, 0x4A, 0x00, 0x01, 0x10,             /* version 1.0        */
    0x10, 0x44, 0x00, 0x01, 0x02,             /* state: configured  */
    0x10, 0x08, 0x00, 0x02, 0x01, 0x0C,       /* config: keypad|label|display */
    0x10, 0x21, 0x00, 0x07, 'T','P','-','L','I','N','K',
    0x10, 0x23, 0x00, 0x09, 'A','r','c','h','e','r',' ','C','7',
    0x10, 0x24, 0x00, 0x02, 'v','2',
};

static void test_wps_reads_the_plate(void)
{
    banner("wps: manufacturer, model and model number come out intact");
    pwps_info_t w;
    CHECK(pwps_parse(k_archer, (uint16_t)sizeof(k_archer), &w),
          "the element is found past the SSID and rates");
    CHECK(w.present, "and marked present");
    CHECK(strcmp(w.manufacturer, "TP-LINK") == 0, "manufacturer: %s",
          w.manufacturer);
    CHECK(strcmp(w.model, "Archer C7") == 0, "model: %s", w.model);
    CHECK(strcmp(w.model_number, "v2") == 0, "model number: %s",
          w.model_number);
    CHECK_EQ(w.version, 0x10);
    CHECK_EQ(w.state, 2);

    /* THE POINT OF ALL THIS: a string somebody can look up. */
    char d[48];
    pwps_model(&w, d, sizeof(d));
    CHECK(strcmp(d, "TP-LINK Archer C7 v2") == 0, "described as: %s", d);
}

/* THE VULNERABILITY THAT IS IN THE ELEMENT ITSELF. */
static void test_wps_pin_exposure(void)
{
    banner("wps: the PIN exposure is read from what the beacon advertises");
    pwps_info_t w;
    pwps_parse(k_archer, (uint16_t)sizeof(k_archer), &w);
    CHECK(pwps_pin_exposed(&w),
          "keypad and label offered, not locked - the flaw applies");

    /* PUSH-BUTTON ONLY is not reachable by the PIN attack, and saying it were
     * would be the false positive that teaches somebody to ignore this. */
    uint8_t pb[sizeof(k_archer)];
    memcpy(pb, k_archer, sizeof(pb));
    /* config methods -> pushbutton only (0x0080) */
    for (unsigned i = 0; i + 1 < sizeof(pb); i++) {
        if (pb[i] == 0x10 && pb[i + 1] == 0x08) {
            pb[i + 4] = 0x00;
            pb[i + 5] = 0x80;
            break;
        }
    }
    pwps_info_t p;
    pwps_parse(pb, (uint16_t)sizeof(pb), &p);
    CHECK_EQ(p.config_methods, 0x0080);
    CHECK(!pwps_pin_exposed(&p), "push-button only is not PIN-exposed");

    /* A REGISTRAR THAT HAS LOCKED ITSELF is the defence working. */
    static const uint8_t locked[] = {
        0xDD, 0x14, 0x00, 0x50, 0xF2, 0x04,
        0x10, 0x08, 0x00, 0x02, 0x01, 0x0C,   /* PIN methods offered */
        0x10, 0x57, 0x00, 0x01, 0x01,         /* AP setup LOCKED     */
        0x10, 0x4A, 0x00, 0x01, 0x10,
    };
    pwps_info_t l;
    CHECK(pwps_parse(locked, (uint16_t)sizeof(locked), &l), "parsed");
    CHECK(l.has_locked && l.locked, "the lock is read");
    CHECK(!pwps_pin_exposed(&l), "a locked registrar is not exposed");

    /* No WPS element at all is not an exposure. */
    pwps_info_t none;
    memset(&none, 0, sizeof(none));
    CHECK(!pwps_pin_exposed(&none), "absent WPS is not exposed");
    CHECK(!pwps_pin_exposed(NULL), "and NULL is survivable");
}

/* WPA1 USES THE SAME OUI. Matching on it alone would parse cipher suites as a
 * model name - a confident string of rubbish, which is the worst outcome here. */
static void test_wps_not_confused_by_wpa1(void)
{
    banner("wps: a WPA1 element is not mistaken for a WPS one");
    static const uint8_t wpa1[] = {
        0xDD, 0x16, 0x00, 0x50, 0xF2, 0x01,   /* type 0x01 = WPA1, not WPS */
        0x01, 0x00, 0x00, 0x50, 0xF2, 0x02,
        0x02, 0x00, 0x00, 0x50, 0xF2, 0x04,
        0x00, 0x50, 0xF2, 0x02, 0x01, 0x00,
    };
    pwps_info_t w;
    CHECK(!pwps_parse(wpa1, (uint16_t)sizeof(wpa1), &w),
          "the type byte distinguishes them");
    CHECK(!w.present, "and nothing is reported");
    CHECK(w.model[0] == '\0', "no model invented from cipher suites");
}

/* MALFORMED INPUT. Every one of these is a plausible thing to receive - a
 * truncated capture, a buggy access point, or somebody deliberately probing
 * what this does. None may read past the buffer or fail to terminate. */
static void test_wps_hostile_input(void)
{
    banner("wps: malformed elements are survived, not trusted");

    /* An attribute claiming more length than the element holds. */
    static const uint8_t overrun[] = {
        0xDD, 0x0C, 0x00, 0x50, 0xF2, 0x04,
        0x10, 0x23, 0xFF, 0xFF, 'A', 'B',     /* model name, length 65535 */
    };
    pwps_info_t w;
    pwps_parse(overrun, (uint16_t)sizeof(overrun), &w);
    CHECK(w.model[0] == '\0', "an over-long attribute is refused, not clamped");

    /* An IE claiming more length than the frame holds. */
    static const uint8_t ie_overrun[] = { 0xDD, 0x7F, 0x00, 0x50, 0xF2, 0x04 };
    CHECK(!pwps_parse(ie_overrun, (uint16_t)sizeof(ie_overrun), &w),
          "an over-long IE is refused");

    /* Zero-length everything - must terminate. */
    static const uint8_t zeros[] = {
        0xDD, 0x08, 0x00, 0x50, 0xF2, 0x04, 0x10, 0x23, 0x00, 0x00,
    };
    CHECK(pwps_parse(zeros, (uint16_t)sizeof(zeros), &w), "a zero attribute parses");
    CHECK(w.model[0] == '\0', "and yields nothing");

    /* Empty and NULL. */
    CHECK(!pwps_parse(NULL, 0, &w), "NULL input");
    CHECK(!pwps_parse(k_archer, 0, &w), "zero length");
    CHECK(!pwps_parse(k_archer, 3, &w), "a truncated first element");
    CHECK(!pwps_parse(k_archer, (uint16_t)sizeof(k_archer), NULL), "NULL output");

    /* NON-PRINTABLE bytes in a model name are dropped, not rendered. This one
     * goes straight onto the glass and into reports. */
    static const uint8_t nasty[] = {
        0xDD, 0x0E, 0x00, 0x50, 0xF2, 0x04,
        0x10, 0x23, 0x00, 0x06, 'A', 0x1B, '[', '2', 'J', 'B',
    };
    pwps_parse(nasty, (uint16_t)sizeof(nasty), &w);
    for (const char *p = w.model; *p; p++) {
        CHECK((unsigned char)*p >= 0x20 && (unsigned char)*p < 0x7F,
              "every rendered byte is printable");
    }
    CHECK(strcmp(w.model, "A[2JB") == 0, "the escape is removed: %s", w.model);
}

/* Placeholder serials are the norm, and crying wolf about them would bury the
 * models that genuinely do leak one. */
static void test_wps_serial_placeholders(void)
{
    banner("wps: a placeholder serial number is not a leak");
    struct { const char *val; bool leak; } k[] = {
        { "00000000", false }, { "12345678", false }, { "none", false },
        { "--------", false }, { "0", false },        { "A1B2C3D4", true },
    };
    for (unsigned i = 0; i < sizeof(k) / sizeof(k[0]); i++) {
        uint8_t buf[64];
        const unsigned n = (unsigned)strlen(k[i].val);
        unsigned j = 0;
        buf[j++] = 0xDD;
        buf[j++] = (uint8_t)(4u + 4u + n);
        buf[j++] = 0x00; buf[j++] = 0x50; buf[j++] = 0xF2; buf[j++] = 0x04;
        buf[j++] = 0x10; buf[j++] = 0x42;
        buf[j++] = 0x00; buf[j++] = (uint8_t)n;
        memcpy(&buf[j], k[i].val, n);
        j += n;
        pwps_info_t w;
        pwps_parse(buf, (uint16_t)j, &w);
        CHECK(w.serial_leaked == k[i].leak, "serial %s leak=%d", k[i].val,
              (int)w.serial_leaked);
    }
}

void test_wps(void)
{
    test_wps_reads_the_plate();
    test_wps_pin_exposure();
    test_wps_not_confused_by_wpa1();
    test_wps_hostile_input();
    test_wps_serial_placeholders();
}
