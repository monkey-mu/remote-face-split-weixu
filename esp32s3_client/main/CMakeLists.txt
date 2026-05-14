idf_component_register(
    SRCS "main.c" "init_board.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_http_client nvs_flash json esp_event esp_netif esp_timer driver esp_lcd
)
