/* Pharos host tests, part four: the regulatory region clamp. */
#include "pharos_region.h"
#include "test_support.h"

void test_region(void)
{
    banner("region: channel plan clamp");

    pharos_region_set(PHAROS_REGION_WORLD);
    CHECK_EQ(pharos_region_get(), PHAROS_REGION_WORLD);
    CHECK_EQ(pharos_region_max_channel(), 13);

    pharos_region_set(PHAROS_REGION_FCC);
    CHECK_EQ(pharos_region_max_channel(), 11);
    CHECK_EQ(pharos_region_clamp_channel(13), 11); /* 12,13 not in the US plan */
    CHECK_EQ(pharos_region_clamp_channel(6), 6);
    CHECK_EQ(pharos_region_clamp_channel(0), 1);   /* below the band          */

    pharos_region_set(PHAROS_REGION_ETSI);
    CHECK_EQ(pharos_region_clamp_channel(13), 13);
    CHECK_EQ(pharos_region_clamp_channel(14), 13);

    pharos_region_set(PHAROS_REGION_JP);
    CHECK_EQ(pharos_region_max_channel(), 14); /* 14 is Japan, 11b only */
    CHECK_EQ(pharos_region_clamp_channel(14), 14);

    /* An out-of-range region enum must be ignored, not stored. */
    pharos_region_set(PHAROS_REGION_ETSI);
    pharos_region_set((pharos_region_t)999);
    CHECK_EQ(pharos_region_get(), PHAROS_REGION_ETSI);

    for (int r = 0; r < PHAROS_REGION_COUNT; r++) {
        const char *nm = pharos_region_name((pharos_region_t)r);
        CHECK(nm && *nm, "region %d named", r);
    }

    pharos_region_set(PHAROS_REGION_WORLD); /* leave it as the default */
}
