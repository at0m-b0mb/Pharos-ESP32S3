/* Pharos - the on-device LVGL screen.
 *
 * Only compiled with a real panel; with the vendor BSP off these become
 * no-ops so the firmware still builds and runs headless.
 *
 * Layout notes for a 466 px CIRCLE. The usable rectangle inside a circle is a
 * lot smaller than the circle: towards the top and bottom the chord narrows to
 * nothing, so anything wide has to live near the middle. Everything here is
 * centre-aligned and the wrapped text column is capped at 300 px, which is the
 * widest a multi-line block can be and still stay clear of the bezel.
 */

/* sdkconfig.h FIRST - see the long note in pharos_bsp.c. The #if below tests
 * CONFIG_PHAROS_HAS_VENDOR_BSP, and without this the macro is undefined at
 * that point and the whole HUD compiles to no-ops. */
#include "sdkconfig.h"

#include "pharos_hud.h"

#if !defined(PHAROS_HOST) && defined(CONFIG_PHAROS_HAS_VENDOR_BSP)

#include <stdint.h>
#include <stdio.h>

#include "lvgl.h"

/* Palette - the same one the Virtual Pharos renderer uses, so the device and
 * the documentation screenshots are the same product. */
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
#define HUD_VOID   0x000000
#define HUD_FIELD  0x081C29
#define HUD_RIM    0x215163
#define HUD_TRACK  0x102839
#define HUD_TRACK2 0x081418
#define HUD_TEXT   0xE7F7F7
#define HUD_DIM    0x7BA6B5
#define HUD_DIMMER 0x4A798C
#define HUD_CYAN   0x21B6C6
#define HUD_AMBER  0xFFC34A
#define HUD_ORANGE 0xEF9239
#define HUD_RED    0xE75142
#define HUD_GREEN  0x39DB84

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

static lv_obj_t *s_field;  /* the instrument face          */
static lv_obj_t *s_ticks;  /* 24 bezel ticks               */
static lv_obj_t *s_track;  /* dark gauge track             */
static lv_obj_t *s_ghost;  /* earned-then-capped, dimmed   */
static lv_obj_t *s_arc;    /* the score itself             */
static lv_obj_t *s_core;   /* dark disc behind the number  */
static lv_obj_t *s_pip;    /* the always-on receive-only dot */
static lv_obj_t *s_lens;    /* lens name, top          */
static lv_obj_t *s_pos;     /* "blue  3 / 16"          */
static lv_obj_t *s_big;     /* headline value          */
static lv_obj_t *s_band;    /* band word               */
static lv_obj_t *s_detail;  /* detail line             */
static lv_obj_t *s_summary; /* what this lens does     */
static lv_obj_t *s_hint;    /* what a tap will do      */
static lv_obj_t *s_rx;      /* receive-only pip        */
static lv_obj_t *s_toast;
static bool s_built;
static pharos_hud_nav_cb_t s_nav_cb;
static uint32_t s_toast_until_ms;

