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
#include "pharos_dial.h"
#include "pharos_flood.h"
#include "pharos_karma.h"
#include "pharos_opsec.h"
#include "pharos_range.h"
#include "pharos_round.h"
#include "pharos_twin.h"
#include "pharos_watch.h"

/* ---- palette ---------------------------------------------------------
 * Dark-first, because the panel is AMOLED: an unlit pixel costs no power, so
 * the HUD is mostly black with a single accent carrying the state. */
#define C_VOID    "#04090F"
#define C_FIELD   "#0B1D2B"
#define C_FIELD2  "#0E2A3C"
#define C_RIM     "#1E5266"
#define C_CYAN    "#1FB6C9"
#define C_CYAN_HI "#7FEBF6"
#define C_AMBER   "#FFC24B"
#define C_ORANGE  "#F0913A"
#define C_RED     "#E8503F"
#define C_GREEN   "#3DDC84"
#define C_TEXT    "#EAF6FA"
#define C_DIM     "#7FA6B5"
#define C_DIMMER  "#4E7A8C"
#define C_DENIED  "#243B49"

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

/* Word-wrap through the real chord guard: how many characters fit on a line
 * at this vertical offset is a property of the circle, not a guess. */
static int wrap(int dy, int size, const char *col, const char *s, int max_lines)
{
    int lines = 0;
    const char *p = s;
    while (*p && lines < max_lines) {
        const int y = PR_CY + dy + lines * (size * 5 / 4);
        const unsigned cap = pd_label_capacity((int16_t)size, (int16_t)(dy + lines * (size * 5 / 4)),
                                               PR_SAFE_R - 6);
        if (cap == 0) break;
        unsigned take = 0, last_space = 0;
        while (p[take] && take < cap) {
            if (p[take] == ' ') last_space = take;
            take++;
        }
        if (p[take] && last_space) take = last_space;
        char buf[128];
        unsigned n = take < sizeof(buf) - 1 ? take : (unsigned)sizeof(buf) - 1;
        memcpy(buf, p, n);
        buf[n] = '\0';
        /* trim trailing space */
        while (n && buf[n - 1] == ' ') buf[--n] = '\0';
        if (n) {
            text(PR_CX, y, size, 'c', col, buf);
            lines++;
        }
        p += take;
        while (*p == ' ') p++;
    }
    return lines;
}

/* ---- shared chrome --------------------------------------------------- */

