idf_component_register(
    SRCS
        "pharos_dial.c"
        "pharos_hud.c"
        "pharos_round.c"
        "pharos_style.c"
        "pharos_theme.c"
        "pharos_ui.c"
    INCLUDE_DIRS "include"
    REQUIRES pharos_core pharos_bsp pharos_radio pharos_engine pharos_audio
    PRIV_REQUIRES esp_timer driver esp_driver_gpio nvs_flash
)
