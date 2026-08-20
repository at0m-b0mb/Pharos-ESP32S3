/* Pharos - Vigil: is a tracker travelling with you?
 *
 * Pure C. The ESP32-S3's Bluetooth radio has sat idle in this firmware since
 * v1.0; this is what it is for. An item tracker - AirTag, Tile, SmartTag -
 * costs a few pounds, fits in a coat lining or a wheel arch, and reports its
 * position to a global crowd-sourced network. It is the cheapest surveillance
 * device ever mass-produced, and the person it is used against is usually the
 * last to know.
 *
 * THE HARD PART is not seeing trackers. Trackers are everywhere - a café at
 * lunchtime has a dozen, all of them minding their own business in other
 * people's bags. Seeing one means nothing. What matters is whether one is
 * still with you after you have MOVED, and that is a different question.
 *
 * Pharos has no GPS. What it does have is a Wi-Fi receiver that already knows
 * what the access points around it look like - so movement is inferred from
 * the world changing rather than from a position fix:
 *
 *     A LOCALE is the set of access points currently audible. When that set
 *     turns over substantially, you are somewhere else. A tracker that is
 *     present in several distinct locales is travelling with you, whatever
 *     the reason.
 *
 * That is the whole idea, and it is honest in a way a signal-strength alarm is
 * not: it cannot be fooled by standing next to somebody's rucksack.
 *
 * WHY ADDRESS ROTATION DOES NOT DEFEAT THIS. Find My devices rotate their
 * Bluetooth address, which is exactly what makes casual detection hard. But
 * the rotation period depends on state: while the owner is nearby it rotates
 * every ~15 minutes, and once SEPARATED from its owner it holds the same
 * address far longer. The separated case is the one that matters - a tracker
 * planted on somebody is by definition away from its owner - so the device we
 * most want to catch is the one that stays addressable. Vigil says plainly
 * that it undercounts rotating tags rather than pretending otherwise.
 *
 * WHAT IT REFUSES TO DO:
 *
 *   - It never says "you are safe" or "you are not being tracked". A receiver
 *     that hears one radio at a time, for a few minutes, cannot support that
 *     sentence, and for this subject in particular a false reassurance is
 *     worse than no answer at all.
 *   - It never claims intent. A tag travelling with you may have been dropped
 *     in your bag by a friend, may be your own, may be in a parcel you are
 *     carrying. The engine reports "travelling with you", which is a fact, and
 *     leaves "why" to a human.
 *   - Your own devices are expected to follow you, so they can be marked known
 *     and are then excluded rather than fudged.
 */
#ifndef PHAROS_VIGIL_H
#define PHAROS_VIGIL_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PV_MAX_TAGS 32
#define PV_MAX_KNOWN 16   /* devices the operator marked as their own    */
#define PV_MAX_LOCALES 8  /* distinct places remembered                  */

/* How much of the access-point landscape must turn over before we accept that
 * we are somewhere else. Two thirds is deliberately demanding: walking to the
 * other end of one office should not count as travelling. */
#define PV_LOCALE_CHANGE_PERMIL 660

typedef enum {
    PV_KIND_UNKNOWN = 0,
    PV_KIND_FINDMY,       /* Apple Find My, owner nearby            */
    PV_KIND_FINDMY_LOST,  /* Apple Find My, SEPARATED from its owner */
    PV_KIND_TILE,
    PV_KIND_SMARTTAG,     /* Samsung Galaxy SmartTag                */
    PV_KIND_CHIPOLO,      /* Chipolo, its own service UUID          */
    PV_KIND_FLIPPER,      /* a Flipper Zero advertising its presence */
    PV_KIND_SERIAL,       /* a bare BLE serial bridge - see below    */
    PV_KIND_GENERIC,      /* a persistent advertiser we cannot name  */
    PV_KIND_COUNT,
} pv_kind_t;

typedef enum {
    PV_BAND_CLEAR = 0,   /*  0-19  nothing has followed across a move   */
    PV_BAND_SEEN,        /* 20-44  trackers nearby - normal in public   */
    PV_BAND_PERSISTENT,  /* 45-69  one has stayed with you a long time  */
    PV_BAND_FOLLOWING,   /* 70-100 present across several locales       */
    PV_BAND_COUNT,
} pv_band_t;

#define PV_NOTE_ROTATION  (1u << 0) /* rotating tags are undercounted   */
#define PV_NOTE_ONE_PLACE (1u << 1) /* you have not moved yet           */
#define PV_NOTE_FULL      (1u << 2) /* more advertisers than we track   */
#define PV_NOTE_KNOWN     (1u << 3) /* something matched your own list  */
#define PV_NOTE_FLIPPER   (1u << 4) /* a Flipper Zero is in the room    */
#define PV_NOTE_SERIAL    (1u << 5) /* a bare serial bridge is present  */

