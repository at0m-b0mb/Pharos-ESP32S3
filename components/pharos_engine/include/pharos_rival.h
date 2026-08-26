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
    PRV_KIND_ROGUE_AP,       /* a dev board offering an OPEN network      */
    PRV_KIND_IMPLANT,        /* a cable or plug with a radio hidden in it */
    PRV_KIND_PINEAPPLE,      /* a rogue-AP appliance's default network    */
    PRV_KIND_FLIPPER,        /* Flipper Zero, over Bluetooth              */
    PRV_KIND_COUNT,
} prv_kind_t;

/* Which evidence families are speaking. Presence alone is deliberately not
 * enough to alarm; see the caps below. */
#define PRV_FAM_PRESENT  (1u << 0) /* a tool is here                      */
#define PRV_FAM_CAPABLE  (1u << 1) /* and it is a highly capable one      */
#define PRV_FAM_ACTIVE   (1u << 2) /* and something is being DONE with it */
#define PRV_FAM_ONE_RADIO (1u << 3) /* many addresses, one signal level    */

#define PRV_NOTE_BREDR_BLIND (1u << 0) /* classic Bluetooth is invisible  */
#define PRV_NOTE_SUBGHZ_BLIND (1u << 1)/* a Flipper's other radios too    */
#define PRV_NOTE_FULL        (1u << 2) /* more devices than the table holds */
#define PRV_NOTE_SPAM        (1u << 3) /* raw advertisement flood in view   */
#define PRV_NOTE_PAIR_SPAM   (1u << 4) /* pairing-popup spam specifically   */
#define PRV_NOTE_COHERENT    (1u << 5) /* one radio behind many addresses   */
#define PRV_NOTE_MANY_NAMES  (1u << 6) /* ...and it is wearing several      */

/* NAMES CANNOT BE TRUSTED WHILE A FLOOD IS RUNNING.
 *
 * Rival identifies hardware by the name it announces, and during a rotation
 * flood the names are chosen by the attacker. One of the four this firmware
 * was measured against is literally "Flipper" - so the lens reported "Flipper
 * Zero present, 245 addresses" while the operator's actual Flipper, renamed
 * r3gen, was not advertising its own identity at all.
 *
 * That is the worst shape of error available here: a confident, specific,
 * WRONG identification, produced by the very attack it was looking at. The
 * flood can make this device say anything it likes.
 *
 * So an identification assembled from a flood is reported as the flood, and
 * the operator is told the names in view are attacker-controlled. */
#define PRV_NOTE_NAMES_FORGED (1u << 7)

/* DEBRIS IS AN IDENTIFICATION WITH NOTHING STEADY BEHIND IT.
 *
 * The first attempt at this discarded any identification carrying more than a
 * dozen addresses, and a test caught it immediately: Rival aggregates by KIND,
 * so a REAL Flipper advertising steadily alongside a flood of spoofed
 * "Flipper" names is ONE record holding both. Suppressing on address count
 * threw the genuine device away with the debris - which would have been a
 * worse bug than the one being fixed, and silent.
 *
 * The discriminator is the ratio instead. A rotation flood draws a fresh
 * address per advertisement, so its sightings and its address count are
 * nearly equal. A device that is actually there keeps an address and is heard
 * from it repeatedly, so its sightings far exceed its addresses - and it
 * survives however much spoofed traffic is piled on top of it.
 *
 * Measured: the operator's real Flipper was NOT advertising at all, so every
 * "Flipper" sighting was spam - 245 sightings across 244 addresses, ratio 1.0.
 * The test fixture's genuine device sits at 2.2. */
#define PRV_STEADY_NUM 3  /* debris when sightings * 2 < addresses * 3, */
#define PRV_STEADY_DEN 2  /* i.e. fewer than 1.5 sightings per address  */

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

/* ---- AND THE SPAM THAT VARIES NEITHER PAYLOAD NOR ADDRESS -------------
 *
 * Both tests above are DIVERSITY tests: how many models, or how many
 * addresses. An iOS-targeted flood measured on hardware satisfied neither -
 * three action codes from a single address - and was scored CLEAR.
 *
 * It was, at the same moment, sending eighty-eight popup-triggering
 * advertisements in a four-second window. Twenty-two a second.
 *
 * Volume is weak evidence for most of what this device looks at, because busy
 * rooms are busy. It is not weak here, and the reason is worth stating: an
 * Apple Continuity advertisement of type 0x0F or 0x07 exists to raise a
 * DIALOG on a stranger's phone. Twenty-two a second is not a chatty
 * accessory, it is the attack itself - the volume IS the mechanism. A real
 * accessory bursts briefly when a case opens and then stops; it does not
 * sustain this across a whole window.
 *
 * MEASURED PER ADDRESS, AND THAT PART IS LOAD-BEARING.
 *
 * A first attempt tested the total rate and a standing test caught it: three
 * headphones in a cafe, each advertising the one model it actually is as
 * often as it likes, also produce ninety advertisements in four seconds. That
 * room is not an attack, and calling it one is how an operator learns to
 * ignore the screen.
 *
 * Both ends of the distribution are suspicious and the middle is not. Many
 * addresses is rotation (the test above). One address hammering is a single
 * radio raising dialogs as fast as it can. Three addresses at thirty each is
 * three accessories. So the number that matters is the busiest SINGLE
 * address: thirty in the cafe, eighty-eight in the measured attack. */
