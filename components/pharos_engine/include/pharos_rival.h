/* Pharos - Rival: the other operator's hardware, announcing itself
 *
 * Every other lens in this project looks for an ATTACK. This one looks for the
 * TOOLS, because a defender walking a site wants to know what capability is in
 * the room before anything has been done with it - and because most of this
 * hardware cannot help introducing itself.
 *
 * ---------------------------------------------------------------------------
 * THE WORD THIS LENS REFUSES TO USE IS "HOSTILE"
 *
 * A Flipper Zero in a room is not an attack. It is a tool, sold openly, and
 * the person carrying it is far more likely to be a hobbyist, a locksmith or
 * another security engineer than a thief. The same is true of an ESP32 on a
 * desk and of a laptop running airodump.
 *
 * So Rival reports PRESENCE and CAPABILITY, never intent. The bands are about
 * how much capability is in the room and whether any of it is actually being
 * USED - which is a different question, and the only one where this lens is
 * entitled to raise its voice. A tool sitting idle scores; a tool sitting idle
 * does not alarm.
 *
 * ---------------------------------------------------------------------------
 * WHAT IT CAN AND CANNOT SEE
 *
 * BLE and 2.4 GHz Wi-Fi management frames, which is what this silicon has.
 * That covers a great deal - most of this hardware advertises over BLE, and
 * the Wi-Fi tools announce themselves in beacons - but it explicitly does NOT
 * cover:
 *
 *   - Bluetooth Classic. The ESP32-S3 has no BR/EDR radio at all.
 *   - Sub-GHz, NFC, infrared, iButton. A Flipper's most interesting radios are
 *     ones this device cannot hear. Rival sees the Flipper's Bluetooth and
 *     nothing else it does.
 *   - Anything switched off, or in a bag, or not transmitting.
 *
 * An empty Rival screen means "nothing announced itself to this receiver",
 * which is a much smaller claim than "there is nothing here", and the lens
 * says so rather than letting the operator infer otherwise.
 */
#ifndef PHAROS_RIVAL_H
#define PHAROS_RIVAL_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PR_MAX_SIGHTINGS 24
#define PR_NAME_MAX      24

/* What kind of hardware announced itself.
 *
 * Ordered by how much capability it implies, because the headline picks the
 * most capable thing in the room and "most capable" needs a defined order. */
typedef enum {
    PRV_KIND_NONE = 0,
    PRV_KIND_SERIAL_BRIDGE,  /* JDY / AT-09 / HM-10 - a radio on a UART   */
    PRV_KIND_DEV_BOARD,      /* an ESP32 or similar advertising a default */
    PRV_KIND_DEAUTHER,       /* a board whose NAME announces the attack   */
    PRV_KIND_PWNAGOTCHI,     /* handshake collector, announces in beacons */
    PRV_KIND_PINEAPPLE,      /* a rogue-AP appliance's default network    */
    PRV_KIND_FLIPPER,        /* Flipper Zero, over Bluetooth              */
    PRV_KIND_COUNT,
} prv_kind_t;

/* Which evidence families are speaking. Presence alone is deliberately not
 * enough to alarm; see the caps below. */
#define PRV_FAM_PRESENT  (1u << 0) /* a tool is here                      */
#define PRV_FAM_CAPABLE  (1u << 1) /* and it is a highly capable one      */
#define PRV_FAM_ACTIVE   (1u << 2) /* and something is being DONE with it */

#define PRV_NOTE_BREDR_BLIND (1u << 0) /* classic Bluetooth is invisible  */
#define PRV_NOTE_SUBGHZ_BLIND (1u << 1)/* a Flipper's other radios too    */
#define PRV_NOTE_FULL        (1u << 2) /* more devices than the table holds */
#define PRV_NOTE_SPAM        (1u << 3) /* BLE advertisement spam in view   */

/* Presence is capped here. Owning a tool is not an offence, and a lens that
 * alarms on the mere sight of one teaches its operator to ignore it. */
#define PRV_CAP_PRESENCE_ONLY 55

typedef enum {
    PRV_BAND_CLEAR = 0,  /*  0-19  nothing announced itself           */
    PRV_BAND_NOTED,      /* 20-39  ordinary hardware in view          */
    PRV_BAND_CAPABLE,    /* 40-59  something genuinely capable is here */
    PRV_BAND_ACTIVE,     /* 60-100 and it is being used               */
    PRV_BAND_COUNT,
} prv_band_t;

