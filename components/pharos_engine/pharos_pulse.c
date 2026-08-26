/* Pharos - the activity ribbon. See pharos_pulse.h for why it is shared. */
#include "pharos_pulse.h"

#include <string.h>

void pharos_pulse_reset(pharos_pulse_t *p)
{
    if (p) {
        memset(p, 0, sizeof(*p));
    }
}

/* Advance the ring to `sec`, clearing whatever it passes over.
 *
 * A gap longer than the window means everything in it is stale, and clearing
 * slot by slot for a device that has been idle for an hour would be a very
 * long loop - so a jump that exceeds the window wipes the lot in one go. */
static void roll_to(pharos_pulse_t *p, uint32_t sec)
{
    if (!p->primed) {
        p->primed = true;
        p->newest_sec = sec;
        p->newest_idx = 0;
        return;
    }
    if (sec == p->newest_sec) {
        return;
    }
    /* Time going BACKWARDS is not something a monotonic clock does, but a
     * lens that reset its own clock could hand us one. Treat it as a fresh
     * start rather than rolling 4 billion slots. */
    if (sec < p->newest_sec) {
        memset(p->slot, 0, sizeof(p->slot));
        p->newest_sec = sec;
        p->newest_idx = 0;
        return;
    }
    const uint32_t gap = sec - p->newest_sec;
    if (gap >= PHAROS_PULSE_SLOTS) {
        memset(p->slot, 0, sizeof(p->slot));
        p->newest_sec = sec;
        p->newest_idx = 0;
        return;
    }
    for (uint32_t i = 0; i < gap; i++) {
        p->newest_idx = (uint8_t)((p->newest_idx + 1u) % PHAROS_PULSE_SLOTS);
        p->slot[p->newest_idx] = 0;
    }
    p->newest_sec = sec;
}

void pharos_pulse_add(pharos_pulse_t *p, uint64_t t_us, uint16_t n)
{
    if (!p || n == 0u) {
        return;
    }
    roll_to(p, (uint32_t)(t_us / 1000000ull));
    uint32_t v = (uint32_t)p->slot[p->newest_idx] + n;
    p->slot[p->newest_idx] = (uint16_t)(v > 0xFFFFu ? 0xFFFFu : v);
}

void pharos_pulse_note(pharos_pulse_t *p, uint64_t t_us)
{
    pharos_pulse_add(p, t_us, 1u);
}

/* Walk the ring oldest-first into a caller's buffer, rolling forward to
 * `now` first so that seconds which have passed with no events show as the
 * silence they were rather than as whatever was last in that slot. */
static void snapshot(const pharos_pulse_t *p, uint64_t now_us,
                     uint16_t out[PHAROS_PULSE_SLOTS])
{
    pharos_pulse_t tmp = *p;
    roll_to(&tmp, (uint32_t)(now_us / 1000000ull));
    for (unsigned i = 0; i < PHAROS_PULSE_SLOTS; i++) {
        const unsigned idx =
            (tmp.newest_idx + 1u + i) % PHAROS_PULSE_SLOTS;
        out[i] = tmp.slot[idx];
    }
}

uint16_t pharos_pulse_peak(const pharos_pulse_t *p, uint64_t now_us)
{
    if (!p || !p->primed) {
        return 0;
    }
    uint16_t buf[PHAROS_PULSE_SLOTS];
    snapshot(p, now_us, buf);
    uint16_t peak = 0;
    for (unsigned i = 0; i < PHAROS_PULSE_SLOTS; i++) {
        if (buf[i] > peak) {
            peak = buf[i];
        }
    }
    return peak;
}

bool pharos_pulse_fill(const pharos_pulse_t *p, uint64_t now_us,
                       uint8_t out[PHAROS_PULSE_SLOTS])
{
    if (!out) {
        return false;
    }
    memset(out, 0, PHAROS_PULSE_SLOTS);
    if (!p || !p->primed) {
        return false; /* never measured: draw nothing, not a flat line */
    }

    uint16_t buf[PHAROS_PULSE_SLOTS];
    snapshot(p, now_us, buf);

    uint16_t peak = 0;
    for (unsigned i = 0; i < PHAROS_PULSE_SLOTS; i++) {
        if (buf[i] > peak) {
            peak = buf[i];
        }
    }
    if (peak == 0u) {
        /* Primed but silent for the whole window. That IS a measurement - the
         * room was quiet - so the ribbon is drawn, empty. */
        return true;
    }
    for (unsigned i = 0; i < PHAROS_PULSE_SLOTS; i++) {
        out[i] = (uint8_t)(((uint32_t)buf[i] * 255u) / peak);
    }
    return true;
}
