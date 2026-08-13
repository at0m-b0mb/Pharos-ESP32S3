#include "pharos_dot11.h"

#include <string.h>

#define DOT11_HDR_MIN 24
#define BEACON_FIXED  12 /* timestamp 8 + beacon interval 2 + capability 2 */

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

bool pharos_dot11_parse_header(const uint8_t *buf, size_t len, pharos_ev_dot11_t *out)
{
    if (!buf || !out || len < DOT11_HDR_MIN) {
        return false;
    }
    memset(out, 0, sizeof(*out));

    const uint16_t fc = rd16(buf);
    out->type = (uint8_t)((fc >> 2) & 0x3);
    out->subtype = (uint8_t)((fc >> 4) & 0xF);

    const bool to_ds = (fc & (1u << 8)) != 0;
    const bool from_ds = (fc & (1u << 9)) != 0;
    if (to_ds) out->flags |= PHAROS_DOT11_F_TO_DS;
    if (from_ds) out->flags |= PHAROS_DOT11_F_FROM_DS;
    if (fc & (1u << 11)) out->flags |= PHAROS_DOT11_F_RETRY;
    if (fc & (1u << 14)) out->flags |= PHAROS_DOT11_F_PROTECTED;

    memcpy(out->a1, buf + 4, 6);
    memcpy(out->a2, buf + 10, 6);
    memcpy(out->a3, buf + 16, 6);
    out->seq = (uint16_t)(rd16(buf + 22) >> 4);

    bool bcast = true;
    for (int i = 0; i < 6; i++) {
        if (out->a1[i] != 0xFF) { bcast = false; break; }
    }
    if (bcast) {
        out->flags |= PHAROS_DOT11_F_BROADCAST;
    }
    return true;
}

bool pharos_dot11_reason(const uint8_t *buf, size_t len, const pharos_ev_dot11_t *hdr,
                         uint16_t *out_reason)
{
    if (!buf || !hdr || !out_reason || hdr->type != PHAROS_FT_MGMT) {
        return false;
    }
    const bool carries =
        hdr->subtype == PHAROS_ST_DEAUTH || hdr->subtype == PHAROS_ST_DISASSOC;
    if (!carries || len < DOT11_HDR_MIN + 2u) {
        return false;
    }
    *out_reason = rd16(buf + DOT11_HDR_MIN);
    return true;
}

const uint8_t *pharos_dot11_find_ie(const uint8_t *body, size_t len, uint8_t id,
                                    uint8_t *out_len)
{
    if (!body || len < BEACON_FIXED) {
        return NULL;
    }
    size_t off = BEACON_FIXED;
    /* Walk the element chain. Every step is bounds-checked against the
     * buffer the radio actually gave us: a truncated or hostile beacon must
     * stop the walk, never read past the end. */
    while (off + 2u <= len) {
        const uint8_t eid = body[off];
        const uint8_t elen = body[off + 1];
        if (off + 2u + elen > len) {
            return NULL; /* element claims more than the frame holds */
        }
        if (eid == id) {
            if (out_len) {
                *out_len = elen;
            }
            return body + off + 2;
        }
        off += 2u + elen;
    }
    return NULL;
}

bool pharos_dot11_rsn(const uint8_t *body, size_t len, pharos_rsn_t *out)
{
    if (!out) {
        return false;
    }
    memset(out, 0, sizeof(*out));

    uint8_t ie_len = 0;
    const uint8_t *rsn = pharos_dot11_find_ie(body, len, PHAROS_IE_RSN, &ie_len);
    if (!rsn) {
        return false;
    }
    out->has_rsn = true;

    /* RSN element layout:
     *   version              2
     *   group cipher suite   4
     *   pairwise count       2   then 4 * count
     *   AKM count            2   then 4 * count
     *   RSN capabilities     2   (optional)
     * Each length is validated before it is used. */
    size_t off = 0;
    if (ie_len < 2u) return true;
    off += 2; /* version */

    if (off + 4u > ie_len) return true;
    off += 4; /* group cipher */

    if (off + 2u > ie_len) return true;
    const uint16_t pw_count = rd16(rsn + off);
    off += 2;
    if (pw_count > 16u || off + (size_t)pw_count * 4u > ie_len) return true;
    off += (size_t)pw_count * 4u;

    if (off + 2u > ie_len) return true;
    const uint16_t akm_count = rd16(rsn + off);
    off += 2;
    if (akm_count > 16u || off + (size_t)akm_count * 4u > ie_len) return true;
    for (uint16_t i = 0; i < akm_count; i++) {
        const uint8_t *s = rsn + off + (size_t)i * 4u;
        /* 00-0F-AC suite selectors: 2 = PSK, 8 = SAE, 18 = OWE. */
        if (s[0] == 0x00 && s[1] == 0x0F && s[2] == 0xAC) {
            if (s[3] == 2) out->has_psk = true;
            if (s[3] == 8) out->has_sae = true;
            if (s[3] == 18) out->has_owe = true;
        }
    }
    off += (size_t)akm_count * 4u;

    if (off + 2u > ie_len) return true; /* capabilities are optional */
    const uint16_t caps = rd16(rsn + off);
    out->mfp_required = (caps & (1u << 6)) != 0;
    out->mfp_capable = (caps & (1u << 7)) != 0;
    if (out->mfp_required) {
        /* The standard forbids required-without-capable, but a beacon is
         * whatever somebody chose to transmit. Normalise rather than trust. */
        out->mfp_capable = true;
    }
    return true;
}