#define PRV_SPAM_PAIR_RATE 60  /* pairing payloads from ONE address */

/* ---- THE SPAM THAT CARRIES NO PAYLOAD WE RECOGNISE -------------------
 *
 * Both tests above need a PAIRING PAYLOAD: one counts distinct model codes,
 * the other counts addresses carrying such a payload. Every published
 * detector surveyed keys on the same thing - the Apple company ID with the
 * proximity-pairing type byte and its model bytes.
 *
 * So a flood that carries neither falls between all of them. Measured on real
 * hardware while a Flipper ran a name-rotation flood: 244 distinct addresses
 * in view, four rotating display names, and "popup models / advs" reading
 * 0/0. The device was looking straight at the attack and scoring it CAPABLE,
 * which means "a tool is present" - not "a tool is being used on you".
 *
 * The fix is to stop reading the payload at all.
 *
 * A radio has a position and a transmit power, and those do not change when
 * it rewrites its address. So a flood of addresses from ONE transmitter
 * arrives in a tight band of signal levels, while a room genuinely full of
 * different accessories does not - they are at different distances, and their
 * levels are spread across tens of dB. That is the whole test, and it works
 * on any payload, including one nobody has catalogued yet.
 *
 * It is the same physical argument attribution uses against a spoofed source
 * MAC (see pharos_attrib.h): the address is written by the attacker, the
 * signal level is written by the world.
 *
 * The window is deliberately narrow. RSSI wanders a few dB on multipath, so
 * anything tighter rejects the true case; anything wider starts absorbing a
 * genuinely busy room. */
#define PRV_COHERE_DB      6   /* how tight the level cluster must be   */
#define PRV_COHERE_ADDRS  14   /* distinct addresses inside that band   */
#define PRV_COHERE_SLOTS  64   /* how many we track at once             */
#define PRV_COHERE_WINDOW_US 6000000ull

/* ONE RADIO WEARING SEVERAL NAMES.
 *
 * The second half of the same observation. A real room contains devices that
 * each announce one name; a name-rotation flood is one device announcing
 * several. Three distinct names inside the coherent level cluster is not an
 * accessory drawer. */
#define PRV_COHERE_NAMES   3
#define PRV_NAME_SLOTS     8

/* HOW LONG A FLOOD STAYS SEEN AFTER THE COUNT DIPS.
 *
 * Without this the verdict oscillated on hardware between IN USE 74 and
 * CAPABLE 40 several times a minute, while the attack ran steadily and
 * nothing in the room changed - because the address count wanders across the
 * threshold as entries expire. It is the same fault the acoustic engine had,
 * and it teaches the same lesson: a detector that changes its mind during a
 * steady attack is one its operator stops believing.
 *
 * Latched on entry and held, rather than re-decided every evaluation. */
#define PRV_FLOOD_HOLD_US 8000000ull

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

/* ...EXCEPT THAT THIRTY SECONDS IS THE ANSWER TO THE WRONG QUESTION.
 *
 * The window above is sized for the quietest thing this engine can see - a
 * beacon that advertises once every ten seconds needs that much patience or it
 * would flicker in and out of the list. But it was being applied to everything,
 * including a Flipper that had been advertising several times a second, and
 * switching one off then watching the screen keep claiming it for half a minute
 * is a tool that looks broken. It was reported exactly that way: "it took some
 * time to remove the Flipper Zero after I closed it".
 *
 * How long silence has to last before it means something is not a constant. It
 * is a property of the device: silence is only evidence in proportion to how
 * talkative the thing was. So the window is derived from the cadence actually
 * observed - eight missed advertisements' worth - and the constant above
 * becomes what it should always have been, the CEILING for something heard so
 * rarely that no cadence can be measured.
 *
 * A Flipper heard twice a second is dropped about five seconds after it stops.
 * A beacon heard every ten seconds still gets the full thirty. Neither number
 * had to be chosen for the other. */
/* ...AND SILENCE ONLY COUNTS WHILE SOMEBODY WAS LISTENING.
 *
 * The window above is measured against the wall clock, which is right only if
 * this lens is always receiving. It is not. One radio serves every watch, and
 * on the home ring the watchtower hands it round - so while another watch has
 * it, Rival hears nothing at all. After ten seconds of somebody else's turn a
 * Flipper sitting on the desk was declared gone, and the screen said the room
 * was empty while the thing was still in it.
 *
 * That is the same error this engine already refuses elsewhere in a different
 * costume: it was treating an absence of evidence as evidence of absence,
 * having failed to notice it had its ears shut.
 *
 * So staleness is measured on a clock that ONLY ADVANCES WHILE SCANNING. A
 * device is gone when we listened long enough to have heard it and did not. */
