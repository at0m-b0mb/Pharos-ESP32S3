/* Pharos - console serial glue
 *
 * Wires the pure, host-tested command console (pharos_console) to the live
 * firmware and pumps it from a serial task. Type commands over USB; they drive
 * the real lenses. The console engine itself has no way to transmit - see
 * pharos_console.h - so this surface is receive-only like everything else.
 *
 * Kept deliberately small: it routes the safe, always-available operations
 * (activate a lens, stop, region, prove the fence, camp Watch, aim Locate) and
 * reports the active lens for status. Per-lens verdict/report plumbing over the
 * console is a later nicety; the dial already shows those.
 */
#include <stdio.h>
#include <string.h>

#include "esp_console.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_system.h"
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "pharos_bsp.h"
#include "pharos_sense.h"
#include "pharos_audio.h"
#include "pharos_hud.h"
#include "pharos_survey.h"
#include "pharos_tower.h"

unsigned pharos_lens_roster_export(char *buf, unsigned cap, bool redact);
unsigned pharos_lens_roster_count(void);
#include "pharos_survey_hook.h"
#include "pharos_ui.h"

#include "pharos_console.h"
#include "pharos_lens.h"
#include "pharos_radio.h"
#include "pharos_region.h"
#include "pharos_aegis.h"
#include "pharos_harvest.h"
#include "pharos_sentinel.h"
#include "pharos_squall.h"
#include "pharos_vigil.h"

/* Provided by the Watch, Locate and Sentinel lens components. */
extern void pharos_lens_watch_camp(uint8_t channel);
extern void pharos_lens_watch_survey(void);
extern void pharos_lens_locate_set_target(const uint8_t bssid[6], uint8_t channel);
extern unsigned pharos_lens_sentinel_adopt(void);
extern bool pharos_lens_sentinel_snapshot(ps_verdict_t *out);
extern bool pharos_lens_harvest_snapshot(ph_verdict_t *out);
extern bool pharos_lens_aegis_snapshot(pa_verdict_t *out);
extern void pharos_lens_aegis_acknowledge(void);
extern bool pharos_lens_squall_snapshot(pq_verdict_t *out);
extern bool pharos_lens_vigil_snapshot(pv_verdict_t *out);
extern bool pharos_lens_vigil_mark_known(void);

/* The console FILES a request rather than switching the lens itself. Doing it
 * here would run Wi-Fi initialisation on the REPL task's small stack, and would
 * race the analytics loop that is walking the current lens. See
 * pharos_ui_request_lens(). */
static bool glue_activate(const char *id) { return pharos_ui_request_lens(id); }
static void glue_deactivate(void) { pharos_ui_request_stop(); }

static void glue_set_channel(int channel)
{
    const pharos_lens_t *a = pharos_lens_active();
    if (a && strcmp(a->id, "wifi.watch") == 0) {
        if (channel < 0) pharos_lens_watch_survey();
        else pharos_lens_watch_camp((uint8_t)channel);
    }
}

static void glue_set_target(const uint8_t bssid[6])
{
    pharos_lens_locate_set_target(bssid, 0);
}

static void glue_set_region(int region)
{
    pharos_region_set((pharos_region_t)region);
}

static unsigned glue_adopt_baseline(void)
{
    return pharos_lens_sentinel_adopt();
}

static bool glue_mark_known(void)
{
    return pharos_lens_vigil_mark_known();
}

static void glue_acknowledge(void)
{
    pharos_lens_aegis_acknowledge();
}

