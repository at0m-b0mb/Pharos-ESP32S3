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

/* Provided by the Watch and Locate lens components. */
extern void pharos_lens_watch_camp(uint8_t channel);
extern void pharos_lens_watch_survey(void);
extern void pharos_lens_locate_set_target(const uint8_t bssid[6], uint8_t channel);

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

static void glue_status(pc_out_t *o)
{
    const pharos_lens_t *a = pharos_lens_active();
    if (!a) {
        pc_println(o, "  idle - nothing active");
        return;
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

static void console_task(void *arg)
{
    (void)arg;
    char line[PC_MAX_LINE];
    fputs("\nPharos console - type 'help'. Receive-only; nothing here transmits.\n"
          "pharos> ", stdout);
    fflush(stdout);
    for (;;) {
        if (fgets(line, sizeof(line), stdin)) {
            pharos_console_run(line);
        } else {
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
}

void pharos_console_start(void)
{
    xTaskCreate(console_task, "pharos_con", 4096, NULL, 4, NULL);
}
