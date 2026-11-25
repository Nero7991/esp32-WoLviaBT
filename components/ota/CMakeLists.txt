idf_component_register(
    SRCS "ota_util.c" "ota_core.c" "ota_httpd.c" "ota_pull.c"
    INCLUDE_DIRS "include"
    # esp_http_server is public: ota_httpd.h exposes httpd_handle_t
    REQUIRES app_update esp_app_format esp_http_server
    PRIV_REQUIRES esp_timer esp_https_ota esp_http_client
)
