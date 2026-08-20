/* Pharos lens: System - the device's honesty panel
 *
 * Settings, battery, storage, region - and, first and loudest, the transmit
 * fence's own status. A defensive tool that asks to be trusted in a building
 * somebody else owns should be able to show, on its own screen, that it cannot
 * transmit: the wrap fence is linked, NimBLE is observer-only, no transmit
 * symbol is in the image, and the fence's attempt counter is zero.
 *
 * If any of that were untrue, this panel would say so in red. That is the
 * point of having the firmware report its own posture rather than a README
 * claiming it.
 */
#include <string.h>

#include "esp_app_desc.h"
#include "esp_log.h"

#include "esp_heap_caps.h"
#include "esp_timer.h"

#include "pharos_audio.h"
#include "pharos_bsp.h"
#include "pharos_lens.h"
#include "pharos_power.h"
#include "pharos_radio.h"
#include "pharos_region.h"
#include "pharos_report.h"
#include "pharos_theme.h"
#include "pharos_ui.h"

static const char *TAG = "lens.system";

typedef struct {
    pharos_tx_fence_t fence;
    pharos_region_t region;
    bool fence_ok;
} sys_state_t;

static sys_state_t s;

static bool system_mount(void)
{
    pharos_radio_fence_status(&s.fence);
    s.region = pharos_region_get();
    s.fence_ok = s.fence.wrap_linked && s.fence.ble_observer_only &&
                 s.fence.tx_symbols_absent && (s.fence.tx_attempts == 0);
    if (!s.fence_ok) {
        ESP_LOGE(TAG, "TRANSMIT FENCE NOT CLEAN: wrap=%d ble_obs=%d tx_absent=%d attempts=%u",
                 s.fence.wrap_linked, s.fence.ble_observer_only,
                 s.fence.tx_symbols_absent, s.fence.tx_attempts);
    } else {
        ESP_LOGI(TAG, "transmit fence clean: receive-only confirmed");
    }
    return true;
}

static void system_tick(uint32_t dt_ms)
{
    (void)dt_ms;
    /* Re-read the fence attempt counter live: if anything ever trips it while
     * the device is running, the panel goes red immediately. */
    pharos_radio_fence_status(&s.fence);
    s.fence_ok = s.fence.wrap_linked && s.fence.ble_observer_only &&
                 s.fence.tx_symbols_absent && (s.fence.tx_attempts == 0);
}

bool pharos_lens_system_fence_ok(void)
{
    return s.fence_ok;
}

void pharos_lens_system_fence(pharos_tx_fence_t *out)
{
    if (out) {
        *out = s.fence;
    }
}

/* A self-audit report, suitable for pasting into an engagement record: it
 * states what the device is and is not capable of. */
bool pharos_lens_system_report(char *buf, size_t cap)
{
    prt_t w;
    prt_init(&w, buf, cap, PRT_REDACT_NONE, 0);
    prt_obj_begin(&w, NULL);
    prt_str(&w, "tool", "pharos");
    prt_str(&w, "posture", "receive-only");

    const esp_app_desc_t *app = esp_app_get_description();
    prt_str(&w, "version", app ? app->version : "unknown");

    prt_obj_begin(&w, "transmit_fence");
    prt_bool(&w, "wrap_linked", s.fence.wrap_linked);
    prt_bool(&w, "ble_observer_only", s.fence.ble_observer_only);
    prt_bool(&w, "tx_symbols_absent", s.fence.tx_symbols_absent);
    prt_u32(&w, "tx_attempts", s.fence.tx_attempts);
    prt_bool(&w, "clean", s.fence_ok);
    prt_obj_end(&w);

    prt_str(&w, "region", pharos_region_name(pharos_region_get()));
    prt_bool(&w, "band_5ghz_capable", false);
    prt_str(&w, "note",
            "This device can receive on 2.4 GHz only and cannot transmit. "
            "Absence of a finding is not proof of absence.");
    prt_obj_end(&w);
    return prt_finish(&w);
}

