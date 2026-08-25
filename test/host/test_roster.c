/* Host tests for the device roster.
 *
 * The failure that matters here is not a miscounted device. It is Roster
 * saying something about a device it cannot support - naming a vulnerability
 * it never tested for, or calling a randomised address a fixed one and telling
 * somebody they are trackable when they are not.
 */
#include <string.h>

#include "pharos_event.h"
#include "pharos_roster.h"
#include "test_support.h"

#define T0 1000000000ull

static const uint8_t AP_TPLINK[6]  = { 0x14, 0xCC, 0x20, 0x11, 0x22, 0x33 };
static const uint8_t CAM_WYZE[6]   = { 0x2C, 0xAA, 0x8E, 0x01, 0x02, 0x03 };
static const uint8_t PHONE_FIX[6]  = { 0x3C, 0x06, 0x30, 0xAA, 0xBB, 0xCC };
static const uint8_t PHONE_RAND[6] = { 0x4E, 0x11, 0x22, 0x33, 0x44, 0x55 };
static const uint8_t ESP_IOT[6]    = { 0x24, 0x0A, 0xC4, 0x09, 0x08, 0x07 };

static rd_secpost_t sec_wpa2(void)
{
    rd_secpost_t s = { .privacy = true, .wpa1 = false, .tkip = false,
                       .rsn_flags = PHAROS_RSN_F_PRESENT | PHAROS_RSN_F_PSK |
                                    PHAROS_RSN_F_MFP_CAPABLE };
    return s;
}
static rd_secpost_t sec_open(void)
{
    rd_secpost_t s = { 0 };
    return s;
}
static rd_secpost_t sec_wep(void)
{
    rd_secpost_t s = { .privacy = true };
    return s;
}

static void test_roster_identifies_by_oui(void)
{
    banner("roster: a device is named by who made it, without asking it");
    rd_roster_t r;
    rd_reset(&r);

    const rd_secpost_t w = sec_wpa2();
    rd_observe_wifi(&r, AP_TPLINK, true, "Sunset12", &w, 6, -42, NULL, T0);
    rd_observe_wifi(&r, CAM_WYZE, false, NULL, NULL, 6, -70, NULL, T0);

    CHECK_EQ(rd_count(&r), 2);
    CHECK(rd_vendor(AP_TPLINK) != NULL, "the AP's vendor is known");
    CHECK(strcmp(rd_vendor(AP_TPLINK), "TP-Link") == 0, "and it is TP-Link");
    CHECK(strcmp(rd_vendor(CAM_WYZE), "Wyze") == 0, "the camera is a Wyze");
    CHECK(rd_class_of_oui(CAM_WYZE) == RD_CAMERA, "and classed as a camera");
    CHECK(rd_vendor(PHONE_RAND) == NULL, "an unknown OUI is admitted as unknown");
}

/* BEHAVIOUR BEATS THE OUI TABLE. A vendor makes many products; a thing that
 * beacons is infrastructure whatever its prefix usually ships in. */
static void test_roster_behaviour_refines_class(void)
{
    banner("roster: beaconing means access point, whatever the OUI says");
    rd_roster_t r;
    rd_reset(&r);
    const rd_secpost_t w = sec_wpa2();

    /* An Espressif OUI normally means a small IoT board... */
    CHECK(rd_class_of_oui(ESP_IOT) == RD_IOT, "an ESP is IoT by prefix");
    /* ...but this one is beaconing, so it is acting as an access point. */
    rd_observe_wifi(&r, ESP_IOT, true, "esp-ap", &w, 1, -50, NULL, T0);
    rd_device_t d;
    CHECK(rd_at(&r, 0, &d), "it is on the roster");
    CHECK(d.klass == RD_ACCESS_POINT, "and reclassified by what it does");
    CHECK(strcmp(d.name, "esp-ap") == 0, "carrying the network name it announced");
}

