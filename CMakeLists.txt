# Toolchain: ESP-IDF v5.5.6 (shared install at ~/esp/v5.5.6/esp-idf). Target: esp32 (WT32-ETH01).
cmake_minimum_required(VERSION 3.16)

# sdkconfig.local (gitignored) holds secrets such as the OTA password.
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.local")
    set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.local")
endif()

# Optional power ledger: the component lives one level down in its repo.
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/components/esp-power-ledger/components/power_ledger")
    list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/components/esp-power-ledger/components/power_ledger")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32-wol-via-bt)
