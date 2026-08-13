#include "pharos_region.h"

static pharos_region_t s_region = PHAROS_REGION_WORLD;

static uint8_t max_for(pharos_region_t r)
{
    switch (r) {
    case PHAROS_REGION_FCC:  return 11;
    case PHAROS_REGION_ETSI: return 13;
    case PHAROS_REGION_JP:   return 14;
    case PHAROS_REGION_WORLD:
    default:                 return 13;
    }
}

void pharos_region_set(pharos_region_t region)
{
    if (region < PHAROS_REGION_COUNT) {
        s_region = region;
    }
}

pharos_region_t pharos_region_get(void)
{
    return s_region;
}

uint8_t pharos_region_max_channel(void)
{
    return max_for(s_region);
}

uint8_t pharos_region_clamp_channel(uint8_t channel)
{
    const uint8_t hi = max_for(s_region);
    if (channel < 1) return 1;
    if (channel > hi) return hi;
    return channel;
}

const char *pharos_region_name(pharos_region_t region)
{
    switch (region) {
    case PHAROS_REGION_WORLD: return "World (1-13)";
    case PHAROS_REGION_FCC:   return "FCC (1-11)";
    case PHAROS_REGION_ETSI:  return "ETSI (1-13)";
    case PHAROS_REGION_JP:    return "Japan (1-14)";
    default:                  return "?";
    }
}
