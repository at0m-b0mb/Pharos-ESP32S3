/* Pharos - the on-device face. LUMEN.
 *
 * Written from scratch against pharos_style.h. The face this replaces was an
 * aircraft-instrument pastiche - a 24-tick bezel, hairline arcs, text down to
 * 12 px - and it had three problems that were structural rather than
 * cosmetic. They are worth writing down, because every one of them is easy to
 * reintroduce:
 *
 *   TYPE TOO SMALL. 266 ppi means 12 px is about 1.1 mm of cap height. Half
 *   the supporting text was set in it. Nothing here is under 14, and 14 is
 *   used for page counters and nothing else.
 *
 *   NO READING ORDER. Six strings and four pips at similar weight, so the eye
 *   had to hunt for the answer. Here it is always the same, on every screen:
 *
 *       COLOUR -> WORD -> NUMBER -> EVIDENCE -> ACTION
 *
 *   The aura carries the verdict before a single glyph is read. Then the word.
 *   The number is support, set SMALLER than the word it explains - which is
 *   the one thing the old face had exactly backwards, at 48 px against 26.
 *
 *   DECORATION IN THE INVALIDATION PATH. Twenty-four bezel ticks that meant
 *   nothing, redrawn under everything that did. The field log read
 *   painted=257 missed=20 - about 7% of frames dropped while the device drew
 *   furniture. There is no furniture here.
 *
 * ---------------------------------------------------------------------------
 * WHAT WAS KEPT, AND WHY IT IS NOT NEGOTIABLE
 *
 * Four hard-won lessons survive the rewrite unchanged. Each one shipped as a
 * bug once:
 *
 *   1. SCROLLABLE IS REMOVED FROM EVERYTHING. lv_obj_create() sets it, and
 *      content here is deliberately laid out past the screen's bounds. A
 *      smeared tap - which on round glass is most of them - scrolled the whole
 *      face away permanently. That was the "it breaks".
 *
 *   2. EVERY WRITE IS DIRTY-CHECKED. lv_label_set_text() marks a label dirty
 *      even when the string is identical. Ten repaints a second of unchanged
 *      text under an opaque disc is the flicker. In the steady state a repaint
 *      here invalidates nothing at all.
 *
 *   3. NOTHING WRAPS. A LONG_WRAP label's height changes with its text, so it
 *      re-lays-out, so the invalidated region moves. Callers truncate against
 *      the real chord instead - ps_capacity() gives them the number.
 *
 *   4. ONE CALL PER REPAINT PER PAGE. The old API was live() then ceiling()
 *      then advice(), and live() hid a label that advice() showed again, every
 *      frame, forever. A single entry point cannot disagree with itself.
 */
#include "pharos_hud.h"

#include <stdio.h>
#include <string.h>

#include "pharos_style.h"
#include "pharos_theme.h"

#ifdef ESP_PLATFORM

/* Before any ESP header. ESP-IDF does not force-include this, so a CONFIG_
 * test without it silently evaluates false and a whole branch disappears from
 * the build with no diagnostic - which is exactly how the panel was once never
 * touched while sdkconfig said it should be. tools/check_display.sh enforces
 * the ordering; this file has no CONFIG_ test today, and will be correct on
 * the day somebody adds one. */
#include "sdkconfig.h"

#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"

static const char *TAG = "hud";

/* ---- chrome, from the theme ------------------------------------------ */

#define C_RIM    (pharos_theme()->rim)
#define C_TRACK  (pharos_theme()->track)
#define C_TRACK2 (pharos_theme()->track2)
#define C_TEXT   (pharos_theme()->text)
#define C_DIM    (pharos_theme()->dim)
#define C_DIMMER (pharos_theme()->dimmer)
#define C_GHOST  (pharos_theme()->ghost)
#define C_ACCENT (pharos_theme()->accent)

/* ---- the widget tree -------------------------------------------------- */

typedef enum {
    PAGE_SPLASH = 0,
    PAGE_HOME,
    PAGE_BROWSE,
    PAGE_LIVE,
    PAGE_DETAIL,
    PAGE_BARS,
    PAGE_N,
} hud_page_t;

static lv_obj_t *s_page[PAGE_N];
static hud_page_t s_current = PAGE_SPLASH;
static bool s_built;

/* Shared chrome */
static lv_obj_t *s_tell;   /* the permanent receive-only pip */
static lv_obj_t *s_toast;

/* The aura: three nested discs whose COLOUR is the verdict. Opacity is fixed
 * at build time and never touched, so changing the verdict invalidates three
 * fills and nothing else. A radial gradient would be one object and would
 * recompute every frame; this is the cheap way to get a soft falloff. */
#define AURA_N 5
static lv_obj_t *s_aura[AURA_N];
static uint32_t s_aura_rgb = 0xFFFFFFFFu;

/* LIVE */
static lv_obj_t *s_l_ribbon[PHAROS_DISP_HISTORY];
static lv_obj_t *s_l_ctx, *s_l_sim, *s_l_word, *s_l_score, *s_l_detail;
static lv_obj_t *s_l_why, *s_l_action, *s_l_action2;
static lv_obj_t *s_l_ring, *s_l_track, *s_l_ceil;
static lv_obj_t *s_l_chip[PHAROS_DISP_FAMILIES], *s_l_chip_txt[PHAROS_DISP_FAMILIES];
static uint8_t s_ribbon_level[PHAROS_DISP_HISTORY];

/* HOME */
static lv_obj_t *s_h_dot[PHAROS_HUD_HOME_MAX];
static lv_obj_t *s_h_clock, *s_h_word, *s_h_sub, *s_h_active, *s_h_ring, *s_h_track;
static unsigned s_h_n;

/* BROWSE */
static lv_obj_t *s_b_kind, *s_b_kind_txt, *s_b_name, *s_b_l1, *s_b_l2;
static lv_obj_t *s_b_pos, *s_b_go, *s_b_go_txt;

/* DETAIL */
static lv_obj_t *s_d_title, *s_d_hl, *s_d_hr, *s_d_page, *s_d_empty;
static lv_obj_t *s_d_card[PHAROS_HUD_ROWS];
static lv_obj_t *s_d_left[PHAROS_HUD_ROWS], *s_d_right[PHAROS_HUD_ROWS];

/* SPLASH */
static lv_obj_t *s_s_name, *s_s_ver, *s_s_fence;

/* BARS */
static lv_obj_t *s_bar_patch[6], *s_bar_name[6];

