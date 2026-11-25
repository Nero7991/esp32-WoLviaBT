/* ota_pull.c - OTA pull transport over esp_https_ota. Plain http only: no
 * certificate bundle is configured, so an https url fails at connect. */
#include <string.h>

#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ota.h"
#include "ota_internal.h"

#define OTA_PULL_URL_MAX 256

static const char *TAG = "ota_pull";

static ota_status_cb_t s_cb;
static char            s_url[OTA_PULL_URL_MAX + 1];

void ota_set_status_cb(ota_status_cb_t cb)
{
    s_cb = cb;
}

static void notify(ota_status_t st, const char *detail)
{
    ota_status_cb_t cb = s_cb;
    if (cb) {
        cb(st, detail);
    }
}

static void fail(esp_err_t err)
{
    ESP_LOGE(TAG, "pull failed: %s", esp_err_to_name(err));
    notify(OTA_STATUS_FAILED, esp_err_to_name(err));
    ota_core_release();
    vTaskDelete(NULL);
}

static void ota_pull_task(void *arg)
{
    (void)arg;
    esp_http_client_config_t http = {
        .url = s_url,
        .timeout_ms = 10000,
        .keep_alive_enable = true,
    };
    esp_https_ota_config_t cfg = { .http_config = &http };
    esp_https_ota_handle_t h = NULL;

    esp_err_t err = esp_https_ota_begin(&cfg, &h);
    if (err != ESP_OK) {
        fail(err);
        return;
    }
    notify(OTA_STATUS_DOWNLOADING, NULL);

    /* esp_https_ota_begin only connects and reads the image header. The first
     * esp_https_ota_perform calls esp_ota_begin (which erases the inactive slot)
     * and writes the first data, so the begin hook fires after that call
     * succeeds, not before: a pull that fails before touching the slot must not
     * clear it. */
    bool begun = false;
    do {
        err = esp_https_ota_perform(h);
        if (!begun && (err == ESP_ERR_HTTPS_OTA_IN_PROGRESS || err == ESP_OK)) {
            begun = true;
            ota_core_notify_begin();
        }
    } while (err == ESP_ERR_HTTPS_OTA_IN_PROGRESS);
    if (err == ESP_OK && !esp_https_ota_is_complete_data_received(h)) {
        err = ESP_ERR_INVALID_SIZE; /* connection ended early */
    }
    if (err != ESP_OK) {
        esp_https_ota_abort(h);
        fail(err);
        return;
    }
    /* finish validates the image and sets the boot partition; it frees h even on error. */
    err = esp_https_ota_finish(h);
    if (err != ESP_OK) {
        fail(err);
        return;
    }
    ESP_LOGI(TAG, "pull complete, restarting");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

esp_err_t ota_pull_start(const char *url)
{
    if (!url || strlen(url) > OTA_PULL_URL_MAX || url[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }
    if (!ota_core_claim()) {
        return ESP_ERR_INVALID_STATE;
    }
    strcpy(s_url, url);
    if (xTaskCreate(ota_pull_task, "ota_pull", 8192, NULL, 5, NULL) != pdPASS) {
        ota_core_release();
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
