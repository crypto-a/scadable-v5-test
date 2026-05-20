idf_component_register(
    SRCS
        "main.c"
        "wifi.c"
        "log_sink.c"
        "heartbeat.c"
        "scadable_client.c"
        "buggy.c"
    INCLUDE_DIRS "."
    REQUIRES
        esp_wifi
        esp_event
        esp_netif
        nvs_flash
        json
        esp_timer
        esp_http_client
)