static void glue_status(pc_out_t *o)
{
    const pharos_lens_t *a = pharos_lens_active();
    if (!a) {
        pc_println(o, "  idle - nothing active");
        return;
    }
    /* Sentinel earns a real status line: the diff against the baseline is the
     * whole point of the lens, so `status` should show it rather than just the
     * lens id. */
    if (strcmp(a->id, "wifi.sentinel") == 0) {
        ps_verdict_t v;
        if (pharos_lens_sentinel_snapshot(&v)) {
            pc_printf(o, "  sentinel: %s %u/%u  new=%u down=%u miss=%u moved=%u\n",
                      ps_band_name(v.band), v.score, v.ceiling,
                      v.n_new, v.n_downgrade, v.n_missing, v.n_moved);
            pc_printf(o, "  %s\n", v.headline ? v.headline : "");
            return;
        }
    }
    if (strcmp(a->id, "wifi.harvest") == 0) {
        ph_verdict_t v;
        if (pharos_lens_harvest_snapshot(&v)) {
            pc_printf(o, "  harvest: %s %u/%u  forced=%u pmkid=%u victims=%u\n",
                      ph_band_name(v.band), v.score, v.ceiling,
                      (unsigned)v.forced_cycles, (unsigned)v.pmkid_orphans,
                      (unsigned)v.victims);
            pc_printf(o, "  %s\n", v.headline ? v.headline : "");
            return;
        }
    }
    if (strcmp(a->id, "wifi.squall") == 0) {
        pq_verdict_t v;
        if (pharos_lens_squall_snapshot(&v)) {
            pc_printf(o, "  squall: %s ch%u %u/%u  graded=%u denial=%u retry=%u%%\n",
                      pq_state_name(v.worst), v.worst_channel, v.score, v.ceiling,
                      v.n_graded, v.n_denial, v.retry_permil / 10);
            pc_printf(o, "  %s\n", v.headline ? v.headline : "");
            return;
        }
    }
    if (strcmp(a->id, "ble.vigil") == 0) {
        pv_verdict_t v;
        if (pharos_lens_vigil_snapshot(&v)) {
            pc_printf(o, "  vigil: %s %u/%u  tags=%u following=%u places=%u\n",
                      pv_band_name(v.band), v.score, v.ceiling,
                      v.n_tags, v.n_following, v.n_locales);
            if (v.n_following) {
                pc_printf(o, "  worst: %s in %u places, %u min\n",
                          pv_kind_name(v.worst_kind), v.worst_locales,
                          (unsigned)v.worst_minutes);
            }
            pc_printf(o, "  %s\n", v.headline ? v.headline : "");
            return;
        }
    }
    if (strcmp(a->id, "sys.aegis") == 0) {
        pa_verdict_t v;
        if (pharos_lens_aegis_snapshot(&v)) {
            pc_printf(o, "  aegis: %s %u/%u  raised=%u live=%u\n",
                      pa_band_name(v.band), v.score, v.ceiling,
                      v.n_raised, v.n_live);
            if (v.worst_peak) {
                pc_printf(o, "  worst: %s %u, %us ago%s\n",
                          pa_stage_name(v.worst), v.worst_peak,
                          (unsigned)v.worst_age_s,
                          (v.notes & PA_NOTE_LATCHED) ? " (latched)" : "");
            }
            pc_printf(o, "  %s\n", v.headline ? v.headline : "");
            return;
        }
    }
    char caps[64];
    pharos_caps_describe(a->caps, caps, sizeof(caps));
    pc_printf(o, "  active: %s [%s]\n", a->id, caps);
}

static void glue_fence(pc_out_t *o)
{
    pharos_tx_fence_t f;
    pharos_radio_fence_status(&f);
    const bool clean = f.wrap_linked && f.ble_observer_only &&
                       f.tx_symbols_absent && f.tx_attempts == 0;
    pc_printf(o, "  %s - wrap=%d ble_obs=%d tx_absent=%d trips=%u\n",
              clean ? "clean" : "BREACH", f.wrap_linked, f.ble_observer_only,
              f.tx_symbols_absent, (unsigned)f.tx_attempts);
}

static pc_ops_t build_ops(void)
{
    pc_ops_t o;
    memset(&o, 0, sizeof(o));
    o.activate_lens = glue_activate;
    o.deactivate = glue_deactivate;
    o.set_channel = glue_set_channel;
    o.set_target = glue_set_target;
    o.set_region = glue_set_region;
    o.status_line = glue_status;
    o.fence_status = glue_fence;
    o.adopt_baseline = glue_adopt_baseline;
    o.acknowledge = glue_acknowledge;
    o.mark_known = glue_mark_known;
    /* report / select_scenario intentionally NULL here - the console reports
     * them as "not wired on this build" rather than pretending. */
    return o;
}

void pharos_console_run(const char *line)
{
    pc_ops_t ops = build_ops();
    static pc_out_t out; /* static: keep it off the task stack (1 KB) */
    pc_exec(&ops, line, &out);
    if (out.len) {
        fputs(out.buf, stdout);
    }
    fflush(stdout);
}

/* ---- the CLI -----------------------------------------------------------
 *
 * Built on ESP-IDF's esp_console REPL rather than a hand-rolled getchar()
 * loop. That buys line editing, history, tab completion and - the reason it
 * matters here - a transport that is correct on whichever console this build
 * uses. The previous loop polled getchar(), which behaves differently on UART
 * and USB-Serial-JTAG and gave no editing at all.
 *
 * Every command in the host-tested table is registered automatically, so the
 * CLI and the dial can never drift apart, and `help` lists the real thing. */

