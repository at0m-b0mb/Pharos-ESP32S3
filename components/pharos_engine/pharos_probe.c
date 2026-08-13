#include "pharos_probe.h"

#include <string.h>

/* ---- name classification -------------------------------------------- */

/* Substring tables. Deliberately conservative: a wrong classification in the
 * revealing direction turns a demonstration into an accusation, so anything
 * ambiguous stays UNKNOWN and scores as if it said nothing. */
static const char *const k_retail[] = {
    "starbucks", "costa", "mcdonald", "subway", "kfc", "pret", "nero",
    "burgerking", "dunkin", "wetherspoon", "tesco", "sainsbury", "walmart",
    "target", "ikea", NULL
};
static const char *const k_telecom[] = {
    "btwifi", "bt-wifi", "xfinitywifi", "cablewifi", "optimumwifi", "sfr wifi",
    "telstra air", "virginmedia", "swisscom", "vodafone hotspot", "attwifi",
    "eduroam-guest", NULL
};
static const char *const k_transit[] = {
    "airport", "_air", "gogoinflight", "united_wi-fi", "delta wi-fi",
    "aainflight", "amtrak", "trainline", "gwr wifi", "lner", "railwifi",
    "metro wifi", "ferry", NULL
};
/* Chains appear both spaced and squashed in the wild ("PremierInn_Guest",
 * "_Premier Inn Free WiFi"), so both forms are listed rather than trying to
 * normalise whitespace out of a name somebody else chose. */
static const char *const k_hospitality[] = {
    "marriott", "hilton", "hyatt", "premierinn", "premier inn", "travelodge",
    "ibis", "novotel", "holidayinn", "holiday inn", "radisson", "sheraton",
    "airbnb", "hotel", "hostel", "guesthouse", "_bandb", NULL
};
static const char *const k_education[] = {
    "eduroam", "campus", "univ", "college", "school", "academy", "student",
    "library-wifi", "uni-", NULL
};
static const char *const k_vehicle[] = {
    "tesla", "mycar", "bmw", "audi connect", "ford", "toyota", "carplay",
    "vw wifi", "onstar", "caravan", "motorhome", NULL
};
static const char *const k_workplace[] = {
    "corp", "-corp", "internal", "staff", "employee", "office", "hq-",
    "-vpn", "secure-lan", "intranet", "enterprise", NULL
};
static const char *const k_healthcare[] = {
    "nhs", "hospital", "clinic", "surgery", "medical", "health", "dental",
    "pharmacy", "ward", "patient", NULL
};
static const char *const k_home[] = {
    "bthub", "sky", "virgin media", "talktalk", "plusnet", "netgear",
    "linksys", "tp-link", "dlink", "asus", "orbi", "eero", "fritz",
    "home", "casa", "wifi-", "-family", NULL
};
static const char *const k_generic[] = {
    "guest", "public", "free wifi", "freewifi", "wifi", "wireless", "default",
    "hotspot", "internet", NULL
};

static char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/* Case-insensitive substring search over a length-delimited haystack. */
static bool contains(const char *hay, uint8_t hay_len, const char *needle)
{
    const size_t n = strlen(needle);
    if (n == 0 || n > hay_len) {
        return false;
    }
    for (uint8_t i = 0; i + n <= hay_len; i++) {
        size_t j = 0;
        while (j < n && lower(hay[i + j]) == lower(needle[j])) {
            j++;
        }
        if (j == n) {
            return true;
        }
    }
    return false;
}

static bool any(const char *ssid, uint8_t len, const char *const *table)
{
    for (unsigned i = 0; table[i]; i++) {
        if (contains(ssid, len, table[i])) {
            return true;
        }
    }
    return false;
}