/* ---- the settings panel ----------------------------------------------
 *
 * This screen promised "Battery, region, and proof the transmit fence is
 * clean" and then implemented neither a display nor a detail page, so the
 * generic face took over and showed a FRAME COUNTER - on a lens that holds no
 * radio and can never see a frame. It was the emptiest screen on the device
 * while claiming to be the honesty panel.
 *
 * Now it leads with the one thing this device asks to be trusted about: it
 * cannot transmit. If that were ever untrue the headline turns red and says
 * so, which is the whole reason for having the firmware report its own posture
 * rather than a README claiming it.
 *
 * Everything below the headline is the state an operator actually reaches for
 * - charge, memory, uptime, region, alarm - and the settings are OPERABLE from
 * the glass rather than being a read-only recital. The centre tap cycles the
 * one setting most likely to be wanted in the field, which is the alarm:
 * somebody in a meeting needs to silence this without finding a USB cable. */

static uint8_t s_setting; /* which setting the centre tap will act on */

static bool k_system_display(struct pharos_lens_display *o)
{
    /* The headline IS the fence. Not the battery, not the uptime - the claim
     * the whole product rests on. */
    if (s.fence_ok) {
        snprintf(o->big, sizeof(o->big), "RX");
        snprintf(o->band, sizeof(o->band), "RECEIVE-ONLY");
    } else {
        snprintf(o->big, sizeof(o->big), "!");
        snprintf(o->band, sizeof(o->band), "FENCE UNVERIFIED");
    }

    pwr_battery_t b;
    const bool have_batt = pharos_bsp_battery(&b);
    if (have_batt && b.present) {
        snprintf(o->detail, sizeof(o->detail), "%u%%  %u.%02uV  %s",
                 (unsigned)b.soc_pct, (unsigned)(b.mv / 1000),
                 (unsigned)((b.mv % 1000) / 10),
                 b.charging ? "charging" : "on battery");
    } else {
        /* No pack, or no PMU answer. Say which - a missing battery and a
         * missing driver are different facts and only one is a fault. */
        snprintf(o->detail, sizeof(o->detail), "%s",
                 have_batt ? "no pack fitted" : "USB power");
    }

    snprintf(o->advice, sizeof(o->advice), "%s",
             s.fence_ok ? "Tap centre to change the alarm."
                        : "Fence not clean - do not deploy.");

    /* The four things that are either true or not. Pips, not prose. */
    o->families = (uint8_t)((s.fence.wrap_linked ? 1u : 0u) |
                            (s.fence.ble_observer_only ? 2u : 0u) |
                            (s.fence.tx_symbols_absent ? 4u : 0u) |
                            ((s.fence.tx_attempts == 0u) ? 8u : 0u));
    o->fam_label[0] = "WRAP";
    o->fam_label[1] = "OBSV";
    o->fam_label[2] = "NOTX";
    o->fam_label[3] = "ZERO";

    /* A posture is not a threat score, so the gauge shows CHARGE - the one
     * quantity on this screen that is genuinely a 0..100 with a meaning. */
    o->score = (have_batt && b.present) ? b.soc_pct : 0u;
    o->ceiling = 100;
    o->has_score = (have_batt && b.present);
    if (!o->has_score) {
        snprintf(o->big, sizeof(o->big), "%s", s.fence_ok ? "RX" : "!");
    }
    return true;
}

/* The centre tap: cycle the alarm through off -> quiet -> normal -> loud.
 *
 * Volume rather than a menu because this is a round screen with three touch
 * zones and no keyboard, and because "make it stop" is overwhelmingly the
 * setting somebody needs in a hurry. Everything else is on the console, where
 * there is room to be precise. */
