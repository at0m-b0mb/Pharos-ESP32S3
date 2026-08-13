idf_component_register(
    SRCS
        "pharos_dial.c"
        "pharos_round.c"
        "pharos_ui.c"
    INCLUDE_DIRS "include"
    REQUIRES pharos_core pharos_bsp
    PRIV_REQUIRES esp_timer
)