/* Touch zones */
static lv_obj_t *s_zone_main[4];
static lv_obj_t *s_zone_row[PHAROS_HUD_ROWS];
static lv_obj_t *s_zone_page[2];
static int s_zones_detail = -1;

static pharos_hud_nav_cb_t s_nav_cb;
static pharos_hud_row_cb_t s_row_cb;
static pharos_hud_home_cb_t s_home_cb;
static uint32_t s_toast_until_ms;

/* ---- dirty-checked setters -------------------------------------------
 *
 * Lesson 2. Every one of these compares before it writes. */

static void set_text(lv_obj_t *o, const char *s)
{
    if (!o) return;
    if (!s) s = "";
    const char *cur = lv_label_get_text(o);
    if (cur && strcmp(cur, s) == 0) return;
    lv_label_set_text(o, s);
}

/* WRITE A STRING THAT MIGHT NOT FIT.
 *
 * The display contract carries up to 96 characters of advice, and the chord
 * at PS_Y_ACTION is 282 px - about 28 characters at PS_TYPE_LABEL. The
 * remainder does not wrap (lesson 3) and it does not shrink; it runs off the
 * side of a circle, where the glass curvature makes it unreadable before the
 * pixels even stop.
 *
 * So every variable-length string goes through here, which truncates against
 * the REAL chord at the offset it is drawn at and marks the cut with an
 * ellipsis. The bounds checker in tools/render is what found this: five
 * primitives escaped the safe radius on the first LUMEN render, four of them
 * advice lines from the actual engines.
 *
 * Truncating is a last resort, not a licence - an engine whose advice needs
 * cutting should shorten its advice. This stops it looking broken. */
static void set_text_fit(lv_obj_t *o, ps_type_t t, int16_t dy, const char *s)
{
    if (!o) return;
    if (!s || !*s) { set_text(o, ""); return; }

    const unsigned cap = ps_capacity(t, dy, PR_SAFE_R);
    const unsigned len = (unsigned)strlen(s);
    if (cap == 0u) { set_text(o, ""); return; }
    if (len <= cap) { set_text(o, s); return; }

    char buf[128];
    unsigned n = cap < sizeof(buf) - 1u ? cap : (unsigned)sizeof(buf) - 1u;
    if (n > 1u) n -= 1u; /* room for the ellipsis */
    /* Cut on a word boundary when one is close, so the tail is not a
     * fragment of a word - "Preserve the l..." reads as damage. */
    unsigned cut = n;
    while (cut > 0u && cut + 6u > n && s[cut] != ' ') cut--;
    if (cut == 0u) cut = n;
    memcpy(buf, s, cut);
    /* One dot, not three: an ellipsis costs three characters of the very
     * width we are already short of. */
    buf[cut] = '.';
    buf[cut + 1u] = '\0';
    set_text(o, buf);
}

static void set_fg(lv_obj_t *o, uint32_t rgb)
{
    if (!o) return;
    const lv_color_t want = lv_color_hex(rgb);
    if (lv_color_eq(lv_obj_get_style_text_color(o, LV_PART_MAIN), want)) return;
    lv_obj_set_style_text_color(o, want, 0);
}

static void set_bg(lv_obj_t *o, uint32_t rgb)
{
    if (!o) return;
    const lv_color_t want = lv_color_hex(rgb);
    if (lv_color_eq(lv_obj_get_style_bg_color(o, LV_PART_MAIN), want)) return;
    lv_obj_set_style_bg_color(o, want, 0);
}

static void set_arc_rgb(lv_obj_t *o, uint32_t rgb)
{
    if (!o) return;
    const lv_color_t want = lv_color_hex(rgb);
    if (lv_color_eq(lv_obj_get_style_arc_color(o, LV_PART_INDICATOR), want)) return;
    lv_obj_set_style_arc_color(o, want, LV_PART_INDICATOR);
}

static void show(lv_obj_t *o, bool on)
{
    if (!o) return;
    const bool hidden = lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN);
    if (on == !hidden) return;
    if (on) lv_obj_remove_flag(o, LV_OBJ_FLAG_HIDDEN);
    else    lv_obj_add_flag(o, LV_OBJ_FLAG_HIDDEN);
}

/* THE NEEDLE GLIDES.
 *
 * The UI loop hands over a reading ten times a second; setting the arc
 * directly makes it step ten times a second, which is what "not smooth" looks
 * like. LVGL is already awake at the panel's refresh rate, so let it
 * interpolate. The duration is capped: an arc that took a leisurely second to
 * swing up to an alarm would be prettier and later. */
static void arc_anim_cb(void *obj, int32_t v) { lv_arc_set_value((lv_obj_t *)obj, v); }

static void set_arc_value(lv_obj_t *o, int v)
{
    if (!o) return;
    if (v < 0) v = 0;
    if (v > 100) v = 100;
    const int cur = (int)lv_arc_get_value(o);
    if (cur == v) return;

    lv_anim_delete(o, arc_anim_cb);
    int delta = v - cur;
    if (delta < 0) delta = -delta;
    if (delta <= 2) { lv_arc_set_value(o, v); return; }

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, o);
    lv_anim_set_exec_cb(&a, arc_anim_cb);
    lv_anim_set_values(&a, cur, v);
    lv_anim_set_duration(&a, (uint32_t)(delta * PS_MS_MOVE / 100) + 60u);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_start(&a);
}

static const lv_font_t *font_of(ps_type_t t)
{
    switch (t) {
    case PS_TYPE_MICRO:  return &lv_font_montserrat_14;
    case PS_TYPE_LABEL:  return &lv_font_montserrat_16;
    case PS_TYPE_BODY:   return &lv_font_montserrat_20;
    case PS_TYPE_TITLE:  return &lv_font_montserrat_28;
    case PS_TYPE_HERO:   return &lv_font_montserrat_36;
    case PS_TYPE_METRIC: return &lv_font_montserrat_48;
    default:             return &lv_font_montserrat_16;
    }
}

/* The one place a lens' stated tone becomes a colour, so red means the same
 * thing on every page in the product. */
static uint32_t tone_colour(pharos_tone_t t)
{
    switch (t) {
    case PHAROS_TONE_GOOD: return PS_GOOD;
    case PHAROS_TONE_WARN: return PS_WARN;
    case PHAROS_TONE_BAD:  return PS_BAD;
    case PHAROS_TONE_DIM:  return C_DIMMER;
    case PHAROS_TONE_NEUTRAL:
    default:               return C_TEXT;
    }
}

/* ---- builders --------------------------------------------------------- */

