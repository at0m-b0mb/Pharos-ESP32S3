idf_component_register(
    SRCS
        "pharos_dial.c"
        "pharos_hud.c"
        "pharos_round.c"
        "pharos_ui.c"
    INCLUDE_DIRS "include"
    REQUIRES pharos_core pharos_bsp pharos_radio
    PRIV_REQUIRES esp_timer
)
