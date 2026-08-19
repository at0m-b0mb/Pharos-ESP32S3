/* Pharos - the event vocabulary
 *
 * Everything the radios hear becomes one of these, and nothing else crosses
 * from the ingest core to the analytics core. The struct is deliberately
 * fixed-size and free of pointers: the producer is an ISR-adjacent callback
 * in the Wi-Fi driver task that must return in single-digit microseconds, so
 * it copies a summary and moves on. Raw frames, when a lens wants them, go
 * to a separate capture ring that is never in the detector's hot path.
 */
#ifndef PHAROS_EVENT_H
#define PHAROS_EVENT_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PHAROS_EV_NONE = 0,
    PHAROS_EV_DOT11,     /* one 802.11 frame header, any type   */
    PHAROS_EV_BLE_ADV,   /* one BLE advertising report          */
    PHAROS_EV_DWELL,     /* the scanner finished a channel dwell */
    PHAROS_EV_MOTION,    /* IMU gesture or orientation change   */
    PHAROS_EV_ACOUSTIC,  /* mic band-energy summary, no audio   */
    PHAROS_EV_SYSTEM,    /* battery, storage, thermal           */
} pharos_ev_type_t;

/* 802.11 frame types, as they appear on the wire. */
#define PHAROS_FT_MGMT 0
#define PHAROS_FT_CTRL 1
#define PHAROS_FT_DATA 2

/* Management subtypes Pharos cares about. */
#define PHAROS_ST_ASSOC_REQ    0x00
#define PHAROS_ST_ASSOC_RESP   0x01
#define PHAROS_ST_REASSOC_REQ  0x02
#define PHAROS_ST_PROBE_REQ    0x04
#define PHAROS_ST_PROBE_RESP   0x05
#define PHAROS_ST_BEACON       0x08
#define PHAROS_ST_DISASSOC     0x0A
#define PHAROS_ST_AUTH         0x0B
#define PHAROS_ST_DEAUTH       0x0C

#define PHAROS_DOT11_F_RETRY      (1u << 0)
#define PHAROS_DOT11_F_PROTECTED  (1u << 1)
#define PHAROS_DOT11_F_TO_DS      (1u << 2)
#define PHAROS_DOT11_F_FROM_DS    (1u << 3)
#define PHAROS_DOT11_F_BROADCAST  (1u << 4) /* addr1 is ff:ff:ff:ff:ff:ff */
#define PHAROS_DOT11_F_MFP_SEEN   (1u << 5) /* frame carried an MMIE      */
#define PHAROS_DOT11_F_PMKID      (1u << 6) /* EAPOL M1 carried a PMKID KDE */
/* The beacon carried Pwnagotchi "whisper" elements.
 *
 * A Pwnagotchi advertises itself to other Pwnagotchi with an ordinary 802.11
 * beacon that has NO SSID and a set of non-standard information elements
 * carrying a chunked JSON payload:
 *
 *   222 (0xDE) whisper payload      224 (0xE0) whisper identity
 *   225 (0xE1) whisper signature    226 (0xE2) whisper stream header
 *
 * It also transmits from a hardcoded source address, de:ad:be:ef:de:ad. Two
 * independent signals, which is why both are carried: forks change the address
 * far more readily than they change the protocol. */
#define PHAROS_DOT11_F_WHISPER    (1u << 7)

/* RSN posture, distilled from the beacon's RSN element into one byte.
 *
 * Carried on the event because the alternative - a second "capture ring" the
 * analytics core walks for element chains - was designed but never built, and
 * in the meantime EVERY name-dependent lens was inert: Census graded nothing
 * because it never saw an RSN element, and Twin grouped nothing because every
 * SSID it was handed was empty. */
