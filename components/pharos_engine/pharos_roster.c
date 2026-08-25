/* Pharos - Roster. See pharos_roster.h for why this is passive and what
 * "vulnerable" is allowed to mean. */
#include "pharos_roster.h"

#include <string.h>
#include <stdio.h>

#include "pharos_event.h" /* PHAROS_RSN_F_* */

/* ---- the OUI table -------------------------------------------------------
 *
 * The first three octets of a MAC name the manufacturer. This is a curated set
 * of the ones a home or office actually sees - real, IEEE-registered prefixes -
 * with the device class each vendor overwhelmingly makes. It is not the full
 * 30,000-entry registry; it is the couple of dozen that cover most rooms, kept
 * small enough to sit in flash and be read at a glance.
 *
 * A vendor that makes several things (Amazon makes both an Echo and a Ring
 * doorbell) is given its commonest class here, and behaviour refines it later:
 * if the thing beacons, it is infrastructure whatever its OUI suggests. */
typedef struct {
    uint8_t oui[3];
    const char *vendor;
    rd_class_t klass;
} oui_entry_t;

static const oui_entry_t k_ouis[] = {
    /* Apple - phones, tablets, laptops, watches. Dozens of blocks; the
     * commonest few. */
    { { 0x3C, 0x06, 0x30 }, "Apple", RD_PHONE },
    { { 0xA4, 0x83, 0xE7 }, "Apple", RD_PHONE },
    { { 0xF0, 0x18, 0x98 }, "Apple", RD_PHONE },
    { { 0xAC, 0xBC, 0x32 }, "Apple", RD_PHONE },
    { { 0x88, 0x66, 0x5A }, "Apple", RD_PHONE },
    { { 0xDC, 0x2B, 0x2A }, "Apple", RD_PHONE },
    /* Samsung */
    { { 0x00, 0x1A, 0x8A }, "Samsung", RD_PHONE },
    { { 0x5C, 0x0A, 0x5B }, "Samsung", RD_PHONE },
    { { 0xA0, 0x21, 0x95 }, "Samsung", RD_PHONE },
    /* Google - Pixel, Nest, Chromecast */
    { { 0x3C, 0x5A, 0xB4 }, "Google", RD_PHONE },
    { { 0xF4, 0xF5, 0xD8 }, "Google", RD_TV_MEDIA },
    { { 0xDA, 0xA1, 0x19 }, "Google", RD_TV_MEDIA },
    /* Amazon - Echo, Fire, Ring */
    { { 0x44, 0x65, 0x0D }, "Amazon", RD_TV_MEDIA },
    { { 0xFC, 0x65, 0xDE }, "Amazon", RD_TV_MEDIA },
    { { 0x68, 0x37, 0xE9 }, "Amazon", RD_CAMERA }, /* Ring */
    /* Espressif - the ESP32/ESP8266 in a thousand IoT gadgets */
    { { 0x24, 0x0A, 0xC4 }, "Espressif", RD_IOT },
    { { 0x30, 0xAE, 0xA4 }, "Espressif", RD_IOT },
    { { 0x7C, 0x9E, 0xBD }, "Espressif", RD_IOT },
    { { 0xA0, 0x76, 0x4E }, "Espressif", RD_IOT },
    { { 0xB8, 0xD6, 0x1A }, "Espressif", RD_IOT },
    /* Raspberry Pi */
    { { 0xB8, 0x27, 0xEB }, "Raspberry Pi", RD_COMPUTER },
    { { 0xDC, 0xA6, 0x32 }, "Raspberry Pi", RD_COMPUTER },
    { { 0xE4, 0x5F, 0x01 }, "Raspberry Pi", RD_COMPUTER },
    /* Intel / laptop NICs */
    { { 0x00, 0x1B, 0x77 }, "Intel", RD_COMPUTER },
    { { 0x34, 0x13, 0xE8 }, "Intel", RD_COMPUTER },
    { { 0x7C, 0xB2, 0x7D }, "Intel", RD_COMPUTER },
    /* Router / AP vendors - refined to AP by behaviour when they beacon */
    { { 0x14, 0xCC, 0x20 }, "TP-Link", RD_ROUTER },
    { { 0x50, 0xC7, 0xBF }, "TP-Link", RD_ROUTER },
    { { 0xB0, 0x4E, 0x26 }, "TP-Link", RD_ROUTER },
    { { 0x18, 0xE8, 0x29 }, "Ubiquiti", RD_ROUTER },
    { { 0x74, 0xAC, 0xB9 }, "Ubiquiti", RD_ROUTER },
    { { 0xFC, 0xEC, 0xDA }, "Ubiquiti", RD_ROUTER },
    { { 0x00, 0x05, 0x5D }, "Netgear", RD_ROUTER },
    { { 0x9C, 0x3D, 0xCF }, "Netgear", RD_ROUTER },
    { { 0x2C, 0x30, 0x33 }, "Netgear", RD_ROUTER },
    { { 0xC0, 0x56, 0x27 }, "Belkin", RD_ROUTER },
    { { 0x00, 0x18, 0x0A }, "Cisco Meraki", RD_ROUTER },
    /* Cameras / doorbells */
    { { 0x00, 0x62, 0x6E }, "Wyze", RD_CAMERA },
    { { 0x2C, 0xAA, 0x8E }, "Wyze", RD_CAMERA },
    { { 0x00, 0x0F, 0x7C }, "Axis", RD_CAMERA },
    { { 0xE0, 0x50, 0x8B }, "Hikvision", RD_CAMERA },
    { { 0x44, 0x19, 0xB6 }, "Hikvision", RD_CAMERA },
    { { 0xBC, 0xAD, 0x28 }, "Dahua", RD_CAMERA },
    /* TV / media / speakers */
    { { 0xB8, 0x27, 0xEB }, "Raspberry Pi", RD_COMPUTER }, /* dup ok, first wins */
    { { 0x00, 0x0E, 0x58 }, "Sonos", RD_TV_MEDIA },
    { { 0x94, 0x9F, 0x3E }, "Sonos", RD_TV_MEDIA },
    { { 0x00, 0x24, 0xE4 }, "Withings", RD_WEARABLE },
    { { 0xCC, 0xF7, 0x35 }, "Roku", RD_TV_MEDIA },
    { { 0xB0, 0xA7, 0x37 }, "Roku", RD_TV_MEDIA },
    { { 0x8C, 0x49, 0x62 }, "Roku", RD_TV_MEDIA },
    { { 0x00, 0x1E, 0x8F }, "Canon", RD_PRINTER },
    { { 0x00, 0x00, 0x48 }, "Epson", RD_PRINTER },
    { { 0x00, 0x1B, 0xA9 }, "Brother", RD_PRINTER },
    { { 0x30, 0x05, 0x5C }, "HP", RD_PRINTER },
    /* Fitness / wearable BLE */
    { { 0xC8, 0x3F, 0x26 }, "Fitbit", RD_WEARABLE },
    { { 0xD8, 0x8C, 0x79 }, "Garmin", RD_WEARABLE },
};

