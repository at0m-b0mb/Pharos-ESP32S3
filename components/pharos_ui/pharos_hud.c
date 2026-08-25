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
    PAGE_GUIDE,
    PAGE_BARS,
    PAGE_N,
} hud_page_t;

static lv_obj_t *s_page[PAGE_N];
static hud_page_t s_current = PAGE_SPLASH;
static bool s_built;

/* Shared chrome */
static lv_obj_t *s_tell;
static lv_obj_t *s_batt_track, *s_batt_fill;
static int s_batt_last = -2;   /* the permanent receive-only pip */
static lv_obj_t *s_toast;

/* The aura: three nested discs whose COLOUR is the verdict. Opacity is fixed
 * at build time and never touched, so changing the verdict invalidates three
 * fills and nothing else. A radial gradient would be one object and would
 * recompute every frame; this is the cheap way to get a soft falloff. */
#define AURA_N 7
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
static lv_obj_t *s_b_pos, *s_b_go, *s_b_go_txt, *s_b_rim;

/* DETAIL */
static lv_obj_t *s_d_title, *s_d_hl, *s_d_hr, *s_d_page, *s_d_empty;
static lv_obj_t *s_d_card[PHAROS_HUD_ROWS];
static lv_obj_t *s_d_left[PHAROS_HUD_ROWS], *s_d_right[PHAROS_HUD_ROWS];
static lv_obj_t *s_d_chev[PHAROS_HUD_ROWS], *s_d_stripe[PHAROS_HUD_ROWS];

/* GUIDE */
static lv_obj_t *s_g_ghost;                 /* outline of the zone taught  */
static lv_obj_t *s_g_finger;                /* the pulsing fingertip       */
static lv_obj_t *s_g_title, *s_g_l1, *s_g_l2, *s_g_hint;
static lv_obj_t *s_g_pip[10];               /* progress                    */
static lv_obj_t *s_g_swatch[4], *s_g_sw_txt[4];
static lv_obj_t *s_g_dot[8];                /* the mini home ring          */
static lv_obj_t *s_g_chip[4], *s_g_chip_txt[4];
static int s_g_step = -1;                   /* what is on screen now       */

/* SPLASH */
static lv_obj_t *s_s_name, *s_s_ver, *s_s_fence, *s_s_ring;

/* BARS */
static lv_obj_t *s_bar_patch[6], *s_bar_name[6];

/* Touch zones */
static lv_obj_t *s_zone_main[4];
static lv_obj_t *s_zone_row[PHAROS_HUD_ROWS];
static lv_obj_t *s_zone_page[2];
static lv_obj_t *s_zone_start;
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
static void set_text_fit_r(lv_obj_t *o, ps_type_t t, int16_t dy, int16_t r,
                           const char *s)
{
    if (!o) return;
    if (!s || !*s) { set_text(o, ""); return; }

    const unsigned cap = ps_capacity(t, dy, r);
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

/* The common case: a page with no gauge, so the glass is the only limit. */
static void set_text_fit(lv_obj_t *o, ps_type_t t, int16_t dy, const char *s)
{
    set_text_fit_r(o, t, dy, PR_SAFE_R, s);
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

/* Shared by the breath and by the guide's fingertip. */
static void opa_anim_cb(void *obj, int32_t v)
{
    lv_obj_set_style_bg_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}

static void text_opa_anim_cb(void *obj, int32_t v)
{
    lv_obj_set_style_text_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}

/* A PAGE ARRIVES BY FADING ITS OWN HEADLINE UP, AND NOT BY ANY OTHER MEANS.
 *
 * The obvious implementation - animate the page container's opacity - is a
 * trap on this board. LVGL renders any object with opa < COVER into a LAYER,
 * and a layer for a full-screen container is 466*466*2 = 434 KB. There are 49
 * KB of internal DMA RAM free. It would either spill to PSRAM and crawl, or
 * fail outright as ESP_ERR_NO_MEM - which is exactly the "Draw bitmap failed"
 * this project has already been bitten by once.
 *
 * text_opa needs no layer: glyphs are blended straight into the frame with
 * their own alpha. So the transition is carried by two or three labels per
 * page rather than by the whole surface, which is also all the eye follows. */
static void fade_in(lv_obj_t *const *objs, unsigned n)
{
    for (unsigned i = 0; i < n; i++) {
        if (!objs[i]) continue;
        lv_anim_delete(objs[i], text_opa_anim_cb);
        lv_obj_set_style_text_opa(objs[i], 0, 0);

        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, objs[i]);
        lv_anim_set_exec_cb(&a, text_opa_anim_cb);
        lv_anim_set_values(&a, 0, 255);
        lv_anim_set_duration(&a, PS_MS_PAGE);
        /* Staggered, so it reads as one movement rather than a flash - but
         * only just: 45 ms each stretched a 260 ms fade to 395 ms of
         * continuous invalidation, and every frame of that competes with the
         * repaint of the page underneath it. */
        lv_anim_set_delay(&a, i * 25u);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_start(&a);
    }
}

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
    /* SEVEN LAYERS. Three showed as three rings; five still stepped
     * visibly on a 300 px disc. Seven at ~20 px with a shallow opacity ramp
     * is below the point the eye resolves a band. */
    const int r[AURA_N] = { PS_AURA_R, 134, 118, 102, 86, 70, 54 };
    const uint8_t opa[AURA_N] = { 28, 32, 36, 40, 44, 48, 52 };
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
    const uint8_t mix[AURA_N] = { 16, 20, 24, 28, 33, 38, 44 };
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
    show(s_zone_start, want == PAGE_BROWSE);
    s_current = want;

    /* Whichever labels carry this page's meaning. Deliberately few. */
    switch (want) {
    case PAGE_HOME: {
        lv_obj_t *const o[] = { s_h_word, s_h_sub };
        fade_in(o, 2);
        break;
    }
    case PAGE_BROWSE: {
        lv_obj_t *const o[] = { s_b_name, s_b_l1, s_b_l2 };
        fade_in(o, 3);
        break;
    }
    case PAGE_LIVE: {
        lv_obj_t *const o[] = { s_l_word, s_l_score };
        fade_in(o, 2);
        break;
    }
    case PAGE_DETAIL: {
        lv_obj_t *const o[] = { s_d_title, s_d_hl, s_d_hr };
        fade_in(o, 3);
        break;
    }
    case PAGE_GUIDE: {
        lv_obj_t *const o[] = { s_g_title, s_g_l1, s_g_l2 };
        fade_in(o, 3);
        break;
    }
    default:
        break;
    }
}

