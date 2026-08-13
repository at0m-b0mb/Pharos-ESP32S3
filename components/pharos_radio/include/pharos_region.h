/* Pharos - regulatory region clamp
 *
 * Even a receive-only device should stay inside the channel plan of the place
 * it is switched on. Channel 14 is legal for 802.11b in Japan and nowhere
 * else; channels 12 and 13 are fine across most of the world but not in the
 * United States. A monitor that tunes to a channel its region does not use is
 * not illegal the way transmitting there would be, but it is a tool that does
 * not know where it is, and this project would rather know.
 *
 * The region is chosen by the operator in Settings and persisted; it defaults
 * to the most permissive plan the hardware supports only until they set it,
 * and the UI nudges them to set it on first boot. Pure functions, host-tested.
 */
#ifndef PHAROS_REGION_H
#define PHAROS_REGION_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PHAROS_REGION_WORLD = 0, /* 1-13, the safe default before a choice   */
    PHAROS_REGION_FCC,       /* 1-11  (US, Canada)                       */
    PHAROS_REGION_ETSI,      /* 1-13  (most of Europe)                   */
    PHAROS_REGION_JP,        /* 1-14  (Japan, 14 for 11b only)           */
    PHAROS_REGION_COUNT,
} pharos_region_t;

void pharos_region_set(pharos_region_t region);
pharos_region_t pharos_region_get(void);

uint8_t pharos_region_max_channel(void);
/* Clamp a requested channel into the current region's plan. */
uint8_t pharos_region_clamp_channel(uint8_t channel);
const char *pharos_region_name(pharos_region_t region);

#ifdef __cplusplus
}
#endif

#endif /* PHAROS_REGION_H */