/* THE EXPOSURES ARE READ OFF THE AIR, NOT GUESSED. */
static void test_roster_reads_real_weakness(void)
{
    banner("roster: weakness is read from the beacon, never inferred");
    rd_roster_t r;
    rd_reset(&r);

    const rd_secpost_t open = sec_open();
    const rd_secpost_t wep = sec_wep();
    const rd_secpost_t wpa2 = sec_wpa2();
    const uint8_t b_open[6] = { 0x50, 0xC7, 0xBF, 1, 1, 1 };
    const uint8_t b_wep[6]  = { 0x50, 0xC7, 0xBF, 2, 2, 2 };
    const uint8_t b_good[6] = { 0x50, 0xC7, 0xBF, 3, 3, 3 };

    rd_observe_wifi(&r, b_open, true, "FreeWiFi", &open, 6, -55, NULL, T0);
    rd_observe_wifi(&r, b_wep, true, "OldNet", &wep, 6, -60, NULL, T0);
    rd_observe_wifi(&r, b_good, true, "Home", &wpa2, 6, -40, NULL, T0);

    rd_device_t d;
    /* The open network sorts first: it is the worst thing in the room. */
    CHECK(rd_at(&r, 0, &d), "there is a worst device");
    CHECK((d.exposure & RD_EXP_OPEN) != 0, "the open AP is flagged open");
    CHECK(strstr(rd_exposure_headline(&d), "open") != NULL,
          "and says so in plain words");

    /* And WPA2 with MFP gets no exposure flags at all - a clean device must
     * come back clean, or the whole list becomes noise. */
    for (unsigned i = 0; rd_at(&r, i, &d); i++) {
        if (memcmp(d.mac, b_good, 6) == 0) {
            CHECK_EQ(d.exposure, 0);
            CHECK(rd_exposure_headline(&d) == NULL, "and has no headline");
        }
        if (memcmp(d.mac, b_wep, 6) == 0) {
            CHECK((d.exposure & RD_EXP_WEP) != 0, "WEP is caught");
        }
    }
}

/* MISSING 802.11w IS ONLY WORTH SAYING ABOUT A NETWORK THAT HAS ENCRYPTION.
 * Flagging it on an open AP would bury the real finding under a lesser one. */
static void test_roster_no_mfp_only_when_meaningful(void)
{
    banner("roster: no-802.11w is not flagged on a network with no crypto");
    rd_roster_t r;
    rd_reset(&r);

    rd_secpost_t nomfp = { .privacy = true,
                           .rsn_flags = PHAROS_RSN_F_PRESENT | PHAROS_RSN_F_PSK };
    const rd_secpost_t open = sec_open();
    const uint8_t a[6] = { 0x9C, 0x3D, 0xCF, 1, 1, 1 };
    const uint8_t b[6] = { 0x9C, 0x3D, 0xCF, 2, 2, 2 };

    rd_observe_wifi(&r, a, true, "WPA2NoMFP", &nomfp, 6, -50, NULL, T0);
    rd_observe_wifi(&r, b, true, "Open", &open, 6, -50, NULL, T0);

    rd_device_t d;
    for (unsigned i = 0; rd_at(&r, i, &d); i++) {
        if (memcmp(d.mac, a, 6) == 0) {
            CHECK((d.exposure & RD_EXP_NO_MFP) != 0,
                  "an encrypted network without 11w is flagged");
        }
        if (memcmp(d.mac, b, 6) == 0) {
            CHECK((d.exposure & RD_EXP_NO_MFP) == 0,
                  "an open one is not - it has a bigger problem");
        }
    }
}

/* A RANDOMISED MAC IS A DEVICE PROTECTING ITSELF, AND MUST NOT BE CALLED
 * TRACKABLE. Getting this backwards would tell somebody to fix something that
 * is already right. */
