# The board support package wraps the Waveshare managed BSP component
# (waveshare/esp32_s3_touch_amoled_1_75c, declared in main/idf_component.yml).
# That component is what actually drives the CO5300 panel, the CST9217 touch
# controller and LVGL; Pharos does not re-implement any of it.
#
# pharos_engine is public because pharos_bsp.h exposes pwr_battery_t.
# The vendor BSP and lvgl are listed so `bsp/esp-bsp.h`, `bsp/display.h` and
# `lvgl.h` resolve when CONFIG_PHAROS_HAS_VENDOR_BSP is on (the default).
idf_component_register(
    SRCS "pharos_bsp.c"
    INCLUDE_DIRS "include"
    REQUIRES pharos_core pharos_engine
    PRIV_REQUIRES driver esp_timer nvs_flash
)

# Link against the managed components only when they are actually in the build.
# Guarding keeps a no-panel/CI build (vendor BSP off) working unchanged.
idf_build_get_property(build_components BUILD_COMPONENTS)
if("waveshare__esp32_s3_touch_amoled_1_75c" IN_LIST build_components)
    idf_component_get_property(BSP_LIB waveshare__esp32_s3_touch_amoled_1_75c COMPONENT_LIB)
    target_link_libraries(${COMPONENT_LIB} PUBLIC ${BSP_LIB})
endif()
if("lvgl__lvgl" IN_LIST build_components)
    idf_component_get_property(LVGL_LIB lvgl__lvgl COMPONENT_LIB)
    target_link_libraries(${COMPONENT_LIB} PUBLIC ${LVGL_LIB})
endif()
