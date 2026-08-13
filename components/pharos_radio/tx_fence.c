/* Pharos - the transmit fence
 *
 * The Espressif and NimBLE transmit primitives are redirected here by the
 * linker (-Wl,--wrap=symbol; see CMakeLists.txt). The real symbol becomes
 * __real_<name>, which this file never calls; anything in the firmware that
 * tries to transmit lands in __wrap_<name> and is trapped.
 *
 * A trap does not silently no-op. Silent failure is how a fence rots: a lens
 * "works", nobody notices it was trying to transmit, and one day the wrap is
 * removed to "fix" it. Instead the trap records the attempt, screams into the
 * log with the return address, and aborts the firmware. A transmit attempt in
 * a receive-only tool is a bug of the most serious kind - the kind that turns
 * a lawful monitor into an unlawful transmitter - and it should stop the
 * device, not degrade it.
 *
 * The counters are read by the self-audit lens and printed at boot. On a
 * correct image tx_attempts is zero for the life of the device.
 */
#include <stdint.h>

#include "esp_log.h"
#include "esp_system.h"

static const char *TAG = "tx_fence";

/* Survives into the panic dump so the cause is visible in a core file. */
volatile uint32_t g_pharos_tx_attempts = 0;

static void trap(const char *what, void *ret_addr)
{
    g_pharos_tx_attempts++;
    ESP_LOGE(TAG,
             "TRANSMIT FENCE TRIPPED: %s was called from %p. Pharos is "
             "receive-only by construction; this is a serious bug. Halting so "
             "the device cannot emit a frame.",
             what, ret_addr);
    /* Abort rather than return: the caller expected to transmit, and letting
     * it continue as though it had is worse than stopping. */
    abort();
}

#define FENCE_RETADDR() __builtin_return_address(0)

/* The wrapped primitives. Signatures match the SDK so the linker is happy;
 * the bodies never reach the hardware. Adding a new transmit primitive to the
 * SDK surface means adding it here AND to the --wrap list AND to
 * check_tx_fence.sh - three places, on purpose, so the fence cannot be
 * widened by forgetting one. */

int __wrap_esp_wifi_80211_tx(int ifx, const void *buffer, int len, bool en)
{
    (void)ifx; (void)buffer; (void)len; (void)en;
    trap("esp_wifi_80211_tx", FENCE_RETADDR());
    return -1;
}

int __wrap_esp_wifi_deauth_sta(uint16_t aid)
{
    (void)aid;
    trap("esp_wifi_deauth_sta", FENCE_RETADDR());
    return -1;
}

int __wrap_esp_now_send(const uint8_t *peer, const uint8_t *data, int len)
{
    (void)peer; (void)data; (void)len;
    trap("esp_now_send", FENCE_RETADDR());
    return -1;
}

int __wrap_esp_now_init(void)
{
    /* ESP-NOW exists only to transmit; refuse to even initialise it. */
    trap("esp_now_init", FENCE_RETADDR());
    return -1;
}

/* esp_wifi_set_mode is legitimate for STA and NULL (monitor); it is only a
 * transmit risk when an AP bit is set. We wrap it, allow the receive modes,
 * and trap the AP modes. The real symbol is reached through __real_. */
#define WIFI_MODE_NULL_ 0
#define WIFI_MODE_STA_  1
#define WIFI_MODE_AP_   2
#define WIFI_MODE_APSTA_ 3

extern int __real_esp_wifi_set_mode(int mode);

int __wrap_esp_wifi_set_mode(int mode)
{
    if (mode == WIFI_MODE_AP_ || mode == WIFI_MODE_APSTA_) {
        trap("esp_wifi_set_mode(AP)", FENCE_RETADDR());
        return -1;
    }
    return __real_esp_wifi_set_mode(mode);
}

uint32_t pharos_tx_fence_attempts(void)
{
    return g_pharos_tx_attempts;
}