/* ---- touch ------------------------------------------------------------ */

static void log_touch(const char *what, unsigned tag)
{
    ESP_LOGI(TAG, "touch: %s %u", what, tag);
}

/* PRESS AND HOLD IS THE WAY OUT, AND UNTIL NOW IT DID NOT EXIST.
 *
 * Only LV_EVENT_CLICKED was ever registered, so PHAROS_NAV_HOME could not be
 * raised by touch at all - it was reachable only from the console. That left
 * no way off a page with the finger, and the two that toggle (the bottom
 * strip swaps LIVE and DETAIL) oscillated forever instead of escaping. A user
 * who opened the ring's settings was simply trapped there.
 *
 * Worse, the guide taught this gesture. Step five said "press and hold to
 * stop and step back out" - a tutorial teaching a control that was never
 * implemented, which is how somebody ends up certain the hardware is broken.
 *
 * LVGL still delivers CLICKED on release after a long press, so the hold sets
 * a flag that the next click consumes; otherwise one gesture would fire two
 * actions - leave the page, then immediately act on whatever it landed on. */
static bool s_long_fired;

static void long_event(lv_event_t *e)
{
    (void)e;
    s_long_fired = true;
    log_touch("hold", (unsigned)PHAROS_NAV_HOME);
    if (s_nav_cb) s_nav_cb(PHAROS_NAV_HOME);
}

