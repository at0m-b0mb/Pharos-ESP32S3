/* Virtual Pharos - the round HUD, rendered on a laptop from the real code.
 *
 * This is not a mock-up. Every coordinate below comes out of the same
 * pharos_round / pharos_dial geometry the firmware uses, and every number
 * comes out of the same detection engines, fed by the same pharos_range
 * scenarios the training lens plays. If a label would be clipped by the
 * curve on the device, it is clipped here; if the Watch engine refuses to
 * alarm while hopping, this picture shows it refusing.
 *
 * It emits a display list - one primitive per line - which tools/render/
 * rasterize.py turns into a PNG with antialiasing and a real font. Splitting
 * it that way keeps the C side pure geometry (no rasteriser to get wrong) and
 * gives a text artefact that can be diffed and bounds-checked.
 *
 * The bounds check is the point: --check verifies that no primitive escapes
 * the panel's safe radius, so "does the UI fit on a circle" is a test result
 * rather than an opinion. See test/host/test_render.c.
 *
 *   make -C tools/render        # writes PNGs into assets/screens
 */
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "pharos_census.h"
#include "pharos_console.h"
#include "pharos_dial.h"
#include "pharos_flood.h"
#include "pharos_karma.h"
#include "pharos_locate.h"
#include "pharos_opsec.h"
#include "pharos_range.h"
#include "pharos_round.h"
#include "pharos_style.h"
#include "pharos_lens.h"
#include "pharos_twin.h"
#include "pharos_watch.h"

/* ---- palette ---------------------------------------------------------
 * Dark-first, because the panel is AMOLED: an unlit pixel costs no power, so
 * the HUD is mostly black with a single accent carrying the state.
 *
 * Every value is snapped to a colour RGB565 can represent EXACTLY, because the
 * device is driven at 16-bit and anything else is silently requantised there -
 * per channel, by different amounts. Keeping these identical to the firmware's
 * palette (pharos_ui/pharos_hud.c) is what makes the gallery images and the
 * panel the same product rather than two approximations of one. */
#define C_VOID    "#000000"
#define C_FIELD   "#000000"  /* true black: see the note in pharos_hud.c */
#define C_FIELD2  "#18384A"
#define C_RIM     "#2A6B80"
#define C_CYAN    "#21B6C6"
#define C_CYAN_HI "#7BEBF7"
#define C_AMBER   "#FFC34A"
#define C_ORANGE  "#EF9239"
#define C_RED     "#E75142"
#define C_GREEN   "#39DB84"
#define C_TEXT    "#E7F7F7"
#define C_DIM     "#94BECC"
#define C_DIMMER  "#6693A6"
#define C_DENIED  "#3A5A6B"

/* Defined in the LUMEN section below. Forward-declared because the
 * engine-driven screens above are all just LIVE pages with different data -
 * which is exactly how the firmware draws them too. */
static void screen_lumen_detail_named(const char *name, const char *title,
                                      const char *hl, const char *hr);
static void screen_lumen_live(const char *name, const char *lens,
                              const char *word, int score, int ceiling,
                              const char *detail, const char *why,
                              const char *action,
                              const char *const *fam, uint8_t lit,
                              const uint8_t *hist, bool simulated,
                              uint32_t rgb_override);

/* ---- display list ---------------------------------------------------- */

static FILE *g_out;
static int g_violations;
static const char *g_screen = "";

/* Every primitive is bounds-checked as it is emitted. A drawing that escapes
 * the glass is a bug we want to hear about at generation time, not discover
 * on hardware. */
static void bound(float x, float y, const char *what)
{
    const float dx = x - (float)PR_CX, dy = y - (float)PR_CY;
    const float r = dx * dx + dy * dy;
    const float lim = (float)PR_SAFE_R * (float)PR_SAFE_R;
    if (r > lim) {
        g_violations++;
        fprintf(stderr, "  BOUNDS: %s/%s at (%.0f,%.0f) is outside safe radius %d\n",
                g_screen, what, (double)x, (double)y, PR_SAFE_R);
    }
}

static void emit(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(g_out, fmt, ap);
    va_end(ap);
    fputc('\n', g_out);
}

static void screen(const char *name)
{
    g_screen = name;
    emit("SCREEN %s %d %d", name, PR_W, PR_H);
}

static void disc(int cx, int cy, int r, const char *col)
{
    emit("DISC %d %d %d %s", cx, cy, r, col);
}

static void ring(int cx, int cy, int r, int w, const char *col)
{
    emit("RING %d %d %d %d %s", cx, cy, r, w, col);
}

static void arc(int cx, int cy, int r, int w, float a0, float sweep, const char *col)
{
    if (sweep <= 0.05f) return;
    emit("ARC %d %d %d %d %.3f %.3f %s", cx, cy, r, w, (double)a0, (double)sweep, col);
}

static void line(int x1, int y1, int x2, int y2, int w, const char *col)
{
    bound((float)x1, (float)y1, "line");
    bound((float)x2, (float)y2, "line");
    emit("LINE %d %d %d %d %d %s", x1, y1, x2, y2, w, col);
}

static void dot(int cx, int cy, int r, const char *col)
{
    bound((float)cx, (float)cy, "dot");
    emit("DOT %d %d %d %s", cx, cy, r, col);
}

/* All four corners are checked. A rectangle on a round panel is the easiest
 * thing in the world to get wrong: the widest chord is at the corner furthest
 * from centre, not at the row's midline, so a card sized from its middle will
 * quietly lose its top corners to the glass. */
static void roundrect(int x, int y, int w, int h, int r, const char *col)
{
    bound((float)x, (float)y, "roundrect");
    bound((float)(x + w), (float)y, "roundrect");
    bound((float)x, (float)(y + h), "roundrect");
    bound((float)(x + w), (float)(y + h), "roundrect");
    emit("ROUNDRECT %d %d %d %d %d %s", x, y, w, h, r, col);
}

/* Widest a centred card of height h may be with its top at `top`, so that
 * every corner stays inside radius rad. */
static int card_width(int top, int h, int rad)
{
    const int dy_top = top - PR_CY;
    const int dy_bot = top + h - PR_CY;
    const int worst = (dy_top < 0 ? -dy_top : dy_top) > (dy_bot < 0 ? -dy_bot : dy_bot)
                          ? (dy_top < 0 ? -dy_top : dy_top)
                          : (dy_bot < 0 ? -dy_bot : dy_bot);
    return pr_chord_halfwidth((int16_t)rad, (int16_t)worst) * 2;
}

/* anchor: 'c' centre, 'l' left, 'r' right */
static void text(int x, int y, int size, char anchor, const char *col, const char *s)
{
    /* Check the ends of the run, using the same advance the firmware assumes. */
    const int w = (int)((long)size * 3 * (long)strlen(s) / 5);
    int x0 = x, x1 = x;
    if (anchor == 'c') { x0 = x - w / 2; x1 = x + w / 2; }
    else if (anchor == 'l') { x1 = x + w; }
    else { x0 = x - w; }
    bound((float)x0, (float)y, "text");
    bound((float)x1, (float)y, "text");
    emit("TEXT %d %d %d %c %s %s", x, y, size, anchor, col, s);
}

/* ---- glow / gradient variants ---------------------------------------
 * These emit the same bounds-checked geometry, but tag it to also bloom (the
 * rasteriser blurs and screens the glow layer) or, for arcs, to fill with a
 * colour gradient. This is the whole of the "nicer" finish - the layout is
 * unchanged and still bounds-checked. */

static void glowtext(int x, int y, int size, char anchor, const char *col, const char *s)
{
    const int w = (int)((long)size * 3 * (long)strlen(s) / 5);
    int x0 = x, x1 = x;
    if (anchor == 'c') { x0 = x - w / 2; x1 = x + w / 2; }
    else if (anchor == 'l') { x1 = x + w; }
    else { x0 = x - w; }
    bound((float)x0, (float)y, "glowtext");
    bound((float)x1, (float)y, "glowtext");
    emit("GLOWTEXT %d %d %d %c %s %s", x, y, size, anchor, col, s);
}