/* WHAT THIS RADIO CANNOT SEE, STATED IN THE HEADER SO NOBODY HAS TO GUESS.
 *
 * The ESP32-S3 has Bluetooth LOW ENERGY only - no BR/EDR, no Classic. That is
 * a property of the silicon, not of this firmware, and it has one consequence
 * worth being loud about:
 *
 *   The card skimmers people most want to find - the HC-05 and HC-06 modules
 *   documented in fuel-pump skimmers for a decade - are CLASSIC Bluetooth
 *   devices. This radio is deaf to them. A quiet Vigil screen is not evidence
 *   that a pump is clean, and anyone told otherwise has been misled.
 *
 * What it CAN see is the newer generation of BLE serial bridges (the JDY, AT-09
 * and MLT-BT05 family), which turn up in the same role. PV_KIND_SERIAL reports
 * those - as a bridge, never as a "skimmer", because the identical module sits
 * inside hobby electronics, scoreboards and cheap door locks. Naming a Bluetooth
 * speaker as a card skimmer would be the single most damaging false positive
 * this project could produce. */
#define PV_BREDR_BLIND 1
#define PV_NOTE_SHORT     (1u << 4) /* too little time to judge         */

typedef struct {
    uint8_t addr[6];
    uint8_t addr_type;
    pv_kind_t kind;
    bool in_use;

    uint32_t sightings;
    uint8_t locale_mask;   /* bit per locale index it was seen in */
    uint8_t n_locales;
    int8_t best_rssi;
    uint64_t first_us, last_us;
} pv_tag_t;

typedef struct {
    pv_tag_t tags[PV_MAX_TAGS];
    unsigned n;
    bool full;

    uint8_t known[PV_MAX_KNOWN][6];
    unsigned n_known;

    /* Locale tracking. The current access-point set is summarised as a small
     * bloom-ish signature; a substantial change starts a new locale. */
    uint32_t locale_sig[PV_MAX_LOCALES];
    unsigned n_locales;
    uint32_t cur_sig;
    unsigned cur_locale;

    /* What the accelerometer says about whether anybody went anywhere. True
     * by default, so a board with no IMU behaves exactly as it did before
     * there was one - see pv_set_moved. */
    bool moved;
    bool moved_known;
    uint64_t first_us, last_us;
} pv_state_t;

typedef struct {
    uint8_t score;
    uint8_t raw_score;
    uint8_t ceiling;
    uint8_t notes;
    pv_band_t band;

    uint8_t n_tags;        /* trackers seen at all              */
    uint8_t n_following;   /* ... present in 2+ locales         */
    uint8_t n_locales;     /* distinct places so far            */

    uint8_t worst_addr[6];
    pv_kind_t worst_kind;
    uint8_t worst_locales;
    uint32_t worst_minutes;

    const char *headline;
} pv_verdict_t;

void pv_reset(pv_state_t *s);

/* Mark an address as the operator's own; it is then excluded from following
 * verdicts. Your own earbuds are supposed to travel with you. */
bool pv_mark_known(pv_state_t *s, const uint8_t addr[6]);

/* Feed the current access-point landscape. `sig` is any stable summary of the
 * audible BSSIDs (the lens supplies a hash); when it differs enough from the
 * current one, a new locale begins. */
void pv_observe_locale(pv_state_t *s, uint32_t sig, uint64_t t_us);

/* DID THE PERSON ACTUALLY MOVE?
 *
 * A locale change is EVIDENCE of movement, not proof of it. Access points
 * switch off at night, a neighbour reboots a router, you turn round in a big
 * building and half the estate drops out of earshot - the signature turns over
 * while you sat still, and a tracker that was merely nearby the whole time
 * starts to look like one that followed you. For this subject in particular
 * that is the worst possible way to be wrong: telling somebody they are being
 * stalked when they are not.
 *
 * The board has an accelerometer (see pharos_motion.h), which is wrong in
 * completely different circumstances - it cannot be fooled by a router
 * rebooting, and it cannot see movement in a lift. Two families that fail
 * independently.
 *
 * Tell the engine what it says. `moved` false means the device is confident
 * NOBODY WENT ANYWHERE, and FOLLOWING is then withheld however many locales
 * were recorded. Leave it unset - or pass true - on a board with no IMU, where
 * the honest position is that movement is unknown and must not be a veto. */
void pv_set_moved(pv_state_t *s, bool moved);

/* Feed one BLE advertisement. `data`/`len` are the raw AD structures. */
void pv_observe_adv(pv_state_t *s, const uint8_t addr[6], uint8_t addr_type,
                    int8_t rssi, const uint8_t *data, uint8_t len,
                    uint64_t t_us);

void pv_evaluate(const pv_state_t *s, uint64_t now_us, pv_verdict_t *out);

/* Classify an advertisement payload. Exposed for testing. */
pv_kind_t pv_classify(const uint8_t *data, uint8_t len);

/* One tag from the table, ranked so index 0 is the one that matters most:
 * anything present across several places first, then the longest-present, then
 * the rest. Returns false past the end.
 *
 * A score saying "something is following you" that cannot say WHICH device is
 * a fright with no remedy - the address and the kind are what let somebody
 * actually search a bag. */
bool pv_tag_at(const pv_state_t *s, unsigned index, uint64_t now_us,
               pv_tag_t *out, uint32_t *minutes);

const char *pv_kind_name(pv_kind_t k);
const char *pv_band_name(pv_band_t b);
const char *pv_band_advice(pv_band_t b);

#ifdef __cplusplus
}
#endif

#endif /* PHAROS_VIGIL_H */
