/* Pharos - the Survey. See pharos_survey.h for the rules that keep this from
 * becoming a scare sheet. */
#include "pharos_survey.h"

#include <stdio.h>
#include <string.h>

static bool same_addr(const uint8_t *a, const uint8_t *b)
{
    return memcmp(a, b, 6) == 0;
}

void psv_reset(psv_t *s, uint64_t now_us)
{
    if (!s) {
        return;
    }
    memset(s, 0, sizeof(*s));
    s->started_us = now_us;
    s->last_us = now_us;
}

void psv_note_network(psv_t *s, const uint8_t bssid[6], uint8_t grade,
                      uint32_t flags, uint64_t now_us)
{
    if (!s || !bssid) {
        return;
    }
    s->last_us = now_us;
    for (unsigned i = 0; i < s->n_net; i++) {
        if (same_addr(s->net[i].addr, bssid)) {
            /* Seen before. Take the WORST grade this network has shown and
             * the union of its flags: a network that was briefly heard without
             * its RSN element should not be able to talk its way up by being
             * heard again more favourably. */
            if (grade > s->net[i].grade) {
                s->net[i].grade = grade;
            }
            s->net[i].flags |= flags;
            return;
        }
    }
    if (s->n_net >= PSV_MAX_NETWORKS) {
        /* Say so rather than quietly under-counting. A number that stops
         * rising with no explanation is worse than one that admits its limit. */
        s->net_full = true;
        return;
    }
    memcpy(s->net[s->n_net].addr, bssid, 6);
    s->net[s->n_net].grade = grade;
    s->net[s->n_net].flags = flags;
    s->n_net++;
}

void psv_note_device(psv_t *s, const uint8_t mac[6], uint8_t names,
                     bool randomised, uint64_t now_us)
{
    if (!s || !mac) {
        return;
    }
    s->last_us = now_us;
    for (unsigned i = 0; i < s->n_dev; i++) {
        if (same_addr(s->dev[i].addr, mac)) {
            if (names > s->dev[i].names) {
                s->dev[i].names = names;
            }
            s->dev[i].randomised = s->dev[i].randomised || randomised;
            return;
        }
    }
    if (s->n_dev >= PSV_MAX_DEVICES) {
        s->dev_full = true;
        return;
    }
    memcpy(s->dev[s->n_dev].addr, mac, 6);
    s->dev[s->n_dev].names = names;
    s->dev[s->n_dev].randomised = randomised;
    s->n_dev++;
}

void psv_note_tool(psv_t *s, uint8_t kind, bool present, uint64_t now_us)
{
    if (!s || !kind) {
        return;
    }
    s->last_us = now_us;
    for (unsigned i = 0; i < s->n_tool; i++) {
        if (s->tool[i].kind == kind) {
            s->tool[i].present = present;
            if (present) {
                s->tool[i].last_us = now_us;
            }
            return;
        }
    }
    if (s->n_tool >= PSV_MAX_TOOLS) {
        return;
    }
    s->tool[s->n_tool].kind = kind;
    s->tool[s->n_tool].first_us = now_us;
    s->tool[s->n_tool].last_us = now_us;
    s->tool[s->n_tool].present = present;
    s->n_tool++;
}

void psv_summarise(const psv_t *s, uint64_t now_us, psv_report_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->headline = "nothing surveyed yet";
    if (!s) {
        return;
    }

    for (unsigned i = 0; i < s->n_net; i++) {
        const uint32_t f = s->net[i].flags;
        out->networks++;
        if (f & PSV_NET_OPEN)   out->open++;
        if (f & PSV_NET_NO_MFP) out->no_mfp++;
        if (f & PSV_NET_WPS)    out->wps++;
        if (f & PSV_NET_WEAK)   out->weak++;
        if (f & PSV_NET_HIDDEN) out->hidden++;
        if (f & PSV_NET_MODERN) out->modern++;
        if (s->net[i].grade > out->worst_grade) {
            out->worst_grade = s->net[i].grade;
        }
    }
    for (unsigned i = 0; i < s->n_dev; i++) {
        if (!s->dev[i].names) {
            continue;
        }
        out->devices++;
        out->names_leaked += s->dev[i].names;
        /* The interesting ones: a private address is supposed to stop a device
         * being followed, and announcing the names of remembered networks
         * undoes it completely. */
        if (s->dev[i].randomised) {
            out->trackable++;
        }
    }
    for (unsigned i = 0; i < s->n_tool; i++) {
        out->tools++;
        if (s->tool[i].present) {
            out->tools_present++;
        }
    }

    out->truncated = s->net_full || s->dev_full;
    if (now_us > s->started_us) {
        out->minutes = (uint32_t)((now_us - s->started_us) / 60000000ull);
    }

    /* The headline leads with whatever is most worth knowing, and NEVER calls
     * a badly configured neighbour an attack. */
    if (out->tools_present) {
        out->headline = "pentest hardware in the room";
    } else if (out->open) {
        out->headline = "open networks here";
    } else if (out->no_mfp && out->no_mfp == out->networks) {
        out->headline = "every network here is floodable";
    } else if (out->no_mfp * 2u > out->networks) {
        out->headline = "most networks are floodable";
    } else if (out->no_mfp) {
        /* "Most" when it is one in four is the kind of overstatement that
         * makes a device untrustworthy the first time somebody checks it. The
         * count is on the line below either way; the word has to match it. */
        out->headline = "some networks are floodable";
    } else if (out->devices) {
        out->headline = "devices are naming their networks";
    } else if (out->networks) {
        out->headline = "nothing alarming here";
    }
}

