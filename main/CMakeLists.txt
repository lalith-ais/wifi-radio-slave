idf_component_register(SRCS "main.c" "wifi_mgr.c" "stream_player.c" "cli.c" "i2s_out.c"
                       INCLUDE_DIRS "."
                       REQUIRES esp_timer esp_wifi esp_http_client esp_audio_codec nvs_flash mbedtls driver)

# TODO next stage: add `esp_partition` / `spiffs` once the SPIFFS-backed
# WiFi credential + playlist store lands (WIFI.SET / PL.* persistence,
# and FACTORY actually wiping something).
