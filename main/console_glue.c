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
#include "esp_log.h"
#include "esp_system.h"
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "pharos_bsp.h"
#include "pharos_hud.h"
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
