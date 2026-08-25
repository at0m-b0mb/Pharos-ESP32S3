/* Pharos - the on-device LVGL screen. See pharos_hud.h for the layout and for
 * the four specific bugs that made the previous one flicker and break.
 *
 * Only compiled with a real panel; with the vendor BSP off these become no-ops
 * so the firmware still builds and runs headless.
 *
 * Layout notes for a 466 px CIRCLE. The usable rectangle inside a circle is a
 * lot smaller than the circle: towards the top and bottom the chord narrows to
 * nothing, so anything wide has to live near the middle. Every position here
 * is the same one tools/render/pharos_render.c draws, and that renderer
 * bounds-checks every primitive against the glass in CI - so the layout is
 * verified on a laptop before any pixel is lit.
 */

/* sdkconfig.h FIRST - see the long note in pharos_bsp.c. The #if below tests
 * CONFIG_PHAROS_HAS_VENDOR_BSP, and without this the macro is undefined at
 * that point and the whole HUD compiles to no-ops. */
#include "sdkconfig.h"

#include "esp_log.h"
#include "pharos_hud.h"
#include "pharos_theme.h"

#if !defined(PHAROS_HOST) && defined(CONFIG_PHAROS_HAS_VENDOR_BSP)

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lvgl.h"

/* PR_W / PR_H and the polar helpers - the panel geometry, shared with the
 * round-screen maths and with the renderer. */
#include "pharos_dial.h"
#include "pharos_round.h"

/* Every colour here is EXACTLY representable in RGB565, which is what the
 * CO5300 is driven at over QSPI.
 *
 * This matters more than it sounds. 16-bit colour expands back to 8-bit as
 * R8 = (r5<<3)|(r5>>2), G8 = (g6<<2)|(g6>>4) - so any colour not of that form
 * is silently rounded, and rounded by a DIFFERENT amount per channel. Flat
 * fills then look faintly speckled and antialiased edges look grainy, because
 * the blend is walking through colours the panel cannot produce. Every value
 * below was snapped to the nearest one it can, so what is asked for is what is
 * shown.
 *
 * VOID is true black on purpose: an unlit AMOLED pixel emits nothing at all,
 * which is both the deepest contrast available and the only shade with no
 * quantisation noise whatsoever. */
/* THE FIELD IS TRUE BLACK, AND ON THIS PANEL THAT IS NOT A STYLE CHOICE.
 *
 * The face used to sit on a "lifted" disc of 0x081C29 - a very dark teal - to
 * read as an instrument rather than as text floating in space. On an LCD that
 * is a reasonable trick. On an AMOLED it throws away the one thing the panel
 * does better than anything else: an unlit pixel emits NOTHING, which is both
 * infinite contrast and zero power.
 *
 * It also looked bad in a way that only a photograph of the real device
 * revealed. 0x081C29 in RGB565 is (1, 7, 5) out of (31, 63, 31) - so few
 * levels that neighbouring shades of it band visibly, and a whole 462 px disc
 * of it photographs as a flat blue wash with the arcs sitting in it like
 * holes. What was meant to read as a machined instrument face read as a
 * screen with the brightness wrong.
 *
 * Black field, lit rim, lit content. Everything that emits light now means
 * something. */
#define HUD_VOID   0x000000u
#define HUD_FIELD  0x000000u /* true black: the panel's best feature  */

/* The chrome comes from the chosen theme; see pharos_theme.h. These stay
 * macros so that every call site below reads exactly as it did when they were
 * constants - the point of the change is that the VALUES move, not the code. */
#define HUD_RIM    (pharos_theme()->rim)
#define HUD_TRACK  (pharos_theme()->track)
#define HUD_TRACK2 (pharos_theme()->track2)
#define HUD_PIP_ON (pharos_theme()->pip_on)
#define HUD_PIP_UP (pharos_theme()->pip_up)
#define HUD_TEXT   (pharos_theme()->text)
#define HUD_DIM    (pharos_theme()->dim)
#define HUD_DIMMER (pharos_theme()->dimmer)
#define HUD_DENIED (pharos_theme()->denied)
#define HUD_GHOST  (pharos_theme()->ghost)
#define HUD_CYAN   (pharos_theme()->accent)

/* The verdict. Fixed, in every theme, deliberately: the tone contract is the
 * reason a red means the same thing on the Census page as on the Watch page,
 * and a palette that could restyle danger would be a way to make the device
 * lie quietly. */
#define HUD_AMBER  0xFFC34Au
#define HUD_ORANGE 0xEF9239u
#define HUD_RED    0xE75142u
#define HUD_GREEN  0x39DB84u

/* The same thresholds the renderer uses, so a screenshot and the panel agree
 * about what a number means. */
static uint32_t band_colour(int score)
{
    if (score >= 75) return HUD_RED;
    if (score >= 60) return HUD_ORANGE;
    if (score >= 40) return HUD_AMBER;
    if (score >= 20) return HUD_CYAN;
    return HUD_DIM;
}

/* ---- the widget set --------------------------------------------------
 *
 * Two page containers. Only these two are ever shown or hidden, and only when
 * the view actually changes - which is what stops the per-frame show/hide
 * thrash that made the old face strobe. */
static lv_obj_t *s_page_browse;
static lv_obj_t *s_page_live;

/* shared chrome, parented to the screen and visible in both views */
static lv_obj_t *s_field;
static lv_obj_t *s_ticks;
static lv_obj_t *s_pip;
static lv_obj_t *s_toast;

/* live view */
static lv_obj_t *s_track;
static lv_obj_t *s_arc;
static lv_obj_t *s_ghost;   /* the score the caps took away, thin and inset */
static lv_obj_t *s_ceiling; /* the hard stop, a tick across the arc         */
static lv_obj_t *s_core;
static lv_obj_t *s_ctx;     /* channel / posture / network name */
static lv_obj_t *s_big;
static lv_obj_t *s_band;
static lv_obj_t *s_detail;
static lv_obj_t *s_why;
static lv_obj_t *s_peak;
static lv_obj_t *s_sim;   /* the SIMULATION banner */
static lv_obj_t *s_bar[PHAROS_DISP_HISTORY];
static lv_obj_t *s_bar_base;
static lv_obj_t *s_fam_box[PHAROS_DISP_FAMILIES];
static lv_obj_t *s_fam_txt[PHAROS_DISP_FAMILIES];

/* detail view: the lens' own evidence as a list */
/* ---- the home ring ---- */
static lv_obj_t *s_page_home;
static lv_obj_t *s_h_dot[PHAROS_HUD_HOME_MAX];
static lv_obj_t *s_h_lbl[PHAROS_HUD_HOME_MAX];
static lv_obj_t *s_h_core;
static lv_obj_t *s_h_clock;
static lv_obj_t *s_h_head;
static lv_obj_t *s_h_sub;
static lv_obj_t *s_h_arc;
static lv_obj_t *s_h_ring;
static lv_obj_t *s_home_hint;
static lv_obj_t *s_h_tick[PHAROS_HUD_HOME_MAX];
static lv_point_t s_h_pos[PHAROS_HUD_HOME_MAX];
static unsigned s_h_n;
static unsigned s_h_laid;
static int16_t s_h_laid_w; /* the label width it was laid out for */

static lv_obj_t *s_page_detail;
static lv_obj_t *s_d_title;
static lv_obj_t *s_d_hl;
static lv_obj_t *s_d_hr;
static lv_obj_t *s_d_left[PHAROS_HUD_ROWS];
static lv_obj_t *s_d_right[PHAROS_HUD_ROWS];
static lv_obj_t *s_d_rule[PHAROS_HUD_ROWS];
static lv_obj_t *s_d_page;
static lv_obj_t *s_d_empty;

/* browse view */
static lv_obj_t *s_b_name;
static lv_obj_t *s_b_pos;
static lv_obj_t *s_b_summary;
static lv_obj_t *s_b_hint;
static lv_obj_t *s_b_arc;

static lv_obj_t *s_d_pill[PHAROS_HUD_ROWS]; /* the focus highlight */
static lv_obj_t *s_d_hint;                  /* "hold to go back"   */
static lv_obj_t *s_d_up;                    /* the page-up cue     */
static lv_obj_t *s_zone_main[4];            /* browse / live       */
static lv_obj_t *s_zone_row[PHAROS_HUD_ROWS];
static lv_obj_t *s_zone_page[2];            /* detail: up / down   */
static int s_zones_detail = -1;             /* which set is live   */

static bool s_built;
static pharos_hud_nav_cb_t s_nav_cb;
static pharos_hud_row_cb_t s_row_cb;
static pharos_hud_home_cb_t s_home_cb;
static uint32_t s_toast_until_ms;

/* ---- dirty checks ----------------------------------------------------
 *
 * The reason the panel is quiet. lv_label_set_text() marks the object dirty
 * whether or not the string differs, and an invalidated region is a region
 * that gets redrawn and flushed over QSPI. At 5 Hz across eight labels and
 * three arcs that is a continuous rolling repaint of the whole face, which is
 * exactly what "it flickers" looks like from the outside.
 *
 * Comparing first turns a steady reading into zero invalidations. */
static void set_text(lv_obj_t *o, const char *s)
{
    if (!o) {
        return;
    }
    if (!s) {
        s = "";
    }
    const char *cur = lv_label_get_text(o);
    if (cur && strcmp(cur, s) == 0) {
        return;
    }
    lv_label_set_text(o, s);
}