static void nav_event(lv_event_t *e)
{
    if (s_long_fired) { s_long_fired = false; return; }
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
    if (s_long_fired) { s_long_fired = false; return; }
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
    /* Every zone is also a way out. Whichever one the finger happens to be
     * over, holding it means "back" - so escaping never depends on having
     * found the right part of the glass first. */
    lv_obj_add_event_cb(z, long_event, LV_EVENT_LONG_PRESSED, NULL);
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
    /* The START target is owned by page_show, which knows whether BROWSE is
     * up; zones_mode only knows detail-or-not and would clobber it. */
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
    /* CHARGE, ON THE RIM, OUTSIDE EVERYTHING ELSE.
     *
     * A handheld that goes flat mid-sweep with no warning is a tool nobody
     * trusts, and until now the only way to ask was over the console - which
     * is exactly where somebody holding the device is not looking.
     *
     * It sits at r=220, outside the score arc's 206 and the browse rim's 212,
     * so it cannot collide with any page's content, and it spans only the 60
     * degrees at the top where no page draws. Persistent chrome, like the
     * receive-only pip at the bottom: present on every page, owned by none. */
    s_batt_track = mk_arc(scr, 440, 3, C_TRACK, 100, 240, 60);
    s_batt_fill  = mk_arc(scr, 440, 3, C_DIM,   100, 240, 60);
    show(s_batt_track, false);
    show(s_batt_fill, false);

    s_tell = mk_surface(scr, 10, 10, 0, PS_Y_TELL, PS_GOOD, LV_OPA_COVER,
                        LV_RADIUS_CIRCLE);
    show(s_tell, false);

    /* THE ONE THING ON THE GLASS THAT NEVER STOPS MOVING.
     *
     * "Is it frozen?" was a real question asked of the old face, because a
     * quiet room produces a completely static screen and a crashed device
     * looks identical to a calm one. This is the answer: a slow breath on the
     * receive-only pip, which is already the one element present on every
     * page. Ten pixels, four seconds, and it never touches the reading - so
     * it costs a rounding error of the frame budget rather than the
     * full-screen shimmer an animated aura would have been. */
    {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_tell);
        lv_anim_set_exec_cb(&a, opa_anim_cb);
        lv_anim_set_values(&a, 255, 90);
        lv_anim_set_duration(&a, PS_MS_BREATH / 2);
        lv_anim_set_reverse_duration(&a, PS_MS_BREATH / 2);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
        lv_anim_start(&a);
    }

    /* ---- SPLASH ---- */
    {
        lv_obj_t *p = s_page[PAGE_SPLASH];
        /* The boot screen is up for a second and a half whatever happens, so
         * it may as well do something. The ring sweeping closed is the
         * lighthouse turning: it is the identity, and it also tells somebody
         * watching that the device is running rather than stuck. */
        s_s_ring  = mk_arc(p, PS_RING_R * 2, 4, C_ACCENT, 0, 270, 360);
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
        /* THE TRACK IS THE SAME WEIGHT AS THE SCORE.
         *
         * It was 6 px of near-black under a 14 px bright arc, so a low
         * reading - QUIET at 12 - drew a lone stub at the bottom left with
         * nothing behind it, and read as a rendering fault rather than as a
         * small number. A gauge needs a visible channel for the needle to
         * fill; that is what makes 12 look like 12 out of 100. */
        s_h_track = mk_arc(p, PS_RING_R * 2, PS_RING_W, C_TRACK, 100, 135, 270);
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
        /* A full rim in the tool's own colour. The browse card was a lot of
         * black around a little text; this gives each of the twenty-one an
         * identity you register before reading the name, and costs one arc. */
        s_b_rim      = mk_arc(p, PS_RING_R * 2 + 12, 3, C_ACCENT, 100, 135, 270);
        s_b_kind     = mk_surface(p, 132, 32, 0, -150, C_TRACK, LV_OPA_COVER, 16);
        s_b_kind_txt = mk_label(p, PS_TYPE_LABEL, C_DIMMER, 0, -150, "");
        s_b_name     = mk_label(p, PS_TYPE_TITLE, C_TEXT, 0, -88, "");
        /* Two fixed lines rather than a wrapping block: lesson 3. The caller
         * splits; the height never changes; nothing re-lays-out. */
        s_b_l1 = mk_label(p, PS_TYPE_BODY, C_DIM, 0, -22, "");
        s_b_l2 = mk_label(p, PS_TYPE_BODY, C_DIM, 0,  10, "");
        s_b_pos      = mk_label(p, PS_TYPE_MICRO, C_DIMMER, 0, 64, "");
        s_b_go       = mk_surface(p, 190, 56, 0, 130, C_ACCENT, LV_OPA_COVER, 28);
        s_b_go_txt   = mk_label(p, PS_TYPE_BODY, PS_VOID, 0, 130, "START");
    }

    /* ---- LIVE ---- */
    {
        lv_obj_t *p = s_page[PAGE_LIVE];

        s_l_track = mk_arc(p, PS_RING_R * 2, PS_RING_W, C_TRACK, 100, 135, 270);
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
            /* A lit chip gets a hairline edge as well as a fill. Fill alone
             * is ambiguous at these sizes against a coloured aura. */
            lv_obj_set_style_border_width(s_l_chip[i], 1, 0);
            lv_obj_set_style_border_opa(s_l_chip[i], LV_OPA_TRANSP, 0);
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

            /* THE TEXT IS A CHILD OF ITS CARD, AND THAT IS THE WHOLE FIX.
             *
             * These were children of the PAGE, positioned with
             * lv_obj_align(LV_ALIGN_CENTER, -w/2 + 18, dy). That aligns the
             * label's BOUNDING BOX centre at that x - and an auto-sized label
             * grows symmetrically about it, so half of a long string extended
             * further left, off the card and off the glass. Every row on
             * every sensor page lost its first few characters: "Top models"
             * rendered as "p models".
             *
             * LV_TEXT_ALIGN_LEFT did not save it, because that aligns lines
             * WITHIN the box; it says nothing about where the box sits.
             *
             * Parented to the card and given an explicit width, the text is
             * clipped by its own card and can no longer reach the edge of the
             * panel however long it gets. LONG_DOT then ellipsises the few
             * labels that genuinely do not fit, which is a legible failure
             * instead of a silent amputation. */
            /* Sized against the NARROWEST card, which is the bottom one:
             * the chord shrinks toward the rim, and a budget taken from
             * the middle rows leaves the last row two characters short.
             * 104 px still holds eight characters of value at
             * PS_TYPE_BODY, which covers every grade and count the row
             * contract can carry. */
            const int pad = 12;
            const int val_w = 104;
            const int lab_w = w - val_w - pad * 2 - 8;

            s_d_left[i] = lv_label_create(s_d_card[i]);
            lv_obj_remove_flag(s_d_left[i], LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_remove_flag(s_d_left[i], LV_OBJ_FLAG_CLICKABLE);
            lv_obj_set_style_text_font(s_d_left[i], font_of(PS_TYPE_LABEL), 0);
            lv_obj_set_style_text_color(s_d_left[i], lv_color_hex(C_TEXT), 0);
            /* WIDTH ALONE IS NOT ENOUGH - SET THE HEIGHT TOO.
             *
             * LV_LABEL_LONG_DOT is documented as "break the text and write
             * dots in the last line". BREAK means wrap: with an auto height
             * the label simply grows to two lines and dots the second one,
             * which is how "unclassified" appeared on a PROBE row as
             * "unclassifi" with a stray "e" floating out of the card.
             *
             * Pinning the height to a single line leaves LONG_DOT no room to
             * wrap into, so it truncates horizontally, which is what was
             * wanted. It also restores lesson 3: a cell whose height cannot
             * change can never move the invalidated region. */
            lv_obj_set_size(s_d_left[i], lab_w,
                            lv_font_get_line_height(font_of(PS_TYPE_LABEL)));
            lv_label_set_long_mode(s_d_left[i], LV_LABEL_LONG_DOT);
            lv_obj_set_style_text_align(s_d_left[i], LV_TEXT_ALIGN_LEFT, 0);
            lv_label_set_text(s_d_left[i], "");
            lv_obj_align(s_d_left[i], LV_ALIGN_LEFT_MID, pad, 0);

            /* A control announces itself twice: an accent stripe down the
             * leading edge, and a chevron where a value that can change
             * lives. Both are children of the card, so neither can drift. */
            s_d_stripe[i] = mk_surface(s_d_card[i], 4, PS_CARD_H - 18, 0, 0,
                                       C_ACCENT, LV_OPA_COVER, 2);
            lv_obj_align(s_d_stripe[i], LV_ALIGN_LEFT_MID, 5, 0);
            show(s_d_stripe[i], false);

            s_d_chev[i] = mk_label(s_d_card[i], PS_TYPE_LABEL, C_ACCENT, 0, 0, ">");
            lv_obj_align(s_d_chev[i], LV_ALIGN_RIGHT_MID, -12, 0);
            show(s_d_chev[i], false);

            s_d_right[i] = lv_label_create(s_d_card[i]);
            lv_obj_remove_flag(s_d_right[i], LV_OBJ_FLAG_SCROLLABLE);
            lv_obj_remove_flag(s_d_right[i], LV_OBJ_FLAG_CLICKABLE);
            /* The VALUE is the judgement - the thing somebody is scanning
             * the page for - so it is the column that must never be cut. At
             * PS_TYPE_LABEL the full 11 characters right[] can carry fit in
             * val_w exactly; at PS_TYPE_BODY they did not, and the overflow
             * became a wrap. */
            lv_obj_set_style_text_font(s_d_right[i], font_of(PS_TYPE_LABEL), 0);
            lv_obj_set_style_text_color(s_d_right[i], lv_color_hex(C_DIM), 0);
            lv_obj_set_size(s_d_right[i], val_w,
                            lv_font_get_line_height(font_of(PS_TYPE_LABEL)));
            lv_label_set_long_mode(s_d_right[i], LV_LABEL_LONG_DOT);
            lv_obj_set_style_text_align(s_d_right[i], LV_TEXT_ALIGN_RIGHT, 0);
            lv_label_set_text(s_d_right[i], "");
            /* Clear of the chevron, not merely beside it: at -12 the
             * value and the chevron rendered as one token, "Beacon>". */
            lv_obj_align(s_d_right[i], LV_ALIGN_RIGHT_MID, -pad - 20, 0);
        }
        s_d_page  = mk_label(p, PS_TYPE_MICRO, C_DIMMER, 0, PS_Y_PAGE, "");
        s_d_empty = mk_label(p, PS_TYPE_BODY, C_DIMMER, 0, 0, "Nothing found yet");
        show(s_d_empty, false);
    }


    /* ---- GUIDE ----
     *
     * Built once and re-pointed per step rather than rebuilt, because a
     * rebuild inside an animation callback is how you free an object LVGL is
     * still walking. Everything that a given step does not use is hidden. */
    {
        lv_obj_t *p = s_page[PAGE_GUIDE];

        /* The zone outline: a hollow rounded rect that says WHERE. Drawn
         * first so the fingertip pulses on top of it. */
        s_g_ghost = mk_box(p);
        lv_obj_set_style_radius(s_g_ghost, 24, 0);
        lv_obj_set_style_bg_opa(s_g_ghost, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_color(s_g_ghost, lv_color_hex(C_ACCENT), 0);
        lv_obj_set_style_border_width(s_g_ghost, 3, 0);
        lv_obj_set_style_border_opa(s_g_ghost, 160, 0);
        lv_obj_set_size(s_g_ghost, 120, 120);
        lv_obj_center(s_g_ghost);
        show(s_g_ghost, false);

        s_g_finger = mk_surface(p, 54, 54, 0, 0, C_ACCENT, 150, LV_RADIUS_CIRCLE);
        show(s_g_finger, false);

        for (unsigned i = 0; i < 10; i++) {
            s_g_pip[i] = mk_surface(p, 8, 8, 0, PS_Y_GUIDE_PIPS, C_TRACK, LV_OPA_COVER,
                                    LV_RADIUS_CIRCLE);
            show(s_g_pip[i], false);
        }

        s_g_title = mk_label(p, PS_TYPE_TITLE, C_TEXT,   0, PS_Y_GUIDE_TITLE, "");
        s_g_l1    = mk_label(p, PS_TYPE_BODY,  C_DIM,    0, PS_Y_GUIDE_L1, "");
        s_g_l2    = mk_label(p, PS_TYPE_BODY,  C_DIM,    0, PS_Y_GUIDE_L2, "");
        s_g_hint  = mk_label(p, PS_TYPE_LABEL, C_ACCENT, 0, PS_Y_GUIDE_HINT, "");

        /* The four verdict colours, named. This is the single most valuable
         * screen in the guide: it is the key to every other screen. */
        {
            const uint32_t v[4] = { PS_GOOD, PS_WARN, PS_HIGH, PS_BAD };
            for (unsigned i = 0; i < 4; i++) {
                const int dy = -58 + (int)i * 46;
                s_g_swatch[i] = mk_surface(p, 44, 34, -112, dy, v[i],
                                           LV_OPA_COVER, 10);
                s_g_sw_txt[i] = mk_label(p, PS_TYPE_LABEL, C_TEXT, 26, dy, "");
                lv_obj_set_width(s_g_sw_txt[i], 196);
                lv_label_set_long_mode(s_g_sw_txt[i], LV_LABEL_LONG_DOT);
                lv_obj_set_style_text_align(s_g_sw_txt[i], LV_TEXT_ALIGN_LEFT, 0);
                show(s_g_swatch[i], false);
                show(s_g_sw_txt[i], false);
            }
        }

        /* A miniature home ring, so the dots are explained with dots. */
        for (unsigned i = 0; i < 8; i++) {
            s_g_dot[i] = mk_surface(p, 16, 16, 0, 0, PS_GOOD, LV_OPA_COVER,
                                    LV_RADIUS_CIRCLE);
            show(s_g_dot[i], false);
        }

        /* And the evidence chips, explained with chips. */
        {
            static const char *k_names[4] = { "RATE", "SHAPE", "FORGE", "AFTER" };
            const int cw = 84, gap = 6;
            const int total = cw * 4 + gap * 3;
            for (unsigned i = 0; i < 4; i++) {
                const int dx = -total / 2 + (int)i * (cw + gap) + cw / 2;
                s_g_chip[i] = mk_surface(p, cw, 34, dx, 0, C_TRACK2,
                                         LV_OPA_COVER, 17);
                s_g_chip_txt[i] = mk_label(p, PS_TYPE_LABEL, C_DIMMER, dx, 0,
                                           k_names[i]);
                show(s_g_chip[i], false);
                show(s_g_chip_txt[i], false);
            }
        }
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
    /* THE BUTTON THAT COULD NOT BE PRESSED.
     *
     * START is drawn at dy +130 and is 56 tall, so it occupies +102..+158.
     * The SELECT zone - the one that actually launches a lens - is 190x190 at
     * the centre, so it stops at +95. The two never overlapped. Pressing the
     * only thing on the page that looks like a button therefore did nothing,
     * or, in its lower half, hit the bottom strip and opened the detail page
     * instead. The centre of the screen worked, which is what the guide
     * teaches, but nothing on the glass said so and the button said otherwise.
     *
     * A drawn control that is not a target where it is drawn is worse than no
     * control at all: it teaches the operator the device is broken. So the
     * button gets a zone of its own, created after the others so it sits above
     * the bottom strip it overlaps, and shown only where the button is. The
     * centre still works too - it is the larger target, and one-handed that
     * matters. */
    s_zone_start = mk_zone(scr, 210, 72, 0, 130, nav_event, PHAROS_NAV_SELECT);
    show(s_zone_start, false);

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

    /* Sweep the ring closed once. Not repeated: a boot screen that loops
     * looks like a boot that has not finished. */
    lv_anim_delete(s_s_ring, arc_anim_cb);
    lv_arc_set_value(s_s_ring, 0);
    {
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_s_ring);
        lv_anim_set_exec_cb(&a, arc_anim_cb);
        lv_anim_set_values(&a, 0, 100);
        lv_anim_set_duration(&a, 1100);
        lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
        lv_anim_start(&a);
    }
    set_arc_rgb(s_s_ring, fence_clean ? PS_GOOD : PS_BAD);
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
    set_text_fit_r(s_h_word, PS_TYPE_HERO, PS_Y_HERO, PS_INNER_R, h->headline);
    set_fg(s_h_word, (h->worst_state == 0) ? C_TEXT : rgb);
    set_text_fit_r(s_h_sub, PS_TYPE_BODY, PS_Y_METRIC, PS_INNER_R, h->sub);

    /* The one name on this screen, and it is always legible because there is
     * only ever one of it. */
    const char *act = (h->active >= 0 && h->active < (int)n) ? h->label[h->active] : NULL;
    set_text_fit_r(s_h_active, PS_TYPE_LABEL, PS_Y_DETAIL + 16, PS_INNER_R,
                   act ? act : "");
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

    set_text_fit_r(s_b_name, PS_TYPE_TITLE, -88, PS_INNER_R, name);
    set_fg(s_b_name, C_TEXT);

    set_text(s_b_kind_txt, team ? team : "");
    set_bg(s_b_kind, ps_tint(rgb ? rgb : C_ACCENT, 34));
    set_fg(s_b_kind_txt, rgb ? rgb : C_ACCENT);

    char l1[40], l2[40];
    const unsigned cap = ps_capacity(PS_TYPE_BODY, -22, PS_INNER_R);
    split_two(summary, cap < 39u ? cap : 39u, l1, sizeof l1, l2, sizeof l2);
    set_text(s_b_l1, l1);
    set_text(s_b_l2, l2);

    char pos[24];
    snprintf(pos, sizeof pos, "%u of %u", index + 1u, total);
    set_text(s_b_pos, pos);

    set_bg(s_b_go, rgb ? rgb : C_ACCENT);
    set_arc_rgb(s_b_rim, rgb ? rgb : C_ACCENT);
    set_text(s_b_go_txt, "START");
}

