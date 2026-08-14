/* Pharos - the on-device LVGL HUD.
 *
 * Only compiled with a real panel; with the vendor BSP off these become
 * no-ops so the firmware still builds and runs headless.
 */
/* sdkconfig.h FIRST - see the long note in pharos_bsp.c. The #if below tests
 * CONFIG_PHAROS_HAS_VENDOR_BSP, and without this the macro is undefined at
 * that point and the whole HUD compiles to no-ops. */
#include "sdkconfig.h"

#include "pharos_hud.h"

#if !defined(PHAROS_HOST) && defined(CONFIG_PHAROS_HAS_VENDOR_BSP)

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
static lv_obj_t *s_lens;
static lv_obj_t *s_big;
static lv_obj_t *s_band;
static lv_obj_t *s_detail;
static lv_obj_t *s_rx;
static bool s_built;

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

    /* The evidence gauge: a 270-degree arc starting at 7 o'clock, matching the
     * renderer's layout so the device and the screenshots agree. */
    s_arc = lv_arc_create(scr);
    lv_obj_set_size(s_arc, 400, 400);
    lv_obj_center(s_arc);
    lv_arc_set_rotation(s_arc, 135);
    lv_arc_set_bg_angles(s_arc, 0, 270);
    lv_arc_set_range(s_arc, 0, 100);
    lv_arc_set_value(s_arc, 0);
    lv_obj_remove_style(s_arc, NULL, LV_PART_KNOB); /* no drag handle */
    lv_obj_set_style_arc_width(s_arc, 16, LV_PART_MAIN);
    lv_obj_set_style_arc_color(s_arc, lv_color_hex(HUD_TRACK), LV_PART_MAIN);
    lv_obj_set_style_arc_width(s_arc, 16, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(s_arc, lv_color_hex(HUD_CYAN), LV_PART_INDICATOR);

    s_lens   = mk_label(scr, &lv_font_montserrat_16, HUD_DIMMER, LV_ALIGN_CENTER, 0, -96, "PHAROS");
    s_big    = mk_label(scr, &lv_font_montserrat_48, HUD_TEXT,   LV_ALIGN_CENTER, 0, -34, "--");
    s_band   = mk_label(scr, &lv_font_montserrat_22, HUD_CYAN,   LV_ALIGN_CENTER, 0,  16, "starting");
    s_detail = mk_label(scr, &lv_font_montserrat_16, HUD_DIM,    LV_ALIGN_CENTER, 0,  56, "");
    s_rx     = mk_label(scr, &lv_font_montserrat_16, HUD_GREEN,  LV_ALIGN_CENTER, 0, 150,
                        "receive-only");

    s_built = true;
    return true;
}

bool pharos_hud_present(void) { return s_built; }

void pharos_hud_update(const char *lens, const char *big, const char *band,
                       const char *detail, int score, uint32_t rgb)
{
    if (!s_built) {
        return;
    }
    if (lens && s_lens)     lv_label_set_text(s_lens, lens);
    if (big && s_big)       lv_label_set_text(s_big, big);
    if (band && s_band)     lv_label_set_text(s_band, band);
    if (detail && s_detail) lv_label_set_text(s_detail, detail);

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

void pharos_hud_splash(const char *version, bool fence_clean)
{
    if (!pharos_hud_create()) {
        return;
    }
    char detail[48];
    snprintf(detail, sizeof(detail), "%s", version ? version : "");
    pharos_hud_update("PHAROS", "\xE2\x97\x89", /* a filled ring: the lamp */
                      fence_clean ? "receive-only" : "FENCE UNVERIFIED",
                      detail, 0, fence_clean ? HUD_GREEN : 0xE8503F);
    if (s_rx) {
        lv_label_set_text(s_rx, fence_clean ? "fence clean" : "check the fence");
        lv_obj_set_style_text_color(s_rx, lv_color_hex(fence_clean ? HUD_GREEN : 0xE8503F), 0);
    }
}

#else /* no panel on this build */

bool pharos_hud_create(void) { return false; }
bool pharos_hud_present(void) { return false; }
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
