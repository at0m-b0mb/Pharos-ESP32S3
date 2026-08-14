idf_component_register(
    SRCS "lens_aegis.c"
    INCLUDE_DIRS "."
    # Depends on pharos_ui because the correlator's state lives with the UI
    # loop - the one thing that runs continuously and knows the active lens.
    REQUIRES pharos_core pharos_engine pharos_ui
    PRIV_REQUIRES esp_timer
    WHOLE_ARCHIVE
)
