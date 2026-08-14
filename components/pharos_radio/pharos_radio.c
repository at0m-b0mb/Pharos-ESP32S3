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
/* NimBLE, compiled observer-only (see this component's Kconfig). The scan is
 * additionally PASSIVE, so this radio never emits a SCAN_REQ. */
#include "host/ble_gap.h"
#include "host/ble_hs.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
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

    /* Per-channel-visit tallies, reset each time we retune. These become one
     * PHAROS_EV_DWELL event when the visit ends - the only summary of what a
     * channel sounded like as opposed to what any single frame said. */
    uint32_t dwell_frames;
    uint32_t dwell_retries;
    uint32_t dwell_busy_us;
    int8_t dwell_peak_rssi;
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

/* Summarise the visit we are leaving and publish it, then clear the tallies.
 * Emitted on the same bus as frames so a lens sees it in order, and pushed
 * best-effort: if the ring is full the drop is already counted and feeds the
 * confidence ceiling, exactly like a dropped frame. */
static void emit_dwell(uint8_t channel, uint32_t us)
{
#if !defined(PHAROS_HOST)
    if (!s.bus || channel == 0 || channel > PHAROS_CHAN_MAX || us == 0) {
        return;
    }
    pharos_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.t_us = (uint64_t)esp_timer_get_time();
    ev.type = PHAROS_EV_DWELL;
    ev.u.dwell.channel = channel;
    ev.u.dwell.band = 0; /* this radio is deaf above 2.4 GHz */
    ev.u.dwell.dwell_ms = (uint16_t)(us / 1000u > 0xFFFFu ? 0xFFFFu : us / 1000u);
    ev.u.dwell.frames = (uint16_t)(s.dwell_frames > 0xFFFFu ? 0xFFFFu : s.dwell_frames);
    ev.u.dwell.peak_rssi = s.dwell_peak_rssi;
    /* No true noise-floor register is exposed, so leave it at 0 - which the
     * engines read as "unknown" and disclose - rather than inventing one. */
    ev.u.dwell.noise_floor = 0;
    {
        const uint64_t busy = ((uint64_t)s.dwell_busy_us * 1000ull) / (uint64_t)us;
        ev.u.dwell.busy_permil = (uint16_t)(busy > 1000ull ? 1000ull : busy);
    }
    ev.u.dwell.retries =
        (uint16_t)(s.dwell_retries > 0xFFFFu ? 0xFFFFu : s.dwell_retries);
    pharos_bus_push(s.bus, &ev);
#else
    (void)channel; (void)us;
#endif
    s.dwell_frames = 0;
    s.dwell_retries = 0;
    s.dwell_busy_us = 0;
    s.dwell_peak_rssi = 0;
}

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

    /* Name and security posture, for the three subtypes that carry them.
     *
     * This is the frame body, so it belongs in a hot path only because the
     * alternative was worse: without it EVERY name-dependent lens was inert -
     * Census graded nothing (pc_grade declines without an RSN element) and Twin
     * grouped nothing (every SSID it was handed was empty). Both walks are
     * bounds-checked and stop at the first malformed element, and only three
     * subtypes pay for them. */
    if (ev.u.dot11.type == PHAROS_FT_MGMT && (size_t)len > 24u) {
        const uint8_t *body = payload + 24;
        const size_t blen = (size_t)len - 24u;
        const uint8_t st = ev.u.dot11.subtype;
        const bool has_fixed = (st == PHAROS_ST_BEACON || st == PHAROS_ST_PROBE_RESP);

        if (has_fixed || st == PHAROS_ST_PROBE_REQ) {
            /* Probe REQUESTS carry no fixed parameters; starting the walk 12
             * bytes in would skip their first element, which is the SSID - the
             * only one that matters. */
            uint8_t ie_len = 0;
            const uint8_t *ssid = pharos_dot11_find_ie_from(
                body, blen, has_fixed ? 12u : 0u, PHAROS_IE_SSID, &ie_len);
            if (ssid && ie_len) {
                const uint8_t n = ie_len > PHAROS_EV_SSID_MAX ? PHAROS_EV_SSID_MAX : ie_len;
                memcpy(ev.u.dot11.ssid, ssid, n);
                ev.u.dot11.ssid_len = n;
            }
        }
        if (has_fixed) {
            pharos_rsn_t rsn;
            memset(&rsn, 0, sizeof(rsn));
            if (pharos_dot11_rsn(body, blen, &rsn) && rsn.has_rsn) {
                uint8_t f = PHAROS_RSN_F_PRESENT;
                if (rsn.mfp_capable)  f |= PHAROS_RSN_F_MFP_CAPABLE;
                if (rsn.mfp_required) f |= PHAROS_RSN_F_MFP_REQUIRED;
                if (rsn.has_sae)      f |= PHAROS_RSN_F_SAE;
                if (rsn.has_psk)      f |= PHAROS_RSN_F_PSK;
                if (rsn.has_owe)      f |= PHAROS_RSN_F_OWE;
                ev.u.dot11.rsn_flags = f;
            }
            /* WPS advertises itself in a vendor element; its presence is a
             * posture fact Census grades, so note it while we are here. */
            uint8_t wps_len = 0;
            const uint8_t *wps = pharos_dot11_find_ie_from(body, blen, 12u, 221, &wps_len);
            if (wps && wps_len >= 4 && wps[0] == 0x00 && wps[1] == 0x50 &&
                wps[2] == 0xF2 && wps[3] == 0x04) {
                ev.u.dot11.rsn_flags |= PHAROS_RSN_F_WPS;
            }
        }
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

    /* Per-visit tallies for the DWELL summary emitted when we leave this
     * channel. Squall reasons about these rather than about any one frame:
     * how much decoded, how much of it was retransmission, how strong the
     * strongest thing here was. */
    s.dwell_frames++;
    if (ev.u.dot11.flags & PHAROS_DOT11_F_RETRY) {
        s.dwell_retries++;
    }
    if (ev.u.dot11.rssi > s.dwell_peak_rssi || s.dwell_peak_rssi == 0) {
        s.dwell_peak_rssi = ev.u.dot11.rssi;
    }
    /* Airtime estimate: a frame occupies the medium for roughly its length at
     * the going rate. Without per-frame duration from the driver, count each
     * decoded frame as a nominal slot - crude, but it is the same crudeness on
     * every channel, and Squall compares channels rather than trusting the
     * absolute number. */
    s.dwell_busy_us += 400;

    if (!pharos_bus_push(s.bus, &ev)) {
        /* Bus full: the count is not lost, it feeds the confidence ceiling. */
    }
    (void)type;
}

