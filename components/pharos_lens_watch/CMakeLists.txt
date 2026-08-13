idf_component_register(
    SRCS "lens_watch.c"
    INCLUDE_DIRS "."
    REQUIRES pharos_core pharos_engine pharos_radio
    PRIV_REQUIRES esp_timer
)

# Lenses register through constructor attributes, so nothing else in the
# project references this translation unit. Force the whole archive in or the
# linker will discard the registration.
target_link_libraries(${COMPONENT_LIB} INTERFACE
    "-Wl,--whole-archive" ${COMPONENT_LIB} "-Wl,--no-whole-archive")
