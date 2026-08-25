/* Pharos - the WPS information element: what a router says about itself
 *
 * Pure C. Every access point with Wi-Fi Protected Setup enabled broadcasts a
 * vendor information element in its beacons, and that element is a
 * manufacturer's plate nailed to the outside of the box - in cleartext, to
 * anybody within earshot, thousands of times an hour.
 *
 * ---------------------------------------------------------------------------
 * WHY THIS IS THE MOST VALUABLE THING A RECEIVE-ONLY TOOL CAN READ
 *
 * Everything else here can say "a TP-Link router". This says "TP-Link Archer
 * C7 v2", because the manufacturer, the model name and the model number are
 * three of the attributes the element carries. That is the difference between
 * a generic checklist and a finding somebody can act on: a model number is
 * what a CVE is indexed by.
 *
 * And it costs nothing. No association, no probe, no transmission of any kind
 * - the access point is already shouting it. A device that could not join a
 * network if it wanted to can still tell you exactly what that network is
 * running.
 *
 * ---------------------------------------------------------------------------
 * THE VULNERABILITY THAT IS IN THE ELEMENT ITSELF
 *
 * WPS's external-registrar PIN is an eight-digit number validated in two
 * halves, which reduces the search from 100,000,000 to about 11,000 - and the
 * last digit is a checksum, so really about 11,000 tries. That is the 2011
 * design flaw the whole Reaver family exploits, and it is still switched on in
 * a great many domestic routers.
 *
 * Three attributes decide whether a given access point is exposed to it, and
 * all three are in the beacon:
 *
 *   - CONFIG METHODS says whether a PIN method (label, display or keypad) is
 *     offered at all. Push-button only is not vulnerable to this.
 *   - VERSION 1.0 has no lockout requirement. Version 2 mandates one.
 *   - AP SETUP LOCKED says whether the registrar has already locked itself,
 *     which is what a router does after too many bad PINs.
 *
 * So this parser reports an exposure that is a real, indexed, decades-old
 * vulnerability, identified passively, with the model number needed to look up
 * everything else that model is known for.
 *
 * WHAT IT WILL NOT DO. It never claims a device IS vulnerable - only that it
 * advertises the configuration under which the flaw applies. Whether a given
 * firmware revision actually falls to it is a question about that firmware,
 * and the honest thing to hand somebody is the model number and the reason to
 * go and look.
 */
#ifndef PHAROS_WPS_H
#define PHAROS_WPS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Bounded, and short enough to fit the glass. Real values are well inside
 * this - "Archer C7" is nine characters - and the spec's own limits are
 * larger than anything worth showing on a 466 px circle. */
#define PWPS_STR_MAX 20

/* The attribute identifiers, from the WSC specification. Named rather than
 * inlined because a wrong constant here does not fail loudly - it produces a
 * confident model string made of somebody else's bytes. */
#define PWPS_ATTR_CONFIG_METHODS 0x1008
#define PWPS_ATTR_DEV_NAME       0x1011
#define PWPS_ATTR_MANUFACTURER   0x1021
#define PWPS_ATTR_MODEL_NAME     0x1023
#define PWPS_ATTR_MODEL_NUMBER   0x1024
#define PWPS_ATTR_SERIAL_NUMBER  0x1042
#define PWPS_ATTR_WPS_STATE      0x1044
#define PWPS_ATTR_UUID_E         0x1047
#define PWPS_ATTR_VERSION        0x104A
#define PWPS_ATTR_PRIMARY_DEV    0x1054
#define PWPS_ATTR_AP_LOCKED      0x1057

/* Config-method bits that constitute a PIN. Push-button is deliberately not
 * among them: it is the one method the PIN flaw does not reach. */
#define PWPS_CFG_LABEL      0x0004
#define PWPS_CFG_DISPLAY    0x0008
#define PWPS_CFG_PUSHBUTTON 0x0080
#define PWPS_CFG_KEYPAD     0x0100
#define PWPS_CFG_PIN_ANY (PWPS_CFG_LABEL | PWPS_CFG_DISPLAY | PWPS_CFG_KEYPAD)

typedef struct {
    bool present;   /* a WPS element was found at all          */

    char manufacturer[PWPS_STR_MAX + 1];
    char model[PWPS_STR_MAX + 1];
    char model_number[PWPS_STR_MAX + 1];
    char device_name[PWPS_STR_MAX + 1];

    uint16_t config_methods;
    uint8_t version;    /* 0x10 = 1.0, 0x20 = 2.0; 0 = not stated */
    uint8_t state;      /* 1 = not configured, 2 = configured     */
    bool locked;        /* AP setup locked                        */
    bool has_locked;    /* ...was actually stated                 */
    bool serial_leaked; /* a real serial number, not a placeholder */
} pwps_info_t;

/* Parse the WPS vendor element out of a beacon's information elements.
 *
 * `ies` points at the IE region of a beacon or probe response - that is, after
 * the fixed 12-byte body - and `len` is its length. Returns false when there
 * is no WPS element, in which case `out` is zeroed.
 *
 * Hostile input is the normal case: this parses bytes from the air, from
 * anybody, and a malformed element must not walk off the end of the buffer.
 * Every length is checked against what remains. */
bool pwps_parse(const uint8_t *ies, uint16_t len, pwps_info_t *out);

/* Parse the WPS element's own attribute region, when the caller has already
 * found it. Exposed for the tests. */
bool pwps_parse_attrs(const uint8_t *attrs, uint16_t len, pwps_info_t *out);

/* Is this access point advertising the configuration the PIN flaw needs?
 *
 * True when a PIN method is offered, the registrar is not locked, and nothing
 * says it is WPS 2.0 with a lockout. NOT a claim that it is exploitable - see
 * the header note. */
bool pwps_pin_exposed(const pwps_info_t *w);

/* "TP-LINK Archer C7", into `buf`. Falls back to whichever parts exist, and
 * writes an empty string when none do. */
const char *pwps_model(const pwps_info_t *w, char *buf, unsigned cap);

#ifdef __cplusplus
}
#endif

#endif /* PHAROS_WPS_H */
