/* Pharos lens: Roster - every device here, and what is exposed
 *
 * The inventory question, answered by listening. See pharos_roster.h for why
 * this is passive, what "vulnerable" is allowed to mean, and how the CVE half
 * of the job leaves the device.
 *
 * This lens is plumbing: it runs both radios in turn, feeds every frame and
 * advertisement to the engine, and hands the UI a snapshot. All the judgement
 * is in pharos_roster.c, which is pure and host-tested.
 */
#include <stdio.h>
#include <string.h>

#include "esp_attr.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#include "pharos_bus.h"
#include "pharos_pulse.h"
#include "pharos_lens.h"
#include "pharos_radio.h"
#include "pharos_roster.h"

static const char *TAG = "lens.roster";

#define ROSTER_RING 512

EXT_RAM_BSS_ATTR static pharos_event_t s_slots[ROSTER_RING];
static pharos_bus_t s_bus;
EXT_RAM_BSS_ATTR static rd_roster_t s_roster;
static SemaphoreHandle_t s_lock;

static bool roster_mount(void)
{
    if (!s_lock) {
        s_lock = xSemaphoreCreateMutex();
    }
    rd_reset(&s_roster);
    return s_lock && pharos_bus_init(&s_bus, s_slots, ROSTER_RING);
}

static bool roster_start(void)
{
    /* Wi-Fi across the whole plan plus BLE: an inventory is the one job that
     * genuinely wants both radios, because half the things in a room only
     * exist on one of them. */
    pharos_scan_plan_t plan = pharos_scan_plan_survey();
    plan.want_mgmt = true;
    plan.want_data = true;
    const bool wifi = pharos_radio_rx_start(&plan, &s_bus);
    const bool ble = pharos_radio_ble_scan_start(&s_bus);
    if (!ble) {
        ESP_LOGW(TAG, "BLE scan would not start; Wi-Fi devices only");
    }
    return wifi;
}

static void roster_stop(void)
{
    pharos_radio_ble_scan_stop();
    pharos_radio_rx_stop();
}

/* Analytics core.
 *
 * THIS PATH TAKES THE LOCK, AND IT IS THE ONE THAT MOST NEEDS TO.
 *
 * Every other lens here has one writer: events accumulate on the analytics
 * core and the UI core only ever reads a snapshot. Roster does not. It expires
 * devices on a timer, and roster_tick() runs on the UI task - so rd_expire()
 * is a second writer, compacting the very table rd_observe_*() is inserting
 * into, on a different core, with nothing between them.
 *
 * The mutex existed from the first version of this file and was never taken by
 * anything. It works now. The timeout on this side is short and a miss simply
 * drops the observation: losing one advertisement out of thousands is free,
 * and blocking the analytics core is not. */
/* The shared activity ribbon: one call per event in, one call per repaint
 * out. Before this, every lens but Watch drew an empty timeline. */
static pharos_pulse_t s_pulse;

