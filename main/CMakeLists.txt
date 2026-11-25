set(reqs esp_eth esp_netif esp_event esp_http_server esp_timer driver lwip nvs_flash esp_wifi bt
         ota netserial esp-bringup)
# Optional, private components, used when checked out (or symlinked) under components/:
#   components/esp-power         frequency scaling and GET /api/power
#   components/esp-power-ledger  power-state counters (the component is one level down)
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../components/esp-power/CMakeLists.txt")
    list(APPEND reqs esp-power)
endif()
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/../components/esp-power-ledger/components/power_ledger")
    list(APPEND reqs power_ledger)
endif()
idf_component_register(SRCS "main.c" "bt_wake.c"
                       INCLUDE_DIRS "."
                       PRIV_REQUIRES ${reqs})
