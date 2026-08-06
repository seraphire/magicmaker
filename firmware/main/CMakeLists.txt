idf_component_register(
    SRCS
        "main.c"
        "audio.c"
        "leds.c"
        "trigger.c"
        "pn532.c"
        "rc522.c"
        "store.c"
        "appcfg.c"
        "wifi.c"
        "portal.c"
        "cli.c"
        "ntp.c"
        "ota.c"
        "verscmp.c"
        "webota.c"
        "assets.c"
        "countdown.c"
        "occasions.c"
        "needhelp.c"
        "sounds.c"
        "bands.c"
    INCLUDE_DIRS "."
    REQUIRES driver esp_timer spiffs esp_hw_support esp_driver_i2c esp_driver_spi esp_adc nvs_flash
             esp_wifi esp_netif esp_event esp_http_server lwip console
             esp_http_client esp-tls mbedtls json app_update esp_app_format esp_https_ota
)

# Build the LittleFS data image from ./spiffs (WAVs + www/ setup page) and
# flash it alongside the app. FLASH_IN_PROJECT ties it into the normal build so
# `idf.py flash` writes the data image too - no manual step.
littlefs_create_partition_image(storage ../spiffs FLASH_IN_PROJECT)
