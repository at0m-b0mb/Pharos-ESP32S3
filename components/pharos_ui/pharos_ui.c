/* Pharos - the UI runtime
 *
 * Two tasks, split by core exactly as the architecture promises:
 *
 *   analytics (core 1): drains the active lens' ingest ring and calls its
 *   on_event per frame. This is the hot side; it never touches the display.
 *
 *   ui (core 0): ticks the active lens ~20 Hz and repaints the round HUD at
 *   ~5 Hz. LVGL runs on the vendor BSP's own task, so painting here is only
 *   pushing fresh values in under its lock.
 *
 * The dial is built from the lens registry, sorted so the tools a defender
 * reaches for first sit at the top. main.c never names a lens; adding one .c
 * file to the build adds it to this dial automatically.
 */
#include "pharos_ui.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"

#include "freertos/semphr.h"

#include "pharos_aegis.h"
#include "pharos_audio.h"
#include "pharos_bus.h"
#include "pharos_dial.h"
#include "pharos_hud.h"
#include "pharos_dial.h"
#include "pharos_theme.h"
#include "pharos_motion.h"
#include "pharos_survey.h"
#include "pharos_survey_hook.h"
#include "pharos_tower.h"
#include "pharos_radio.h"
#include "pharos_lens.h"

static const char *TAG = "ui";

static bool s_fence_ok;
static volatile bool s_analytics_run;

/* ---- the Aegis latch ------------------------------------------------
 *
 * Lives here because the UI loop is the one thing that runs continuously and
 * always knows which lens is active. Every second it asks the active lens for
 * its finding and folds it in, so the picture survives lens changes - and the
 * operator not looking. */
static pa_state_t s_aegis;
static SemaphoreHandle_t s_aegis_lock;

/* ---- the lens lifecycle guard ---------------------------------------
 *
 * Three tasks used to mutate the active lens - the UI task (touch and the BOOT
 * button), the console REPL task, and main at boot - while a FOURTH, the
 * analytics task, was walking it:
 *
 *     struct pharos_bus *bus = lens->ingest();   // lens A's ring
 *     while (pharos_bus_pop(bus, &ev))           // ... A unmounted here ...
 *         lens->on_event(&ev);                   // writing into torn-down state
 *
 * That is a use-after-free, and it is why the device felt unstable: it did not
 * fail every time, it failed when the timing lined up.
 *
 * Two rules now. Lens switching happens ONLY on the UI task - everyone else
 * files a request. And the analytics pump holds this mutex while it drains, so
 * a switch cannot start underneath it. */
static SemaphoreHandle_t s_lens_mtx;
static char s_req_lens[32];
static volatile bool s_req_stop;

bool pharos_ui_request_lens(const char *id)
{
    if (!id || !*id) {
        return false;
    }
    if (!pharos_lens_find(id)) {
        return false; /* answer the caller honestly, before queueing anything */
    }
    strncpy(s_req_lens, id, sizeof(s_req_lens) - 1);
    s_req_lens[sizeof(s_req_lens) - 1] = '\0';
    return true;
}

void pharos_ui_request_stop(void) { s_req_stop = true; }

/* Defined with the pump, which is where the guard is taken. */
static bool lens_switch(const char *id);
static void lens_halt(void);
static uint32_t s_aegis_accum_ms;

static void aegis_pump(const pharos_lens_t *active, uint32_t dt_ms)
{
    s_aegis_accum_ms += dt_ms;
    if (s_aegis_accum_ms < 1000u) {
        return;
    }
    s_aegis_accum_ms = 0;
    if (!active || !active->stage_report || !s_aegis_lock) {
        return;
    }
    uint8_t stage = 0, score = 0, ceiling = 0;
    if (!active->stage_report(&stage, &score, &ceiling)) {
        return;
    }
    if (xSemaphoreTake(s_aegis_lock, 0) != pdTRUE) {
        return;
    }
    pa_observe(&s_aegis, (pa_stage_t)stage, score, ceiling,
               (uint64_t)esp_timer_get_time());
    xSemaphoreGive(s_aegis_lock);
}

bool pharos_ui_aegis_snapshot(pa_verdict_t *out)
{
    if (!out || !s_aegis_lock) {
        return false;
    }
    if (xSemaphoreTake(s_aegis_lock, pdMS_TO_TICKS(5)) != pdTRUE) {
        return false;
    }
    pa_evaluate(&s_aegis, (uint64_t)esp_timer_get_time(), out);
    xSemaphoreGive(s_aegis_lock);
    return true;
}

void pharos_ui_aegis_ack(void)
{
    if (!s_aegis_lock || xSemaphoreTake(s_aegis_lock, pdMS_TO_TICKS(20)) != pdTRUE) {
        return;
    }
    pa_acknowledge(&s_aegis);
    xSemaphoreGive(s_aegis_lock);
    ESP_LOGI(TAG, "aegis: latch acknowledged; watch restarts clean");
}

/* ---- navigation ------------------------------------------------------
 *
 * THE RULE, learned the hard way: the touch callback runs on LVGL's task,
 * holding LVGL's lock and using LVGL's stack. Activating a lens tears down and
 * restarts the Wi-Fi driver, which needs far more stack than that task has -
 * and re-enters the display lock. Doing it there overflowed the stack and
 * rebooted the board on every lens change; that shipped in v1.8.0.
 *
 * So the callback records an intent and returns. The UI task, which owns the
 * lens lifecycle already, performs it on the next tick. */
typedef enum { VIEW_HOME = 0, VIEW_BROWSE, VIEW_LIVE, VIEW_DETAIL,
               VIEW_OPENED, VIEW_GUIDE } view_t;

static const pharos_lens_t *s_order[PHAROS_MAX_LENSES];
static unsigned s_order_n;

/* ---- the watchtower -------------------------------------------------
 *
 * "I should not have to be inside the right lens at the moment it happens."
 * There is one 2.4 GHz receiver, so the watches take turns; see
 * pharos_tower.h for why that is the honest reading of the request and what
 * the ring has to show for it not to become a comfortable lie.
 *
 * s_tower_on is the mode, not a view: the rotation keeps running while the
 * operator is reading a detail page, and stops the moment they pick a lens by
 * hand - somebody who chose a watch did not ask to be moved off it. */
/* THE SESSION SURVEY.
 *
 * Lives here for the same reason the Aegis latch does: the UI loop is the one
 * thing that runs continuously and always knows which lens is active, so it is
 * the only place a picture can accumulate ACROSS the rotation. A lens cannot
 * hold this - it is unmounted every few seconds. */
static psv_t s_survey;

/* ---- motion ----------------------------------------------------------
 *
 * Sampled here rather than in a lens because it must run whatever lens the
 * rotation is on: Vigil needs to know whether the person walked somewhere
 * during the forty seconds it was NOT holding the radio, and a lens that is
 * unmounted cannot measure that. */
static pm_engine_t s_motion;

static ptw_state_st s_tower;
static bool s_tower_on;
static const char *s_tower_pending; /* lens the rotation wants next */
static unsigned s_home_sel;         /* which dot the side zones point at */
static int s_home_tap = -1;         /* a dot touched on the glass, or -1 */
/* Display position -> tower index. The ring carries only ARMED watches, so the
 * two are not the same once anything has been switched off. */
static uint8_t s_home_map[PHAROS_HUD_HOME_MAX];
static unsigned s_home_map_n;
static uint32_t s_paints, s_paint_misses;

/* How a lens' own display maps onto the one scale the ring can draw. Every
 * engine has its own bands; the ring needs a single answer to "how much should
 * this worry somebody", so the score - which every engine already calibrates
 * against its own ceiling - is what carries across. */
static ptw_state_t tower_state_of(const struct pharos_lens_display *d)
{
    if (!d) {
        return PTW_QUIET;
    }
    /* A lens whose score is not a threat scale states its own ring severity;
     * see pharos_lens_display::alert. Only the lenses whose score genuinely
     * measures threat let the ring read it off the number. */
    if (d->has_alert) {
        switch (d->alert) {
        case 3:  return PTW_ALARM;
        case 2:  return PTW_ELEVATED;
        case 1:  return PTW_NOTED;
        default: return PTW_QUIET;
        }
    }
    if (!d->has_score) {
        return PTW_QUIET; /* running, nothing to report is a real answer */
    }
    if (d->score >= 70u) return PTW_ALARM;
    if (d->score >= 45u) return PTW_ELEVATED;
    if (d->score >= 20u) return PTW_NOTED;
    return PTW_QUIET;
}
static unsigned s_cursor;
static view_t s_view = VIEW_HOME;
static unsigned s_detail_page;
/* Which row the centre tap would open, as an absolute index across the whole
 * list. The page shown follows it, so moving the cursor off the bottom turns
 * the page rather than making the operator do both. */
static unsigned s_detail_cursor;
static unsigned s_opened_row;
/* A row touched on the glass, 0..ROWS-1 within the page shown, or -1. Filed
 * from LVGL's task and acted on by the UI task, same as s_nav_pending: doing
 * the work in the callback runs it on LVGL's stack and reboots the board. */
static int s_row_pending = -1;
static volatile int s_nav_pending = -1; /* pharos_nav_t, or -1 for none */

static void on_nav(pharos_nav_t what)
{
    s_nav_pending = (int)what; /* record only - see the note above */
}

void pharos_ui_request_nav(pharos_nav_t what)
{
    /* Deliberately the same slot the touch callback writes, so a console-driven
     * test cannot take a different path from a finger. */
    on_nav(what);
}

/* Colour a lens by which team it serves, so the browser is readable at a
 * glance rather than uniformly cyan. */
static uint32_t lens_rgb(const pharos_lens_t *l)
{
    if (!l) return 0x1FB6C9;
    switch (l->kind) {
    case PHAROS_LENS_TRAIN:  return 0xE8A33F; /* amber: training      */
    case PHAROS_LENS_SYSTEM: return 0x7FA6B5; /* slate: housekeeping  */
    case PHAROS_LENS_ANALYSE:return 0xB07FE8; /* violet: analysis     */
    case PHAROS_LENS_OBSERVE:
    default:                 return 0x1FB6C9; /* cyan: listening      */
    }
}

