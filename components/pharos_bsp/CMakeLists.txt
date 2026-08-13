# The board support package is a thin wrapper over the Waveshare managed BSP
# component. Pharos does not re-implement the display/touch/PMU drivers - it
# pins the board component and adds the pin header above plus the boot glue.
#
# The managed dependency is declared in main/idf_component.yml:
#   waveshare/esp32_s3_touch_amoled_1_75c: "^3.0.0"
idf_component_register(
    SRCS "pharos_bsp.c"
    INCLUDE_DIRS "include"
    REQUIRES pharos_core driver esp_timer
)