static int cli_forward(int argc, char **argv)
{
    /* Rebuild the line and hand it to the tested dispatcher, rather than
     * duplicating argument handling here. */
    char line[PC_MAX_LINE];
    size_t n = 0;
    for (int i = 0; i < argc && n + 1 < sizeof(line); i++) {
        const int w = snprintf(line + n, sizeof(line) - n, "%s%s",
                               i ? " " : "", argv[i]);
        if (w < 0) {
            break;
        }
        n += (size_t)w;
        if (n >= sizeof(line)) {
            n = sizeof(line) - 1;
            break;
        }
    }
    line[n] = '\0';
    pharos_console_run(line);
    return 0;
}

/* `diag` - why is the screen black? The device should be able to answer that
 * itself rather than making somebody read a boot log they may not have kept. */
static int cli_diag(int argc, char **argv)
{
    (void)argc; (void)argv;
    pharos_bsp_status_t st;
    memset(&st, 0, sizeof(st));
    pharos_bsp_last_status(&st);

    printf("display   : %s\n", pharos_disp_result_name(st.disp_result));
    printf("  size    : %dx%d\n", st.disp_w, st.disp_h);
    printf("  touch   : %s\n", st.touch_ok ? "up" : "DOWN (screen still works)");
    printf("  lock    : %s\n",
           pharos_bsp_display_lock(50) ? (pharos_bsp_display_unlock(), "acquired")
                                       : "COULD NOT ACQUIRE");
    printf("  hud     : %s\n", pharos_hud_present() ? "built" : "NOT BUILT");
    printf("alarm     : %s\n",
           pharos_audio_present()
               ? (pharos_audio_enabled() ? "ready" : "ready (MUTED)")
               : "no codec - silent");
    printf("internal  : %u KB free (DMA-capable; the display flush and the "
           "wifi driver compete for this)\n",
           (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL |
                                              MALLOC_CAP_DMA) / 1024));
    printf("psram free: %u KB\n", (unsigned)(st.psram_free / 1024));
    printf("heap free : %u B (min %u)\n",
           (unsigned)esp_get_free_heap_size(),
           (unsigned)esp_get_minimum_free_heap_size());

    const pharos_lens_t *a = pharos_lens_active();
    printf("lens      : %s (%u registered)\n", a ? a->id : "none",
           pharos_lens_count());

    pharos_radio_stats_t rs;
    pharos_radio_stats(&rs);
    printf("radio     : ch%u %s seen=%u dropped=%u\n",
           rs.current_channel, rs.camped ? "camped" : "hopping",
           (unsigned)rs.frames_seen, (unsigned)rs.frames_dropped);

    pharos_tx_fence_t f;
    pharos_radio_fence_status(&f);
    printf("fence     : %s (trips=%u)\n",
           (f.wrap_linked && f.ble_observer_only && f.tx_symbols_absent &&
            f.tx_attempts == 0) ? "clean - receive-only" : "BREACH",
           (unsigned)f.tx_attempts);
    return 0;
}

/* `rotate` - which way is up on a round device is the operator's call. */
/* Drive the screen exactly as a finger would.
 *
 * The touch zones are the only way to reach several views, which made them the
 * only paths that could not be exercised without a hand on the glass - and so
 * the only ones whose bugs reached the operator first. This files the SAME
 * intent the touch callback files, applied by the same UI task on the same
 * tick, so what is tested here is what a tap actually does. */
/* Print exactly what the DETAIL page is showing.
 *
 * The screen is the product, and until now the only way to check what was on
 * it was to photograph it. This asks the active lens for the same rows the HUD
 * asks for, through the same callback, so a wrong column or a truncated name
 * shows up in a log instead of in somebody's hand. */
/* The alarm: hear it, mute it, set how loud. `alarm test` walks the whole
 * vocabulary so the five sounds can be told apart deliberately rather than
 * discovered one at a time during an incident. */