static const char *lens_team(const pharos_lens_t *l)
{
    if (!l) return "";
    switch (l->kind) {
    case PHAROS_LENS_TRAIN:   return "train";
    case PHAROS_LENS_SYSTEM:  return "system";
    case PHAROS_LENS_ANALYSE: return "analyse";
    case PHAROS_LENS_OBSERVE:
    default:                  return "watch";
    }
}

static bool lens_launchable(const pharos_lens_t *l);
static void theme_sync(void);
static bool lens_switch(const char *id);

/* Which lenses go on the ring.
 *
 * The observing lenses - the ones that answer a question about the room right
 * now. A trainer has nothing to watch, and the settings page is not a watch,
 * so putting either on the ring would pad the count in "6 watches armed" with
 * things that cannot report. */
static void tower_arm_all(void)
{
    /* THE RING IS ORDERED ON PURPOSE, AND IT IS SHORT ON PURPOSE.
     *
     * Registry order put Watch - the headline detector, the whole reason this
     * project exists - tenth of twelve, and twelve watches at six seconds each
     * is a seventy-second lap. Every dot would spend most of its life stale,
     * which on a ring whose honesty rests on freshness is close to useless.
     *
     * So: a stated running order, headliners first, and a cap. Eight watches
     * at five seconds is a forty-second lap, and eight labels fit round a
     * 466 px circle without colliding. Anything not on the ring is still one
     * press away through the browser - it just is not something the device
     * promises to keep checking on its own. */
    /* THE RING IS ORDERED ON PURPOSE, AND WEIGHTED ON PURPOSE.
     *
     * Registry order put Watch - the headline detector, the whole reason this
     * project exists - tenth of twelve. And arming everything uniformly makes
     * a sixty-five second lap, so the flood detector would be deaf for a
     * minute at a stretch in order to re-count the same access points.
     *
     * So each watch says how often it needs the radio. An EVENT lasts seconds
     * and is missed if you are elsewhere; a STANDING FACT changes over minutes
     * and is none the worse for being checked every other lap. Adding more
     * surveys now costs the event detectors nothing. */
    /* `on` is whether it ships ARMED. All thirteen are available and one tap
     * away in the Ring lens; ten are on to begin with, because that is what
     * the dial can label comfortably.
     *
     * That number is measured, not chosen by eye: pd_ring_layout() leaves
     * 21 px between names at ten watches, 12 px at twelve, and 10 px at
     * thirteen - which is the point at which two names read as one long word.
     * See test_ring.c, which pins the spacing at every count. Somebody who
     * wants all thirteen can have them, knowing what they are trading. */
    static const struct { const char *id; uint8_t period; bool on; } k_ring[] = {
        /* Events: every lap. Miss the lap, miss the attack. */
        { "wifi.watch",   1, true  }, /* deauthentication - the headliner  */
        { "wifi.karma",   1, true  }, /* a radio answering to any name     */
        { "wifi.mirage",  1, true  }, /* beacon and SSID floods            */
        { "wifi.harvest", 1, true  }, /* handshake and PMKID collection    */
        { "wifi.twin",    1, true  }, /* evil twin / rogue AP              */
        { "wifi.ward",    1, false }, /* one network, guarded specifically */
        { "rf.rival",     1, true  }, /* Flippers and pentest hardware     */
        { "net.roster",   1, true  }, /* every device here, and what it leaks */

        /* Standing facts: every other lap is plenty. */
        { "wifi.census",  2, true  }, /* how safe the neighbours are       */
        { "wifi.probe",   2, true  }, /* what devices leak by asking       */
        { "wifi.squall",  2, true  }, /* busy, broken, or jammed           */
        { "ble.vigil",    2, true  }, /* is a tracker travelling with you  */
        { "mic.whisper",  2, false }, /* ultrasonic beacons in the room    */

        /* Slower still: a baseline drifts over many minutes, and the
         * spectrum waterfall is a picture to go and look at rather than
         * something that needs catching in the act. */
        { "wifi.sentinel", 3, false },
        { "wifi.spectrum", 3, false },
        /* Off by default: it grades how loud YOUR OWN tooling is, which is a
         * thing somebody switches on deliberately while testing, not a watch
         * that wants a slice of every lap. */
        { "train.footprint", 4, false },
    };
    const unsigned want = (unsigned)(sizeof(k_ring) / sizeof(k_ring[0]));

    ptw_reset(&s_tower, 5000);
    for (unsigned i = 0; i < want && s_tower.n < PTW_MAX_WATCHES; i++) {
        const pharos_lens_t *l = pharos_lens_find(k_ring[i].id);
        /* A name here that does not resolve is a typo or a renamed lens. It
         * degrades gracefully - the ring just carries one fewer watch - and
         * that is exactly why it has to be LOUD: "rf.rival" was written
         * "ble.rival" and the only symptom was a ring with seven dots instead
         * of eight, which looks like a design decision. tools/check_lenses.sh
         * now fails the build on it; this catches a lens renamed at runtime. */
        if (!l) {
            ESP_LOGE(TAG, "watchtower: no lens '%s' - ring is short one watch",
                     k_ring[i].id);
            continue;
        }
        if (!l->display || !lens_launchable(l)) {
            ESP_LOGW(TAG, "watchtower: %s cannot join the ring%s", l->id,
                     l->display ? " (radio locked)" : " (no display)");
            continue;
        }
        /* Eight characters, not eleven: thirteen labels round a 466 px circle
         * leaves about seventy pixels each, and eleven characters needs more
         * than that - so the longest names would have overlapped their
         * neighbours rather than been read. Every lens name is legible at
         * eight ("SENTINEL", "SPECTRUM", "HARVEST"). */
        /* Seven characters. Thirteen labels round a 466 px circle leaves
         * about sixty pixels of arc each, and the ones that meet horizontally
         * at the top and bottom of the dial have less than that - so the long
         * names ran into their neighbours. Staggered radii (see home_layout)
         * do most of the work; this does the rest. */
        /* Nine, not seven. Fewer labels are drawn now, so there is room for
         * a name that is actually a name - SENTINEL rather than SENTINE. */
        char up[10];
        unsigned k = 0;
        for (; l->name[k] && k < sizeof(up) - 1; k++) {
            const char c = l->name[k];
            up[k] = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
        }
        up[k] = '\0';
        const int slot = ptw_arm_every(&s_tower, l->id, up, k_ring[i].period);
        if (slot >= 0 && !k_ring[i].on) {
            /* Available, not armed. Refused only if it would empty the ring,
             * which cannot happen while anything above it is on. */
            ptw_set_armed(&s_tower, (unsigned)slot, false);
        }
    }
    /* The ARMED count, not the registered one. This said "16 watches armed"
     * while eleven were, which is how a default that was not being applied
     * looked exactly like one that was. */
    unsigned on = 0;
    for (unsigned i = 0; i < s_tower.n; i++) {
        if (s_tower.w[i].armed) {
            on++;
        }
    }
    ESP_LOGI(TAG, "watchtower: %u of %u watches armed by default, %ums a lap",
             on, s_tower.n, (unsigned)ptw_lap_ms(&s_tower));
}

/* Open the watch a dot names.
 *
 * Picking a watch by hand STOPS the rotation. Somebody who chose Watch did not
 * ask to be moved off it five seconds later, and a device that wandered away
 * from the lens you just opened would be unusable. Long-pressing back to the
 * ring resumes it. */
static void home_open(unsigned i)
{
    if (i >= s_tower.n) {
        return;
    }
    const pharos_lens_t *l = pharos_lens_find(s_tower.w[i].id);
    if (!l) {
        return;
    }
    if (!lens_launchable(l)) {
        if (pharos_bsp_display_lock(30)) {
            pharos_hud_toast("radio locked");
            pharos_bsp_display_unlock();
        }
        return;
    }
    if (lens_switch(l->id)) { /* which pauses the rotation; see lens_switch */
        s_view = VIEW_LIVE;
        /* Keep the browser's cursor with the ring, so leaving a lens the old
         * way lands somewhere that makes sense. */
        for (unsigned k = 0; k < s_order_n; k++) {
            if (s_order[k] == l) {
                s_cursor = k;
                break;
            }
        }
        ESP_LOGI(TAG, "watchtower: opened %s (rotation paused)", l->id);
    }
}