/* ---- LIVE ------------------------------------------------------------- */

static void ribbon_set(unsigned i, uint8_t level, uint32_t rgb)
{
    if (i >= PHAROS_DISP_HISTORY) return;
    if (s_ribbon_level[i] == level) return;
    s_ribbon_level[i] = level;

    /* OLDER IS DIMMER.
     *
     * history[] is oldest-first, and drawing all sixteen seconds at one
     * intensity made the ribbon read as a static pattern rather than as time
     * passing. Fading the tail gives it direction: the bright end is NOW. */
    const uint8_t age_opa = (uint8_t)(96u + (159u * i) / (PHAROS_DISP_HISTORY - 1u));
    lv_obj_set_style_bg_opa(s_l_ribbon[i], level ? age_opa : 90u, 0);
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
        set_text_fit_r(s_l_ctx, PS_TYPE_LABEL, PS_Y_CONTEXT, PS_INNER_R, lens);
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
    set_text_fit_r(s_l_word, PS_TYPE_HERO, PS_Y_HERO, PS_INNER_R, d->band);
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

    set_text_fit_r(s_l_ctx, PS_TYPE_LABEL, PS_Y_CONTEXT, PS_INNER_R, lens);
    set_text_fit_r(s_l_detail, PS_TYPE_LABEL, PS_Y_DETAIL, PS_INNER_R, d->detail);
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
        lv_obj_set_style_border_color(s_l_chip[i], lv_color_hex(rgb), 0);
        lv_obj_set_style_border_opa(s_l_chip[i], lit ? 150 : LV_OPA_TRANSP, 0);
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
    set_text_fit_r(s_l_why, PS_TYPE_LABEL, PS_Y_WHY, PS_INNER_R, d->why);
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
        set_fg(s_d_left[i], C_TEXT);

        /* A control's VALUE is drawn in the accent, because the accent is
         * what this device uses for "you can act on this" everywhere else. A
         * reading keeps its tone, which is the verdict contract and is not a
         * theme's to borrow. */
        const bool act = rows[i].tappable;
        show(s_d_stripe[i], act);
        show(s_d_chev[i], act);
        set_fg(s_d_right[i],
               (act && rows[i].tone == PHAROS_TONE_NEUTRAL) ? C_ACCENT
                                                            : tone_colour(rows[i].tone));
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

/* ---- charge ---------------------------------------------------------- */

void pharos_hud_battery(uint8_t pct, bool charging, bool present)
{
    if (!s_built) return;

    if (!present) {
        if (s_batt_last != -1) {
            show(s_batt_track, false);
            show(s_batt_fill, false);
            s_batt_last = -1;
        }
        return;
    }
    if (pct > 100u) pct = 100u;

    /* Charging is a state, not a level, so it gets its own key rather than
     * sharing the level's. Packed into the dirty check so a steady battery
     * costs nothing per frame. */
    const int key = (int)pct + (charging ? 1000 : 0);
    if (key == s_batt_last) return;
    s_batt_last = key;

    show(s_batt_track, true);
    show(s_batt_fill, true);
    set_arc_value(s_batt_fill, pct);

    /* Green while it fills, then the ordinary chrome colour, and the warning
     * hues only when they are earned - the same contract every other reading
     * on this device follows. */
    const uint32_t rgb = charging ? PS_GOOD
                       : (pct <= 10u) ? PS_BAD
                       : (pct <= 25u) ? PS_WARN
                       : C_DIM;
    set_arc_rgb(s_batt_fill, rgb);
}

/* ---- the guide -------------------------------------------------------
 *
 * The animations are the content. A sentence that says "press and hold to go
 * back" is a sentence somebody skims; an outline of the bottom strip with a
 * fingertip swelling inside it is a gesture somebody copies. So each step
 * points the same two objects - an outline and a fingertip - at the zone it is
 * teaching, and lets LVGL run the motion.
 *
 * All of it is LVGL-owned. Nothing here is driven from the UI loop, so a step
 * that is already on screen costs exactly nothing to "redraw". */

static void g_finger_size_cb(void *o, int32_t v)
{
    lv_obj_set_size((lv_obj_t *)o, v, v);
    lv_obj_set_style_radius((lv_obj_t *)o, LV_RADIUS_CIRCLE, 0);
}

static void g_x_cb(void *o, int32_t v)
{
    lv_obj_set_x((lv_obj_t *)o, v);
}

/* A tap: the fingertip swells and fades, forever, at one spot. */
static void g_pulse_at(int dx, int dy, int base, int peak, uint32_t period)
{
    lv_anim_delete(s_g_finger, NULL);
    lv_obj_align(s_g_finger, LV_ALIGN_CENTER, dx, dy);
    lv_obj_set_size(s_g_finger, base, base);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_g_finger);
    lv_anim_set_exec_cb(&a, g_finger_size_cb);
    lv_anim_set_values(&a, base, peak);
    lv_anim_set_duration(&a, period);
    lv_anim_set_reverse_duration(&a, period);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);

    lv_anim_t o;
    lv_anim_init(&o);
    lv_anim_set_var(&o, s_g_finger);
    lv_anim_set_exec_cb(&o, opa_anim_cb);
    lv_anim_set_values(&o, 200, 40);
    lv_anim_set_duration(&o, period);
    lv_anim_set_reverse_duration(&o, period);
    lv_anim_set_repeat_count(&o, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&o);
}

