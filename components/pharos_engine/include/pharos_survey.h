/* Pharos - the Survey: what this place is actually like
 *
 * Pure C, host-tested.
 *
 * ---------------------------------------------------------------------------
 * THE THING EVERY LENS WAS THROWING AWAY
 *
 * Aegis remembers ATTACKS. That is the right thing to latch and it is not the
 * question most people who pick this device up are asking. They are asking a
 * quieter one: what is this place like, and is anything here putting me at
 * risk? Nothing answered it, because every lens reported the present tense and
 * then forgot - and with the watches on a rotation, "the present tense" is a
 * five-second glance every forty seconds.
 *
 * So Census would grade twenty-three networks, find six that would fall to a
 * deauthentication flood, and lose all of it the moment the radio moved on.
 * The device knew something genuinely worth knowing about the room and had
 * nowhere to put it.
 *
 * The Survey is that place. It accumulates over the whole session, deduplicated
 * by address, and answers in the plainest terms available:
 *
 *     23 networks here. 6 would fall to a deauthentication flood.
 *     2 are open. 4 devices announced 11 network names they remember.
 *     A Flipper Zero was present for 3 minutes.
 *
 * ---------------------------------------------------------------------------
 * THE RULES THAT KEEP IT FROM BECOMING A SCARE SHEET
 *
 *   1. IT COUNTS WHAT IT SAW, NOT WHAT IT INFERS. Every number here is a count
 *      of distinct addresses that actually appeared, never an estimate scaled
 *      up for coverage. A rotation hears a fraction of the air; the honest
 *      response is to report the fraction it heard, not to guess at the rest.
 *
 *   2. MOST OF THIS IS NOT AN ATTACK. A neighbour without 802.11w is not
 *      attacking anybody, and the summary says "would fall to" rather than
 *      "is under attack". The one place attack language is allowed is where
 *      Aegis has actually latched a stage.
 *
 *   3. IT IS SOMEBODY ELSE'S NETWORK. The survey names what it saw and what
 *      the owner could change; it never suggests doing anything TO any of it.
 *
 *   4. NOTHING IDENTIFYING LEAVES. Addresses are held only to deduplicate and
 *      are never displayed or written out - see pharos_report.h for the
 *      redaction the reports already apply.
 */
#ifndef PHAROS_SURVEY_H
#define PHAROS_SURVEY_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Deduplication capacity. Beyond this the survey stops counting NEW addresses
 * and says so, rather than silently under-reporting a busy place - a number
 * that quietly stops rising is worse than one that admits its limit. */
#define PSV_MAX_NETWORKS 96
#define PSV_MAX_DEVICES 64
#define PSV_MAX_TOOLS 8

/* What is wrong with one network, as flags, so a single network counted once
 * can be counted again under each thing worth telling its owner. */
#define PSV_NET_OPEN     (1u << 0) /* no encryption at all               */
#define PSV_NET_NO_MFP   (1u << 1) /* would fall to a deauth flood       */
#define PSV_NET_WPS      (1u << 2) /* WPS advertised                     */
#define PSV_NET_WEAK     (1u << 3) /* WEP, WPA1 or TKIP                  */
#define PSV_NET_HIDDEN   (1u << 4) /* no SSID in the beacon              */
#define PSV_NET_MODERN   (1u << 5) /* WPA3 / SAE / OWE - the good news   */

typedef struct {
    uint8_t addr[6];
    uint8_t grade;  /* pc_grade_t, carried as a byte */
    uint32_t flags;
} psv_net_t;

typedef struct {
    uint8_t addr[6];
    uint8_t names;    /* distinct network names this device announced */
    bool randomised;  /* it was using a private address               */
} psv_dev_t;

typedef struct {
    uint8_t kind;       /* prv_kind_t, carried as a byte */
    uint64_t first_us;
    uint64_t last_us;
    bool present;       /* heard within its own staleness window */
} psv_tool_t;

typedef struct {
    psv_net_t net[PSV_MAX_NETWORKS];
    unsigned n_net;
    bool net_full;

    psv_dev_t dev[PSV_MAX_DEVICES];
    unsigned n_dev;
    bool dev_full;

    psv_tool_t tool[PSV_MAX_TOOLS];
    unsigned n_tool;

    uint64_t started_us;
    uint64_t last_us;
} psv_t;

/* What the survey has to say, already counted. */
typedef struct {
    unsigned networks;
    unsigned open;
    unsigned no_mfp;
    unsigned wps;
    unsigned weak;
    unsigned hidden;
    unsigned modern;
    uint8_t worst_grade;

    unsigned devices;      /* devices that announced network names   */
    unsigned names_leaked; /* total distinct names across them       */
    unsigned trackable;    /* leaking despite address randomisation  */

    unsigned tools;        /* pentest hardware kinds seen this session */
    unsigned tools_present;/* ...still here                            */

    uint32_t minutes;      /* how long the survey has been running     */
    bool truncated;        /* a table filled up; counts are a floor    */

    const char *headline;  /* the one line, in plain words             */
} psv_report_t;

void psv_reset(psv_t *s, uint64_t now_us);

/* One network, graded. Idempotent per address: calling it repeatedly for the
 * same network updates that entry rather than counting it again. */
void psv_note_network(psv_t *s, const uint8_t bssid[6], uint8_t grade,
                      uint32_t flags, uint64_t now_us);

/* One device heard announcing the names of networks it remembers. */
void psv_note_device(psv_t *s, const uint8_t mac[6], uint8_t names,
                     bool randomised, uint64_t now_us);

/* One piece of hardware that announced itself. `present` is the lens' own
 * staleness judgement - the survey does not second-guess it, but it does
 * remember that the thing was here even after it leaves. */
void psv_note_tool(psv_t *s, uint8_t kind, bool present, uint64_t now_us);

void psv_summarise(const psv_t *s, uint64_t now_us, psv_report_t *out);

/* The plain-English lines, in priority order: the most useful thing first.
 * Fill line `index` and return true, or false when there are no more. This is
 * the whole point of the survey - it is what somebody reads and understands
 * without knowing what MFP stands for. */
bool psv_line(const psv_report_t *r, unsigned index, char *buf, unsigned cap);

#ifdef __cplusplus
}
#endif

#endif /* PHAROS_SURVEY_H */
