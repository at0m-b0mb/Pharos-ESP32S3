/* Pharos - WPS information element parser. See pharos_wps.h for why a
 * receive-only tool cares so much about this one element. */
#include "pharos_wps.h"

#include <string.h>

/* Copy an attribute value as a printable, bounded string.
 *
 * These bytes came off the air from a stranger. Anything non-printable is
 * dropped rather than rendered: a model name is drawn straight onto the glass
 * and into reports, and control characters in it would at best corrupt the
 * display and at worst be the interesting part of somebody's day. */
static void copy_printable(char *dst, unsigned cap, const uint8_t *src,
                           uint16_t len)
{
    unsigned w = 0;
    for (uint16_t i = 0; i < len && w + 1u < cap; i++) {
        const unsigned char c = src[i];
        if (c >= 0x20 && c < 0x7F) {
            dst[w++] = (char)c;
        }
    }
    while (w && dst[w - 1] == ' ') {
        w--; /* trailing padding is common and looks like a bug on the glass */
    }
    dst[w] = '\0';
}

/* Manufacturers ship placeholders in the serial-number attribute far more
 * often than real serials - "0000...", "12345678", "none". Reporting one of
 * those as a leaked serial number would be crying wolf about a field that
 * genuinely does leak on some models. */
static bool serial_is_real(const char *s)
{
    if (!s || !s[0]) {
        return false;
    }
    unsigned distinct = 0;
    bool seen[128] = { false };
    for (const char *p = s; *p; p++) {
        const unsigned char c = (unsigned char)*p;
        if (c < 128 && !seen[c]) {
            seen[c] = true;
            distinct++;
        }
    }
    if (distinct <= 1u) {
        return false; /* "00000000", "--------" */
    }
    static const char *k_placeholder[] = { "12345678", "none", "n/a", "0",
                                           "00000000", "unknown" };
    for (unsigned i = 0; i < sizeof(k_placeholder) / sizeof(k_placeholder[0]); i++) {
        const char *a = s, *b = k_placeholder[i];
        while (*a && *b) {
            char ca = *a, cb = *b;
            if (ca >= 'A' && ca <= 'Z') ca = (char)(ca - 'A' + 'a');
            if (ca != cb) break;
            a++; b++;
        }
        if (!*a && !*b) {
            return false;
        }
    }
    return true;
}

bool pwps_parse_attrs(const uint8_t *attrs, uint16_t len, pwps_info_t *out)
{
    if (!out) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    if (!attrs || len < 4u) {
        return false;
    }
    out->present = true;

    uint16_t i = 0;
    while ((uint16_t)(i + 4u) <= len) {
        const uint16_t id = (uint16_t)(((uint16_t)attrs[i] << 8) | attrs[i + 1]);
        const uint16_t alen =
            (uint16_t)(((uint16_t)attrs[i + 2] << 8) | attrs[i + 3]);
        const uint16_t body = (uint16_t)(i + 4u);

        /* THE BOUNDS CHECK THAT MATTERS. A declared length longer than what is
         * left is either a truncated capture or somebody being interesting;
         * either way the only safe response is to stop, not to clamp and carry
         * on reading whatever follows in memory. */
        if ((uint32_t)body + alen > (uint32_t)len) {
            break;
        }
        const uint8_t *v = &attrs[body];

        switch (id) {
        case PWPS_ATTR_MANUFACTURER:
            copy_printable(out->manufacturer, sizeof(out->manufacturer), v, alen);
            break;
        case PWPS_ATTR_MODEL_NAME:
            copy_printable(out->model, sizeof(out->model), v, alen);
            break;
        case PWPS_ATTR_MODEL_NUMBER:
            copy_printable(out->model_number, sizeof(out->model_number), v, alen);
            break;
        case PWPS_ATTR_DEV_NAME:
            copy_printable(out->device_name, sizeof(out->device_name), v, alen);
            break;
        case PWPS_ATTR_SERIAL_NUMBER: {
            char tmp[PWPS_STR_MAX + 1];
            copy_printable(tmp, sizeof(tmp), v, alen);
            out->serial_leaked = serial_is_real(tmp);
            break;
        }
        case PWPS_ATTR_CONFIG_METHODS:
            if (alen >= 2u) {
                out->config_methods =
                    (uint16_t)(((uint16_t)v[0] << 8) | v[1]);
            }
            break;
        case PWPS_ATTR_VERSION:
            if (alen >= 1u) {
                out->version = v[0];
            }
            break;
        case PWPS_ATTR_WPS_STATE:
            if (alen >= 1u) {
                out->state = v[0];
            }
            break;
        case PWPS_ATTR_AP_LOCKED:
            if (alen >= 1u) {
                out->locked = (v[0] != 0);
                out->has_locked = true;
            }
            break;
        default:
            break;
        }

        /* A zero-length attribute is legal; a zero-length STEP is not, and
         * would spin here forever on malformed input. */
        i = (uint16_t)(body + alen);
    }
    return true;
}