pp_place_t pp_classify(const char *ssid, uint8_t len)
{
    if (!ssid || len == 0) {
        return PP_PLACE_UNKNOWN;
    }
    /* Most revealing first: a name can match several tables, and the honest
     * answer is the one that narrows a person down the most. "NHS Guest"
     * is healthcare, not generic. */
    if (any(ssid, len, k_healthcare))  return PP_PLACE_HEALTHCARE;
    if (any(ssid, len, k_workplace))   return PP_PLACE_WORKPLACE;
    if (any(ssid, len, k_vehicle))     return PP_PLACE_VEHICLE;
    if (any(ssid, len, k_education))   return PP_PLACE_EDUCATION;
    if (any(ssid, len, k_hospitality)) return PP_PLACE_HOSPITALITY;
    if (any(ssid, len, k_transit))     return PP_PLACE_TRANSIT;
    if (any(ssid, len, k_telecom))     return PP_PLACE_TELECOM;
    if (any(ssid, len, k_retail))      return PP_PLACE_RETAIL;
    if (any(ssid, len, k_home))        return PP_PLACE_HOME;
    if (any(ssid, len, k_generic))     return PP_PLACE_GENERIC;
    return PP_PLACE_UNKNOWN;
}

/* A name nobody else is likely to be carrying: long, and mixing letters with
 * digits or punctuation the way a personal hotspot or a named household
 * router does. Shared public names are short and wordy. */
static bool looks_unique(const char *ssid, uint8_t len)
{
    if (len < 10) {
        return false;
    }
    unsigned digits = 0, uppers = 0, punct = 0;
    for (uint8_t i = 0; i < len; i++) {
        const char c = ssid[i];
        if (c >= '0' && c <= '9') digits++;
        else if (c >= 'A' && c <= 'Z') uppers++;
        else if (c == '_' || c == '-' || c == '.' || c == '\'') punct++;
    }
    return (digits >= 2 && (uppers >= 2 || punct >= 1)) || (digits >= 4);
}

/* ---- device tracking -------------------------------------------------- */

void pp_reset(pp_engine_t *e)
{
    if (e) {
        memset(e, 0, sizeof(*e));
    }
}

static bool seq_continues(uint16_t last, uint16_t now)
{
    /* 802.11 sequence numbers are 12 bits and wrap. A device that changed
     * its MAC but kept counting will land a small distance ahead; a genuinely
     * different device lands anywhere. Sixty-four is wide enough to survive
     * frames we did not hear and narrow enough to mean something. */
    const uint16_t delta = (uint16_t)((now - last) & 0x0FFF);
    return delta > 0 && delta <= 64;
}

static void remember_network(pp_device_t *d, const char *ssid, uint8_t len)
{
    if (len == 0 || len > PP_SSID_MAX) {
        return;
    }
    for (unsigned i = 0; i < d->n_networks; i++) {
        if (strlen(d->networks[i]) == len && memcmp(d->networks[i], ssid, len) == 0) {
            return;
        }
    }
    if (d->n_networks >= PP_MAX_NETWORKS) {
        return;
    }
    memcpy(d->networks[d->n_networks], ssid, len);
    d->networks[d->n_networks][len] = '\0';
    d->places[d->n_networks] = (uint8_t)pp_classify(ssid, len);
    d->n_networks++;
}