static int cli_alarm(int argc, char **argv)
{
    if (!pharos_audio_present()) {
        printf("no audio codec on this build/board - the device runs silent\n");
        return 1;
    }
    if (argc < 2) {
        printf("alarm: %s at %u%%\n",
               pharos_audio_enabled() ? "enabled" : "MUTED",
               (unsigned)pharos_audio_volume());
        printf("  alarm on | off | vol <0-100> | test | notice|suspect|alarm|clear|ack\n");
        return 0;
    }
    const char *w = argv[1];
    if (strcmp(w, "on") == 0)  { pharos_audio_set_enabled(true);  printf("alarm on\n"); return 0; }
    if (strcmp(w, "off") == 0) { pharos_audio_set_enabled(false); printf("alarm muted\n"); return 0; }
    if (strcmp(w, "vol") == 0) {
        if (argc < 3) { printf("vol <0-100>\n"); return 1; }
        pharos_audio_set_volume((uint8_t)atoi(argv[2]));
        printf("volume %u%%\n", (unsigned)pharos_audio_volume());
        return 0;
    }
    if (strcmp(w, "test") == 0) {
        static const struct { const char *n; pharos_alert_t a; } all[] = {
            { "notice",  PHAROS_ALERT_NOTICE },
            { "suspect", PHAROS_ALERT_SUSPECT },
            { "alarm",   PHAROS_ALERT_ALARM },
            { "clear",   PHAROS_ALERT_CLEAR },
            { "ack",     PHAROS_ALERT_ACK },
        };
        for (unsigned i = 0; i < sizeof(all) / sizeof(all[0]); i++) {
            printf("  %s\n", all[i].n);
            pharos_audio_alert(all[i].a);
            vTaskDelay(pdMS_TO_TICKS(900));
        }
        return 0;
    }
    if (strcmp(w, "notice") == 0)  { pharos_audio_alert(PHAROS_ALERT_NOTICE); return 0; }
    if (strcmp(w, "suspect") == 0) { pharos_audio_alert(PHAROS_ALERT_SUSPECT); return 0; }
    if (strcmp(w, "alarm") == 0)   { pharos_audio_alert(PHAROS_ALERT_ALARM); return 0; }
    if (strcmp(w, "clear") == 0)   { pharos_audio_alert(PHAROS_ALERT_CLEAR); return 0; }
    if (strcmp(w, "ack") == 0)     { pharos_audio_alert(PHAROS_ALERT_ACK); return 0; }
    printf("unknown: %s\n", w);
    return 1;
}

extern unsigned pharos_lens_rival_raw(unsigned index, uint8_t addr[6],
                                      char *name, size_t cap, int8_t *rssi,
                                      uint16_t *hits, uint8_t *adv,
                                      uint8_t *adv_len);
extern uint32_t pharos_lens_rival_raw_total(void);

/* Every BLE advertiser in range, named or not.
 *
 * Rival's engine only admits hardware it can CLASSIFY, which is correct but
 * makes one question unanswerable from the screen: is a device missing because
 * the classifier is wrong, or because it is not transmitting? Those want
 * completely different fixes. This prints the raw roster so the difference is
 * visible instead of guessed at. */
static int cli_ble(int argc, char **argv)
{
    (void)argc; (void)argv;
    const pharos_lens_t *l = pharos_lens_active();
    if (!l || strcmp(l->id, "rf.rival") != 0) {
        printf("start the Rival lens first: lens rf.rival\n");
        return 1;
    }
    uint8_t addr[6];
    char name[40];
    int8_t rssi;
    uint16_t hits;
    uint8_t adv[31];
    uint8_t adv_len = 0;
    unsigned n = 0;
    printf("BLE advertisers in range (%u advertisements total)\n",
           (unsigned)pharos_lens_rival_raw_total());
    for (unsigned i = 0; i < 64; i++) {
        const unsigned total = pharos_lens_rival_raw(i, addr, name, sizeof(name),
                                                     &rssi, &hits, adv, &adv_len);
        if (total == 0) {
            break;
        }
        n = total;
        printf("  %02x:%02x:%02x:%02x:%02x:%02x  %4d dBm  %4u seen  \"%s\"\n",
               addr[0], addr[1], addr[2], addr[3], addr[4], addr[5],
               (int)rssi, (unsigned)hits, name);
        /* The payload itself. Most of these advertisers are nameless because a
         * passive listener never sees a scan response, so the bytes are the
         * only thing there is to identify them by - and printing them is how a
         * signature gets found rather than guessed. */
        if (adv_len) {
            printf("        ");
            for (unsigned b = 0; b < adv_len; b++) {
                printf("%02x", adv[b]);
            }
            printf("\n");
        }
    }
    if (n == 0) {
        printf("  (nothing - the observer is receiving no advertisements)\n");
    }
    return 0;
}

static int cli_motion(int argc, char **argv)
{
    (void)argc; (void)argv;
    uint8_t st = 0;
    uint32_t steps = 0, still = 0;
    if (!pharos_ui_motion(&st, &steps, &still)) {
        printf("motion   : no IMU answered\n");
        printf("  the QMI8658 is on the shared I2C bus; the vendor BSP ships\n"
               "  no driver for it, so this probes 0x6A and 0x6B itself.\n");
        return 1;
    }
    static const char *k[] = { "unknown", "still", "walking", "moving" };
    printf("motion   : %s\n", (st < 4u) ? k[st] : "?");
    printf("  steps  : %u\n", (unsigned)steps);
    if (still) {
        printf("  still for %us\n", (unsigned)still);
    }
    printf("  travelled enough to judge \"following\": %s\n",
           pharos_ui_has_travelled(0) ? "yes" : "no");
    int32_t x = 0, y = 0, z = 0;
    if (pharos_bsp_imu_read(&x, &y, &z)) {
        printf("  raw    : %ld, %ld, %ld mg\n", (long)x, (long)y, (long)z);
    }
    return 0;
}

