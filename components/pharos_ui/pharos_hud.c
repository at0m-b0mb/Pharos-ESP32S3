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

#include "pharos_hud.h"
#include "pharos_theme.h"

#if !defined(PHAROS_HOST) && defined(CONFIG_PHAROS_HAS_VENDOR_BSP)

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "lvgl.h"

/* PR_W / PR_H and the polar helpers - the panel geometry, shared with the
 * round-screen maths and with the renderer. */
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

static bool s_built;
static pharos_hud_nav_cb_t s_nav_cb;
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

static void set_arc_value(lv_obj_t *o, int v)
{
    if (!o) {
        return;
    }
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    if (lv_arc_get_value(o) == v) {
        return;
    }
    lv_arc_set_value(o, v);
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

static void nav_event(lv_event_t *e)
{
    if (!s_nav_cb) {
        return;
    }
    const lv_event_code_t code = lv_event_get_code(e);
    const pharos_nav_t what = (pharos_nav_t)(uintptr_t)lv_event_get_user_data(e);
    /* Record the intent and return. See the header: doing the real work here
     * runs it on LVGL's task, with LVGL's stack, and reboots the board. */
    if (code == LV_EVENT_LONG_PRESSED) {
        s_nav_cb(PHAROS_NAV_HOME);
    } else if (code == LV_EVENT_SHORT_CLICKED) {
        s_nav_cb(what);
    }
}

static void mk_zone(lv_obj_t *parent, lv_align_t align, int w, int h,
                    pharos_nav_t what)
{
    lv_obj_t *z = lv_obj_create(parent);
    lv_obj_remove_style_all(z);
    lv_obj_set_size(z, w, h);
    lv_obj_align(z, align, 0, 0);
    /* SCROLLABLE off. This is the one flag whose absence let a drag - or a
     * smeared tap, which on round glass is most of them - scroll the entire
     * face out of view with no way back. It was the "and it breaks" half of
     * the bug report. */
    lv_obj_remove_flag(z, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scrollbar_mode(z, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(z, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(z, LV_OPA_TRANSP, 0);
    lv_obj_add_event_cb(z, nav_event, LV_EVENT_SHORT_CLICKED, (void *)(uintptr_t)what);
    lv_obj_add_event_cb(z, nav_event, LV_EVENT_LONG_PRESSED, (void *)(uintptr_t)what);
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

    s_d_title = mk_label(s_page_detail, &lv_font_montserrat_18, HUD_TEXT, 0, -150, "");

    s_d_hl = mk_label(s_page_detail, &lv_font_montserrat_12, HUD_DIMMER, CX_L, -122, "");
    lv_obj_set_width(s_d_hl, COL_W_L);
    lv_obj_set_style_text_align(s_d_hl, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_align(s_d_hl, LV_ALIGN_CENTER, CX_L, -122);

    s_d_hr = mk_label(s_page_detail, &lv_font_montserrat_12, HUD_DIMMER, CX_R, -122, "");
    lv_obj_set_width(s_d_hr, COL_W_R);
    lv_obj_set_style_text_align(s_d_hr, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_align(s_d_hr, LV_ALIGN_CENTER, CX_R, -122);

    for (unsigned i = 0; i < PHAROS_HUD_ROWS; i++) {
        const int y = -92 + (int)i * 36;

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

        /* A hairline under each row. Six unruled lines of small text on a
         * round black face is a wall; the rule gives the eye a track to
         * follow across to the value. Sized to the block, not the screen. */
        s_d_rule[i] = mk_box(s_page_detail);
        lv_obj_set_size(s_d_rule[i], BLOCK_HALF * 2, 1);
        lv_obj_align(s_d_rule[i], LV_ALIGN_CENTER, 0, y + 17);
        lv_obj_set_style_bg_color(s_d_rule[i], lv_color_hex(HUD_TRACK2), 0);
        lv_obj_set_style_bg_opa(s_d_rule[i], LV_OPA_COVER, 0);
    }

    s_d_page  = mk_label(s_page_detail, &lv_font_montserrat_16, HUD_DIMMER, 0, 148, "");
    s_d_empty = mk_label(s_page_detail, &lv_font_montserrat_18, HUD_DIM, 0, 0, "");

    /* ---- overlays ---- */

    s_toast = mk_label(scr, &lv_font_montserrat_20, HUD_TEXT, 0, 86, "");
    lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);

    /* Navigation zones, added last so they sit above everything. Left and
     * right thirds step through the lenses; the centre is the action. */
    mk_zone(scr, LV_ALIGN_LEFT_MID,  140, 330, PHAROS_NAV_PREV);
    mk_zone(scr, LV_ALIGN_RIGHT_MID, 140, 330, PHAROS_NAV_NEXT);
    mk_zone(scr, LV_ALIGN_CENTER,    160, 230, PHAROS_NAV_SELECT);
    /* The bottom strip: what did you actually find? Short, so it cannot eat
     * the left/right thirds, and only meaningful while a lens is running. */
    mk_zone(scr, LV_ALIGN_BOTTOM_MID, 200, 74, PHAROS_NAV_DETAIL);

    show(s_page_live, false);
    show(s_page_detail, false);
    show(s_page_browse, true);

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

    lv_obj_clean(scr);
    s_built = false;

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

void pharos_hud_browse(const char *name, const char *summary, const char *team,
                       unsigned index, unsigned total, uint32_t rgb)
{
    if (!s_built) {
        return;
    }
    toast_tick();
    show(s_page_live, false);
    show(s_page_detail, false);
    show(s_page_browse, true);

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
    show(s_page_browse, false);
    show(s_page_detail, false);
    show(s_page_live, true);

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
    show(s_page_browse, false);
    show(s_page_live, false);
    show(s_page_detail, true);

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
        /* The focused row's rule brightens rather than the text changing
         * colour: the tone column already carries meaning, and overriding it
         * to show a cursor would make a network look worse than it graded. */
        show(s_d_rule[i], used);
        if (used && s_d_rule[i]) {
            const bool on = ((int)i == focus);
            lv_obj_set_style_bg_color(s_d_rule[i],
                                      lv_color_hex(on ? HUD_CYAN : HUD_TRACK2), 0);
            lv_obj_set_height(s_d_rule[i], on ? 2 : 1);
        }
    }

    /* An empty list is a finding, not a blank screen. */
    const bool empty = (n == 0);
    show(s_d_empty, empty);
    if (empty) {
        set_text(s_d_empty, "nothing heard yet");
    }
    show(s_d_hl, !empty);
    show(s_d_hr, !empty);

    {
        char buf[32];
        if (pages > 1 && openable) {
            snprintf(buf, sizeof(buf), "%u / %u   tap to open", page + 1u, pages);
        } else if (pages > 1) {
            snprintf(buf, sizeof(buf), "%u / %u", page + 1u, pages);
        } else if (openable) {
            snprintf(buf, sizeof(buf), "tap to open");
        } else {
            buf[0] = '\0';
        }
        set_text(s_d_page, buf);
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

    lv_obj_clean(scr);
    s_built = false;
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

bool pharos_hud_create(void) { return false; }
bool pharos_hud_present(void) { return false; }
void pharos_hud_set_nav_cb(pharos_hud_nav_cb_t cb) { (void)cb; }
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
    if (!s_built) {
        return;
    }
    toast_tick();
    show(s_page_browse, false);
    show(s_page_live, false);
    show(s_page_detail, true);

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
        /* The focused row's rule brightens rather than the text changing
         * colour: the tone column already carries meaning, and overriding it
         * to show a cursor would make a network look worse than it graded. */
        show(s_d_rule[i], used);
        if (used && s_d_rule[i]) {
            const bool on = ((int)i == focus);
            lv_obj_set_style_bg_color(s_d_rule[i],
                                      lv_color_hex(on ? HUD_CYAN : HUD_TRACK2), 0);
            lv_obj_set_height(s_d_rule[i], on ? 2 : 1);
        }
    }

    /* An empty list is a finding, not a blank screen. */
    const bool empty = (n == 0);
    show(s_d_empty, empty);
    if (empty) {
        set_text(s_d_empty, "nothing heard yet");
    }
    show(s_d_hl, !empty);
    show(s_d_hr, !empty);

    {
        char buf[32];
        if (pages > 1 && openable) {
            snprintf(buf, sizeof(buf), "%u / %u   tap to open", page + 1u, pages);
        } else if (pages > 1) {
            snprintf(buf, sizeof(buf), "%u / %u", page + 1u, pages);
        } else if (openable) {
            snprintf(buf, sizeof(buf), "tap to open");
        } else {
            buf[0] = '\0';
        }
        set_text(s_d_page, buf);
    }
}

void pharos_hud_detail(const char *lens, const char *head_left,
                       const char *head_right,
                       const struct pharos_lens_row *rows, unsigned n,
                       unsigned page, unsigned pages, int focus, bool openable)
{
    (void)lens; (void)head_left; (void)head_right; (void)rows; (void)n;
    (void)page; (void)pages;
}
void pharos_hud_splash(const char *version, bool fence_clean)
{
    (void)version; (void)fence_clean;
}
void pharos_hud_colourbars(void) {}

#endif
