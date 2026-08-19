idf_component_register(
    SRCS "pharos_audio.c"
    INCLUDE_DIRS "include"
    REQUIRES nvs_flash
    PRIV_REQUIRES esp_codec_dev waveshare__esp32_s3_touch_amoled_1_75c
)