static void glowdot(int cx, int cy, int r, const char *col)
{
    bound((float)cx, (float)cy, "glowdot");
    emit("GLOWDOT %d %d %d %s", cx, cy, r, col);
}

static void glowline(int x1, int y1, int x2, int y2, int w, const char *col)
{
    bound((float)x1, (float)y1, "glowline");
    bound((float)x2, (float)y2, "glowline");
    emit("GLOWLINE %d %d %d %d %d %s", x1, y1, x2, y2, w, col);
}

static void glowarc(int cx, int cy, int r, int w, float a0, float sweep, const char *col)
{
    if (sweep <= 0.05f) return;
    emit("GLOWARC %d %d %d %d %.3f %.3f %s", cx, cy, r, w, (double)a0, (double)sweep, col);
}

/* Gradient arc: fills from col0 at the start to col1 at the end of the sweep. */
static void garc(int cx, int cy, int r, int w, float a0, float sweep,
                 const char *col0, const char *col1)
{
    if (sweep <= 0.05f) return;
    emit("GARC %d %d %d %d %.3f %.3f %s %s", cx, cy, r, w, (double)a0, (double)sweep,
         col0, col1);
}

/* Largest type size at which a run of `nchars` centred on (px,py) keeps BOTH
 * ends inside radius r.
 *
 * This is not the same question pd_label_size answers, and the difference
 * caught a real bug here. pd_label_size assumes the run is centred on the
 * screen's vertical axis, so it only needs the chord at that height - correct
 * for the core readouts, wrong for anything placed off-axis like a dial
 * label, where the run's far end is radially further out than its centre. Off
 * -axis text has to be measured against the circle itself. */
static int fit_text_at(int px, int py, unsigned nchars, int r, int max_size)
{
    for (unsigned i = 0; i < 4; i++) {
        const int size = PD_TYPE_SCALE[i];
        if (size > max_size) {
            continue; /* caller has a design size in mind; do not inflate it */
        }
        const int w = (int)(((long)size * 3 * (long)nchars) / 5);
        const int xs[2] = { px - w / 2, px + w / 2 };
        bool ok = true;
        for (unsigned k = 0; k < 2; k++) {
            const long dx = xs[k] - PR_CX, dy = py - PR_CY;
            if (dx * dx + dy * dy > (long)r * r) { ok = false; break; }
        }
        if (ok) {
            return size;
        }
    }
    return 0;
}

/* The old wrap() lived here. LUMEN never wraps - a label whose height
 * changes with its text re-lays-out and drags the invalidated region around
 * under the arc. Long strings are truncated against the real chord instead. */


/* ---- shared chrome --------------------------------------------------- */

static void panel_base(void)
{
    /* Black disc, lit rim. On the AMOLED the disc emits nothing at all - see
     * the long note on HUD_FIELD in pharos_ui/pharos_hud.c for why a "lifted"
     * instrument face is the wrong instinct on this panel. */
    disc(PR_CX, PR_CY, PR_R, C_VOID);
    disc(PR_CX, PR_CY, PR_R - 2, C_FIELD);
    ring(PR_CX, PR_CY, PR_RIM_R - 4, 2, C_RIM);
}

/* 24 rim ticks, the instrument's bezel. */
static void rim_ticks(void)
{
    for (int i = 0; i < 24; i++) {
        const float a = (float)i * 15.0f;
        const int len = (i % 6 == 0) ? 12 : 7;
        pr_point_t o = pr_polar((int16_t)(PR_SAFE_R - 2), a);
        pr_point_t in = pr_polar((int16_t)(PR_SAFE_R - 2 - len), a);
        line(o.x, o.y, in.x, in.y, (i % 6 == 0) ? 3 : 2,
             (i % 6 == 0) ? C_CYAN : C_RIM);
    }
}

/* Battery / posture arc on the rim, plus the permanent receive-only dot.
 * The band arc glows so the device's current state reads at a glance from the
 * bezel alone; the green dot is the always-on "receive-only" tell. */
static void rim_status(int battery_pct, const char *band_col)
{
    arc(PR_CX, PR_CY, PR_SAFE_R - 12, 4, 200.0f,
        160.0f * (float)battery_pct / 100.0f, C_DIMMER);
    glowarc(PR_CX, PR_CY, PR_SAFE_R - 12, 4, 20.0f, 60.0f, band_col);
    glowdot(PR_CX, PR_CY + PR_RIM_R - 40, 4, C_GREEN);
}

static const char *band_colour(int score)
{
    if (score >= 75) return C_RED;
    if (score >= 60) return C_ORANGE;
    if (score >= 40) return C_AMBER;
    if (score >= 20) return C_CYAN;
    return C_DIM;
}

/* ---- screen: the Lamp Room dial -------------------------------------- */

typedef struct { const char *name; const char *kind; } dial_item_t;

__attribute__((unused)) static void screen_lamp_room(void)
{
    static const dial_item_t items[] = {
        { "SPECTRUM", "observe" }, { "WATCH", "observe" }, { "CENSUS", "observe" },
        { "TWIN", "observe" },     { "KARMA", "observe" }, { "MIRAGE", "observe" },
        { "PROBE", "observe" },    { "RANGE", "train" },   { "FOOTPRINT", "train" },
        { "SYSTEM", "system" },
    };
    const unsigned n = sizeof(items) / sizeof(items[0]);

    screen("lamp_room");
    panel_base();
    rim_ticks();

    pd_dial_t dial;
    pd_dial_layout(n, 0.0f, PR_RING_R - 34, PR_SAFE_R - 16, &dial);

    /* Wedges, drawn from the real layout. */
    for (unsigned i = 0; i < n; i++) {
        const float centre = pd_dial_item_angle(&dial, i);
        const float a0 = centre - dial.wedge_deg * 0.5f;
        const bool selected = (i == 1); /* Watch under the selector */
        emit("WEDGE %d %d %d %d %.3f %.3f %s", PR_CX, PR_CY,
             dial.r_inner, dial.r_outer, (double)a0, (double)dial.wedge_deg,
             selected ? C_FIELD2 : C_VOID);
        arc(PR_CX, PR_CY, (dial.r_inner + dial.r_outer) / 2, 2, a0, dial.wedge_deg,
            selected ? C_CYAN_HI : C_RIM);

        /* Label sits on the wedge's mid-radius - off-axis, so it is measured
         * against the circle rather than the chord (see fit_text_at). If it
         * still will not fit, the label is shortened rather than clipped. */
        pr_point_t p = pr_polar((int16_t)((dial.r_inner + dial.r_outer) / 2 + 2), centre);
        char label[16];
        snprintf(label, sizeof(label), "%s", items[i].name);
        int size = fit_text_at(p.x, p.y, (unsigned)strlen(label), PR_SAFE_R - 6, 20);
        while (size == 0 && strlen(label) > 3) {
            label[strlen(label) - 1] = '\0';
            size = fit_text_at(p.x, p.y, (unsigned)strlen(label), PR_SAFE_R - 6, 20);
        }
        text(p.x, p.y, size ? size : 14, 'c',
             selected ? C_CYAN_HI : C_DIM, label);
    }

    /* Selector notch at 12 o'clock. */
    pr_point_t s0 = pr_polar((int16_t)(dial.r_outer + 4), 0.0f);
    pr_point_t s1 = pr_polar((int16_t)(dial.r_outer - 6), 0.0f);
    line(s0.x, s0.y, s1.x, s1.y, 3, C_CYAN_HI);

    /* Core: the selected lens, its declared powers, and predicted runtime. */
    disc(PR_CX, PR_CY, PR_CORE_R + 8, C_VOID);
    ring(PR_CX, PR_CY, PR_CORE_R + 8, 1, C_RIM);
    text(PR_CX, PR_CY - 40, 20, 'c', C_DIMMER, "LAMP ROOM");
    text(PR_CX, PR_CY - 6, 34, 'c', C_TEXT, "Watch");
    text(PR_CX, PR_CY + 24, 14, 'c', C_DIM, "wifi.rx chan store");
    text(PR_CX, PR_CY + 46, 14, 'c', C_GREEN, "2h 41m @ 135mA");

    /* No lens-count caption: it landed on the KARMA wedge, and the dial
     * already shows the count by being a dial. */
    rim_status(78, C_CYAN);
}

