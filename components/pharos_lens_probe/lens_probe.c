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
#include "pharos_survey.h"
#include "pharos_survey_hook.h"
#include "pharos_probe.h"
#include "pharos_radio.h"
#include "pharos_report.h"


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
    /* The named networks a phone asks for ARE the privacy leak this lens
     * exists to show; without them every probe looked like a wildcard and the
     * exposure grade was always the floor.
     *
     * The fingerprint stays coarse - a full IE-order hash needs the element
     * chain, and this one is built from what the summary carries. It is enough
     * to separate devices that differ, not enough to claim two probes came from
     * the same handset, so the engine treats it as corroboration only. */
    p.fingerprint = ((uint32_t)f->flags << 8) ^ f->rate_idx ^ 0xA53Cu;
    if (f->ssid_len) {
        const uint8_t n = f->ssid_len > PP_SSID_MAX ? PP_SSID_MAX : f->ssid_len;
        memcpy(p.ssid, f->ssid, n);
        p.ssid[n] = '\0';
        p.ssid_len = n;
    }

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


/* ---- what Probe actually knows ---------------------------------------
 *
 * Probe had no display at all, so the screen showed a frame counter while the
 * engine sat on a table of devices and the network names each one was
 * shouting. The headline is the WORST-exposed device, because the point of
 * this lens is that somebody in the room is broadcasting where they live and
 * work to anyone with an antenna.
 *
 * The detail page names them. That is uncomfortable, and it is the entire
 * value: "a device here leaks 6 networks" is an abstraction nobody acts on,
 * and seeing your own home network's name on a stranger's screen is not. */
static bool k_probe_display(struct pharos_lens_display *o)
{
    if (!s_lock || xSemaphoreTake(s_lock, 0) != pdTRUE) {
        return false;
    }
    const unsigned n = s_engine.n_devices;
    pp_verdict_t worst;
    memset(&worst, 0, sizeof(worst));
    unsigned leaky = 0;
    for (unsigned i = 0; i < n; i++) {
        pp_verdict_t v;
        pp_grade_device(&s_engine.devices[i], &v);
        if (v.networks) {
            leaky++;
        }
        if (v.exposure > worst.exposure) {
            worst = v;
        }
    }
    xSemaphoreGive(s_lock);

    if (!n) {
        snprintf(o->big, sizeof(o->big), "--");
        snprintf(o->band, sizeof(o->band), "listening");
        snprintf(o->detail, sizeof(o->detail), "no probe requests yet");
        snprintf(o->advice, sizeof(o->advice), "Phones probe when unlocked.");
        o->has_score = false;
        return true;
    }
    snprintf(o->big, sizeof(o->big), "%u", worst.exposure);
    snprintf(o->band, sizeof(o->band), "%s", pp_grade_name(worst.grade));
    snprintf(o->detail, sizeof(o->detail), "%u devices  %u leaking names", n, leaky);
    snprintf(o->advice, sizeof(o->advice), "%s",
             leaky ? "Names announced to everyone." : "Nothing named yet.");
    if (worst.networks) {
        snprintf(o->why, sizeof(o->why), "worst leaks %u network names",
                 (unsigned)worst.networks);
    }
    o->score = worst.exposure;
    o->ceiling = 100;
    o->has_score = true;

    /* FEED THE SESSION SURVEY. Deduplicated by address inside the survey, so
     * pushing the whole table each time is correct and simple. A device that
     * named nothing is not a leaky device and is filtered there. */
    /* ONCE A SECOND, NOT TEN TIMES.
     *
     * display() is called at the repaint rate, and walking the whole table to
     * push facts the survey has already deduplicated is work with no output.
     * The survey only needs to have heard each thing once; a second is far
     * inside the rotation's own dwell, so nothing is missed. */
    static uint64_t s_fed_us;
    const uint64_t feed_now = (uint64_t)esp_timer_get_time();
    if (feed_now - s_fed_us >= 1000000ull) {
        s_fed_us = feed_now;
    for (unsigned i = 0; i < s_engine.n_devices; i++) {
        const pp_device_t *d = &s_engine.devices[i];
        if (!d->in_use) {
            continue;
        }
        /* A locally-administered address is a private one: the bit exists to
         * stop a device being followed, and announcing remembered network
         * names undoes it completely. That pairing is the finding. */
        const bool randomised = (d->addr[0] & 0x02) != 0;
        pharos_survey_device(d->addr, d->n_networks, randomised);
    }
    }

    /* PROBE MEASURES EXPOSURE, NOT ATTACK. A phone shouting the names of the
     * networks it knows is leaking, and that is worth a dot on the ring - but
     * it is the phone's own behaviour, not somebody doing something to it. See
     * pharos_lens_display::alert. */
    o->has_alert = true;
    o->alert = leaky ? 1u : 0u;
    return true;
}

/* One row per device, then the networks that device named. The names are the
 * finding; a count is not. */
static bool k_probe_row(unsigned index, struct pharos_lens_row *out)
{
    if (!s_lock || xSemaphoreTake(s_lock, 0) != pdTRUE) {
        return false;
    }
    bool ok = false;
    unsigned row = 0;
    for (unsigned i = 0; i < s_engine.n_devices && !ok; i++) {
        const pp_device_t *d = &s_engine.devices[i];
        if (row == index) {
            pp_verdict_t v;
            pp_grade_device(d, &v);
            snprintf(out->left, sizeof(out->left), "%02x:%02x:%02x  %u seen",
                     d->addr[3], d->addr[4], d->addr[5], (unsigned)d->probes);
            snprintf(out->right, sizeof(out->right), "%s", pp_grade_name(v.grade));
            out->tone = (v.exposure >= 60) ? PHAROS_TONE_BAD
                      : (v.exposure >= 30) ? PHAROS_TONE_WARN : PHAROS_TONE_GOOD;
            ok = true;
            break;
        }
        row++;
        for (unsigned k = 0; k < d->n_networks && !ok; k++) {
            if (row == index) {
                /* Indented, because it belongs to the device above it. */
                snprintf(out->left, sizeof(out->left), "  \"%.18s\"", d->networks[k]);
                snprintf(out->right, sizeof(out->right), "%.11s",
                         pp_place_name((pp_place_t)d->places[k]));
                out->tone = PHAROS_TONE_WARN;
                ok = true;
                break;
            }
            row++;
        }
    }
    xSemaphoreGive(s_lock);
    return ok;
}

static const pharos_lens_t k_probe = {
    .id = "wifi.probe",
    .purpose = "what phones leak",
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
    .display = k_probe_display,
    .row = k_probe_row,
    .row_head_left = "DEVICE / NETWORK",
    .row_head_right = "GRADE",
};

PHAROS_LENS_REGISTER(&k_probe);
