idf_component_register(
    SRCS "discovery.c" "espnow_manager.c" "forwarding.c" "neighbor_manager.c" "main.c"
    INCLUDE_DIRS "."
    REQUIRES esp_wifi esp_event esp_netif nvs_flash esp_timer
)