static void panel_base(void)
{
    disc(PR_CX, PR_CY, PR_R, C_VOID);
    disc(PR_CX, PR_CY, PR_R - 2, C_FIELD);
    ring(PR_CX, PR_CY, PR_RIM_R - 4, 1, C_RIM);
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

/* Battery / posture arc on the rim, plus the permanent receive-only dot. */
static void rim_status(int battery_pct, const char *band_col)
{
    arc(PR_CX, PR_CY, PR_SAFE_R - 12, 4, 200.0f,
        160.0f * (float)battery_pct / 100.0f, C_DIMMER);
    arc(PR_CX, PR_CY, PR_SAFE_R - 12, 4, 20.0f, 60.0f, band_col);
    dot(PR_CX, PR_CY + PR_RIM_R - 40, 4, C_GREEN);
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

static void screen_lamp_room(void)
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

/* ---- screen: the Watch gauge ----------------------------------------- */

/* The gauge's one-line summary. Deliberately consistent with, and shorter
 * than, pw_band_advice() - the full sentence is shown on the lens info card
 * where there is room for it. */
static const char *hud_summary(pw_band_t band)
{
    switch (band) {
    case PW_BAND_QUIET:      return "Nothing in view. One channel at a time.";
    case PW_BAND_BACKGROUND: return "Normal roaming and idle timeouts.";
    case PW_BAND_ELEVATED:   return "More than housekeeping. Camp to sharpen.";
    case PW_BAND_SUSPICIOUS: return "Evidence thin. Camp here to confirm.";
    case PW_BAND_LIKELY:     return "Broad, spoofed deauth. Preserve the log.";
    default:                 return "";
    }
}

/* Renders one Watch verdict. The camped/hopping pair is generated from the
 * *identical* event stream, which is the whole argument of the project made
 * visible: same evidence, different entitlement to claim it. */
static void screen_watch(const char *name, const pw_verdict_t *v, const char *mode,
                         int dwell_pct)
{
    screen(name);
    panel_base();
    rim_ticks();

    const char *col = band_colour(v->score);

    /* The evidence gauge: 270 degrees starting at 7 o'clock. */
    const float A0 = 225.0f, SWEEP = 270.0f;
    const uint8_t comps[4] = { v->c_rate, v->c_target, v->c_identity, v->c_reason };
    pd_gauge_t g;
    pd_gauge_layout(comps, 4, v->score, v->ceiling, A0, SWEEP, &g);

    /* Track. */
    arc(PR_CX, PR_CY, PR_RING_R - 8, 14, A0, SWEEP, C_VOID);
    arc(PR_CX, PR_CY, PR_RING_R - 8, 14, A0, SWEEP, "#0F2231");

    /* Component arcs; denied ones dimmed so the operator can see what the
     * observation quality cost them. */
    static const char *fam_col[4] = { C_CYAN, C_CYAN_HI, C_AMBER, C_DIMMER };
    unsigned fam = 0;
    for (unsigned i = 0; i < g.n_arcs; i++) {
        const pd_arc_t *a = &g.arcs[i];
        arc(PR_CX, PR_CY, PR_RING_R - 8, 14, a->start_deg, a->sweep_deg,
            a->denied ? C_DENIED : fam_col[fam % 4]);
        if (!a->denied) fam++;
    }

    /* The ceiling tick: a hard stop the score may not pass. Its label goes
     * *inside* the gauge ring - outside, it would sit among the rim ticks and
     * fight the bezel for attention at exactly the angles it matters most. */
    {
        pr_point_t t0 = pr_polar((int16_t)(PR_RING_R + 2), g.ceiling_deg);
        pr_point_t t1 = pr_polar((int16_t)(PR_RING_R - 24), g.ceiling_deg);
        line(t0.x, t0.y, t1.x, t1.y, 3, C_RED);
        pr_point_t lp = pr_polar((int16_t)(PR_RING_R - 42), g.ceiling_deg);
        char buf[24];
        snprintf(buf, sizeof(buf), "CEIL %u", v->ceiling);
        const int size = fit_text_at(lp.x, lp.y, (unsigned)strlen(buf), PR_SAFE_R - 6, 14);
        if (size) {
            text(lp.x, lp.y, size, 'c', C_RED, buf);
        }
    }

    /* Core: score and band. */
    disc(PR_CX, PR_CY, PR_CORE_R + 4, C_VOID);
    {
        char buf[16];
        snprintf(buf, sizeof(buf), "%u", v->score);
        text(PR_CX, PR_CY - 12, 64, 'c', col, buf);
    }
    text(PR_CX, PR_CY + 30, 20, 'c', col, pw_band_name(v->band));
    text(PR_CX, PR_CY - 54, 14, 'c', C_DIMMER, "DEAUTH WATCH");

    /* Below the core: the mode that produced this reading. */
    {
        char buf[48];
        snprintf(buf, sizeof(buf), "%s  dwell %d%%", mode, dwell_pct);
        text(PR_CX, PR_CY + 82, 14, 'c', C_DIM, buf);
    }
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "%u.%u/s  %u obs", v->est_per_s_x100 / 100,
                 (v->est_per_s_x100 / 10) % 10, v->observed);
        text(PR_CX, PR_CY + 102, 14, 'c', C_DIMMER, buf);
    }

    /* Families as three pips: filled when that family fired. */
    for (unsigned i = 0; i < 3; i++) {
        const int x = PR_CX - 26 + (int)i * 26;
        dot(x, PR_CY + 124, 6, (v->families & (1u << i)) ? col : C_DENIED);
    }

    /* The HUD carries a one-line summary; the engine's full advice text lives
     * on the info card, because 466 round pixels cannot hold 120 characters
     * and truncating a sentence mid-clause is exactly the sort of quiet
     * dishonesty this project is trying not to commit. */
    wrap(146, 14, C_DIM, hud_summary(v->band), 2);

    rim_status(74, col);
}

/* ---- screen: Census list --------------------------------------------- */

typedef struct { const char *ssid; const char *grade; int score; const char *why; } row_t;