int pp_observe(pp_engine_t *e, const pp_probe_t *p)
{
    if (!e || !p) {
        return -1;
    }
    e->probes_seen++;
    if (p->ssid_len == 0) {
        e->wildcards_seen++;
    }

    /* Exact address match first: the cheap, ordinary case. */
    pp_device_t *dev = NULL;
    for (unsigned i = 0; i < e->n_devices; i++) {
        if (e->devices[i].in_use && memcmp(e->devices[i].addr, p->addr, 6) == 0) {
            dev = &e->devices[i];
            break;
        }
    }

    /* Then the interesting case: a new address whose chipset fingerprint we
     * have seen, arriving on a sequence counter that carries on where the old
     * address left off. That is one device wearing a new name. */
    if (!dev && p->fingerprint) {
        for (unsigned i = 0; i < e->n_devices; i++) {
            pp_device_t *c = &e->devices[i];
            if (!c->in_use || c->fingerprint != p->fingerprint) {
                continue;
            }
            if (seq_continues(c->last_seq, p->seq)) {
                dev = c;
                dev->identities++;
                memcpy(dev->addr, p->addr, 6);
                break;
            }
        }
    }

    if (!dev) {
        if (e->n_devices >= PP_MAX_DEVICES) {
            return -1;
        }
        dev = &e->devices[e->n_devices++];
        memset(dev, 0, sizeof(*dev));
        dev->in_use = true;
        dev->identities = 1;
        dev->best_rssi = -128;
        memcpy(dev->addr, p->addr, 6);
        dev->fingerprint = p->fingerprint;
    }

    dev->probes++;
    dev->last_seq = p->seq;
    dev->last_us = p->t_us;
    if (p->rssi > dev->best_rssi) {
        dev->best_rssi = p->rssi;
    }
    if (p->ssid_len == 0) {
        if (dev->wildcards < 255) dev->wildcards++;
    } else {
        remember_network(dev, p->ssid, p->ssid_len);
    }
    return (int)(dev - e->devices);
}

/* ---- grading ---------------------------------------------------------- */

/* How much knowing you have been somewhere of this kind narrows you down. */
static uint8_t place_weight(pp_place_t p)
{
    switch (p) {
    case PP_PLACE_HEALTHCARE: return 22;
    case PP_PLACE_HOME:       return 20;
    case PP_PLACE_WORKPLACE:  return 18;
    case PP_PLACE_VEHICLE:    return 14;
    case PP_PLACE_EDUCATION:  return 12;
    case PP_PLACE_HOSPITALITY:return 12;
    case PP_PLACE_TRANSIT:    return 8;
    case PP_PLACE_TELECOM:    return 3;
    case PP_PLACE_RETAIL:     return 3;
    case PP_PLACE_GENERIC:    return 1;
    case PP_PLACE_UNKNOWN:
    default:                  return 2;
    }
}

static bool addr_is_random(const uint8_t addr[6])
{
    /* Randomised addresses are locally administered by construction. */
    return (addr[0] & 0x02) != 0;
}

static pp_grade_t grade_of(uint8_t exposure)
{
    if (exposure == 0)  return PP_GRADE_A_PLUS;
    if (exposure <= 12) return PP_GRADE_A;
    if (exposure <= 28) return PP_GRADE_B;
    if (exposure <= 48) return PP_GRADE_C;
    if (exposure <= 70) return PP_GRADE_D;
    return PP_GRADE_F;
}

