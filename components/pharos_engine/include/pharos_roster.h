/* Pharos - Roster: what is on this network, and what is exposed
 *
 * Pure C, host-tested. The device inventory somebody actually wants - "what
 * are all these things, and is any of them a problem" - built the one way this
 * firmware is allowed to build it: by LISTENING.
 *
 * ---------------------------------------------------------------------------
 * WHY THIS IS PASSIVE, AND WHY THAT IS NOT A COMPROMISE
 *
 * The obvious way to inventory a network is to join it and scan: ARP-sweep the
 * subnet, port-scan each host, fetch CVEs for what answers. Pharos cannot do
 * any of that, and the reason is the whole point of the device - it holds a
 * transmit fence with four CI audits and a link-time trap proving it never
 * sends a frame. An active scanner would tear that up, and with it the one
 * thing that lets somebody hold this up in a building they do not own.
 *
 * It turns out not to matter, because every device on a network announces
 * itself continuously in frames Pharos is already receiving:
 *
 *   - Access points BEACON their name, channel and exact security posture.
 *   - Clients send PROBE REQUESTS carrying their MAC and, often, the names of
 *     networks they will silently rejoin - which is a list of where they have
 *     been and what they will trust.
 *   - Every data and management frame names its transmitter and receiver, so
 *     the set of MACs talking on a channel is the set of devices present.
 *   - BLE devices ADVERTISE their address, name and service UUIDs several
 *     times a second.
 *
 * The first three octets of a MAC are the OUI - the manufacturer - so a device
 * is identified by who made it without asking it anything. That plus what it
 * is doing is enough to say "this is an Apple phone", "this is an Espressif IoT
 * board", "this is a printer", and to flag the ones whose behaviour is a
 * problem.
 *
 * ---------------------------------------------------------------------------
 * WHAT "VULNERABLE" MEANS HERE, HONESTLY
 *
 * Pharos cannot reach a device to test it, so it never claims a specific CVE
 * against a specific host - that would be a fabrication. What it can do is far
 * more defensible and, for a person trying to secure their own space, more
 * useful:
 *
 *   - Flag OBSERVABLE weakness directly: an access point offering WEP or WPA1,
 *     a device that broadcasts a fixed (non-randomised) MAC everywhere it
 *     goes, a client probing for open networks it will auto-join, WPS left on.
 *     These are not guesses; they are read straight off the air.
 *
 *   - Map each device's CLASS to the weakness CATEGORIES (CWE) that class is
 *     known for, as a checklist. "This looks like an IP camera; cameras
 *     commonly ship with hardcoded credentials (CWE-798) and missing auth
 *     (CWE-306) - check those." That is a true statement about a class, framed
 *     as something to verify, not a claim about this unit.
 *
 * Live CVE correlation needs the internet, which needs transmit. Roster
 * instead produces a redacted inventory (pharos_roster_export) that a normal
 * computer - one allowed to talk to the network and to a CVE database - can
 * take the rest of the way. The device gathers; the desktop looks up. The
 * fence stays intact and the CVE step still happens, just not from here.
 */
#ifndef PHAROS_ROSTER_H
#define PHAROS_ROSTER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PR_ROSTER_MAX 64  /* devices tracked at once */
#define PR_ROSTER_NAME 24

/* What a device appears to be, inferred from its OUI and its behaviour. The
 * list is coarse on purpose: a receiver cannot tell a fridge from a kettle,
 * and pretending to would be the kind of false precision this project avoids. */
typedef enum {
    RD_UNKNOWN = 0,
    RD_PHONE,        /* a handset - Apple, Samsung, Google, ... */
    RD_COMPUTER,     /* laptop / desktop NIC vendors            */
    RD_ACCESS_POINT, /* infrastructure: it beacons              */
    RD_ROUTER,       /* AP made by a router/gateway vendor      */
    RD_IOT,          /* a small connected thing (ESP, etc.)     */
    RD_CAMERA,       /* an IP / doorbell camera                 */
    RD_TV_MEDIA,     /* TV, streaming stick, speaker            */
    RD_PRINTER,      /* a network printer                       */
    RD_WEARABLE,     /* watch / tracker / earbuds (BLE)         */
    RD_CLASS_COUNT,
} rd_class_t;

/* How the device was heard - which lets the roster say a thing appeared on
 * two different radios, which is itself information. */
#define RD_SEEN_WIFI (1u << 0)
#define RD_SEEN_BLE  (1u << 1)