static void system_select(void)
{
    if (!pharos_audio_present()) {
        return;
    }
    /* Start the cycle from where the alarm ACTUALLY is, not from wherever
     * this variable happened to be left. Starting at zero meant the first tap
     * on a working alarm always muted it - the opposite of what somebody
     * reaching for a settings screen expects, and it persisted to NVS so the
     * device then booted silent. */
    if (!pharos_audio_enabled()) {
        s_setting = 0;
    } else {
        const uint8_t v = pharos_audio_volume();
        s_setting = (v <= 40u) ? 1u : (v <= 80u) ? 2u : 3u;
    }
    s_setting = (uint8_t)((s_setting + 1u) % 4u);
    switch (s_setting) {
    case 0: pharos_audio_set_enabled(false); break;
    case 1: pharos_audio_set_enabled(true); pharos_audio_set_volume(35); break;
    case 2: pharos_audio_set_enabled(true); pharos_audio_set_volume(70); break;
    default: pharos_audio_set_enabled(true); pharos_audio_set_volume(100); break;
    }
    /* Sound the new level, so the setting demonstrates itself. */
    pharos_audio_alert(PHAROS_ALERT_ACK);
    ESP_LOGI(TAG, "alarm now %s at %u%%",
             pharos_audio_enabled() ? "on" : "off",
             (unsigned)pharos_audio_volume());
}

/* THE SETTINGS THAT CAN BE CHANGED, FIRST.
 *
 * The five rows at the top are the ones a person came here to move; everything
 * below them is the device reporting on itself. Putting them in that order
 * means the cursor starts on something useful, which on a screen with three
 * touch zones is the difference between a settings page and a status page. */
enum {
    ROW_THEME = 0,
    ROW_BRIGHT,
    ROW_ALARM,
    ROW_VOLUME,
    ROW_REGION,
    ROW_EDITABLE_N, /* everything from here down is read-only */
};

/* Four steps, not a slider: a round screen has no slider, and on an AMOLED the
 * difference between 100 and 90 is not worth a tap. */
static const uint8_t k_bright_steps[] = { 100, 70, 40, 20 };
#define BRIGHT_N ((unsigned)(sizeof(k_bright_steps) / sizeof(k_bright_steps[0])))

static const uint8_t k_vol_steps[] = { 25, 50, 75, 100 };
#define VOL_N ((unsigned)(sizeof(k_vol_steps) / sizeof(k_vol_steps[0])))