static void set_text_colour(lv_obj_t *o, uint32_t rgb)
{
    if (!o) {
        return;
    }
    const lv_color_t want = lv_color_hex(rgb);
    const lv_color_t cur = lv_obj_get_style_text_color(o, LV_PART_MAIN);
    if (lv_color_eq(cur, want)) {
        return;
    }
    lv_obj_set_style_text_color(o, want, 0);
}

/* LVGL drives this, not the UI loop. */
static void arc_anim_cb(void *obj, int32_t v)
{
    lv_arc_set_value((lv_obj_t *)obj, v);
}

/* THE NEEDLE GLIDES.
 *
 * The UI loop hands over a fresh reading ten times a second. Setting the arc
 * directly made it step ten times a second, which is what "not smooth" looks
 * like - and the obvious fix, running the loop faster, measurably starved the
 * analytics tick that does the actual detecting.
 *
 * So LVGL interpolates instead. It is already awake at its own refresh rate to
 * composite the screen; animating between the last value and the new one costs
 * this project nothing and runs at the panel's rate rather than the loop's.
 *
 * A big jump is deliberately not slowed down to match: an arc that took a
 * leisurely second to swing up to an alarm would be prettier and later. The
 * duration is capped so the display never lags the finding by more than a
 * couple of frames' worth of travel. */
static void set_arc_value(lv_obj_t *o, int v)
{
    if (!o) {
        return;
    }
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    const int32_t cur = lv_arc_get_value(o);
    if (cur == v) {
        return;
    }
    const int32_t delta = (cur > v) ? (cur - v) : (v - cur);
    if (delta <= 1) {
        lv_arc_set_value(o, v); /* not worth an animation object */
        return;
    }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, o);
    lv_anim_set_exec_cb(&a, arc_anim_cb);
    lv_anim_set_values(&a, cur, v);
    /* Roughly one paint interval of travel, longer for a big swing but never
     * so long that the glass is telling you about a reading that has been
     * superseded twice. */
    lv_anim_set_duration(&a, (uint32_t)(delta > 40 ? 260 : 160));
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

/* The ceiling tick, placed by absolute angle within the 270-degree sweep. */
static int s_ceiling_at = -1;
static void set_ceiling_tick(int ceiling)
{
    if (!s_ceiling) {
        return;
    }
    if (ceiling < 0) ceiling = 0;
    if (ceiling > 100) ceiling = 100;
    if (s_ceiling_at == ceiling) {
        return;
    }
    s_ceiling_at = ceiling;
    if (ceiling == 0) {
        lv_arc_set_angles(s_ceiling, 0, 0);
        return;
    }
    const int32_t a = (270 * ceiling) / 100;
    const int32_t end = (a + 2 > 270) ? 270 : a + 2;
    lv_arc_set_angles(s_ceiling, a > 0 ? a - 1 : 0, end);
}

static void set_arc_colour(lv_obj_t *o, uint32_t rgb)
{
    if (!o) {
        return;
    }
    const lv_color_t want = lv_color_hex(rgb);
    const lv_color_t cur = lv_obj_get_style_arc_color(o, LV_PART_INDICATOR);
    if (lv_color_eq(cur, want)) {
        return;
    }
    lv_obj_set_style_arc_color(o, want, LV_PART_INDICATOR);
}

static void set_bg_colour(lv_obj_t *o, uint32_t rgb)
{
    if (!o) {
        return;
    }
    const lv_color_t want = lv_color_hex(rgb);
    const lv_color_t cur = lv_obj_get_style_bg_color(o, LV_PART_MAIN);
    if (lv_color_eq(cur, want)) {
        return;
    }
    lv_obj_set_style_bg_color(o, want, 0);
}

/* The one place a lens' stated tone becomes a colour, so that a red on the
 * Census page means what a red on the Watch page means. */
static uint32_t tone_colour(pharos_tone_t t)
{
    switch (t) {
    case PHAROS_TONE_GOOD: return HUD_GREEN;
    case PHAROS_TONE_WARN: return HUD_AMBER;
    case PHAROS_TONE_BAD:  return HUD_RED;
    case PHAROS_TONE_DIM:  return HUD_DIMMER;
    case PHAROS_TONE_NEUTRAL:
    default:               return HUD_TEXT;
    }
}

