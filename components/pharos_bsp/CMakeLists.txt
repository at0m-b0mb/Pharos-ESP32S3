# The board support package wraps the Waveshare managed BSP component. By
# default (CONFIG_PHAROS_HAS_VENDOR_BSP=n) it builds a simulated board so the
# whole firmware compiles and links for ESP32-S3 in CI without asserting the
# vendor's bsp_* API, which is unverified until hardware bring-up (M1).
#
# heap / log / freertos are ESP-IDF common requirements and need not be listed.
# pharos_engine is public because pharos_bsp.h exposes pwr_battery_t.
#
# At M1, set CONFIG_PHAROS_HAS_VENDOR_BSP=y and add the vendor component here:
#   REQUIRES pharos_core pharos_engine esp32_s3_touch_amoled_1_75c
idf_component_register(
    SRCS "pharos_bsp.c"
    INCLUDE_DIRS "include"
    REQUIRES pharos_core pharos_engine
    PRIV_REQUIRES driver esp_timer
)
