/* Pharos - LUMEN: the design system
 *
 * Every screen on this device is built from the tokens and the five
 * components in this file. That is the whole point of it existing: twenty-one
 * lenses drawn by twenty-one hands look like twenty-one products, and the
 * operator has to relearn the glass each time they turn the dial.
 *
 * ---------------------------------------------------------------------------
 * WHY THE OLD FACE WAS REPLACED
 *
 * The face this replaces was an aircraft-instrument pastiche: a 24-tick bezel,
 * hairline arcs, and text down to 12 px. It photographed well. It did not read
 * well in a hand, which is the only place it is ever used, and three specific
 * things were wrong with it:
 *
 *   1. TYPE TOO SMALL. This panel is 466 px across a 1.75 inch circle - 266
 *      ppi. 12 px is about 1.1 mm of cap height. That is below what a person
 *      resolves at arm's length, and half the supporting text on the old face
 *      was set in it. LUMEN's smallest size is 14 and it is used for page
 *      counters and nothing else.
 *
 *   2. NO READING ORDER. Score, band, detail, why, advice and four pips were
 *      all on the glass at similar weight, so the eye had to search for the
 *      answer instead of landing on it. LUMEN fixes one order everywhere:
 *
 *          COLOUR -> WORD -> NUMBER -> EVIDENCE -> ACTION
 *
 *      You learn the verdict from the colour before you have read anything,
 *      the word confirms it, and the number is support - not the headline. An
 *      operator glancing at the device mid-corridor gets the answer from the
 *      first of those and can stop.
 *
 *   3. DECORATION THAT COST FRAMES. Those 24 bezel ticks carried no
 *      information and sat in the invalidation path of everything else. The
 *      device was dropping ~7% of frames (painted=257 missed=20 in the field
 *      log) while redrawing furniture. LUMEN draws nothing that does not mean
 *      something.
 *
 * ---------------------------------------------------------------------------
 * WHAT LUMEN MAY NOT CHANGE
 *
 * The four verdict colours. GOOD / WARN / HIGH / BAD are a contract with the
 * operator that outranks any visual refresh: a red on Census means what a red
 * on Watch means. They are reproduced here byte-identical to the face this
 * replaces, and test_style.c fails the build if they drift. Everything else -
 * chrome, spacing, type, motion - is ours to change.
 */
#ifndef PHAROS_STYLE_H
#define PHAROS_STYLE_H

#include <stdbool.h>
#include <stdint.h>

#include "pharos_round.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- the type scale -------------------------------------------------
 *
 * Six sizes, and a screen may use at most four of them. More than that and
 * the hierarchy stops being a hierarchy - everything is "sort of important".
 *
 * The bottom of the scale is 14, not 12. See note 1 above; this is the single
 * change that did most for legibility, and it is why 28/32/36 were added to
 * the build - the old scale jumped 26 -> 48 with nothing usable between, so
 * every verdict WORD had to be set at 26 to fit, which made words look
 * subordinate to numbers set at 48. In LUMEN the word leads.
 */
typedef enum {
    PS_TYPE_MICRO = 0, /* 14 - page counters. Nothing else.        */
    PS_TYPE_LABEL,     /* 16 - chips, units, secondary             */
    PS_TYPE_BODY,      /* 20 - anything meant to be READ           */
    PS_TYPE_TITLE,     /* 28 - lens names, screen titles           */
    PS_TYPE_HERO,      /* 36 - the verdict word                    */
    PS_TYPE_METRIC,    /* 48 - a bare number, when it IS the point */
    PS_TYPE_N,
} ps_type_t;

/* Point size of each step, for layout maths on the host. */
extern const int16_t PS_TYPE_PX[PS_TYPE_N];

/* Montserrat's caps are close enough to 0.60 em wide, averaged over mixed
 * case, that this is what the layout uses to decide whether a string fits.
 * It is deliberately a slight OVER-estimate: guessing narrow puts text
 * through the edge of the glass, guessing wide only wastes a few pixels.
 * (The old dial guessed 0.6 em against a real 73 px and wrote SENTINEL
 * through the middle of the face. Round up.) */