static void show(lv_obj_t *o, bool on)
{
    if (!o) {
        return;
    }
    const bool hidden = lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN);
    if (on == !hidden) {
        return; /* already in the wanted state: touch nothing */
    }
    if (on) {
        lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ---- construction helpers -------------------------------------------- */

/* A plain, non-interactive container. lv_obj_create() hands back something
 * scrollable and clickable with a border and a background; every one of those
 * defaults is wrong for a HUD element, and the scrollable one is what let a
 * stray drag push the whole face off the glass. */
static lv_obj_t *mk_box(lv_obj_t *parent)
{
    lv_obj_t *o = lv_obj_create(parent);
    lv_obj_remove_style_all(o);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(o, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_set_scrollbar_mode(o, LV_SCROLLBAR_MODE_OFF);
    return o;
}

static lv_obj_t *mk_label(lv_obj_t *parent, const lv_font_t *font, uint32_t rgb,
                          int dx, int dy, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(rgb), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    /* No wrapping, ever. A label whose height changes with its text re-lays
     * out, and a re-layout moves the invalidated region around underneath the
     * arc. Long strings are truncated by the caller against the real chord
     * width instead. */
    lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
    lv_label_set_text(l, text);
    lv_obj_align(l, LV_ALIGN_CENTER, dx, dy);
    return l;
}

/* A gauge arc: 270 degrees opening at the bottom, and emphatically NOT an
 * input. An lv_arc is a slider by default - it tracks your finger - and this
 * one reports a confidence score, so being draggable would let a fingertip
 * overwrite a measurement. */
static lv_obj_t *mk_arc(lv_obj_t *parent, int size, int width, uint32_t rgb,
                        int value)
{
    lv_obj_t *a = lv_arc_create(parent);
    lv_obj_set_size(a, size, size);
    lv_obj_center(a);
    lv_arc_set_rotation(a, 135);
    lv_arc_set_bg_angles(a, 0, 270);
    lv_arc_set_range(a, 0, 100);
    lv_arc_set_value(a, value);
    lv_arc_set_change_rate(a, 0);
    lv_obj_remove_style(a, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(a, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(a, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(a, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_set_style_arc_opa(a, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_width(a, width, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(a, true, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(a, lv_color_hex(rgb), LV_PART_INDICATOR);
    return a;
}

/* ---- touch ----------------------------------------------------------- */

/* WHERE THE FINGER ACTUALLY LANDED.
 *
 * "Sometimes I am not able to click them correctly" has two possible causes
 * and they need opposite fixes: targets too small for a finger, or a touch
 * controller reporting presses nobody made. Guessing between them is how you
 * spend a day making the wrong thing bigger. Every press is logged with its
 * coordinates, so the log answers it. */
static void log_touch(const char *what, unsigned tag)
{
    lv_indev_t *in = lv_indev_active();
    lv_point_t p = { -1, -1 };
    if (in) {
        lv_indev_get_point(in, &p);
    }
    ESP_LOGI("touch", "%s %u at %d,%d", what, tag, (int)p.x, (int)p.y);
}

static void row_event(lv_event_t *e)
{
    const lv_event_code_t code = lv_event_get_code(e);
    const unsigned row = (unsigned)(uintptr_t)lv_event_get_user_data(e);
    log_touch(code == LV_EVENT_LONG_PRESSED ? "hold row" : "tap row", row);
    /* A long press anywhere still means BACK - including on a row, because
     * that is where the finger already is when somebody wants out. */
    if (code == LV_EVENT_LONG_PRESSED) {
        if (s_nav_cb) {
            s_nav_cb(PHAROS_NAV_HOME);
        }
        return;
    }
    if (code == LV_EVENT_SHORT_CLICKED && s_row_cb) {
        s_row_cb(row);
    }
}

void pharos_hud_set_row_cb(pharos_hud_row_cb_t cb) { s_row_cb = cb; }
void pharos_hud_set_home_cb(pharos_hud_home_cb_t cb) { s_home_cb = cb; }

static void nav_event(lv_event_t *e)
{
    if (!s_nav_cb) {
        return;
    }
    const lv_event_code_t code = lv_event_get_code(e);
    const pharos_nav_t what = (pharos_nav_t)(uintptr_t)lv_event_get_user_data(e);
    log_touch(code == LV_EVENT_LONG_PRESSED ? "hold zone" : "tap zone",
              (unsigned)what);

    /* On the ring, a short press is aimed at a DOT, and only falls through to
     * the zone's own meaning when it lands near none of them. */
    if (code == LV_EVENT_SHORT_CLICKED && s_home_cb && s_page_home &&
        !lv_obj_has_flag(s_page_home, LV_OBJ_FLAG_HIDDEN)) {
        lv_indev_t *in = lv_indev_active();
        lv_point_t p;
        if (in) {
            lv_indev_get_point(in, &p);
            const int hit = pharos_hud_home_hit((int16_t)p.x, (int16_t)p.y);
            if (hit >= 0) {
                s_home_cb((unsigned)hit);
                return;
            }
        }
    }
    /* Record the intent and return. See the header: doing the real work here
     * runs it on LVGL's task, with LVGL's stack, and reboots the board. */
    if (code == LV_EVENT_LONG_PRESSED) {
        s_nav_cb(PHAROS_NAV_HOME);
    } else if (code == LV_EVENT_SHORT_CLICKED) {
        s_nav_cb(what);
    }
}

static lv_obj_t *mk_zone_at(lv_obj_t *parent, lv_align_t align, int x, int y,
                            int w, int h, lv_event_cb_t cb, void *tag)
{
    lv_obj_t *z = lv_obj_create(parent);
    lv_obj_remove_style_all(z);
    lv_obj_set_size(z, w, h);
    lv_obj_align(z, align, x, y);
    lv_obj_remove_flag(z, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(z, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(z, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(z, LV_OPA_TRANSP, 0);
    lv_obj_add_event_cb(z, cb, LV_EVENT_SHORT_CLICKED, tag);
    lv_obj_add_event_cb(z, cb, LV_EVENT_LONG_PRESSED, tag);
    return z;
}

/* EXACTLY ONE PAGE IS UP.
 *
 * Each page function used to hide the others by name, which meant every new
 * page had to be added to every existing function. A fourth page was added -
 * the home ring - and three of those lists were not updated, so the ring
 * stayed visible UNDERNEATH the lens you opened and the two faces drew on top
 * of each other. Reported as "the UI of the home screen and the sensor were
 * merging", which is exactly what it was.
 *
 * There is now one list, and it is derived: naming the page you want hides
 * every other page by construction. A fifth page cannot reintroduce this. */
typedef enum {
    PAGE_HOME = 0,
    PAGE_BROWSE,
    PAGE_LIVE,
    PAGE_DETAIL,
} hud_page_t;

static void page_show(hud_page_t want)
{
    show(s_page_home, want == PAGE_HOME);

    /* THE HINT HAS TO BE TRUE ON THE PAGE IT IS SHOWN ON.
     *
     * A hold is "back one step", not "go home": from a detail page it lands on
     * the live face, and only from there does it reach the ring. A single
     * "hold for home" everywhere would be wrong on exactly the pages somebody
     * most needs it to be right. */
    show(s_home_hint, want != PAGE_HOME);
    if (want == PAGE_DETAIL) {
        set_text(s_home_hint, "hold to go back");
    } else {
        set_text(s_home_hint, "hold for the home ring");
    }
    show(s_page_browse, want == PAGE_BROWSE);
    show(s_page_live, want == PAGE_LIVE);
    show(s_page_detail, want == PAGE_DETAIL);
}

/* WHICH SET OF TARGETS IS LIVE.
 *
 * The browse and live faces want three columns and a bottom strip. The detail
 * page wants one target per row, full width, plus two big page controls -
 * which overlap the columns, so the two sets cannot both be listening. The
 * page that is being drawn says which it needs, and the HUD does not have to
 * be told what view it is in. */
static void zones_mode(bool detail)
{
    if (s_zones_detail == (int)detail || !s_built) {
        return;
    }
    s_zones_detail = (int)detail;
    for (unsigned i = 0; i < 4; i++) {
        show(s_zone_main[i], !detail);
    }
    for (unsigned i = 0; i < PHAROS_HUD_ROWS; i++) {
        show(s_zone_row[i], detail);
    }
    for (unsigned i = 0; i < 2; i++) {
        show(s_zone_page[i], detail);
    }
}

void pharos_hud_set_nav_cb(pharos_hud_nav_cb_t cb) { s_nav_cb = cb; }

bool pharos_hud_present(void) { return s_built; }

/* ---- the activity ribbon ---------------------------------------------
 *
 * Sixteen radial bars across the top of the dial, one per second, height
 * proportional to that second's activity. A score says whether something is
 * happening; this says what shape it is. A steady trickle and one violent
 * burst produce the same ten-second mean and are not the same event.
 *
 * Each bar is an lv_line with its own two-point array. The arrays are static
 * because LVGL keeps the pointer rather than copying, and updating a bar
 * invalidates only its own few pixels. */
#define BAR_BASE_R 184
#define BAR_MAX_H  34
#define BAR_SPAN   140.0f

static lv_point_precise_t s_bar_pts[PHAROS_DISP_HISTORY][2];
static uint8_t s_bar_level[PHAROS_DISP_HISTORY];
static uint32_t s_bar_rgb[PHAROS_DISP_HISTORY];

static void bar_set(unsigned i, uint8_t level, uint32_t rgb)
{
    if (i >= PHAROS_DISP_HISTORY || !s_bar[i]) {
        return;
    }
    const uint32_t want = level ? rgb : HUD_DENIED;
    if (s_bar_rgb[i] != want) {
        s_bar_rgb[i] = want;
        lv_obj_set_style_line_color(s_bar[i], lv_color_hex(want), 0);
    }
    if (s_bar_level[i] == level) {
        return; /* same height: no geometry to rewrite, nothing invalidated */
    }
    s_bar_level[i] = level;

    const float a = -BAR_SPAN / 2.0f +
                    BAR_SPAN * ((float)i + 0.5f) / (float)PHAROS_DISP_HISTORY;
    const int h = ((int)level * BAR_MAX_H) / 255;
    const pr_point_t p0 = pr_polar((int16_t)BAR_BASE_R, a);
    const pr_point_t p1 = pr_polar((int16_t)(BAR_BASE_R + (h > 5 ? h : 5)), a);

    /* An lv_line's points are relative to the object, and an object with
     * LV_SIZE_CONTENT sizes itself to the largest point - so points carrying
     * their full screen offset would give every bar a bounding box reaching
     * back to the top-left corner. Changing one bar would then invalidate most
     * of the panel, which is the exact cost this whole file exists to avoid.
     *
     * Anchor each bar at its own bounding box instead and keep the points
     * local, so a bar redraws a few dozen pixels and nothing else. */
    const int16_t minx = (p0.x < p1.x) ? p0.x : p1.x;
    const int16_t miny = (p0.y < p1.y) ? p0.y : p1.y;

    s_bar_pts[i][0].x = p0.x - minx;
    s_bar_pts[i][0].y = p0.y - miny;
    s_bar_pts[i][1].x = p1.x - minx;
    s_bar_pts[i][1].y = p1.y - miny;
    lv_obj_set_pos(s_bar[i], minx, miny);
    lv_line_set_points(s_bar[i], s_bar_pts[i], 2);
}

/* ---- construction ---------------------------------------------------- */

bool pharos_hud_create(void)
{
    if (s_built) {
        return true;
    }
    lv_obj_t *scr = lv_screen_active();
    if (!scr) {
        return false;
    }

    /* THE FLAG. The screen itself is scrollable by default and the content
     * deliberately extends past its bounds, so without this a drag slides the
     * whole HUD away permanently. */
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);

    /* AMOLED: an unlit pixel costs no power, so the field is near-black. */
    lv_obj_set_style_bg_color(scr, lv_color_hex(HUD_VOID), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);

    /* ---- shared chrome, under both pages ---- */

    /* The instrument face: a slightly lifted disc inside the void, with a rim
     * line. This is what makes it read as an instrument rather than as text on
     * a black background. */
    /* Black disc, lit rim. The disc still exists so the face has a defined
     * edge and so a repaint has something to clear to, but it emits nothing. */
    s_field = mk_box(scr);
    lv_obj_set_size(s_field, 462, 462);
    lv_obj_center(s_field);
    lv_obj_set_style_radius(s_field, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_field, lv_color_hex(HUD_FIELD), 0);
    lv_obj_set_style_bg_opa(s_field, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_field, lv_color_hex(HUD_RIM), 0);
    lv_obj_set_style_border_width(s_field, 2, 0);

    /* 24 bezel ticks, every sixth long and cyan - the instrument's compass. */
    s_ticks = lv_scale_create(scr);
    lv_obj_set_size(s_ticks, 440, 440);
    lv_obj_center(s_ticks);
    lv_obj_remove_flag(s_ticks, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_ticks, LV_OBJ_FLAG_CLICKABLE);
    lv_scale_set_mode(s_ticks, LV_SCALE_MODE_ROUND_INNER);
    lv_scale_set_total_tick_count(s_ticks, 25);
    lv_scale_set_major_tick_every(s_ticks, 6);
    lv_scale_set_range(s_ticks, 0, 24);
    lv_scale_set_angle_range(s_ticks, 360);
    lv_scale_set_rotation(s_ticks, 270);
    lv_scale_set_label_show(s_ticks, false);
    lv_obj_set_style_length(s_ticks, 7, LV_PART_ITEMS);
    lv_obj_set_style_line_width(s_ticks, 2, LV_PART_ITEMS);
    lv_obj_set_style_line_color(s_ticks, lv_color_hex(HUD_TRACK), LV_PART_ITEMS);
    lv_obj_set_style_length(s_ticks, 12, LV_PART_INDICATOR);
    lv_obj_set_style_line_width(s_ticks, 3, LV_PART_INDICATOR);
    lv_obj_set_style_line_color(s_ticks, lv_color_hex(HUD_CYAN), LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(s_ticks, LV_OPA_TRANSP, LV_PART_MAIN);

    /* The permanent receive-only tell, on the bezel where it is always true. */
    s_pip = mk_box(scr);
    lv_obj_set_size(s_pip, 8, 8);
    lv_obj_align(s_pip, LV_ALIGN_CENTER, 0, 200);
    lv_obj_set_style_radius(s_pip, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_pip, lv_color_hex(HUD_GREEN), 0);
    lv_obj_set_style_bg_opa(s_pip, LV_OPA_COVER, 0);

    /* ---- the LIVE page ---- */

    s_page_live = mk_box(scr);
    lv_obj_set_size(s_page_live, PR_W, PR_H);
    lv_obj_center(s_page_live);

    /* Ribbon baseline, so a quiet stretch reads as a timeline with nothing on
     * it rather than as a rendering fault. */
    /* LVGL measures arc angles clockwise from 3 o'clock, so 12 o'clock is 270.
     * Centring a BAR_SPAN-wide arc there starts it at 270 - BAR_SPAN/2. */
    s_bar_base = mk_arc(s_page_live, (BAR_BASE_R - 4) * 2, 2, HUD_DENIED, 100);
    lv_arc_set_rotation(s_bar_base, 270 - (int32_t)(BAR_SPAN / 2.0f));
    lv_arc_set_bg_angles(s_bar_base, 0, (int32_t)BAR_SPAN);

    for (unsigned i = 0; i < PHAROS_DISP_HISTORY; i++) {
        s_bar[i] = lv_line_create(s_page_live);
        lv_obj_remove_flag(s_bar[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(s_bar[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_line_width(s_bar[i], 7, 0);
        lv_obj_set_style_line_rounded(s_bar[i], true, 0);
        lv_obj_set_style_line_color(s_bar[i], lv_color_hex(HUD_DENIED), 0);
        s_bar_level[i] = 0xFF; /* force the first write through */
        s_bar_rgb[i] = 0xFFFFFFFFu;
        bar_set(i, 0, HUD_DENIED);
    }

    /* Track, then the capped-away ghost, then the score. */
    s_track = mk_arc(s_page_live, 332, 22, HUD_TRACK, 100);
    s_ghost = mk_arc(s_page_live, 286, 5, HUD_GHOST, 0);
    s_arc = mk_arc(s_page_live, 332, 18, HUD_CYAN, 0);

    /* The ceiling: a hard stop drawn as a short TICK across the arc, not as a
     * second band of colour. The old face drew it as a fat arc behind the
     * score, where two bars of the same weight showing different numbers read
     * as a bug rather than as a limit.
     *
     * A tick means driving the indicator's angles directly rather than through
     * a value - lv_arc_set_value() would fill everything from the start of the
     * sweep to that point, which is the band we are trying not to draw. */
    s_ceiling = mk_arc(s_page_live, 332, 24, HUD_RED, 0);
    lv_obj_set_style_arc_rounded(s_ceiling, false, LV_PART_INDICATOR);
    lv_arc_set_angles(s_ceiling, 0, 0);

    /* The dark disc the headline sits on, so the number never competes with
     * the gauge behind it. */
    s_core = mk_box(s_page_live);
    lv_obj_set_size(s_core, 192, 192);
    lv_obj_center(s_core);
    lv_obj_set_style_radius(s_core, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_core, lv_color_hex(HUD_VOID), 0);
    lv_obj_set_style_bg_opa(s_core, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_core, lv_color_hex(HUD_TRACK), 0);
    lv_obj_set_style_border_width(s_core, 2, 0);

    /* Sizes are the ones the panel actually has compiled in (12/16/18/20/22/
     * 24/26/48 - there is no 14). Everything that was 12 px is now 16: on a
     * 1.75 inch 466 px circle, 12 px is about 1.6 mm of cap height and it
     * reads as pixel mush at arm's length, which is exactly the "not sharp"
     * the operator reported. */
    /* The drill banner. Amber, above the reading, and impossible to miss -
     * it exists because a training lens showed "FLOOD LIKELY 77" and was read
     * as a real attack on a real building. */
    s_sim    = mk_label(s_page_live, &lv_font_montserrat_16, HUD_AMBER, 0, -96,
                        "SIMULATION - NOT THIS ROOM");
    lv_obj_add_flag(s_sim, LV_OBJ_FLAG_HIDDEN);
    s_ctx    = mk_label(s_page_live, &lv_font_montserrat_16, HUD_DIMMER, 0,  -54, "");
    s_big    = mk_label(s_page_live, &lv_font_montserrat_48, HUD_TEXT,   0,   -4, "--");
    s_band   = mk_label(s_page_live, &lv_font_montserrat_22, HUD_CYAN,   0,   46, "");
    s_detail = mk_label(s_page_live, &lv_font_montserrat_16, HUD_DIM,    0,   90, "");
    s_why    = mk_label(s_page_live, &lv_font_montserrat_16, HUD_DIM,    0,  152, "");
    /* The ribbon's scale label was "16s", which told the operator nothing they
     * could act on and put a third tiny string in the busiest part of the
     * face. The ribbon is a shape, and a shape does not need a legend. */
    s_peak   = NULL;

    /* The evidence pips: four labelled boxes, not four anonymous dots. Naming
     * them is the whole improvement - "why does it think so" becomes
     * answerable from the glass instead of from the manual. */
    {
        const int w = 80, gap = 6, h = 30;
        const int total = 4 * w + 3 * gap;
        for (unsigned i = 0; i < PHAROS_DISP_FAMILIES; i++) {
            const int dx = -total / 2 + (int)i * (w + gap) + w / 2;
            s_fam_box[i] = mk_box(s_page_live);
            lv_obj_set_size(s_fam_box[i], w, h);
            lv_obj_align(s_fam_box[i], LV_ALIGN_CENTER, dx, 118);
            lv_obj_set_style_radius(s_fam_box[i], 8, 0);
            lv_obj_set_style_bg_color(s_fam_box[i], lv_color_hex(HUD_PIP_UP), 0);
            lv_obj_set_style_bg_opa(s_fam_box[i], LV_OPA_COVER, 0);
            /* A hairline so an unlit pip is still a SLOT rather than a void -
             * four holes in black read as nothing at all. */
            lv_obj_set_style_border_color(s_fam_box[i], lv_color_hex(HUD_TRACK), 0);
            lv_obj_set_style_border_width(s_fam_box[i], 1, 0);
            s_fam_txt[i] = mk_label(s_page_live, &lv_font_montserrat_16,
                                    HUD_DENIED, dx, 118, "");
        }
    }

    /* ---- the BROWSE page ---- */

    s_page_browse = mk_box(scr);
    lv_obj_set_size(s_page_browse, PR_W, PR_H);
    lv_obj_center(s_page_browse);

    s_b_arc = mk_arc(s_page_browse, 332, 16, HUD_CYAN, 0);
    s_b_name = mk_label(s_page_browse, &lv_font_montserrat_26, HUD_TEXT, 0, -60, "");
    s_b_pos  = mk_label(s_page_browse, &lv_font_montserrat_18, HUD_DIMMER, 0, -22, "");
    /* The one place a wrap is worth its cost: this text is static for as long
     * as the cursor sits on a lens, so it re-lays-out on a cursor move and
     * never during a repaint. */
    s_b_summary = lv_label_create(s_page_browse);
    lv_obj_remove_flag(s_b_summary, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_text_font(s_b_summary, &lv_font_montserrat_18, 0);
    lv_obj_set_style_text_color(s_b_summary, lv_color_hex(HUD_DIM), 0);
    lv_obj_set_style_text_align(s_b_summary, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_b_summary, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_b_summary, 300);
    lv_label_set_text(s_b_summary, "");
    lv_obj_align(s_b_summary, LV_ALIGN_CENTER, 0, 40);
    s_b_hint = mk_label(s_page_browse, &lv_font_montserrat_16, HUD_DIMMER, 0, 130,
                        "tap centre to start");

    /* ---- the DETAIL page ---- */

    s_page_detail = mk_box(scr);
    lv_obj_set_size(s_page_detail, PR_W, PR_H);
    lv_obj_center(s_page_detail);

    /* ---- column geometry, and why it is stated rather than eyeballed ----
     *
     * The first version gave the labels no explicit width, so LVGL sized each
     * one to its content - 26 characters is 229 px at this font - and then
     * centred that on dx = -110. The left edge landed at -224 while the glass
     * only allows +/-204 at the top and bottom rows, so the first characters
     * of EVERY row were cut off by the curve. On a round screen a label with
     * no width is a label whose position you do not actually control.
     *
     * So: fix the block to 340 px wide, which keeps |x| <= 170 and clears the
     * narrowest row (chord half-width 204 at y = +/-92) with room to spare.
     * The left column is width-bounded and left-aligned so names line up and
     * can be read down the page; the right column is right-aligned so the
     * values do too. */
    const int COL_W_L = 232;      /* ~26 chars at montserrat_16 */
    const int COL_W_R = 100;      /* ~11 chars                  */
    const int BLOCK_HALF = 170;   /* |x| never exceeds this     */
    const int CX_L = -BLOCK_HALF + COL_W_L / 2;      /* -54 */
    const int CX_R = BLOCK_HALF - COL_W_R / 2;       /* +120 */

    s_d_title = mk_label(s_page_detail, &lv_font_montserrat_18, HUD_TEXT, 0, -172, "");

    s_d_hl = mk_label(s_page_detail, &lv_font_montserrat_12, HUD_DIMMER, CX_L, -142, "");
    lv_obj_set_width(s_d_hl, COL_W_L);
    lv_obj_set_style_text_align(s_d_hl, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_align(s_d_hl, LV_ALIGN_CENTER, CX_L, -142);

    s_d_hr = mk_label(s_page_detail, &lv_font_montserrat_12, HUD_DIMMER, CX_R, -142, "");
    lv_obj_set_width(s_d_hr, COL_W_R);
    lv_obj_set_style_text_align(s_d_hr, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(s_d_hr, LV_ALIGN_CENTER, CX_R, -142);

    /* Four rows on a 58 px pitch, centred on the face: -87, -29, +29, +87. */
    for (unsigned i = 0; i < PHAROS_HUD_ROWS; i++) {
        const int y = -87 + (int)i * PHAROS_HUD_ROW_PITCH;

        /* THE FOCUS PILL, under the text.
         *
         * A brightened hairline was the whole of the focus indication, which
         * is a 1 px cue for a state that decides what the next press does.
         * A filled rounded block behind the row cannot be missed, and it is
         * the same shape as the target the finger is aiming at - so the thing
         * you see and the thing you press are one object. */
        s_d_pill[i] = mk_box(s_page_detail);
        lv_obj_set_size(s_d_pill[i], BLOCK_HALF * 2 + 16, PHAROS_HUD_ROW_PITCH - 8);
        lv_obj_align(s_d_pill[i], LV_ALIGN_CENTER, 0, y);
        lv_obj_set_style_radius(s_d_pill[i], (PHAROS_HUD_ROW_PITCH - 8) / 2, 0);
        lv_obj_set_style_bg_color(s_d_pill[i], lv_color_hex(HUD_PIP_ON), 0);
        lv_obj_set_style_bg_opa(s_d_pill[i], LV_OPA_COVER, 0);
        lv_obj_add_flag(s_d_pill[i], LV_OBJ_FLAG_HIDDEN);

        s_d_left[i] = mk_label(s_page_detail, &lv_font_montserrat_16, HUD_TEXT,
                               CX_L, y, "");
        lv_obj_set_width(s_d_left[i], COL_W_L);
        lv_obj_set_style_text_align(s_d_left[i], LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_align(s_d_left[i], LV_ALIGN_CENTER, CX_L, y);

        s_d_right[i] = mk_label(s_page_detail, &lv_font_montserrat_16, HUD_TEXT,
                                CX_R, y, "");
        lv_obj_set_width(s_d_right[i], COL_W_R);
        lv_obj_set_style_text_align(s_d_right[i], LV_TEXT_ALIGN_RIGHT, 0);
        lv_obj_align(s_d_right[i], LV_ALIGN_CENTER, CX_R, y);

        /* A hairline BETWEEN rows. Sized to the block, not the screen. */
        s_d_rule[i] = mk_box(s_page_detail);
        lv_obj_set_size(s_d_rule[i], BLOCK_HALF * 2, 1);
        lv_obj_align(s_d_rule[i], LV_ALIGN_CENTER, 0,
                     y + PHAROS_HUD_ROW_PITCH / 2);
        lv_obj_set_style_bg_color(s_d_rule[i], lv_color_hex(HUD_TRACK2), 0);
        lv_obj_set_style_bg_opa(s_d_rule[i], LV_OPA_COVER, 0);
    }

    /* THE TWO BIGGEST TARGETS ON THE DEVICE, MARKED.
     *
     * Everything above the rows pages back and everything below pages on -
     * about 117 px of glass each, which is the most forgiving control here.
     * Unmarked, it is also the least discoverable: nothing suggests the empty
     * band under a list does anything. A chevron at each end costs one glyph
     * and turns a hidden gesture into a button. */
    /* A short accent rule under the title. The header is three separate
     * things stacked - lens name, then column headings, then the list - and
     * without a line between them the eye reads the headings as the first
     * row. One rule, the width of a word, fixes that. */
    {
        lv_obj_t *u = mk_box(s_page_detail);
        lv_obj_set_size(u, 96, 2);
        lv_obj_align(u, LV_ALIGN_CENTER, 0, -156);
        lv_obj_set_style_radius(u, 1, 0);
        lv_obj_set_style_bg_color(u, lv_color_hex(HUD_CYAN), 0);
        lv_obj_set_style_bg_opa(u, LV_OPA_60, 0);
    }

    s_d_up    = mk_label(s_page_detail, &lv_font_montserrat_16, HUD_DIMMER, 0, -196, "");
    s_d_page  = mk_label(s_page_detail, &lv_font_montserrat_16, HUD_DIMMER, 0, 146, "");
    s_d_hint  = mk_label(s_page_detail, &lv_font_montserrat_12, HUD_DIMMER, 0, 176, "");
    s_d_empty = mk_label(s_page_detail, &lv_font_montserrat_18, HUD_DIM, 0, 0, "");

    /* ---- the home ring ----
     *
     * Twelve dots at most, laid out by pd_dial_layout() so the spacing is the
     * host-tested geometry rather than a second copy of it here - and so that
     * pd_dial_hit() can answer "which one did they press" from the same
     * numbers that drew it. */
    s_page_home = mk_box(scr);
    lv_obj_set_size(s_page_home, 466, 466);
    lv_obj_center(s_page_home);

    /* The rim ring: the structure the dots sit on, so a quiet ring still
     * reads as an instrument rather than as scattered dots on black. */
    s_h_ring = mk_arc(s_page_home, 396, 2, HUD_TRACK, 0);
    lv_arc_set_bg_angles(s_h_ring, 0, 360);
    lv_arc_set_angles(s_h_ring, 0, 360);
    lv_obj_set_style_arc_color(s_h_ring, lv_color_hex(HUD_TRACK), LV_PART_MAIN);

    /* The worst score, as a sweep on the outside. One glance says how bad the
     * worst thing on the ring is without reading any label. */
    s_h_arc = mk_arc(s_page_home, 440, 10, HUD_CYAN, 0);

    {
        for (unsigned i = 0; i < PHAROS_HUD_HOME_MAX; i++) {
            s_h_pos[i].x = PR_CX;
            s_h_pos[i].y = PR_CY;

            /* A tick at the rim, so the ring has structure even where a watch
             * is unarmed and its dot is hidden. Placed by home_layout(). */
            s_h_tick[i] = mk_box(s_page_home);
            lv_obj_set_size(s_h_tick[i], 3, 3);
            lv_obj_set_style_radius(s_h_tick[i], LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_color(s_h_tick[i], lv_color_hex(HUD_TRACK), 0);
            lv_obj_set_style_bg_opa(s_h_tick[i], LV_OPA_COVER, 0);

            s_h_dot[i] = mk_box(s_page_home);
            lv_obj_set_size(s_h_dot[i], 16, 16);
            lv_obj_set_style_radius(s_h_dot[i], LV_RADIUS_CIRCLE, 0);
            lv_obj_set_style_bg_opa(s_h_dot[i], LV_OPA_COVER, 0);
            lv_obj_set_style_border_width(s_h_dot[i], 2, 0);
            lv_obj_add_flag(s_h_dot[i], LV_OBJ_FLAG_HIDDEN);

            /* The label sits inboard of its dot. Placed by home_layout(). */
            s_h_lbl[i] = mk_label(s_page_home, &lv_font_montserrat_12, HUD_DIMMER,
                                  0, 0, "");
            lv_obj_add_flag(s_h_lbl[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* The core: the one thing somebody reads from across a room. */
    s_h_core = mk_box(s_page_home);
    lv_obj_set_size(s_h_core, 196, 196);
    lv_obj_center(s_h_core);
    lv_obj_set_style_radius(s_h_core, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_h_core, lv_color_hex(HUD_VOID), 0);
    lv_obj_set_style_bg_opa(s_h_core, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_h_core, 1, 0);
    lv_obj_set_style_border_color(s_h_core, lv_color_hex(HUD_TRACK), 0);

    mk_label(s_page_home, &lv_font_montserrat_12, HUD_DIMMER, 0, -54, "PHAROS");
    s_h_clock = mk_label(s_page_home, &lv_font_montserrat_26, HUD_TEXT, 0, -20, "");
    s_h_head  = mk_label(s_page_home, &lv_font_montserrat_18, HUD_GREEN, 0, 20, "");
    s_h_sub   = mk_label(s_page_home, &lv_font_montserrat_12, HUD_DIMMER, 0, 48, "");

    /* THE HINT THAT COLLIDED WITH THE RING.
     *
     * "tap the middle for the full picture" is thirty-four characters - about
     * 225 px - and it was drawn at y=148, immediately under a ring of labels
     * whose lowest sit at y=136. They touched, and on the glass the sentence
     * ran straight through VIGIL, WHISPER and SENTINEL.
     *
     * There is no room for a line that wide anywhere outside the core: the
     * label ring owns the middle band and the dots own the one outside it. So
     * the hint moved INTO the core, where it shares the sub-line - see
     * paint_home() - and this label is gone. */

    /* ---- overlays ---- */

    /* THE WAY BACK, ALWAYS ON SCREEN.
     *
     * The home ring is the boot screen and the centre of the device, and there
     * was nothing anywhere that said how to return to it - so somebody who
     * opened a lens had a round screen, three touch zones and no visible way
     * out. "How do I get to the home screen" is a fair question to have to
     * ask, and it should not be one.
     *
     * A single dim line, on every page except home itself, that never changes
     * and never competes with the reading. */
    s_home_hint = mk_label(scr, &lv_font_montserrat_12, HUD_DIMMER, 0, 202,
                           "hold anywhere for home");
    lv_obj_add_flag(s_home_hint, LV_OBJ_FLAG_HIDDEN);

    s_toast = mk_label(scr, &lv_font_montserrat_20, HUD_TEXT, 0, 86, "");
    lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);

    /* ---- touch targets, added last so they sit above everything ----
     *
     * Two sets, because the two kinds of page want different shapes; see
     * zones_mode(). Both PARTITION the glass rather than sitting on it with
     * gaps between - the old layout left a 13 px dead lane either side of the
     * centre column, and a press that lands in a gap does nothing at all,
     * which reads as "the touchscreen is unreliable". */

    /* BROWSE and LIVE: three full-height columns and a bottom strip. The
     * columns meet exactly, and the strip spans the whole width instead of
     * the old 200 px, because it sits at the bottom of a CIRCLE where the
     * usable width is already shrinking. */
    s_zone_main[0] = mk_zone_at(scr, LV_ALIGN_TOP_LEFT, 0, 0, 155, 372,
                                nav_event, (void *)(uintptr_t)PHAROS_NAV_PREV);
    s_zone_main[1] = mk_zone_at(scr, LV_ALIGN_TOP_LEFT, 311, 0, 155, 372,
                                nav_event, (void *)(uintptr_t)PHAROS_NAV_NEXT);
    s_zone_main[2] = mk_zone_at(scr, LV_ALIGN_TOP_LEFT, 155, 0, 156, 372,
                                nav_event, (void *)(uintptr_t)PHAROS_NAV_SELECT);
    s_zone_main[3] = mk_zone_at(scr, LV_ALIGN_TOP_LEFT, 0, 372, 466, 94,
                                nav_event, (void *)(uintptr_t)PHAROS_NAV_DETAIL);

    /* DETAIL: one target per row, full width - so only the vertical dimension
     * has to be got right, and 58 px of it is 5.5 mm. Above and below the
     * block, two page controls that are the largest targets on the device. */
    for (unsigned i = 0; i < PHAROS_HUD_ROWS; i++) {
        const int top = 233 - 87 - PHAROS_HUD_ROW_PITCH / 2 +
                        (int)i * PHAROS_HUD_ROW_PITCH;
        s_zone_row[i] = mk_zone_at(scr, LV_ALIGN_TOP_LEFT, 0, top, 466,
                                   PHAROS_HUD_ROW_PITCH, row_event,
                                   (void *)(uintptr_t)i);
    }
    {
        const int block_top = 233 - 87 - PHAROS_HUD_ROW_PITCH / 2;
        const int block_bot = block_top + PHAROS_HUD_ROWS * PHAROS_HUD_ROW_PITCH;
        s_zone_page[0] = mk_zone_at(scr, LV_ALIGN_TOP_LEFT, 0, 0, 466, block_top,
                                    nav_event, (void *)(uintptr_t)PHAROS_NAV_PREV);
        s_zone_page[1] = mk_zone_at(scr, LV_ALIGN_TOP_LEFT, 0, block_bot, 466,
                                    466 - block_bot, nav_event,
                                    (void *)(uintptr_t)PHAROS_NAV_NEXT);
    }
    for (unsigned i = 0; i < PHAROS_HUD_ROWS; i++) {
        lv_obj_add_flag(s_zone_row[i], LV_OBJ_FLAG_HIDDEN);
    }
    lv_obj_add_flag(s_zone_page[0], LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_zone_page[1], LV_OBJ_FLAG_HIDDEN);
    s_zones_detail = 0;

    page_show(PAGE_BROWSE);

    s_built = true;
    return true;
}

/* A theme change, applied by building the face again.
 *
 * The alternative - walking every widget and re-setting the colours it was
 * given at construction - is faster and wrong: it is a second list of which
 * widget takes which colour, kept in a different function from the first, and
 * the failure mode when they drift is one line on the screen still wearing the
 * old palette. Whereas a rebuild cannot miss anything, because there is only
 * ever one description of the face.
 *
 * The cost is one full repaint, on a change a person makes by hand. That is an
 * easy trade. The caller holds the display lock. */
void pharos_hud_rebuild(void)
{
    lv_obj_t *scr = lv_screen_active();
    if (!s_built || !scr) {
        return;
    }
    /* Drop any gesture in flight FIRST. The zones are about to be destroyed
     * and immediately recreated at the same coordinates, and an input device
     * still holding a press over one of them can land the release on its
     * replacement - which is a tap nobody made, on a face nobody was looking
     * at yet. It cost a lens switch the first time the theme changed. */
    lv_indev_reset(NULL, NULL);

    /* Any animation still running targets a widget that is about to be freed;
     * LVGL would keep writing into it. Stop them before the clean, not after. */
    lv_anim_delete_all();
    lv_obj_clean(scr);
    s_built = false;
    s_h_laid = 0; /* the widgets are gone; the layout must be redone */

    /* Every pointer above just became dangling, and the dirty checks read the
     * widgets themselves - except these, which are caches OUTSIDE them and
     * would otherwise suppress the first paint of the new face. */
    s_ceiling_at = -1;
    for (unsigned i = 0; i < PHAROS_DISP_HISTORY; i++) {
        s_bar_level[i] = 0;
        s_bar_rgb[i] = 0;
    }
    s_toast_until_ms = 0;

    pharos_hud_create();
}

static void toast_tick(void)
{
    if (s_toast && s_toast_until_ms && lv_tick_get() > s_toast_until_ms) {
        lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
        s_toast_until_ms = 0;
    }
}

void pharos_hud_toast(const char *msg)
{
    if (!s_built || !s_toast || !msg) {
        return;
    }
    lv_label_set_text(s_toast, msg);
    lv_obj_remove_flag(s_toast, LV_OBJ_FLAG_HIDDEN);
    s_toast_until_ms = lv_tick_get() + 1400u;
}

/* ---- BROWSE: what does this tool actually do? ------------------------ */

/* The colour of a watch's dot. Verdict colours, not theme colours - a red on
 * the home ring must mean what a red on the Census page means. */
static uint32_t home_state_colour(uint8_t st)
{
    switch (st) {
    case 4:  return HUD_RED;    /* PTW_ALARM    */
    case 3:  return HUD_ORANGE; /* PTW_ELEVATED */
    case 2:  return HUD_AMBER;  /* PTW_NOTED    */
    case 1:  return HUD_GREEN;  /* PTW_QUIET    */
    default: return HUD_DENIED; /* PTW_UNKNOWN  */
    }
}

/* THE RING FITS HOWEVER MANY WATCHES ARE ARMED.
 *
 * It was laid out once at construction for PHAROS_HUD_HOME_MAX slots, so eight
 * watches sat in twelve slots with a third of the circle empty - and thirteen
 * would have overlapped, because the spacing was for twelve. The number of
 * watches is a runtime fact (a lens that will not start does not join), so the
 * spacing has to be one too.
 *
 * Recomputed only when the count changes, which is almost never. */
/* THE WIDTH OF THE LABELS ACTUALLY BEING DRAWN.
 *
 * Sizing the ring for PD_RING_LABEL_W - the longest name in the project,
 * FOOTPRINT - assumed every watch on the dial was nine characters wide. With
 * eleven armed, the longest is HARVEST, and the difference is enough to decide
 * whether all eleven get a name or one is silently left blank. It was left
 * blank, and an unnamed dot among named ones reads as a fault.
 *
 * Worst-case width AND worst-case count is double-counting. Measure what is
 * there. */
static int16_t home_label_w(const struct pharos_hud_home *h, unsigned n)
{
    unsigned widest = 0;
    for (unsigned i = 0; i < n; i++) {
        if (!h->label_on[i] || !h->label[i]) {
            continue;
        }
        unsigned k = 0;
        while (h->label[i][k]) {
            k++;
        }
        if (k > widest) {
            widest = k;
        }
    }
    if (!widest) {
        widest = 7u;
    }
    /* Montserrat 12 capitals measure about 7.6 px of advance, plus a little
     * bearing either side. */
    return (int16_t)((widest * 76u) / 10u + 4u);
}

static void home_layout(unsigned n, int16_t label_w)
{
    if ((n == s_h_laid && label_w == s_h_laid_w) || !n ||
        n > PHAROS_HUD_HOME_MAX) {
        return;
    }
    s_h_laid = n;
    s_h_laid_w = label_w;

    /* The geometry is computed, not guessed - see pd_ring_layout(), whose
     * spacing at every count is pinned by test_ring.c. Photographing the
     * screen to find out whether the names fit was how thirteen of them
     * shipped reading as one long word. */
    pd_ring_t g;
    pd_ring_layout(n, label_w, 14, 12, &g);

    pd_dial_t d;
    pd_dial_layout(n, -90.0f, 150, 205, &d);
    for (unsigned i = 0; i < n; i++) {
        const float a = pd_dial_item_angle(&d, i);
        const pr_point_t p = pr_polar(g.r_dot, a);
        const pr_point_t t = pr_polar(202, a);
        const pr_point_t lp = pr_polar(pd_ring_label_r(&g, i), a);

        s_h_pos[i].x = p.x;
        s_h_pos[i].y = p.y;
        lv_obj_align(s_h_dot[i], LV_ALIGN_CENTER, p.x - PR_CX, p.y - PR_CY);
        lv_obj_align(s_h_tick[i], LV_ALIGN_CENTER, t.x - PR_CX, t.y - PR_CY);
        lv_obj_align(s_h_lbl[i], LV_ALIGN_CENTER, lp.x - PR_CX, lp.y - PR_CY);
    }
}

void pharos_hud_home(const struct pharos_hud_home *h)
{
    if (!s_built || !h) {
        return;
    }
    toast_tick();
    page_show(PAGE_HOME);
    zones_mode(false);

    s_h_n = (h->n > PHAROS_HUD_HOME_MAX) ? PHAROS_HUD_HOME_MAX : h->n;
    /* The caller's own measurement, so the layout and the choice of which
     * labels to show are sized against the same number. */
    home_layout(s_h_n, h->label_w ? h->label_w : home_label_w(h, s_h_n));

    for (unsigned i = 0; i < PHAROS_HUD_HOME_MAX; i++) {
        const bool used = (i < s_h_n);
        show(s_h_dot[i], used);
        show(s_h_lbl[i], used && h->label_on[i]);
        show(s_h_tick[i], used);
        if (!used) {
            continue;
        }
        set_text(s_h_lbl[i], h->label[i] ? h->label[i] : "");

        const uint32_t col = home_state_colour(h->state[i]);
        /* FADE IS THE HONESTY. There is one radio and the watches take turns,
         * so a dot is only as trustworthy as it is recent: a live reading is
         * filled, an ageing one is dimmed, an expired one is drawn hollow -
         * an outline with nothing inside it, which is exactly what the device
         * knows about that watch right now. */
        const uint8_t fade = h->fade[i];
        const bool hollow = (fade >= 2u);
        set_bg_colour(s_h_dot[i], hollow ? HUD_VOID : col);
        lv_obj_set_style_bg_opa(s_h_dot[i],
                                hollow ? LV_OPA_TRANSP
                                       : (fade ? LV_OPA_50 : LV_OPA_COVER), 0);
        lv_obj_set_style_border_color(s_h_dot[i], lv_color_hex(col), 0);
        lv_obj_set_style_border_opa(s_h_dot[i],
                                    hollow ? LV_OPA_70 : LV_OPA_COVER, 0);

        /* THE WATCH HOLDING THE RADIO BREATHES.
         *
         * A slightly larger dot says which one is live, and on a ring of
         * sixteen it is easy to miss - the difference between 16 px and 22 px
         * is not much when the eye is scanning colours. A slow pulse is not
         * decoration: it is the one cue on the face that says the device is
         * WORKING rather than showing a frozen picture, which on a monitor
         * that spends most of its time reporting "quiet" is worth having.
         *
         * Two seconds a cycle, and only ever the active dot - a face where
         * everything moves is harder to read, not easier. */
        const bool live = ((int)i == h->active);
        int size = live ? 22 : 16;
        if (live) {
            const uint32_t phase = (lv_tick_get() % 2000u);
            const uint32_t up = (phase < 1000u) ? phase : (2000u - phase);
            size = 20 + (int)((up * 5u) / 1000u); /* 20..25 and back */
        }
        /* DIRTY-CHECKED, like every other write on this face.
         *
         * lv_obj_set_size() marks the object for re-layout whether or not the
         * size differs, and a pulse recomputed every frame therefore forced a
         * relayout and redraw of the ring ten times a second - which held the
         * LVGL lock long enough that the next paint could not get it. The
         * console filled with "Failed to acquire LVGL lock" and the face
         * started dropping frames, to animate a dot by five pixels.
         *
         * The pulse only actually changes size a few times a second; asking
         * first costs one comparison and gives the rest of the frames back. */
        if (lv_obj_get_width(s_h_dot[i]) != size) {
            lv_obj_set_size(s_h_dot[i], size, size);
        }
        lv_obj_align(s_h_dot[i], LV_ALIGN_CENTER,
                     s_h_pos[i].x - PR_CX, s_h_pos[i].y - PR_CY);
        set_text_colour(s_h_lbl[i], live ? HUD_TEXT
                                         : (hollow ? HUD_DENIED : HUD_DIM));
    }

    set_text(s_h_clock, h->clock ? h->clock : "");
    set_text(s_h_head, h->headline ? h->headline : "");
    set_text(s_h_sub, h->sub ? h->sub : "");

    const uint32_t wc = home_state_colour(h->worst_state);
    set_text_colour(s_h_head, wc);
    lv_obj_set_style_border_color(
        s_h_core, lv_color_hex(h->worst_state >= 3u ? wc : HUD_TRACK), 0);

    set_arc_colour(s_h_arc, wc);
    set_arc_value(s_h_arc, h->worst_score);
}

int pharos_hud_home_hit(int16_t x, int16_t y)
{
    /* The core first: it is the biggest target on the device and it is in the
     * middle, so it must not be stolen by a dot whose reach overlaps it. */
    {
        const int32_t dx = (int32_t)x - PR_CX;
        const int32_t dy = (int32_t)y - PR_CY;
        if (dx * dx + dy * dy <= 96 * 96) {
            return (int)PHAROS_HUD_HOME_CORE;
        }
    }

    /* Nearest dot within a thumb's reach, rather than a wedge test: the dots
     * are 16 px and a fingertip is about 90, so "which did they mean" is a
     * distance question. PR_TOUCH_MIN is the same minimum the dial geometry is
     * checked against. */
    int best = -1;
    int32_t best_d2 = (int32_t)PR_TOUCH_MIN * PR_TOUCH_MIN;
    for (unsigned i = 0; i < s_h_n; i++) {
        const int32_t dx = (int32_t)x - s_h_pos[i].x;
        const int32_t dy = (int32_t)y - s_h_pos[i].y;
        const int32_t d2 = dx * dx + dy * dy;
        if (d2 <= best_d2) {
            best_d2 = d2;
            best = (int)i;
        }
    }
    return best;
}


void pharos_hud_browse(const char *name, const char *summary, const char *team,
                       unsigned index, unsigned total, uint32_t rgb)
{
    if (!s_built) {
        return;
    }
    toast_tick();
    page_show(PAGE_BROWSE);
    zones_mode(false);

    set_text(s_b_name, name ? name : "");
    set_text_colour(s_b_name, rgb);
    {
        char buf[32];
        snprintf(buf, sizeof(buf), "%s  %u / %u", team ? team : "", index + 1u,
                 total ? total : 1u);
        set_text(s_b_pos, buf);
    }
    set_text(s_b_summary, summary ? summary : "");
    set_arc_value(s_b_arc, (int)((index + 1u) * 100u / (total ? total : 1u)));
    set_arc_colour(s_b_arc, rgb);
}

/* ---- LIVE: the lens running ------------------------------------------ */

void pharos_hud_live(const char *lens, const struct pharos_lens_display *d,
                     uint32_t rgb_override)
{
    if (!s_built) {
        return;
    }
    toast_tick();
    page_show(PAGE_LIVE);
    zones_mode(false); /* three columns and a bottom strip */

    if (!d) {
        show(s_sim, false);
        set_text(s_ctx, lens ? lens : "");
        set_text(s_big, "--");
        set_text(s_band, "starting");
        set_text(s_detail, "");
        set_text(s_why, "");
        set_arc_value(s_arc, 0);
        set_arc_value(s_ghost, 0);
        set_ceiling_tick(0);
        for (unsigned i = 0; i < PHAROS_DISP_FAMILIES; i++) {
            set_text(s_fam_txt[i], "");
            set_bg_colour(s_fam_box[i], HUD_PIP_UP);
        }
        for (unsigned i = 0; i < PHAROS_DISP_HISTORY; i++) {
            bar_set(i, 0, HUD_DENIED);
        }
        return;
    }

    const int score = d->has_score ? (int)d->score : 0;
    /* The band decides the colour, not the caller: a 78 must look like a 78
     * whichever lens produced it, exactly as in the rendered screens. The
     * override exists only for the splash. */
    const uint32_t col = rgb_override ? rgb_override
                                      : (d->has_score ? band_colour(score) : HUD_DIM);

    /* The context line carries what is under pressure and how hard this
     * receiver is looking. It used to live on the top rim, which is where the
     * activity ribbon now is - a text run and a bar chart sharing an arc is
     * how you get a screen that looks broken. */
    show(s_sim, d->simulated);
    set_text(s_ctx, lens ? lens : "");
    set_text(s_big, d->big);
    set_text(s_band, d->band);
    set_text(s_detail, d->detail);
    set_text_colour(s_big, col);
    set_text_colour(s_band, col);

    /* Prefer the specific finding over the generic advice: "sequence counter
     * went backwards" tells an operator something "the shape looks wrong"
     * does not. */
    const bool specific = d->why[0] != '\0';
    set_text(s_why, specific ? d->why : d->advice);
    set_text_colour(s_why, specific ? col : HUD_DIM);

    set_arc_value(s_arc, score);
    set_arc_colour(s_arc, col);
    /* What the caps took away, thin and inset. Zero when nothing was taken,
     * so the trace only appears when it has something to say. */
    set_arc_value(s_ghost, (d->raw_score > d->score) ? (int)d->raw_score : 0);
    set_ceiling_tick(d->has_score ? (int)d->ceiling : 0);

    for (unsigned i = 0; i < PHAROS_DISP_FAMILIES; i++) {
        const bool lit = (d->families & (1u << i)) != 0;
        set_text(s_fam_txt[i], d->fam_label[i] ? d->fam_label[i] : "");
        set_text_colour(s_fam_txt[i], lit ? col : HUD_DENIED);
        set_bg_colour(s_fam_box[i], lit ? HUD_PIP_ON : HUD_PIP_UP);
    }

    if (d->has_history) {
        for (unsigned i = 0; i < PHAROS_DISP_HISTORY; i++) {
            bar_set(i, d->history[i], col);
        }
    } else {
        for (unsigned i = 0; i < PHAROS_DISP_HISTORY; i++) {
            bar_set(i, 0, HUD_DENIED);
        }
    }
}

void pharos_hud_detail(const char *lens, const char *head_left,
                       const char *head_right,
                       const struct pharos_lens_row *rows, unsigned n,
                       unsigned page, unsigned pages, int focus, bool openable)
{
    if (!s_built) {
        return;
    }
    toast_tick();
    page_show(PAGE_DETAIL);
    zones_mode(true); /* one target per row; see zones_mode() */

    set_text(s_d_title, lens ? lens : "");
    set_text(s_d_hl, head_left ? head_left : "");
    set_text(s_d_hr, head_right ? head_right : "");

    if (n > PHAROS_HUD_ROWS) {
        n = PHAROS_HUD_ROWS;
    }
    for (unsigned i = 0; i < PHAROS_HUD_ROWS; i++) {
        const bool used = (i < n);
        set_text(s_d_left[i], used ? rows[i].left : "");
        set_text(s_d_right[i], used ? rows[i].right : "");
        if (used) {
            set_text_colour(s_d_left[i], HUD_TEXT);
            set_text_colour(s_d_right[i], tone_colour(rows[i].tone));
        }
        /* Focus is a filled pill behind the row, not a recoloured word: the
         * tone column already carries meaning, and overriding it to show a
         * cursor would make a network look worse than it graded. */
        const bool on = used && ((int)i == focus);
        show(s_d_pill[i], on);
        /* The rule under the LAST row of a page is redundant with the page
         * controls below it, and drawing one under an empty row draws a line
         * under nothing. */
        show(s_d_rule[i], used && (i + 1u) < n);
    }

    /* An empty list is a finding, not a blank screen. */
    const bool empty = (n == 0);
    show(s_d_empty, empty);
    if (empty) {
        set_text(s_d_empty, "nothing heard yet");
    }
    show(s_d_hl, !empty);
    show(s_d_hr, !empty);

    /* The page counter sits IN the lower page control, so the number and the
     * thing that changes it are the same object. */
    {
        char buf[32];
        if (pages > 1 && (page + 1u) < pages) {
            snprintf(buf, sizeof(buf), "%u / %u  %s", page + 1u, pages,
                     LV_SYMBOL_DOWN);
        } else if (pages > 1) {
            snprintf(buf, sizeof(buf), "%u / %u", page + 1u, pages);
        } else {
            buf[0] = '\0';
        }
        set_text(s_d_page, buf);
        set_text(s_d_up, (page > 0) ? LV_SYMBOL_UP : "");
        /* The back instruction is on the shared hint below; this line says
         * only what is specific to a list of rows. */
        set_text(s_d_hint, (!empty && openable) ? "touch a row to open it" : "");
    }
}

void pharos_hud_splash(const char *version, bool fence_clean)
{
    if (!pharos_hud_create()) {
        return;
    }
    struct pharos_lens_display d;
    memset(&d, 0, sizeof(d));
    snprintf(d.big, sizeof(d.big), "%s", "\xE2\x97\x89"); /* a filled ring: the lamp */
    snprintf(d.band, sizeof(d.band), "%s",
             fence_clean ? "receive-only" : "FENCE UNVERIFIED");
    snprintf(d.detail, sizeof(d.detail), "%s", version ? version : "");
    snprintf(d.advice, sizeof(d.advice), "%s",
             fence_clean ? "tap the edges to browse" : "check the fence");
    pharos_hud_live("PHAROS", &d, fence_clean ? HUD_GREEN : 0xE8503F);
}

void pharos_hud_colourbars(void)
{
    lv_obj_t *scr = lv_screen_active();
    if (!scr) {
        return;
    }
    /* Start from a clean screen: this is a measurement, not a HUD overlay.
     * Everything above is destroyed with it, so the pointers must go too or
     * the next repaint writes into freed objects. */
    /* Drop any gesture in flight FIRST. The zones are about to be destroyed
     * and immediately recreated at the same coordinates, and an input device
     * still holding a press over one of them can land the release on its
     * replacement - which is a tap nobody made, on a face nobody was looking
     * at yet. It cost a lens switch the first time the theme changed. */
    lv_indev_reset(NULL, NULL);

    /* Any animation still running targets a widget that is about to be freed;
     * LVGL would keep writing into it. Stop them before the clean, not after. */
    lv_anim_delete_all();
    lv_obj_clean(scr);
    s_built = false;
    s_h_laid = 0; /* the widgets are gone; the layout must be redone */
    s_page_browse = NULL;
    s_page_live = NULL;
    for (unsigned i = 0; i < PHAROS_DISP_HISTORY; i++) {
        s_bar[i] = NULL;
    }
    for (unsigned i = 0; i < PHAROS_DISP_FAMILIES; i++) {
        s_fam_box[i] = NULL;
        s_fam_txt[i] = NULL;
    }
    s_field = s_ticks = s_pip = s_toast = NULL;
    s_track = s_arc = s_ghost = s_ceiling = s_core = NULL;
    s_ctx = s_big = s_band = s_detail = s_why = s_peak = s_bar_base = NULL;
    s_sim = NULL;
    s_ceiling_at = -1;
    s_b_name = s_b_pos = s_b_summary = s_b_hint = s_b_arc = NULL;
    s_page_detail = s_d_title = s_d_hl = s_d_hr = s_d_page = s_d_empty = NULL;
    for (unsigned i = 0; i < PHAROS_HUD_ROWS; i++) {
        s_d_left[i] = s_d_right[i] = s_d_rule[i] = NULL;
    }

    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);

    static const struct { uint32_t rgb; const char *name; } bar[] = {
        { 0xFF0000, "RED"   }, { 0x00FF00, "GREEN" }, { 0x0000FF, "BLUE"  },
        { 0xFFFF00, "YELLOW"}, { 0xFFFFFF, "WHITE" }, { 0x000000, "BLACK" },
    };
    const int n = (int)(sizeof(bar) / sizeof(bar[0]));
    const int h = PR_H / n;
    for (int i = 0; i < n; i++) {
        lv_obj_t *b = lv_obj_create(scr);
        lv_obj_remove_style_all(b);
        lv_obj_remove_flag(b, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(b, PR_W, h);
        lv_obj_set_pos(b, 0, i * h);
        lv_obj_set_style_bg_color(b, lv_color_hex(bar[i].rgb), 0);
        lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);

        lv_obj_t *t = lv_label_create(b);
        lv_obj_set_style_text_font(t, &lv_font_montserrat_20, 0);
        /* Label in the opposite luminance so it is readable on its own patch. */
        lv_obj_set_style_text_color(
            t, lv_color_hex(bar[i].rgb == 0xFFFFFF || bar[i].rgb == 0xFFFF00
                                ? 0x000000 : 0xFFFFFF), 0);
        lv_label_set_text(t, bar[i].name);
        lv_obj_center(t);
    }
}

#else /* no panel on this build */

/* Stubs, one per public entry point and nothing more.
 *
 * This branch had acquired a verbatim copy of the LVGL implementation - a
 * bulk edit that matched in both halves of the file - so it declared
 * pharos_hud_detail twice and referenced widgets that do not exist here. It
 * compiled for nobody, because nobody builds without the vendor BSP, which is
 * exactly how a broken branch survives. tools/check_sources.sh now counts the
 * definitions so it cannot happen quietly again. */

bool pharos_hud_create(void) { return false; }
void pharos_hud_rebuild(void) {}
bool pharos_hud_present(void) { return false; }
void pharos_hud_set_nav_cb(pharos_hud_nav_cb_t cb) { (void)cb; }
void pharos_hud_set_row_cb(pharos_hud_row_cb_t cb) { (void)cb; }
void pharos_hud_toast(const char *msg) { (void)msg; }
void pharos_hud_browse(const char *name, const char *summary, const char *team,
                       unsigned index, unsigned total, uint32_t rgb)
{
    (void)name; (void)summary; (void)team; (void)index; (void)total; (void)rgb;
}
void pharos_hud_live(const char *lens, const struct pharos_lens_display *d,
                     uint32_t rgb_override)
{
    (void)lens; (void)d; (void)rgb_override;
}
void pharos_hud_detail(const char *lens, const char *head_left,
                       const char *head_right,
                       const struct pharos_lens_row *rows, unsigned n,
                       unsigned page, unsigned pages, int focus, bool openable)
{
    (void)lens; (void)head_left; (void)head_right; (void)rows; (void)n;
    (void)page; (void)pages; (void)focus; (void)openable;
}
void pharos_hud_splash(const char *version, bool fence_clean)
{
    (void)version; (void)fence_clean;
}
void pharos_hud_colourbars(void) {}
void pharos_hud_home(const struct pharos_hud_home *h) { (void)h; }
void pharos_hud_set_home_cb(pharos_hud_home_cb_t cb) { (void)cb; }
int pharos_hud_home_hit(int16_t x, int16_t y) { (void)x; (void)y; return -1; }

#endif
