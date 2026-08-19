idf_component_register(
    SRCS "main.c" "console_glue.c"
    INCLUDE_DIRS "."
    REQUIRES
        nvs_flash
        console
        esp_event
        esp_netif
        pharos_core
        pharos_engine
        pharos_bsp
        pharos_ui
        pharos_radio
        pharos_audio
        # Lenses. Each registers itself through a constructor; listing it here
        # is only what pulls the component into the link. Adding a lens is:
        # create the component, add one line here. main.c names none of them.
        pharos_lens_spectrum
        pharos_lens_census
        pharos_lens_watch
        pharos_lens_probe
        pharos_lens_karma
        pharos_lens_mirage
        pharos_lens_locate
        pharos_lens_range
        pharos_lens_footprint
        pharos_lens_sentinel
        pharos_lens_aegis
        pharos_lens_harvest
        pharos_lens_squall
        pharos_lens_vigil
        pharos_lens_system
        pharos_lens_whisper
)

# LVGL builds with -Werror under ESP-IDF, and on the GCC 15 toolchain that
# ships with IDF 6.0 its LV_ATTRIBUTE_FAST_MEM (IRAM) placement trips
#   error: ignoring attribute 'section (".iram1.8")' because it conflicts
#          with previous 'section (".iram1.4")' [-Werror=attributes]
# in the software blend units. It is a warning about LVGL's own codegen, not
# about anything Pharos does, and Waveshare's own example carries the identical
# guard. Downgrade it for the LVGL library only, so the rest of the project
# keeps its warnings-as-errors.
idf_build_get_property(build_components BUILD_COMPONENTS)
if("lvgl__lvgl" IN_LIST build_components)
    idf_component_get_property(LVGL_LIB lvgl__lvgl COMPONENT_LIB)
    if(TARGET ${LVGL_LIB})
        target_compile_options(${LVGL_LIB} PRIVATE -Wno-error=attributes -Wno-error=cpp)
    endif()
endif()