#define PHAROS_RSN_F_PRESENT      (1u << 0)
#define PHAROS_RSN_F_MFP_CAPABLE  (1u << 1)
#define PHAROS_RSN_F_MFP_REQUIRED (1u << 2)
#define PHAROS_RSN_F_SAE          (1u << 3)
#define PHAROS_RSN_F_PSK          (1u << 4)
#define PHAROS_RSN_F_OWE          (1u << 5)
#define PHAROS_RSN_F_WPS          (1u << 6)

#define PHAROS_EV_SSID_MAX 32

typedef struct {
    uint8_t a1[6]; /* receiver / destination */
    uint8_t a2[6]; /* transmitter            */
    uint8_t a3[6]; /* BSSID, usually         */
    uint16_t seq;  /* 12-bit sequence number */
    uint16_t reason_or_status;
    int8_t rssi;
    uint8_t channel;
    uint8_t type;    /* PHAROS_FT_*  */
    uint8_t subtype; /* PHAROS_ST_*  */
    uint8_t flags;   /* PHAROS_DOT11_F_* */
    uint8_t rate_idx;
    /* EAPOL-Key message number 1..4, or 0 when the frame is not one.
     *
     * Only the pairwise 4-way handshake is identified, and only messages 1
     * and 2 are reliably visible: they are exchanged BEFORE the PTK is
     * installed, so they travel unprotected and a passive receiver can read
     * them. That is precisely the pair an attacker needs for offline
     * cracking, which is why noticing them is worth the byte. */
    uint8_t eapol;

    /* The network name and security posture, parsed in the hot path for the
     * three subtypes that carry them (beacon, probe response, probe request).
     * Bounded and copied - never a pointer into the driver's buffer, which is
     * recycled the moment the callback returns. */
    uint8_t ssid_len;
    uint8_t rsn_flags; /* PHAROS_RSN_F_*; 0 for frames that carry no RSN */
    char ssid[PHAROS_EV_SSID_MAX];
} pharos_ev_dot11_t;

#define PHAROS_BLE_ADV_MAX 31

typedef struct {
    uint8_t addr[6];
    uint8_t addr_type; /* public / random-static / RPA / NRPA */
    uint8_t adv_type;  /* ADV_IND, ADV_NONCONN_IND, SCAN_RSP ... */
    int8_t rssi;
    uint8_t data_len;
    uint8_t data[PHAROS_BLE_ADV_MAX];
} pharos_ev_ble_t;

typedef struct {
    uint8_t channel;
    uint8_t band;         /* 0 = 2.4 GHz */
    uint16_t frames;      /* frames seen during the dwell      */
    uint16_t retries;     /* of those, how many were retries   */
    uint16_t dwell_ms;    /* how long we actually sat there    */
    int8_t noise_floor;   /* dBm, quartile estimate            */
    int8_t peak_rssi;
    uint16_t busy_permil; /* airtime occupancy, per mille      */
} pharos_ev_dwell_t;

typedef struct {
    uint8_t kind;   /* wrist-raise, shake, flip, still */
    int16_t pitch_cdeg, roll_cdeg, yaw_cdeg;
} pharos_ev_motion_t;

typedef struct {
    uint16_t band_hz_lo, band_hz_hi;
    int16_t level_dbfs_x10;
} pharos_ev_acoustic_t;

typedef struct {
    uint8_t code;
    int32_t value;
} pharos_ev_system_t;

typedef struct pharos_event {
    uint64_t t_us;   /* monotonic, filled by the producer */
    uint8_t type;    /* pharos_ev_type_t */
    uint8_t src;     /* which ingest path produced it     */
    uint16_t serial; /* wraps; lets a lens spot gaps      */
    union {
        pharos_ev_dot11_t dot11;
        pharos_ev_ble_t ble;
        pharos_ev_dwell_t dwell;
        pharos_ev_motion_t motion;
        pharos_ev_acoustic_t acoustic;
        pharos_ev_system_t system;
    } u;
} pharos_event_t;

#ifdef __cplusplus
}
#endif

#endif /* PHAROS_EVENT_H */