void pp_grade_device(const pp_device_t *d, pp_verdict_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->headline = "";
    if (!d || !d->in_use) {
        return;
    }

    out->networks = d->n_networks;
    out->identities = d->identities;

    /* Honesty gate. A device that said nothing in the seconds we listened is
     * not necessarily a quiet device; it may simply not have probed. Below
     * four probes the engine declines rather than awarding a good grade the
     * owner did not earn. */
    if (d->probes < 4) {
        out->notes |= PP_NOTE_THIN;
        out->grade = PP_GRADE_UNGRADED;
        out->headline = "Too few probes heard to judge this device";
        return;
    }

    if (!addr_is_random(d->addr)) {
        out->notes |= PP_NOTE_NO_RANDOM;
    }
    if (d->identities > 1) {
        out->notes |= PP_NOTE_RELINKED;
    }
    if (d->n_networks > 0) {
        out->notes |= PP_NOTE_DIRECTED;
    }

    /* Silence is the only way to an A+. */
    if (d->n_networks == 0) {
        out->exposure = 0;
        out->grade = (out->notes & PP_NOTE_NO_RANDOM) ? PP_GRADE_B : PP_GRADE_A_PLUS;
        out->headline = (out->notes & PP_NOTE_NO_RANDOM)
                            ? "Names nothing, but always uses the same address"
                            : "Asks for nothing by name - this is what good looks like";
        return;
    }

    /* Specificity, not volume: ten coffee shops say almost nothing, one
     * hospital says a great deal. */
    unsigned exposure = 0;
    uint8_t narrowest = PP_PLACE_UNKNOWN;
    for (unsigned i = 0; i < d->n_networks; i++) {
        const pp_place_t place = (pp_place_t)d->places[i];
        exposure += place_weight(place);
        if (place > narrowest) {
            narrowest = (uint8_t)place;
        }
        if (looks_unique(d->networks[i], (uint8_t)strlen(d->networks[i]))) {
            exposure += 6;
            out->notes |= PP_NOTE_UNIQUE_NAME;
        }
    }
    out->narrowest = (pp_place_t)narrowest;

    /* Being followed across a MAC change is worse than any single name. */
    if (out->notes & PP_NOTE_RELINKED) {
        exposure += 12;
    }

    if (exposure > 100) {
        exposure = 100;
    }
    out->exposure = (uint8_t)exposure;
    out->grade = grade_of(out->exposure);

    /* A device that never randomises is trackable regardless of what it
     * names, so it cannot be graded above D however little it says. */
    if ((out->notes & PP_NOTE_NO_RANDOM) && out->grade < PP_GRADE_D) {
        out->grade = PP_GRADE_D;
    }

    if (narrowest >= PP_PLACE_WORKPLACE) {
        out->headline = "Names a place that identifies its owner, not just a network";
    } else if (out->notes & PP_NOTE_RELINKED) {
        out->headline = "Changed address, and was followed through it anyway";
    } else if (out->grade <= PP_GRADE_B) {
        out->headline = "Names a few networks, none of them revealing";
    } else {
        out->headline = "Announces a history of places to anyone listening";
    }
}

const char *pp_place_name(pp_place_t p)
{
    switch (p) {
    case PP_PLACE_GENERIC:     return "generic";
    case PP_PLACE_RETAIL:      return "shop or cafe";
    case PP_PLACE_TELECOM:     return "carrier hotspot";
    case PP_PLACE_TRANSIT:     return "travel";
    case PP_PLACE_HOSPITALITY: return "hotel";
    case PP_PLACE_EDUCATION:   return "education";
    case PP_PLACE_VEHICLE:     return "vehicle";
    case PP_PLACE_WORKPLACE:   return "workplace";
    case PP_PLACE_HEALTHCARE:  return "healthcare";
    case PP_PLACE_HOME:        return "home";
    case PP_PLACE_UNKNOWN:
    default:                   return "unclassified";
    }
}

const char *pp_grade_name(pp_grade_t g)
{
    switch (g) {
    case PP_GRADE_A_PLUS: return "A+";
    case PP_GRADE_A:      return "A";
    case PP_GRADE_B:      return "B";
    case PP_GRADE_C:      return "C";
    case PP_GRADE_D:      return "D";
    case PP_GRADE_F:      return "F";
    case PP_GRADE_UNGRADED:
    default:              return "--";
    }
}

const char *pp_grade_advice(const pp_verdict_t *v)
{
    if (!v) {
        return "";
    }
    if (v->grade == PP_GRADE_UNGRADED) {
        return "Not enough probes heard. Stay in range longer.";
    }
    if (v->notes & PP_NOTE_NO_RANDOM) {
        return "This device uses one fixed address everywhere. Turn on private "
               "or randomised Wi-Fi addresses in its settings.";
    }
    if (v->notes & PP_NOTE_RELINKED) {
        return "Randomisation is on, but the device was still followed through "
               "an address change. Forgetting old networks is what actually "
               "helps - the names are the leak, not the address.";
    }
    if (v->narrowest >= PP_PLACE_WORKPLACE) {
        return "Forget the networks that name a specific place. A phone only "
               "needs to remember the ones it uses now.";
    }
    if (v->networks > 0) {
        return "Forget saved networks you no longer use. Each one is announced "
               "to every receiver in the room.";
    }
    return "This device asks for nothing by name. Nothing more to do here.";
}