#define PRV_SILENCE_MULTIPLE 8ull
#define PRV_STALE_MIN_US 5000000ull /* never twitchier than five seconds */

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
    uint64_t last_listen_us; /* the listen clock when last heard */
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
    uint16_t pair_addr_cnt[PRV_PAIR_ADDR_SLOTS]; /* how loud each one is */
    uint8_t pair_n_addrs;

    /* Payload-independent spam: addresses bucketed by the signal level they
     * arrived at, and the names seen inside the strongest bucket. Hashes
     * again - the question is only ever "how many different ones". */
    uint32_t coh_addr_h[PRV_COHERE_SLOTS];
    int8_t coh_rssi[PRV_COHERE_SLOTS];
    uint64_t coh_us[PRV_COHERE_SLOTS];
    uint8_t coh_n;

    uint32_t coh_name_h[PRV_NAME_SLOTS];
    uint64_t coh_name_us[PRV_NAME_SLOTS];
    uint8_t coh_n_names;
    uint64_t coh_flood_us;  /* when the cluster last cleared the bar */

    /* Time spent actually receiving, advanced by the lens. Staleness is
     * measured on this, never on the wall clock. */
    uint64_t listen_us;

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

    /* What the coherence test found, reported so the operator can check the
     * reasoning rather than take "one radio" on trust. */
    uint16_t pair_worst_addr; /* advertisements from the busiest address */
    uint8_t cohere_addrs;   /* addresses inside the level cluster */
    uint8_t cohere_names;   /* distinct names inside it           */
    int8_t cohere_rssi;     /* the level they clustered at        */
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

/* As above, plus whether the network is OPEN.
 *
 * The extra bit is what separates a Marauder's evil portal from a smart plug:
 * both are ESP32s running an access point, and only one of them needs the
 * network to be joinable without a key. See prv_is_devboard_oui(). */
void prv_observe_beacon_ex(prv_state_t *s, const uint8_t bssid[6],
                           const char *ssid, uint8_t ssid_len, bool whisper,
                           bool open_network, int8_t rssi, uint64_t t_us);

/* Kept as the plain-SSID form for callers with nothing else to offer. */
void prv_observe_ssid(prv_state_t *s, const uint8_t bssid[6], const char *ssid,
                      uint8_t ssid_len, int8_t rssi, uint64_t t_us);

/* Is this the Pwnagotchi advertisement source address? */
bool prv_is_pwnagotchi_addr(const uint8_t addr[6]);

/* True for Hak5's registered OUI (00:13:37). A rogue-AP appliance's network
 * name can be changed in a minute; the OUI is what the vendor shipped, so this
 * sees a renamed Pineapple that a name match never could. */
bool prv_is_hak5_oui(const uint8_t addr[6]);

/* True when this address belongs to one of Espressif's IEEE-assigned prefixes
 * - the radios inside Marauder, the Deauther family and the evil-portal
 * builds. An access point on one is a dev board, not a manufactured router.
 *
 * NOT proof of an attack: ESPHome sensors, Tasmota plugs and Shelly relays are
 * the same silicon, and plenty of them run a setup access point. See the
 * definition for what raises it and what does not. Returns false for
 * locally-administered addresses, where an OUI means nothing. */
bool prv_is_devboard_oui(const uint8_t addr[6]);

/* THE CABLE WITH A RADIO IN IT.
 *
 * O.MG cables and plugs look exactly like the charging cable they replace and
 * carry an ESP8266 that stands up an access point for command and control.
 * Espressif's assignment for that part is DC:4F:22 - already covered by the
 * dev-board table - but the documented indicator for these implants is
 * DE:4F:22, the SAME prefix with the locally-administered bit set.
 *
 * That bit is why this needs its own test: prv_is_devboard_oui() refuses
 * locally-administered addresses outright, because in general an OUI table
 * says nothing about an address a device made up. This is the narrow, known
 * exception - a specific made-up prefix that is documented as belonging to a
 * specific implant.
 *
 * NOT proof. Any device can set any locally-administered address, so this is a
 * strong hint rather than an identification, and a carefully configured
 * implant will not be in access-point mode at all. A quiet result here means
 * nothing was heard, never that a cable is safe. */
bool prv_is_implant_oui(const uint8_t addr[6]);

/* How long THIS device may be silent before it is treated as gone, from the
 * cadence it was actually heard at. Between PRV_STALE_MIN_US and
 * PRV_STALE_US. One rule, so the count and the list cannot disagree. */
uint64_t prv_expiry_us(const prv_device_t *d);

/* True when `d` has been silent past its own window at `now_us`. */
bool prv_is_stale(const prv_device_t *d, uint64_t now_us);

/* Advance the listening clock by dt_us. The lens calls this only while the
 * receiver is actually on this lens' band; see PRV_SILENCE_MULTIPLE. */
void prv_listen(prv_state_t *s, uint64_t dt_us);

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