/* ---- screen: the Watch gauge -----------------------------------------
 *
 * The redesign, and the reasoning behind each decision:
 *
 *   The old screen showed a big number, a band word, two dense monospace stat
 *   lines and three anonymous dots. The number was fine. Everything else asked
 *   the operator to do work: the dots said "three families" without saying
 *   WHICH, the stat lines packed four unrelated quantities into one string,
 *   and the advice sentence was word-wrapped into the gauge ring, where it
 *   collided with the arc at exactly the moment it mattered most.
 *
 *   So: one ring answers "how bad", one ribbon answers "what shape, over
 *   time", four labelled pips answer "on what evidence", and one unwrapped
 *   line answers "what do I do". Nothing wraps. Nothing overlaps. Every
 *   element has one job.
 */

/* The one-line fallback now comes from the ENGINE (pw_band_hint), not from a
 * copy kept here. The renderer used to carry its own shorter phrasing, which
 * meant the documentation screenshots and the device could drift apart - and
 * the device's own strings were the ones being clipped on the glass. One
 * source, one wording, one length budget, asserted by the host tests. */

/* Sixteen radial bars around the top of the dial: one per second of the
 * engine's own window, height proportional to that second's disconnect count.
 *
 * This is the single most useful thing a round screen can carry here. A steady
 * trickle and one violent burst produce the same ten-second average and are
 * not the same event; the mean cannot tell them apart and this shows it at a
 * glance. The scale is per-screen (tallest bar is full height) with the peak
 * printed, because the shape is the point and the absolute number is already
 * in the stats line. */
static void watch_history(const uint16_t *hist, unsigned n, const char *col)
{
    uint16_t peak = 0;
    for (unsigned i = 0; i < n; i++) {
        if (hist[i] > peak) peak = hist[i];
    }

    /* r_base + r_max must stay inside PR_SAFE_R (224) - the bounds check in
     * this file is what caught the first attempt at 196+30 poking through the
     * glass, which is exactly what it is for. */
    const int r_base = 184;   /* inner end of every bar   */
    const int r_max  = 34;    /* tallest a bar may grow   */
    const float span = 140.0f; /* degrees of arc the ribbon occupies */
    const float a0 = -span / 2.0f;

    /* A baseline under the bars, so the ribbon reads as a timeline with quiet
     * stretches rather than as a handful of stray marks. Without it, sixteen
     * seconds of silence followed by a burst looks like a rendering fault
     * instead of like the most informative thing on the screen. */
    arc(PR_CX, PR_CY, r_base - 4, 2, a0, span, "#16303F");

    for (unsigned i = 0; i < n; i++) {
        const float a = a0 + span * ((float)i + 0.5f) / (float)n;
        const int h = peak ? (int)(((long)hist[i] * r_max) / peak) : 0;
        pr_point_t p0 = pr_polar((int16_t)r_base, a);
        /* An empty second still draws a 2 px stub, so the ribbon reads as a
         * timeline with gaps rather than as a shorter timeline. */
        pr_point_t p1 = pr_polar((int16_t)(r_base + (h > 5 ? h : 5)), a);
        if (h > 0) {
            glowline(p0.x, p0.y, p1.x, p1.y, 7, col);
        } else {
            line(p0.x, p0.y, p1.x, p1.y, 7, "#16303F");
        }
    }
}

/* The four evidence families, named. A lit pip means that family met its own
 * threshold and is contributing; a dark one means it is not. Naming them is
 * the entire improvement over three unlabelled dots: "why does it think so"
 * is answerable from the glass instead of from the manual. */
static void watch_families(const pw_verdict_t *v, const char *col)
{
    static const struct { uint8_t bit; const char *label; } fam[4] = {
        { PW_FAM_RATE,      "RATE"  },
        { PW_FAM_SHAPE,     "SHAPE" },
        { PW_FAM_FORGERY,   "FORGE" },
        { PW_FAM_AFTERMATH, "AFTER" },
    };
    const int w = 80, gap = 6, h = 30;
    const int total = 4 * w + 3 * gap;
    const int top = PR_CY + 103;

    for (unsigned i = 0; i < 4; i++) {
        const int x = PR_CX - total / 2 + (int)i * (w + gap);
        const bool lit = (v->families & fam[i].bit) != 0;
        roundrect(x, top, w, h, 8, lit ? "#1B4257" : "#0A1620");
        text(x + w / 2, top + h / 2 + 6, 16, 'c', lit ? col : C_DENIED,
             fam[i].label);
    }
}

/* Renders one Watch verdict. The camped/hopping pair is generated from the
 * *identical* event stream, which is the whole argument of the project made
 * visible: same evidence, different entitlement to claim it. */
static void screen_watch(const char *name, const pw_verdict_t *v,
                         const uint16_t *hist, unsigned nhist,
                         const char *mode_short, int dwell_pct)
{
    screen(name);
    panel_base();
    rim_ticks();

    const char *col = band_colour(v->score);

    /* --- the activity ribbon, on the rim where it does not crowd anything -- */
    watch_history(hist, nhist, col);

    /* --- the score arc: 270 degrees starting at 7 o'clock ---------------- */
    const float A0 = 225.0f, SWEEP = 270.0f;
    const int R = PR_RING_R - 14;

    arc(PR_CX, PR_CY, R, 22, A0, SWEEP, "#0A1C26");
    arc(PR_CX, PR_CY, R, 18, A0, SWEEP, "#18384A");
    /* One colour, fading up out of the track. The old cyan-to-band gradient
     * read as if the first half of the score were somehow benign; it is one
     * number and it should look like one. */
    garc(PR_CX, PR_CY, R, 18, A0, SWEEP * (float)v->score / 100.0f, "#1B3A4E", col);

    /* What the caps took away, as a thin inner trace. The old screen drew
     * this as a second fat arc behind the score, where it read as a bug -
     * two bars of the same weight saying different numbers. Thin, inset and
     * dim, it reads as what it is: the score this evidence would have earned
     * if the receiver had been entitled to claim it. */
    if (v->raw_score > v->score) {
        arc(PR_CX, PR_CY, R - 22, 4, A0, SWEEP * (float)v->raw_score / 100.0f,
            "#2A4257");
    }

    /* The ceiling: a hard stop the score may not pass, drawn as a tick across
     * the arc rather than as a competing band of colour. */
    {
        const float a = A0 + SWEEP * (float)v->ceiling / 100.0f;
        pr_point_t t0 = pr_polar((int16_t)(R + 14), a);
        pr_point_t t1 = pr_polar((int16_t)(R - 14), a);
        glowline(t0.x, t0.y, t1.x, t1.y, 3, C_RED);
    }

    /* --- the core: the reading ------------------------------------------- */
    disc(PR_CX, PR_CY, PR_CORE_R + 6, C_VOID);
    ring(PR_CX, PR_CY, PR_CORE_R + 6, 1, "#12283A");

    /* Context first, in small type: what is under pressure, where, and how
     * hard this receiver is actually looking. The posture used to live on the
     * top rim, which is where the activity ribbon now is - and a text run and
     * a bar chart sharing an arc is how you get a screen that looks broken. */
    {
        char buf[48];
        if (v->ssid[0]) {
            snprintf(buf, sizeof(buf), "%.14s", v->ssid);
        } else if (v->channel) {
            snprintf(buf, sizeof(buf), "CH %u  %s", v->channel, mode_short);
        } else {
            snprintf(buf, sizeof(buf), "%s", mode_short);
        }
        text(PR_CX, PR_CY - 54, 16, 'c', C_DIMMER, buf);
    }
    {
        char buf[16];
        snprintf(buf, sizeof(buf), "%u", v->score);
        glowtext(PR_CX, PR_CY - 4, 60, 'c', col, buf);
    }
    /* The band word is what a person actually reads, so it gets the widest
     * type the core can hold without touching the ring. */
    {
        const char *word = pw_band_name(v->band);
        const int size = fit_text_at(PR_CX, PR_CY + 46, (unsigned)strlen(word),
                                     PR_CORE_R + 2, 22);
        text(PR_CX, PR_CY + 46, size ? size : 16, 'c', col, word);
    }

    /* --- the supporting numbers, one line, no packing -------------------- */
    /* Three quantities, three separators, no packing. Sized and placed to
     * clear the score arc's inner edge: at this height the arc occupies
     * |x| >= 131, so the run may not be wider than 262. */
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "%u seen  -  %u/s  -  ceil %u", v->observed,
                 v->est_per_s_x100 / 100, v->ceiling);
        text(PR_CX, PR_CY + 90, 16, 'c', C_DIM, buf);
    }

    /* --- the evidence ---------------------------------------------------- */
    watch_families(v, col);

    /* --- one line of what to do, never wrapped --------------------------- */
    {
        /* Prefer the specific finding over the generic advice: "sequence
         * counter went backwards" tells an operator something that
         * "the shape looks wrong" does not. */
        const char *why = pw_forgery_name(v->forgery);
        const char *line_s = why ? why : pw_band_hint(v->band);
        const unsigned cap = pd_label_capacity(16, 152, PR_SAFE_R - 8);
        char buf[64];
        snprintf(buf, sizeof(buf), "%.*s", (int)(cap < sizeof(buf) - 1 ? cap
                                                                       : sizeof(buf) - 1),
                 line_s);
        text(PR_CX, PR_CY + 152, 16, 'c', why ? col : C_DIM, buf);
    }

    /* The ribbon's own scale, printed once where it cannot be mistaken for a
     * reading: this is what the tallest bar means. */
    rim_status(74, col);
}