#define OUI_N ((unsigned)(sizeof(k_ouis) / sizeof(k_ouis[0])))

const char *rd_vendor(const uint8_t mac[6])
{
    if (!mac) {
        return NULL;
    }
    for (unsigned i = 0; i < OUI_N; i++) {
        if (memcmp(k_ouis[i].oui, mac, 3) == 0) {
            return k_ouis[i].vendor;
        }
    }
    return NULL;
}

rd_class_t rd_class_of_oui(const uint8_t mac[6])
{
    if (!mac) {
        return RD_UNKNOWN;
    }
    for (unsigned i = 0; i < OUI_N; i++) {
        if (memcmp(k_ouis[i].oui, mac, 3) == 0) {
            return k_ouis[i].klass;
        }
    }
    return RD_UNKNOWN;
}

/* A locally-administered MAC (bit 1 of the first octet) is not a burned-in
 * hardware address - it is randomised, which modern phones do to resist
 * exactly this kind of tracking. Recognising it lets Roster say "this device
 * is protecting itself" rather than mistaking it for an unknown vendor. */
static bool mac_is_random(const uint8_t mac[6])
{
    return (mac[0] & 0x02) != 0;
}

void rd_reset(rd_roster_t *r)
{
    if (r) {
        memset(r, 0, sizeof(*r));
    }
}