static lv_obj_t *mk_label(lv_obj_t *parent, const lv_font_t *font, uint32_t rgb,
                          lv_align_t align, int dx, int dy, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(rgb), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(l, text);
    lv_obj_align(l, align, dx, dy);
    return l;
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
    lv_obj_add_flag(z, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(z, LV_OPA_TRANSP, 0);
    lv_obj_add_event_cb(z, nav_event, LV_EVENT_SHORT_CLICKED, (void *)(uintptr_t)what);
    lv_obj_add_event_cb(z, nav_event, LV_EVENT_LONG_PRESSED, (void *)(uintptr_t)what);
}

void pharos_hud_set_nav_cb(pharos_hud_nav_cb_t cb) { s_nav_cb = cb; }

bool pharos_hud_present(void) { return s_built; }

bool pharos_hud_create(void)
{
    if (s_built) {
        return true;
    }
    lv_obj_t *scr = lv_screen_active();
    if (!scr) {
        return false;
    }

    /* AMOLED: an unlit pixel costs no power, so the field is near-black. */
    lv_obj_set_style_bg_color(scr, lv_color_hex(HUD_VOID), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);

    /* The instrument face: a slightly lifted disc inside the void, with a rim
     * line. This is what makes it read as an instrument rather than as text on
     * a black background. */
    s_field = lv_obj_create(scr);
    lv_obj_remove_style_all(s_field);
    lv_obj_set_size(s_field, 462, 462);
    lv_obj_center(s_field);
    lv_obj_set_style_radius(s_field, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_field, lv_color_hex(HUD_FIELD), 0);
    lv_obj_set_style_bg_opa(s_field, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_field, lv_color_hex(HUD_RIM), 0);
    lv_obj_set_style_border_width(s_field, 1, 0);
    lv_obj_remove_flag(s_field, LV_OBJ_FLAG_SCROLLABLE);

    /* 24 bezel ticks, every sixth long and cyan - the instrument's compass. */
    s_ticks = lv_scale_create(scr);
    lv_obj_set_size(s_ticks, 440, 440);
    lv_obj_center(s_ticks);
    lv_scale_set_mode(s_ticks, LV_SCALE_MODE_ROUND_INNER);
    lv_scale_set_total_tick_count(s_ticks, 25);
    lv_scale_set_major_tick_every(s_ticks, 6);
    lv_scale_set_range(s_ticks, 0, 24);
    lv_scale_set_angle_range(s_ticks, 360);
    lv_scale_set_rotation(s_ticks, 270);
    lv_scale_set_label_show(s_ticks, false);
    lv_obj_set_style_length(s_ticks, 7, LV_PART_ITEMS);
    lv_obj_set_style_line_width(s_ticks, 2, LV_PART_ITEMS);
    lv_obj_set_style_line_color(s_ticks, lv_color_hex(HUD_RIM), LV_PART_ITEMS);
    lv_obj_set_style_length(s_ticks, 12, LV_PART_INDICATOR);
    lv_obj_set_style_line_width(s_ticks, 3, LV_PART_INDICATOR);
    lv_obj_set_style_line_color(s_ticks, lv_color_hex(HUD_CYAN), LV_PART_INDICATOR);
    lv_obj_set_style_arc_opa(s_ticks, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_remove_flag(s_ticks, LV_OBJ_FLAG_CLICKABLE);

    /* The gauge track, then the ghost, then the score - three arcs on the same
     * radius. The ghost is the honesty: score EARNED that the confidence
     * ceiling then took away, shown dimmed rather than silently dropped. */
    s_track = lv_arc_create(scr);
    s_ghost = lv_arc_create(scr);
    s_arc = lv_arc_create(scr);
    lv_obj_set_size(s_arc, 400, 400);
    lv_obj_center(s_arc);
    lv_arc_set_rotation(s_arc, 135);
    lv_arc_set_bg_angles(s_arc, 0, 270);
    lv_arc_set_range(s_arc, 0, 100);
    lv_arc_set_value(s_arc, 0);
    /* An lv_arc is an INPUT widget by default - it tracks your finger like a
     * slider. This one is a gauge reporting a confidence score, so being
     * draggable is not a cosmetic problem: it lets a fingertip overwrite a
     * measurement. Strip the knob and every way of grabbing it. */
    lv_obj_remove_style(s_arc, NULL, LV_PART_KNOB);
    lv_obj_remove_flag(s_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_arc, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_arc, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_arc_set_change_rate(s_arc, 0);
    lv_obj_set_style_arc_opa(s_arc, LV_OPA_TRANSP, LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_arc, 18, LV_PART_INDICATOR);
    lv_obj_set_style_arc_rounded(s_arc, true, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_arc, lv_color_hex(HUD_CYAN), LV_PART_INDICATOR);

    /* The two arcs behind it, configured the same way. */
    {
        lv_obj_t *back[2] = { s_track, s_ghost };
        for (int i = 0; i < 2; i++) {
            lv_obj_t *a = back[i];
            lv_obj_set_size(a, 400, 400);
            lv_obj_center(a);
            lv_arc_set_rotation(a, 135);
            lv_arc_set_bg_angles(a, 0, 270);
            lv_arc_set_range(a, 0, 100);
            lv_arc_set_value(a, i == 0 ? 100 : 0);
            lv_obj_remove_style(a, NULL, LV_PART_KNOB);
            lv_obj_remove_flag(a, LV_OBJ_FLAG_CLICKABLE);
            lv_obj_remove_flag(a, LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_remove_flag(a, LV_OBJ_FLAG_CLICK_FOCUSABLE);
            lv_obj_set_style_arc_opa(a, LV_OPA_TRANSP, LV_PART_MAIN);
            lv_obj_set_style_arc_width(a, i == 0 ? 22 : 16, LV_PART_INDICATOR);
            lv_obj_set_style_arc_rounded(a, true, LV_PART_INDICATOR);
            lv_obj_set_style_arc_color(a,
                lv_color_hex(i == 0 ? HUD_TRACK2 : HUD_TRACK), LV_PART_INDICATOR);
        }
        /* Keep the painting order: track, ghost, score. */
        lv_obj_move_to_index(s_track, 1);
        lv_obj_move_to_index(s_ghost, 2);
        lv_obj_move_to_index(s_arc, 3);
    }

    /* The dark disc the headline sits on, so the number never competes with
     * the gauge behind it. */
    s_core = lv_obj_create(scr);
    lv_obj_remove_style_all(s_core);
    lv_obj_set_size(s_core, 210, 210);
    lv_obj_align(s_core, LV_ALIGN_CENTER, 0, -16);
    lv_obj_set_style_radius(s_core, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_core, lv_color_hex(HUD_VOID), 0);
    lv_obj_set_style_bg_opa(s_core, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_core, lv_color_hex(HUD_TRACK), 0);
    lv_obj_set_style_border_width(s_core, 1, 0);
    lv_obj_remove_flag(s_core, LV_OBJ_FLAG_SCROLLABLE);

    s_lens   = mk_label(scr, &lv_font_montserrat_22, HUD_TEXT,   LV_ALIGN_CENTER, 0, -122, "PHAROS");
    s_pos    = mk_label(scr, &lv_font_montserrat_16, HUD_DIMMER, LV_ALIGN_CENTER, 0,  -92, "");
    s_big    = mk_label(scr, &lv_font_montserrat_48, HUD_TEXT,   LV_ALIGN_CENTER, 0,  -34, "--");
    s_band   = mk_label(scr, &lv_font_montserrat_22, HUD_CYAN,   LV_ALIGN_CENTER, 0,   16, "starting");
    s_detail = mk_label(scr, &lv_font_montserrat_16, HUD_DIM,    LV_ALIGN_CENTER, 0,   52, "");

    /* The description, wrapped. Capped at the widest a multi-line block can be
     * inside this circle without running into the bezel. */
    s_summary = mk_label(scr, &lv_font_montserrat_18, HUD_DIM, LV_ALIGN_CENTER, 0, -6, "");
    lv_label_set_long_mode(s_summary, LV_LABEL_LONG_WRAP);
    lv_obj_set_width(s_summary, 300);
    lv_obj_align(s_summary, LV_ALIGN_CENTER, 0, 84);

    s_hint = mk_label(scr, &lv_font_montserrat_16, HUD_DIMMER, LV_ALIGN_CENTER, 0, 120, "");
    s_rx   = mk_label(scr, &lv_font_montserrat_16, HUD_GREEN,  LV_ALIGN_CENTER, 0, 152,
                      "receive-only");

    /* The permanent receive-only tell, on the bezel where it is always true. */
    s_pip = lv_obj_create(scr);
    lv_obj_remove_style_all(s_pip);
    lv_obj_set_size(s_pip, 8, 8);
    lv_obj_align(s_pip, LV_ALIGN_CENTER, 0, 186);
    lv_obj_set_style_radius(s_pip, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_pip, lv_color_hex(HUD_GREEN), 0);
    lv_obj_set_style_bg_opa(s_pip, LV_OPA_COVER, 0);

    s_toast = mk_label(scr, &lv_font_montserrat_20, HUD_TEXT, LV_ALIGN_CENTER, 0, 86, "");
    lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);

    /* Navigation zones, added last so they sit above everything. Left and
     * right thirds step through the lenses; the centre is the action. */
    mk_zone(scr, LV_ALIGN_LEFT_MID,  150, 466, PHAROS_NAV_PREV);
    mk_zone(scr, LV_ALIGN_RIGHT_MID, 150, 466, PHAROS_NAV_NEXT);
    mk_zone(scr, LV_ALIGN_CENTER,    160, 300, PHAROS_NAV_SELECT);

    s_built = true;
    return true;
}

static void show(lv_obj_t *o, bool on)
{
    if (!o) {
        return;
    }
    if (on) {
        lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
    }
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

    /* Nothing is running, so the reading widgets have nothing honest to show
     * and are hidden rather than left displaying a stale number. */
    show(s_big, false);
    show(s_band, false);
    show(s_detail, false);
    show(s_summary, true);
    show(s_hint, true);

    if (s_lens) {
        lv_label_set_text(s_lens, name ? name : "");
        lv_obj_set_style_text_color(s_lens, lv_color_hex(HUD_TEXT), 0);
    }
    if (s_pos) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%s  %u / %u", team ? team : "",
                 index + 1u, total ? total : 1u);
        lv_label_set_text(s_pos, buf);
    }
    if (s_summary) {
        lv_label_set_text(s_summary, summary ? summary : "");
    }
    if (s_hint) {
        lv_label_set_text(s_hint, "tap centre to start");
    }
    if (s_arc) {
        lv_arc_set_value(s_arc, (int)((index + 1u) * 100u / (total ? total : 1u)));
        lv_obj_set_style_arc_color(s_arc, lv_color_hex(rgb), LV_PART_INDICATOR);
    }
    if (s_ghost) {
        lv_arc_set_value(s_ghost, 0);
    }
    /* No headline number here, so the disc it would sit on is hidden too. */
    show(s_core, false);
    show(s_ticks, true);
    if (s_lens) lv_obj_set_style_text_color(s_lens, lv_color_hex(rgb), 0);
}

/* ---- LIVE: the lens running ------------------------------------------ */

void pharos_hud_advice(const char *advice)
{
    if (!s_built || !s_summary) {
        return;
    }
    /* The advice reuses the summary label: in LIVE the question is no longer
     * "what is this tool" but "what do I do now", and both belong in the same
     * place on the glass. */
    lv_label_set_text(s_summary, advice ? advice : "");
    show(s_summary, advice && advice[0]);
}

void pharos_hud_ceiling(int ceiling)
{
    if (!s_built || !s_ghost) {
        return;
    }
    if (ceiling < 0) ceiling = 0;
    if (ceiling > 100) ceiling = 100;
    lv_arc_set_value(s_ghost, ceiling);
}

void pharos_hud_live(const char *lens, const char *big, const char *band,
                     const char *detail, int score, uint32_t rgb)
{
    if (!s_built) {
        return;
    }
    toast_tick();

    show(s_big, true);
    show(s_band, true);
    show(s_detail, true);
    show(s_summary, false);
    show(s_hint, true);

    if (score < 0) score = 0;
    if (score > 100) score = 100;

    /* The band decides the colour, not the caller: a 78 must look like a 78
     * whichever lens produced it, exactly as in the rendered screens. rgb is
     * honoured only when a caller deliberately overrides (the splash). */
    const uint32_t col = rgb ? rgb : band_colour(score);

    if (lens && s_lens)     lv_label_set_text(s_lens, lens);
    if (big && s_big)       lv_label_set_text(s_big, big);
    if (band && s_band)     lv_label_set_text(s_band, band);
    if (detail && s_detail) lv_label_set_text(s_detail, detail);
    if (s_pos)              lv_label_set_text(s_pos, "");
    if (s_hint)             lv_label_set_text(s_hint, "hold to go back");

    if (s_arc) {
        lv_arc_set_value(s_arc, score);
        lv_obj_set_style_arc_color(s_arc, lv_color_hex(col), LV_PART_INDICATOR);
    }
    if (s_big)  lv_obj_set_style_text_color(s_big, lv_color_hex(col), 0);
    if (s_band) lv_obj_set_style_text_color(s_band, lv_color_hex(col), 0);
    show(s_core, true);
    show(s_ticks, true);
}

void pharos_hud_update(const char *lens, const char *big, const char *band,
                       const char *detail, int score, uint32_t rgb)
{
    pharos_hud_live(lens, big, band, detail, score, rgb);
}

void pharos_hud_colourbars(void)
{
    lv_obj_t *scr = lv_screen_active();
    if (!scr) {
        return;
    }
    /* Start from a clean screen: this is a measurement, not a HUD overlay. */
    lv_obj_clean(scr);
    s_built = false;
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

void pharos_hud_splash(const char *version, bool fence_clean)
{
    if (!pharos_hud_create()) {
        return;
    }
    pharos_hud_live("PHAROS", "\xE2\x97\x89", /* a filled ring: the lamp */
                    fence_clean ? "receive-only" : "FENCE UNVERIFIED",
                    version ? version : "", 0,
                    fence_clean ? HUD_GREEN : 0xE8503F);
    if (s_hint) {
        lv_label_set_text(s_hint, "tap the edges to browse");
    }
    if (s_rx) {
        lv_label_set_text(s_rx, fence_clean ? "fence clean" : "check the fence");
        lv_obj_set_style_text_color(s_rx, lv_color_hex(fence_clean ? HUD_GREEN : 0xE8503F), 0);
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
void pharos_hud_live(const char *lens, const char *big, const char *band,
                     const char *detail, int score, uint32_t rgb)
{
    (void)lens; (void)big; (void)band; (void)detail; (void)score; (void)rgb;
}
void pharos_hud_update(const char *lens, const char *big, const char *band,
                       const char *detail, int score, uint32_t rgb)
{
    (void)lens; (void)big; (void)band; (void)detail; (void)score; (void)rgb;
}
void pharos_hud_colourbars(void)
{
    lv_obj_t *scr = lv_screen_active();
    if (!scr) {
        return;
    }
    /* Start from a clean screen: this is a measurement, not a HUD overlay. */
    lv_obj_clean(scr);
    s_built = false;
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

void pharos_hud_splash(const char *version, bool fence_clean)
{
    (void)version; (void)fence_clean;
}
void pharos_hud_colourbars(void) {}
void pharos_hud_ceiling(int ceiling) { (void)ceiling; }
void pharos_hud_advice(const char *advice) { (void)advice; }

#endif