/* ---- screen: Census list --------------------------------------------- */

typedef struct { const char *ssid; const char *grade; int score; const char *why; } row_t;

static void screen_census(void)
{
    screen_lumen_detail_named("census", "CENSUS", "worst first", "2.4 GHz");
}

/* ---- screen: Karma --------------------------------------------------- */

static void screen_karma(const pk_verdict_t *v)
{
    static const char *fam[4] = { "ANSWER", "SILENT", "SPREAD", "GAP" };
    static const uint8_t h[PHAROS_DISP_HISTORY] = {
        12,20,40,80,140,190,220,240,230,210,180,150,120,90,60,40 };
    screen_lumen_live("karma", "KARMA WATCH", pk_band_name(v->band),
                      v->score, v->ceiling,
                      "5 names, 5 never announced",
                      "one radio answers to names it never announces",
                      "Rogue AP. Do not join. Note the BSSID.",
                      fam, 0x0Bu, h, false, 0);
}

/* ---- screen: Spectrum ------------------------------------------------ */

static void screen_spectrum(void)
{
    static const char *fam[4] = { NULL, NULL, NULL, NULL };
    static const uint8_t h[PHAROS_DISP_HISTORY] = {
        60,90,140,190,230,255,240,200,160,120,150,190,220,180,130,90 };
    screen_lumen_live("spectrum", "SPECTRUM", "BUSY", 44, 0,
                      "ch1 ch6 ch11 carry the room",
                      "airtime is not an attack by itself",
                      "Read the other watches against this.",
                      fam, 0x00u, h, false, 0x21B6C6u);
}

/* ---- driving the real engines ---------------------------------------- */

/* Play the range's deauth-flood scenario into the real Watch engine, then
 * grade the identical stream twice - camped and hopping. */
__attribute__((unused)) static void watch_pair(void)
{
    pr_range_t r;
    pr_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.scenario = PR_SCENARIO_DEAUTH_FLOOD;
    cfg.seed = 0xBEEF;
    cfg.intensity = 800;
    pr_range_init(&r, &cfg);

    pw_engine_t eng;
    pw_reset(&eng);
    pharos_event_t ev;
    uint64_t last = 0;
    while (pr_range_next(&r, &ev)) {
        pw_observe(&eng, &ev.u.dot11, ev.t_us);
        last = ev.t_us;
    }

    pw_context_t camped = { .dwell_permil = 1000, .bus_yield_permil = 1000,
                            .window_ms = 12000 };
    pw_context_t hopping = { .dwell_permil = 71, .bus_yield_permil = 1000,
                             .window_ms = 12000 };
    pw_verdict_t vc, vh;
    pw_evaluate(&eng, last, &camped, &vc);
    pw_evaluate(&eng, last, &hopping, &vh);

    uint16_t hist[PW_WINDOW_SLOTS];
    pw_history(&eng, last, hist);

    screen_watch("watch_camped", &vc, hist, PW_WINDOW_SLOTS, "CAMPED", 100);
    screen_watch("watch_hopping", &vh, hist, PW_WINDOW_SLOTS, "HOPPING", 8);

    fprintf(stderr, "  watch: camped=%u/%u %s   hopping=%u/%u %s\n",
            vc.score, vc.ceiling, pw_band_name(vc.band),
            vh.score, vh.ceiling, pw_band_name(vh.band));

    /* The companion: the same kind of attack against a network that requires
     * protected management frames, graded from a HOPPING receiver. The
     * contradiction is not weakened by the short visit, so this one is
     * entitled to alarm where the flood above is not. */
    {
        pr_range_t r2;
        pr_config_t c2;
        memset(&c2, 0, sizeof(c2));
        c2.scenario = PR_SCENARIO_DEAUTH_PROVEN;
        c2.seed = 0xBEEF;
        c2.intensity = 800;
        pr_range_init(&r2, &c2);

        pw_engine_t e2;
        pw_reset(&e2);
        pharos_event_t ev2;
        uint64_t last2 = 0;
        while (pr_range_next(&r2, &ev2)) {
            pw_observe(&e2, &ev2.u.dot11, ev2.t_us);
            last2 = ev2.t_us;
        }
        pw_verdict_t vp;
        pw_evaluate(&e2, last2, &hopping, &vp);
        uint16_t h2[PW_WINDOW_SLOTS];
        pw_history(&e2, last2, h2);
        screen_watch("watch_proven", &vp, h2, PW_WINDOW_SLOTS, "HOPPING", 8);
        fprintf(stderr, "  watch: proven(hopping)=%u/%u %s  forgery=0x%02x\n",
                vp.score, vp.ceiling, pw_band_name(vp.band), vp.forgery);
    }
}

static void karma_screen(void)
{
    pk_engine_t e;
    pk_reset(&e);
    static const uint8_t rogue[6] = { 0x02, 0x66, 0x6E, 0x00, 0x00, 0x02 };
    static const uint8_t honest[6] = { 0xAC, 0x11, 0x22, 0x00, 0x00, 0x01 };
    pk_observe_beacon(&e, honest, "Acme-Staff", 10, -50, 6, 1000);
    static const char *const asked[] = {
        "HomeNet_5G", "Starbucks WiFi", "Heathrow_Free", "TheRobinsons", "eduroam"
    };
    for (unsigned i = 0; i < 5; i++) {
        const uint8_t len = (uint8_t)strlen(asked[i]);
        const uint64_t t = 2000000ull + i * 200000ull;
        pk_observe_probe(&e, asked[i], len, t);
        pk_observe_response(&e, rogue, asked[i], len, -40, 6, t + 100000);
    }
    pk_context_t camped = { .dwell_permil = 1000, .bus_yield_permil = 1000 };
    pk_verdict_t v;
    pk_evaluate(&e, &camped, &v);
    screen_karma(&v);
    fprintf(stderr, "  karma: %u/%u %s\n", v.score, v.ceiling, pk_band_name(v.band));
}

