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
#define HUD_VOID   0x04090F
#define HUD_TRACK  0x12283A
#define HUD_TEXT   0xEAF6FA
#define HUD_DIM    0x7FA6B5
#define HUD_DIMMER 0x4E7A8C
#define HUD_CYAN   0x1FB6C9
#define HUD_GREEN  0x3DDC84

static lv_obj_t *s_arc;
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

    /* The evidence gauge: a 270-degree arc starting at 7 o'clock. */
    s_arc = lv_arc_create(scr);
    lv_obj_set_size(s_arc, 400, 400);
    lv_obj_center(s_arc);
    lv_arc_set_rotation(s_arc, 135);
    lv_arc_set_bg_angles(s_arc, 0, 270);
    lv_arc_set_range(s_arc, 0, 100);
    lv_arc_set_value(s_arc, 0);
    lv_obj_remove_style(s_arc, NULL, LV_PART_KNOB); /* no drag handle */
    lv_obj_remove_flag(s_arc, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_arc_width(s_arc, 16, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_arc, lv_color_hex(HUD_TRACK), LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_arc, 16, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_arc, lv_color_hex(HUD_CYAN), LV_PART_INDICATOR);

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
    lv_obj_align(s_summary, LV_ALIGN_CENTER, 0, -6);

    s_hint = mk_label(scr, &lv_font_montserrat_16, HUD_DIMMER, LV_ALIGN_CENTER, 0, 120, "");
    s_rx   = mk_label(scr, &lv_font_montserrat_16, HUD_GREEN,  LV_ALIGN_CENTER, 0, 152,
                      "receive-only");

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
}

/* ---- LIVE: the lens running ------------------------------------------ */

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

    if (lens && s_lens)     lv_label_set_text(s_lens, lens);
    if (big && s_big)       lv_label_set_text(s_big, big);
    if (band && s_band)     lv_label_set_text(s_band, band);
    if (detail && s_detail) lv_label_set_text(s_detail, detail);
    if (s_pos)              lv_label_set_text(s_pos, "running");
    if (s_hint)             lv_label_set_text(s_hint, "hold to go back");

    if (score < 0) score = 0;
    if (score > 100) score = 100;
    if (s_arc) {
        lv_arc_set_value(s_arc, score);
        lv_obj_set_style_arc_color(s_arc, lv_color_hex(rgb), LV_PART_INDICATOR);
    }
    if (s_band) {
        lv_obj_set_style_text_color(s_band, lv_color_hex(rgb), 0);
    }
}

void pharos_hud_update(const char *lens, const char *big, const char *band,
                       const char *detail, int score, uint32_t rgb)
{
    pharos_hud_live(lens, big, band, detail, score, rgb);
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
void pharos_hud_splash(const char *version, bool fence_clean)
{
    (void)version; (void)fence_clean;
}

#endif
