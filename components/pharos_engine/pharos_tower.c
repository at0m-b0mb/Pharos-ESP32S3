/* Pharos - the Watchtower rotation. See pharos_tower.h for why this is a
 * rotation and not six simultaneous watches. */
#include "pharos_tower.h"

#include <string.h>

static void copy_bounded(char *dst, size_t cap, const char *src)
{
    size_t i = 0;
    if (!src) {
        dst[0] = '\0';
        return;
    }
    for (; src[i] && i + 1u < cap; i++) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

void ptw_reset(ptw_state_st *s, uint32_t dwell_ms)
{
    if (!s) {
        return;
    }
    memset(s, 0, sizeof(*s));
    /* A floor, because a rotation faster than the engines' own windows would
     * hand every watch a slice too short to conclude anything - six lenses all
     * reporting UNKNOWN forever, which looks exactly like six broken lenses. */
    s->dwell_ms = (dwell_ms < 2000u) ? 2000u : dwell_ms;
}

int ptw_find(const ptw_state_st *s, const char *id)
{
    if (!s || !id) {
        return -1;
    }
    for (unsigned i = 0; i < s->n; i++) {
        if (strcmp(s->w[i].id, id) == 0) {
            return (int)i;
        }
    }
    return -1;
}

int ptw_arm_every(ptw_state_st *s, const char *id, const char *name,
                  uint8_t period)
{
    if (!s || !id || !id[0] || s->n >= PTW_MAX_WATCHES) {
        return -1;
    }
    if (ptw_find(s, id) >= 0) {
        return -1;
    }
    if (period < 1u) {
        period = 1u;
    }
    if (period > PTW_MAX_PERIOD) {
        /* A watch that only ran every fifth lap would be stale on the ring
         * more often than not, which is a dot that means nothing. */
        period = PTW_MAX_PERIOD;
    }
    ptw_watch_t *w = &s->w[s->n];
    memset(w, 0, sizeof(*w));
    copy_bounded(w->id, sizeof(w->id), id);
    copy_bounded(w->name, sizeof(w->name), name ? name : id);
    w->state = PTW_UNKNOWN;
    w->period = period;
    w->armed = true;
    return (int)s->n++;
}

int ptw_arm(ptw_state_st *s, const char *id, const char *name)
{
    return ptw_arm_every(s, id, name, 1u);
}

/* Is this watch due on the lap now in progress? */
static bool due_now(const ptw_state_st *s, unsigned i)
{
    const ptw_watch_t *w = &s->w[i];
    if (!w->armed) {
        return false;
    }
    const uint8_t p = w->period ? w->period : 1u;
    return (s->rotations % (uint32_t)p) == 0u;
}

void ptw_report(ptw_state_st *s, const char *id, ptw_state_t state,
                uint8_t score, uint8_t ceiling, uint64_t now_us)
{
    const int i = ptw_find(s, id);
    if (i < 0) {
        return;
    }
    ptw_watch_t *w = &s->w[i];
    w->state = state;
    w->score = score;
    w->ceiling = ceiling;
    w->seen_us = now_us;

    /* The high-water mark. A quiet reading never lowers it: the air going
     * calm again is not evidence that nothing happened, and this is the only
     * record that it did. */
    if (state > w->peak_state ||
        (state == w->peak_state && score > w->peak_score)) {
        w->peak_state = state;
        w->peak_score = score;
        w->peak_us = now_us;
    }
}

void ptw_acknowledge(ptw_state_st *s)
{
    if (!s) {
        return;
    }
    for (unsigned i = 0; i < s->n; i++) {
        s->w[i].peak_state = PTW_UNKNOWN;
        s->w[i].peak_score = 0;
        s->w[i].peak_us = 0;
    }
}

/* One full pass of the ring, in microseconds. Used as the unit for freshness
 * so that arming another watch cannot quietly turn honest dots into lying
 * ones: add a seventh watch and every reading is allowed to be correspondingly
 * older before it counts as stale, because that is genuinely how much longer
 * it now takes to get back round. */
static uint64_t rotation_us(const ptw_state_st *s)
{
    /* A lap is however many watches are due on it, and that varies - so the
     * unit of freshness is the AVERAGE lap: the total work of one full cycle
     * of every period, divided by the laps in it. Using the shortest lap would
     * declare a period-2 watch stale the moment it skipped a turn it was never
     * due to take. */
    unsigned slices = 0;
    for (unsigned i = 0; i < s->n; i++) {
        if (!s->w[i].armed) {
            continue;
        }
        const uint8_t p = s->w[i].period ? s->w[i].period : 1u;
        /* Turns this watch takes per PTW_MAX_PERIOD laps. */
        slices += PTW_MAX_PERIOD / p;
    }
    if (!slices) {
        slices = 1;
    }
    const uint64_t cycle = (uint64_t)slices * (uint64_t)s->dwell_ms * 1000ull;
    return cycle / PTW_MAX_PERIOD;
}

ptw_freshness_t ptw_freshness(const ptw_state_st *s, unsigned i, uint64_t now_us)
{
    if (!s || i >= s->n) {
        return PTW_EXPIRED;
    }
    const ptw_watch_t *w = &s->w[i];
    /* Never having looked is not the same as having a stale reading, but for
     * drawing purposes both are "do not stand behind this". */
    if (!w->seen_us || now_us < w->seen_us) {
        return PTW_EXPIRED;
    }
    const uint64_t age = now_us - w->seen_us;
    /* Scaled by THIS watch's period: a survey that is only due every second
     * lap must not be called stale for arriving exactly on schedule. */
    const uint64_t rot =
        rotation_us(s) * (uint64_t)(w->period ? w->period : 1u);
    if (age <= rot * PTW_AGEING_ROTATIONS) {
        return PTW_FRESH;
    }
    if (age <= rot * PTW_EXPIRED_ROTATIONS) {
        return PTW_AGEING;
    }
    return PTW_EXPIRED;
}

int ptw_turn(ptw_state_st *s, uint64_t now_us, bool hold, bool *changed)
{
    if (changed) {
        *changed = false;
    }
    if (!s || !s->n) {
        return -1;
    }
    unsigned armed = 0;
    for (unsigned i = 0; i < s->n; i++) {
        if (s->w[i].armed) {
            armed++;
        }
    }
    if (!armed) {
        return -1;
    }

    /* First call: start the clock and take the first armed watch. */
    if (!s->handover_us) {
        s->handover_us = now_us;
        s->cursor = 0;
        while (!s->w[s->cursor].armed) {
            s->cursor = (s->cursor + 1u) % s->n;
        }
        s->w[s->cursor].visits++;
        if (changed) {
            *changed = true;
        }
        return (int)s->cursor;
    }

    /* STAYING PUT WHILE SOMETHING IS HAPPENING.
     *
     * A rotation that walks away from an attack in progress to keep its rota
     * tidy is worse than no rotation: the one moment the operator needs a full
     * dwell is the moment evidence is arriving, and that is also exactly when
     * the confidence ceiling most needs the airtime. The caller says when the
     * active watch is earning its slice, and the tower simply waits. */
    const uint64_t slice = (uint64_t)s->dwell_ms * 1000ull;
    const bool slice_done =
        (now_us >= s->handover_us) && (now_us - s->handover_us >= slice);

    if (!slice_done) {
        return (int)s->cursor; /* mid-slice; nothing to decide */
    }

    /* The slice is up. A watch that is hearing something may keep it - but
     * only so many times in a row. An unbounded hold starves the whole ring,
     * and it does so quietest where it matters most: the other watches simply
     * never run, and their dots stay hollow forever. */
    if (hold && s->held < PTW_MAX_HOLD_SLICES) {
        s->held++;
        s->handover_us = now_us; /* extend, do not accumulate debt */
        return (int)s->cursor;
    }
    s->held = 0;

    const unsigned was = s->cursor;
    /* Two full sweeps at most: one to find the next watch due on this lap, and
     * if the lap has nothing else due, a second after the lap counter turns
     * over. Without the second pass a ring of period-2 watches would stall the
     * moment it reached an odd lap. */
    bool moved = false;
    for (unsigned pass = 0; pass < 2u && !moved; pass++) {
        for (unsigned step = 0; step < s->n; step++) {
            const unsigned next = (s->cursor + 1u) % s->n;
            if (next <= s->cursor) {
                s->rotations++; /* the lap turned over */
            }
            s->cursor = next;
            if (due_now(s, s->cursor)) {
                moved = true;
                break;
            }
        }
    }
    s->handover_us = now_us;
    s->w[s->cursor].visits++;
    if (changed) {
        *changed = (s->cursor != was);
    }
    return (int)s->cursor;
}

bool ptw_set_armed(ptw_state_st *s, unsigned i, bool armed)
{
    if (!s || i >= s->n) {
        return false;
    }
    if (s->w[i].armed == armed) {
        return true;
    }
    if (!armed) {
        /* The last one cannot be switched off. The ring's own controls are
         * reached THROUGH the ring, so a watchtower watching nothing would be
         * a state somebody could enter and not obviously get out of. */
        unsigned others = 0;
        for (unsigned k = 0; k < s->n; k++) {
            if (k != i && s->w[k].armed) {
                others++;
            }
        }
        if (!others) {
            return false;
        }
    }
    s->w[i].armed = armed;
    if (armed) {
        /* A watch just switched on has no reading, and must not inherit the
         * stale one it had before it was switched off. */
        s->w[i].seen_us = 0;
        s->w[i].state = PTW_UNKNOWN;
        s->w[i].score = 0;
    } else if (s->cursor == i) {
        /* It was holding the radio. Let the next turn move on rather than
         * leaving the cursor on something that is no longer running. */
        s->handover_us = 0;
    }
    return true;
}

bool ptw_set_period(ptw_state_st *s, unsigned i, uint8_t period)
{
    if (!s || i >= s->n) {
        return false;
    }
    if (period < 1u) {
        period = 1u;
    }
    if (period > PTW_MAX_PERIOD) {
        period = PTW_MAX_PERIOD;
    }
    s->w[i].period = period;
    return true;
}

uint32_t ptw_lap_ms(const ptw_state_st *s)
{
    if (!s) {
        return 0;
    }
    /* The average lap, in the same terms rotation_us uses: total work of one
     * full cycle of every period, divided by the laps in it. */
    unsigned slices = 0;
    for (unsigned i = 0; i < s->n; i++) {
        if (!s->w[i].armed) {
            continue;
        }
        const uint8_t p = s->w[i].period ? s->w[i].period : 1u;
        slices += PTW_MAX_PERIOD / p;
    }
    return (uint32_t)((slices * s->dwell_ms) / PTW_MAX_PERIOD);
}

void ptw_summarise(const ptw_state_st *s, uint64_t now_us, ptw_summary_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->worst_index = -1;
    out->latched_index = -1;
    out->latched_state = PTW_UNKNOWN;
    out->worst = PTW_UNKNOWN;
    if (!s) {
        out->headline = "no watches";
        return;
    }

    for (unsigned i = 0; i < s->n; i++) {
        const ptw_watch_t *w = &s->w[i];
        if (!w->armed) {
            continue;
        }
        out->armed++;

        const ptw_freshness_t f = ptw_freshness(s, i, now_us);
        if (!w->seen_us) {
            out->unknown++;
            continue;
        }
        if (f == PTW_EXPIRED) {
            /* Had a reading once, but not one to stand behind now. It counts
             * as neither reporting nor quiet - saying "all quiet" on the
             * strength of a reading three rotations old is the exact comfort
             * this engine exists to refuse. */
            continue;
        }
        out->reporting++;
        if (w->state <= PTW_QUIET) {
            out->quiet++;
        }
        if (w->state == PTW_ALARM) {
            out->alarms++;
        }
        if (w->state > out->worst ||
            (w->state == out->worst && out->worst_index >= 0 &&
             w->score > s->w[out->worst_index].score)) {
            out->worst = w->state;
            out->worst_index = (int)i;
        }
    }

    /* WHAT HAPPENED WHILE NOBODY WAS LOOKING.
     *
     * Scanned across every armed watch regardless of freshness, because that
     * is the entire point: a burst that lasted two seconds four minutes ago is
     * exactly the thing a rotation exists to catch and a live reading cannot
     * show. */
    for (unsigned i = 0; i < s->n; i++) {
        const ptw_watch_t *w = &s->w[i];
        if (!w->armed || w->peak_state < PTW_ELEVATED) {
            continue;
        }
        if (w->peak_state > out->latched_state) {
            out->latched_state = w->peak_state;
            out->latched_index = (int)i;
            out->latched_age_s =
                (now_us > w->peak_us) ? (uint32_t)((now_us - w->peak_us) / 1000000ull)
                                      : 0u;
        }
    }

    /* The headline says what is TRUE of the watches that have actually looked
     * recently - never "all quiet" on the strength of watches that have not. */
    if (out->alarms) {
        out->headline = (out->alarms > 1u) ? "ALERTS" : "ALERT";
    } else if (out->latched_state >= PTW_ALARM) {
        /* Not happening now, but it happened. Distinguished from a live alarm
         * in words as well as in the age reported alongside it - somebody
         * walking up to the device must be able to tell "there is an attack"
         * from "there was one". */
        out->headline = "SOMETHING HAPPENED";
    } else if (out->worst == PTW_ELEVATED) {
        out->headline = "something is up";
    } else if (out->worst == PTW_NOTED) {
        out->headline = "worth a look";
    } else if (!out->armed) {
        out->headline = "no watches";
    } else if (!out->reporting) {
        out->headline = "still listening";
    } else if (out->reporting < out->armed) {
        /* Deliberately not "all quiet": some of the ring has not reported
         * inside its own window, and the operator should know the picture is
         * partial rather than be told everything is fine. */
        out->headline = "quiet so far";
    } else {
        out->headline = "all quiet";
    }
}

const char *ptw_state_name(ptw_state_t st)
{
    switch (st) {
    case PTW_UNKNOWN:  return "not yet";
    case PTW_QUIET:    return "quiet";
    case PTW_NOTED:    return "noted";
    case PTW_ELEVATED: return "elevated";
    case PTW_ALARM:    return "ALARM";
    default:           return "?";
    }
}