/* Observable exposure flags - each read straight off the air, none inferred. */
#define RD_EXP_OPEN        (1u << 0) /* AP with no encryption at all         */
#define RD_EXP_WEP         (1u << 1) /* AP offering WEP                      */
#define RD_EXP_WPA1        (1u << 2) /* AP offering the original WPA/TKIP    */
#define RD_EXP_WPS         (1u << 3) /* AP with WPS advertised               */
#define RD_EXP_FIXED_MAC   (1u << 4) /* a client not randomising its MAC     */
#define RD_EXP_PROBES_OPEN (1u << 5) /* client will auto-join an open SSID   */
#define RD_EXP_NO_MFP      (1u << 6) /* AP without management-frame protection*/
#define RD_EXP_NAME_LEAK   (1u << 7) /* device broadcasts a personal name    */
/* WPS 1.0 still offering PIN registration on a configured, unlocked AP - the
 * configuration behind the external-registrar PIN attack. Read directly from
 * the beacon, not inferred; see pharos_wps.h. */
#define RD_EXP_WPS_PIN     (1u << 8)

/* Uploading continuously - the shape of a camera. See rd_device_t's note: this
 * says what the traffic LOOKS like, never what the device is. A video call and
 * a backup do the same thing, and the honest report names the shape and leaves
 * the conclusion to somebody who can look at the room. */
#define RD_EXP_STREAMING   (1u << 9)

typedef struct {
    uint8_t mac[6];
    rd_class_t klass;
    const char *vendor; /* points into the static OUI table, or NULL */
    char name[PR_ROSTER_NAME + 1]; /* SSID or BLE name, if it leaked one */

    /* THE EXACT MODEL, WHEN THE DEVICE BROADCASTS IT.
     *
     * Access points supporting WPS put their manufacturer and model in every
     * beacon, in cleartext (see pharos_wps.h). That turns "a TP-Link router" -
     * which nobody can look a CVE up against - into "Archer C7 v2", which they
     * can. Empty when the device did not volunteer one. */
    char model[PR_ROSTER_NAME + 1];
    uint8_t seen;       /* RD_SEEN_* bitfield */
    uint16_t exposure;  /* RD_EXP_* bitfield  */
    int8_t rssi;
    uint8_t channel;
    uint32_t sightings;
    uint64_t first_us, last_us;
    bool in_use;
    bool randomised_mac; /* the locally-administered bit is set */

    /* ---- IS THIS THING UPLOADING, CONTINUOUSLY? ----
     *
     * The vendor table names Ring, Wyze, Axis and Hikvision. It cannot name
     * the no-name camera off a marketplace, which is the one somebody is most
     * likely to find hidden in a room they are staying in - and a camera with
     * a randomised address defeats the table entirely.
     *
     * What no camera can hide is that it UPLOADS. Video goes out continuously
     * for as long as it is recording, and that shape is different from
     * everything else on a network: browsing is bursty, a sensor sends a few
     * bytes an hour, a phone is silent while its owner is asleep.
     *
     * So the test is CONTINUITY, not volume. Volume alone would flag a laptop
     * pulling a large download - or miss a low-bitrate camera entirely. What
     * is counted is how many distinct seconds carried an upload from this
     * device, out of the seconds it was heard at all.
     *
     * This is deliberately unable to see inside anything: on a protected
     * network the payload is ciphertext. Only the size of the envelope and
     * the direction are used, both of which are in the clear for everybody. */
    uint32_t up_bytes;    /* bytes sent TOWARDS the access point       */
    uint16_t up_seconds;  /* distinct seconds in which it uploaded     */
    uint16_t heard_seconds; /* distinct seconds it was heard at all    */
    uint32_t last_up_sec; /* the last second counted, to dedupe        */
    uint32_t last_heard_sec;
} rd_device_t;

typedef struct {
    rd_device_t dev[PR_ROSTER_MAX];
    unsigned n;
    uint32_t admitted; /* total ever admitted, for the "table full" case */
} rd_roster_t;

void rd_reset(rd_roster_t *r);

/* Fold in one Wi-Fi sighting. `bssid`/`is_ap` describe an access point when
 * is_ap is true (a beacon); otherwise `mac` is a client. `rsn_flags` are the
 * PHAROS_RSN_F_* bits from the frame. `probed_ssid` is the network a probe
 * request named, or NULL. */
typedef struct {
    bool privacy;  /* the capability Privacy bit was set    */
    bool wpa1;     /* a legacy WPA1 vendor element present   */
    bool tkip;     /* TKIP offered as a pairwise cipher      */
    uint8_t rsn_flags; /* PHAROS_RSN_F_* from the frame       */
} rd_secpost_t;