/* lv_obj_create() hands back something scrollable, clickable and bordered.
 * Every one of those defaults is wrong here, and the scrollable one is
 * lesson 1. */
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

static lv_obj_t *mk_label(lv_obj_t *parent, ps_type_t t, uint32_t rgb,
                          int dx, int dy, const char *text)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(l, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_text_font(l, font_of(t), 0);
    lv_obj_set_style_text_color(l, lv_color_hex(rgb), 0);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP); /* lesson 3 */
    lv_label_set_text(l, text ? text : "");
    lv_obj_align(l, LV_ALIGN_CENTER, dx, dy);
    return l;
}

/* A rounded surface that lifts off the true-black field. This is the one
 * shape LUMEN uses for everything that is a THING - a card, a chip, a
 * button - so the operator learns one affordance instead of five. */
static lv_obj_t *mk_surface(lv_obj_t *parent, int w, int h, int dx, int dy,
                            uint32_t rgb, uint8_t opa, int radius)
{
    lv_obj_t *o = mk_box(parent);
    lv_obj_set_size(o, w, h);
    lv_obj_align(o, LV_ALIGN_CENTER, dx, dy);
    lv_obj_set_style_radius(o, radius, 0);
    lv_obj_set_style_bg_color(o, lv_color_hex(rgb), 0);
    lv_obj_set_style_bg_opa(o, opa, 0);
    return o;
}

/* A gauge arc. Emphatically NOT an input: lv_arc is a slider by default and
 * this one reports a confidence score, so being draggable would let a
 * fingertip overwrite a measurement. */
