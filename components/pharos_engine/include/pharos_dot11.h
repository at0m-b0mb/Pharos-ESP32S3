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

#define PHAROS_IE_SSID 0
#define PHAROS_IE_DS_PARAM 3
#define PHAROS_IE_RSN 48

#ifdef __cplusplus
}
#endif

#endif /* PHAROS_DOT11_H */