static void screen_census(void)
{
    screen("census");
    panel_base();
    rim_ticks();

    static const row_t rows[] = {
        { "GuestNet",    "F", 15, "open - no key at all" },
        { "OfficeWiFi",  "D", 58, "WPA2, no 802.11w" },
        { "Acme-Staff",  "C", 70, "WPA2 + MFP capable" },
        { "Acme-Secure", "A+", 100, "WPA3 + MFP required" },
    };
    const unsigned n = sizeof(rows) / sizeof(rows[0]);

    text(PR_CX, PR_CY - 176, 14, 'c', C_DIMMER, "CENSUS  4 NETWORKS");

    for (unsigned i = 0; i < n; i++) {
        const int top = PR_CY - 148 + (int)i * 62;
        const int h = 52;
        const char *col = (rows[i].score >= 88) ? C_GREEN
                        : (rows[i].score >= 68) ? C_AMBER
                        : (rows[i].score >= 48) ? C_ORANGE : C_RED;
        /* Sized from the worst corner, not the midline - see card_width. */
        int w = card_width(top, h, PR_SAFE_R - 6);
        if (w > 372) w = 372;
        const int x = PR_CX - w / 2;
        roundrect(x, top, w, h, 10, C_FIELD2);

        text(x + 16, top + 20, 20, 'l', C_TEXT, rows[i].ssid);
        text(x + 16, top + 40, 14, 'l', C_DIMMER, rows[i].why);
        text(x + w - 30, top + 28, 34, 'c', col, rows[i].grade);
    }

    text(PR_CX, PR_CY + 160, 14, 'c', C_DIM, "worst posture first");
    text(PR_CX, PR_CY + 180, 14, 'c', C_DIMMER, "2.4 GHz only");
    rim_status(74, C_AMBER);
}

/* ---- screen: Karma --------------------------------------------------- */

static void screen_karma(const pk_verdict_t *v)
{
    screen("karma");
    panel_base();
    rim_ticks();

    const char *col = band_colour(v->score);
    const float A0 = 225.0f, SWEEP = 270.0f;
    const uint8_t comps[3] = { v->c_breadth, v->c_absence, v->c_echo };
    pd_gauge_t g;
    pd_gauge_layout(comps, 3, v->score, v->ceiling, A0, SWEEP, &g);

    arc(PR_CX, PR_CY, PR_RING_R - 8, 14, A0, SWEEP, "#0F2231");
    static const char *fam_col[3] = { C_CYAN, C_AMBER, C_CYAN_HI };
    unsigned fam = 0;
    for (unsigned i = 0; i < g.n_arcs; i++) {
        arc(PR_CX, PR_CY, PR_RING_R - 8, 14, g.arcs[i].start_deg, g.arcs[i].sweep_deg,
            g.arcs[i].denied ? C_DENIED : fam_col[fam % 3]);
        if (!g.arcs[i].denied) fam++;
    }
    {
        pr_point_t t0 = pr_polar(PR_RING_R + 4, g.ceiling_deg);
        pr_point_t t1 = pr_polar((int16_t)(PR_RING_R - 22), g.ceiling_deg);
        line(t0.x, t0.y, t1.x, t1.y, 3, C_RED);
    }

    disc(PR_CX, PR_CY, PR_CORE_R + 4, C_VOID);
    text(PR_CX, PR_CY - 58, 14, 'c', C_DIMMER, "KARMA WATCH");
    {
        char buf[16];
        snprintf(buf, sizeof(buf), "%u", v->score);
        text(PR_CX, PR_CY - 14, 64, 'c', col, buf);
    }
    text(PR_CX, PR_CY + 28, 20, 'c', col, pk_band_name(v->band));
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "%u names, %u never announced",
                 v->answered_ssids, v->unannounced);
        text(PR_CX, PR_CY + 84, 14, 'c', C_DIM, buf);
    }
    {
        char buf[64];
        snprintf(buf, sizeof(buf), "%02X:%02X:%02X:xx:xx:xx",
                 v->suspect[0], v->suspect[1], v->suspect[2]);
        text(PR_CX, PR_CY + 106, 14, 'c', C_DIMMER, buf);
    }
    wrap(136, 14, C_DIM, v->headline, 2);
    rim_status(70, col);
}

/* ---- screen: Spectrum ------------------------------------------------ */