static rd_device_t *find(rd_roster_t *r, const uint8_t mac[6])
{
    for (unsigned i = 0; i < r->n; i++) {
        if (r->dev[i].in_use && memcmp(r->dev[i].mac, mac, 6) == 0) {
            return &r->dev[i];
        }
    }
    return NULL;
}

static rd_device_t *admit(rd_roster_t *r, const uint8_t mac[6], uint64_t t_us)
{
    rd_device_t *d = find(r, mac);
    if (d) {
        return d;
    }
    /* Reuse a free slot, or the stalest occupied one - a full table should
     * drop what has not been heard in longest, not refuse the newcomer. */
    unsigned pick = r->n;
    if (r->n < PR_ROSTER_MAX) {
        r->n++;
    } else {
        uint64_t oldest = t_us;
        pick = 0;
        for (unsigned i = 0; i < PR_ROSTER_MAX; i++) {
            if (r->dev[i].last_us <= oldest) {
                oldest = r->dev[i].last_us;
                pick = i;
            }
        }
    }
    d = &r->dev[pick];
    memset(d, 0, sizeof(*d));
    memcpy(d->mac, mac, 6);
    d->in_use = true;
    d->first_us = t_us;
    d->rssi = -128;
    d->randomised_mac = mac_is_random(mac);
    d->vendor = rd_vendor(mac);
    d->klass = rd_class_of_oui(mac);
    r->admitted++;
    return d;
}

static void set_name(rd_device_t *d, const char *name)
{
    if (!name || !name[0] || d->name[0]) {
        return; /* keep the first real name we heard */
    }
    unsigned k = 0;
    for (; name[k] && k < PR_ROSTER_NAME; k++) {
        d->name[k] = name[k];
    }
    d->name[k] = '\0';
}

void rd_observe_wifi(rd_roster_t *r, const uint8_t mac[6], bool is_ap,
                     const char *ssid, const rd_secpost_t *sec, uint8_t channel,
                     int8_t rssi, const char *probed_ssid, uint64_t t_us)
{
    if (!r || !mac) {
        return;
    }
    rd_device_t *d = admit(r, mac, t_us);
    d->seen |= RD_SEEN_WIFI;
    d->last_us = t_us;
    d->sightings++;
    if (channel) {
        d->channel = channel;
    }
    if (rssi > d->rssi) {
        d->rssi = rssi;
    }

    if (is_ap) {
        /* Beaconing means infrastructure, whatever the OUI's usual product. */
        if (d->klass != RD_ROUTER) {
            d->klass = RD_ACCESS_POINT;
        }
        set_name(d, ssid);

        if (sec) {
            const bool has_rsn = (sec->rsn_flags & PHAROS_RSN_F_PRESENT) != 0;
            /* OPEN: privacy bit clear and no RSN/WPA1 element at all. */
            if (!sec->privacy && !has_rsn && !sec->wpa1) {
                d->exposure |= RD_EXP_OPEN;
            } else if (sec->privacy && !has_rsn && !sec->wpa1) {
                /* Privacy on but no modern element: WEP. */
                d->exposure |= RD_EXP_WEP;
            }
            if (sec->wpa1 || sec->tkip) {
                d->exposure |= RD_EXP_WPA1;
            }
            if (sec->rsn_flags & PHAROS_RSN_F_WPS) {
                d->exposure |= RD_EXP_WPS;
            }
            /* No management-frame protection on a modern network is what lets
             * a deauthentication flood work at all. Only flag it when there IS
             * a modern network to protect - an open AP has bigger problems. */
            if (has_rsn && !(sec->rsn_flags & PHAROS_RSN_F_MFP_CAPABLE)) {
                d->exposure |= RD_EXP_NO_MFP;
            }
        }
    } else {
        /* A client. A fixed (non-randomised) MAC is trackable across every
         * network it joins - the thing MAC randomisation exists to stop. */
        if (!d->randomised_mac) {
            d->exposure |= RD_EXP_FIXED_MAC;
        }
        if (probed_ssid && probed_ssid[0]) {
            /* A device probing by name will silently rejoin that network
             * wherever it appears - an attacker who hears the name can stand
             * one up. If the probe is unusual enough to be a person's name or
             * a home SSID, it is also a location leak. */
            set_name(d, probed_ssid);
            d->exposure |= RD_EXP_NAME_LEAK;
        }
    }
}