/* THE HONEST ROUTE TO CVEs.
 *
 * Pharos cannot reach a device to test it and cannot reach the internet to
 * look one up - it holds a transmit fence, and that fence is the whole reason
 * the device can be trusted in somebody else's building. So the inventory
 * leaves here as text, and a machine that IS allowed to talk to the network
 * and to a CVE database finishes the job.
 *
 * Redacted by default: the vendor half of each address is public and carries
 * the device class, and the host half is hashed, so the export can be pasted
 * into a ticket without naming anyone's specific hardware. */
/* The motion sensor, and whether it is there at all.
 *
 * The QMI8658 has no vendor driver on this board, so the first question is
 * simply whether it answers. Everything else is a live read - which is also
 * how the motion engine's thresholds get checked against a real hand rather
 * than against a synthesised trace. */
static int cli_imu(int argc, char **argv)
{
    (void)argc; (void)argv;
    if (!pharos_bsp_imu_present()) {
        printf("no IMU: the QMI8658 did not answer on either address\n");
        return 1;
    }
    printf("QMI8658 present. Ten samples:\n");
    for (unsigned i = 0; i < 10u; i++) {
        int32_t x = 0, y = 0, z = 0;
        if (!pharos_bsp_imu_read(&x, &y, &z)) {
            printf("  read failed\n");
            return 1;
        }
        const int32_t mag = (int32_t)(x * x + y * y + z * z);
        printf("  %5ld %5ld %5ld mg   |a|^2=%ld\n", (long)x, (long)y, (long)z,
               (long)mag);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    return 0;
}

static int cli_devices(int argc, char **argv)
{
    const bool raw = (argc >= 2 && strcmp(argv[1], "full") == 0);
    static char buf[3072];
    const unsigned n = pharos_lens_roster_export(buf, sizeof(buf), !raw);
    if (!n) {
        printf("no devices heard yet (run `lens net.roster` and give it a minute)\n");
        return 0;
    }
    printf("# vendor\tmodel\tclass\taddress\texposure  (%u devices%s)\n",
           pharos_lens_roster_count(), raw ? "" : ", redacted");
    fwrite(buf, 1, n, stdout);
    printf("# exposure bits: 1=open 2=WEP 4=WPA1 8=WPS 10=fixed-MAC "
           "20=probes-open 40=no-MFP 80=name-leak 100=WPS-PIN\n");
    printf("# models come from the AP's own WPS beacon - feed them to a CVE\n"
           "# database from a machine that is allowed to talk to one.\n");
    printf("# `devices full` for unredacted addresses\n");
    return 0;
}

static int cli_ring(int argc, char **argv)
{
    const unsigned n = pharos_ui_ring_count();
    if (argc >= 2 && strcmp(argv[1], "reset") == 0) {
        pharos_ui_ring_reset();
        printf("ring: back to defaults\n");
        return 0;
    }
    if (argc >= 2) {
        const int i = atoi(argv[1]);
        if (i < 0 || (unsigned)i >= n) {
            printf("watch out of range (0-%u)\n", n ? n - 1u : 0u);
            return 1;
        }
        if (argc >= 3) {
            const int p = atoi(argv[2]);
            for (int k = 0; k < 8; k++) {
                uint8_t cur = 1;
                pharos_ui_ring_at((unsigned)i, NULL, NULL, &cur, NULL);
                if ((int)cur == p) {
                    break;
                }
                pharos_ui_ring_cycle_period((unsigned)i);
            }
        } else if (!pharos_ui_ring_toggle((unsigned)i)) {
            printf("refused: that is the last watch armed\n");
            return 1;
        }
    }
    printf("ring: %ums a lap, rotation %s\n", (unsigned)pharos_ui_ring_lap_ms(),
           pharos_ui_ring_running() ? "running" : "PAUSED");
    for (unsigned i = 0; i < n; i++) {
        const char *name = NULL;
        bool armed = false;
        uint8_t period = 1, st = 0;
        if (!pharos_ui_ring_at(i, &name, &armed, &period, &st)) {
            break;
        }
        printf("  %2u  %-9s %-3s  every %u lap(s)\n", i, name ? name : "?",
               armed ? "on" : "off", (unsigned)period);
    }
    printf("  ring <n> toggles; ring <n> <1-%u> sets how often\n",
           (unsigned)PTW_MAX_PERIOD);
    return 0;
}

static int cli_survey(int argc, char **argv)
{
    (void)argc; (void)argv;
    psv_report_t r;
    if (!pharos_survey_read((struct psv_report *)&r)) {
        printf("no survey yet\n");
        return 1;
    }
    printf("survey: %s  (%u min%s)\n", r.headline, (unsigned)r.minutes,
           r.truncated ? ", counts are a floor" : "");
    char line[40];
    for (unsigned i = 0; i < 16 && psv_line(&r, i, line, sizeof(line)); i++) {
        printf("  %s\n", line);
    }
    printf("  --\n");
    printf("  networks %u  open %u  no-MFP %u  WPS %u  weak %u  modern %u\n",
           r.networks, r.open, r.no_mfp, r.wps, r.weak, r.modern);
    printf("  devices %u naming %u networks (%u followable)\n", r.devices,
           r.names_leaked, r.trackable);
    printf("  tools seen %u, here now %u\n", r.tools, r.tools_present);
    return 0;
}

static int cli_tower(int argc, char **argv)
{
    (void)argc; (void)argv;
    static char buf[1024];
    pharos_ui_tower_dump(buf, sizeof(buf));
    printf("%s", buf);
    return 0;
}

static int cli_tap(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: tap <0-%u>\n", PHAROS_HUD_ROWS - 1u);
        return 1;
    }
    const int r = atoi(argv[1]);
    if (r < 0 || r >= (int)PHAROS_HUD_ROWS) {
        printf("row out of range (0-%u)\n", PHAROS_HUD_ROWS - 1u);
        return 1;
    }
    pharos_ui_tap_row((unsigned)r);
    printf("tap row %d filed\n", r);
    return 0;
}