static void screen_spectrum(void)
{
    screen("spectrum");
    panel_base();
    rim_ticks();

    /* A polar bar per channel: the round-native way to show a band.
     *
     * The bar length is scaled so that a fully saturated channel plus its
     * label still lands inside the glass. Deriving the scale from the radius
     * budget - rather than picking a pixels-per-unit that happens to look
     * right - is what stops a busy channel drawing off the panel. */
    static const uint8_t busy[13] = { 210, 60, 40, 90, 120, 255, 150, 70, 40, 30, 190, 80, 45 };
    const int base_r = PR_CORE_R + 12;
    const int label_gap = 16;
    const int max_tip = PR_SAFE_R - 10 - label_gap;
    const int span = max_tip - base_r - 30; /* room left after the 30px stub */

    for (unsigned c = 0; c < 13; c++) {
        const float a = 225.0f + (270.0f * (float)c) / 12.0f;
        const int len = 30 + (int)(((long)busy[c] * span) / 255);
        pr_point_t base = pr_polar((int16_t)base_r, a);
        pr_point_t tip = pr_polar((int16_t)(base_r + len), a);
        const char *col = busy[c] > 200 ? C_RED : busy[c] > 130 ? C_AMBER : C_CYAN;
        line(base.x, base.y, tip.x, tip.y, 9, col);

        if (c == 0 || c == 5 || c == 10 || c == 12) {
            char lbl[4];
            snprintf(lbl, sizeof(lbl), "%u", c + 1);
            pr_point_t lp = pr_polar((int16_t)(base_r + len + label_gap), a);
            const int size = fit_text_at(lp.x, lp.y, (unsigned)strlen(lbl), PR_SAFE_R - 4, 14);
            if (size) {
                text(lp.x, lp.y, size, 'c', C_DIMMER, lbl);
            }
        }
    }

    disc(PR_CX, PR_CY, PR_CORE_R - 4, C_VOID);
    text(PR_CX, PR_CY - 30, 14, 'c', C_DIMMER, "BUSIEST");
    text(PR_CX, PR_CY + 2, 64, 'c', C_TEXT, "6");
    text(PR_CX, PR_CY + 40, 14, 'c', C_DIM, "2.4 GHz only");
    rim_status(74, C_CYAN);
}

/* ---- driving the real engines ---------------------------------------- */

/* Play the range's deauth-flood scenario into the real Watch engine, then
 * grade the identical stream twice - camped and hopping. */
