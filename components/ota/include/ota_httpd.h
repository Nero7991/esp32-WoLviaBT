#pragma once

#include "esp_err.h"
#include "esp_http_server.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Register POST /ota (raw firmware image body, HTTP Basic auth) on the given server.
 * user and password are copied into static storage (max 63 chars each). */
esp_err_t ota_httpd_register(httpd_handle_t server, const char *user, const char *password);

#ifdef __cplusplus
}
#endif
