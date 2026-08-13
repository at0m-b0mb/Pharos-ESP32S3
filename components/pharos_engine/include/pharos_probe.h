/* Pharos - probe request privacy analysis
 *
 * Pure C. This is the lens that makes a room go quiet.
 *
 * A phone looking for Wi-Fi does not wait to be spoken to. It broadcasts the
 * names of networks it has joined before, to anybody with a receiver, from
 * inside a pocket. Probe reads those names, sorts them into kinds of place,
 * and grades how much the device has told the room about its owner.
 *
 * Two things make it more than a party trick.
 *
 * MAC randomisation is supposed to stop this, and mostly it stops the naive
 * version. It does not stop the two signals underneath: the set and order of
 * information elements in a probe is a fingerprint of the chipset, driver and
 * OS build, and the 802.11 sequence counter usually keeps counting across a
 * MAC change. A fingerprint match plus a plausible sequence continuation
 * links two "different" devices back together. Pharos does this to show you
 * that it can be done - and grades a device *worse* when it works.
 *
 * And the grade is about specificity, not volume. Ten coffee shops say
 * almost nothing. One hospital, one employer and one home router named after
 * a family says a great deal. The scoring weights kinds of place accordingly.
 *
 * Nothing here is stored beyond the session, and the report writer redacts
 * addresses at write time. The point is to demonstrate the exposure, not to
 * build a copy of it.
 */
#ifndef PHAROS_PROBE_H
#define PHAROS_PROBE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define PP_SSID_MAX     32
#define PP_MAX_DEVICES  32
#define PP_MAX_NETWORKS 12 /* remembered per device */

/* Kinds of place a network name can betray, ordered roughly by how much
 * knowing it narrows down a person. */
typedef enum {
    PP_PLACE_UNKNOWN = 0,
    PP_PLACE_GENERIC,    /* "guest", "wifi", "default"                  */
    PP_PLACE_RETAIL,     /* a coffee chain: almost everybody has one    */
    PP_PLACE_TELECOM,    /* carrier hotspot roaming SSID                */
    PP_PLACE_TRANSIT,    /* airport, train, in-flight                   */
    PP_PLACE_HOSPITALITY,/* hotel: says where you slept                 */
    PP_PLACE_EDUCATION,  /* campus, eduroam                             */
    PP_PLACE_VEHICLE,    /* a car's own hotspot                         */
    PP_PLACE_WORKPLACE,  /* corporate SSID: says who employs you        */
    PP_PLACE_HEALTHCARE, /* says something nobody chose to share        */
    PP_PLACE_HOME,       /* a router named after a household            */
    PP_PLACE_COUNT,
} pp_place_t;

typedef enum {
    PP_GRADE_UNGRADED = 0, /* too few probes heard to say anything */
    PP_GRADE_A_PLUS,       /* silent: passive scanning only        */
    PP_GRADE_A,
    PP_GRADE_B,
    PP_GRADE_C,
    PP_GRADE_D,
    PP_GRADE_F,
} pp_grade_t;

#define PP_NOTE_NO_RANDOM   (1u << 0) /* a stable, globally administered MAC */
#define PP_NOTE_RELINKED    (1u << 1) /* randomisation defeated in this session */
#define PP_NOTE_UNIQUE_NAME (1u << 2) /* a name unlikely to be shared        */
#define PP_NOTE_THIN        (1u << 3) /* few probes: silence may be chance   */
#define PP_NOTE_DIRECTED    (1u << 4) /* named networks, not wildcard probes */

/* One probe request, as handed over by the lens. */
typedef struct {
    uint8_t addr[6];
    char ssid[PP_SSID_MAX + 1];
    uint8_t ssid_len; /* 0 = wildcard probe, which reveals nothing */
    uint16_t seq;
    uint32_t fingerprint; /* hash of the IE set and order */
    int8_t rssi;
    uint64_t t_us;
} pp_probe_t;

typedef struct {
    uint8_t addr[6];      /* most recent address used  */
    uint32_t fingerprint;
    uint16_t last_seq;
    uint64_t last_us;
    uint16_t probes;
    uint8_t wildcards;
    uint8_t identities;   /* distinct MACs linked to this device */
    uint8_t n_networks;
    char networks[PP_MAX_NETWORKS][PP_SSID_MAX + 1];
    uint8_t places[PP_MAX_NETWORKS];
    int8_t best_rssi;
    bool in_use;
} pp_device_t;

typedef struct {
    pp_device_t devices[PP_MAX_DEVICES];
    unsigned n_devices;
    uint32_t probes_seen;
    uint32_t wildcards_seen;
} pp_engine_t;

typedef struct {
    uint8_t exposure;   /* 0..100, higher is worse            */
    pp_grade_t grade;
    uint8_t notes;
    uint8_t networks;   /* distinct named networks announced  */
    uint8_t identities; /* MACs we linked back to one device  */
    pp_place_t narrowest; /* the single most revealing kind    */
    const char *headline;
} pp_verdict_t;

void pp_reset(pp_engine_t *e);

/* Feed one probe request. Returns the device index it was attributed to, or
 * -1 if the table is full. */
int pp_observe(pp_engine_t *e, const pp_probe_t *p);

void pp_grade_device(const pp_device_t *d, pp_verdict_t *out);

/* Classify a network name. Length-delimited: SSIDs are not NUL-terminated on
 * the wire. Case-insensitive, and never allocates. */
pp_place_t pp_classify(const char *ssid, uint8_t len);

const char *pp_place_name(pp_place_t p);
const char *pp_grade_name(pp_grade_t g);
/* What the owner of this device could do about it. */
const char *pp_grade_advice(const pp_verdict_t *v);

#ifdef __cplusplus
}
#endif

#endif /* PHAROS_PROBE_H */
