idf_component_register(
    SRCS "main.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_timer
        esp_netif
        esp_wifi
        esp_event
        esp_http_server
        esp_media_protocols
        nvs_flash
        esp_driver_gpio
)
