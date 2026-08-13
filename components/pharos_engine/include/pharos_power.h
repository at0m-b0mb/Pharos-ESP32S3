/* Pharos - the power budget planner
 *
 * Pure C. Answers the question the Lamp Room asks before you launch anything:
 * "how long will this lens run on what is left in the battery?"
 *
 * Every lens declares a worst-case draw in its descriptor. Combined with the
 * AXP2101's state of charge and the display mode, that is enough to predict
 * runtime - and predicting it *before* launch is the difference between a
 * field tool and a toy. Somebody about to leave a device recording overnight
 * needs the number in advance, not a low-battery warning at 3 a.m.
 *
 * The estimate is deliberately pessimistic. A prediction that runs out early
 * costs an evidence gap; one that runs long costs nothing.
 */
#ifndef PHAROS_POWER_H
#define PHAROS_POWER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PWR_SCREEN_ACTIVE = 0, /* full brightness, operator watching  */
    PWR_SCREEN_DIM,        /* readable, idle                      */
    PWR_SCREEN_OFF,        /* unattended: Sentry's normal state    */
} pwr_screen_t;

typedef struct {
    uint16_t capacity_mah; /* pack capacity as fitted            */
    uint8_t soc_pct;       /* AXP2101 state of charge            */
    uint16_t mv;           /* pack voltage                       */
    bool charging;
    bool present;
} pwr_battery_t;

/* Baseline draw of the board with the radio idle and the screen off, in mA.
 * Measured on hardware at M1; until then this is a documented estimate and
 * pwr_plan reports its results as such. */
#define PWR_BASE_MA 38

/* Screen contribution at each mode. AMOLED, so this depends on content:
 * these figures assume the Pharos dark HUD, which is mostly unlit pixels. */
#define PWR_SCREEN_ACTIVE_MA 62
#define PWR_SCREEN_DIM_MA    18
#define PWR_SCREEN_OFF_MA     0

typedef struct {
    uint16_t draw_ma;      /* total predicted draw                */
    uint32_t minutes;      /* predicted runtime at that draw      */
    bool estimated;        /* true until the baseline is measured */
    bool insufficient;     /* not enough charge to be useful      */
} pwr_plan_t;

/* Predict runtime for one lens at one screen mode. lens_ma is the lens
 * descriptor's budget_ma. */
void pwr_plan(const pwr_battery_t *b, uint16_t lens_ma, pwr_screen_t screen,
              pwr_plan_t *out);

/* The longest-running screen mode that still meets a target runtime, or
 * PWR_SCREEN_OFF if even that will not reach it. */
pwr_screen_t pwr_best_screen(const pwr_battery_t *b, uint16_t lens_ma,
                             uint32_t target_minutes);

/* "4h 20m" into buf. Returns buf. */
char *pwr_format(uint32_t minutes, char *buf, unsigned buflen);

#ifdef __cplusplus
}
#endif

#endif /* PHAROS_POWER_H */