static void roster_event(const pharos_event_t *ev)
{
    if (!ev) {
        return;
    }

    pharos_pulse_note(&s_pulse, ev->t_us);
    if (!s_lock || xSemaphoreTake(s_lock, pdMS_TO_TICKS(2)) != pdTRUE) {
        return;
    }
    if (ev->type == PHAROS_EV_DOT11) {
        const pharos_ev_dot11_t *d = &ev->u.dot11;
        const bool is_beacon = (d->type == PHAROS_FT_MGMT &&
                                (d->subtype == PHAROS_ST_BEACON ||
                                 d->subtype == PHAROS_ST_PROBE_RESP));
        const bool is_probe = (d->type == PHAROS_FT_MGMT &&
                               d->subtype == PHAROS_ST_PROBE_REQ);

        char ssid[PHAROS_EV_SSID_MAX + 1];
        const uint8_t n = d->ssid_len > PHAROS_EV_SSID_MAX ? PHAROS_EV_SSID_MAX
                                                           : d->ssid_len;
        memcpy(ssid, d->ssid, n);
        ssid[n] = '\0';

        if (is_beacon) {
            /* The security posture comes straight out of the frame. The
             * engine turns it into exposure flags; the lens does not judge. */
            const rd_secpost_t sec = {
                .privacy = (d->rsn_flags & PHAROS_RSN_F_PRESENT) != 0 ||
                           (d->rsn_flags != 0),
                .wpa1 = false,
                .tkip = false,
                .rsn_flags = d->rsn_flags,
            };
            rd_observe_wifi(&s_roster, d->a3, true, ssid, &sec, d->channel,
                            d->rssi, NULL, ev->t_us);
        } else if (is_probe) {
            rd_observe_wifi(&s_roster, d->a2, false, NULL, NULL, d->channel,
                            d->rssi, n ? ssid : NULL, ev->t_us);
        } else {
            /* Any other frame still proves the transmitter is here, which is
             * most of what an inventory needs. */
            rd_observe_wifi(&s_roster, d->a2, false, NULL, NULL, d->channel,
                            d->rssi, NULL, ev->t_us);

            /* And a DATA frame carries the one thing a camera cannot hide -
             * that it is uploading, continuously. Only the envelope's size and
             * direction are used; the body is ciphertext on any protected
             * network. See rd_device_t's note. */
            if (d->type == PHAROS_FT_DATA && d->frame_len) {
                const bool to_ap = (d->flags & PHAROS_DOT11_F_TO_DS) != 0;
                rd_observe_traffic(&s_roster, d->a2, d->frame_len, to_ap,
                                   ev->t_us);
            }
        }
    } else if (ev->type == PHAROS_EV_WPS) {
        /* The access point's own word about what it is - see pharos_wps.h. */
        rd_observe_wps(&s_roster, ev->u.wps.bssid, ev->u.wps.vendor,
                       ev->u.wps.model, ev->u.wps.pin_exposed, ev->t_us);
    } else if (ev->type == PHAROS_EV_BLE_ADV) {
        const pharos_ev_ble_t *b = &ev->u.ble;
        rd_observe_ble(&s_roster, b->addr, b->addr_type != 0, NULL, 0, NULL, 0,
                       b->rssi, ev->t_us);
    }
    xSemaphoreGive(s_lock);
}

static struct pharos_bus *roster_ingest(void) { return &s_bus; }

static void roster_tick(uint32_t dt_ms)
{
    (void)dt_ms;
    static uint32_t acc;
    acc += dt_ms;
    if (acc < 5000u) {
        return;
    }
    acc = 0;
    /* The second writer. Never concurrently with the first, now. */
    if (s_lock && xSemaphoreTake(s_lock, pdMS_TO_TICKS(5)) == pdTRUE) {
        rd_expire(&s_roster, (uint64_t)esp_timer_get_time());
        xSemaphoreGive(s_lock);
    }
}

static bool k_roster_display(struct pharos_lens_display *o)
{
    if (!s_lock || xSemaphoreTake(s_lock, 0) != pdTRUE) {
        return false;
    }
    const unsigned n = rd_count(&s_roster);
    const unsigned bad = rd_exposed_count(&s_roster);
    rd_device_t worst;
    const bool have_worst = rd_at(&s_roster, 0, &worst);
    /* Whether anything actionable is in the list, decided while the lock is
     * held rather than by walking the table again afterwards. */
    bool actionable = false;
    {
        rd_device_t d;
        for (unsigned i = 0; rd_at(&s_roster, i, &d); i++) {
            if (d.exposure & (RD_EXP_OPEN | RD_EXP_WEP | RD_EXP_WPS_PIN)) {
                actionable = true;
                break;
            }
        }
    }
    xSemaphoreGive(s_lock);

    snprintf(o->big, sizeof(o->big), "%u", n);
    snprintf(o->band, sizeof(o->band), "%s", n == 1u ? "device here" : "devices here");
    if (!n) {
        snprintf(o->detail, sizeof(o->detail), "%s", "listening on both radios");
        snprintf(o->advice, sizeof(o->advice), "%s",
                 "Nothing has announced itself yet.");
        o->has_score = false;
        return true;
    }
    snprintf(o->detail, sizeof(o->detail), "%u with something exposed", bad);

    if (have_worst && worst.exposure) {
        const char *h = rd_exposure_headline(&worst);
        snprintf(o->why, sizeof(o->why), "%.47s", h ? h : "");
        snprintf(o->advice, sizeof(o->advice), "%.40s - tap for the list",
                 worst.vendor ? worst.vendor : rd_class_name(worst.klass));
    } else {
        snprintf(o->advice, sizeof(o->advice), "%s",
                 "Nothing here is advertising a weakness.");
    }

    /* The gauge is the SHARE of devices with something exposed, which is a
     * real 0..100 with a meaning - not a threat score. A room full of clean
     * devices reads zero and a room where everything is open reads 100. */
    o->score = (uint8_t)((bad * 100u) / n);
    o->ceiling = 100;
    o->has_score = true;

    /* WHAT THIS MEANS ON THE HOME RING, WHICH IS NOT WHAT THE NUMBER SAYS.
     *
     * The gauge is the share of devices with something exposed, and in an
     * ordinary street that is most of them - almost no domestic access point
     * enables 802.11w. Read as a threat level it would put the home screen at
     * ALARM permanently, about other people's routers, and a device that
     * shouts every second of every day is one somebody stops looking at.
     *
     * None of this is an attack in progress. It is a survey of how well the
     * neighbourhood is configured, so it caps at "worth knowing" - and only
     * rises that far when something genuinely actionable is in the list, not
     * merely because the percentage is high. */
    o->has_alert = true;
    o->alert = actionable ? 1u : 0u;
    o->has_history = pharos_pulse_fill(&s_pulse, (uint64_t)esp_timer_get_time(), o->history);
    return true;
}

