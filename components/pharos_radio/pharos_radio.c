/* Pharos - the radio HAL implementation
 *
 * Receive-only, capability-gated, region-clamped. The interesting logic in
 * here is small and deliberate; the promiscuous callback is the one piece of
 * code in the project with a hard real-time budget, so it does the minimum -
 * parse the fixed header, timestamp it, push a summary - and hands everything
 * else to the analytics core through the bus.
 *
 * The ESP-IDF entry points are declared weak-ish behind PHAROS_HOST so this
 * file also compiles in a host harness for the parts that do not need a
 * radio (channel clamping, dwell bookkeeping). The radio itself is only real
 * on target.
 */
#include "pharos_radio.h"

#include <string.h>

#include "pharos_dot11.h"
#include "pharos_lens.h"
#include "pharos_region.h"

#if !defined(PHAROS_HOST)
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#endif

static const char *TAG = "radio";

/* Fence status, filled at init and never mutated afterwards. */
extern volatile uint32_t g_pharos_tx_attempts;

static struct {
    pharos_bus_t *bus;
    pharos_scan_plan_t plan;
    uint8_t plan_idx;
    uint8_t channel;
    bool camped;
    bool running;
    uint64_t channel_since_us;
    uint32_t dwell_us[PHAROS_CHAN_MAX + 1];
    uint64_t window_us;
    pharos_radio_stats_t stats;
} s;

/* ---- channel plans (host-safe) -------------------------------------- */

pharos_scan_plan_t pharos_scan_plan_survey(void)
{
    pharos_scan_plan_t p;
    memset(&p, 0, sizeof(p));
    const uint8_t hi = pharos_region_max_channel();
    for (uint8_t c = PHAROS_CHAN_MIN; c <= hi && p.n_channels < PHAROS_CHAN_MAX; c++) {
        p.channels[p.n_channels++] = c;
    }
    p.dwell_ms = 200;
    p.hop = PHAROS_HOP_SURVEY;
    p.want_mgmt = true;
    return p;
}

pharos_scan_plan_t pharos_scan_plan_camp(uint8_t channel)
{
    pharos_scan_plan_t p;
    memset(&p, 0, sizeof(p));
    p.channels[0] = pharos_region_clamp_channel(channel);
    p.n_channels = 1;
    p.dwell_ms = 1000;
    p.hop = PHAROS_HOP_CAMP;
    p.want_mgmt = true;
    return p;
}

/* ---- dwell bookkeeping (host-safe) ---------------------------------- */

static void account_dwell(uint8_t channel, uint32_t us)
{
    if (channel <= PHAROS_CHAN_MAX) {
        s.dwell_us[channel] += us;
        s.window_us += us;
    }
    /* Decay over roughly a ten-second window so the ceiling reflects recent
     * behaviour, not the whole session. */
    if (s.window_us > 10ull * 1000000ull) {
        for (unsigned c = 0; c <= PHAROS_CHAN_MAX; c++) {
            s.dwell_us[c] = (s.dwell_us[c] * 7u) / 8u;
        }
        s.window_us = (s.window_us * 7u) / 8u;
    }
}

uint16_t pharos_radio_dwell_permil(uint8_t channel)
{
    if (channel > PHAROS_CHAN_MAX || s.window_us == 0) {
        /* No history yet: assume an even hop over the plan so a fresh verdict
         * is cautious rather than falsely confident. */
        const uint8_t n = s.plan.n_channels ? s.plan.n_channels : 14;
        return (uint16_t)(1000u / n);
    }
    return (uint16_t)(((uint64_t)s.dwell_us[channel] * 1000ull) / s.window_us);
}

uint8_t pharos_radio_channel(void) { return s.channel; }
bool pharos_radio_is_camped(void) { return s.camped; }

void pharos_radio_stats(pharos_radio_stats_t *out)
{
    if (out) {
        *out = s.stats;
        out->current_channel = s.channel;
        out->camped = s.camped;
    }
}

void pharos_radio_fence_status(pharos_tx_fence_t *out)
{
    if (!out) {
        return;
    }
    /* These three are true by construction of the build; check_tx_fence.sh is
     * what proves it, and CI is what runs check_tx_fence.sh. The firmware
     * reports the design, and refuses to claim a clean fence if the wrap
     * traps ever fired. */
    out->wrap_linked = true;
    out->ble_observer_only = true;
    out->tx_symbols_absent = true;
    out->tx_attempts = g_pharos_tx_attempts;
}

#if !defined(PHAROS_HOST)

/* ---- the hot path --------------------------------------------------- */

static void promisc_cb(void *buf, wifi_promiscuous_pkt_type_t type)
{
    if (!s.bus || !buf) {
        return;
    }
    const wifi_promiscuous_pkt_t *pkt = (const wifi_promiscuous_pkt_t *)buf;
    const uint8_t *payload = pkt->payload;
    const int len = pkt->rx_ctrl.sig_len;

    pharos_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.t_us = (uint64_t)esp_timer_get_time();
    ev.type = PHAROS_EV_DOT11;

    if (!pharos_dot11_parse_header(payload, (size_t)len, &ev.u.dot11)) {
        return;
    }
    ev.u.dot11.rssi = (int8_t)pkt->rx_ctrl.rssi;
    ev.u.dot11.channel = s.channel;

    uint16_t reason = 0;
    if (pharos_dot11_reason(payload, (size_t)len, &ev.u.dot11, &reason)) {
        ev.u.dot11.reason_or_status = reason;
    }
    /* The RSN/MFP flag comes from the beacon body; the analytics core walks
     * the element chain against the capture ring. Here we only note that a
     * management frame arrived. */

    /* One fixed-offset check on data frames: is this an unprotected EAPOL-Key?
     * It is a handful of byte compares - affordable in the hot path - and it
     * is what lets the Harvest lens see that somebody is collecting 4-way
     * handshakes. No nonce, MIC or key material is copied; only which message
     * it was, and whether message 1 carried a PMKID. */
    if (ev.u.dot11.type == PHAROS_FT_DATA) {
        pharos_eapol_t eap;
        if (pharos_dot11_eapol(payload, (size_t)len, &eap)) {
            ev.u.dot11.eapol = eap.msg;
            if (eap.has_pmkid) {
                ev.u.dot11.flags |= PHAROS_DOT11_F_PMKID;
            }
        }
    }

    s.stats.frames_seen++;
    if (!pharos_bus_push(s.bus, &ev)) {
        /* Bus full: the count is not lost, it feeds the confidence ceiling. */
    }
    (void)type;
}

