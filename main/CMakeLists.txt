idf_component_register(SRCS "main.c" "bt_wake.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES esp_eth esp_netif esp_event esp_http_server esp_timer driver lwip nvs_flash esp_wifi bt ota netserial)
