#include "pharos_dot11.h"

#include <string.h>

#define DOT11_HDR_MIN 24
#define BEACON_FIXED  12 /* timestamp 8 + beacon interval 2 + capability 2 */

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

/* 802.11 fields are little-endian, but everything riding ON them - the SNAP
 * ethertype, and every field of EAPOL - is network byte order. Mixing the two
 * up silently reads the wrong bits and still "parses", so they are two
 * clearly-named readers rather than one. */
static uint16_t rd16be(const uint8_t *p)
{
    return (uint16_t)(((uint16_t)p[0] << 8) | (uint16_t)p[1]);
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

    /* Bytes 2-3, straight off the header. See the note in pharos_event.h:
     * a real AP varies this, a template does not. */
    out->duration = rd16(buf + 2);

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
    return pharos_dot11_find_ie_from(body, len, BEACON_FIXED, id, out_len);
}

const uint8_t *pharos_dot11_find_ie_from(const uint8_t *body, size_t len,
                                         size_t start, uint8_t id,
                                         uint8_t *out_len)
{
    if (!body || len < start) {
        return NULL;
    }
    size_t off = start;
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

/* ---- WPA 4-way handshake ------------------------------------------------
 *
 * See the header for why messages 1 and 2 are visible to a passive listener
 * and why that matters. The walk below is deliberately paranoid about length:
 * this runs against frames chosen by whoever is transmitting.
 */
bool pharos_dot11_eapol(const uint8_t *buf, size_t len, pharos_eapol_t *out)
{
    if (!buf || !out) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    if (len < 24u) {
        return false;
    }

    const uint8_t fc0 = buf[0];
    const uint8_t fc1 = buf[1];
    const uint8_t type = (uint8_t)((fc0 >> 2) & 0x3u);
    const uint8_t subtype = (uint8_t)((fc0 >> 4) & 0xFu);
    if (type != PHAROS_FT_DATA) {
        return false;
    }
    /* A protected frame is CCMP/TKIP ciphertext from here on. Parsing it would
     * be reading noise and calling it evidence. */
    if (fc1 & 0x40u) {
        return false;
    }

    /* Header length: 24, plus 6 for the 4-address (WDS) form, plus 2 for the
     * QoS control field that the QoS data subtypes carry. */
    size_t off = 24u;
    const bool to_ds = (fc1 & 0x01u) != 0;
    const bool from_ds = (fc1 & 0x02u) != 0;
    if (to_ds && from_ds) {
        off += 6u;
    }
    if (subtype & 0x08u) {
        off += 2u;
    }

    /* LLC/SNAP: AA AA 03 00 00 00, then the ethertype. 0x888E is EAPOL. */
    if (off + 8u > len) {
        return false;
    }
    if (!(buf[off] == 0xAAu && buf[off + 1] == 0xAAu && buf[off + 2] == 0x03u &&
          buf[off + 3] == 0x00u && buf[off + 4] == 0x00u && buf[off + 5] == 0x00u)) {
        return false;
    }
    if (rd16be(buf + off + 6) != 0x888Eu) {
        return false;
    }
    off += 8u;

    /* EAPOL header: version, packet type, body length. Type 3 = EAPOL-Key. */
    if (off + 4u > len) {
        return false;
    }
    if (buf[off + 1] != 0x03u) {
        return false;
    }
    off += 4u;

    /* EAPOL-Key: descriptor type, then the 2-byte key information field. */
    if (off + 3u > len) {
        return false;
    }
    const uint8_t desc = buf[off];
    if (desc != 2u && desc != 254u) { /* RSN, or the legacy WPA descriptor */
        return false;
    }
    const uint16_t info = rd16be(buf + off + 1);

    const bool pairwise = (info & 0x0008u) != 0; /* key type: 1 = pairwise */
    const bool install  = (info & 0x0040u) != 0;
    const bool ack      = (info & 0x0080u) != 0;
    const bool mic      = (info & 0x0100u) != 0;
    const bool secure   = (info & 0x0200u) != 0;
    out->is_pairwise = pairwise;
    if (!pairwise) {
        /* A group-key rekey. Routine housekeeping, not a handshake capture. */
        return true;
    }

    /* The four messages are told apart by ACK/MIC/Secure/Install, which is the
     * standard's own way of distinguishing them. */
    if (ack && !mic) {
        out->msg = 1;
    } else if (!ack && mic && !secure) {
        out->msg = 2;
    } else if (ack && mic && install) {
        out->msg = 3;
    } else if (!ack && mic && secure) {
        out->msg = 4;
    }

    /* Key data follows the fixed part: descriptor(1) + info(2) + key_len(2) +
     * replay(8) + nonce(32) + iv(16) + rsc(8) + reserved(8) + mic(16), then a
     * 2-byte key-data length. A PMKID rides in message 1 as a KDE:
     *   DD <len> 00 0F AC 04 <16 bytes>
     * Its presence is the clientless attack's signature. */
    if (out->msg == 1) {
        const size_t kd_len_off = off + 1u + 2u + 2u + 8u + 32u + 16u + 8u + 8u + 16u;
        if (kd_len_off + 2u <= len) {
            const uint16_t kd_len = rd16be(buf + kd_len_off);
            const size_t kd = kd_len_off + 2u;
            if (kd_len >= 22u && kd + kd_len <= len) {
                for (size_t i = 0; i + 6u <= (size_t)kd_len; i++) {
                    const uint8_t *p = buf + kd + i;
                    if (p[0] == 0xDDu && p[2] == 0x00u && p[3] == 0x0Fu &&
                        p[4] == 0xACu && p[5] == 0x04u) {
                        out->has_pmkid = true;
                        break;
                    }
                }
            }
        }
    }
    return true;
}
