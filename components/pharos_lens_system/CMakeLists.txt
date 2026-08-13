idf_component_register(
    SRCS "lens_system.c"
    INCLUDE_DIRS "."
    REQUIRES pharos_core pharos_engine pharos_radio
    PRIV_REQUIRES esp_app_format
)

target_link_libraries(${COMPONENT_LIB} INTERFACE
    "-Wl,--whole-archive" ${COMPONENT_LIB} "-Wl,--no-whole-archive")
