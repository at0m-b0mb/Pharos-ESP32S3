/* Pharos lens: Probe - the privacy mirror
 *
 * Point it at a room and it shows the room what the room is broadcasting: the
 * names of networks people's phones have joined before, the devices that never
 * randomise their address, and the ones whose randomisation it just defeated.
 *
 * This is the lens that changes behaviour. A blue-team awareness session runs
 * on it; a red-team recon demonstration runs on it too - and it stays lawful
 * because it only ever listens, keeps nothing past the session, and redacts
 * addresses at write time. Judgement lives in pharos_probe.c, host-tested.
 */
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "pharos_bus.h"
#include "pharos_dot11.h"
#include "pharos_lens.h"
#include "pharos_probe.h"
#include "pharos_radio.h"
#include "pharos_report.h"

static const char *TAG = "lens.probe";

#define PROBE_RING 512

EXT_RAM_BSS_ATTR static pharos_event_t s_slots[PROBE_RING];
static pharos_bus_t s_bus;
EXT_RAM_BSS_ATTR static pp_engine_t s_engine;
static SemaphoreHandle_t s_lock;

static bool probe_mount(void)
{
    pp_reset(&s_engine);
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    return s_lock && pharos_bus_init(&s_bus, s_slots, PROBE_RING);
}

static bool probe_start(void)
{
    pharos_scan_plan_t plan = pharos_scan_plan_survey();
    plan.dwell_ms = 250;
    plan.want_mgmt = true; /* probe requests are management frames */
    return pharos_radio_rx_start(&plan, &s_bus);
}

static void probe_stop(void) { pharos_radio_rx_stop(); }

static void probe_event(const pharos_event_t *ev)
{
    if (!ev || ev->type != PHAROS_EV_DOT11) {
        return;
    }
    const pharos_ev_dot11_t *f = &ev->u.dot11;
    if (f->type != PHAROS_FT_MGMT || f->subtype != PHAROS_ST_PROBE_REQ) {
        return;
    }

    pp_probe_t p;
    memset(&p, 0, sizeof(p));
    memcpy(p.addr, f->a2, 6);
    p.seq = f->seq;
    p.rssi = f->rssi;
    p.t_us = ev->t_us;
    /* The SSID and the IE-order fingerprint come from the frame body, held in
     * the capture ring; the summary carries a coarse fingerprint from the
     * rate/flags for now. The full IE fingerprint lands in M5. */
    p.fingerprint = ((uint32_t)f->flags << 8) ^ f->rate_idx ^ 0xA53Cu;
    /* p.ssid_len stays 0 (wildcard) until the body walk fills it in. */

    if (xSemaphoreTake(s_lock, 0) == pdTRUE) {
        pp_observe(&s_engine, &p);
        xSemaphoreGive(s_lock);
    }
}

unsigned pharos_lens_probe_snapshot(pp_device_t *devs, pp_verdict_t *verdicts, unsigned max)
{
    if (!devs || !verdicts || !s_lock) {
        return 0;
    }
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(5)) != pdTRUE) {
        return 0;
    }
    const unsigned n = (s_engine.n_devices < max) ? s_engine.n_devices : max;
    for (unsigned i = 0; i < n; i++) {
        devs[i] = s_engine.devices[i];
        pp_grade_device(&s_engine.devices[i], &verdicts[i]);
    }
    xSemaphoreGive(s_lock);
    return n;
}

bool pharos_lens_probe_report(char *buf, size_t cap, prt_redact_t redact, uint32_t salt)
{
    prt_t w;
    prt_init(&w, buf, cap, redact, salt);
    prt_obj_begin(&w, NULL);
    prt_str(&w, "tool", "pharos");
    prt_str(&w, "lens", "wifi.probe");
    prt_u32(&w, "uptime_ms", (uint32_t)(esp_timer_get_time() / 1000));
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
        prt_u32(&w, "probes_seen", s_engine.probes_seen);
        prt_u32(&w, "wildcards_seen", s_engine.wildcards_seen);
        prt_arr_begin(&w, "devices");
        for (unsigned i = 0; i < s_engine.n_devices; i++) {
            pp_verdict_t v;
            pp_grade_device(&s_engine.devices[i], &v);
            prt_obj_begin(&w, NULL);
            prt_mac(&w, "addr", s_engine.devices[i].addr);
            prt_str(&w, "grade", pp_grade_name(v.grade));
            prt_u32(&w, "exposure", v.exposure);
            prt_u32(&w, "networks", v.networks);
            prt_u32(&w, "identities", v.identities);
            prt_str(&w, "narrowest", pp_place_name(v.narrowest));
            prt_u32(&w, "notes", v.notes);
            prt_obj_end(&w);
        }
        prt_arr_end(&w);
        xSemaphoreGive(s_lock);
    }
    prt_obj_end(&w);
    return prt_finish(&w);
}

static struct pharos_bus *probe_ingest(void) { return &s_bus; }

static const pharos_lens_t k_probe = {
    .id = "wifi.probe",
    .name = "Probe",
    .summary = "Shows a room what its phones are broadcasting about their owners",
    .glyph = "ear",
    .kind = PHAROS_LENS_OBSERVE,
    .caps = PHAROS_CAP_WIFI_RX | PHAROS_CAP_WIFI_CHAN | PHAROS_CAP_STORAGE_W,
    .budget_ma = 128,
    .on_mount = probe_mount,
    .on_start = probe_start,
    .on_stop = probe_stop,
    .on_event = probe_event,
    .ingest = probe_ingest,
};

PHAROS_LENS_REGISTER(&k_probe);