/* ---- screen: Mirage (beacon flood) ----------------------------------- */

static void screen_mirage(const pf_verdict_t *v)
{
    static const char *fam[4] = { "VOLUME", "EPHEM", "SYNTH", "SPREAD" };
    static const uint8_t h[PHAROS_DISP_HISTORY] = {
        20,60,120,200,255,255,240,255,250,255,230,210,190,160,130,100 };
    screen_lumen_live("mirage", "BEACON FLOOD", pf_band_name(v->band),
                      v->score, v->ceiling,
                      "128 names   3010/min",
                      "every name from one software radio",
                      "Fake networks. Your phone list is junk.",
                      fam, 0x07u, h, false, 0);
}

static void mirage_screen(void)
{
    pf_engine_t e;
    pf_reset(&e);
    static const char *const words[] = { "Free", "Guest", "WiFi", "Net", "Fast",
                                         "Home", "Fibre", "5G", "Cafe", "Pub" };
    for (unsigned i = 0; i < 300; i++) {
        uint8_t b[6] = { 0x02, 0xAB, 0xCD, (uint8_t)(i >> 8), (uint8_t)i, 0x01 };
        char name[24];
        snprintf(name, sizeof(name), "%s_%s_%03u", words[i % 10], words[(i / 10) % 10], i);
        const uint64_t t = 5000000ull + i * 20000ull;
        pf_observe(&e, b, name, (uint8_t)strlen(name), t);
        pf_observe(&e, b, name, (uint8_t)strlen(name), t + 500);
    }
    pf_context_t camped = { .dwell_permil = 1000, .bus_yield_permil = 1000, .window_ms = 12000 };
    pf_verdict_t v;
    pf_evaluate(&e, &camped, &v);
    screen_mirage(&v);
    fprintf(stderr, "  mirage: %u/%u %s\n", v.score, v.ceiling, pf_band_name(v.band));
}

/* ---- screen: Footprint (OPSEC) --------------------------------------- */

static void screen_footprint(const po_report_t *r)
{
    static const char *fam[4] = { "RATE", "SHAPE", "FORGE", "AFTER" };
    static const uint8_t h[PHAROS_DISP_HISTORY] = {
        40,90,160,220,255,250,255,240,255,235,220,200,170,140,110,80 };
    char det[64];
    snprintf(det, sizeof det, "%u camped vs %u hopping",
             r->camped_score, r->hopping_score);
    /* A drill, not the room - and the banner says so on every frame. */
    screen_lumen_live("footprint", "FOOTPRINT", po_grade_name(r->grade),
                      r->camped_score, 0, det, r->tell_name,
                      "Loud when watched. A hopper still misses it.",
                      fam, 0x0Fu, h, true, 0);
}

static void footprint_screen(void)
{
    pr_range_t r;
    pr_config_t cfg;
    memset(&cfg, 0, sizeof(cfg));
    cfg.scenario = PR_SCENARIO_DEAUTH_FLOOD;
    cfg.seed = 0xF007;
    cfg.intensity = 800;
    pr_range_init(&r, &cfg);
    pw_engine_t eng;
    pw_reset(&eng);
    pharos_event_t ev;
    uint64_t last = 0;
    while (pr_range_next(&r, &ev)) {
        pw_observe(&eng, &ev.u.dot11, ev.t_us);
        last = ev.t_us;
    }
    pw_context_t c = { .dwell_permil = 1000, .bus_yield_permil = 1000, .window_ms = 12000 };
    pw_context_t h = { .dwell_permil = 71, .bus_yield_permil = 1000, .window_ms = 12000 };
    pw_verdict_t vc, vh;
    pw_evaluate(&eng, last, &c, &vc);
    pw_evaluate(&eng, last, &h, &vh);
    po_report_t rep;
    po_assess(&vc, &vh, &rep);
    screen_footprint(&rep);
    fprintf(stderr, "  footprint: %s  tell=%s  %u vs %u  hoppers-miss=%d\n",
            po_grade_name(rep.grade), rep.tell_name, rep.camped_score,
            rep.hopping_score, rep.invisible_to_hoppers);
}

/* ---- screen: Locate (RSSI hotter/colder finder) ---------------------- */

static void screen_locate(const pl_verdict_t *v)
{
    static const char *fam[4] = { NULL, NULL, NULL, NULL };
    static const uint8_t h[PHAROS_DISP_HISTORY] = {
        30,40,55,70,90,110,130,150,170,190,205,220,232,240,248,255 };
    char det[64];
    snprintf(det, sizeof det, "%d dBm   peak %d", v->rssi_smoothed, v->rssi_peak);
    /* Closeness is the reading here, not a threat score, so the ring carries
     * it and the word is the trend. Honest: this is relative closeness on a
     * fixed scale, never metres. */
    screen_lumen_live("locate", "LOCATE", pl_trend_name(v->trend),
                      v->closeness, 0, det,
                      "signal is still climbing",
                      "Warmer - keep going.",
                      fam, 0x00u, h, false, PS_GOOD);
}

static void locate_screen(void)
{
    /* Simulate a walk toward the transmitter, feeding the real engine. */
    static const uint8_t target[6] = { 0x02, 0x66, 0x6E, 0x00, 0x00, 0x02 };
    pl_engine_t e;
    pl_reset(&e, target);
    uint64_t t = 1000000;
    for (int i = 0; i < 44; i++) {
        int r = -84 + (int)(46.0 * i / 43.0);   /* -84 dBm up to about -38 */
        pl_observe(&e, target, (int8_t)r, t);
        t += 200000;
    }
    pl_verdict_t v;
    pl_evaluate(&e, &v);
    screen_locate(&v);
    fprintf(stderr, "  locate: %s close=%u%% rssi=%d peak=%d\n",
            pl_trend_name(v.trend), v.closeness, v.rssi_smoothed, v.rssi_peak);
}

/* ---- screen: Home overview ------------------------------------------- */

__attribute__((unused)) static void screen_home_legacy(void)
{
    screen("home");
    panel_base();
    rim_ticks();

    /* A quiet "all watches nominal" home face: the lighthouse identity, the
     * clock, the posture, and a ring of small state pips - one per observing
     * lens - so a glance tells you the estate is calm. */
    const char *lens[6] = { "SPECTRUM", "WATCH", "CENSUS", "TWIN", "KARMA", "MIRAGE" };
    const char *pip[6]  = { C_CYAN, C_GREEN, C_AMBER, C_GREEN, C_GREEN, C_GREEN };

    /* Ring of lens pips around the ring zone. */
    for (unsigned i = 0; i < 6; i++) {
        const float a = 30.0f + 300.0f * (float)i / 5.0f;
        pr_point_t p = pr_polar((int16_t)(PR_RING_R - 6), a);
        glowdot(p.x, p.y, 6, pip[i]);
        pr_point_t lp = pr_polar((int16_t)(PR_RING_R - 34), a);
        const int size = fit_text_at(lp.x, lp.y, (unsigned)strlen(lens[i]), PR_SAFE_R - 8, 14);
        if (size) text(lp.x, lp.y, size, 'c', C_DIMMER, lens[i]);
    }

    disc(PR_CX, PR_CY, PR_CORE_R + 10, C_VOID);
    ring(PR_CX, PR_CY, PR_CORE_R + 10, 1, C_RIM);
    text(PR_CX, PR_CY - 46, 14, 'c', C_DIMMER, "PHAROS");
    glowtext(PR_CX, PR_CY - 8, 52, 'c', C_TEXT, "20:47");
    text(PR_CX, PR_CY + 30, 14, 'c', C_GREEN, "all quiet");
    text(PR_CX, PR_CY + 50, 14, 'c', C_DIMMER, "6 watches armed");

    /* The permanent receive-only tell is the green dot the rim already draws;
     * no caption needed (it would collide with the lower lens pips). */
    rim_status(82, C_GREEN);
}

/* ---- screen: Console (terminal) -------------------------------------- */

