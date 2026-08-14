idf_component_register(
    SRCS "lens_vigil.c"
    INCLUDE_DIRS "."
    REQUIRES pharos_core pharos_engine pharos_radio
    PRIV_REQUIRES esp_timer
    WHOLE_ARCHIVE
)