bool pwps_parse(const uint8_t *ies, uint16_t len, pwps_info_t *out)
{
    if (out) {
        memset(out, 0, sizeof(*out));
    }
    if (!ies || !out) {
        return false;
    }
    /* The WPS element is a vendor-specific IE (id 221) whose first four bytes
     * are Microsoft's OUI 00:50:F2 and type 0x04. WPA1 uses the same OUI with
     * type 0x01, so the type byte is what distinguishes them - matching on the
     * OUI alone would parse a WPA1 element as WPS and produce a model name out
     * of cipher suite identifiers. */
    static const uint8_t k_wps_oui[4] = { 0x00, 0x50, 0xF2, 0x04 };

    uint16_t i = 0;
    while ((uint16_t)(i + 2u) <= len) {
        const uint8_t id = ies[i];
        const uint8_t elen = ies[i + 1];
        const uint16_t body = (uint16_t)(i + 2u);
        if ((uint32_t)body + elen > (uint32_t)len) {
            break; /* truncated; stop rather than read past the end */
        }
        if (id == 221u && elen >= 4u &&
            memcmp(&ies[body], k_wps_oui, sizeof(k_wps_oui)) == 0) {
            return pwps_parse_attrs(&ies[body + 4u], (uint16_t)(elen - 4u), out);
        }
        i = (uint16_t)(body + elen);
    }
    return false;
}

bool pwps_pin_exposed(const pwps_info_t *w)
{
    if (!w || !w->present) {
        return false;
    }
    /* No PIN method offered, no PIN flaw. A push-button-only access point is
     * not reachable this way, and saying otherwise would be the kind of
     * false positive that teaches somebody to ignore the finding. */
    if ((w->config_methods & PWPS_CFG_PIN_ANY) == 0u) {
        return false;
    }
    /* Already locked out: the registrar has stopped answering, which is the
     * defence working. */
    if (w->has_locked && w->locked) {
        return false;
    }
    return true;
}

const char *pwps_model(const pwps_info_t *w, char *buf, unsigned cap)
{
    if (!buf || !cap) {
        return "";
    }
    buf[0] = '\0';
    if (!w || !w->present) {
        return buf;
    }
    unsigned k = 0;
    /* Manufacturer then model, and the model NUMBER last - it is the part a
     * vulnerability database is indexed by, so it is the part that must
     * survive if the line has to be cut short. Hence model number goes in
     * only when there is room for all of it. */
    const char *parts[3] = { w->manufacturer, w->model, w->model_number };
    for (unsigned p = 0; p < 3; p++) {
        const char *s = parts[p];
        if (!s || !s[0]) {
            continue;
        }
        const unsigned need = (unsigned)strlen(s) + (k ? 1u : 0u);
        if (k + need + 1u > cap) {
            break;
        }
        if (k) {
            buf[k++] = ' ';
        }
        for (const char *q = s; *q && k + 1u < cap; q++) {
            buf[k++] = *q;
        }
    }
    buf[k] = '\0';
    if (!k && w->device_name[0]) {
        /* Some access points fill in only the device name. */
        unsigned n = 0;
        for (const char *q = w->device_name; *q && n + 1u < cap; q++) {
            buf[n++] = *q;
        }
        buf[n] = '\0';
    }
    return buf;
}
