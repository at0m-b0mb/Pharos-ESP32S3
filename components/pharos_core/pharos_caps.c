#include "pharos_caps.h"

#include <string.h>

static const struct {
    pharos_caps_t bit;
    const char *name;
} k_names[] = {
    { PHAROS_CAP_WIFI_RX,   "wifi.rx"   },
    { PHAROS_CAP_WIFI_CHAN, "wifi.chan" },
    { PHAROS_CAP_BLE_SCAN,  "ble.scan"  },
    { PHAROS_CAP_IMU,       "imu"       },
    { PHAROS_CAP_MIC,       "mic"       },
    { PHAROS_CAP_SPEAKER,   "speaker"   },
    { PHAROS_CAP_RTC,       "rtc"       },
    { PHAROS_CAP_STORAGE_R, "storage.r" },
    { PHAROS_CAP_STORAGE_W, "storage.w" },
    { PHAROS_CAP_PMU,       "pmu"       },
    { PHAROS_CAP_UPLINK,    "uplink"    },
};

const char *pharos_caps_name(pharos_caps_t single_bit)
{
    for (unsigned i = 0; i < sizeof(k_names) / sizeof(k_names[0]); i++) {
        if (k_names[i].bit == single_bit) {
            return k_names[i].name;
        }
    }
    return "?";
}

char *pharos_caps_describe(pharos_caps_t caps, char *buf, unsigned buflen)
{
    if (!buf || buflen == 0) {
        return buf;
    }
    buf[0] = '\0';
    unsigned used = 0;
    for (unsigned i = 0; i < sizeof(k_names) / sizeof(k_names[0]); i++) {
        if (!(caps & k_names[i].bit)) {
            continue;
        }
        const char *n = k_names[i].name;
        unsigned need = (unsigned)strlen(n) + (used ? 1u : 0u);
        if (used + need + 1u > buflen) {
            break;
        }
        if (used) {
            buf[used++] = ' ';
        }
        memcpy(buf + used, n, strlen(n));
        used += (unsigned)strlen(n);
        buf[used] = '\0';
    }
    if (used == 0 && buflen >= 5) {
        memcpy(buf, "none", 5);
    }
    return buf;
}
