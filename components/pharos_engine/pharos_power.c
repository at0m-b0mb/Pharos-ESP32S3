#include "pharos_power.h"

#include <string.h>

static uint16_t screen_ma(pwr_screen_t s)
{
    switch (s) {
    case PWR_SCREEN_ACTIVE: return PWR_SCREEN_ACTIVE_MA;
    case PWR_SCREEN_DIM:    return PWR_SCREEN_DIM_MA;
    case PWR_SCREEN_OFF:
    default:                return PWR_SCREEN_OFF_MA;
    }
}

void pwr_plan(const pwr_battery_t *b, uint16_t lens_ma, pwr_screen_t screen,
              pwr_plan_t *out)
{
    if (!out) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->estimated = true; /* until PWR_BASE_MA is measured at M1 */

    const uint32_t draw = (uint32_t)PWR_BASE_MA + lens_ma + screen_ma(screen);
    out->draw_ma = (uint16_t)(draw > 0xFFFFu ? 0xFFFFu : draw);

    if (!b || !b->present || b->capacity_mah == 0 || draw == 0) {
        out->insufficient = true;
        return;
    }

    /* Usable charge, not nameplate charge. Two haircuts, both deliberate:
     * the pack is not run to zero (the PMU cuts out first, and a lithium cell
     * that is regularly emptied does not last), and a real discharge under a
     * varying load never reaches the datasheet figure. A prediction that runs
     * out early costs an evidence gap; one that runs long costs nothing. */
    const uint32_t soc = (b->soc_pct > 100) ? 100u : b->soc_pct;
    const uint32_t nominal_mah = ((uint32_t)b->capacity_mah * soc) / 100u;
    const uint32_t usable_mah = (nominal_mah * 85u) / 100u;

    out->minutes = (usable_mah * 60u) / draw;
    if (out->minutes < 5) {
        out->insufficient = true;
    }
}

pwr_screen_t pwr_best_screen(const pwr_battery_t *b, uint16_t lens_ma,
                             uint32_t target_minutes)
{
    /* Brightest first: give the operator the most usable screen that still
     * reaches the target, rather than defaulting to the dimmest. */
    static const pwr_screen_t order[] = {
        PWR_SCREEN_ACTIVE, PWR_SCREEN_DIM, PWR_SCREEN_OFF
    };
    for (unsigned i = 0; i < 3; i++) {
        pwr_plan_t p;
        pwr_plan(b, lens_ma, order[i], &p);
        if (!p.insufficient && p.minutes >= target_minutes) {
            return order[i];
        }
    }
    return PWR_SCREEN_OFF;
}

char *pwr_format(uint32_t minutes, char *buf, unsigned buflen)
{
    if (!buf || buflen == 0) {
        return buf;
    }
    const uint32_t h = minutes / 60u;
    const uint32_t m = minutes % 60u;
    unsigned n = 0;

    if (h > 0) {
        /* Hours, up to three digits: a Sentry session on a big pack can run
         * for days, and "999h" is more useful than a wrapped number. */
        uint32_t hh = (h > 999u) ? 999u : h;
        char tmp[4];
        unsigned t = 0;
        if (hh == 0) tmp[t++] = '0';
        while (hh && t < sizeof(tmp)) { tmp[t++] = (char)('0' + hh % 10u); hh /= 10u; }
        while (t && n + 1 < buflen) buf[n++] = tmp[--t];
        if (n + 1 < buflen) buf[n++] = 'h';
        if (n + 1 < buflen) buf[n++] = ' ';
    }
    if (n + 1 < buflen) buf[n++] = (char)('0' + (m / 10u));
    if (n + 1 < buflen) buf[n++] = (char)('0' + (m % 10u));
    if (n + 1 < buflen) buf[n++] = 'm';
    buf[n < buflen ? n : buflen - 1] = '\0';
    return buf;
}
