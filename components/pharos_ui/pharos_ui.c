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
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"

#include "freertos/semphr.h"

#include "pharos_aegis.h"
#include "pharos_bus.h"
#include "pharos_dial.h"
#include "pharos_hud.h"
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
typedef enum { VIEW_BROWSE = 0, VIEW_LIVE } view_t;

static const pharos_lens_t *s_order[PHAROS_MAX_LENSES];
static unsigned s_order_n;
static unsigned s_cursor;
static view_t s_view = VIEW_BROWSE;
static volatile int s_nav_pending = -1; /* pharos_nav_t, or -1 for none */

static void on_nav(pharos_nav_t what)
{
    s_nav_pending = (int)what; /* record only - see the note above */
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
        s_view = VIEW_LIVE;
        for (unsigned i = 0; i < s_order_n; i++) {
            if (s_order[i] && strcmp(s_order[i]->id, id) == 0) {
                s_cursor = i;
                break;
            }
        }
        ESP_LOGI(TAG, "console started %s", id);
    } else {
        ESP_LOGW(TAG, "console could not start %s", id);
    }
}

static void nav_apply(void)
{
    const int want = s_nav_pending;
    if (want < 0) {
        return;
    }
    s_nav_pending = -1;
    if (!s_order_n) {
        return;
    }

    switch ((pharos_nav_t)want) {
    case PHAROS_NAV_NEXT:
        s_cursor = (s_cursor + 1u) % s_order_n;
        break;
    case PHAROS_NAV_PREV:
        s_cursor = (s_cursor + s_order_n - 1u) % s_order_n;
        break;
    case PHAROS_NAV_SELECT: {
        if (s_view == VIEW_LIVE) {
            return; /* already running; the centre does nothing here */
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
        lens_halt();
        s_view = VIEW_BROWSE;
        ESP_LOGI(TAG, "stopped; back to browse");
        paint_browse();
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
static bool lens_switch(const char *id)
{
    if (!s_lens_mtx) {
        return false;
    }
    xSemaphoreTake(s_lens_mtx, portMAX_DELAY);
    const bool ok = pharos_lens_activate(id);
    xSemaphoreGive(s_lens_mtx);
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
static uint32_t s_paints, s_paint_misses;

static void paint(const pharos_lens_t *active)
{
    if (s_view == VIEW_BROWSE) {
        return; /* the browse card is painted when the cursor moves */
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

    if (!active) {
        pharos_hud_update("PHAROS", "--", s_fence_ok ? "idle" : "FENCE UNVERIFIED",
                          s_fence_ok ? "no lens running" : "radio locked",
                          0, s_fence_ok ? 0x7FA6B5 : 0xE8503F);
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
     * ignoring them. */
    struct pharos_lens_display d;
    memset(&d, 0, sizeof(d));

    if (active->display && active->display(&d)) {
        pharos_hud_ceiling(d.has_score ? d.ceiling : 0);
        pharos_hud_live(name, d.big, d.band, d.detail,
                        d.has_score ? d.score : 0, d.has_score ? 0 : 0x7FA6B5);
        pharos_hud_advice(d.advice);
    } else {
        /* No verdict yet. Say so plainly and show what IS true - how much has
         * been heard - rather than dressing a counter up as a measurement. */
        pharos_radio_stats_t st;
        pharos_radio_stats(&st);
        char detail[48];
        snprintf(detail, sizeof(detail), "ch %u  %s",
                 (unsigned)st.current_channel, st.camped ? "camped" : "hopping");
        char frames[16];
        snprintf(frames, sizeof(frames), "%u",
                 (unsigned)(st.frames_seen > 99999 ? 99999 : st.frames_seen));
        pharos_hud_ceiling(0);
        pharos_hud_live(name, frames, "frames heard", detail, 0, 0x7FA6B5);
        pharos_hud_advice("listening - no verdict yet");
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
    boot_button_init();

    ESP_LOGI(TAG, "Lamp Room: %u lenses on the dial%s", count,
             s_fence_ok ? "" : " (radio lenses locked: fence not clean)");

    xTaskCreatePinnedToCore(analytics_task, "pharos_rx", 4096, NULL, 6, NULL, 1);

    /* Default landing lens: Spectrum if the fence is clean (you look before
     * you judge), otherwise the System panel so the operator sees why radio
     * is locked. */
    /* Land in BROWSE rather than straight into a lens: the first thing the
     * operator should see is what this tool does, not a number with no
     * explanation attached. */
    s_view = VIEW_BROWSE;
    s_cursor = 0;

    /* Put the identity on the panel immediately, so the operator sees the
     * device is alive long before a lens has anything to say. */
    if (pharos_bsp_display_lock(200)) {
        pharos_hud_splash("v1.12.0", s_fence_ok);
        pharos_bsp_display_unlock();
    }
    vTaskDelay(pdMS_TO_TICKS(1500)); /* let the identity be read */
    paint_browse();

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

        boot_button_poll(dt_ms);
        nav_apply();
        request_apply();

        /* Repaint at ~5 Hz. LVGL runs on the BSP's own task, so all we do here
         * is push fresh text/values in under its lock; a short timeout means a
         * busy display never stalls the analytics tick. */
        since_paint += dt_ms;
        if (since_paint >= 200) {
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