/* THE PLAIN-ENGLISH LINES.
 *
 * This is the whole point of the survey, and the reason the text is here in
 * the engine rather than in the lens: the wording is a judgement about what is
 * true and how strongly to say it, which makes it exactly the sort of thing
 * that should be host-tested rather than retyped into a display function.
 *
 * Ordered by usefulness, not severity - "6 of 23 networks" is more use than a
 * grade letter, and a count somebody can act on beats a count that only ranks. */
bool psv_line(const psv_report_t *r, unsigned index, char *buf, unsigned cap)
{
    if (!r || !buf || cap < 8) {
        return false;
    }
    /* EVERY LINE FITS char[26] AT FULL SCALE.
     *
     * A detail row's left column is twenty-five characters and a terminator.
     * These sentences were written against single-digit counts and one of them
     * reached the glass as "2 would drop clients if f" - a sentence cut in
     * half, which is worse than no sentence. They are now written for the
     * LONGEST count that can occur, and test_survey.c checks them at that
     * length rather than at a convenient one. */
    unsigned k = 0;

    if (r->networks) {
        if (index == k++) {
            snprintf(buf, cap, "%u networks seen here", r->networks);
            return true;
        }
    }
    if (r->no_mfp) {
        if (index == k++) {
            /* The single most useful sentence this device can say to somebody
             * who is not a radio engineer. */
            snprintf(buf, cap, "%u drop clients if hit", r->no_mfp);
            return true;
        }
    }
    if (r->open) {
        if (index == k++) {
            snprintf(buf, cap, "%u open to anyone", r->open);
            return true;
        }
    }
    if (r->weak) {
        if (index == k++) {
            snprintf(buf, cap, "%u use broken crypto", r->weak);
            return true;
        }
    }
    if (r->wps) {
        if (index == k++) {
            snprintf(buf, cap, "%u leave WPS on", r->wps);
            return true;
        }
    }
    if (r->modern) {
        if (index == k++) {
            /* The good news is reported too. A device that only ever lists
             * faults teaches its operator that everything is always bad. */
            snprintf(buf, cap, "%u run WPA3 or OWE", r->modern);
            return true;
        }
    }
    if (r->devices) {
        if (index == k++) {
            /* The exact number of names is on the counts row; a five-digit
             * total in a sentence would not fit and would not help. */
            snprintf(buf, cap, "%u devices leak names", r->devices);
            return true;
        }
    }
    if (r->trackable) {
        if (index == k++) {
            snprintf(buf, cap, "%u beat MAC randomising", r->trackable);
            return true;
        }
    }
    if (r->tools) {
        if (index == k++) {
            if (r->tools_present == 1u) {
                snprintf(buf, cap, "a pentest device is here");
            } else if (r->tools_present) {
                snprintf(buf, cap, "%u pentest devices here", r->tools_present);
            } else if (r->tools == 1u) {
                snprintf(buf, cap, "a pentest device was here");
            } else {
                snprintf(buf, cap, "%u pentest seen earlier", r->tools);
            }
            return true;
        }
    }
    if (r->truncated) {
        if (index == k++) {
            /* An honest floor, stated as one. */
            snprintf(buf, cap, "counts are a minimum");
            return true;
        }
    }
    if (!k) {
        if (index == 0) {
            snprintf(buf, cap, "listening - nothing yet");
            return true;
        }
    }
    return false;
}