static void apply_channel(uint8_t channel)
{
    const uint64_t now = (uint64_t)esp_timer_get_time();
    if (s.channel && s.channel_since_us) {
        const uint32_t visit_us = (uint32_t)(now - s.channel_since_us);
        account_dwell(s.channel, visit_us);
        emit_dwell(s.channel, visit_us);
    }
    s.channel = pharos_region_clamp_channel(channel);
    s.channel_since_us = now;
    esp_wifi_set_channel(s.channel, WIFI_SECOND_CHAN_NONE);
    s.stats.channel_changes++;
}

/* The channel hopper. Exactly one may exist at a time - see rx_stop for why
 * that is worth enforcing rather than assuming. */
static volatile bool s_hop_alive;

static void hop_task(void *arg)
{
    (void)arg;
    while (s.running) {
        const pharos_scan_plan_t *p = &s.plan;
        apply_channel(p->channels[s.plan_idx]);
        s.plan_idx = (uint8_t)((s.plan_idx + 1) % (p->n_channels ? p->n_channels : 1));
        vTaskDelay(pdMS_TO_TICKS(p->dwell_ms ? p->dwell_ms : 200));
    }
    /* Publish the exit BEFORE deleting, so rx_stop knows the task is really
     * gone and it is safe to tear the state down. */
    s_hop_alive = false;
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

    /* Idempotent. Starting while already running used to spawn a SECOND hop
     * task and leak the first: every lens change added one more, all of them
     * driving the channel and all of them reading state that rx_start was
     * about to memset. Three or four lens changes exhausted the heap and
     * rebooted the board - which is exactly how it was reported. */
    if (s.running || s_hop_alive) {
        pharos_radio_rx_stop();
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

    /* Marked alive HERE, not inside the task: the task may not be scheduled
     * before rx_start returns, and a stop in that window would tear the driver
     * down just as the hopper woke up. */
    s_hop_alive = true;
    if (xTaskCreatePinnedToCore(hop_task, "pharos_hop", 3072, NULL, 5, NULL, 0) != pdPASS) {
        s_hop_alive = false;
        s.running = false;
        ESP_LOGE(TAG, "could not create the hop task");
        esp_wifi_set_promiscuous(false);
        esp_wifi_stop();
        esp_wifi_deinit();
        return false;
    }
    ESP_LOGI(TAG, "rx started: %u channels, %u ms dwell, %s",
             plan->n_channels, plan->dwell_ms, s.camped ? "camped" : "hopping");
    return true;
}

void pharos_radio_rx_stop(void)
{
    if (!s.running && !s_hop_alive) {
        return;
    }
    /* Ask the hopper to finish, then WAIT for it. Tearing down the Wi-Fi
     * driver while a task is still calling esp_wifi_set_channel() is a use
     * after free with extra steps. The hopper checks s.running once per dwell,
     * so the wait is bounded by the longest dwell we use plus slack. */
    s.running = false;
    for (int i = 0; i < 200 && s_hop_alive; i++) {
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (s_hop_alive) {
        ESP_LOGW(TAG, "hop task did not exit in 2s; tearing down anyway");
    }

    esp_wifi_set_promiscuous(false);
    esp_wifi_set_promiscuous_rx_cb(NULL);
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
/* ---- BLE observer ---------------------------------------------------
 *
 * PASSIVE scanning, and that word is load-bearing.
 *
 * An ACTIVE BLE scan transmits: for every advertisement it hears it sends a
 * SCAN_REQ back to ask for the scan response. That is a radio transmission,
 * it is attributable to this device, and it would break the single promise
 * the whole project is built on. ble_gap_disc_params.passive = 1 is therefore
 * not a tuning choice - it is the fence, expressed in the one place BLE could
 * otherwise emit. Pharos hears advertisements and never answers them.
 *
 * NimBLE is additionally compiled observer-only (see Kconfig), so the
 * advertising, peripheral and central code is not in the image at all. This
 * is the belt to that's braces. */
#if !defined(PHAROS_HOST)

static pharos_bus_t *s_ble_bus;
static bool s_ble_running;

static int ble_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;
    if (!event || event->type != BLE_GAP_EVENT_DISC || !s_ble_bus) {
        return 0;
    }
    const struct ble_gap_disc_desc *d = &event->disc;

    pharos_event_t ev;
    memset(&ev, 0, sizeof(ev));
    ev.t_us = (uint64_t)esp_timer_get_time();
    ev.type = PHAROS_EV_BLE_ADV;
    memcpy(ev.u.ble.addr, d->addr.val, 6);
    ev.u.ble.addr_type = d->addr.type;
    ev.u.ble.adv_type = d->event_type;
    ev.u.ble.rssi = (int8_t)d->rssi;
    uint8_t n = d->length_data;
    if (n > PHAROS_BLE_ADV_MAX) {
        n = PHAROS_BLE_ADV_MAX;
    }
    ev.u.ble.data_len = n;
    if (n && d->data) {
        memcpy(ev.u.ble.data, d->data, n);
    }

    s.stats.ble_reports++;
    pharos_bus_push(s_ble_bus, &ev);
    return 0;
}

static void ble_on_sync(void)
{
    struct ble_gap_disc_params p;
    memset(&p, 0, sizeof(p));
    p.passive = 1;           /* THE FENCE: never send SCAN_REQ. See above. */
    p.filter_duplicates = 0; /* we want repeats: persistence is the signal */
    p.itvl = 0x0060;         /* ~60 ms */
    p.window = 0x0030;       /* ~30 ms - a 50% duty listen              */
    const int rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, BLE_HS_FOREVER, &p,
                                ble_gap_event, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_disc failed: %d", rc);
    } else {
        ESP_LOGI(TAG, "ble observer up (passive - transmits nothing)");
    }
}

static void ble_host_task(void *param)
{
    (void)param;
    nimble_port_run();
    nimble_port_freertos_deinit();
}
#endif /* !PHAROS_HOST */

bool pharos_radio_ble_scan_start(pharos_bus_t *bus)
{
    if (!(pharos_lens_active_caps() & PHAROS_CAP_BLE_SCAN)) {
        return false;
    }
#if defined(PHAROS_HOST)
    (void)bus;
    return false;
#else
    if (!bus) {
        return false;
    }
    s_ble_bus = bus;
    if (s_ble_running) {
        return true;
    }
    const esp_err_t err = nimble_port_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(err));
        return false;
    }
    ble_hs_cfg.sync_cb = ble_on_sync;
    nimble_port_freertos_init(ble_host_task);
    s_ble_running = true;
    return true;
#endif
}

void pharos_radio_ble_scan_stop(void)
{
#if !defined(PHAROS_HOST)
    if (!s_ble_running) {
        return;
    }
    ble_gap_disc_cancel();
    s_ble_bus = NULL;
    /* The NimBLE host stays up: tearing the controller down and back up costs
     * seconds and this lens is switched in and out constantly. Cancelling the
     * scan is what actually stops the radio listening. */
    ESP_LOGI(TAG, "ble observer stopped");
#endif
}