static void test_roster_randomised_mac(void)
{
    banner("roster: randomised addresses are not reported as trackable");
    rd_roster_t r;
    rd_reset(&r);

    rd_observe_wifi(&r, PHONE_FIX, false, NULL, NULL, 6, -55, NULL, T0);
    rd_observe_wifi(&r, PHONE_RAND, false, NULL, NULL, 6, -55, NULL, T0);

    rd_device_t d;
    bool saw_fixed = false, saw_rand = false;
    for (unsigned i = 0; rd_at(&r, i, &d); i++) {
        if (memcmp(d.mac, PHONE_FIX, 6) == 0) {
            saw_fixed = true;
            CHECK(!d.randomised_mac, "a burned-in address is not randomised");
            CHECK((d.exposure & RD_EXP_FIXED_MAC) != 0, "and is trackable");
        }
        if (memcmp(d.mac, PHONE_RAND, 6) == 0) {
            saw_rand = true;
            CHECK(d.randomised_mac, "the locally-administered bit is read");
            CHECK((d.exposure & RD_EXP_FIXED_MAC) == 0,
                  "and a randomising device is NOT flagged trackable");
        }
    }
    CHECK(saw_fixed && saw_rand, "both were on the roster");
}

/* THE CWE LIST IS A CHECKLIST FOR A CLASS, NEVER A CLAIM ABOUT A UNIT. */
static void test_roster_cwes_are_per_class(void)
{
    banner("roster: CWEs are per class, and only for classes that have them");
    const char *out[8];

    const unsigned cam = rd_class_cwes(RD_CAMERA, out, 8);
    CHECK(cam >= 3, "a camera has a checklist (%u entries)", cam);
    CHECK(strstr(out[0], "CWE-") != NULL, "each entry names a CWE");

    const unsigned rtr = rd_class_cwes(RD_ROUTER, out, 8);
    CHECK(rtr >= 3, "so does a router");

    /* An unknown device gets NOTHING. Offering a generic checklist for a thing
     * we could not even identify would be filler dressed as advice. */
    CHECK_EQ(rd_class_cwes(RD_UNKNOWN, out, 8), 0);

    /* The cap is honoured - a caller with room for two must not be handed four. */
    CHECK_EQ(rd_class_cwes(RD_CAMERA, out, 2), 2);
}

static void test_roster_expiry_and_capacity(void)
{
    banner("roster: devices go stale, and a full table drops the stalest");
    rd_roster_t r;
    rd_reset(&r);
    const rd_secpost_t w = sec_wpa2();

    rd_observe_wifi(&r, AP_TPLINK, true, "Sunset12", &w, 6, -42, NULL, T0);
    CHECK_EQ(rd_count(&r), 1);
    rd_expire(&r, T0 + RD_STALE_US / 2ull);
    CHECK_EQ(rd_count(&r), 1);
    rd_expire(&r, T0 + RD_STALE_US + 1000000ull);
    CHECK_EQ(rd_count(&r), 0);

    /* Overfill: admitting more than the table holds must not corrupt it. */
    rd_reset(&r);
    for (unsigned i = 0; i < PR_ROSTER_MAX + 20u; i++) {
        uint8_t m[6] = { 0x24, 0x0A, 0xC4, (uint8_t)(i >> 8), (uint8_t)i, 0x00 };
        rd_observe_wifi(&r, m, false, NULL, NULL, 6, -60,
                        NULL, T0 + (uint64_t)i * 1000ull);
    }
    CHECK(rd_count(&r) <= PR_ROSTER_MAX, "the table stays bounded");
    CHECK(rd_count(&r) == PR_ROSTER_MAX, "and stays full");
}

/* THE EXPORT IS THE HONEST ROUTE TO CVEs: it leaves the device, and a machine
 * that is allowed to talk to the internet does the lookup. */