#define PS_EM_W(px) (((px) * 62 + 50) / 100)

/* Width in px of an n-character run at a type step. */
int16_t ps_text_w(ps_type_t t, unsigned chars);

/* The largest step at which `chars` characters fit on ONE line, centred at
 * vertical offset dy, inside radius r. Returns PS_TYPE_N when nothing fits -
 * which means SHORTEN THE STRING, never "draw it and hope". */
ps_type_t ps_fit(unsigned chars, int16_t dy, int16_t r);

/* How many characters fit at a step and offset. Use to truncate with care. */
unsigned ps_capacity(ps_type_t t, int16_t dy, int16_t r);

/* ---- the verdict palette --------------------------------------------
 *
 * FROZEN. See the contract note at the top. */
#define PS_GOOD  0x39DB84u /* nothing to do here          */
#define PS_WARN  0xFFC34Au /* worth knowing               */
#define PS_HIGH  0xEF9239u /* one real finding            */
#define PS_BAD   0xE75142u /* act on this                 */

/* The ground. AMOLED: an unlit pixel costs no power AND is a true black, so
 * the field is 0x000000 rather than a dark grey. Cards lift off it instead of
 * the field sinking behind them. */
#define PS_VOID  0x000000u

/* A verdict colour at aura strength - the same hue, mixed hard towards black,
 * for the glow behind the hero and the fill behind a card. Computed rather
 * than tabulated so a theme cannot get them out of step with the four above. */
uint32_t ps_tint(uint32_t rgb, uint8_t pct);

/* The verdict colour for a 0..100 threat score, and for an alert level. The
 * two exist separately because a score is not always a threat scale - see the
 * `alert` field on pharos_lens_display and the home ring shouting ALERT at a
 * neighbour's WPS. */
uint32_t ps_score_colour(int score);
uint32_t ps_alert_colour(uint8_t alert);

/* ---- the vertical rhythm --------------------------------------------
 *
 * Named bands, in dy from centre, so that a "context line" is in the same
 * place on the Watch face as on the Census face. Screens place content at
 * these offsets and nowhere else; that consistency is most of what makes
 * twenty-one lenses feel like one instrument.
 *
 * The bands are asymmetric on purpose. There is more room below the hero than
 * above it because the top of the circle carries the activity ribbon on every
 * live screen, and a ribbon that moves needs clear air above the text or the
 * eye reads the motion as the text flickering.
 */
#define PS_Y_RIBBON  (-150) /* activity over time                      */
/* THE DRILL BANNER NEEDS ITS OWN BAND.
 *
 * It was sitting at PS_Y_CONTEXT-30, which is inside the ribbon: a full-height
 * activity bar reaches -127 and the banner's top edge was -130. They touched
 * on the Footprint render. This banner is the one thing on the device that
 * must never be ambiguous - a training lens showing FLOOD LIKELY was once read
 * as a real attack on a real building - so it gets a reserved band, not the
 * gap under another one. */
#define PS_Y_SIM     (-116)
#define PS_Y_CONTEXT (-92)  /* channel, network, the lens name          */
#define PS_Y_HERO    (-18)  /* the verdict word. The centre of gravity */
#define PS_Y_METRIC  (30)   /* the number that supports it             */
#define PS_Y_DETAIL  (66)   /* supporting figures                      */
#define PS_Y_CHIPS   (104)  /* labelled evidence                       */
#define PS_Y_WHY     (140)  /* the ONE specific finding                */
#define PS_Y_ACTION  (170)  /* what to do...                           */
#define PS_Y_ACTION2 (192)  /* ...on two lines, because one was a lie  */

