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

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "pharos_console.h"
#include "pharos_lens.h"
#include "pharos_radio.h"
#include "pharos_region.h"
#include "pharos_aegis.h"
#include "pharos_harvest.h"
#include "pharos_sentinel.h"
#include "pharos_squall.h"

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

static bool glue_activate(const char *id) { return pharos_lens_activate(id); }
static void glue_deactivate(void) { pharos_lens_deactivate(); }

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
    fputs("pharos> ", stdout);
    fflush(stdout);
}

/* Read a line by polling getchar().
 *
 * Deliberately NOT fgets(stdin): with ESP-IDF's default VFS the stdin stream is
 * non-blocking, so fgets returns immediately with EOF and a naive loop either
 * spins at 100% or never sees input at all. Accumulating characters and
 * yielding when the buffer is dry is the behaviour that actually works over
 * both UART and USB-CDC, and it lets us handle backspace properly. */
static void console_task(void *arg)
{
    (void)arg;
    char line[PC_MAX_LINE];
    size_t len = 0;

    fputs("\nPharos console - type 'help'. Receive-only; nothing here transmits.\n"
          "pharos> ", stdout);
    fflush(stdout);

    for (;;) {
        const int c = getchar();
        if (c == EOF) {
            vTaskDelay(pdMS_TO_TICKS(20));
            continue;
        }
        if (c == '\r' || c == '\n') {
            fputc('\n', stdout);
            line[len] = '\0';
            pharos_console_run(line);
            len = 0;
            continue;
        }
        if (c == 0x7F || c == '\b') { /* DEL / backspace */
            if (len) {
                len--;
                fputs("\b \b", stdout);
                fflush(stdout);
            }
            continue;
        }
        if (c >= 0x20 && c < 0x7F && len < sizeof(line) - 1) {
            line[len++] = (char)c;
            fputc(c, stdout); /* local echo */
            fflush(stdout);
        }
    }
}

void pharos_console_start(void)
{
    xTaskCreate(console_task, "pharos_con", 4096, NULL, 4, NULL);
}