typedef struct {
    uint8_t addr[6];
    prv_kind_t kind;
    char name[PR_NAME_MAX + 1];
    int8_t best_rssi;
    uint32_t sightings;
    uint64_t first_us, last_us;
    bool ble;      /* seen over Bluetooth rather than Wi-Fi */
    bool in_use;
} prv_device_t;

typedef struct {
    prv_device_t dev[PR_MAX_SIGHTINGS];
    unsigned n;
    bool full;
    /* BLE advertisement spam - the Flipper's signature party trick - shows up
     * as a burst of DISTINCT addresses in a very short window. Counted per
     * second so a busy room and a spam burst can be told apart. */
    uint32_t adv_sec;
    uint32_t adv_distinct[8]; /* rolling per-second distinct-address counts */
    uint8_t adv_slot;
    uint64_t first_us, last_us;
} prv_state_t;

typedef struct {
    uint8_t score;
    uint8_t raw_score;
    uint8_t families;
    uint8_t notes;
    prv_band_t band;

    uint8_t n_devices;
    uint8_t n_flipper;
    uint8_t n_pwnagotchi;
    uint8_t n_wifi_tools;
    prv_kind_t worst_kind;
    uint8_t worst_addr[6];
    char worst_name[PR_NAME_MAX + 1];
    int8_t worst_rssi;
    uint32_t peak_adv_per_s;
    const char *headline;
} prv_verdict_t;

void prv_reset(prv_state_t *s);

/* A BLE advertisement, with its raw payload - which is what actually carries
 * the identifying signatures for a passive listener. `name` and `adv` may both
 * be NULL. */
void prv_observe_ble_adv(prv_state_t *s, const uint8_t addr[6], const char *name,
                         const uint8_t *adv, uint8_t adv_len, int8_t rssi,
                         uint64_t t_us);

/* Name-only form, for callers with no payload to hand over. */
void prv_observe_ble(prv_state_t *s, const uint8_t addr[6], const char *name,
                     int8_t rssi, uint64_t t_us);

/* A Wi-Fi beacon. Some of this hardware announces itself as an access point
 * rather than over Bluetooth, and one of them - the Pwnagotchi - does not use
 * an SSID at all.
 *
 * `whisper` is true when the beacon carried the Pwnagotchi advertisement
 * elements (222 / 224-226). That, and the hardcoded de:ad:be:ef:de:ad source
 * address, are two INDEPENDENT signals for the same device: forks change the
 * address far more readily than they change the protocol, so either alone is
 * enough and both together is stronger. `ssid` may be the unit's own name,
 * which the radio lifts out of the whisper payload. */
void prv_observe_beacon(prv_state_t *s, const uint8_t bssid[6], const char *ssid,
                        uint8_t ssid_len, bool whisper, int8_t rssi,
                        uint64_t t_us);

/* Kept as the plain-SSID form for callers with nothing else to offer. */
void prv_observe_ssid(prv_state_t *s, const uint8_t bssid[6], const char *ssid,
                      uint8_t ssid_len, int8_t rssi, uint64_t t_us);

/* Is this the Pwnagotchi advertisement source address? */
bool prv_is_pwnagotchi_addr(const uint8_t addr[6]);

void prv_evaluate(const prv_state_t *s, uint64_t now_us, prv_verdict_t *out);

/* Classify a device name on its own. Exposed so the host tests can hold it to
 * account directly, including for the names it must REFUSE. */
prv_kind_t prv_classify_name(const char *name, uint8_t len, bool ble);

/* Classify a RAW BLE advertisement.
 *
 * This exists because name matching cannot find a Flipper Zero and never
 * could. Pharos scans PASSIVELY - the transmit fence forbids sending a
 * SCAN_REQ - so it only ever sees the advertisement packet, and most devices
 * put their name in the SCAN RESPONSE, which a passive listener is not
 * entitled to ask for. Measured in a real room: 23 advertisers, 19 of them
 * nameless.
 *
 * What the Flipper does put in the advertisement is a 128-bit service UUID,
 * and two of its bytes identify the device and its shell colour. That is
 * visible passively, so that is what is matched.
 *
 * Returns the kind; `colour` (may be NULL) receives a static string for the
 * Flipper's case. */
prv_kind_t prv_classify_adv(const uint8_t *data, uint8_t len,
                            const char **colour);

bool prv_device_at(const prv_state_t *s, unsigned index, prv_device_t *out);

const char *prv_kind_name(prv_kind_t k);
const char *prv_kind_note(prv_kind_t k); /* what that hardware can actually do */
const char *prv_band_name(prv_band_t b);
const char *prv_band_advice(prv_band_t b);

#ifdef __cplusplus
}
#endif

#endif /* PHAROS_RIVAL_H */
