idf_component_register(SRCS "main.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_lcd_jd9365 avi_player esp_driver_jpeg esp_psram esp_timer esp_partition
                       EMBED_FILES "diagnostic.jpg")