static bool k_system_row(unsigned index, struct pharos_lens_row *out)
{
    pwr_battery_t b;
    const bool have_batt = pharos_bsp_battery(&b);
    const esp_app_desc_t *app = esp_app_get_description();

    switch (index) {
    case ROW_THEME:
        snprintf(out->left, sizeof(out->left), "theme");
        snprintf(out->right, sizeof(out->right), "%s", pharos_theme()->name);
        out->tone = PHAROS_TONE_NEUTRAL;
        return true;
    case ROW_BRIGHT:
        snprintf(out->left, sizeof(out->left), "brightness");
        snprintf(out->right, sizeof(out->right), "%u%%",
                 (unsigned)pharos_theme_brightness());
        out->tone = PHAROS_TONE_NEUTRAL;
        return true;
    case ROW_ALARM:
        snprintf(out->left, sizeof(out->left), "alarm");
        if (!pharos_audio_present()) {
            snprintf(out->right, sizeof(out->right), "no codec");
            out->tone = PHAROS_TONE_DIM;
        } else if (!pharos_audio_enabled()) {
            snprintf(out->right, sizeof(out->right), "MUTED");
            /* Amber, not dim: a muted alarm on a watchdog is a thing the next
             * person to pick this up needs to notice. */
            out->tone = PHAROS_TONE_WARN;
        } else {
            snprintf(out->right, sizeof(out->right), "on");
            out->tone = PHAROS_TONE_GOOD;
        }
        return true;
    case ROW_VOLUME:
        snprintf(out->left, sizeof(out->left), "speaker volume");
        if (!pharos_audio_present()) {
            snprintf(out->right, sizeof(out->right), "--");
            out->tone = PHAROS_TONE_DIM;
        } else {
            snprintf(out->right, sizeof(out->right), "%u%%",
                     (unsigned)pharos_audio_volume());
            out->tone = pharos_audio_enabled() ? PHAROS_TONE_NEUTRAL
                                               : PHAROS_TONE_DIM;
        }
        return true;
    case ROW_REGION: {
        snprintf(out->left, sizeof(out->left), "region");
        /* The right column is 12 bytes; "World (1-13)" does not fit and was
         * being cut mid-parenthesis. The channel span is its own row below. */
        const char *rn = pharos_region_name(pharos_region_get());
        char sh[12];
        unsigned k = 0;
        for (; rn[k] && rn[k] != ' ' && rn[k] != '(' && k < sizeof(sh) - 1; k++) {
            sh[k] = rn[k];
        }
        sh[k] = '\0';
        snprintf(out->right, sizeof(out->right), "%s", sh);
        out->tone = PHAROS_TONE_NEUTRAL;
        return true;
    }
    case 5:
        snprintf(out->left, sizeof(out->left), "transmit fence");
        snprintf(out->right, sizeof(out->right), "%s", s.fence_ok ? "CLEAN" : "BROKEN");
        out->tone = s.fence_ok ? PHAROS_TONE_GOOD : PHAROS_TONE_BAD;
        return true;
    case 6:
        snprintf(out->left, sizeof(out->left), "transmit attempts");
        snprintf(out->right, sizeof(out->right), "%u", (unsigned)s.fence.tx_attempts);
        out->tone = s.fence.tx_attempts ? PHAROS_TONE_BAD : PHAROS_TONE_GOOD;
        return true;
    case 7:
        snprintf(out->left, sizeof(out->left), "battery");
        if (have_batt && b.present) {
            snprintf(out->right, sizeof(out->right), "%u%%", (unsigned)b.soc_pct);
            out->tone = (b.soc_pct < 20u) ? PHAROS_TONE_BAD
                      : (b.soc_pct < 50u) ? PHAROS_TONE_WARN : PHAROS_TONE_GOOD;
        } else {
            snprintf(out->right, sizeof(out->right), "USB");
            out->tone = PHAROS_TONE_DIM;
        }
        return true;
    case 8:
        snprintf(out->left, sizeof(out->left), "pack voltage");
        if (have_batt && b.mv) {
            snprintf(out->right, sizeof(out->right), "%u.%02uV",
                     (unsigned)(b.mv / 1000), (unsigned)((b.mv % 1000) / 10));
            out->tone = PHAROS_TONE_NEUTRAL;
        } else {
            snprintf(out->right, sizeof(out->right), "--");
            out->tone = PHAROS_TONE_DIM;
        }
        return true;
    case 9:
        snprintf(out->left, sizeof(out->left), "channels");
        snprintf(out->right, sizeof(out->right), "1-%u",
                 (unsigned)pharos_region_max_channel());
        out->tone = PHAROS_TONE_DIM;
        return true;
    case 10:
        snprintf(out->left, sizeof(out->left), "bands heard");
        snprintf(out->right, sizeof(out->right), "2.4 only");
        /* Amber on purpose: a defender who forgets this device is deaf above
         * 2.4 GHz will read a quiet screen as a quiet building. */
        out->tone = PHAROS_TONE_WARN;
        return true;
    case 11:
        snprintf(out->left, sizeof(out->left), "screen rotation");
        snprintf(out->right, sizeof(out->right), "%d deg", pharos_bsp_rotation());
        out->tone = PHAROS_TONE_DIM;
        return true;
    case 12: {
        snprintf(out->left, sizeof(out->left), "internal RAM");
        const unsigned kb =
            (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA) / 1024);
        snprintf(out->right, sizeof(out->right), "%u KB", kb);
        /* The display flush and the Wi-Fi driver compete for this; when it
         * runs out the panel starts dropping frames. Worth watching. */
        out->tone = (kb < 24u) ? PHAROS_TONE_BAD
                  : (kb < 48u) ? PHAROS_TONE_WARN : PHAROS_TONE_GOOD;
        return true;
    }
    case 13:
        snprintf(out->left, sizeof(out->left), "PSRAM free");
        snprintf(out->right, sizeof(out->right), "%u KB",
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
        out->tone = PHAROS_TONE_DIM;
        return true;
    case 14: {
        snprintf(out->left, sizeof(out->left), "uptime");
        const uint32_t sec = (uint32_t)(esp_timer_get_time() / 1000000ull);
        if (sec >= 3600u) {
            snprintf(out->right, sizeof(out->right), "%uh%02um",
                     (unsigned)(sec / 3600u), (unsigned)((sec % 3600u) / 60u));
        } else {
            snprintf(out->right, sizeof(out->right), "%um%02us",
                     (unsigned)(sec / 60u), (unsigned)(sec % 60u));
        }
        out->tone = PHAROS_TONE_DIM;
        return true;
    }
    case 15:
        snprintf(out->left, sizeof(out->left), "firmware");
        snprintf(out->right, sizeof(out->right), "%.11s", app ? app->version : "?");
        out->tone = PHAROS_TONE_DIM;
        return true;
    case 16:
        snprintf(out->left, sizeof(out->left), "lenses");
        snprintf(out->right, sizeof(out->right), "%u", pharos_lens_count());
        out->tone = PHAROS_TONE_DIM;
        return true;
    case 17: {
        /* The IMU the vendor BSP declares as absent. Worth a row because when
         * it is missing, Vigil quietly loses a whole evidence family - and a
         * capability that silently is not there is the kind of thing an
         * operator should be able to check rather than assume. */
        snprintf(out->left, sizeof(out->left), "motion sensor");
        uint8_t st = 0;
        uint32_t steps = 0;
        if (!pharos_ui_motion(&st, &steps, NULL)) {
            snprintf(out->right, sizeof(out->right), "absent");
            out->tone = PHAROS_TONE_WARN;
        } else {
            static const char *k[] = { "settling", "still", "walking", "moving" };
            snprintf(out->right, sizeof(out->right), "%s", (st < 4u) ? k[st] : "?");
            out->tone = PHAROS_TONE_GOOD;
        }
        return true;
    }
    case 18: {
        snprintf(out->left, sizeof(out->left), "steps this session");
        uint32_t steps = 0;
        if (!pharos_ui_motion(NULL, &steps, NULL)) {
            snprintf(out->right, sizeof(out->right), "--");
            out->tone = PHAROS_TONE_DIM;
        } else {
            snprintf(out->right, sizeof(out->right), "%u", (unsigned)steps);
            out->tone = PHAROS_TONE_DIM;
        }
        return true;
    }
    default:
        return false;
    }
}

