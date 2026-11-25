#include <string.h>
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ota.h"
#include "ota_httpd.h"

#define OTA_CRED_MAX   64
#define OTA_AUTH_MAX   256
#define OTA_CHUNK_SIZE 4096
#define ESP_IMAGE_MAGIC 0xE9
#define OTA_RECV_STALL_MS 20000 /* no-progress deadline, reset on any received bytes */

static const char *TAG = "ota_httpd";

static char s_user[OTA_CRED_MAX];
static char s_pass[OTA_CRED_MAX];
/* Static because the httpd task stack is small. Safe because the default httpd runs all
 * handlers on a single task (no concurrent handlers); revisit if that ever changes. */
static uint8_t s_chunk[OTA_CHUNK_SIZE];

/* Ask httpd to close the socket after the reply so an unread body is not drained. */
static void set_close(httpd_req_t *req)
{
    httpd_resp_set_hdr(req, "Connection", "close");
}

static esp_err_t send_401(httpd_req_t *req)
{
    set_close(req);
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"ota\"");
    httpd_resp_sendstr(req, "Unauthorized");
    return ESP_OK;
}

static bool auth_ok(httpd_req_t *req)
{
    size_t len = httpd_req_get_hdr_value_len(req, "Authorization");
    if (len == 0 || len >= OTA_AUTH_MAX) {
        return false;
    }
    char hdr[OTA_AUTH_MAX];
    if (httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) != ESP_OK) {
        return false;
    }
    return ota_basic_auth_ok(hdr, s_user, s_pass);
}

/* Fill buf with exactly want bytes. Retries recv timeouts. Returns false on error. */
static bool recv_full(httpd_req_t *req, uint8_t *buf, size_t want)
{
    size_t got = 0;
    int64_t last_progress = esp_timer_get_time();
    while (got < want) {
        int r = httpd_req_recv(req, (char *)buf + got, want - got);
        if (r == HTTPD_SOCK_ERR_TIMEOUT) {
            if ((esp_timer_get_time() - last_progress) > (int64_t)OTA_RECV_STALL_MS * 1000) {
                ESP_LOGE(TAG, "recv stalled for %d ms", OTA_RECV_STALL_MS);
                return false;
            }
            continue;
        }
        if (r <= 0) {
            return false;
        }
        got += (size_t)r;
        last_progress = esp_timer_get_time();
    }
    return true;
}

static esp_err_t ota_post(httpd_req_t *req)
{
    if (!auth_ok(req)) {
        return send_401(req);
    }
    if (req->content_len == 0) {
        set_close(req);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Empty body");
        return ESP_OK;
    }
    if (ota_is_busy()) { /* early-out only; ota_begin below is authoritative */
        set_close(req);
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, "OTA already in progress");
        return ESP_OK;
    }

    size_t remaining = req->content_len;
    size_t first = remaining < OTA_CHUNK_SIZE ? remaining : OTA_CHUNK_SIZE;
    if (!recv_full(req, s_chunk, first)) {
        ESP_LOGE(TAG, "recv failed on first chunk");
        return ESP_FAIL; /* closes the socket */
    }
    if (s_chunk[0] != ESP_IMAGE_MAGIC) {
        set_close(req);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Not an ESP firmware image");
        return ESP_OK;
    }

    esp_err_t err = ota_begin();
    if (err == ESP_ERR_INVALID_STATE) {
        set_close(req);
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_sendstr(req, "OTA already in progress");
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota_begin: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA begin failed");
        return ESP_OK;
    }

    size_t chunk = first;
    for (;;) {
        err = ota_write(s_chunk, chunk);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "ota_write: %s", esp_err_to_name(err));
            ota_abort();
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA write failed");
            return ESP_OK;
        }
        remaining -= chunk;
        if (remaining == 0) {
            break;
        }
        chunk = remaining < OTA_CHUNK_SIZE ? remaining : OTA_CHUNK_SIZE;
        if (!recv_full(req, s_chunk, chunk)) {
            ESP_LOGE(TAG, "recv failed mid-upload");
            ota_abort();
            return ESP_FAIL;
        }
    }

    err = ota_end();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "ota_end: %s", esp_err_to_name(err));
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "OTA validation failed");
        return ESP_OK;
    }

    httpd_resp_sendstr(req, "OTA OK, rebooting");
    /* Restart from the handler: the 200 is already queued; delay lets it flush. */
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

esp_err_t ota_httpd_register(httpd_handle_t server, const char *user, const char *password)
{
    if (!server || !user || !password || !*user) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strlen(user) >= OTA_CRED_MAX || strlen(password) >= OTA_CRED_MAX) {
        return ESP_ERR_INVALID_ARG;
    }
    strcpy(s_user, user);
    strcpy(s_pass, password);

    static const httpd_uri_t uri = {
        .uri = "/ota",
        .method = HTTP_POST,
        .handler = ota_post,
        .user_ctx = NULL,
    };
    return httpd_register_uri_handler(server, &uri);
}
