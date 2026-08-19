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

static bool k_system_row(unsigned index, struct pharos_lens_row *out)
{
    pwr_battery_t b;
    const bool have_batt = pharos_bsp_battery(&b);
    const esp_app_desc_t *app = esp_app_get_description();

    switch (index) {
    case 0:
        snprintf(out->left, sizeof(out->left), "transmit fence");
        snprintf(out->right, sizeof(out->right), "%s", s.fence_ok ? "CLEAN" : "BROKEN");
        out->tone = s.fence_ok ? PHAROS_TONE_GOOD : PHAROS_TONE_BAD;
        return true;
    case 1:
        snprintf(out->left, sizeof(out->left), "transmit attempts");
        snprintf(out->right, sizeof(out->right), "%u", (unsigned)s.fence.tx_attempts);
        out->tone = s.fence.tx_attempts ? PHAROS_TONE_BAD : PHAROS_TONE_GOOD;
        return true;
    case 2:
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
    case 3:
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
    case 4:
        snprintf(out->left, sizeof(out->left), "alarm");
        if (!pharos_audio_present()) {
            snprintf(out->right, sizeof(out->right), "no codec");
            out->tone = PHAROS_TONE_DIM;
        } else if (!pharos_audio_enabled()) {
            snprintf(out->right, sizeof(out->right), "MUTED");
            out->tone = PHAROS_TONE_WARN;
        } else {
            snprintf(out->right, sizeof(out->right), "%u%%",
                     (unsigned)pharos_audio_volume());
            out->tone = PHAROS_TONE_GOOD;
        }
        return true;
    case 5:
        snprintf(out->left, sizeof(out->left), "region");
        /* The right column is 12 bytes; "World (1-13)" does not fit and was
         * being cut mid-parenthesis. The channel span is its own row below. */
        {
            const char *rn = pharos_region_name(pharos_region_get());
            char sh[12];
            unsigned k = 0;
            for (; rn[k] && rn[k] != ' ' && rn[k] != '(' && k < sizeof(sh) - 1; k++) {
                sh[k] = rn[k];
            }
            sh[k] = '\0';
            snprintf(out->right, sizeof(out->right), "%s", sh);
        }
        out->tone = PHAROS_TONE_NEUTRAL;
        return true;
    case 6:
        snprintf(out->left, sizeof(out->left), "channels");
        snprintf(out->right, sizeof(out->right), "1-%u",
                 (unsigned)pharos_region_max_channel());
        out->tone = PHAROS_TONE_DIM;
        return true;
    case 7:
        snprintf(out->left, sizeof(out->left), "bands heard");
        snprintf(out->right, sizeof(out->right), "2.4 only");
        /* Amber on purpose: a defender who forgets this device is deaf above
         * 2.4 GHz will read a quiet screen as a quiet building. */
        out->tone = PHAROS_TONE_WARN;
        return true;
    case 8:
        snprintf(out->left, sizeof(out->left), "screen rotation");
        snprintf(out->right, sizeof(out->right), "%d deg", pharos_bsp_rotation());
        out->tone = PHAROS_TONE_DIM;
        return true;
    case 9: {
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
    case 10:
        snprintf(out->left, sizeof(out->left), "PSRAM free");
        snprintf(out->right, sizeof(out->right), "%u KB",
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
        out->tone = PHAROS_TONE_DIM;
        return true;
    case 11: {
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
    case 12:
        snprintf(out->left, sizeof(out->left), "firmware");
        snprintf(out->right, sizeof(out->right), "%.11s", app ? app->version : "?");
        out->tone = PHAROS_TONE_DIM;
        return true;
    case 13:
        snprintf(out->left, sizeof(out->left), "lenses");
        snprintf(out->right, sizeof(out->right), "%u", pharos_lens_count());
        out->tone = PHAROS_TONE_DIM;
        return true;
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
};

PHAROS_LENS_REGISTER(&k_system);
