/* Pharos - 802.11 frame dissection, pure and host-testable.
 *
 * Split out of the radio component on purpose: the promiscuous callback runs
 * inside the Wi-Fi driver task with a hard time budget, so the only thing it
 * is allowed to do is fill a pharos_ev_dot11_t from the fixed header. Any
 * parsing that walks information elements happens later, on the analytics
 * core, against a copied buffer - and because none of it touches ESP-IDF, all
 * of it runs in the host tests.
 */
#ifndef PHAROS_DOT11_H
#define PHAROS_DOT11_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "pharos_event.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Fixed-header parse. buf points at the start of the MAC header, len is what
 * the radio handed us. Returns false if the frame is too short to trust.
 * Fills type/subtype/addresses/seq/flags; the caller supplies rssi/channel. */
bool pharos_dot11_parse_header(const uint8_t *buf, size_t len, pharos_ev_dot11_t *out);

/* Reason or status code for the subtypes that carry one, appended to the
 * event by the caller. Returns false when the frame carries none. */
bool pharos_dot11_reason(const uint8_t *buf, size_t len, const pharos_ev_dot11_t *hdr,
                         uint16_t *out_reason);

/* Robust-management-frame posture, read from the RSN element of a beacon or
 * probe response. This decides whether a deauthentication flood aimed at the
 * network would even work: with MFP required, forged deauths are discarded
 * by the client, and Pharos reports the attempt rather than an outage. */
typedef struct {
    bool has_rsn;
    bool mfp_capable; /* RSN capabilities bit 6 */
    bool mfp_required; /* RSN capabilities bit 7 */
    bool has_sae;      /* WPA3-Personal AKM present  */
    bool has_psk;      /* WPA2-PSK AKM present       */
    bool has_owe;      /* Opportunistic Wireless Encryption */
} pharos_rsn_t;

/* body points just past the 24-byte MAC header of a beacon/probe response,
 * i.e. at the fixed parameters (timestamp/interval/capability), and len is
 * the remaining length. Tolerant of truncation: a malformed element chain
 * stops the walk instead of reading past the buffer. */
bool pharos_dot11_rsn(const uint8_t *body, size_t len, pharos_rsn_t *out);

/* Locates one information element in a beacon/probe-response body. Returns a
 * pointer to the element payload and sets *out_len, or NULL. */
const uint8_t *pharos_dot11_find_ie(const uint8_t *body, size_t len, uint8_t id,
                                    uint8_t *out_len);

/* Same walk, but starting at an explicit offset into the body. Beacons and
 * probe RESPONSES carry 12 bytes of fixed parameters before the element chain;
 * probe REQUESTS carry none, so passing the wrong start silently skips their
 * first element - which is the SSID, the only one that matters. */
const uint8_t *pharos_dot11_find_ie_from(const uint8_t *body, size_t len,
                                         size_t start, uint8_t id,
                                         uint8_t *out_len);

/* WPA 4-way handshake visibility.
 *
 * The handshake is what an attacker actually wants: capture it and the
 * passphrase can be attacked offline, at leisure, with no further contact
 * with the network. Two things make it observable to a purely passive
 * receiver, and both are worth stating plainly:
 *
 *   - Messages 1 and 2 are exchanged BEFORE the pairwise key is installed,
 *     so they are not protected at the 802.11 layer. A listener reads them.
 *     Messages 3 and 4 usually are protected, and Pharos does not pretend to
 *     see inside them.
 *   - Message 1 may carry a PMKID. That is the clientless attack: an attacker
 *     associates to the AP itself and the AP hands over a PMKID with no
 *     client involved at all - no deauthentication, no waiting, no victim.
 *
 * Pharos detects that this is happening. It does not store nonces, MICs or
 * key data, and it could not complete a handshake if it wanted to: it has no
 * transmitter. This is the defender learning that their handshakes are being
 * collected. */
typedef struct {
    uint8_t msg;      /* 1..4, or 0 when this is not a pairwise EAPOL-Key */
    bool has_pmkid;   /* message 1 carried a PMKID KDE (clientless route) */
    bool is_pairwise; /* group-key rekeys are ordinary housekeeping       */
} pharos_eapol_t;

/* buf/len are the whole frame, starting at the MAC header. Returns false when
 * the frame is not an unprotected EAPOL-Key, which includes every ordinary
 * data frame - so this is safe (and cheap) to call on anything. */
bool pharos_dot11_eapol(const uint8_t *buf, size_t len, pharos_eapol_t *out);

#define PHAROS_IE_SSID 0
#define PHAROS_IE_DS_PARAM 3
#define PHAROS_IE_RSN 48

#ifdef __cplusplus
}
#endif

#endif /* PHAROS_DOT11_H */