/* A handful of BLE GAP appearance values worth naming; the field is a 10-bit
 * category in the top of the 16. */
static rd_class_t class_from_appearance(uint16_t appearance)
{
    const uint16_t cat = appearance >> 6;
    switch (cat) {
    case 0x001: return RD_PHONE;    /* Generic Phone           */
    case 0x002: return RD_COMPUTER; /* Generic Computer        */
    case 0x00A: return RD_WEARABLE; /* Watch                   */
    case 0x00D: return RD_WEARABLE; /* Heart Rate / fitness     */
    case 0x040: return RD_WEARABLE; /* Generic wearable         */
    case 0x041: return RD_WEARABLE; /* earbuds / audio          */
    default:    return RD_UNKNOWN;
    }
}

void rd_observe_ble(rd_roster_t *r, const uint8_t addr[6], bool addr_random,
                    const char *name, uint16_t appearance,
                    const uint8_t *svc_uuids16, unsigned n_uuids, int8_t rssi,
                    uint64_t t_us)
{
    if (!r || !addr) {
        return;
    }
    (void)svc_uuids16;
    (void)n_uuids;
    rd_device_t *d = admit(r, addr, t_us);
    d->seen |= RD_SEEN_BLE;
    d->last_us = t_us;
    d->sightings++;
    d->randomised_mac = addr_random;
    if (rssi > d->rssi) {
        d->rssi = rssi;
    }

    if (d->klass == RD_UNKNOWN) {
        const rd_class_t a = class_from_appearance(appearance);
        d->klass = (a != RD_UNKNOWN) ? a : RD_WEARABLE; /* most BLE-only is */
    }

    if (name && name[0]) {
        set_name(d, name);
        /* A BLE device broadcasting a human-legible name ("Kai's Buds") is
         * announcing its owner to the room continuously. */
        d->exposure |= RD_EXP_NAME_LEAK;
    }
    if (!addr_random) {
        d->exposure |= RD_EXP_FIXED_MAC;
    }
}

void rd_observe_wps(rd_roster_t *r, const uint8_t bssid[6], const char *vendor,
                    const char *model, bool pin_exposed, uint64_t t_us)
{
    if (!r || !bssid) {
        return;
    }
    rd_device_t *d = admit(r, bssid, t_us);
    d->last_us = t_us;
    if (model && model[0] && !d->model[0]) {
        unsigned k = 0;
        for (; model[k] && k < PR_ROSTER_NAME; k++) {
            d->model[k] = model[k];
        }
        d->model[k] = '\0';
    }
    /* THE DEVICE'S OWN WORD BEATS THE OUI TABLE.
     *
     * The prefix table says who registered the address block, which is a good
     * guess and only that - vendors buy blocks and rebadge hardware. The WPS
     * element is the device stating its own manufacturer. When it does, it
     * wins. */
    if (vendor && vendor[0]) {
        static const char *k_known[] = { "TP-Link", "Netgear",  "Ubiquiti",
                                         "ASUS",    "D-Link",   "Linksys",
                                         "Belkin",  "Zyxel",    "Huawei",
                                         "Xiaomi",  "Tenda",    "Mercusys" };
        for (unsigned i = 0; i < sizeof(k_known) / sizeof(k_known[0]); i++) {
            if (strcmp(vendor, k_known[i]) == 0) {
                d->vendor = k_known[i];
                break;
            }
        }
    }
    if (pin_exposed) {
        d->exposure |= RD_EXP_WPS_PIN;
    }
}

uint8_t rd_upload_duty(const rd_device_t *d)
{
    if (!d || !d->heard_seconds) {
        return 0;
    }
    const uint32_t duty = ((uint32_t)d->up_seconds * 100u) / d->heard_seconds;
    return (uint8_t)((duty > 100u) ? 100u : duty);
}