/* The two side zones, taught by moving the fingertip between them - because
 * the thing being explained is that there are TWO of them and they do
 * opposite things. A pulse on one side alone teaches half the gesture. */
static void g_sweep_sides(int dy)
{
    lv_anim_delete(s_g_finger, NULL);
    lv_obj_align(s_g_finger, LV_ALIGN_CENTER, -140, dy);
    lv_obj_set_size(s_g_finger, 54, 54);
    lv_obj_set_style_bg_opa(s_g_finger, 170, 0);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_g_finger);
    lv_anim_set_exec_cb(&a, g_x_cb);
    lv_anim_set_values(&a, -140, 140);
    lv_anim_set_duration(&a, 1100);
    lv_anim_set_reverse_duration(&a, 1100);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_set_path_cb(&a, lv_anim_path_ease_in_out);
    lv_anim_start(&a);
}

static void g_hide_all(void)
{
    show(s_g_ghost, false);
    show(s_g_finger, false);
    for (unsigned i = 0; i < 4; i++) {
        show(s_g_swatch[i], false);
        show(s_g_sw_txt[i], false);
        show(s_g_chip[i], false);
        show(s_g_chip_txt[i], false);
    }
    for (unsigned i = 0; i < 8; i++) show(s_g_dot[i], false);
}