static void apply_channel(uint8_t channel)
{
    const uint64_t now = (uint64_t)esp_timer_get_time();
    if (s.channel && s.channel_since_us) {
        account_dwell(s.channel, (uint32_t)(now - s.channel_since_us));
    }
    s.channel = pharos_region_clamp_channel(channel);
    s.channel_since_us = now;
    esp_wifi_set_channel(s.channel, WIFI_SECOND_CHAN_NONE);
    s.stats.channel_changes++;
}

static void hop_task(void *arg)
{
    (void)arg;
    while (s.running) {
        const pharos_scan_plan_t *p = &s.plan;
        apply_channel(p->channels[s.plan_idx]);
        s.plan_idx = (uint8_t)((s.plan_idx + 1) % (p->n_channels ? p->n_channels : 1));
        vTaskDelay(pdMS_TO_TICKS(p->dwell_ms ? p->dwell_ms : 200));
    }
    vTaskDelete(NULL);
}

bool pharos_radio_rx_start(const pharos_scan_plan_t *plan, pharos_bus_t *bus)
{
    /* Capability gate: no lens may receive without declaring CAP_WIFI_RX,
     * and no lens may hop without CAP_WIFI_CHAN. There is no TX bit to hold. */
    const pharos_caps_t caps = pharos_lens_active_caps();
    if (!(caps & PHAROS_CAP_WIFI_RX)) {
        ESP_LOGE(TAG, "rx_start refused: active lens has not declared wifi.rx");
        return false;
    }
    if (!plan || !bus || plan->n_channels == 0) {
        return false;
    }
    if (plan->n_channels > 1 && !(caps & PHAROS_CAP_WIFI_CHAN)) {
        ESP_LOGE(TAG, "hopping plan refused: lens has not declared wifi.chan");
        return false;
    }

    memset(&s, 0, sizeof(s));
    s.bus = bus;
    s.plan = *plan;
    s.camped = (plan->n_channels == 1);
    s.running = true;

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    esp_wifi_init(&cfg);
    /* STA + NULL only. The fence traps AP modes; we never ask for one. */
    esp_wifi_set_mode(WIFI_MODE_NULL);
    esp_wifi_start();

    wifi_promiscuous_filter_t filter = { 0 };
    filter.filter_mask = 0;
    if (plan->want_mgmt) filter.filter_mask |= WIFI_PROMIS_FILTER_MASK_MGMT;
    if (plan->want_data) filter.filter_mask |= WIFI_PROMIS_FILTER_MASK_DATA;
    if (plan->want_ctrl) filter.filter_mask |= WIFI_PROMIS_FILTER_MASK_CTRL;
    if (filter.filter_mask == 0) filter.filter_mask = WIFI_PROMIS_FILTER_MASK_MGMT;
    esp_wifi_set_promiscuous_filter(&filter);
    esp_wifi_set_promiscuous_rx_cb(&promisc_cb);
    esp_wifi_set_promiscuous(true);

    xTaskCreatePinnedToCore(hop_task, "pharos_hop", 3072, NULL, 5, NULL, 0);
    ESP_LOGI(TAG, "rx started: %u channels, %u ms dwell, %s",
             plan->n_channels, plan->dwell_ms, s.camped ? "camped" : "hopping");
    return true;
}

void pharos_radio_rx_stop(void)
{
    if (!s.running) {
        return;
    }
    s.running = false;
    esp_wifi_set_promiscuous(false);
    esp_wifi_stop();
    esp_wifi_deinit();
    s.bus = NULL;
    ESP_LOGI(TAG, "rx stopped");
}

#else  /* PHAROS_HOST: no radio, just the arithmetic */

bool pharos_radio_rx_start(const pharos_scan_plan_t *plan, pharos_bus_t *bus)
{
    if (!(pharos_lens_active_caps() & PHAROS_CAP_WIFI_RX)) return false;
    if (!plan || !bus) return false;
    memset(&s, 0, sizeof(s));
    s.bus = bus; s.plan = *plan; s.camped = (plan->n_channels == 1);
    s.channel = plan->channels[0];
    return true;
}
void pharos_radio_rx_stop(void) { s.running = false; s.bus = NULL; }

#endif /* PHAROS_HOST */

/* BLE observer is a M4 milestone; the entry points exist so lenses can be
 * written against them, and refuse cleanly until the NimBLE observer glue
 * lands. Refusing is the honest behaviour, not a stub that pretends. */
bool pharos_radio_ble_scan_start(pharos_bus_t *bus)
{
    (void)bus;
    if (!(pharos_lens_active_caps() & PHAROS_CAP_BLE_SCAN)) {
        return false;
    }
    ESP_LOGW(TAG, "ble observer not yet wired (milestone M4)");
    return false;
}

void pharos_radio_ble_scan_stop(void) {}