static int cli_rows(int argc, char **argv)
{
    (void)argc; (void)argv;
    const pharos_lens_t *l = pharos_lens_active();
    if (!l) {
        printf("no lens running\n");
        return 1;
    }
    if (!l->row) {
        printf("%s has no detail page\n", l->id);
        return 0;
    }
    printf("%s detail - %s | %s\n", l->id,
           l->row_head_left ? l->row_head_left : "",
           l->row_head_right ? l->row_head_right : "");
    static const char *tone[] = { "", "dim", "GOOD", "WARN", "BAD" };
    int opened = -1;
    const int cursor = pharos_ui_detail_cursor(&opened);
    struct pharos_lens_row r;
    unsigned i = 0;
    for (; i < 240u; i++) {
        memset(&r, 0, sizeof(r));
        if (!l->row(i, &r)) {
            break;
        }
        /* The marker is the point of printing this at all: which row the
         * centre tap would act on is the thing a photograph cannot settle. */
        printf("%s %2u  %-26s %-11s %s\n", ((int)i == cursor) ? " >" : "  ", i,
               r.left, r.right, (r.tone < 5) ? tone[r.tone] : "");
    }
    printf("  (%u rows, %u page(s) of %u)\n", i,
           i ? (i + PHAROS_HUD_ROWS - 1u) / PHAROS_HUD_ROWS : 1u, PHAROS_HUD_ROWS);
    if (cursor < 0) {
        printf("  detail page is not up (nav detail to open it)\n");
    } else if (l->row_edit || l->row_expand) {
        printf("  centre tap: %s row %d\n",
               l->row_edit ? "changes or opens" : "opens", cursor);
    }

    /* And what the opened row is showing, since that is a second page nothing
     * else can print. */
    if (opened >= 0 && l->row_expand) {
        printf("  -- row %d opened --\n", opened);
        for (unsigned k = 0; k < 240u; k++) {
            memset(&r, 0, sizeof(r));
            if (!l->row_expand((unsigned)opened, k, &r)) {
                break;
            }
            printf("     %2u  %-26s %-11s %s\n", k, r.left, r.right,
                   (r.tone < 5) ? tone[r.tone] : "");
        }
    }
    return 0;
}