/* Stub ops so the console produces real output lines to render. */
static bool cx_activate(const char *id) { (void)id; return true; }
static void cx_set_channel(int ch) { (void)ch; }
static void cx_status(pc_out_t *o) { pc_println(o, "  SUSPICIOUS  60/60  hopping"); }
static void cx_fence(pc_out_t *o) { pc_println(o, "  clean - rx-only - 0 trips"); }

/* Render one terminal line, truncated to fit the round terminal card. */
static void term_line(int x, int *y, int maxchars, const char *col, const char *s)
{
    char buf[64];
    unsigned n = 0;
    while (s[n] && (int)n < maxchars && n < sizeof(buf) - 1) { buf[n] = s[n]; n++; }
    buf[n] = '\0';
    text(x, *y, 14, 'l', col, buf);
    *y += 20;
}

static void screen_console(void)
{
    screen("console");
    panel_base();
    rim_ticks();

    text(PR_CX, PR_CY - 176, 14, 'c', C_DIMMER, "CONSOLE  receive-only");

    /* A terminal card, corner-safe. */
    const int top = PR_CY - 150, h = 300;
    int w = card_width(top, h, PR_SAFE_R - 8);
    if (w > 320) w = 320;
    const int x = PR_CX - w / 2;
    roundrect(x, top, w, h, 12, "#07131b");
    const int lx = x + 16;
    const int maxc = (w - 28) / 8;   /* ~8px per char */
    int y = top + 22;

    pc_ops_t ops;
    memset(&ops, 0, sizeof(ops));
    ops.activate_lens = cx_activate;
    ops.set_channel = cx_set_channel;
    ops.status_line = cx_status;
    ops.fence_status = cx_fence;
    pc_out_t o;

    /* Each block: a glowing prompt line, then the console's REAL output. */
    struct { const char *cmd; const char *outcol; } steps[] = {
        { "fence", C_GREEN },
        { "watch camp 6", C_AMBER },
    };
    for (unsigned s = 0; s < 2; s++) {
        char prompt[48];
        snprintf(prompt, sizeof(prompt), "pharos> %s", steps[s].cmd);
        /* prompt: cyan chevron via colour, command in bright text */
        text(lx, y, 14, 'l', C_CYAN_HI, "pharos>");
        text(lx + 66, y, 14, 'l', C_TEXT, steps[s].cmd);
        y += 20;
        pc_exec(&ops, steps[s].cmd, &o);
        /* split output on newlines */
        const char *p = o.buf;
        while (*p && y < top + h - 40) {
            char line[64]; unsigned n = 0;
            while (*p && *p != '\n' && n < sizeof(line) - 1) line[n++] = *p++;
            line[n] = '\0';
            if (*p == '\n') p++;
            if (n) term_line(lx, &y, maxc, steps[s].outcol, line);
        }
        y += 4;
    }

    /* Live prompt with a block cursor. */
    text(lx, y, 14, 'l', C_CYAN_HI, "pharos>");
    glowline(lx + 70, y - 6, lx + 70, y + 6, 9, C_CYAN);

    /* The always-there reminder. */
    text(PR_CX, PR_CY + 168, 14, 'c', C_DIMMER, "type 'help' - no command transmits");
    rim_status(80, C_CYAN);
}


/* ======================================================================
 * LUMEN - the current face.
 *
 * These draw exactly what pharos_hud.c draws, from the same tokens in
 * pharos_style.h, so a PNG from this file is a preview and not an artist's
 * impression. When the two disagree it is a bug in one of them, and the
 * bounds check below is what catches the common half of that.
 * ====================================================================== */

/* Truncate exactly as pharos_hud.c's set_text_fit() does, so the PNG shows
 * what the glass shows - including the cut. A preview that quietly fitted
 * text the device would clip would be worse than no preview. */
static const char *fit(ps_type_t t, int16_t dy, const char *s)
{
    static char buf[4][128];
    static unsigned slot;
    char *b = buf[slot++ & 3u];
    const unsigned cap = ps_capacity(t, dy, PR_SAFE_R);
    const unsigned len = (unsigned)strlen(s);
    if (cap == 0u) { b[0] = '\0'; return b; }
    if (len <= cap) { snprintf(b, 128, "%s", s); return b; }
    unsigned n = cap < 127u ? cap : 127u;
    if (n > 1u) n -= 1u;
    unsigned cut = n;
    while (cut > 0u && cut + 6u > n && s[cut] != ' ') cut--;
    if (cut == 0u) cut = n;
    memcpy(b, s, cut);
    b[cut] = '.';
    b[cut + 1u] = '\0';
    return b;
}

/* A verdict colour at tint strength, as a hex string the display list can
 * carry. Same arithmetic as the firmware's ps_tint(). */
static const char *tint_hex(uint32_t rgb, uint8_t pct)
{
    static char buf[8][8];
    static unsigned slot;
    char *b = buf[slot++ & 7u];
    snprintf(b, 8, "#%06X", (unsigned)ps_tint(rgb, pct));
    return b;
}

static const char *rgb_hex(uint32_t rgb)
{
    static char buf[8][8];
    static unsigned slot;
    char *b = buf[slot++ & 7u];
    snprintf(b, 8, "#%06X", (unsigned)(rgb & 0xFFFFFFu));
    return b;
}

/* The true-black field. No bezel ticks: they carried no information and sat
 * in the invalidation path of everything that did. */
static void lumen_base(void)
{
    disc(PR_CX, PR_CY, PR_R, C_VOID);
}

/* The instant read. Three nested discs, opacity faked by stepping the tint,
 * because the display list has no alpha. */
static void lumen_aura(uint32_t rgb)
{
    /* Five layers, not three: three showed as three visible rings. */
    const int r[5]   = { PS_AURA_R, 128, 108, 88, 68 };
    const uint8_t m[5] = { 9, 13, 18, 24, 31 };
    for (unsigned i = 0; i < 5; i++) {
        disc(PR_CX, PR_CY, r[i], tint_hex(rgb, m[i]));
    }
}

static void lumen_ring(int score, int ceiling, uint32_t rgb)
{
    const float A0 = 225.0f, SWEEP = 270.0f;
    const int R = PS_RING_R;
    arc(PR_CX, PR_CY, R, 6, A0, SWEEP, "#0A1C26");
    if (score > 0) {
        arc(PR_CX, PR_CY, R, PS_RING_W, A0, SWEEP * (float)score / 100.0f,
            rgb_hex(rgb));
    }
    /* The ceiling is a TICK across the arc, never a second band: two bars of
     * the same weight showing different numbers read as a fault. */
    if (ceiling > 0 && ceiling < 100) {
        arc(PR_CX, PR_CY, R, PS_RING_W + 8,
            A0 + SWEEP * (float)ceiling / 100.0f, 1.6f, C_RED);
    }
}

/* Straight, not curved. Sixteen curved lines each had a screen-sized bounding
 * box, so one bar changing invalidated the panel. A timeline is a line. */
static void lumen_ribbon(const uint8_t *hist, uint32_t rgb)
{
    const int pitch = 14;
    for (unsigned i = 0; i < PHAROS_DISP_HISTORY; i++) {
        const int dx = -(int)(PHAROS_DISP_HISTORY - 1) * pitch / 2 + (int)i * pitch;
        const int h = 6 + ((int)hist[i] * 40) / 255;
        const int x = PR_CX + dx - 3;
        const int y = PR_CY + PS_Y_RIBBON - h / 2;
        roundrect(x, y, 7, h, 3, hist[i] ? rgb_hex(rgb) : "#18384A");
    }
}

static void lumen_chips(const char *const *lab, unsigned n, uint8_t lit,
                        uint32_t rgb)
{
    const int cw = ps_chip_w(n);
    if (cw <= 0) return;
    const int total = cw * (int)n + PS_CARD_GAP * ((int)n - 1);
    for (unsigned i = 0; i < n; i++) {
        if (!lab[i]) continue;
        const int dx = -total / 2 + (int)i * (cw + PS_CARD_GAP);
        const bool on = (lit & (1u << i)) != 0u;
        roundrect(PR_CX + dx, PR_CY + PS_Y_CHIPS - 15, cw, 30, 15,
                  on ? tint_hex(rgb, 40) : "#0E2028");
        text(PR_CX + dx + cw / 2, PR_CY + PS_Y_CHIPS + 5, 16, 'c',
             on ? rgb_hex(rgb) : C_DIMMER, lab[i]);
    }
}