/* THE BOTTOM OF THE GAUGE IS EMPTY, SO PUT SOMETHING THERE.
 *
 * The score arc spans 270 degrees opening at the bottom, which leaves a clean
 * 90-degree notch at 6 o'clock that no reading ever occupies. The clock lived
 * at the TOP until a render showed it sitting on the watch dots - the ring is
 * crowded up there and empty down here. */
#define PS_Y_CLOCK   (192)
#define PS_Y_PAGE    (188)  /* the detail page counter                 */
#define PS_Y_TELL    (214)  /* the permanent receive-only pip          */

/* THE GUIDE'S OWN BANDS.
 *
 * The first draft put its body text at +132/+162 and its hint at +198, which
 * are fine offsets on a rectangle and hopeless on a circle: the chord at +198
 * is 210 px, about thirteen characters. Ten primitives escaped the glass.
 * These are the bands where the text actually fits, with the animation living
 * in the clear space between the title and the first line. */
#define PS_Y_GUIDE_PIPS  (-196)
#define PS_Y_GUIDE_TITLE (-152)
#define PS_Y_GUIDE_L1    (112)
#define PS_Y_GUIDE_L2    (142)
#define PS_Y_GUIDE_HINT  (176)

/* WHY ADVICE GETS TWO LINES.
 *
 * The display contract carries 96 characters of advice and the chord at
 * PS_Y_ACTION holds about 30. The first LUMEN render truncated a real engine
 * string to "Broad, spoofed deauth." and threw away "Preserve the log." -
 * which is the half that tells the operator what to DO. Losing the verb
 * defeats the entire point of the line. Two fixed labels (never a wrapping
 * one - see lesson 3 in pharos_hud.c) hold about 54 characters between them,
 * which covers every advice string the engines currently produce. */

/* ---- motion ----------------------------------------------------------
 *
 * Three durations and one easing, because motion on this device has exactly
 * one job: to show that a value CHANGED, so a glance that arrives late still
 * knows something happened. It is never decoration - a device held in a dark
 * corridor by somebody who does not want to be noticed should not be
 * animating for fun.
 *
 * Nothing loops forever except the breath, and the breath is 4 s and ±6% of
 * opacity, which is under the threshold at which peripheral vision reports
 * motion. It exists so a quiet screen still looks alive rather than crashed -
 * "is it frozen?" was a real question asked of the old face.
 */
#define PS_MS_FAST   180 /* a chip lighting, a row highlighting     */
#define PS_MS_MOVE   420 /* an arc travelling to a new value        */
#define PS_MS_PAGE   260 /* a page fading in over another           */
#define PS_MS_BREATH 4000

/* ---- component geometry ---------------------------------------------- */

#define PS_AURA_R    150 /* the glow behind the hero                */
#define PS_RING_R    206 /* the score arc's radius                  */
#define PS_RING_W    14  /* ...and its weight                       */

/* A card is the list row, and it is 58 px tall because a fingertip contact
 * patch is about 9 mm and this panel is 266 ppi. 36 px rows were legible and
 * untappable, which is exactly how "I can't click them correctly" happens.
 * Four of them, not six. That trade - fewer rows, reachable ones - is the
 * same call the face this replaces got right, and it is kept. */
#define PS_CARD_H     58
#define PS_CARD_GAP   8
#define PS_CARDS      4

/* Smallest thing a finger can reliably hit here. Anything CLICKABLE that is
 * smaller than this is a bug, and test_style.c is where that gets caught. */
#define PS_TOUCH_MIN  PR_TOUCH_MIN

/* Is this rect a legal touch target? */
bool ps_touchable(int16_t w, int16_t h);

/* Widest a chip may be when n of them share the width available at PS_Y_CHIPS.
 * Returns 0 when n chips cannot be drawn at a legible size, which means draw
 * fewer - the caller shows the strongest and drops the rest, the way an
 * instrument does not number every tick. */
int16_t ps_chip_w(unsigned n);

#ifdef __cplusplus
}
#endif

#endif /* PHAROS_STYLE_H */
