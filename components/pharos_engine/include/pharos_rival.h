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
#define PRV_NOTE_SPAM        (1u << 3) /* raw advertisement flood in view   */
#define PRV_NOTE_PAIR_SPAM   (1u << 4) /* pairing-popup spam specifically   */

/* PAIRING-POPUP SPAM, WHICH IS THE ONE ATTACK THAT NAMES ITSELF.
 *
 * The trick every one of these tools ships with - Flipper, Marauder, the
 * Android apps - is to broadcast the advertisements a phone treats as "a new
 * accessory is nearby", over and over, so the target's screen fills with
 * pairing dialogs. Apple's Nearby Action (company 0x004C, type 0x0F) and
 * Proximity Pairing (type 0x07) are the payloads; Google Fast Pair (service
 * 0xFE2C) is the Android equivalent.
 *
 * The advertisements themselves are ORDINARY - a real pair of AirPods sends
 * the same kind. What makes it an attack is the DIVERSITY: a genuine room has
 * a few accessories each advertising the one model they are, while a spammer
 * cycles through dozens of model and action codes from rotating addresses in
 * seconds. So the test is how many distinct model bytes appear, not how many
 * advertisements do - which is also why it does not fire in an airport. */
#define PRV_SPAM_MODELS   6  /* distinct model/action codes in the window */
#define PRV_SPAM_ADVS    20  /* ...over at least this many advertisements */
#define PRV_SPAM_WINDOW_US 4000000ull

/* THE SPAM THAT DIVERSITY CANNOT SEE.
 *
 * The model-diversity test above is the right test for the spam that cycles
 * payloads, and it is blind to the spam that does not. Several of the common
 * tools pick ONE payload - the iOS dialog that is most annoying, or the one
 * that crashes a particular version - and repeat it as fast as the radio will
 * go. One model code, thousands of advertisements: diversity says one, and the
 * raw-rate test only fires above sixty a second, so a steady twenty-a-second
 * single-payload flood fell straight between them. That is what "it doesn't
 * detect all the spamming attacks" meant, and it was true.
 *
 * The signal that catches it is IDENTITY, not payload. A real accessory keeps
 * its address for minutes at a time - Apple and Google both rotate on a
 * roughly fifteen-minute schedule - whereas every one of these tools draws a
 * fresh random address for each advertisement, because reusing one lets a
 * phone dismiss it permanently. So: how many DISTINCT addresses were heard
 * carrying a pairing payload in the window? Eight in four seconds is not a
 * room with accessories in it. */
#define PRV_SPAM_ADDRS    8
#define PRV_PAIR_ADDR_SLOTS 24

/* HOW LONG A DEVICE STAYS ON THE LIST AFTER IT STOPS TRANSMITTING.
 *
 * A Flipper that has been switched off is not in the room, and a lens that
 * goes on reporting it is telling the operator something false - the exact
 * failure this project refuses everywhere else. But dropping it the instant a
 * single advertisement is missed would make the list flicker, because BLE
 * advertisers are intermittent and a passive listener misses plenty.
 *
 * Thirty seconds is long enough to ride out the gaps and short enough that
 * "there is a Flipper here" stops being said within half a minute of it
 * leaving. The row carries how long ago it was last heard, so a fading entry
 * is visible as one rather than presented as current. */
#define PRV_STALE_US 30000000ull

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

/* One PIECE OF HARDWARE, not one address.
 *
 * BLE addresses are not identities and this lens learned that the hard way: a
 * single Flipper running its pairing-spam broadcasts from a fresh random
 * address for every advertisement, so a table keyed on address showed
 * twenty-four Flippers in a room containing one. Counting addresses and
 * calling them devices is the same mistake twice.
 *
 * So a device is a KIND, and `addresses` is how many addresses that kind was
 * heard from - which is not a defect of the measurement, it is a finding.
 * Hardware that rotates its address dozens of times a minute is hardware doing
 * something, and the number says so.
 *
 * The cost is that two genuine Flippers in one room read as one. That is the
 * honest trade: with address randomisation a passive listener CANNOT count
 * them, and reporting "Flipper Zero, 24 addresses" is far less wrong than
 * reporting twenty-four Flippers. */
