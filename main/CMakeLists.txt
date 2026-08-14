idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES
        nvs_flash
        esp_event
        esp_netif
        pharos_core
        pharos_bsp
        pharos_ui
        pharos_radio
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
        pharos_lens_system
)