/* The receive-only tell: a fact, not an alert, so it never animates. */
static void lumen_tell(void)
{
    dot(PR_CX, PR_CY + PS_Y_TELL, 5, C_GREEN);
}

/* ---- LUMEN: HOME ------------------------------------------------------
 *
 * No labels on the ring. Fourteen names never fitted a 466 px circle and
 * twenty-one made it worse; the names were never the point. "Is anything
 * wrong" is answered by counting the dots that are not green, and WHICH watch
 * matters only once you have decided to look - so it is one name in the
 * middle, always legible because there is only ever one of it. */
static void screen_lumen_home(void)
{
    screen("home");
    lumen_base();

    const unsigned n = 11;
    const uint8_t state[11] = { 0,0,1,0,0,0,2,0,0,0,0 };
    const int active = 6;
    const uint32_t worst = ps_alert_colour(2);

    lumen_aura(worst);
    lumen_ring(58, 0, worst);

    for (unsigned i = 0; i < n; i++) {
        /* Along the gauge's own 270 degrees, leaving the bottom notch for
         * the clock. */
        const float a = 225.0f + 270.0f * (float)i / (float)(n - 1u);
        pr_point_t p = pr_polar((int16_t)(PS_RING_R - 28), a);
        const uint32_t c = (state[i] == 0) ? PS_GOOD
                         : (state[i] == 1) ? PS_WARN
                         : (state[i] == 2) ? PS_HIGH : PS_BAD;
        const int rr = ((int)i == active) ? 10 : 7;
        dot(p.x, p.y, rr, rgb_hex(c));
    }

    text(PR_CX, PR_CY + PS_Y_CLOCK, 16, 'c', C_DIMMER, "20:47");
    glowtext(PR_CX, PR_CY + PS_Y_HERO, 36, 'c', rgb_hex(worst),
             fit(PS_TYPE_HERO, PS_Y_HERO, "WORTH A LOOK"));
    text(PR_CX, PR_CY + PS_Y_METRIC, 20, 'c', C_DIM, "11 watches armed");
    text(PR_CX, PR_CY + PS_Y_DETAIL + 16, 16, 'c', C_CYAN, "MIRAGE");
    lumen_tell();
}

/* ---- LUMEN: LIVE ------------------------------------------------------ */
static void screen_lumen_live(const char *name, const char *lens,
                              const char *word, int score, int ceiling,
                              const char *detail, const char *why,
                              const char *action,
                              const char *const *fam, uint8_t lit,
                              const uint8_t *hist, bool simulated,
                              uint32_t rgb_override)
{
    screen(name);
    lumen_base();

    /* A lens whose score is not a THREAT scale says so, exactly as the
     * firmware's has_alert does. Without it, Locate paints the glass red as
     * you walk toward the thing you are hunting. */
    const uint32_t rgb = rgb_override ? rgb_override : ps_score_colour(score);
    lumen_aura(rgb);
    lumen_ribbon(hist, rgb);
    lumen_ring(score, ceiling, rgb);

    if (simulated) {
        text(PR_CX, PR_CY + PS_Y_SIM, 16, 'c', C_AMBER,
             "SIMULATION - NOT THIS ROOM");
    }
    text(PR_CX, PR_CY + PS_Y_CONTEXT, 16, 'c', C_DIMMER,
         fit(PS_TYPE_LABEL, PS_Y_CONTEXT, lens));

    /* The word leads; the number supports it and is set smaller. The face
     * this replaces had that backwards, at 48 px of score over 26 px of
     * meaning, which is why it read as a number generator. */
    glowtext(PR_CX, PR_CY + PS_Y_HERO, 36, 'c', rgb_hex(rgb),
             fit(PS_TYPE_HERO, PS_Y_HERO, word));

    char num[24];
    if (ceiling > 0 && ceiling < 100) snprintf(num, sizeof num, "%d / %d", score, ceiling);
    else                              snprintf(num, sizeof num, "%d", score);
    text(PR_CX, PR_CY + PS_Y_METRIC, 28, 'c', C_DIM, num);
    text(PR_CX, PR_CY + PS_Y_DETAIL, 16, 'c', C_DIM,
         fit(PS_TYPE_LABEL, PS_Y_DETAIL, detail));

    lumen_chips(fam, PHAROS_DISP_FAMILIES, lit, rgb);

    if (why && *why)
        text(PR_CX, PR_CY + PS_Y_WHY, 16, 'c', C_DIM,
             fit(PS_TYPE_LABEL, PS_Y_WHY, why));
    {
        /* Split on a word across two bands, as the firmware does. */
        const unsigned cap = ps_capacity(PS_TYPE_LABEL, PS_Y_ACTION, PR_SAFE_R);
        char a1[64], a2[64];
        a1[0] = a2[0] = '\0';
        const unsigned len = (unsigned)strlen(action);
        if (len <= cap) {
            snprintf(a1, sizeof a1, "%s", action);
        } else {
            unsigned cut = cap;
            while (cut > 0u && action[cut] != ' ') cut--;
            if (cut == 0u) cut = cap;
            memcpy(a1, action, cut); a1[cut] = '\0';
            snprintf(a2, sizeof a2, "%s", action + cut + (action[cut] == ' ' ? 1 : 0));
        }
        text(PR_CX, PR_CY + PS_Y_ACTION, 16, 'c', C_CYAN, a1);
        if (a2[0]) text(PR_CX, PR_CY + PS_Y_ACTION2, 16, 'c', C_CYAN,
                        fit(PS_TYPE_LABEL, PS_Y_ACTION2, a2));
    }
    lumen_tell();
}

/* ---- LUMEN: BROWSE ----------------------------------------------------
 *
 * Plain English leads and the evocative name is subordinate to it. KARMA and
 * SQUALL tell a newcomer nothing; what the tool DOES tells them everything. */
static void screen_lumen_browse(void)
{
    screen("browse");
    lumen_base();

    roundrect(PR_CX - 66, PR_CY - 174, 132, 32, 16, tint_hex(0x21B6C6, 34));
    text(PR_CX, PR_CY - 152, 16, 'c', C_CYAN, "OBSERVE");

    text(PR_CX, PR_CY - 88, 28, 'c', C_TEXT, "Karma");
    text(PR_CX, PR_CY - 18, 20, 'c', C_DIM, "is a rogue AP answering");
    text(PR_CX, PR_CY + 14, 20, 'c', C_DIM, "for networks it has not got");
    text(PR_CX, PR_CY + 66, 14, 'c', C_DIMMER, "5 of 21");

    roundrect(PR_CX - 95, PR_CY + 102, 190, 56, 28, C_CYAN);
    text(PR_CX, PR_CY + 136, 20, 'c', "#00181C", "START");
    lumen_tell();
}

/* ---- LUMEN: DETAIL ---------------------------------------------------- */
static void screen_lumen_detail_named(const char *name, const char *title,
                                      const char *hl, const char *hr);

static void screen_lumen_detail(void)
{
    screen_lumen_detail_named("detail", "CENSUS", "worst first", "2.4 GHz");
}