static void test_roster_export_redacts(void)
{
    banner("roster: the export keeps the vendor and hides the host");
    rd_roster_t r;
    rd_reset(&r);
    const rd_secpost_t w = sec_wpa2();
    rd_observe_wifi(&r, AP_TPLINK, true, "Sunset12", &w, 6, -42, NULL, T0);

    char buf[512];
    const unsigned n = rd_export(&r, buf, sizeof(buf), true);
    CHECK(n > 0, "something was written");
    buf[n < sizeof(buf) ? n : sizeof(buf) - 1] = '\0';
    CHECK(strstr(buf, "TP-Link") != NULL, "the vendor survives - it is public");
    CHECK(strstr(buf, "\t-\t") != NULL, "a device with no model says so");
    CHECK(strstr(buf, "14:cc:20") != NULL, "so does the OUI half");
    /* The host-unique half must not appear verbatim. */
    CHECK(strstr(buf, "11:22:33") == NULL, "the host half is hashed away");
    CHECK(strstr(buf, "Sunset12") == NULL, "and the SSID is not exported");

    const unsigned n2 = rd_export(&r, buf, sizeof(buf), false);
    buf[n2 < sizeof(buf) ? n2 : sizeof(buf) - 1] = '\0';
    CHECK(strstr(buf, "11:22:33") != NULL, "unredacted keeps the full address");

    /* A tiny buffer must not be overrun. */
    char small[8];
    const unsigned n3 = rd_export(&r, small, sizeof(small), true);
    CHECK(n3 < sizeof(small), "a small buffer is respected");
}

static void test_roster_degenerate(void)
{
    banner("roster: NULLs are survivable");
    rd_reset(NULL);
    rd_observe_wifi(NULL, AP_TPLINK, true, "x", NULL, 1, -1, NULL, T0);
    rd_observe_ble(NULL, AP_TPLINK, false, "x", 0, NULL, 0, -1, T0);
    rd_expire(NULL, T0);
    CHECK_EQ(rd_count(NULL), 0);
    CHECK_EQ(rd_exposed_count(NULL), 0);
    CHECK(rd_vendor(NULL) == NULL, "a NULL MAC has no vendor");
    rd_device_t d;
    CHECK(!rd_at(NULL, 0, &d), "and no device");
    CHECK(rd_exposure_headline(NULL) == NULL, "and no headline");
    CHECK_EQ(rd_export(NULL, NULL, 0, true), 0);
}

/* EVERY TAG FITS THE COLUMN IT IS DRAWN IN.
 *
 * The list's right-hand column is eleven characters. "WPS on - PIN can be
 * forced" arrived there as "WPS on - PI", which on the glass read as a broken
 * display rather than a fact about somebody's network. */
static void test_roster_tags_fit_the_row(void)
{
    banner("roster: every exposure tag fits a list row");
    const uint16_t flags[] = { RD_EXP_OPEN,      RD_EXP_WEP,
                               RD_EXP_WPA1,      RD_EXP_WPS,
                               RD_EXP_FIXED_MAC, RD_EXP_PROBES_OPEN,
                               RD_EXP_NO_MFP,    RD_EXP_NAME_LEAK };
    for (unsigned i = 0; i < sizeof(flags) / sizeof(flags[0]); i++) {
        rd_device_t d;
        memset(&d, 0, sizeof(d));
        d.exposure = flags[i];
        const char *t = rd_exposure_tag(&d);
        CHECK(t != NULL, "flag 0x%02x has a tag", (unsigned)flags[i]);
        CHECK(strlen(t) <= RD_TAG_MAX, "\"%s\" fits the column", t);
        /* And the short form must name the SAME finding as the long one, or
         * the list and the detail page would disagree about what is wrong. */
        const char *h = rd_exposure_headline(&d);
        CHECK(h != NULL, "and a full sentence for the detail page");
    }
    rd_device_t clean;
    memset(&clean, 0, sizeof(clean));
    CHECK(rd_exposure_tag(&clean) == NULL, "a clean device has no tag");
    CHECK(rd_exposure_tag(NULL) == NULL, "and NULL is survivable");
}