static int cli_nav(int argc, char **argv)
{
    if (argc < 2) {
        printf("nav <detail|next|prev|select|home>\n");
        return 1;
    }
    const char *w = argv[1];
    pharos_nav_t n;
    if (strcmp(w, "detail") == 0)      n = PHAROS_NAV_DETAIL;
    else if (strcmp(w, "next") == 0)   n = PHAROS_NAV_NEXT;
    else if (strcmp(w, "prev") == 0)   n = PHAROS_NAV_PREV;
    else if (strcmp(w, "select") == 0) n = PHAROS_NAV_SELECT;
    else if (strcmp(w, "home") == 0)   n = PHAROS_NAV_HOME;
    else {
        printf("unknown direction: %s\n", w);
        return 1;
    }
    pharos_ui_request_nav(n);
    printf("nav %s filed\n", w);
    return 0;
}

static int cli_rotate(int argc, char **argv)
{
    if (argc < 2) {
        printf("rotation is %d degrees; usage: rotate <0|90|180|270>\n",
               pharos_bsp_rotation());
        return 0;
    }
    const int deg = atoi(argv[1]);
    if (!pharos_bsp_rotate(deg)) {
        printf("rotate takes 0, 90, 180 or 270\n");
        return 1;
    }
    printf("rotated to %d degrees (saved - it will come back this way)\n", deg);
    return 0;
}

/* `lens` - switch lenses from the CLI, same order as the touch zones. */
static int cli_lens(int argc, char **argv)
{
    if (argc < 2) {
        for (unsigned i = 0; i < pharos_lens_count(); i++) {
            const pharos_lens_t *l = pharos_lens_at(i);
            const pharos_lens_t *a = pharos_lens_active();
            printf("  %c %-14s %s\n", (a && a == l) ? '*' : ' ', l->id, l->summary);
        }
        return 0;
    }
    printf(pharos_ui_request_lens(argv[1]) ? "starting: %s\n" : "no such lens: %s\n",
           argv[1]);
    return 0;
}

/* `screen` - force a repaint and prove the pixel path end to end. If this
 * shows something and the lenses do not, the fault is in the UI loop; if it
 * shows nothing, the fault is below LVGL. */
static int cli_screen(int argc, char **argv)
{
    const bool on = !(argc >= 2 && strcmp(argv[1], "off") == 0);
    if (argc >= 2 && strcmp(argv[1], "colour") == 0) {
        if (!pharos_bsp_display_lock(500)) {
            printf("could not take the LVGL lock - the display task is not running\n");
            return 1;
        }
        pharos_hud_colourbars();
        pharos_bsp_display_unlock();
        printf("colour bars pushed. Top to bottom the patches are LABELLED:\n");
        printf("  RED  GREEN  BLUE  YELLOW  WHITE  BLACK\n");
        printf("If a patch does not match its own label, the panel's channel\n");
        printf("order is wrong - tell me which label showed which colour.\n");
        printf("Run 'screen test' to go back, or switch lenses.\n");
        return 0;
    }
    if (argc >= 2 && strcmp(argv[1], "test") == 0) {
        if (!pharos_bsp_display_lock(500)) {
            printf("could not take the LVGL lock - the display task is not running\n");
            return 1;
        }
        pharos_hud_create();
        struct pharos_lens_display t;
        memset(&t, 0, sizeof(t));
        snprintf(t.big, sizeof(t.big), "TEST");
        snprintf(t.band, sizeof(t.band), "screen test");
        snprintf(t.detail, sizeof(t.detail), "if you can read this, pixels work");
        t.score = 66;
        t.ceiling = 96;
        t.has_score = true;
        pharos_hud_live("DIAG", &t, 0x3DDC84);
        pharos_bsp_display_unlock();
        printf("test pattern pushed - look at the panel\n");
        return 0;
    }
    pharos_bsp_brightness(on ? 255 : 0);
    printf("brightness %s\n", on ? "100%" : "0%");
    return 0;
}