void rd_observe_wifi(rd_roster_t *r, const uint8_t mac[6], bool is_ap,
                     const char *ssid, const rd_secpost_t *sec, uint8_t channel,
                     int8_t rssi, const char *probed_ssid, uint64_t t_us);

/* Fold in what an access point broadcast about itself: an exact model string
 * and, when the configuration warrants it, the WPS PIN finding. Both come
 * straight out of the beacon - see pharos_wps.h. */
void rd_observe_wps(rd_roster_t *r, const uint8_t bssid[6], const char *vendor,
                    const char *model, bool pin_exposed, uint64_t t_us);

/* Fold in one BLE advertisement. `name` may be NULL; `appearance` is the GAP
 * appearance value or 0. `addr_random` is true for a randomised address. */
void rd_observe_ble(rd_roster_t *r, const uint8_t addr[6], bool addr_random,
                    const char *name, uint16_t appearance,
                    const uint8_t *svc_uuids16, unsigned n_uuids, int8_t rssi,
                    uint64_t t_us);

/* Expire devices unheard for longer than the stale window. */
void rd_expire(rd_roster_t *r, uint64_t now_us);

/* Fold in one DATA frame's envelope: who sent it, how big it was, and whether
 * it was travelling towards the access point. Payload is never touched - on a
 * protected network it is ciphertext anyway. */
void rd_observe_traffic(rd_roster_t *r, const uint8_t mac[6], uint16_t frame_len,
                        bool towards_ap, uint64_t now_us);

/* What fraction of the seconds this device was heard in carried an upload,
 * 0..100. The continuity measure behind RD_EXP_STREAMING. */
uint8_t rd_upload_duty(const rd_device_t *d);

/* How many seconds of continuous upload, and how much data, before a device is
 * called streaming. A camera clears these in well under a minute; a phone
 * checking mail does not. */
#define RD_STREAM_MIN_SECONDS 8
#define RD_STREAM_MIN_DUTY    60  /* per cent of the seconds it was heard  */
#define RD_STREAM_MIN_BYTES   200000ul

unsigned rd_count(const rd_roster_t *r);

/* The i-th device, most-exposed first then strongest signal. */
bool rd_at(const rd_roster_t *r, unsigned i, rd_device_t *out);

/* How many carry at least one exposure flag. */
unsigned rd_exposed_count(const rd_roster_t *r);

const char *rd_class_name(rd_class_t c);
const char *rd_class_icon(rd_class_t c); /* a short glyph-ish tag */

/* The single worst thing about this device, in plain words, or NULL if it has
 * shown nothing worth flagging. Full sentence - for the detail page. */
const char *rd_exposure_headline(const rd_device_t *d);

/* THE SAME FINDING, SHORT ENOUGH FOR A LIST ROW.
 *
 * A row's right-hand column is eleven characters. "WPS on - PIN can be forced"
 * arrived there as "WPS on - PI", which is not a shortened finding, it is a
 * broken one - and on the device that shipped it read as a fault in the
 * display rather than a fact about the network. The list gets a tag that fits;
 * opening the row gets the sentence. Guaranteed <= 11 characters, and
 * test_roster.c asserts it for every flag. */
#define RD_TAG_MAX 11
const char *rd_exposure_tag(const rd_device_t *d);

/* THE CWE CHECKLIST for a device class.
 *
 * Fills `out` with up to `max` weakness-category lines this class is known for
 * - "CWE-798 hardcoded credentials", and so on - as things to CHECK, not
 * claims about this unit. Returns how many were written. A desktop tool can
 * take the exported inventory and turn these into live CVE lookups; on the
 * device they are the honest, offline half of the answer. */
unsigned rd_class_cwes(rd_class_t c, const char **out, unsigned max);

/* One line of vendor:class:mac:exposure per device, redacted, for a companion
 * tool to correlate against a CVE database off the device. Returns bytes
 * written. */
unsigned rd_export(const rd_roster_t *r, char *buf, unsigned cap, bool redact);

/* OUI lookup, exposed for tests and the console. Returns the vendor name for a
 * MAC's first three octets, or NULL. */
const char *rd_vendor(const uint8_t mac[6]);
rd_class_t rd_class_of_oui(const uint8_t mac[6]);

/* How long a device may go unheard before rd_expire drops it. */
#define RD_STALE_US 120000000ull /* two minutes */

#ifdef __cplusplus
}
#endif

#endif /* PHAROS_ROSTER_H */