static lv_obj_t *mk_arc(lv_obj_t *parent, int size, int width, uint32_t rgb,
                        int value, int rotation, int span)
{
    lv_obj_t *a = lv_arc_create(parent);
    lv_obj_set_size(a, size, size);
    lv_obj_center(a);
    lv_arc_set_rotation(a, rotation);
    lv_arc_set_bg_angles(a, 0, span);
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

/* ---- the aura ---------------------------------------------------------
 *
 * The instant read. Three nested discs of the verdict colour, mixed hard
 * towards black and stacked so the opacity compounds into a soft falloff.
 *
 * This is the single biggest usability change in LUMEN: the operator knows
 * the answer from across a room, before reading anything. Every other element
 * on the screen exists to explain a conclusion the colour already delivered. */
static void aura_build(lv_obj_t *parent)
{
    /* FIVE LAYERS, NOT THREE. Three showed as three visible rings on the
     * first render - a 66 px step between discs is a hard edge, not a
     * falloff. Five at ~33 px with gentler opacity reads as a glow. */
    const int r[AURA_N] = { PS_AURA_R, 128, 108, 88, 68 };
    const uint8_t opa[AURA_N] = { 34, 40, 46, 52, 58 };
    for (unsigned i = 0; i < AURA_N; i++) {
        s_aura[i] = mk_surface(parent, r[i] * 2, r[i] * 2, 0, 0,
                               ps_tint(PS_GOOD, 42), opa[i], LV_RADIUS_CIRCLE);
    }
}

static void aura_set(uint32_t rgb)
{
    if (rgb == s_aura_rgb) return; /* the whole point: verdicts change rarely */
    s_aura_rgb = rgb;
    /* Mixed towards black so the hero text stays the brightest thing inside
     * it. An aura at full strength turns the word into a silhouette. */
    const uint8_t mix[AURA_N] = { 20, 26, 32, 38, 44 };
    for (unsigned i = 0; i < AURA_N; i++) {
        set_bg(s_aura[i], ps_tint(rgb, mix[i]));
    }
}

static void split_two(const char *s, unsigned cap, char *a, unsigned an,
                      char *b, unsigned bn);

/* ---- pages ------------------------------------------------------------ */

static void zones_mode(bool detail);

static void page_show(hud_page_t want)
{
    if (want == s_current) return;
    for (unsigned i = 0; i < PAGE_N; i++) {
        show(s_page[i], i == (unsigned)want);
    }
    /* The aura belongs to HOME and LIVE. On BROWSE and DETAIL it would be
     * colour with nothing to say. */
    const bool aura_on = (want == PAGE_HOME || want == PAGE_LIVE);
    for (unsigned i = 0; i < AURA_N; i++) show(s_aura[i], aura_on);
    show(s_tell, want != PAGE_SPLASH && want != PAGE_BARS);
    zones_mode(want == PAGE_DETAIL);
    s_current = want;
}

/* ---- touch ------------------------------------------------------------ */

static void log_touch(const char *what, unsigned tag)
{
    ESP_LOGI(TAG, "touch: %s %u", what, tag);
}

static void nav_event(lv_event_t *e)
{
    const unsigned which = (unsigned)(uintptr_t)lv_event_get_user_data(e);
    log_touch("nav", which);
    if (!s_nav_cb) return;

    /* On HOME a press near a dot means that watch, not "next". The hit test
     * lives here because the ring's geometry is the HUD's; asking the UI
     * layer to reach into LVGL for a touch point would put the coordinate
     * system in two places and they would drift. */
    if (s_current == PAGE_HOME && s_home_cb) {
        lv_indev_t *indev = lv_indev_active();
        if (indev) {
            lv_point_t p;
            lv_indev_get_point(indev, &p);
            const int hit = pharos_hud_home_hit((int16_t)p.x, (int16_t)p.y);
            if (hit >= 0) { s_home_cb((unsigned)hit); return; }
            if (pr_radius_of((int16_t)p.x, (int16_t)p.y) < PR_CORE_R) {
                s_home_cb(PHAROS_HUD_HOME_CORE); return;
            }
        }
    }
    s_nav_cb((pharos_nav_t)which);
}

static void row_event(lv_event_t *e)
{
    const unsigned row = (unsigned)(uintptr_t)lv_event_get_user_data(e);
    log_touch("row", row);
    if (s_row_cb) s_row_cb(row);
}

static lv_obj_t *mk_zone(lv_obj_t *parent, int w, int h, int dx, int dy,
                         lv_event_cb_t cb, unsigned tag)
{
    lv_obj_t *z = lv_obj_create(parent);
    lv_obj_remove_style_all(z);
    lv_obj_remove_flag(z, LV_OBJ_FLAG_SCROLLABLE); /* lesson 1 */
    lv_obj_set_scrollbar_mode(z, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_size(z, w, h);
    lv_obj_align(z, LV_ALIGN_CENTER, dx, dy);
    lv_obj_add_flag(z, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_bg_opa(z, LV_OPA_TRANSP, 0);
    lv_obj_add_event_cb(z, cb, LV_EVENT_CLICKED, (void *)(uintptr_t)tag);
    return z;
}

static void zones_mode(bool detail)
{
    const int want = detail ? 1 : 0;
    if (s_zones_detail == want) return;
    s_zones_detail = want;
    for (unsigned i = 0; i < 4; i++) show(s_zone_main[i], !detail);
    for (unsigned i = 0; i < PHAROS_HUD_ROWS; i++) show(s_zone_row[i], detail);
    for (unsigned i = 0; i < 2; i++) show(s_zone_page[i], detail);
}

/* ---- build ------------------------------------------------------------ */

bool pharos_hud_create(void)
{
    if (s_built) return true;
    lv_obj_t *scr = lv_screen_active();
    if (!scr) return false;

    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE); /* lesson 1 */
    lv_obj_set_scrollbar_mode(scr, LV_SCROLLBAR_MODE_OFF);
    lv_obj_set_style_bg_color(scr, lv_color_hex(PS_VOID), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_pad_all(scr, 0, LV_PART_MAIN);

    /* The aura sits under every page, built first so it composites beneath. */
    aura_build(scr);
    for (unsigned i = 0; i < AURA_N; i++) show(s_aura[i], false);

    for (unsigned i = 0; i < PAGE_N; i++) {
        s_page[i] = mk_box(scr);
        lv_obj_set_size(s_page[i], PR_W, PR_H);
        lv_obj_center(s_page[i]);
        lv_obj_add_flag(s_page[i], LV_OBJ_FLAG_HIDDEN);
    }

    /* The permanent receive-only tell, on the rim where it is always true.
     * Green, small, and never animated - it is a fact, not an alert. */
    s_tell = mk_surface(scr, 10, 10, 0, PS_Y_TELL, PS_GOOD, LV_OPA_COVER,
                        LV_RADIUS_CIRCLE);
    show(s_tell, false);

    /* ---- SPLASH ---- */
    {
        lv_obj_t *p = s_page[PAGE_SPLASH];
        s_s_name  = mk_label(p, PS_TYPE_METRIC, C_TEXT, 0, -34, "PHAROS");
        s_s_ver   = mk_label(p, PS_TYPE_BODY, C_DIM, 0, 24, "");
        s_s_fence = mk_label(p, PS_TYPE_LABEL, PS_GOOD, 0, 78, "");
    }

    /* ---- HOME ----
     *
     * THE LABELS ARE GONE, AND THAT IS THE FIX.
     *
     * The old ring wrote a name beside every dot. Fourteen names do not fit
     * on a 466 px circle at any radius - the capacity function said twelve -
     * so names were being drawn through the headline, and the workaround was
     * a rule about which ones were "worth naming". Twenty-one lenses made
     * that worse.
     *
     * The names were never the point. The question HOME answers is "is
     * anything wrong", and for that a ring of coloured dots is complete: you
     * count the ones that are not green. Which watch is which matters only
     * once you have decided to look, and then it is one name in the middle -
     * always legible, never clipped, because there is only ever one.
     *
     * Removing the labels removed the clipping bug, the capacity rule and the
     * staggered-radius machinery all at once. */
    {
        lv_obj_t *p = s_page[PAGE_HOME];
        s_h_track = mk_arc(p, PS_RING_R * 2, 6, C_TRACK2, 100, 135, 270);
        s_h_ring  = mk_arc(p, PS_RING_R * 2, PS_RING_W, PS_GOOD, 0, 135, 270);
        for (unsigned i = 0; i < PHAROS_HUD_HOME_MAX; i++) {
            s_h_dot[i] = mk_surface(p, 14, 14, 0, 0, C_TRACK, LV_OPA_COVER,
                                    LV_RADIUS_CIRCLE);
            show(s_h_dot[i], false);
        }
        s_h_clock  = mk_label(p, PS_TYPE_LABEL, C_DIMMER, 0, PS_Y_CLOCK, "");
        s_h_word   = mk_label(p, PS_TYPE_HERO,  C_TEXT,   0, PS_Y_HERO,   "");
        s_h_sub    = mk_label(p, PS_TYPE_BODY,  C_DIM,    0, PS_Y_METRIC, "");
        s_h_active = mk_label(p, PS_TYPE_LABEL, C_ACCENT, 0, PS_Y_DETAIL + 16, "");
    }

    /* ---- BROWSE ----
     *
     * The answer to "I don't know what this device does". You read what a
     * tool is for BEFORE you start it, so this page leads with plain English
     * and keeps the evocative name subordinate to it. KARMA, SQUALL and
     * MIRAGE tell a newcomer nothing; "is a rogue AP answering for networks
     * it does not have" tells them everything. */
    {
        lv_obj_t *p = s_page[PAGE_BROWSE];
        s_b_kind     = mk_surface(p, 132, 32, 0, -158, C_TRACK, LV_OPA_COVER, 16);
        s_b_kind_txt = mk_label(p, PS_TYPE_LABEL, C_DIMMER, 0, -158, "");
        s_b_name     = mk_label(p, PS_TYPE_TITLE, C_TEXT, 0, -96, "");
        /* Two fixed lines rather than a wrapping block: lesson 3. The caller
         * splits; the height never changes; nothing re-lays-out. */
        s_b_l1 = mk_label(p, PS_TYPE_BODY, C_DIM, 0, -26, "");
        s_b_l2 = mk_label(p, PS_TYPE_BODY, C_DIM, 0,   6, "");
        s_b_pos      = mk_label(p, PS_TYPE_MICRO, C_DIMMER, 0, 62, "");
        s_b_go       = mk_surface(p, 190, 56, 0, 130, C_ACCENT, LV_OPA_COVER, 28);
        s_b_go_txt   = mk_label(p, PS_TYPE_BODY, PS_VOID, 0, 130, "START");
    }

    /* ---- LIVE ---- */
    {
        lv_obj_t *p = s_page[PAGE_LIVE];

        s_l_track = mk_arc(p, PS_RING_R * 2, 6, C_TRACK2, 100, 135, 270);
        s_l_ring  = mk_arc(p, PS_RING_R * 2, PS_RING_W, PS_GOOD, 0, 135, 270);

        /* The ceiling: a hard stop drawn as a TICK across the arc, never as a
         * second band. Two bars of the same weight showing different numbers
         * read as a bug rather than as a limit. */
        s_l_ceil = mk_arc(p, PS_RING_R * 2, PS_RING_W + 8, PS_BAD, 0, 135, 270);
        lv_obj_set_style_arc_rounded(s_l_ceil, false, LV_PART_INDICATOR);
        lv_arc_set_angles(s_l_ceil, 0, 0);

        /* THE RIBBON IS STRAIGHT NOW.
         *
         * The old one curved along an arc, which cost sixteen lv_line objects
         * whose bounding boxes each spanned most of the screen - so one bar
         * changing invalidated the whole panel. Straight bars have tight
         * boxes. It also reads better: a timeline is a line. */
        for (unsigned i = 0; i < PHAROS_DISP_HISTORY; i++) {
            const int pitch = 14;
            const int dx = -(int)(PHAROS_DISP_HISTORY - 1) * pitch / 2 + (int)i * pitch;
            s_l_ribbon[i] = mk_surface(p, 7, 6, dx, PS_Y_RIBBON, C_TRACK,
                                       LV_OPA_COVER, 3);
            s_ribbon_level[i] = 0xFF;
        }

        s_l_sim = mk_label(p, PS_TYPE_LABEL, PS_WARN, 0, PS_Y_SIM,
                           "SIMULATION - NOT THIS ROOM");
        lv_obj_add_flag(s_l_sim, LV_OBJ_FLAG_HIDDEN);
        s_l_ctx    = mk_label(p, PS_TYPE_LABEL, C_DIMMER, 0, PS_Y_CONTEXT, "");

        /* The word leads and the number supports it. The old face had this
         * exactly backwards - 48 px of score above 26 px of meaning - which
         * is why it read as a number generator rather than as a judgement. */
        s_l_word   = mk_label(p, PS_TYPE_HERO,  C_TEXT, 0, PS_Y_HERO, "");
        s_l_score  = mk_label(p, PS_TYPE_TITLE, C_DIM,  0, PS_Y_METRIC, "");
        s_l_detail = mk_label(p, PS_TYPE_LABEL, C_DIM,  0, PS_Y_DETAIL, "");

        const int16_t cw = ps_chip_w(PHAROS_DISP_FAMILIES);
        for (unsigned i = 0; i < PHAROS_DISP_FAMILIES; i++) {
            const int total = cw * PHAROS_DISP_FAMILIES
                            + PS_CARD_GAP * (PHAROS_DISP_FAMILIES - 1);
            const int dx = -total / 2 + (int)i * (cw + PS_CARD_GAP) + cw / 2;
            s_l_chip[i]     = mk_surface(p, cw, 30, dx, PS_Y_CHIPS, C_TRACK,
                                         LV_OPA_COVER, 15);
            s_l_chip_txt[i] = mk_label(p, PS_TYPE_LABEL, C_DIMMER, dx, PS_Y_CHIPS, "");
            show(s_l_chip[i], false);
            show(s_l_chip_txt[i], false);
        }

        s_l_why    = mk_label(p, PS_TYPE_LABEL, C_DIM,    0, PS_Y_ACTION - 26, "");
        s_l_action = mk_label(p, PS_TYPE_LABEL, C_ACCENT, 0, PS_Y_ACTION + 4, "");
    }

    /* ---- DETAIL ---- */
    {
        lv_obj_t *p = s_page[PAGE_DETAIL];
        s_d_title = mk_label(p, PS_TYPE_LABEL, C_DIMMER, 0, -196, "");
        /* Pulled in from +/-110. At dy=-166 the chord is narrow and two
         * headers have to share it; the outer corner of each was landing
         * outside the safe radius, which the render bounds check caught. */
        s_d_hl    = mk_label(p, PS_TYPE_MICRO, C_DIMMER, -72, -164, "");
        s_d_hr    = mk_label(p, PS_TYPE_MICRO, C_DIMMER,  72, -164, "");

        const int stack = PHAROS_HUD_ROWS * PS_CARD_H + (PHAROS_HUD_ROWS - 1) * PS_CARD_GAP;
        for (unsigned i = 0; i < PHAROS_HUD_ROWS; i++) {
            const int dy = -stack / 2 + (int)i * (PS_CARD_H + PS_CARD_GAP)
                         + PS_CARD_H / 2 + 8;
            /* The card is as wide as the circle allows at ITS OWN offset -
             * not the screen width. Sizing every card the same is how the
             * top and bottom ones end up with their corners off the glass. */
            const int16_t half = pr_chord_halfwidth(PR_SAFE_R,
                (int16_t)(dy >= 0 ? dy + PS_CARD_H / 2 : dy - PS_CARD_H / 2));
            const int w = (half - 6) * 2;
            s_d_card[i]  = mk_surface(p, w, PS_CARD_H, 0, dy, C_TRACK2, LV_OPA_COVER, 14);
            s_d_left[i]  = mk_label(p, PS_TYPE_BODY,  C_TEXT, 0, dy, "");
            s_d_right[i] = mk_label(p, PS_TYPE_BODY,  C_DIM,  0, dy, "");
            lv_obj_align(s_d_left[i],  LV_ALIGN_CENTER, -w / 2 + 18, dy);
            lv_obj_set_style_text_align(s_d_left[i], LV_TEXT_ALIGN_LEFT, 0);
            lv_obj_align(s_d_right[i], LV_ALIGN_CENTER,  w / 2 - 18, dy);
            lv_obj_set_style_text_align(s_d_right[i], LV_TEXT_ALIGN_RIGHT, 0);
        }
        s_d_page  = mk_label(p, PS_TYPE_MICRO, C_DIMMER, 0, PS_Y_PAGE, "");
        s_d_empty = mk_label(p, PS_TYPE_BODY, C_DIMMER, 0, 0, "Nothing found yet");
        show(s_d_empty, false);
    }

    /* ---- BARS (the `screen test` command) ---- */
    {
        lv_obj_t *p = s_page[PAGE_BARS];
        static const uint32_t rgb[6] = { 0xFF0000, 0x00FF00, 0x0000FF,
                                         0xFFFFFF, 0xFFC34A, 0x000000 };
        static const char *nm[6] = { "RED", "GREEN", "BLUE", "WHITE", "AMBER", "BLACK" };
        for (unsigned i = 0; i < 6; i++) {
            const int dy = -190 + (int)i * 76;
            s_bar_patch[i] = mk_surface(p, 300, 72, 0, dy, rgb[i], LV_OPA_COVER, 8);
            s_bar_name[i]  = mk_label(p, PS_TYPE_LABEL,
                                      (i == 3 || i == 4) ? 0x000000u : 0xFFFFFFu,
                                      0, dy, nm[i]);
        }
    }

    /* ---- toast ---- */
    s_toast = mk_label(scr, PS_TYPE_LABEL, PS_VOID, 0, 150, "");
    lv_obj_set_style_bg_color(s_toast, lv_color_hex(C_ACCENT), 0);
    lv_obj_set_style_bg_opa(s_toast, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(s_toast, 10, 0);
    lv_obj_set_style_radius(s_toast, 16, 0);
    lv_obj_add_flag(s_toast, LV_OBJ_FLAG_HIDDEN);

    /* ---- touch zones ----
     *
     * Four quadrant-ish zones on the ordinary pages, swapped for row targets
     * on DETAIL. Every one is well over a fingertip; see ps_touchable(). */
    s_zone_main[0] = mk_zone(scr, 150, 300, -158,  0, nav_event, PHAROS_NAV_PREV);
    s_zone_main[1] = mk_zone(scr, 150, 300,  158,  0, nav_event, PHAROS_NAV_NEXT);
    s_zone_main[2] = mk_zone(scr, 190, 190,    0,  0, nav_event, PHAROS_NAV_SELECT);
    s_zone_main[3] = mk_zone(scr, 300,  84,    0, 176, nav_event, PHAROS_NAV_DETAIL);
    for (unsigned i = 0; i < PHAROS_HUD_ROWS; i++) {
        const int stack = PHAROS_HUD_ROWS * PS_CARD_H + (PHAROS_HUD_ROWS - 1) * PS_CARD_GAP;
        const int dy = -stack / 2 + (int)i * (PS_CARD_H + PS_CARD_GAP) + PS_CARD_H / 2 + 8;
        s_zone_row[i] = mk_zone(scr, 380, PS_CARD_H + PS_CARD_GAP, 0, dy, row_event, i);
        show(s_zone_row[i], false);
    }
    s_zone_page[0] = mk_zone(scr, 300, 74, 0, -186, nav_event, PHAROS_NAV_PREV);
    s_zone_page[1] = mk_zone(scr, 300, 74, 0,  186, nav_event, PHAROS_NAV_NEXT);
    show(s_zone_page[0], false);
    show(s_zone_page[1], false);
    s_zones_detail = 0;

    s_built = true;
    s_current = PAGE_N; /* force the first page_show through */
    page_show(PAGE_SPLASH);
    ESP_LOGI(TAG, "LUMEN face built");
    return true;
}

/* A theme change is a rebuild rather than a walk over the widgets: the walk
 * has to know which of forty objects took which of twelve colours, and that
 * knowledge would then exist in two places and drift. Deleting and rebuilding
 * cannot drift. */
void pharos_hud_rebuild(void)
{
    if (!s_built) return;
    lv_obj_t *scr = lv_screen_active();
    if (!scr) return;
    const hud_page_t was = s_current;
    lv_obj_clean(scr);
    s_built = false;
    s_aura_rgb = 0xFFFFFFFFu;
    s_zones_detail = -1;
    if (pharos_hud_create() && was < PAGE_N) {
        page_show(was);
    }
}

bool pharos_hud_present(void) { return s_built; }
void pharos_hud_set_nav_cb(pharos_hud_nav_cb_t cb) { s_nav_cb = cb; }
void pharos_hud_set_row_cb(pharos_hud_row_cb_t cb) { s_row_cb = cb; }
void pharos_hud_set_home_cb(pharos_hud_home_cb_t cb) { s_home_cb = cb; }

/* ---- toast ------------------------------------------------------------ */

void pharos_hud_toast(const char *msg)
{
    if (!s_built || !msg) return;
    set_text(s_toast, msg);
    show(s_toast, true);
    s_toast_until_ms = (uint32_t)(esp_timer_get_time() / 1000) + 1600u;
}

static void toast_tick(void)
{
    if (!s_toast_until_ms) return;
    if ((uint32_t)(esp_timer_get_time() / 1000) >= s_toast_until_ms) {
        show(s_toast, false);
        s_toast_until_ms = 0;
    }
}

/* ---- SPLASH ----------------------------------------------------------- */

void pharos_hud_splash(const char *version, bool fence_clean)
{
    if (!s_built) return;
    page_show(PAGE_SPLASH);
    set_text(s_s_ver, version ? version : "");
    set_text(s_s_fence, fence_clean ? "RECEIVE ONLY - FENCE CLEAN"
                                    : "FENCE NOT PROVEN");
    set_fg(s_s_fence, fence_clean ? PS_GOOD : PS_BAD);
}

/* ---- HOME ------------------------------------------------------------- */

/* WHERE WATCH i SITS ON THE RING.
 *
 * Along the same 270 degrees the score arc covers, not all the way round.
 * Spreading them over the full circle put a dot in the gauge's bottom notch,
 * on top of the clock - and that notch is the one piece of rim the reading
 * never uses, which is exactly why the clock was moved there. Sharing the
 * arc's own span also makes the dots read as part of the instrument rather
 * than as a second, unrelated ring.
 *
 * Degrees clockwise from 12 o'clock, matching pr_polar(). */
static float home_dot_deg(unsigned i, unsigned n)
{
    if (n <= 1u) return 0.0f;
    return 225.0f + 270.0f * (float)i / (float)(n - 1u);
}

static uint32_t home_dot_colour(uint8_t st)
{
    /* ptw_state_t, carried as a plain byte so this file does not depend on
     * the engine's header. 0 quiet, 1 noted, 2 finding, 3 alarm. */
    switch (st) {
    case 0:  return PS_GOOD;
    case 1:  return PS_WARN;
    case 2:  return PS_HIGH;
    case 3:  return PS_BAD;
    default: return C_TRACK;
    }
}

void pharos_hud_home(const struct pharos_hud_home *h)
{
    if (!s_built || !h) return;
    page_show(PAGE_HOME);
    toast_tick();

    unsigned n = h->n;
    if (n > PHAROS_HUD_HOME_MAX) n = PHAROS_HUD_HOME_MAX;
    s_h_n = n;

    /* One dot per watch, evenly round the rim. No labels - see the note at
     * the build site. The dot's colour is what it last found; its OPACITY is
     * how long ago, because there is one radio and the watches take turns. A
     * ring that drew a forty-second-old reading in the same ink as a live one
     * would be claiming sixteen receivers this device does not have. */
    for (unsigned i = 0; i < PHAROS_HUD_HOME_MAX; i++) {
        if (i >= n) { show(s_h_dot[i], false); continue; }
        const pr_point_t pt = pr_polar(PS_RING_R - 28, home_dot_deg(i, n));
        const bool active = ((int)i == h->active);
        const int size = active ? 20 : 14;
        lv_obj_set_size(s_h_dot[i], size, size);
        lv_obj_align(s_h_dot[i], LV_ALIGN_CENTER, pt.x - PR_CX, pt.y - PR_CY);
        set_bg(s_h_dot[i], home_dot_colour(h->state[i]));
        /* fade 0 = fresh. Never below 25%: a dot that faded to nothing would
         * read as "this watch is gone" rather than "not looked at lately". */
        const uint8_t f = h->fade[i];
        uint8_t opa = (f >= 3u) ? 64u : (uint8_t)(255u - f * 60u);
        if (active) opa = 255u;
        lv_obj_set_style_bg_opa(s_h_dot[i], opa, 0);
        show(s_h_dot[i], true);
    }

    const uint32_t rgb = ps_alert_colour(h->worst_state);
    aura_set(rgb);
    set_arc_rgb(s_h_ring, rgb);
    set_arc_value(s_h_ring, h->worst_score);

    set_text(s_h_clock, h->clock);
    set_text_fit(s_h_word, PS_TYPE_HERO, PS_Y_HERO, h->headline);
    set_fg(s_h_word, (h->worst_state == 0) ? C_TEXT : rgb);
    set_text_fit(s_h_sub, PS_TYPE_BODY, PS_Y_METRIC, h->sub);

    /* The one name on this screen, and it is always legible because there is
     * only ever one of it. */
    const char *act = (h->active >= 0 && h->active < (int)n) ? h->label[h->active] : NULL;
    set_text_fit(s_h_active, PS_TYPE_LABEL, PS_Y_DETAIL + 16, act ? act : "");
}

int pharos_hud_home_hit(int16_t x, int16_t y)
{
    if (!s_built || s_h_n == 0) return -1;
    /* Same layout the paint used, so the thing you press is the thing you
     * saw. A separate hit-test table is a second source of truth. */
    for (unsigned i = 0; i < s_h_n; i++) {
        const pr_point_t pt = pr_polar(PS_RING_R - 28, home_dot_deg(i, s_h_n));
        const int dx = (int)x - pt.x, dy = (int)y - pt.y;
        /* 30 px of slop, not 7: the dot is the TARGET, not the button. A
         * fingertip is ~9 mm and the dots are 14 px apart from nothing. */
        if (dx * dx + dy * dy <= 30 * 30) return (int)i;
    }
    return -1;
}

/* ---- BROWSE ----------------------------------------------------------- */

/* Split a summary onto two fixed lines at a word boundary. Fixed lines rather
 * than wrapping is lesson 3; splitting on a space rather than mid-word is
 * because "detec / tor" reads as a rendering fault. */
static void split_two(const char *s, unsigned cap, char *a, unsigned an,
                      char *b, unsigned bn)
{
    a[0] = b[0] = '\0';
    if (!s || !*s) return;
    const unsigned len = (unsigned)strlen(s);
    if (len <= cap) { snprintf(a, an, "%s", s); return; }

    unsigned cut = cap;
    while (cut > 0 && s[cut] != ' ') cut--;
    if (cut == 0) cut = cap; /* one very long word: hard-cut it */

    unsigned na = cut < an - 1 ? cut : an - 1;
    memcpy(a, s, na); a[na] = '\0';

    const char *rest = s + cut + (s[cut] == ' ' ? 1 : 0);
    snprintf(b, bn, "%.*s", (int)(bn - 1), rest);
    /* If the tail still will not fit, ellipsize rather than clip mid-glyph. */
    if (strlen(b) > cap) { b[cap - 1] = '.'; b[cap] = '.'; b[cap + 1] = '\0'; }
}

void pharos_hud_browse(const char *name, const char *summary, const char *team,
                       unsigned index, unsigned total, uint32_t rgb)
{
    if (!s_built) return;
    page_show(PAGE_BROWSE);
    toast_tick();

    set_text_fit(s_b_name, PS_TYPE_TITLE, -96, name);
    set_fg(s_b_name, C_TEXT);

    set_text(s_b_kind_txt, team ? team : "");
    set_bg(s_b_kind, ps_tint(rgb ? rgb : C_ACCENT, 34));
    set_fg(s_b_kind_txt, rgb ? rgb : C_ACCENT);

    char l1[40], l2[40];
    const unsigned cap = ps_capacity(PS_TYPE_BODY, -26, PR_SAFE_R);
    split_two(summary, cap < 39u ? cap : 39u, l1, sizeof l1, l2, sizeof l2);
    set_text(s_b_l1, l1);
    set_text(s_b_l2, l2);

    char pos[24];
    snprintf(pos, sizeof pos, "%u of %u", index + 1u, total);
    set_text(s_b_pos, pos);

    set_bg(s_b_go, rgb ? rgb : C_ACCENT);
    set_text(s_b_go_txt, "START");
}

/* ---- LIVE ------------------------------------------------------------- */

static void ribbon_set(unsigned i, uint8_t level, uint32_t rgb)
{
    if (i >= PHAROS_DISP_HISTORY) return;
    if (s_ribbon_level[i] == level) return;
    s_ribbon_level[i] = level;
    /* 6 px minimum so a quiet stretch is a visible baseline rather than a
     * gap - a timeline with nothing on it is information, and a hole in the
     * ribbon reads as a rendering fault. */
    int h = 6 + ((int)level * 40) / 255;
    lv_obj_set_size(s_l_ribbon[i], 7, h);
    lv_obj_align(s_l_ribbon[i], LV_ALIGN_CENTER,
                 lv_obj_get_x_aligned(s_l_ribbon[i]), PS_Y_RIBBON);
    set_bg(s_l_ribbon[i], level ? rgb : C_TRACK);
}

void pharos_hud_live(const char *lens, const struct pharos_lens_display *d,
                     uint32_t rgb_override)
{
    if (!s_built) return;
    page_show(PAGE_LIVE);
    toast_tick();

    if (!d) {
        aura_set(ps_tint(C_ACCENT, 60));
        set_text(s_l_word, "STARTING");
        set_fg(s_l_word, C_DIM);
        set_text(s_l_score, "");
        set_text_fit(s_l_ctx, PS_TYPE_LABEL, PS_Y_CONTEXT, lens);
        set_text(s_l_detail, "");
        set_text(s_l_why, "");
        set_text_fit(s_l_action, PS_TYPE_LABEL, PS_Y_ACTION, "listening");
        set_text(s_l_action2, "");
        set_arc_value(s_l_ring, 0);
        for (unsigned i = 0; i < PHAROS_DISP_FAMILIES; i++) {
            show(s_l_chip[i], false);
            show(s_l_chip_txt[i], false);
        }
        show(s_l_sim, false);
        return;
    }

    /* COLOUR first. */
    const uint32_t rgb = rgb_override ? rgb_override
                       : (d->has_alert ? ps_alert_colour(d->alert)
                                       : ps_score_colour(d->score));
    aura_set(rgb);
    set_arc_rgb(s_l_ring, rgb);
    set_arc_value(s_l_ring, d->has_score ? d->score : 0);

    /* The ceiling tick: where this observation could not have got past. */
    if (d->has_score && d->ceiling > 0 && d->ceiling < 100) {
        const int32_t at = 135 + (int32_t)(270 * d->ceiling / 100);
        lv_arc_set_angles(s_l_ceil, at, at + 2);
        show(s_l_ceil, true);
    } else {
        show(s_l_ceil, false);
    }

    /* WORD second, and it is the biggest thing on the glass. */
    set_text_fit(s_l_word, PS_TYPE_HERO, PS_Y_HERO, d->band);
    set_fg(s_l_word, rgb);

    /* NUMBER third, deliberately smaller than the word it supports. */
    char num[24];
    if (d->has_score) {
        if (d->ceiling > 0 && d->ceiling < 100) {
            snprintf(num, sizeof num, "%u / %u", d->score, d->ceiling);
        } else {
            snprintf(num, sizeof num, "%u", d->score);
        }
    } else {
        snprintf(num, sizeof num, "%s", d->big);
    }
    set_text(s_l_score, num);

    set_text_fit(s_l_ctx, PS_TYPE_LABEL, PS_Y_CONTEXT, lens);
    set_text_fit(s_l_detail, PS_TYPE_LABEL, PS_Y_DETAIL, d->detail);
    show(s_l_sim, d->simulated);

    /* EVIDENCE fourth: named chips, never anonymous dots. "Why does it think
     * so" has to be answerable from the glass, not from the manual. */
    for (unsigned i = 0; i < PHAROS_DISP_FAMILIES; i++) {
        const bool have = d->fam_label[i] != NULL;
        show(s_l_chip[i], have);
        show(s_l_chip_txt[i], have);
        if (!have) continue;
        const bool lit = (d->families & (1u << i)) != 0u;
        set_text(s_l_chip_txt[i], d->fam_label[i]);
        set_bg(s_l_chip[i], lit ? ps_tint(rgb, 40) : C_TRACK2);
        set_fg(s_l_chip_txt[i], lit ? rgb : C_DIMMER);
    }

    /* The ribbon: what shape, over time. */
    if (d->has_history) {
        for (unsigned i = 0; i < PHAROS_DISP_HISTORY; i++) {
            ribbon_set(i, d->history[i], rgb);
        }
    } else {
        for (unsigned i = 0; i < PHAROS_DISP_HISTORY; i++) ribbon_set(i, 0, rgb);
    }

    /* ACTION last: the one specific finding, then what to do about it. */
    set_text_fit(s_l_why, PS_TYPE_LABEL, PS_Y_WHY, d->why);
    {
        /* Two lines, split on a word. See the note on PS_Y_ACTION2: cutting
         * "Broad, spoofed deauth. Preserve the log." down to its first
         * sentence throws away the only part that says what to DO. */
        char a1[48], a2[48];
        const unsigned cap = ps_capacity(PS_TYPE_LABEL, PS_Y_ACTION, PR_SAFE_R);
        split_two(d->advice, cap < 47u ? cap : 47u, a1, sizeof a1, a2, sizeof a2);
        set_text(s_l_action, a1);
        set_text_fit(s_l_action2, PS_TYPE_LABEL, PS_Y_ACTION2, a2);
    }
}

/* ---- DETAIL ----------------------------------------------------------- */

void pharos_hud_detail(const char *lens, const char *head_left,
                       const char *head_right,
                       const struct pharos_lens_row *rows, unsigned n,
                       unsigned page, unsigned pages, int focus, bool openable)
{
    if (!s_built) return;
    page_show(PAGE_DETAIL);
    toast_tick();

    set_text_fit(s_d_title, PS_TYPE_LABEL, -196, lens);
    set_text_fit(s_d_hl, PS_TYPE_MICRO, -164, head_left);
    set_text_fit(s_d_hr, PS_TYPE_MICRO, -164, head_right);

    if (n > PHAROS_HUD_ROWS) n = PHAROS_HUD_ROWS;
    show(s_d_empty, n == 0);

    for (unsigned i = 0; i < PHAROS_HUD_ROWS; i++) {
        const bool have = (i < n);
        show(s_d_card[i], have);
        show(s_d_left[i], have);
        show(s_d_right[i], have);
        if (!have) continue;
        set_text(s_d_left[i], rows[i].left);
        set_text(s_d_right[i], rows[i].right);
        set_fg(s_d_right[i], tone_colour(rows[i].tone));
        set_fg(s_d_left[i], C_TEXT);
        /* The focused row lifts rather than outlines: a border on a rounded
         * card at this size reads as a rendering artefact. */
        const bool hot = ((int)i == focus);
        set_bg(s_d_card[i], hot ? ps_tint(C_ACCENT, 30) : C_TRACK2);
    }

    char pg[24];
    if (pages > 1u) snprintf(pg, sizeof pg, "%u / %u", page + 1u, pages);
    else            pg[0] = '\0';
    set_text(s_d_page, pg);
    (void)openable;
}

/* ---- colour bars ------------------------------------------------------ */

void pharos_hud_colourbars(void)
{
    if (!s_built) return;
    page_show(PAGE_BARS);
}

#else /* !ESP_PLATFORM - the host build has no panel */

bool pharos_hud_create(void) { return false; }
void pharos_hud_rebuild(void) {}
bool pharos_hud_present(void) { return false; }
void pharos_hud_set_nav_cb(pharos_hud_nav_cb_t cb) { (void)cb; }
void pharos_hud_set_row_cb(pharos_hud_row_cb_t cb) { (void)cb; }
void pharos_hud_set_home_cb(pharos_hud_home_cb_t cb) { (void)cb; }
void pharos_hud_toast(const char *msg) { (void)msg; }
void pharos_hud_home(const struct pharos_hud_home *h) { (void)h; }
int pharos_hud_home_hit(int16_t x, int16_t y) { (void)x; (void)y; return -1; }
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

#endif /* ESP_PLATFORM */
