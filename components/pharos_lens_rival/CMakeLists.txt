idf_component_register(
    SRCS "lens_rival.c"
    INCLUDE_DIRS "."
    REQUIRES pharos_core pharos_engine pharos_radio
    PRIV_REQUIRES esp_timer

    # A lens registers itself from a constructor, so nothing references this
    # translation unit. WHOLE_ARCHIVE is what stops the linker discarding it.
    WHOLE_ARCHIVE
)
