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

#include "pharos_lens.h"
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
};

PHAROS_LENS_REGISTER(&k_system);
