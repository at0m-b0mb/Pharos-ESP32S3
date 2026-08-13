/* Pharos - beacon-flood / SSID-spam detection
 *
 * Pure C. This is the lens that watches for the attack the ESP32 world is most
 * famous for: a radio rapidly beaconing hundreds of fabricated network names
 * to spam every phone's Wi-Fi list. Evil-M5Project ships it; so does half of
 * cheap-hardware Twitter. Pharos does the opposite - it detects it, without
 * transmitting a single frame - which is the whole point of the project made
 * concrete against the exact thing it is the inverse of.
 *
 * The detection rests on three things a flood cannot hide, and one thing an
 * honest-but-busy city cannot help doing, which is the false positive we must
 * defeat:
 *
 *   VOLUME    - brand-new SSIDs appear far faster than any real environment
 *               introduces them. A street corner in a city gains a new SSID
 *               every few minutes; a flood invents dozens per second.
 *   EPHEMERAL - flood SSIDs beacon a handful of times and vanish (the attacker
 *               rotates the list), so most names are heard once or twice and
 *               never again. Real networks beacon steadily, forever.
 *   SYNTHETIC - one transmitter wearing many faces uses locally-administered
 *               (software) BSSIDs, and a single flooding tool tends to reuse
 *               structure - the same vendor prefix across dozens of names.
 *
 * The false positive: a dense urban or corporate rooftop genuinely has many
 * SSIDs. So VOLUME alone is capped below the alarm band - a lot of networks is
 * a lot of networks. It only becomes a flood when those names are also
 * ephemeral or synthetic. Many SSIDs is not an attack, exactly as many BSSIDs
 * is not an attack in pharos_twin.
 *
 * And, as everywhere, the confidence ceiling: "these names never beacon again"
 * is a claim a hopping receiver is poorly placed to make, so it is scaled by
 * measured dwell and the verdict never reads certain.
 */
#ifndef PHAROS_FLOOD_H
#define PHAROS_FLOOD_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PF_SSID_MAX     32
#define PF_MAX_SSIDS   128 /* distinct network names tracked */
#define PF_MAX_OUI      32 /* distinct BSSID prefixes tracked */

#define PF_FAM_VOLUME    (1u << 0) /* new names per second        */
#define PF_FAM_EPHEMERAL (1u << 1) /* names seen once, then gone  */
#define PF_FAM_SYNTHETIC (1u << 2) /* software / patterned BSSIDs */

typedef enum {
    PF_BAND_QUIET = 0,   /*  0-19  ordinary airspace                */
    PF_BAND_BUSY,        /* 20-44  many networks, all persistent    */
    PF_BAND_SUSPICIOUS,  /* 45-69  churny names, evidence thin      */
    PF_BAND_FLOOD_LIKELY,/* 70-100 beacon flood likely              */
} pf_band_t;

#define PF_NOTE_THIN_DWELL (1u << 0) /* hopping: churn claim is weak    */
#define PF_NOTE_TABLE_FULL (1u << 1) /* more names than we can track    */
#define PF_NOTE_URBAN      (1u << 2) /* many names, but they persist    */
#define PF_NOTE_SHORT      (1u << 3) /* window too short to judge rate  */

typedef struct {
    uint16_t dwell_permil;
    uint16_t bus_yield_permil;
    uint32_t window_ms;
} pf_context_t;

typedef struct {
    char name[PF_SSID_MAX + 1];
    uint8_t len;
    uint16_t beacons;
    uint8_t bssid[6];
    uint64_t first_us;
    uint64_t last_us;
    bool in_use;
} pf_ssid_t;

typedef struct {
    uint8_t oui[3];
    uint16_t names;   /* distinct SSIDs seen from this prefix */
    bool local;       /* locally-administered (software) prefix */
    bool in_use;
} pf_oui_t;

typedef struct {
    pf_ssid_t ssids[PF_MAX_SSIDS];
    unsigned n_ssids;
    pf_oui_t ouis[PF_MAX_OUI];
    unsigned n_ouis;
    uint32_t total_beacons;
    uint32_t distinct_created;   /* SSIDs ever admitted, incl. evicted */
    uint32_t evictions;
    uint64_t first_us, last_us;
    bool overflow;
} pf_engine_t;

typedef struct {
    uint8_t score;
    uint8_t raw_score;
    uint8_t ceiling;
    uint8_t families;
    uint8_t notes;
    pf_band_t band;

    uint8_t c_volume, c_ephemeral, c_synthetic;

    uint16_t distinct_ssids;
    uint16_t new_per_min_x10;    /* duty-corrected novelty rate     */
    uint16_t ephemeral_permil;   /* share of names seen <= 2 times  */
    uint16_t synthetic_permil;   /* share of names on software MACs */
    uint8_t widest_oui_names;    /* most SSIDs from one prefix      */
    const char *headline;
} pf_verdict_t;

void pf_reset(pf_engine_t *e);

/* One beacon: a network name announced by a BSSID. */
void pf_observe(pf_engine_t *e, const uint8_t bssid[6], const char *ssid,
                uint8_t len, uint64_t t_us);

void pf_evaluate(const pf_engine_t *e, const pf_context_t *ctx, pf_verdict_t *out);

uint8_t pf_ceiling(const pf_context_t *ctx);
const char *pf_band_name(pf_band_t band);
const char *pf_band_advice(pf_band_t band);

#ifdef __cplusplus
}
#endif

#endif /* PHAROS_FLOOD_H */