static void paint_home(void)
{
    /* Sixty, not thirty. The LVGL task holds this while it composites, and a
     * full ring repaint under radio load takes longer than 30 ms - so the
     * paint that wanted the lock gave up, the vendor adapter logged an error,
     * and the face dropped the frame. Waiting a little longer costs this loop
     * nothing it was doing anything else with, and the miss counter below
     * still reports it if the wait was not enough. */
    if (!pharos_bsp_display_lock(60)) {
        s_paint_misses++;
        return;
    }
    s_paints++;
    pharos_hud_create();
    theme_sync();

    const uint64_t now = (uint64_t)esp_timer_get_time();
    ptw_summary_t sum;
    ptw_summarise(&s_tower, now, &sum);

    /* ONLY THE ARMED WATCHES GET A DOT.
     *
     * A watch somebody switched off must not occupy a slot on the ring - the
     * spacing is computed from the count, so leaving gaps would spread twelve
     * dots over thirteen places and lie about what is being watched. The map
     * carries display position back to tower index, because a tap arrives as
     * the former and every other call here takes the latter. */
    struct pharos_hud_home h;
    memset(&h, 0, sizeof(h));
    s_home_map_n = 0;
    for (unsigned i = 0; i < s_tower.n && s_home_map_n < PHAROS_HUD_HOME_MAX; i++) {
        if (!s_tower.w[i].armed) {
            continue;
        }
        const unsigned d = s_home_map_n;
        s_home_map[d] = (uint8_t)i;
        h.label[d] = s_tower.w[i].name;
        h.state[d] = (uint8_t)s_tower.w[i].state;
        h.fade[d] = (uint8_t)ptw_freshness(&s_tower, i, now);
        h.score[d] = s_tower.w[i].score;
        s_home_map_n++;
    }
    h.n = s_home_map_n;
    h.active = -1;
    {
        const pharos_lens_t *live = pharos_lens_active();
        if (live && s_tower_on) {
            const int idx = ptw_find(&s_tower, live->id);
            for (unsigned d = 0; d < s_home_map_n; d++) {
                if ((int)s_home_map[d] == idx) {
                    h.active = (int)d;
                    break;
                }
            }
        }
    }

    /* WHICH DOTS GET A NAME.
     *
     * The dial holds twelve labels (pd_ring_capacity), and fourteen watches
     * were being drawn anyway - the two that did not fit went through the
     * headline. Rather than shrink the text until nothing is readable, the
     * ring names what matters and leaves the rest as dots:
     *
     *   1. whichever watch holds the radio, so "what am I listening to" never
     *      needs a tap;
     *   2. anything with something to report, because a coloured dot tells you
     *      something is wrong and the name tells you what;
     *   3. whatever the side controls have selected, so stepping round the
     *      ring names each one as you reach it.
     *
     * In a quiet room that is one or two labels and the dial is clean. When
     * something happens, the relevant watches name themselves. */
    {
        /* Sized by the names actually on the ring, not by the longest name in
         * the project. Assuming every watch is nine characters wide - which
         * only FOOTPRINT is - cost one label at eleven armed, and an unnamed
         * dot among named ones reads as a fault rather than a decision. */
        unsigned widest = 0;
        for (unsigned d = 0; d < s_home_map_n; d++) {
            const char *nm = s_tower.w[s_home_map[d]].name;
            unsigned k = 0;
            while (nm[k]) {
                k++;
            }
            if (k > widest) {
                widest = k;
            }
        }
        const int16_t lw = (int16_t)((widest ? widest : 7u) * 76u / 10u + 4u);
        /* Handed to the HUD so it sizes the ring against the same width this
         * capacity was computed from - see pharos_hud_home::label_w. */
        h.label_w = lw;
        const unsigned cap = pd_ring_capacity(lw, 14, 12);
        unsigned used = 0;
        if (h.active >= 0 && used < cap) {
            h.label_on[h.active] = true;
            used++;
        }
        if (s_home_sel < s_home_map_n && !h.label_on[s_home_sel] && used < cap) {
            h.label_on[s_home_sel] = true;
            used++;
        }
        /* Worst first, so if there are more findings than room the loudest
         * ones keep their names. */
        for (unsigned pass = PTW_ALARM; pass >= PTW_NOTED && used < cap; pass--) {
            for (unsigned d = 0; d < s_home_map_n && used < cap; d++) {
                if (!h.label_on[d] && h.state[d] == pass) {
                    h.label_on[d] = true;
                    used++;
                }
            }
        }
        /* If everything is quiet there is room to spare, so name as many as
         * will fit rather than leaving a dial of anonymous dots. */
        for (unsigned d = 0; d < s_home_map_n && used < cap; d++) {
            if (!h.label_on[d]) {
                h.label_on[d] = true;
                used++;
            }
        }
    }

    h.headline = sum.headline;
    /* A room that is quiet NOW but was attacked ten minutes ago must not read
     * as green. The latched state colours the face; the sub-line says when. */
    h.worst_state = (uint8_t)((sum.latched_state > sum.worst) ? sum.latched_state
                                                             : sum.worst);

    /* PAUSED IS NOT QUIET.
     *
     * With the rotation stopped the watches stop reporting, every dot goes
     * hollow, and the summary - which only counts watches that have reported
     * recently - correctly finds nothing to worry about and says "all quiet".
     * Which is true, and is the most dangerous sentence this screen could
     * show: the room is quiet because nobody is listening to it.
     *
     * The state of the device beats the state of the room. */
    if (!s_tower_on && s_tower.n) {
        h.headline = "NOT WATCHING";
        h.worst_state = 2u; /* amber: this is a thing to notice, not an alarm */
        h.worst_score = 0;
    }
    h.worst_score = (sum.worst_index >= 0) ? s_tower.w[sum.worst_index].score : 0;

    /* THE SUB-LINE, WHICH NOW CARRIES THE HINT TOO.
     *
     * It read "SPECTRUM" on its own when something was elevated, which on the
     * glass looked like a stray fourteenth label rather than a statement about
     * the ring - there was already a SPECTRUM label out on the rim, so the
     * word appeared twice with nothing to distinguish them. It says what the
     * name MEANS now.
     *
     * And when there is nothing to report it carries the "tap the middle"
     * hint, which used to be a 225 px line of its own drawn straight through
     * the label ring. */
    /* THE SUB-LINE TEACHES THE DIAL ITS OWN VOCABULARY.
     *
     * The names on the rim are evocative and opaque - KARMA and SQUALL tell a
     * person nothing about what they will see if they press one. Rather than
     * rename fourteen lenses into something duller, the centre says what the
     * watch currently holding the radio is FOR, in plain words. The rotation
     * then walks the operator through the whole vocabulary on its own, one
     * watch at a time, while it works.
     *
     * A finding outranks the lesson: when something is actually up, this line
     * names the watch that found it instead. */
    static char sub[40];
    if (!s_fence_ok) {
        snprintf(sub, sizeof(sub), "FENCE UNVERIFIED");
    } else if (!s_tower_on) {
        snprintf(sub, sizeof(sub), "hold to start the %u watches", sum.armed);
    } else if (sum.worst_index >= 0 && sum.worst >= PTW_ELEVATED) {
        snprintf(sub, sizeof(sub), "worst: %s",
                 s_tower.w[sum.worst_index].name);
    } else if (sum.latched_index >= 0) {
        /* WHAT HAPPENED WHILE NOBODY WAS LOOKING.
         *
         * The age is not decoration - it is the difference between "there is
         * an attack" and "there was one", and somebody walking up to the
         * device has to be able to tell those apart at a glance. */
        const uint32_t a = sum.latched_age_s;
        if (a >= 3600u) {
            snprintf(sub, sizeof(sub), "%s %uh ago",
                     s_tower.w[sum.latched_index].name, (unsigned)(a / 3600u));
        } else if (a >= 60u) {
            snprintf(sub, sizeof(sub), "%s %um ago",
                     s_tower.w[sum.latched_index].name, (unsigned)(a / 60u));
        } else {
            snprintf(sub, sizeof(sub), "%s %us ago",
                     s_tower.w[sum.latched_index].name, (unsigned)a);
        }
    } else {
        const pharos_lens_t *live = pharos_lens_active();
        if (live && live->purpose && live->purpose[0]) {
            snprintf(sub, sizeof(sub), "%s", live->purpose);
        } else {
            snprintf(sub, sizeof(sub), "%u watches - tap one", sum.armed);
        }
    }
    h.sub = sub;

    /* The clock is uptime until something sets the time - a wall clock this
     * device has no way to know would be a made-up number on the largest text
     * on the screen. */
    static char clk[12];
    const uint32_t up_s = (uint32_t)(now / 1000000ull);
    snprintf(clk, sizeof(clk), "%02u:%02u", (unsigned)((up_s / 60u) % 100u),
             (unsigned)(up_s % 60u));
    h.clock = clk;

    pharos_hud_home(&h);
    pharos_bsp_display_unlock();
}

/* Paint the browse card for wherever the cursor is. */
static void paint_browse(void)
{
    if (!s_order_n || !pharos_bsp_display_lock(30)) {
        return;
    }
    pharos_hud_create();
    const pharos_lens_t *l = s_order[s_cursor % s_order_n];
    pharos_hud_browse(l ? l->name : "", l ? l->summary : "", lens_team(l),
                      s_cursor % s_order_n, s_order_n, lens_rgb(l));
    pharos_bsp_display_unlock();
}

/* Run the pending intent. Called from the UI task only. */
/* Requests filed by other tasks (the console). Applied here, on the UI task,
 * so lens switching stays single-threaded - and so Wi-Fi initialisation never
 * runs on the REPL's small stack. */
static void request_apply(void)
{
    if (s_req_stop) {
        s_req_stop = false;
        lens_halt();
        s_view = VIEW_BROWSE;
        paint_browse();
        return;
    }
    if (s_req_lens[0] == '\0') {
        return;
    }
    char id[32];
    strncpy(id, s_req_lens, sizeof(id) - 1);
    id[sizeof(id) - 1] = '\0';
    s_req_lens[0] = '\0';

    if (lens_switch(id)) {
        /* CHOOSING A LENS BY HAND STOPS THE ROTATION.
         *
         * Same rule as tapping a dot on the ring, and for the same reason:
         * somebody who asked for Roster did not ask to be moved off it six
         * seconds later. The console path was missing it, which made `lens`
         * effectively unusable while the watchtower was running - the tower
         * simply took the radio back at the next turn, and every attempt to
         * inspect a lens over USB landed on whatever was next in the rota. */
        s_tower_on = false;
        s_tower_pending = NULL;
        s_view = VIEW_LIVE;
        for (unsigned i = 0; i < s_order_n; i++) {
            if (s_order[i] && strcmp(s_order[i]->id, id) == 0) {
                s_cursor = i;
                break;
            }
        }
        ESP_LOGI(TAG, "console started %s (rotation paused)", id);
    } else {
        ESP_LOGW(TAG, "console could not start %s", id);
    }
}

/* Filed from LVGL's task. Record and return - see pharos_hud.h. */
static void hud_home_cb(unsigned dot)
{
    s_home_tap = (int)dot;
}

/* Filed from LVGL's task. Record and return - see pharos_hud.h. */
static void hud_row_cb(unsigned row_on_page)
{
    s_row_pending = (int)row_on_page;
}

/* ONE TOUCH DOES THE WHOLE JOB.
 *
 * Reaching a setting used to be: press a side zone until a cursor arrived on
 * the row, then press the centre. Four presses to change the volume, on a
 * device somebody is holding up one-handed. Touching the row is one press, and
 * it lands on the row under the finger rather than wherever a counter had got
 * to.
 *
 * The row is focused as well as acted on, because the pill appearing under the
 * finger is what says the press registered - on a control that CYCLES (theme,
 * volume, region) the value changing is the only other feedback, and if the
 * press missed there is none at all. */