typedef struct {
    uint8_t addr[6];   /* the most-heard address for this kind */
    prv_kind_t kind;
    char name[PR_NAME_MAX + 1];
    int8_t best_rssi;
    uint32_t sightings;
    uint16_t addresses; /* distinct addresses this kind was heard from */
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

    /* Pairing-popup spam, over a window that ACTUALLY slides.
     *
     * The first version kept one counter and zeroed it every four seconds,
     * which is a tumbling window wearing a sliding window's comment. Against
     * continuous spam it produced a four-second sawtooth: the counters reset,
     * the attack went undetected until they refilled, and the verdict dropped
     * out of IN USE and climbed back in - over and over, while nothing in the
     * room had changed. A detector whose reading oscillates during a steady
     * attack teaches its operator to distrust it.
     *
     * Each model now carries its own last-seen stamp and expires on its own,
     * and the advertisements are counted in per-second buckets summed across
     * the window. Nothing is ever reset wholesale, so there is no cliff to
     * fall off. */
    uint8_t pair_models[16];
    uint64_t pair_model_us[16];
    uint8_t pair_n_models;
    uint32_t pair_adv_cnt[8];
    uint32_t pair_adv_sec[8];

    /* Addresses seen carrying a pairing payload, each expiring on its own.
     * Hashes rather than addresses: this only ever answers "how many
     * different ones", never "which", so keeping the addresses would be
     * storing identifying material for no purpose. */
    uint32_t pair_addr_h[PRV_PAIR_ADDR_SLOTS];
    uint64_t pair_addr_us[PRV_PAIR_ADDR_SLOTS];
    uint8_t pair_n_addrs;
    uint64_t first_us, last_us;
} prv_state_t;

typedef struct {
    uint8_t score;
    uint8_t raw_score;
    uint8_t families;
    uint8_t notes;
    prv_band_t band;

    uint8_t n_devices;      /* distinct KINDS, not addresses */
    uint16_t n_addresses;   /* addresses across all of them  */
    uint8_t n_flipper;
    uint8_t n_pwnagotchi;
    uint16_t worst_addresses; /* address rotation by the most capable device */
    uint32_t worst_age_s;     /* how long since the most capable was heard  */
    uint8_t n_wifi_tools;
    prv_kind_t worst_kind;
    uint8_t worst_addr[6];
    char worst_name[PR_NAME_MAX + 1];
    int8_t worst_rssi;
    uint32_t peak_adv_per_s;
    uint8_t pair_models;   /* distinct pairing model/action codes seen   */
    uint32_t pair_advs;    /* pairing-popup advertisements in the window */
    uint8_t pair_addrs;    /* distinct addresses those came from         */
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

/* True for Hak5's registered OUI (00:13:37). A rogue-AP appliance's network
 * name can be changed in a minute; the OUI is what the vendor shipped, so this
 * sees a renamed Pineapple that a name match never could. */
bool prv_is_hak5_oui(const uint8_t addr[6]);

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

/* The same, but dropping anything not heard within PRV_STALE_US of now_us, so
 * the list cannot disagree with the count the verdict reported. Pass now_us=0
 * to list everything ever seen. */
bool prv_device_at_now(const prv_state_t *s, unsigned index, uint64_t now_us,
                       prv_device_t *out);

const char *prv_kind_name(prv_kind_t k);
const char *prv_kind_note(prv_kind_t k); /* what that hardware can actually do */
const char *prv_band_name(prv_band_t b);
const char *prv_band_advice(prv_band_t b);

#ifdef __cplusplus
}
#endif

#endif /* PHAROS_RIVAL_H */