void rd_observe_traffic(rd_roster_t *r, const uint8_t mac[6], uint16_t frame_len,
                        bool towards_ap, uint64_t now_us)
{
    if (!r || !mac) {
        return;
    }
    rd_device_t *d = admit(r, mac, now_us);
    if (!d) {
        return;
    }
    const uint32_t sec = (uint32_t)(now_us / 1000000ull);

    /* Seconds, counted once each. A camera at thirty frames a second would
     * otherwise register thirty "active seconds" per second and clear any
     * continuity threshold instantly - which would make the measure a
     * restatement of volume, the very thing it exists not to be. */
    if (sec != d->last_heard_sec) {
        d->last_heard_sec = sec;
        if (d->heard_seconds < 0xFFFFu) {
            d->heard_seconds++;
        }
    }

    /* ONLY UPLOAD COUNTS.
     *
     * Traffic towards the access point is what the device chose to send. The
     * other direction is what somebody sent IT, which says nothing about
     * whether it is a camera - a tablet streaming a film downloads
     * continuously and is not watching anybody. */
    if (!towards_ap) {
        return;
    }
    /* Tiny frames are not video. Acknowledgements, keep-alives and ARP are
     * upload too, and counting them would let a device that says nothing all
     * day accumulate a perfect duty cycle. */
    if (frame_len < 200u) {
        return;
    }
    d->up_bytes += frame_len;
    if (sec != d->last_up_sec) {
        d->last_up_sec = sec;
        if (d->up_seconds < 0xFFFFu) {
            d->up_seconds++;
        }
    }

    /* All three, together. Duration says it is not a burst, duty says it is
     * not bursty, and volume says it is not a trickle. Any one alone has an
     * innocent explanation. */
    if (d->up_seconds >= RD_STREAM_MIN_SECONDS &&
        rd_upload_duty(d) >= RD_STREAM_MIN_DUTY &&
        d->up_bytes >= RD_STREAM_MIN_BYTES) {
        d->exposure |= RD_EXP_STREAMING;
    }
}

void rd_expire(rd_roster_t *r, uint64_t now_us)
{
    if (!r) {
        return;
    }
    for (unsigned i = 0; i < r->n; i++) {
        if (r->dev[i].in_use && now_us > r->dev[i].last_us &&
            now_us - r->dev[i].last_us > RD_STALE_US) {
            r->dev[i].in_use = false;
        }
    }
}

unsigned rd_count(const rd_roster_t *r)
{
    if (!r) {
        return 0;
    }
    unsigned n = 0;
    for (unsigned i = 0; i < r->n; i++) {
        if (r->dev[i].in_use) {
            n++;
        }
    }
    return n;
}

unsigned rd_exposed_count(const rd_roster_t *r)
{
    if (!r) {
        return 0;
    }
    unsigned n = 0;
    for (unsigned i = 0; i < r->n; i++) {
        if (r->dev[i].in_use && r->dev[i].exposure) {
            n++;
        }
    }
    return n;
}

/* Weight the exposure flags so the worst thing floats to the top of the list.
 * Open and WEP networks are actively dangerous; a leaked name is a privacy
 * matter; a fixed MAC is the mildest. */
static unsigned exposure_weight(uint16_t e)
{
    unsigned w = 0;
    if (e & RD_EXP_OPEN)        w += 100;
    if (e & RD_EXP_WEP)         w += 90;
    if (e & RD_EXP_WPA1)        w += 60;
    if (e & RD_EXP_WPS_PIN)     w += 80; /* a named, documented attack */
    if (e & RD_EXP_WPS)         w += 50;
    if (e & RD_EXP_NO_MFP)      w += 30;
    if (e & RD_EXP_PROBES_OPEN) w += 40;
    if (e & RD_EXP_NAME_LEAK)   w += 20;
    if (e & RD_EXP_FIXED_MAC)   w += 10;
    return w;
}

