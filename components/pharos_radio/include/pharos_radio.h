/* Pharos - the radio HAL, and the fence around it
 *
 * This is the only component in Pharos that touches esp_wifi and NimBLE, and
 * it exposes exactly one shape of power: receive. There is no transmit call
 * in this header, there is no transmit call behind it, and the build is rigged
 * so that a transmit call cannot be linked in even by accident. That is not a
 * slogan - it is four mechanisms, and CI checks all four:
 *
 *   1. Capability gate. Every entry point checks the active lens' declared
 *      capabilities (pharos_lens_active_caps). There is no CAP_WIFI_TX bit to
 *      hold, so no lens can ask to transmit; a lens that has not declared even
 *      CAP_WIFI_RX is refused receive.
 *
 *   2. Link-time wrap fence. The Espressif transmit primitives
 *      (esp_wifi_80211_tx, esp_wifi_set_mode with an AP bit, esp_now_send,
 *      esp_wifi_deauth_sta) are redirected with -Wl,--wrap to abort traps in
 *      tx_fence.c. If any code in any component calls one, the firmware
 *      panics at the call site instead of emitting a frame. See the linker
 *      fragment in this component's CMakeLists.
 *
 *   3. Build-time role fence. NimBLE is configured observer-only: the
 *      broadcaster and peripheral roles are compiled out, so the advertising
 *      and connection code is not in the image to be reached.
 *
 *   4. Source audit. tools/check_tx_fence.sh greps every source file for the
 *      transmit primitives and fails the build if one appears outside
 *      tx_fence.c's wrap targets.
 *
 * Receive-only is the product. A tool that promises it is listening, and could
 * be made to transmit by a one-line change, is a tool nobody should deploy in
 * a building they do not own. Pharos is built so that promise survives contact
 * with a hurried patch.
 */
#ifndef PHAROS_RADIO_H
#define PHAROS_RADIO_H

#include <stdbool.h>
#include <stdint.h>

#include "pharos_bus.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---- channel plans -------------------------------------------------- */

#define PHAROS_CHAN_MIN 1
#define PHAROS_CHAN_MAX 14 /* region-clamped at runtime; see pharos_region */

typedef enum {
    PHAROS_HOP_SURVEY = 0, /* even sweep of the plan, for a broad look     */
    PHAROS_HOP_CAMP,       /* hold one channel: the way to earn confidence */
    PHAROS_HOP_WEIGHTED,   /* dwell longer where frames are landing        */
} pharos_hop_t;

typedef struct {
    uint8_t channels[PHAROS_CHAN_MAX];
    uint8_t n_channels;
    uint16_t dwell_ms;   /* per channel                         */
    pharos_hop_t hop;
    bool want_mgmt;      /* deliver management frames           */
    bool want_data;      /* deliver data frame headers          */
    bool want_ctrl;      /* deliver control frames              */
} pharos_scan_plan_t;

/* A broad 2.4 GHz survey across the region's legal channels. */
pharos_scan_plan_t pharos_scan_plan_survey(void);
/* Camp on one channel - the plan the UI switches to when the operator wants
 * to raise a verdict's confidence ceiling. */
pharos_scan_plan_t pharos_scan_plan_camp(uint8_t channel);

/* ---- lifecycle ------------------------------------------------------ */

/* Bring the Wi-Fi driver up in promiscuous receive. Refuses, returning false,
 * if the active lens has not declared CAP_WIFI_RX. Frames land on `bus` as
 * pharos_ev_dot11_t summaries; the raw body goes to the capture ring. */
bool pharos_radio_rx_start(const pharos_scan_plan_t *plan, pharos_bus_t *bus);
void pharos_radio_rx_stop(void);

/* BLE observer (scan-only). Refuses without CAP_BLE_SCAN. NimBLE is built
 * observer-only, so this cannot advertise or connect. */
bool pharos_radio_ble_scan_start(pharos_bus_t *bus);
void pharos_radio_ble_scan_stop(void);

/* ---- introspection -------------------------------------------------- */

uint8_t pharos_radio_channel(void);
bool pharos_radio_is_camped(void);

/* Per mille of recent wall time spent on `channel`. This is the number the
 * engines multiply their confidence ceiling by: camped => ~1000, hopping =>
 * roughly 1000 / n_channels. */
uint16_t pharos_radio_dwell_permil(uint8_t channel);

typedef struct {
    uint32_t frames_seen;
    uint32_t frames_dropped;  /* driver-side, before our bus            */
    uint32_t channel_changes;
    uint32_t ble_reports;
    uint8_t current_channel;
    bool camped;
} pharos_radio_stats_t;

void pharos_radio_stats(pharos_radio_stats_t *out);

/* The fence's own view of itself, for the self-audit lens and the boot log.
 * A firmware image that cannot fill this in honestly should not ship. */
typedef struct {
    bool wrap_linked;       /* --wrap fence present in this image       */
    bool ble_observer_only; /* NimBLE roles compiled out               */
    bool tx_symbols_absent; /* no transmit primitive linked            */
    uint32_t tx_attempts;   /* times the fence caught a call (target 0) */
} pharos_tx_fence_t;

void pharos_radio_fence_status(pharos_tx_fence_t *out);

#ifdef __cplusplus
}
#endif

#endif /* PHAROS_RADIO_H */
