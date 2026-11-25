/* ota_core.c - OTA update with rollback (ported from quickvolt qv_ota.c). */
#include "ota.h"
#include "ota_internal.h"

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "ota";

static esp_ota_handle_t       s_handle;
static const esp_partition_t *s_part;
static bool                   s_active;
static bool                   s_claimed; /* pull transport owns the update */
static SemaphoreHandle_t      s_lock;
static portMUX_TYPE           s_init_mux = portMUX_INITIALIZER_UNLOCKED;
static ota_begin_cb_t         s_begin_cb;

void ota_set_begin_cb(ota_begin_cb_t cb)
{
    s_begin_cb = cb;
}

void ota_core_notify_begin(void)
{
    ota_begin_cb_t cb = s_begin_cb;
    if (cb) {
        cb();
    }
}

/* Create the guard mutex on first use. Returns false if it cannot be created. */
static bool lock_take(void)
{
    if (!s_lock) {
        SemaphoreHandle_t m = xSemaphoreCreateMutex();
        if (!m) {
            return false;
        }
        portENTER_CRITICAL(&s_init_mux);
        if (!s_lock) {
            s_lock = m;
            m = NULL;
        }
        portEXIT_CRITICAL(&s_init_mux);
        if (m) {
            vSemaphoreDelete(m);
        }
    }
    return xSemaphoreTake(s_lock, portMAX_DELAY) == pdTRUE;
}

static void lock_give(void)
{
    xSemaphoreGive(s_lock);
}

bool ota_core_claim(void)
{
    if (!lock_take()) {
        return false;
    }
    bool ok = !s_active && !s_claimed;
    if (ok) {
        s_claimed = true;
    }
    lock_give();
    return ok;
}

void ota_core_release(void)
{
    if (!lock_take()) {
        return;
    }
    s_claimed = false;
    lock_give();
}

esp_err_t ota_begin(void)
{
    if (!lock_take()) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t ret = ESP_OK;
    if (s_active || s_claimed) {
        ret = ESP_ERR_INVALID_STATE;
        goto out;
    }
    s_part = esp_ota_get_next_update_partition(NULL);
    if (!s_part) {
        ESP_LOGE(TAG, "no OTA partition available");
        ret = ESP_FAIL;
        goto out;
    }
    ret = esp_ota_begin(s_part, OTA_SIZE_UNKNOWN, &s_handle);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed (%d)", (int)ret);
        goto out;
    }
    s_active = true;
    ESP_LOGI(TAG, "OTA begin -> %s", s_part->label);
out:
    lock_give();
    if (ret == ESP_OK) {
        ota_core_notify_begin(); /* outside the lock: the hook may touch NVS */
    }
    return ret;
}

esp_err_t ota_write(const void *data, size_t len)
{
    if (!lock_take()) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t ret = ESP_ERR_INVALID_STATE;
    if (s_active) {
        ret = esp_ota_write(s_handle, data, len);
    }
    lock_give();
    return ret;
}

void ota_abort(void)
{
    if (!lock_take()) {
        ESP_LOGE(TAG, "abort: cannot create guard mutex, update not aborted");
        return;
    }
    if (s_active) {
        esp_ota_abort(s_handle);
        s_active = false;
        ESP_LOGW(TAG, "OTA aborted");
    }
    lock_give();
}

esp_err_t ota_end(void)
{
    if (!lock_take()) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t ret = ESP_OK;
    if (!s_active) {
        ret = ESP_ERR_INVALID_STATE;
        goto out;
    }
    s_active = false;

    ret = esp_ota_end(s_handle); /* validates magic/size/sha */
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "image validation failed (%d)", (int)ret);
        goto out;
    }
    ret = esp_ota_set_boot_partition(s_part);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "set boot partition failed (%d)", (int)ret);
        goto out;
    }
    /* Keep the guard held until the caller restarts: a pull must not claim and
     * erase the slot that was just set to boot. */
    s_claimed = true;
    ESP_LOGI(TAG, "OTA complete, next boot from %s", s_part->label);
out:
    lock_give();
    return ret;
}

bool ota_is_busy(void)
{
    return s_active || s_claimed;
}

bool ota_pending_verify(void)
{
    const esp_partition_t *p = esp_ota_get_running_partition();
    esp_ota_img_states_t st;
    if (!p) {
        return false;
    }
    /* ESP_ERR_NOT_FOUND: USB-flashed board with blank otadata, not pending. */
    if (esp_ota_get_state_partition(p, &st) != ESP_OK) {
        return false;
    }
    return st == ESP_OTA_IMG_PENDING_VERIFY;
}

esp_err_t ota_mark_valid(void)
{
    return esp_ota_mark_app_valid_cancel_rollback();
}

esp_err_t ota_rollback(void)
{
    /* Reboots into the previous image on success (does not return). Returns when
     * no other valid image exists, or when the otadata rewrite fails; the caller
     * decides whether to keep the image or restart. A restart is loop-free: the
     * bootloader rewrites PENDING_VERIFY to ABORTED, and an ABORTED image that
     * boots again is not pending. */
    esp_err_t e = esp_ota_mark_app_invalid_rollback_and_reboot();
    ESP_LOGE(TAG, "rollback could not proceed (%s)", esp_err_to_name(e));
    return e != ESP_OK ? e : ESP_FAIL;
}

bool ota_rollback_possible(void)
{
    return esp_ota_check_rollback_is_possible();
}

bool ota_previous_rolled_back(void)
{
    const esp_partition_t *other = esp_ota_get_next_update_partition(NULL);
    esp_ota_img_states_t st;
    if (!other || esp_ota_get_state_partition(other, &st) != ESP_OK) {
        return false;
    }
    return st == ESP_OTA_IMG_INVALID || st == ESP_OTA_IMG_ABORTED;
}

const char *ota_running_label(void)
{
    const esp_partition_t *p = esp_ota_get_running_partition();
    return p ? p->label : "?";
}

const char *ota_running_version(void)
{
    const esp_app_desc_t *d = esp_app_get_description();
    return d ? d->version : "?";
}