bool rd_at(const rd_roster_t *r, unsigned i, rd_device_t *out)
{
    if (!r || !out) {
        return false;
    }
    /* Build an order: most-exposed first, then strongest signal. Done on each
     * call rather than kept sorted, because the table is tiny and a sort that
     * runs off stale data is worse than one that is always fresh. */
    unsigned order[PR_ROSTER_MAX];
    unsigned m = 0;
    for (unsigned k = 0; k < r->n; k++) {
        if (r->dev[k].in_use) {
            order[m++] = k;
        }
    }
    for (unsigned a = 1; a < m; a++) {
        const unsigned key = order[a];
        unsigned b = a;
        while (b > 0) {
            const rd_device_t *x = &r->dev[order[b - 1]];
            const rd_device_t *y = &r->dev[key];
            const unsigned wx = exposure_weight(x->exposure);
            const unsigned wy = exposure_weight(y->exposure);
            const bool y_first = (wy > wx) || (wy == wx && y->rssi > x->rssi);
            if (!y_first) {
                break;
            }
            order[b] = order[b - 1];
            b--;
        }
        order[b] = key;
    }
    if (i >= m) {
        return false;
    }
    *out = r->dev[order[i]];
    return true;
}

const char *rd_class_name(rd_class_t c)
{
    switch (c) {
    case RD_PHONE:        return "phone";
    case RD_COMPUTER:     return "computer";
    case RD_ACCESS_POINT: return "access point";
    case RD_ROUTER:       return "router";
    case RD_IOT:          return "IoT device";
    case RD_CAMERA:       return "camera";
    case RD_TV_MEDIA:     return "TV / media";
    case RD_PRINTER:      return "printer";
    case RD_WEARABLE:     return "wearable";
    case RD_UNKNOWN:
    default:              return "unknown";
    }
}

const char *rd_class_icon(rd_class_t c)
{
    switch (c) {
    case RD_PHONE:        return "PHON";
    case RD_COMPUTER:     return "COMP";
    case RD_ACCESS_POINT: return "AP";
    case RD_ROUTER:       return "RTR";
    case RD_IOT:          return "IOT";
    case RD_CAMERA:       return "CAM";
    case RD_TV_MEDIA:     return "TV";
    case RD_PRINTER:      return "PRNT";
    case RD_WEARABLE:     return "WEAR";
    default:              return "?";
    }
}

const char *rd_exposure_tag(const rd_device_t *d)
{
    if (!d || !d->exposure) {
        return NULL;
    }
    /* Same order as the headline and the sort weights, so the short form and
     * the long form can never name different findings. */
    /* First, because it is the only one of these that is about the ROOM
     * rather than about somebody's configuration - and it is the reason a
     * person sweeps a hotel room before unpacking. */
    if (d->exposure & RD_EXP_STREAMING)   return "UPLOADING";
    if (d->exposure & RD_EXP_OPEN)        return "OPEN";
    if (d->exposure & RD_EXP_WEP)         return "WEP";
    if (d->exposure & RD_EXP_WPA1)        return "WPA1/TKIP";
    if (d->exposure & RD_EXP_WPS_PIN)     return "WPS PIN";
    if (d->exposure & RD_EXP_WPS)         return "WPS on";
    if (d->exposure & RD_EXP_PROBES_OPEN) return "auto-join";
    if (d->exposure & RD_EXP_NO_MFP)      return "no 11w";
    if (d->exposure & RD_EXP_NAME_LEAK)   return "name leak";
    if (d->exposure & RD_EXP_FIXED_MAC)   return "fixed MAC";
    return NULL;
}

const char *rd_exposure_headline(const rd_device_t *d)
{
    if (!d || !d->exposure) {
        return NULL;
    }
    /* Worst first, matching the sort weights. */
    if (d->exposure & RD_EXP_OPEN)        return "open network - no encryption";
    if (d->exposure & RD_EXP_WEP)         return "WEP - broken since 2004";
    if (d->exposure & RD_EXP_WPA1)        return "WPA1/TKIP - deprecated";
    if (d->exposure & RD_EXP_WPS_PIN)
        return "WPS 1.0 PIN - brute-forceable";
    if (d->exposure & RD_EXP_WPS)         return "WPS on - PIN can be forced";
    if (d->exposure & RD_EXP_PROBES_OPEN) return "will auto-join open Wi-Fi";
    if (d->exposure & RD_EXP_NO_MFP)      return "no 802.11w - deauth works";
    if (d->exposure & RD_EXP_NAME_LEAK)   return "broadcasts a personal name";
    if (d->exposure & RD_EXP_FIXED_MAC)   return "fixed MAC - trackable";
    return NULL;
}