static void screen_lumen_detail_named(const char *name, const char *title,
                                      const char *hl, const char *hr)
{
    screen(name);
    lumen_base();

    text(PR_CX, PR_CY - 196, 16, 'c', C_DIMMER, title);
    text(PR_CX - 72, PR_CY - 164, 14, 'c', C_DIMMER, hl);
    text(PR_CX + 72, PR_CY - 164, 14, 'c', C_DIMMER, hr);

    /* The third of these is the string that broke on hardware: parented to
     * the page and centre-aligned, its left half ran off the glass and the
     * row read "p models / advs". It is kept here as the regression. */
    const char *L[4] = { "GuestNet", "OfficeWiFi",
                         "Top models / advs", "Acme-Secure" };
    const char *R[4] = { "F", "D", "0/0", "A+" };
    const char *T[4] = { C_RED, C_ORANGE, C_AMBER, C_GREEN };

    const int stack = 4 * PS_CARD_H + 3 * PS_CARD_GAP;
    for (unsigned i = 0; i < 4; i++) {
        const int dy = -stack / 2 + (int)i * (PS_CARD_H + PS_CARD_GAP)
                     + PS_CARD_H / 2 + 8;
        const int top = PR_CY + dy - PS_CARD_H / 2;
        const int w = card_width(top, PS_CARD_H, PR_SAFE_R) - 12;
        roundrect(PR_CX - w / 2, top, w, PS_CARD_H, 14,
                  i == 0 ? tint_hex(0x21B6C6, 30) : "#0B1A21");
        text(PR_CX - w / 2 + 18, PR_CY + dy + 7, 20, 'l', C_TEXT, L[i]);
        text(PR_CX + w / 2 - 18, PR_CY + dy + 7, 20, 'r', T[i], R[i]);
    }
    text(PR_CX, PR_CY + PS_Y_PAGE, 14, 'c', C_DIMMER, "1 / 3");
    lumen_tell();
}

/* ---- LUMEN: SPLASH ---------------------------------------------------- */
static void screen_lumen_splash(void)
{
    screen("splash");
    lumen_base();
    glowtext(PR_CX, PR_CY - 20, 48, 'c', C_TEXT, "PHAROS");
    text(PR_CX, PR_CY + 30, 20, 'c', C_DIM, "v3.0.0");
    text(PR_CX, PR_CY + 84, 16, 'c', C_GREEN, "RECEIVE ONLY - FENCE CLEAN");
}

/* ---- LUMEN: the first-run guide ---------------------------------------
 *
 * Drawn from the same tokens as the firmware's guide page. The animations are
 * static here, obviously - a still shows the outline and the fingertip at one
 * moment of the pulse, which is enough to check the geometry fits. */
static void guide_frame(const char *name, unsigned step, unsigned total,
                        const char *title, const char *l1, const char *l2,
                        const char *hint)
{
    screen(name);
    lumen_base();

    const int pitch = 18;
    for (unsigned i = 0; i < total; i++) {
        const int dx = -(int)(total - 1u) * pitch / 2 + (int)i * pitch;
        const bool here = (i == step);
        dot(PR_CX + dx, PR_CY + PS_Y_GUIDE_PIPS, here ? 6 : 4,
            here ? C_CYAN : "#18384A");
    }
    text(PR_CX, PR_CY + PS_Y_GUIDE_TITLE + 10, 28, 'c', C_TEXT, title);
    text(PR_CX, PR_CY + PS_Y_GUIDE_L1 + 7, 20, 'c', C_DIM, l1);
    text(PR_CX, PR_CY + PS_Y_GUIDE_L2 + 7, 20, 'c', C_DIM, l2);
    text(PR_CX, PR_CY + PS_Y_GUIDE_HINT + 5, 16, 'c', C_CYAN, hint);
    lumen_tell();
}

static void screen_guide_sides(void)
{
    guide_frame("guide_sides", 1, 9, "Change tool",
                "Tap the left or right edge",
                "to move through the tools.",
                "try it, this page waits");
    /* the zone, and the fingertip travelling between the two of them */
    roundrect(PR_CX - 195, PR_CY - 100, 110, 200, 24, "#0E2630");
    dot(PR_CX - 140, PR_CY, 27, "#2F7C8C");
    dot(PR_CX + 140, PR_CY, 20, "#173D48");
}

static void screen_guide_verdict(void)
{
    guide_frame("guide_verdict", 5, 9, "The colour key",
                "Every screen uses these four.",
                "You may stop at the colour.",
                "right side to go on");
    static const char *col[4] = { C_GREEN, C_AMBER, C_ORANGE, C_RED };
    static const char *mean[4] = { "nothing to do", "worth knowing",
                                   "one real finding", "act on this" };
    for (unsigned i = 0; i < 4; i++) {
        const int dy = -58 + (int)i * 46;
        roundrect(PR_CX - 134, PR_CY + dy - 17, 44, 34, 10, col[i]);
        text(PR_CX - 78, PR_CY + dy + 6, 16, 'l', C_TEXT, mean[i]);
    }
}

static void screen_guide_ring(void)
{
    guide_frame("guide_ring", 6, 9, "The home ring",
                "One dot for each watch.",
                "Count the ones not green.",
                "right side to go on");
    static const uint8_t st[8] = { 0, 0, 0, 1, 0, 0, 3, 0 };
    for (unsigned i = 0; i < 8; i++) {
        const float a = 225.0f + 270.0f * (float)i / 7.0f;
        pr_point_t p = pr_polar(96, a);
        const char *c = st[i] == 0 ? C_GREEN : st[i] == 1 ? C_AMBER : C_RED;
        dot(p.x, p.y, st[i] ? 11 : 8, c);
    }
}

/* The live screens, driven from the REAL engines where one exists. */
static void lumen_screens(void)
{
    static const uint8_t quiet[PHAROS_DISP_HISTORY] = {
        8,12,6,10,14,9,7,11,8,13,6,9,12,7,10,8 };
    static const uint8_t burst[PHAROS_DISP_HISTORY] = {
        10,14,22,90,180,240,255,230,190,240,255,210,120,60,30,18 };
    static const char *fam4[4] = { "RATE", "SHAPE", "FORGE", "AFTER" };

    screen_lumen_splash();
    screen_guide_sides();
    screen_guide_verdict();
    screen_guide_ring();
    screen_lumen_home();
    screen_lumen_browse();
    screen_lumen_detail();

    screen_lumen_live("watch_camped", "DEAUTH WATCH", "FLOOD LIKELY", 88, 96,
                      "camped ch6   107/s   840 obs",
                      "sequence counter went backwards",
                      "Broad, spoofed deauth. Preserve the log.",
                      fam4, 0x0Fu, burst, false, 0);

    screen_lumen_live("watch_hopping", "DEAUTH WATCH", "SUSPICIOUS", 60, 60,
                      "hopping 1-13   dwell 7%",
                      "", "Evidence thin. Camp here to confirm.",
                      fam4, 0x03u, burst, false, 0);

    /* The third face in the README's argument: same hopping receiver, but the
     * evidence is a CONTRADICTION (protected-management-frame network,
     * unprotected disconnects) rather than an extrapolation - so it raises its
     * own ceiling to 88 and is allowed to alarm while hopping. */
    screen_lumen_live("watch_proven", "DEAUTH WATCH", "FLOOD LIKELY", 88, 88,
                      "hopping 1-13   MFP required",
                      "unprotected deauth on a protected net",
                      "Forged. This cannot be the real AP.",
                      fam4, 0x0Du, burst, false, 0);

    screen_lumen_live("quiet", "DEAUTH WATCH", "QUIET", 12, 96,
                      "camped ch1   0.2/s   14 obs",
                      "", "Nothing to do.",
                      fam4, 0x00u, quiet, false, 0);
}

int main(int argc, char **argv)
{
    const bool check_only = (argc > 1 && strcmp(argv[1], "--check") == 0);
    g_out = check_only ? fopen("/dev/null", "w") : stdout;
    if (!g_out) {
        return 2;
    }

    lumen_screens();
    screen_console();
    screen_census();
    karma_screen();
    mirage_screen();
    locate_screen();
    footprint_screen();
    screen_spectrum();

    if (g_violations) {
        fprintf(stderr, "FAIL: %d primitive(s) escaped the safe radius\n", g_violations);
        return 1;
    }
    fprintf(stderr, "  all primitives inside the %dpx safe radius\n", PR_SAFE_R);
    return 0;
}