static bool k_roster_row(unsigned index, struct pharos_lens_row *out)
{
    rd_device_t d;
    if (!s_lock || xSemaphoreTake(s_lock, 0) != pdTRUE) {
        return false;
    }
    const bool got = rd_at(&s_roster, index, &d);
    xSemaphoreGive(s_lock);
    if (!got) {
        return false;
    }
    /* Vendor and class first - that is what somebody is scanning the list for.
     * The name, when one leaked, is more identifying than either. */
    if (d.model[0]) {
        /* The exact model beats everything else: it is what somebody can look
         * up, and it is the device's own statement rather than our guess. */
        snprintf(out->left, sizeof(out->left), "%.4s %.19s",
                 rd_class_icon(d.klass), d.model);
    } else if (d.name[0]) {
        snprintf(out->left, sizeof(out->left), "%.4s %.19s",
                 rd_class_icon(d.klass), d.name);
    } else if (d.vendor) {
        snprintf(out->left, sizeof(out->left), "%.4s %.13s %02x%02x",
                 rd_class_icon(d.klass), d.vendor, d.mac[4], d.mac[5]);
    } else {
        snprintf(out->left, sizeof(out->left), "%.4s %02x:%02x:%02x:%02x",
                 rd_class_icon(d.klass), d.mac[2], d.mac[3], d.mac[4], d.mac[5]);
    }

    if (d.exposure) {
        /* Name the worst thing, not a count - "WEP" tells somebody what to do
         * and "3 issues" does not. The TAG, not the sentence: the column is
         * eleven characters and a truncated sentence reads as a broken
         * display rather than a finding. */
        const char *t = rd_exposure_tag(&d);
        snprintf(out->right, sizeof(out->right), "%.11s", t ? t : "exposed");
        out->tone = (d.exposure &
                     (RD_EXP_STREAMING | RD_EXP_OPEN | RD_EXP_WEP | RD_EXP_WPA1))
                        ? PHAROS_TONE_BAD
                        : PHAROS_TONE_WARN;
    } else {
        snprintf(out->right, sizeof(out->right), "%d dBm", (int)d.rssi);
        out->tone = PHAROS_TONE_GOOD;
    }
    return true;
}

/* Opening a device: everything known about it, then the checklist for its
 * kind. This is where the CWE half lives - stated as things to verify about a
 * class, never as a claim about this unit. */
