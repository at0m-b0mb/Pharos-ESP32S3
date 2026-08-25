idf_component_register(
    SRCS "pharos_sense.c"
    INCLUDE_DIRS "include"
    REQUIRES pharos_engine pharos_bsp
    PRIV_REQUIRES esp_timer
)
