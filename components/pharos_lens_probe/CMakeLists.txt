idf_component_register(
    SRCS "lens_probe.c"
    INCLUDE_DIRS "."
    REQUIRES pharos_core pharos_engine pharos_radio pharos_ui
    PRIV_REQUIRES esp_timer

    # A lens registers itself from a constructor, so nothing in the project
    # holds a reference to this translation unit. WHOLE_ARCHIVE is what stops
    # the linker discarding it - without it the component compiles, links
    # clean, and the device boots with the lens simply absent.
    WHOLE_ARCHIVE
)