void pharos_console_start(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t rc = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    rc.prompt = "pharos>";
    rc.max_cmdline_length = PC_MAX_LINE;
    /* Headroom: command handlers print, format and read lens snapshots. Lens
     * activation is deliberately NOT done here (see glue_activate), which is
     * what keeps this modest. */
    rc.task_stack_size = 6144;

    esp_err_t err;
#if defined(CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG)
    esp_console_dev_usb_serial_jtag_config_t hw =
        ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    err = esp_console_new_repl_usb_serial_jtag(&hw, &rc, &repl);
#elif defined(CONFIG_ESP_CONSOLE_USB_CDC)
    esp_console_dev_usb_cdc_config_t hw = ESP_CONSOLE_DEV_CDC_CONFIG_DEFAULT();
    err = esp_console_new_repl_usb_cdc(&hw, &rc, &repl);
#else
    esp_console_dev_uart_config_t hw = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    err = esp_console_new_repl_uart(&hw, &rc, &repl);
#endif
    if (err != ESP_OK || !repl) {
        ESP_LOGE("console", "REPL init failed: %s", esp_err_to_name(err));
        return;
    }

    esp_console_register_help_command();

    /* Every command from the receive-only table, registered for real - so tab
     * completion and `help` reflect the tested table rather than a copy. */
    unsigned n = 0;
    const pc_cmd_t *tbl = pc_table(&n);
    for (unsigned i = 0; i < n; i++) {
        if (strcmp(tbl[i].name, "help") == 0) {
            continue; /* esp_console provides its own */
        }
        const esp_console_cmd_t c = {
            .command = tbl[i].name,
            .help = tbl[i].summary,
            .hint = tbl[i].usage,
            .func = &cli_forward,
        };
        esp_console_cmd_register(&c);
    }

    const esp_console_cmd_t diag = {
        .command = "diag",
        .help = "board, display, radio and fence state",
        .hint = NULL,
        .func = &cli_diag,
    };
    esp_console_cmd_register(&diag);

    const esp_console_cmd_t rotate = {
        .command = "rotate",
        .help = "set panel rotation: 0, 90, 180 or 270 (saved)",
        .hint = "<0|90|180|270>",
        .func = &cli_rotate,
    };
    esp_console_cmd_register(&rotate);

    const esp_console_cmd_t nav = {
        .command = "nav",
        .help = "drive the screen as a finger would",
        .hint = "<detail|next|prev|select|home>",
        .func = &cli_nav,
    };
    esp_console_cmd_register(&nav);

    const esp_console_cmd_t motion = {
        .command = "motion",
        .help = "the accelerometer: still, walking, or moving",
        .hint = NULL,
        .func = &cli_motion,
    };
    esp_console_cmd_register(&motion);

    const esp_console_cmd_t imu = {
        .command = "imu",
        .help = "is the motion sensor there, and what does it read?",
        .hint = NULL,
        .func = &cli_imu,
    };
    esp_console_cmd_register(&imu);

    const esp_console_cmd_t devices = {
        .command = "devices",
        .help = "export the device inventory for offline CVE lookup",
        .hint = "[full]",
        .func = &cli_devices,
    };
    esp_console_cmd_register(&devices);

    const esp_console_cmd_t ring = {
        .command = "ring",
        .help = "choose which watches the home screen carries",
        .hint = "[reset | <n> [1-4]]",
        .func = &cli_ring,
    };
    esp_console_cmd_register(&ring);

    const esp_console_cmd_t survey = {
        .command = "survey",
        .help = "what this place is like: everything seen this session",
        .hint = NULL,
        .func = &cli_survey,
    };
    esp_console_cmd_register(&survey);

    const esp_console_cmd_t tower = {
        .command = "tower",
        .help = "the watchtower ring: whose turn, and what each watch found",
        .hint = NULL,
        .func = &cli_tower,
    };
    esp_console_cmd_register(&tower);

    const esp_console_cmd_t tap = {
        .command = "tap",
        .help = "touch a detail row, as a finger would",
        .hint = "<0-3>",
        .func = &cli_tap,
    };
    esp_console_cmd_register(&tap);

    const esp_console_cmd_t rows = {
        .command = "rows",
        .help = "print what the detail page is showing for the active lens",
        .hint = NULL,
        .func = &cli_rows,
    };
    esp_console_cmd_register(&rows);

    const esp_console_cmd_t ble = {
        .command = "ble",
        .help = "list every BLE advertiser in range, named or not",
        .hint = NULL,
        .func = &cli_ble,
    };
    esp_console_cmd_register(&ble);

    const esp_console_cmd_t alarm = {
        .command = "alarm",
        .help = "the alarm: on | off | vol <0-100> | test",
        .hint = "[on|off|vol|test]",
        .func = &cli_alarm,
    };
    esp_console_cmd_register(&alarm);

    const esp_console_cmd_t lens = {
        .command = "lens",
        .help = "list lenses, or switch to one by id",
        .hint = "[id]",
        .func = &cli_lens,
    };
    esp_console_cmd_register(&lens);

    const esp_console_cmd_t screen = {
        .command = "screen",
        .help = "screen test | colour | on | off - prove the pixel path",
        .hint = "[test|on|off]",
        .func = &cli_screen,
    };
    esp_console_cmd_register(&screen);

    printf("\nPharos CLI - type 'help'. Receive-only; nothing here transmits.\n"
           "  touch: tap LEFT/RIGHT of the glass to change lens, long-press = stop\n"
           "  BOOT button: short press = next lens, hold = stop\n"
           "  'rotate 90' if the screen is sideways, 'diag' if it is dark\n");
    esp_console_start_repl(repl);
}