/* A dot touched on the ring. Same rule as everywhere else: LVGL's callback
 * records, the UI task acts. */
static void home_apply(void)
{
    const int want = s_home_tap;
    s_home_tap = -1;
    if (want < 0 || s_view != VIEW_HOME) {
        return;
    }
    /* The middle of the ring: "tell me more about all of this". The Survey is
     * the accumulated picture of the place - every network and device seen
     * this session - which is exactly what somebody reaching for the centre of
     * a summary screen is asking for. */
    if ((unsigned)want == PHAROS_HUD_HOME_CORE) {
        if (lens_switch("sys.survey")) {
            s_view = VIEW_LIVE;
            ESP_LOGI(TAG, "watchtower: opened the survey");
        }
        return;
    }
    if ((unsigned)want >= s_home_map_n) {
        return;
    }
    s_home_sel = (unsigned)want;
    home_open(s_home_map[want]);
}

static void row_apply(void)
{
    const int want = s_row_pending;
    s_row_pending = -1;
    if (want < 0 || (s_view != VIEW_DETAIL && s_view != VIEW_OPENED)) {
        return;
    }
    if (s_view == VIEW_OPENED) {
        /* An opened row is a page of readings, not a menu. Touching it closes
         * it, which is what a finger reaching for a full-screen page means. */
        s_view = VIEW_DETAIL;
        s_detail_page = s_opened_row / PHAROS_HUD_ROWS;
        return;
    }
    const pharos_lens_t *live = pharos_lens_active();
    if (!live || !live->row) {
        return;
    }
    const unsigned abs_row = s_detail_page * PHAROS_HUD_ROWS + (unsigned)want;

    /* Never act on a row that is not there. The last page is usually short,
     * and without this a press below the final row would edit or open
     * whatever index happened to be one past the end of the list. */
    struct pharos_lens_row probe;
    memset(&probe, 0, sizeof(probe));
    if (!live->row(abs_row, &probe)) {
        return;
    }
    s_detail_cursor = abs_row;

    if (live->row_edit && live->row_edit(abs_row)) {
        return;
    }
    if (live->row_expand) {
        s_opened_row = abs_row;
        s_detail_page = 0;
        s_view = VIEW_OPENED;
    }
}

/* ---- the first-run guide ---------------------------------------------
 *
 * Twenty-one tools, four verdict colours, and a touch surface with no visible
 * buttons. The console banner explained the controls, which is no help at all:
 * a person meeting this device is looking at the GLASS, not at a serial
 * terminal on a laptop they may not even have connected.
 *
 * So the glass explains itself, once. Each step shows the gesture rather than
 * describing it - the outline of a zone appears and a fingertip pulses inside
 * it - because somebody copies what they just watched and skims what they had
 * to read.
 *
 * Order matters. Controls first (you cannot explore without them), then the
 * colour key (it is the key to every other screen), then the two pieces of
 * vocabulary that are actually unusual: the ring of dots and the evidence
 * chips. Nine steps, which is about as long as anybody will sit through.
 */
static const struct pharos_hud_guide k_guide[] = {
    /* Every string here is sized against the chord at the band it is drawn
     * in - see PS_Y_GUIDE_* in pharos_style.h. The bottom of a circle is
     * narrow: the hint band holds about 27 characters and the second body
     * line about 27, so these are not arbitrary phrasings. tools/render
     * bounds-checks them, and caught ten escapes on the first draft. */
    { 0, 9, "Pharos",
      "It listens to the air",
      "and never transmits.",
      "tap the right side", PHAROS_GUIDE_ANIM_NONE },

    { 1, 9, "Change tool",
      "Tap the left or right edge",
      "to move through the tools.",
      "try it, this page waits", PHAROS_GUIDE_ANIM_SIDES },

    { 2, 9, "Start it",
      "Tap the middle to run",
      "the tool you are on.",
      "right side to go on", PHAROS_GUIDE_ANIM_CENTRE },

    { 3, 9, "See the evidence",
      "Tap the bottom strip for",
      "the numbers behind it.",
      "right side to go on", PHAROS_GUIDE_ANIM_BOTTOM },

    { 4, 9, "Go back",
      "Press and hold to stop",
      "and step back out.",
      "right side to go on", PHAROS_GUIDE_ANIM_HOLD },

    { 5, 9, "The colour key",
      "Every screen uses these four.",
      "You may stop at the colour.",
      "right side to go on", PHAROS_GUIDE_ANIM_VERDICT },

    { 6, 9, "The home ring",
      "One dot for each watch.",
      "Count the ones not green.",
      "right side to go on", PHAROS_GUIDE_ANIM_RING },

    { 7, 9, "Why it thinks so",
      "Lit chips name the evidence",
      "a verdict was built from.",
      "right side to go on", PHAROS_GUIDE_ANIM_CHIPS },

    { 8, 9, "Ready",
      "Nothing here transmits.",
      "Tap the middle to begin.",
      "tap the middle", PHAROS_GUIDE_ANIM_NONE },
};
#define GUIDE_STEPS ((unsigned)(sizeof(k_guide) / sizeof(k_guide[0])))

static unsigned s_guide_step;

static void paint_guide(void)
{
    if (!pharos_bsp_display_lock(30)) {
        s_paint_misses++;
        return;
    }
    s_paints++;
    pharos_hud_create();
    theme_sync();
    pharos_hud_guide(&k_guide[s_guide_step < GUIDE_STEPS ? s_guide_step : 0]);
    pharos_bsp_display_unlock();
}

/* Remembered so it appears once and never nags. Replayable forever with the
 * `guide` command, which is also what somebody handing the device to a
 * colleague wants. */
static bool guide_seen(void)
{
    nvs_handle_t h;
    if (nvs_open("pharos", NVS_READONLY, &h) != ESP_OK) {
        return false;
    }
    uint8_t v = 0;
    const bool ok = (nvs_get_u8(h, "guide_seen", &v) == ESP_OK) && v;
    nvs_close(h);
    return ok;
}