static bool k_roster_expand(unsigned row, unsigned sub,
                            struct pharos_lens_row *out)
{
    rd_device_t d;
    if (!s_lock || xSemaphoreTake(s_lock, 0) != pdTRUE) {
        return false;
    }
    const bool got = rd_at(&s_roster, row, &d);
    xSemaphoreGive(s_lock);
    if (!got) {
        return false;
    }
    switch (sub) {
    case 0:
        snprintf(out->left, sizeof(out->left), "looks like");
        snprintf(out->right, sizeof(out->right), "%.11s", rd_class_name(d.klass));
        out->tone = PHAROS_TONE_NEUTRAL;
        return true;
    case 1:
        if (d.model[0]) {
            snprintf(out->left, sizeof(out->left), "model  (it says)");
            snprintf(out->right, sizeof(out->right), "%.11s", d.model);
            out->tone = PHAROS_TONE_NEUTRAL;
            return true;
        }
        snprintf(out->left, sizeof(out->left), "made by");
        snprintf(out->right, sizeof(out->right), "%.11s",
                 d.vendor ? d.vendor : "unknown");
        out->tone = d.vendor ? PHAROS_TONE_NEUTRAL : PHAROS_TONE_DIM;
        return true;
    case 2:
        snprintf(out->left, sizeof(out->left), "address");
        snprintf(out->right, sizeof(out->right), "%02x:%02x:%02x", d.mac[3],
                 d.mac[4], d.mac[5]);
        out->tone = PHAROS_TONE_DIM;
        return true;
    case 3:
        snprintf(out->left, sizeof(out->left), "address is");
        snprintf(out->right, sizeof(out->right), "%s",
                 d.randomised_mac ? "random" : "FIXED");
        /* Randomised is the good case, and it is worth a green tick so
         * somebody can see which of their devices are protecting themselves. */
        out->tone = d.randomised_mac ? PHAROS_TONE_GOOD : PHAROS_TONE_WARN;
        return true;
    case 4:
        snprintf(out->left, sizeof(out->left), "heard on");
        snprintf(out->right, sizeof(out->right), "%s",
                 (d.seen == (RD_SEEN_WIFI | RD_SEEN_BLE)) ? "wifi+BLE"
                 : (d.seen & RD_SEEN_BLE)                 ? "BLE"
                                                          : "wifi");
        out->tone = PHAROS_TONE_DIM;
        return true;
    case 5:
        snprintf(out->left, sizeof(out->left), "signal / seen");
        snprintf(out->right, sizeof(out->right), "%d / %u", (int)d.rssi,
                 (unsigned)d.sightings);
        out->tone = PHAROS_TONE_DIM;
        return true;
    case 6: {
        const char *h = rd_exposure_headline(&d);
        snprintf(out->left, sizeof(out->left), "%.25s",
                 h ? h : "nothing exposed on air");
        snprintf(out->right, sizeof(out->right), "%s", h ? "FIX" : "ok");
        out->tone = h ? PHAROS_TONE_BAD : PHAROS_TONE_GOOD;
        return true;
    }
    default: {
        /* The checklist. Headed by a line saying exactly what it is, because
         * a bare list of CWEs under a device name reads as "this device has
         * these", which would be a fabrication. */
        const unsigned k = sub - 7u;
        if (k == 0) {
            snprintf(out->left, sizeof(out->left), "for this KIND, check:");
            out->right[0] = '\0';
            out->tone = PHAROS_TONE_DIM;
            return true;
        }
        const char *cwes[6];
        const unsigned n = rd_class_cwes(d.klass, cwes, 6);
        if (k - 1u >= n) {
            return false;
        }
        snprintf(out->left, sizeof(out->left), "%.25s", cwes[k - 1u]);
        snprintf(out->right, sizeof(out->right), "check");
        out->tone = PHAROS_TONE_WARN;
        return true;
    }
    }
}

unsigned pharos_lens_roster_export(char *buf, unsigned cap, bool redact)
{
    return rd_export(&s_roster, buf, cap, redact);
}

unsigned pharos_lens_roster_count(void)
{
    if (!s_lock || xSemaphoreTake(s_lock, 0) != pdTRUE) {
        return 0;
    }
    const unsigned n = rd_count(&s_roster);
    xSemaphoreGive(s_lock);
    return n;
}

static const pharos_lens_t k_roster = {
    .id = "net.roster",
    .purpose = "every device here",
    .name = "Roster",
    .summary = "Every device in range, what it is, and what it leaves exposed",
    .glyph = "list",
    .kind = PHAROS_LENS_OBSERVE,
    .caps = PHAROS_CAP_WIFI_RX | PHAROS_CAP_WIFI_CHAN | PHAROS_CAP_BLE_SCAN,
    .budget_ma = 150,
    .on_mount = roster_mount,
    .on_start = roster_start,
    .on_stop = roster_stop,
    .on_tick = roster_tick,
    .on_event = roster_event,
    .ingest = roster_ingest,
    .display = k_roster_display,
    .row = k_roster_row,
    .row_head_left = "DEVICE",
    .row_head_right = "EXPOSURE",
    .row_expand = k_roster_expand,
};

PHAROS_LENS_REGISTER(&k_roster);