static void watch_pair(void)
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

    screen_watch("watch_camped", &vc, "CAMPED ch6", 100);
    screen_watch("watch_hopping", &vh, "HOPPING 1-13", 7);

    fprintf(stderr, "  watch: camped=%u/%u %s   hopping=%u/%u %s\n",
            vc.score, vc.ceiling, pw_band_name(vc.band),
            vh.score, vh.ceiling, pw_band_name(vh.band));
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
    screen("mirage");
    panel_base();
    rim_ticks();

    const char *col = band_colour(v->score);
    const float A0 = 225.0f, SWEEP = 270.0f;
    const uint8_t comps[3] = { v->c_volume, v->c_ephemeral, v->c_synthetic };
    pd_gauge_t g;
    pd_gauge_layout(comps, 3, v->score, v->ceiling, A0, SWEEP, &g);

    arc(PR_CX, PR_CY, PR_RING_R - 8, 14, A0, SWEEP, "#0F2231");
    static const char *fam_col[3] = { C_CYAN, C_AMBER, C_CYAN_HI };
    unsigned fam = 0;
    for (unsigned i = 0; i < g.n_arcs; i++) {
        arc(PR_CX, PR_CY, PR_RING_R - 8, 14, g.arcs[i].start_deg, g.arcs[i].sweep_deg,
            g.arcs[i].denied ? C_DENIED : fam_col[fam % 3]);
        if (!g.arcs[i].denied) fam++;
    }
    {
        pr_point_t t0 = pr_polar((int16_t)(PR_RING_R + 2), g.ceiling_deg);
        pr_point_t t1 = pr_polar((int16_t)(PR_RING_R - 24), g.ceiling_deg);
        line(t0.x, t0.y, t1.x, t1.y, 3, C_RED);
    }

    disc(PR_CX, PR_CY, PR_CORE_R + 4, C_VOID);
    text(PR_CX, PR_CY - 58, 14, 'c', C_DIMMER, "BEACON FLOOD");
    {
        char buf[16];
        snprintf(buf, sizeof(buf), "%u", v->score);
        text(PR_CX, PR_CY - 14, 64, 'c', col, buf);
    }
    text(PR_CX, PR_CY + 28, 20, 'c', col, pf_band_name(v->band));
    {
        char buf[48];
        snprintf(buf, sizeof(buf), "%u names  %u.%u new/min", v->distinct_ssids,
                 v->new_per_min_x10 / 10, v->new_per_min_x10 % 10);
        text(PR_CX, PR_CY + 84, 14, 'c', C_DIM, buf);
    }
    {
        char buf[48];
        snprintf(buf, sizeof(buf), "%u%% on software radios", v->synthetic_permil / 10);
        text(PR_CX, PR_CY + 104, 14, 'c', C_DIMMER, buf);
    }
    for (unsigned i = 0; i < 3; i++) {
        dot(PR_CX - 26 + (int)i * 26, PR_CY + 128, 6,
            (v->families & (1u << i)) ? col : C_DENIED);
    }
    rim_status(72, col);
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
    screen("footprint");
    panel_base();
    rim_ticks();

    const char *col = (r->grade >= PO_GRADE_LOUD) ? C_RED
                    : (r->grade == PO_GRADE_FAINT) ? C_AMBER : C_GREEN;

    /* Two facing arcs: camped (what a still defender sees) vs hopping (what a
     * moving one sees). The gap between them IS the OPSEC insight. */
    arc(PR_CX, PR_CY, PR_RING_R - 8, 14, 180.0f, 150.0f, "#0F2231");
    arc(PR_CX, PR_CY, PR_RING_R - 8, 14, 30.0f, 150.0f, "#0F2231");
    arc(PR_CX, PR_CY, PR_RING_R - 8, 14, 180.0f, 150.0f * (float)r->camped_score / 100.0f, C_RED);
    arc(PR_CX, PR_CY, PR_RING_R - 8, 14, 30.0f, 150.0f * (float)r->hopping_score / 100.0f, C_CYAN);

    /* One legend line in the open gap at the top, between the two arcs, rather
     * than side labels that would collide with the thick arcs at 9 and 3. */
    {
        dot(PR_CX - 78, PR_CY - 150, 5, C_RED);
        text(PR_CX - 40, PR_CY - 150, 14, 'c', C_RED, "CAMPED");
        dot(PR_CX + 12, PR_CY - 150, 5, C_CYAN);
        text(PR_CX + 52, PR_CY - 150, 14, 'c', C_CYAN, "HOPPING");
    }

    disc(PR_CX, PR_CY, PR_CORE_R + 6, C_VOID);
    text(PR_CX, PR_CY - 56, 14, 'c', C_DIMMER, "FOOTPRINT");
    text(PR_CX, PR_CY - 20, 44, 'c', col, po_grade_name(r->grade));
    {
        char buf[32];
        snprintf(buf, sizeof(buf), "%u vs %u", r->camped_score, r->hopping_score);
        text(PR_CX, PR_CY + 16, 24, 'c', C_TEXT, buf);
    }
    {
        char buf[40];
        snprintf(buf, sizeof(buf), "tell: %s", r->tell_name);
        text(PR_CX, PR_CY + 44, 14, 'c', C_DIM, buf);
    }

    if (r->invisible_to_hoppers) {
        wrap(120, 14, C_AMBER, "Loud when watched - a hopping defender misses it", 2);
    } else {
        char buf[40];
        snprintf(buf, sizeof(buf), "stealth gap %u pts", r->stealth_gap);
        text(PR_CX, PR_CY + 132, 14, 'c', C_DIMMER, buf);
    }
    rim_status(74, col);
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

int main(int argc, char **argv)
{
    const bool check_only = (argc > 1 && strcmp(argv[1], "--check") == 0);
    g_out = check_only ? fopen("/dev/null", "w") : stdout;
    if (!g_out) {
        return 2;
    }

    screen_lamp_room();
    watch_pair();
    screen_census();
    karma_screen();
    mirage_screen();
    footprint_screen();
    screen_spectrum();

    if (g_violations) {
        fprintf(stderr, "FAIL: %d primitive(s) escaped the safe radius\n", g_violations);
        return 1;
    }
    fprintf(stderr, "  all primitives inside the %dpx safe radius\n", PR_SAFE_R);
    return 0;
}