static void guide_mark_seen(void)
{
    nvs_handle_t h;
    if (nvs_open("pharos", NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    nvs_set_u8(h, "guide_seen", 1);
    nvs_commit(h);
    nvs_close(h);
}

void pharos_ui_guide_start(void)
{
    s_guide_step = 0;
    s_view = VIEW_GUIDE;
}

/* Left/right walk it, the centre finishes it. Returns true when the guide
 * consumed the press, which keeps every other view's handling untouched. */
static bool guide_nav(pharos_nav_t what)
{
    if (s_view != VIEW_GUIDE) {
        return false;
    }
    switch (what) {
    case PHAROS_NAV_NEXT:
        if (s_guide_step + 1u < GUIDE_STEPS) {
            s_guide_step++;
        } else {
            guide_mark_seen();
            s_view = VIEW_HOME;
        }
        return true;
    case PHAROS_NAV_PREV:
        if (s_guide_step) {
            s_guide_step--;
        }
        return true;
    case PHAROS_NAV_SELECT:
    case PHAROS_NAV_HOME:
    case PHAROS_NAV_DETAIL:
    default:
        /* Anything else leaves. Somebody who already knows the device should
         * not have to walk nine screens to get out of the tutorial. */
        guide_mark_seen();
        s_view = VIEW_HOME;
        return true;
    }
}

static void nav_apply(void)
{
    const int want = s_nav_pending;
    if (want < 0) {
        return;
    }
    s_nav_pending = -1;
    if (guide_nav((pharos_nav_t)want)) {
        return;
    }
    if (!s_order_n) {
        return;
    }

    /* THE RING IS NOT A LIST. Left and right step the ring's own selection,
     * the centre opens whatever is selected, and the bottom strip is what
     * takes somebody out to the old one-lens-at-a-time browser. */
    if (s_view == VIEW_HOME) {
        switch ((pharos_nav_t)want) {
        case PHAROS_NAV_NEXT:
            if (s_home_map_n) {
                s_home_sel = (s_home_sel + 1u) % s_home_map_n;
            }
            return;
        case PHAROS_NAV_PREV:
            if (s_home_map_n) {
                s_home_sel = (s_home_sel + s_home_map_n - 1u) % s_home_map_n;
            }
            return;
        case PHAROS_NAV_SELECT: {
            /* A press in the middle of the ring, with something remembered,
             * means "I have seen it". Only a person can clear a latched
             * finding - time passing is not somebody having looked. */
            ptw_summary_t ack;
            ptw_summarise(&s_tower, (uint64_t)esp_timer_get_time(), &ack);
            if (ack.latched_index >= 0 && ack.worst < PTW_ELEVATED) {
                ptw_acknowledge(&s_tower);
                ESP_LOGI(TAG, "watchtower: findings acknowledged");
                if (pharos_bsp_display_lock(30)) {
                    pharos_hud_toast("acknowledged");
                    pharos_bsp_display_unlock();
                }
                return;
            }
            if (s_home_sel < s_home_map_n) {
                home_open(s_home_map[s_home_sel]);
            }
            return;
        }
        case PHAROS_NAV_DETAIL:
            s_view = VIEW_BROWSE;
            paint_browse();
            return;
        case PHAROS_NAV_HOME:
        default:
            /* Already home. A hold opens the ring's own settings.
             *
             * It used to toggle the rotation, silently - so a stray long-press
             * could stop the device watching with nothing on screen to say it
             * had happened. That is a bad thing for a monitor to be able to do
             * by accident. The pause is still available, as a labelled row on
             * the page this now opens. */
            if (lens_switch("sys.ring")) {
                s_view = VIEW_DETAIL;
                s_detail_page = 0;
                s_detail_cursor = 0;
                ESP_LOGI(TAG, "watchtower: editing the ring");
            }
            return;
        }
    }

    switch ((pharos_nav_t)want) {
    case PHAROS_NAV_DETAIL: {
        /* Only meaningful while something is running and has rows to show. */
        const pharos_lens_t *live = pharos_lens_active();
        if (s_view == VIEW_DETAIL) {
            s_view = VIEW_LIVE;
            return;
        }
        if (s_view == VIEW_LIVE && live && live->row) {
            s_detail_page = 0;
            s_detail_cursor = 0;
            s_view = VIEW_DETAIL;
            return;
        }
        if (s_view == VIEW_LIVE) {
            if (pharos_bsp_display_lock(30)) {
                pharos_hud_toast("no detail here");
                pharos_bsp_display_unlock();
            }
        }
        return;
    }
    case PHAROS_NAV_NEXT:
        /* In DETAIL the sides move the CURSOR rather than changing lens - you
         * are reading, not browsing, and losing your place to a stray tap
         * would make a long list unusable. The page follows the cursor, so
         * nobody has to move both. */
        if (s_view == VIEW_DETAIL || s_view == VIEW_OPENED) {
            /* PAGE, not step. Rows are touched directly now, so the side
             * controls no longer have to walk a cursor to reach one - and a
             * control that moves by a whole screen is worth its size. */
            s_detail_page++;
            s_detail_cursor = s_detail_page * PHAROS_HUD_ROWS;
            return;
        }
        s_cursor = (s_cursor + 1u) % s_order_n;
        break;
    case PHAROS_NAV_PREV:
        if (s_view == VIEW_DETAIL || s_view == VIEW_OPENED) {
            if (s_detail_page) {
                s_detail_page--;
            }
            s_detail_cursor = s_detail_page * PHAROS_HUD_ROWS;
            return;
        }
        s_cursor = (s_cursor + s_order_n - 1u) % s_order_n;
        break;
    case PHAROS_NAV_SELECT: {
        if (s_view == VIEW_OPENED) {
            s_view = VIEW_DETAIL; /* close it again */
            s_detail_page = 0;
            return;
        }
        if (s_view == VIEW_DETAIL) {
            const pharos_lens_t *live = pharos_lens_active();
            /* Change it if it is changeable, otherwise open it. A lens may
             * offer both, row by row. */
            if (live && live->row_edit && live->row_edit(s_detail_cursor)) {
                return;
            }
            if (live && live->row_expand) {
                s_opened_row = s_detail_cursor;
                s_detail_page = 0;
                s_view = VIEW_OPENED;
            }
            return;
        }
        if (s_view == VIEW_LIVE) {
            /* The centre used to do nothing here, and that hole is why a
             * SUSPICIOUS Watch reading was a dead end: the way to raise the
             * confidence ceiling is to stop hopping, and no control on the
             * glass did it. A running lens may now claim the tap. This runs on
             * the UI task, which is the only task allowed to touch the radio -
             * see the note on the touch callback above. */
            const pharos_lens_t *live = pharos_lens_active();
            if (live && live->on_select) {
                live->on_select();
            }
            return;
        }
        const pharos_lens_t *l = s_order[s_cursor % s_order_n];
        if (!l) {
            return;
        }
        if (!lens_launchable(l)) {
            if (pharos_bsp_display_lock(30)) {
                pharos_hud_toast("radio locked");
                pharos_bsp_display_unlock();
            }
            return;
        }
        if (lens_switch(l->id)) {
            s_view = VIEW_LIVE;
            ESP_LOGI(TAG, "started %s", l->id);
        } else {
            ESP_LOGW(TAG, "could not start %s", l->id);
            if (pharos_bsp_display_lock(30)) {
                pharos_hud_toast("would not start");
                pharos_bsp_display_unlock();
            }
        }
        return;
    }
    case PHAROS_NAV_HOME:
    default:
        if (s_view == VIEW_OPENED) {
            s_view = VIEW_DETAIL;
            s_detail_page = 0;
            return;
        }
        if (s_view == VIEW_DETAIL) {
            s_view = VIEW_LIVE; /* one step back, not all the way out */
            return;
        }
        /* Out of a lens goes back to the RING, not to the one-at-a-time
         * browser: the ring is where somebody can see everything, so it is
         * where "back" should land. The rotation picks up again by itself. */
        lens_halt();
        s_tower_on = true;
        s_view = VIEW_HOME;
        ESP_LOGI(TAG, "stopped; back to the watchtower");
        return;
    }

    /* NEXT/PREV: stepping the list stops whatever is running, so the reading
     * on screen always belongs to the lens named above it. */
    if (s_view == VIEW_LIVE) {
        lens_halt();
        s_view = VIEW_BROWSE;
    }
    paint_browse();
}

/* The board exposes no user buttons (BSP_CAPS_BUTTONS is 0); the two on the
 * side are RESET and BOOT. BOOT is GPIO0 and readable, so it becomes a
 * physical control - useful with gloves, in a pocket, or if the touch
 * controller is dead, which is a real case on this board. */
#define PHAROS_BOOT_GPIO 0

static void boot_button_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = 1ULL << PHAROS_BOOT_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    gpio_config(&io);
}

static void boot_button_poll(uint32_t dt_ms)
{
    static bool was_down;
    static uint32_t held_ms;
    const bool down = (gpio_get_level(PHAROS_BOOT_GPIO) == 0); /* active low */

    if (down) {
        held_ms += dt_ms;
        was_down = true;
        return;
    }
    if (was_down) {
        /* Hold goes back; a short press advances in browse, and starts what is
         * under the cursor when you are already looking at it. */
        if (held_ms >= 800u) {
            s_nav_pending = (int)PHAROS_NAV_HOME;
        } else {
            s_nav_pending = (int)(s_view == VIEW_BROWSE ? PHAROS_NAV_SELECT
                                                        : PHAROS_NAV_NEXT);
        }
        was_down = false;
        held_ms = 0;
    }
}

unsigned pharos_ui_pump(void)
{
    /* Held for the whole drain: the lens, its ingest ring and the state
     * on_event writes into must all still belong to the same lens when we
     * finish. A short timeout keeps the analytics task responsive while a
     * switch is in progress rather than piling up behind it. */
    if (!s_lens_mtx || xSemaphoreTake(s_lens_mtx, pdMS_TO_TICKS(20)) != pdTRUE) {
        return 0;
    }
    unsigned n = 0;
    const pharos_lens_t *lens = pharos_lens_active();
    if (lens && lens->ingest && lens->on_event) {
        struct pharos_bus *bus = lens->ingest();
        if (bus) {
            pharos_event_t ev;
            /* Bounded per call so one very busy lens cannot starve the loop. */
            while (n < 256 && pharos_bus_pop((pharos_bus_t *)bus, &ev)) {
                lens->on_event(&ev);
                n++;
            }
        }
    }
    xSemaphoreGive(s_lens_mtx);
    return n;
}

/* Every lens change in the firmware goes through these two, on the UI task. */
/* CHOOSING A LENS BY HAND STOPS THE ROTATION - WHEREVER THE CHOICE CAME FROM.
 *
 * home_open() paused the tower and nothing else did, so every OTHER way of
 * picking a lens was broken: choose one from the browser, or type `census` on
 * the console, and the rotation yanked the radio away within a second and
 * moved on. It looked like the device ignoring you.
 *
 * The rule belongs at the one place every path goes through, not repeated at
 * each of them - repeating it is how home_open ended up being the only one
 * that had it. The rotation marks its own switches; everything else is a
 * person, and a person who chose a lens meant it. */
static bool s_tower_switching;

static bool lens_switch(const char *id)
{
    if (!s_lens_mtx) {
        return false;
    }
    xSemaphoreTake(s_lens_mtx, portMAX_DELAY);
    const bool ok = pharos_lens_activate(id);
    xSemaphoreGive(s_lens_mtx);
    if (ok && !s_tower_switching && s_tower_on) {
        s_tower_on = false;
        s_tower_pending = NULL;
        ESP_LOGI(TAG, "watchtower: paused - %s was chosen by hand", id);
    }
    return ok;
}

static void lens_halt(void)
{
    if (!s_lens_mtx) {
        return;
    }
    xSemaphoreTake(s_lens_mtx, portMAX_DELAY);
    pharos_lens_deactivate();
    xSemaphoreGive(s_lens_mtx);
}

static void analytics_task(void *arg)
{
    (void)arg;
    s_analytics_run = true;
    while (s_analytics_run) {
        const unsigned n = pharos_ui_pump();
        /* Sleep a little longer when idle, stay hot under load. */
        vTaskDelay(pdMS_TO_TICKS(n ? 2 : 10));
    }
    vTaskDelete(NULL);
}

/* Is a lens safe to auto-launch given the fence state? A lens that holds any
 * radio capability is gated behind a clean fence. */
static bool lens_launchable(const pharos_lens_t *l)
{
    if (s_fence_ok) {
        return true;
    }
    const pharos_caps_t radio =
        PHAROS_CAP_WIFI_RX | PHAROS_CAP_WIFI_CHAN | PHAROS_CAP_BLE_SCAN;
    return (l->caps & radio) == 0;
}

/* Push the active lens' state onto the panel.
 *
 * The HUD is deliberately generic - lens name, one big value, a band word, a
 * detail line, a 0..100 gauge - so a new lens gets a screen for free. Where a
 * lens exposes a verdict snapshot we show it; otherwise we show that it is
 * running, which is still the truth and still better than a black panel. */

/* ---- the alarm latch -------------------------------------------------
 *
 * Alerts fire on a band CHANGE, not on a band. A flood that sits at FLOOD
 * LIKELY for four minutes is one event; a device that shrieks continuously
 * gets muted, and a muted alarm is worse than no alarm because it is still
 * trusted. Rising into a band is worth a sound - staying in it is worth the
 * screen.
 *
 * Only RISING edges alert, plus one falling note when the reading returns all
 * the way to quiet, which is the other thing an operator actually wants to
 * know without looking. Tracked per lens id so that switching lenses does not
 * fire an alert for a band the new lens was already sitting in. */
static char s_alarm_lens[32];
static uint8_t s_alarm_band;

static void alarm_pump(const pharos_lens_t *active,
                       const struct pharos_lens_display *d)
{
    if (!active || !d || !d->has_score) {
        return;
    }
    /* The band the score falls in, using the same thresholds the face colours
     * by, so what is heard and what is seen can never disagree. */
    uint8_t band;
    if (d->score >= 75)      band = 4;
    else if (d->score >= 60) band = 3;
    else if (d->score >= 40) band = 2;
    else if (d->score >= 20) band = 1;
    else                     band = 0;

    if (strcmp(s_alarm_lens, active->id) != 0) {
        /* New lens: adopt its current band silently. */
        strncpy(s_alarm_lens, active->id, sizeof(s_alarm_lens) - 1);
        s_alarm_lens[sizeof(s_alarm_lens) - 1] = '\0';
        s_alarm_band = band;
        return;
    }
    if (band > s_alarm_band) {
        pharos_audio_alert(pharos_audio_alert_for_band(band));
    } else if (band == 0 && s_alarm_band >= 2) {
        pharos_audio_alert(PHAROS_ALERT_CLEAR);
    }
    s_alarm_band = band;
}

/* Pull one page of the active lens' own rows and put them on the glass.
 *
 * The lens fills rows by absolute index and the HUD does the slicing, so a
 * lens never has to know how tall the screen is. The row count is discovered
 * by asking one past the end - lists here are tens of entries, not thousands,
 * and a lens that would rather not be asked simply leaves ->row NULL. */
/* The lens' name in capitals, in a static buffer - the HUD copies it. */
static const char *lens_caps(const pharos_lens_t *active)
{
    static char name[16];
    unsigned k = 0;
    if (active) {
        for (; active->name[k] && k < sizeof(name) - 1; k++) {
            const char c = active->name[k];
            name[k] = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
        }
    }
    name[k] = '\0';
    return name;
}

/* Test seam: a row touch, as if a finger had landed on it.
 *
 * The detail page is the hardest part of this device to verify - it is six
 * lines of small text on a round screen and a photograph cannot show which
 * row the next press would act on. Driving it from the console makes the whole
 * interaction testable over USB. */
void pharos_ui_tap_row(unsigned row_on_page)
{
    if (row_on_page < PHAROS_HUD_ROWS) {
        s_row_pending = (int)row_on_page;
    }
}

/* ---- the survey hooks; see pharos_survey_hook.h ---- */

/* One accelerometer sample per UI tick - about 20 Hz, which is a dozen samples
 * per footfall at ordinary walking cadence. Cheap: one 6-byte I2C read. */
static void motion_pump(void)
{
    if (!s_motion.present) {
        return;
    }
    int32_t x = 0, y = 0, z = 0;
    if (pharos_bsp_imu_read(&x, &y, &z)) {
        pm_observe(&s_motion, x, y, z, (uint64_t)esp_timer_get_time());
    }
}

bool pharos_ui_motion(uint8_t *state, uint32_t *steps, uint32_t *still_for_s)
{
    pm_verdict_t v;
    pm_evaluate(&s_motion, (uint64_t)esp_timer_get_time(), &v);
    if (state)       *state = (uint8_t)v.state;
    if (steps)       *steps = v.steps;
    if (still_for_s) *still_for_s = v.still_for_s;
    return v.present;
}

bool pharos_ui_has_travelled(uint32_t since_steps)
{
    return pm_has_travelled(&s_motion, since_steps);
}

void pharos_survey_network(const uint8_t bssid[6], uint8_t grade, uint32_t flags)
{
    psv_note_network(&s_survey, bssid, grade, flags,
                     (uint64_t)esp_timer_get_time());
}

void pharos_survey_device(const uint8_t mac[6], uint8_t names, bool randomised)
{
    psv_note_device(&s_survey, mac, names, randomised,
                    (uint64_t)esp_timer_get_time());
}

void pharos_survey_tool(uint8_t kind, bool present)
{
    psv_note_tool(&s_survey, kind, present, (uint64_t)esp_timer_get_time());
}

bool pharos_survey_read(struct psv_report *out)
{
    if (!out) {
        return false;
    }
    psv_summarise(&s_survey, (uint64_t)esp_timer_get_time(), (psv_report_t *)out);
    return true;
}

/* ---- customising the ring; see pharos_ui.h ---- */

/* REMEMBERED ACROSS BOOTS.
 *
 * A ring somebody spent a minute tuning and then lost to a power cycle is a
 * setting nobody tunes twice. Stored as a bitmap of armed watches plus a byte
 * of period per watch, keyed by POSITION in the ring order - which is a
 * deliberate trade: reordering the ring in a future firmware resets everyone's
 * choice, and that is better than a stale id-keyed map silently arming the
 * wrong watches. The version byte makes that reset explicit. */
/* Bumped when the ring's SHAPE changes.
 *
 * The saved choice is keyed by position, so adding a watch shifts every
 * position after it. A version bump discards the old map rather than applying
 * it to the wrong watches - which is a deliberate, visible reset of somebody's
 * preferences, and far better than silently disarming the two newest sensors,
 * which is exactly what happened when Roster and Footprint were added. */
#define RING_NVS_VERSION 2u

static void ring_save(void)
{
    nvs_handle_t h;
    if (nvs_open("pharos", NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    uint16_t armed = 0;
    uint8_t period[PTW_MAX_WATCHES];
    memset(period, 1, sizeof(period));
    for (unsigned i = 0; i < s_tower.n && i < 16u; i++) {
        if (s_tower.w[i].armed) {
            armed |= (uint16_t)(1u << i);
        }
        period[i] = s_tower.w[i].period ? s_tower.w[i].period : 1u;
    }
    nvs_set_u8(h, "ring_ver", RING_NVS_VERSION);
    nvs_set_u8(h, "ring_n", (uint8_t)s_tower.n);
    nvs_set_u16(h, "ring_on", armed);
    nvs_set_blob(h, "ring_per", period, sizeof(period));
    nvs_commit(h);
    nvs_close(h);
}

static void ring_load(void)
{
    nvs_handle_t h;
    if (nvs_open("pharos", NVS_READONLY, &h) != ESP_OK) {
        return;
    }
    uint8_t ver = 0, n = 0;
    uint16_t armed = 0;
    const esp_err_t vr = nvs_get_u8(h, "ring_ver", &ver);
    ESP_LOGI(TAG, "ring: stored version %u (want %u), %s", (unsigned)ver,
             (unsigned)RING_NVS_VERSION,
             (vr == ESP_OK) ? "present" : "absent");
    if (vr != ESP_OK || ver != RING_NVS_VERSION ||
        nvs_get_u8(h, "ring_n", &n) != ESP_OK || n != (uint8_t)s_tower.n ||
        nvs_get_u16(h, "ring_on", &armed) != ESP_OK) {
        /* A ring of a different shape than the one saved: the positions no
         * longer mean the same watches, so the saved choice is discarded
         * rather than applied to the wrong ones. */
        nvs_close(h);
        return;
    }
    uint8_t period[PTW_MAX_WATCHES];
    size_t len = sizeof(period);
    if (nvs_get_blob(h, "ring_per", period, &len) == ESP_OK &&
        len == sizeof(period)) {
        for (unsigned i = 0; i < s_tower.n; i++) {
            ptw_set_period(&s_tower, i, period[i]);
        }
    }
    for (unsigned i = 0; i < s_tower.n && i < 16u; i++) {
        /* set_armed refuses to empty the ring, so a corrupt all-zero bitmap
         * cannot leave the device watching nothing. */
        ptw_set_armed(&s_tower, i, (armed & (1u << i)) != 0u);
    }
    nvs_close(h);
    ESP_LOGI(TAG, "watchtower: restored ring (%ums a lap)",
             (unsigned)ptw_lap_ms(&s_tower));
}

unsigned pharos_ui_ring_count(void) { return s_tower.n; }

bool pharos_ui_ring_at(unsigned i, const char **name, bool *armed,
                       uint8_t *period, uint8_t *state)
{
    if (i >= s_tower.n) {
        return false;
    }
    if (name)   *name = s_tower.w[i].name;
    if (armed)  *armed = s_tower.w[i].armed;
    if (period) *period = s_tower.w[i].period ? s_tower.w[i].period : 1u;
    if (state)  *state = (uint8_t)s_tower.w[i].state;
    return true;
}

bool pharos_ui_ring_toggle(unsigned i)
{
    if (i >= s_tower.n) {
        return false;
    }
    if (!ptw_set_armed(&s_tower, i, !s_tower.w[i].armed)) {
        return false; /* refused - the last watch cannot be switched off */
    }
    ring_save();
    ESP_LOGI(TAG, "watchtower: %s %s (%ums a lap)", s_tower.w[i].name,
             s_tower.w[i].armed ? "on" : "off",
             (unsigned)ptw_lap_ms(&s_tower));
    return true;
}

bool pharos_ui_ring_cycle_period(unsigned i)
{
    uint8_t p = 0;
    if (!pharos_ui_ring_at(i, NULL, NULL, &p, NULL)) {
        return false;
    }
    p = (uint8_t)((p % PTW_MAX_PERIOD) + 1u);
    if (!ptw_set_period(&s_tower, i, p)) {
        return false;
    }
    ring_save();
    return true;
}

uint32_t pharos_ui_ring_lap_ms(void) { return ptw_lap_ms(&s_tower); }
bool pharos_ui_ring_running(void) { return s_tower_on; }

void pharos_ui_ring_set_running(bool on)
{
    s_tower_on = on;
    if (!on) {
        lens_halt();
    }
    ESP_LOGI(TAG, "watchtower %s", on ? "running" : "paused");
}

void pharos_ui_ring_reset(void)
{
    nvs_handle_t h;
    if (nvs_open("pharos", NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, "ring_ver");
        nvs_erase_key(h, "ring_n");
        nvs_erase_key(h, "ring_on");
        nvs_erase_key(h, "ring_per");
        nvs_commit(h);
        nvs_close(h);
    }
    tower_arm_all();
    ESP_LOGI(TAG, "ring: reset to defaults");
}

void pharos_ui_tower_dump(char *buf, size_t cap)
{
    if (!buf || !cap) {
        return;
    }
    const uint64_t now = (uint64_t)esp_timer_get_time();
    ptw_summary_t sum;
    ptw_summarise(&s_tower, now, &sum);
    static const char *fresh[] = { "fresh", "ageing", "expired" };
    size_t k = 0;
    k += (size_t)snprintf(buf + k, (k < cap) ? cap - k : 0,
                          "watchtower: %s, %u armed, dwell %ums, %u rotations\n"
                          "  headline: %s   (reporting %u, quiet %u, unknown %u,"
                          " alarms %u)\n",
                          s_tower_on ? "running" : "PAUSED", sum.armed,
                          (unsigned)s_tower.dwell_ms, (unsigned)s_tower.rotations,
                          sum.headline, sum.reporting, sum.quiet, sum.unknown,
                          sum.alarms);
    for (unsigned i = 0; i < s_tower.n; i++) {
        const ptw_watch_t *w = &s_tower.w[i];
        const ptw_freshness_t f = ptw_freshness(&s_tower, i, now);
        const uint32_t age_s =
            w->seen_us ? (uint32_t)((now - w->seen_us) / 1000000ull) : 0u;
        /* A disarmed watch reads "off" rather than "not yet". They looked
         * identical in this dump - both never-run, both expired, both zero
         * visits - and telling a watch that is switched off from one that has
         * simply not had its turn yet is the first thing anybody asks of it. */
        if (!w->armed) {
            k += (size_t)snprintf(buf + k, (k < cap) ? cap - k : 0,
                                  "   %-11s off\n", w->name);
            continue;
        }
        /* And what it CAUGHT, which a live reading cannot show - see
         * ptw_watch_t::peak_state. */
        char latched[40];
        latched[0] = '\0';
        if (w->peak_state >= PTW_ELEVATED) {
            const uint32_t pa = (now > w->peak_us)
                                    ? (uint32_t)((now - w->peak_us) / 1000000ull)
                                    : 0u;
            snprintf(latched, sizeof(latched), "  [caught %s %us ago]",
                     ptw_state_name(w->peak_state), (unsigned)pa);
        }
        k += (size_t)snprintf(buf + k, (k < cap) ? cap - k : 0,
                              "%s %-11s %-9s %3u/%-3u  %-7s %us ago  visits=%u%s\n",
                              (i == s_tower.cursor) ? " >" : "  ", w->name,
                              ptw_state_name(w->state), w->score, w->ceiling,
                              fresh[f], (unsigned)age_s, (unsigned)w->visits,
                              latched);
    }
    if (k < cap) {
        buf[k] = '\0';
    } else if (cap) {
        buf[cap - 1] = '\0';
    }
}

int pharos_ui_detail_cursor(int *opened)
{
    if (opened) {
        *opened = (s_view == VIEW_OPENED) ? (int)s_opened_row : -1;
    }
    if (s_view != VIEW_DETAIL && s_view != VIEW_OPENED) {
        return -1;
    }
    return (int)s_detail_cursor;
}

static void paint_detail(const pharos_lens_t *active)
{
    struct pharos_lens_row rows[PHAROS_HUD_ROWS];
    unsigned total = 0;

    /* THE OPENED ROW. A grade with no way to ask "why" is a claim rather than
     * a finding, so a lens that can say more about one of its rows gets a page
     * to say it on. */
    /* THE OPENED ROW MAY HAVE GONE.
     *
     * These lists are live: a Flipper is switched off, a network stops
     * beaconing, and the row somebody opened thirty seconds ago is no longer
     * anything. Staying on an empty expansion would present a page about
     * nothing; dropping back to the list shows what is actually there. */
    if (s_view == VIEW_OPENED && active && active->row_expand) {
        struct pharos_lens_row gone;
        memset(&gone, 0, sizeof(gone));
        if (!active->row_expand(s_opened_row, 0, &gone)) {
            s_view = VIEW_DETAIL;
            s_detail_page = s_opened_row / PHAROS_HUD_ROWS;
        }
    }

    const bool opened = (s_view == VIEW_OPENED);
    if (opened && active && active->row_expand) {
        struct pharos_lens_row probe;
        while (total < 240u) {
            memset(&probe, 0, sizeof(probe));
            if (!active->row_expand(s_opened_row, total, &probe)) {
                break;
            }
            total++;
        }
        const unsigned pages =
            total ? ((total + PHAROS_HUD_ROWS - 1u) / PHAROS_HUD_ROWS) : 1u;
        if (s_detail_page >= pages) {
            s_detail_page = pages - 1u;
        }
        unsigned n = 0;
        const unsigned base = s_detail_page * PHAROS_HUD_ROWS;
        for (; n < PHAROS_HUD_ROWS; n++) {
            memset(&rows[n], 0, sizeof(rows[n]));
            if (!active->row_expand(s_opened_row, base + n, &rows[n])) {
                break;
            }
        }
        /* Head the page with the row you opened, so a page of numbers is
         * never orphaned from the thing it describes. */
        memset(&probe, 0, sizeof(probe));
        const bool named = active->row && active->row(s_opened_row, &probe);
        pharos_hud_detail(lens_caps(active), named ? probe.left : "DETAIL",
                          "\xEF\x81\x93", rows, n, s_detail_page, pages, -1,
                          false);
        return;
    }

    /* The list. The lens fills rows by absolute index and the HUD does the
     * slicing, so a lens never has to know how tall the screen is. */
    if (active && active->row) {
        struct pharos_lens_row probe;
        while (total < 240u) {
            memset(&probe, 0, sizeof(probe));
            if (!active->row(total, &probe)) {
                break;
            }
            total++;
        }
    }

    const unsigned pages = total ? ((total + PHAROS_HUD_ROWS - 1u) / PHAROS_HUD_ROWS) : 1u;
    /* The page is now driven by the page controls, not by a cursor walking off
     * the bottom - so it is the page that gets clamped, and the cursor that
     * follows it. A list that shrinks under you (devices going stale is the
     * normal case here) must not leave the view past the end. */
    if (s_detail_page >= pages) {
        s_detail_page = pages - 1u;
    }
    if (total && s_detail_cursor >= total) {
        s_detail_cursor = total - 1u;
    }

    unsigned n = 0;
    if (active && active->row) {
        const unsigned base = s_detail_page * PHAROS_HUD_ROWS;
        for (; n < PHAROS_HUD_ROWS; n++) {
            memset(&rows[n], 0, sizeof(rows[n]));
            if (!active->row(base + n, &rows[n])) {
                break;
            }
        }
    }

    const int focus = total ? (int)(s_detail_cursor % PHAROS_HUD_ROWS) : -1;
    pharos_hud_detail(lens_caps(active), active ? active->row_head_left : NULL,
                      active ? active->row_head_right : NULL, rows, n,
                      s_detail_page, pages, focus,
                      active && (active->row_expand || active->row_edit));
}

/* Apply a theme change wherever the paint loop next happens to be.
 *
 * The lens that changes the theme runs on this task but must not reach into
 * the HUD - a settings screen knowing how to rebuild the face is the kind of
 * coupling that means the NEXT settings screen has to know it too. So the
 * lens moves a number, and the one place that owns the face notices. */
static void theme_sync(void)
{
    static unsigned seen = (unsigned)-1;
    const unsigned now = pharos_theme_index();
    if (seen == now) {
        return;
    }
    const bool first = (seen == (unsigned)-1);
    seen = now;
    if (!first) {
        /* The browse card is painted on view entry rather than per frame, and
         * a theme can only be changed from a lens' detail page, so by the time
         * anyone gets back to BROWSE it has been repainted anyway. */
        pharos_hud_rebuild();
        /* And nothing the teardown raised is a real intent. Tearing the face
         * down and building it again is not a thing a finger did. */
        s_nav_pending = -1;
        s_row_pending = -1;
    }
}

/* Hand the radio round the ring.
 *
 * Called every tick. The one rule that makes a rotation safe is in the engine
 * (see ptw_turn): it does not walk away from a watch that is hearing
 * something, because the moment evidence is arriving is the moment the
 * confidence ceiling most needs the airtime. This decides what "hearing
 * something" means and does the actual lens switch.
 *
 * The switch itself is deferred to the top of the loop rather than done here,
 * for the same reason nav intents are: activating a lens restarts the radio,
 * and doing that from inside a paint is how the device used to reboot. */
static void tower_rotate(void)
{
    if (!s_tower_on || !s_tower.n) {
        return;
    }
    const uint64_t now = (uint64_t)esp_timer_get_time();
    const pharos_lens_t *live = pharos_lens_active();

    /* Record what the watch holding the radio is currently saying, so its dot
     * carries a real reading rather than the last one it happened to leave. */
    bool hold = false;
    if (live) {
        struct pharos_lens_display d;
        memset(&d, 0, sizeof(d));
        if (live->display && live->display(&d)) {
            const ptw_state_t st = tower_state_of(&d);
            ptw_report(&s_tower, live->id, st, d.score, d.ceiling, now);
            /* ONLY AN ALARM KEEPS THE RADIO.
             *
             * "Above background" was the rule, and it was too loose: the
             * microphone watch sits at ELEVATED in any room with something at
             * 19 kHz, so it held its slice every single lap and stretched the
             * whole rotation. ELEVATED is often a standing fact about a place
             * rather than an event in progress - and a standing fact will
             * still be there next lap. An ALARM might not be. */
            hold = (st >= PTW_ALARM);
        }
    }

    bool changed = false;
    const int who = ptw_turn(&s_tower, now, hold, &changed);
    if (who < 0 || !changed) {
        return;
    }
    if (live && strcmp(live->id, s_tower.w[who].id) == 0) {
        return;
    }
    s_tower_pending = s_tower.w[who].id;
}

static void paint(const pharos_lens_t *active)
{
    if (s_view == VIEW_GUIDE) {
        paint_guide();
        return;
    }
    if (s_view == VIEW_HOME) {
        paint_home();
        return;
    }
    if (s_view == VIEW_BROWSE) {
        return; /* the browse card is painted when the cursor moves */
    }
    if (s_view == VIEW_DETAIL || s_view == VIEW_OPENED) {
        if (!pharos_bsp_display_lock(30)) {
            s_paint_misses++;
            return;
        }
        s_paints++;
        pharos_hud_create();
        theme_sync();
        paint_detail(active);
        pharos_bsp_display_unlock();
        return;
    }
    if (!pharos_bsp_display_lock(30)) {
        /* Counted, not ignored: a paint that never lands is exactly what a
         * black screen looks like from in here, and the heartbeat below
         * reports it so a boot log alone is enough to diagnose. */
        s_paint_misses++;
        return;
    }
    s_paints++;

    /* Idempotent: builds the widgets on the first frame under a good lock, so
     * the HUD exists even if the one-shot splash lock happened to time out.
     * Without this, a single missed lock at boot would leave the panel blank
     * for the whole session. */
    pharos_hud_create();
    theme_sync();

    if (!active) {
        struct pharos_lens_display idle;
        memset(&idle, 0, sizeof(idle));
        snprintf(idle.big, sizeof(idle.big), "--");
        snprintf(idle.band, sizeof(idle.band), "%s",
                 s_fence_ok ? "idle" : "FENCE UNVERIFIED");
        snprintf(idle.advice, sizeof(idle.advice), "%s",
                 s_fence_ok ? "no lens running" : "radio locked");
        pharos_hud_live("PHAROS", &idle, s_fence_ok ? 0x7FA6B5 : 0xE8503F);
        pharos_bsp_display_unlock();
        return;
    }

    /* Uppercase short name for the header. */
    char name[16];
    unsigned n = 0;
    for (; active->name[n] && n < sizeof(name) - 1; n++) {
        const char c = active->name[n];
        name[n] = (c >= 'a' && c <= 'z') ? (char)(c - 'a' + 'A') : c;
    }
    name[n] = '\0';

    /* Ask the lens what it actually found.
     *
     * The screen used to show a running FRAME COUNTER and the logarithm of it
     * as a "score", identically for every lens. It climbed forever and meant
     * nothing - which is precisely how it was reported from the field. The
     * engines were computing real verdicts all along and this function was
     * ignoring them.
     *
     * ONE call does the whole face now. It used to be three - live(), then
     * ceiling(), then advice() - and the second and third disagreed with the
     * first about whether the summary label should be visible, so it was
     * hidden and shown again on every repaint. That was half the flicker. */
    struct pharos_lens_display d;
    memset(&d, 0, sizeof(d));

    if (active->display && active->display(&d)) {
        alarm_pump(active, &d);
        pharos_hud_live(name, &d, 0);
    } else {
        /* No verdict yet. Say so plainly and show what IS true - how much has
         * been heard - rather than dressing a counter up as a measurement. */
        pharos_radio_stats_t st;
        pharos_radio_stats(&st);
        snprintf(d.detail, sizeof(d.detail), "ch %u  %s",
                 (unsigned)st.current_channel, st.camped ? "camped" : "hopping");
        snprintf(d.big, sizeof(d.big), "%u",
                 (unsigned)(st.frames_seen > 99999 ? 99999 : st.frames_seen));
        snprintf(d.band, sizeof(d.band), "frames heard");
        snprintf(d.advice, sizeof(d.advice), "listening - no verdict yet");
        pharos_hud_live(name, &d, 0x7FA6B5);
    }

    pharos_bsp_display_unlock();
}

/* Order the dial: observe lenses first (the working tools), then train, then
 * system. Within a kind, registry order. This is presentation only; the
 * registry itself is untouched. */
static void build_dial(pd_dial_t *dial, const pharos_lens_t **order, unsigned *count)
{
    unsigned n = 0;
    const pharos_lens_kind_t kinds[] = {
        PHAROS_LENS_OBSERVE, PHAROS_LENS_ANALYSE, PHAROS_LENS_TRAIN, PHAROS_LENS_SYSTEM
    };
    for (unsigned k = 0; k < 4; k++) {
        for (unsigned i = 0; i < pharos_lens_count(); i++) {
            const pharos_lens_t *l = pharos_lens_at(i);
            if (l->kind == kinds[k]) {
                order[n++] = l;
            }
        }
    }
    *count = n;
    pd_dial_layout(n, 0.0f, PR_RING_R, PR_SAFE_R, dial);
    if (!dial->hittable) {
        /* Too many lenses for one ring of thumb-sized wedges. M2 pages the
         * dial; for now log it so it is never a silent usability failure. */
        ESP_LOGW(TAG, "%u lenses exceed one dial page (%u hittable); M2 will page",
                 n, dial->max_hittable);
    }
}

void pharos_ui_run(const pharos_bsp_status_t *bsp, bool fence_ok)
{
    (void)bsp;
    s_fence_ok = fence_ok;

    pa_reset(&s_aegis);
    if (!s_aegis_lock) {
        s_aegis_lock = xSemaphoreCreateMutex();
    }
    if (!s_lens_mtx) {
        s_lens_mtx = xSemaphoreCreateMutex();
    }

    pd_dial_t dial;
    unsigned count = 0;
    build_dial(&dial, s_order, &count);
    s_order_n = count;

    /* Touch is the primary control; the BOOT button is the fallback. */
    pharos_hud_set_nav_cb(on_nav);
    pharos_hud_set_row_cb(hud_row_cb);
    pharos_hud_set_home_cb(hud_home_cb);
    boot_button_init();

    ESP_LOGI(TAG, "Lamp Room: %u lenses on the dial%s", count,
             s_fence_ok ? "" : " (radio lenses locked: fence not clean)");

    xTaskCreatePinnedToCore(analytics_task, "pharos_rx", 4096, NULL, 6, NULL, 1);

    /* Default landing lens: Spectrum if the fence is clean (you look before
     * you judge), otherwise the System panel so the operator sees why radio
     * is locked. */
    /* Land on the WATCHTOWER: every armed watch on one ring, taking turns.
     *
     * The old landing was BROWSE - one lens card at a time - which meant the
     * device could only tell you about whichever lens you had happened to
     * open. That is the thing the ring exists to fix: nobody should have to be
     * sitting inside the right lens at the moment an attack happens. */
    psv_reset(&s_survey, (uint64_t)esp_timer_get_time());
    pm_reset(&s_motion);
    pm_set_present(&s_motion, pharos_bsp_imu_present());
    ESP_LOGI(TAG, "motion sensing %s",
             s_motion.present ? "live" : "unavailable (no IMU answered)");
    tower_arm_all();
    ring_load();
    s_tower_on = s_fence_ok && s_tower.n > 0;
    s_view = VIEW_HOME;
    s_cursor = 0;
    s_home_sel = 0;

    /* Put the identity on the panel immediately, so the operator sees the
     * device is alive long before a lens has anything to say. */
    if (pharos_bsp_display_lock(200)) {
        pharos_hud_splash("v3.2.0", s_fence_ok);
        pharos_bsp_display_unlock();
    }
    vTaskDelay(pdMS_TO_TICKS(1500)); /* let the identity be read */

    /* FIRST BOOT: teach the device before handing it over.
     *
     * Only once, and only when the fence is clean - a device that could not
     * prove it is receive-only has something more urgent to say than a
     * tutorial. Replay it any time with the `guide` command. */
    if (s_fence_ok && !guide_seen()) {
        ESP_LOGI(TAG, "first boot: showing the guide");
        pharos_ui_guide_start();
        paint_guide();
    } else {
        paint_home();
    }

    uint64_t last_us = (uint64_t)esp_timer_get_time();
    uint32_t heartbeat = 0;
    uint32_t since_paint = 0;
    for (;;) {
        const uint64_t now = (uint64_t)esp_timer_get_time();
        const uint32_t dt_ms = (uint32_t)((now - last_us) / 1000);
        last_us = now;

        const pharos_lens_t *active = pharos_lens_active();
        if (active && active->on_tick && lens_launchable(active)) {
            active->on_tick(dt_ms);
        }

        /* Fold whatever the active lens is seeing into the correlator. */
        aegis_pump(active, dt_ms);

        motion_pump();
        boot_button_poll(dt_ms);
        nav_apply();
        row_apply();
        home_apply();
        request_apply();

        /* Hand the radio round the ring, and make the switch the rotation
         * asked for. Done here rather than inside tower_rotate() for the same
         * reason nav intents are deferred: activating a lens restarts the
         * radio, and that is not something to do from inside a paint. */
        tower_rotate();
        if (s_tower_pending) {
            const char *id = s_tower_pending;
            s_tower_pending = NULL;
            s_tower_switching = true;
            if (!lens_switch(id)) {
                ESP_LOGW(TAG, "watchtower: %s would not start", id);
            }
            s_tower_switching = false;
        }

        /* Repaint at ~5 Hz. LVGL runs on the BSP's own task, so all we do here
         * is push fresh text/values in under its lock; a short timeout means a
         * busy display never stalls the analytics tick. */
        /* REPAINT RATE, AND WHY IT COULD GO UP.
         *
         * Five a second is what a stepping gauge looks like: the arc jumps in
         * visible increments and the ring's dots change state between frames
         * rather than during them. It was set low because the ORIGINAL HUD
         * pushed every value into every widget on every frame and repainting
         * faster meant flickering faster.
         *
         * That is no longer true. Every write goes through a dirty check
         * against what the widget already holds, so a reading that has not
         * moved invalidates nothing at all and a faster loop costs only the
         * comparisons.
         *
         * But it is still not free, and pushing it to fifteen was measurably
         * too far: the loop's own period went from 50 ms to 93 ms, which
         * starves the analytics tick - the thing that actually detects
         * attacks - to make a gauge look nicer. That is the wrong trade in
         * this device of all devices.
         *
         * Ten a second, and SMOOTHNESS COMES FROM SOMEWHERE ELSE: the arcs are
         * animated by LVGL, which interpolates between the values this loop
         * hands it at its own 60-odd Hz refresh. The reading updates ten times
         * a second and the needle glides, which is both nicer to look at and
         * cheaper than asking this loop to do it. */
        since_paint += dt_ms;
        if (since_paint >= 100) {
            since_paint = 0;
            paint(active);
        }

        if ((++heartbeat % 100) == 0 && active) {
            ESP_LOGI(TAG, "active: %s (dt=%ums) painted=%u missed=%u hud=%d",
                     active->id, dt_ms, (unsigned)s_paints,
                     (unsigned)s_paint_misses, (int)pharos_hud_present());
        }
        vTaskDelay(pdMS_TO_TICKS(50)); /* ~20 Hz */
    }
}