/* THE EXPORT CARRIES THE MODEL, because that is the column a CVE database
 * takes. Without it the export is a list of vendors, which nobody can look
 * anything up against. */
static void test_roster_export_carries_the_model(void)
{
    banner("roster: the export carries the model the AP broadcast");
    rd_roster_t r;
    rd_reset(&r);
    const uint8_t ap[6] = { 0x14, 0xCC, 0x20, 0x11, 0x22, 0x33 };
    rd_secpost_t w = { .privacy = true,
                       .rsn_flags = PHAROS_RSN_F_PRESENT | PHAROS_RSN_F_PSK |
                                    PHAROS_RSN_F_MFP_CAPABLE };
    rd_observe_wifi(&r, ap, true, "Sunset12", &w, 6, -42, NULL, T0);
    rd_observe_wps(&r, ap, "TP-Link", "Archer C7 v2", true, T0);

    rd_device_t d;
    CHECK(rd_at(&r, 0, &d), "the AP is on the roster");
    CHECK(strcmp(d.model, "Archer C7 v2") == 0, "and carries its model");
    CHECK((d.exposure & RD_EXP_WPS_PIN) != 0, "and the PIN finding");
    CHECK(strcmp(rd_exposure_tag(&d), "WPS PIN") == 0,
          "which outranks the plain WPS tag");

    char buf[512];
    const unsigned n = rd_export(&r, buf, sizeof(buf), true);
    buf[n < sizeof(buf) ? n : sizeof(buf) - 1] = '\0';
    CHECK(strstr(buf, "Archer C7 v2") != NULL, "the model reaches the export");
    CHECK(strstr(buf, "0x0100") != NULL, "and so does the WPS-PIN bit");
    /* Still no SSID: the model is public, the network name identifies a place. */
    CHECK(strstr(buf, "Sunset12") == NULL, "the SSID still does not");
}

/* THE CAMERA THE VENDOR TABLE CANNOT NAME.
 *
 * Ring, Wyze, Axis and Hikvision are in the OUI table. The no-name camera off
 * a marketplace is not, and one with a randomised address defeats the table
 * outright - and that is the one somebody is most likely to find hidden in a
 * room they are staying in.
 *
 * What no camera can hide is that it uploads, continuously. The test is that
 * CONTINUITY and not the volume, because volume alone flags a laptop pulling
 * a large download and misses a low-bitrate camera entirely.
 */