/* The centre tap, on a row. Each editable setting advances by one step and
 * the row it sits on is the readout, so nothing is changed blind. */
static bool k_system_row_edit(unsigned row)
{
    switch (row) {
    case ROW_THEME:
        pharos_theme_next();
        ESP_LOGI(TAG, "theme now %s", pharos_theme()->name);
        return true;

    case ROW_BRIGHT: {
        /* Step from where the panel ACTUALLY is rather than from a counter of
         * our own - the same bug the alarm had, where a variable that started
         * at zero meant the first tap always did the wrong thing. */
        const uint8_t cur = pharos_theme_brightness();
        /* A value that is not one of the steps - left by an older build, or
         * by the console - starts the cycle at the beginning rather than
         * landing on whatever the search happened to fall off at. */
        unsigned i = BRIGHT_N - 1u;
        for (unsigned k = 0; k < BRIGHT_N; k++) {
            if (k_bright_steps[k] == cur) {
                i = k;
                break;
            }
        }
        pharos_theme_set_brightness(k_bright_steps[(i + 1u) % BRIGHT_N]);
        return true;
    }

    case ROW_ALARM:
        if (!pharos_audio_present()) {
            return false; /* nothing to toggle; let the tap fall through */
        }
        pharos_audio_set_enabled(!pharos_audio_enabled());
        if (pharos_audio_enabled()) {
            pharos_audio_alert(PHAROS_ALERT_ACK); /* prove it works */
        }
        return true;

    case ROW_VOLUME: {
        if (!pharos_audio_present()) {
            return false;
        }
        const uint8_t cur = pharos_audio_volume();
        unsigned i = VOL_N - 1u; /* an unrecognised value starts at the top */
        for (unsigned k = 0; k < VOL_N; k++) {
            if (k_vol_steps[k] == cur) {
                i = k;
                break;
            }
        }
        const uint8_t next = k_vol_steps[(i + 1u) % VOL_N];
        pharos_audio_set_volume(next);
        /* Turning the volume up on a muted alarm is what somebody means, so
         * do that rather than making them find the row above as well. */
        pharos_audio_set_enabled(true);
        pharos_audio_alert(PHAROS_ALERT_ACK); /* the setting demonstrates itself */
        return true;
    }

    case ROW_REGION: {
        const pharos_region_t r = pharos_region_get();
        pharos_region_set((pharos_region_t)((r + 1) % PHAROS_REGION_COUNT));
        /* region_name() already carries the channel span; printing it again
         * gave "FCC (1-11) (1-11)". */
        ESP_LOGI(TAG, "region now %s", pharos_region_name(pharos_region_get()));
        return true;
    }

    default:
        return false;
    }
}

