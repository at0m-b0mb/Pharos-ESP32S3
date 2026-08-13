/* Pharos - capability tokens
 *
 * Every lens declares, at compile time, the exact set of hardware powers it
 * needs. The HAL refuses service to a lens that did not declare the power it
 * is asking for. This is the structural half of the safety story: a lens
 * cannot quietly grow a new capability without the declaration changing, and
 * the declaration is printed on the lens' info card in the UI.
 *
 * There is deliberately no CAP_WIFI_TX / CAP_BLE_ADV token. Those powers do
 * not exist in this firmware - see components/pharos_radio/tx_fence.c.
 */
#ifndef PHAROS_CAPS_H
#define PHAROS_CAPS_H

#include <stdint.h>

typedef uint32_t pharos_caps_t;

#define PHAROS_CAP_NONE        (0u)
#define PHAROS_CAP_WIFI_RX     (1u << 0)  /* 802.11 promiscuous receive */
#define PHAROS_CAP_WIFI_CHAN   (1u << 1)  /* set/hop the receive channel  */
#define PHAROS_CAP_BLE_SCAN    (1u << 2)  /* BLE observer role, scan only */
#define PHAROS_CAP_IMU         (1u << 3)  /* QMI8658 motion               */
#define PHAROS_CAP_MIC         (1u << 4)  /* ES7210 array, analysis only  */
#define PHAROS_CAP_SPEAKER     (1u << 5)  /* ES8311 out                   */
#define PHAROS_CAP_RTC         (1u << 6)  /* wall clock read              */
#define PHAROS_CAP_STORAGE_R   (1u << 7)  /* read logs / replay corpora   */
#define PHAROS_CAP_STORAGE_W   (1u << 8)  /* append to the evidence log   */
#define PHAROS_CAP_PMU         (1u << 9)  /* AXP2101 battery telemetry    */
#define PHAROS_CAP_UPLINK      (1u << 10) /* STA join, operator-owned net */

/* Powers that make the device audible/visible to somebody else. A lens
 * holding any of these shows a persistent rim indicator. */
#define PHAROS_CAP_OUTWARD     (PHAROS_CAP_SPEAKER | PHAROS_CAP_UPLINK)

/* Powers that touch personally identifying capture. Holding one of these
 * forces the redaction policy on any log the lens writes. */
#define PHAROS_CAP_SENSITIVE   (PHAROS_CAP_WIFI_RX | PHAROS_CAP_BLE_SCAN | PHAROS_CAP_MIC)

const char *pharos_caps_name(pharos_caps_t single_bit);

/* Renders e.g. "wifi.rx chan storage.w" into buf. Returns buf. */
char *pharos_caps_describe(pharos_caps_t caps, char *buf, unsigned buflen);

#endif /* PHAROS_CAPS_H */