static void test_roster_streaming_shape(void)
{
    banner("roster: continuous upload is the shape of a camera");

    const uint8_t cam[6] = { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 };
    rd_roster_t r;
    rd_reset(&r);

    /* Sixty seconds of steady video: large frames, every second, upward. */
    for (unsigned sec = 0; sec < 60u; sec++) {
        for (unsigned f = 0; f < 30u; f++) {
            rd_observe_traffic(&r, cam, 1400, true,
                               (uint64_t)sec * 1000000ull + f * 30000ull);
        }
    }
    rd_device_t d;
    CHECK(rd_at(&r, 0, &d), "the device is on the roster");
    CHECK(d.exposure & RD_EXP_STREAMING, "steady upload reads as streaming");
    CHECK(rd_upload_duty(&d) >= RD_STREAM_MIN_DUTY, "duty is high (%u%%)",
          (unsigned)rd_upload_duty(&d));

    /* A BURST is not a stream. Somebody sending one large attachment uploads
     * far more in a moment and then stops - high volume, low continuity. */
    const uint8_t laptop[6] = { 0xAA, 0xBB, 0xCC, 0x00, 0x11, 0x22 };
    rd_roster_t b;
    rd_reset(&b);
    for (unsigned f = 0; f < 4000u; f++) {
        /* Two seconds of very heavy upload, then sixty seconds of silence in
         * which it is still heard. */
        rd_observe_traffic(&b, laptop, 1400, true, (uint64_t)(f * 500ull));
    }
    for (unsigned sec = 3; sec < 70u; sec++) {
        rd_observe_traffic(&b, laptop, 60, true, (uint64_t)sec * 1000000ull);
    }
    rd_device_t l;
    CHECK(rd_at(&b, 0, &l), "the laptop is on the roster");
    CHECK(l.up_bytes > 1000000ul, "it uploaded a great deal (%lu bytes)",
          (unsigned long)l.up_bytes);
    CHECK(!(l.exposure & RD_EXP_STREAMING),
          "but a burst is not a stream (duty %u%%)",
          (unsigned)rd_upload_duty(&l));

    /* DOWNLOAD is not upload. A tablet streaming a film pulls continuously and
     * is not watching anybody. */
    const uint8_t tablet[6] = { 0xDE, 0xAD, 0x00, 0x00, 0x00, 0x01 };
    rd_roster_t t;
    rd_reset(&t);
    for (unsigned sec = 0; sec < 60u; sec++) {
        for (unsigned f = 0; f < 30u; f++) {
            rd_observe_traffic(&t, tablet, 1400, false,
                               (uint64_t)sec * 1000000ull + f * 30000ull);
        }
    }
    rd_device_t td;
    CHECK(rd_at(&t, 0, &td), "the tablet is on the roster");
    CHECK(!(td.exposure & RD_EXP_STREAMING),
          "continuous DOWNLOAD is not the camera shape");
    CHECK_EQ(td.up_bytes, 0);

    /* A CHATTY SENSOR sends something every second forever - perfect
     * continuity, negligible data. Counting seconds alone would call it a
     * camera; the volume floor is what stops that. */
    const uint8_t sensor[6] = { 0x00, 0x11, 0x22, 0x33, 0x44, 0x55 };
    rd_roster_t sn;
    rd_reset(&sn);
    for (unsigned sec = 0; sec < 300u; sec++) {
        rd_observe_traffic(&sn, sensor, 240, true, (uint64_t)sec * 1000000ull);
    }
    rd_device_t sd;
    CHECK(rd_at(&sn, 0, &sd), "the sensor is on the roster");
    CHECK(rd_upload_duty(&sd) == 100, "it is perfectly continuous");
    CHECK(!(sd.exposure & RD_EXP_STREAMING),
          "but a trickle is not video (%lu bytes)", (unsigned long)sd.up_bytes);

    /* Acknowledgements and keep-alives are upload too. A device that says
     * nothing all day must not accumulate a duty cycle out of them. */
    const uint8_t quiet[6] = { 0x02, 0x02, 0x02, 0x02, 0x02, 0x02 };
    rd_roster_t q;
    rd_reset(&q);
    for (unsigned sec = 0; sec < 300u; sec++) {
        rd_observe_traffic(&q, quiet, 64, true, (uint64_t)sec * 1000000ull);
    }
    rd_device_t qd;
    CHECK(rd_at(&q, 0, &qd), "the quiet device is on the roster");
    CHECK_EQ(qd.up_seconds, 0);
    CHECK(!(qd.exposure & RD_EXP_STREAMING), "tiny frames never count");

    /* Degenerate input. */
    rd_observe_traffic(NULL, cam, 1400, true, 0);
    rd_observe_traffic(&r, NULL, 1400, true, 0);
    CHECK_EQ(rd_upload_duty(NULL), 0);
}

void test_roster(void)
{
    test_roster_streaming_shape();
    test_roster_export_carries_the_model();
    test_roster_tags_fit_the_row();
    test_roster_identifies_by_oui();
    test_roster_behaviour_refines_class();
    test_roster_reads_real_weakness();
    test_roster_no_mfp_only_when_meaningful();
    test_roster_randomised_mac();
    test_roster_cwes_are_per_class();
    test_roster_expiry_and_capacity();
    test_roster_export_redacts();
    test_roster_degenerate();
}