/* ---- the CWE checklist ---------------------------------------------------
 *
 * Per class, the weakness CATEGORIES that class is known for, phrased as things
 * to check. These are not claims about any one device; they are what a person
 * securing a device of this kind should verify. A desktop companion tool can
 * turn each into a live CVE query for the specific model.
 */
unsigned rd_class_cwes(rd_class_t c, const char **out, unsigned max)
{
    static const char *cam[] = {
        "CWE-798 hardcoded credentials",
        "CWE-306 missing authentication",
        "CWE-259 default password",
        "CWE-79 web UI XSS",
    };
    static const char *iot[] = {
        "CWE-798 hardcoded credentials",
        "CWE-311 unencrypted transport",
        "CWE-1188 insecure default config",
    };
    static const char *rtr[] = {
        "CWE-78 command injection in web UI",
        "CWE-798 hardcoded credentials",
        "CWE-306 unauthenticated admin",
        "CWE-16 outdated firmware",
    };
    static const char *prn[] = {
        "CWE-306 unauthenticated admin",
        "CWE-200 stored-document exposure",
    };
    static const char *media[] = {
        "CWE-306 unauthenticated control API",
        "CWE-319 cleartext streaming",
    };
    static const char *comp[] = {
        "CWE-16 unpatched OS / services",
        "CWE-522 weak credentials",
    };
    static const char *phone[] = {
        "CWE-16 outdated OS",
        "CWE-200 name/location leakage",
    };
    const char **src = NULL;
    unsigned n = 0;
    switch (c) {
    case RD_CAMERA:   src = cam;   n = 4; break;
    case RD_IOT:      src = iot;   n = 3; break;
    case RD_ROUTER:
    case RD_ACCESS_POINT: src = rtr; n = 4; break;
    case RD_PRINTER:  src = prn;   n = 2; break;
    case RD_TV_MEDIA: src = media; n = 2; break;
    case RD_COMPUTER: src = comp;  n = 2; break;
    case RD_PHONE:    src = phone; n = 2; break;
    default:          return 0;
    }
    if (n > max) {
        n = max;
    }
    for (unsigned i = 0; i < n; i++) {
        out[i] = src[i];
    }
    return n;
}

unsigned rd_export(const rd_roster_t *r, char *buf, unsigned cap, bool redact)
{
    if (!r || !buf || !cap) {
        return 0;
    }
    unsigned w = 0;
    for (unsigned i = 0; i < r->n && w < cap; i++) {
        const rd_device_t *d = &r->dev[i];
        if (!d->in_use) {
            continue;
        }
        char mac[18];
        if (redact) {
            /* Vendor octets kept - they carry the class and are public - and
             * the host-unique half hashed to a short tag, so the export can be
             * shared without naming anyone's specific device. */
            const uint32_t h = (uint32_t)(d->mac[3] * 65599u + d->mac[4] * 257u +
                                          d->mac[5]);
            snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%04x",
                     d->mac[0], d->mac[1], d->mac[2], (unsigned)(h & 0xFFFF));
        } else {
            snprintf(mac, sizeof(mac), "%02x:%02x:%02x:%02x:%02x:%02x",
                     d->mac[0], d->mac[1], d->mac[2], d->mac[3], d->mac[4],
                     d->mac[5]);
        }
        /* The MODEL is the column that makes this export worth having: it is
         * what a CVE database takes. Everything else is context. */
        const int n = snprintf(buf + w, cap - w, "%s\t%s\t%s\t%s\t0x%04x\n",
                               d->vendor ? d->vendor : "?",
                               d->model[0] ? d->model : "-",
                               rd_class_name(d->klass), mac,
                               (unsigned)d->exposure);
        if (n <= 0 || (unsigned)n >= cap - w) {
            break;
        }
        w += (unsigned)n;
    }
    return w;
}
