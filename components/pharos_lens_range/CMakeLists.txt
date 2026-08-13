idf_component_register(
    SRCS "lens_range.c"
    INCLUDE_DIRS "."
    REQUIRES pharos_core pharos_engine
    PRIV_REQUIRES esp_timer
)

target_link_libraries(${COMPONENT_LIB} INTERFACE
    "-Wl,--whole-archive" ${COMPONENT_LIB} "-Wl,--no-whole-archive")