/* Opening a row: what the setting is FOR, and what the alternatives are.
 *
 * A cycling control has an obvious weakness - you cannot see what is coming
 * next, so you tap until you recognise something. Opening the row lists every
 * value, marks the one in force, and says what each is good for, which turns
 * five blind taps into one informed one. */
static bool k_system_expand(unsigned row, unsigned sub,
                            struct pharos_lens_row *out)
{
    switch (row) {
    case ROW_THEME: {
        if (sub == 0) {
            snprintf(out->left, sizeof(out->left), "verdict colours");
            snprintf(out->right, sizeof(out->right), "fixed");
            /* Stated on the screen because it is a promise, not a detail: a
             * theme changes the instrument, never the reading. */
            out->tone = PHAROS_TONE_GOOD;
            return true;
        }
        const unsigned i = sub - 1u;
        const pharos_theme_t *t = pharos_theme_at(i);
        if (!t) {
            return false;
        }
        snprintf(out->left, sizeof(out->left), "%s", t->note);
        snprintf(out->right, sizeof(out->right), "%s%s",
                 (i == pharos_theme_index()) ? "* " : "", t->name);
        out->tone = (i == pharos_theme_index()) ? PHAROS_TONE_NEUTRAL
                                                : PHAROS_TONE_DIM;
        return true;
    }

    case ROW_BRIGHT:
        /* One row per step with the one in force marked, so the cycle stops
         * being blind - the same shape as the theme and region lists. */
        if (sub < BRIGHT_N) {
            snprintf(out->left, sizeof(out->left), "%u%%%s",
                     (unsigned)k_bright_steps[sub],
                     (k_bright_steps[sub] == 20u) ? "  (dark rooms)" : "");
            snprintf(out->right, sizeof(out->right), "%s",
                     (k_bright_steps[sub] == pharos_theme_brightness()) ? "IN USE"
                                                                       : "");
            out->tone = (k_bright_steps[sub] == pharos_theme_brightness())
                            ? PHAROS_TONE_NEUTRAL
                            : PHAROS_TONE_DIM;
            return true;
        }
        if (sub == BRIGHT_N) {
            /* Why there is a floor: a screen you cannot turn back on without
             * a USB cable is a bricked screen to whoever is holding it. */
            snprintf(out->left, sizeof(out->left), "never goes below");
            snprintf(out->right, sizeof(out->right), "10%%");
            out->tone = PHAROS_TONE_DIM;
            return true;
        }
        return false;

    case ROW_ALARM:
    case ROW_VOLUME:
        switch (sub) {
        case 0:
            snprintf(out->left, sizeof(out->left), "codec");
            snprintf(out->right, sizeof(out->right), "%s",
                     pharos_audio_present() ? "ES8311" : "absent");
            out->tone = pharos_audio_present() ? PHAROS_TONE_GOOD
                                               : PHAROS_TONE_BAD;
            return true;
        case 1:
            snprintf(out->left, sizeof(out->left), "state");
            snprintf(out->right, sizeof(out->right), "%s",
                     pharos_audio_enabled() ? "sounding" : "MUTED");
            out->tone = pharos_audio_enabled() ? PHAROS_TONE_GOOD
                                               : PHAROS_TONE_WARN;
            return true;
        case 2:
            snprintf(out->left, sizeof(out->left), "volume");
            snprintf(out->right, sizeof(out->right), "%u%%",
                     (unsigned)pharos_audio_volume());
            out->tone = PHAROS_TONE_NEUTRAL;
            return true;
        case 3:
            snprintf(out->left, sizeof(out->left), "alerts it can play");
            snprintf(out->right, sizeof(out->right), "5");
            out->tone = PHAROS_TONE_DIM;
            return true;
        case 4:
            snprintf(out->left, sizeof(out->left), "remembered");
            snprintf(out->right, sizeof(out->right), "yes");
            out->tone = PHAROS_TONE_DIM;
            return true;
        default:
            return false;
        }

    case ROW_REGION: {
        if (sub >= (unsigned)PHAROS_REGION_COUNT) {
            return false;
        }
        const pharos_region_t r = (pharos_region_t)sub;
        snprintf(out->left, sizeof(out->left), "%s", pharos_region_name(r));
        snprintf(out->right, sizeof(out->right), "%s",
                 (r == pharos_region_get()) ? "IN USE" : "");
        out->tone = (r == pharos_region_get()) ? PHAROS_TONE_NEUTRAL
                                               : PHAROS_TONE_DIM;
        return true;
    }

    case 5: /* transmit fence */
        switch (sub) {
        case 0:
            snprintf(out->left, sizeof(out->left), "wrap traps linked");
            snprintf(out->right, sizeof(out->right), "%s",
                     s.fence.wrap_linked ? "yes" : "NO");
            out->tone = s.fence.wrap_linked ? PHAROS_TONE_GOOD : PHAROS_TONE_BAD;
            return true;
        case 1:
            snprintf(out->left, sizeof(out->left), "BLE observer only");
            snprintf(out->right, sizeof(out->right), "%s",
                     s.fence.ble_observer_only ? "yes" : "NO");
            out->tone = s.fence.ble_observer_only ? PHAROS_TONE_GOOD
                                                  : PHAROS_TONE_BAD;
            return true;
        case 2:
            snprintf(out->left, sizeof(out->left), "no TX symbol in image");
            snprintf(out->right, sizeof(out->right), "%s",
                     s.fence.tx_symbols_absent ? "yes" : "NO");
            out->tone = s.fence.tx_symbols_absent ? PHAROS_TONE_GOOD
                                                  : PHAROS_TONE_BAD;
            return true;
        case 3:
            snprintf(out->left, sizeof(out->left), "attempts since boot");
            snprintf(out->right, sizeof(out->right), "%u",
                     (unsigned)s.fence.tx_attempts);
            out->tone = s.fence.tx_attempts ? PHAROS_TONE_BAD : PHAROS_TONE_GOOD;
            return true;
        case 4:
            snprintf(out->left, sizeof(out->left), "checked every tick");
            snprintf(out->right, sizeof(out->right), "yes");
            out->tone = PHAROS_TONE_DIM;
            return true;
        default:
            return false;
        }

    default:
        return false;
    }
}

static const pharos_lens_t k_system = {
    .id = "sys.audit",
    .name = "System",
    .summary = "Battery, region, and proof the transmit fence is clean",
    .glyph = "gear",
    .kind = PHAROS_LENS_SYSTEM,
    .caps = PHAROS_CAP_PMU | PHAROS_CAP_RTC,
    .budget_ma = 40,
    .on_mount = system_mount,
    .on_tick = system_tick,
    .display = k_system_display,
    .on_select = system_select,
    .row = k_system_row,
    .row_head_left = "SETTING",
    .row_head_right = "STATE",
    .row_edit = k_system_row_edit,
    .row_expand = k_system_expand,
};

PHAROS_LENS_REGISTER(&k_system);
