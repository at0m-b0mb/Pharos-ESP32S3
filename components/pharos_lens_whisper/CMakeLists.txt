idf_component_register(
    SRCS "lens_whisper.c"
    REQUIRES pharos_core pharos_engine
    PRIV_REQUIRES esp_codec_dev waveshare__esp32_s3_touch_amoled_1_75c
    WHOLE_ARCHIVE
)