static void g_ghost(int w, int h, int dx, int dy)
{
    lv_obj_set_size(s_g_ghost, w, h);
    lv_obj_align(s_g_ghost, LV_ALIGN_CENTER, dx, dy);
    show(s_g_ghost, true);
}

void pharos_hud_guide(const struct pharos_hud_guide *g)
{
    if (!s_built || !g) return;
    page_show(PAGE_GUIDE);
    toast_tick();

    set_text_fit(s_g_title, PS_TYPE_TITLE, PS_Y_GUIDE_TITLE, g->title);
    set_text_fit(s_g_l1, PS_TYPE_BODY, PS_Y_GUIDE_L1, g->line1);
    set_text_fit(s_g_l2, PS_TYPE_BODY, PS_Y_GUIDE_L2, g->line2);
    set_text_fit(s_g_hint, PS_TYPE_LABEL, PS_Y_GUIDE_HINT, g->hint);

    /* Progress. Ten is the most the strip can hold at a legible spacing; a
     * longer guide would need a bar, and a guide longer than ten steps is a
     * manual nobody finishes anyway. */
    unsigned total = g->total > 10u ? 10u : g->total;
    for (unsigned i = 0; i < 10u; i++) {
        const bool on = i < total;
        show(s_g_pip[i], on);
        if (!on) continue;
        const int pitch = 18;
        const int dx = -(int)(total - 1u) * pitch / 2 + (int)i * pitch;
        lv_obj_align(s_g_pip[i], LV_ALIGN_CENTER, dx, PS_Y_GUIDE_PIPS);
        const bool here = (i == g->step);
        set_bg(s_g_pip[i], here ? C_ACCENT : C_TRACK);
        lv_obj_set_size(s_g_pip[i], here ? 12 : 8, here ? 12 : 8);
        lv_obj_set_style_radius(s_g_pip[i], LV_RADIUS_CIRCLE, 0);
    }

    /* Restarting an infinite animation every repaint would reset it to its
     * first frame ten times a second, which looks exactly like a stutter. */
    if ((int)g->step == s_g_step) return;
    s_g_step = (int)g->step;

    g_hide_all();

    switch (g->anim) {
    case PHAROS_GUIDE_ANIM_SIDES:
        /* 110x200 at dx=-140: the corners land at r=219, inside the
         * 224 safe radius. A 150x250 zone outline did not. */
        g_ghost(110, 200, -140, 0);
        show(s_g_finger, true);
        g_sweep_sides(0);
        break;

    case PHAROS_GUIDE_ANIM_CENTRE:
        g_ghost(190, 190, 0, 0);
        show(s_g_finger, true);
        g_pulse_at(0, 0, 44, 96, 700);
        break;

    case PHAROS_GUIDE_ANIM_BOTTOM:
        g_ghost(280, 84, 0, 60);
        show(s_g_finger, true);
        g_pulse_at(0, 60, 40, 84, 700);
        break;

    case PHAROS_GUIDE_ANIM_HOLD:
        /* Slower and bigger than a tap, which is the entire difference being
         * taught. */
        g_ghost(190, 190, 0, 0);
        show(s_g_finger, true);
        g_pulse_at(0, 0, 40, 150, 1500);
        break;

    case PHAROS_GUIDE_ANIM_VERDICT: {
        static const char *k_meaning[4] = {
            "nothing to do",
            "worth knowing",
            "one real finding",
            "act on this",
        };
        for (unsigned i = 0; i < 4; i++) {
            show(s_g_swatch[i], true);
            show(s_g_sw_txt[i], true);
            set_text(s_g_sw_txt[i], k_meaning[i]);
        }
        break;
    }

    case PHAROS_GUIDE_ANIM_RING: {
        /* One dot per watch, and the point of the picture is that you count
         * the ones that are not green. */
        static const uint8_t k_state[8] = { 0, 0, 0, 1, 0, 0, 3, 0 };
        for (unsigned i = 0; i < 8; i++) {
            const float deg = 225.0f + 270.0f * (float)i / 7.0f;
            const pr_point_t pt = pr_polar(96, deg);
            lv_obj_align(s_g_dot[i], LV_ALIGN_CENTER,
                         pt.x - PR_CX, pt.y - PR_CY);
            set_bg(s_g_dot[i], home_dot_colour(k_state[i]));
            lv_obj_set_size(s_g_dot[i], k_state[i] ? 22 : 16,
                            k_state[i] ? 22 : 16);
            lv_obj_set_style_radius(s_g_dot[i], LV_RADIUS_CIRCLE, 0);
            show(s_g_dot[i], true);
        }
        break;
    }

    case PHAROS_GUIDE_ANIM_CHIPS: {
        /* Two lit, two not - so the difference is visible rather than
         * asserted. */
        static const bool k_lit[4] = { true, true, false, false };
        for (unsigned i = 0; i < 4; i++) {
            show(s_g_chip[i], true);
            show(s_g_chip_txt[i], true);
            set_bg(s_g_chip[i], k_lit[i] ? ps_tint(PS_BAD, 40) : C_TRACK2);
            set_fg(s_g_chip_txt[i], k_lit[i] ? PS_BAD : C_DIMMER);
        }
        break;
    }

    case PHAROS_GUIDE_ANIM_NONE:
    default:
        break;
    }
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
void pharos_hud_battery(uint8_t p, bool c, bool q) { (void)p; (void)c; (void)q; }
void pharos_hud_guide(const struct pharos_hud_guide *g) { (void)g; }

#endif /* ESP_PLATFORM */
