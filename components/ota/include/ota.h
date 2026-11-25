#pragma once

/* OTA component umbrella header. Streaming write to the inactive OTA slot;
 * ota_end validates the image before the boot partition is switched. The new
 * image must call ota_mark_valid after its self-test or the bootloader rolls back. */

#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#include "ota_util.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t ota_begin(void); /* ESP_ERR_INVALID_STATE if an update is active */
esp_err_t ota_write(const void *data, size_t len);
/* Validate the image, then set the boot partition. On success the busy guard stays
 * held (ota_is_busy() true) until restart: the caller must restart. */
esp_err_t ota_end(void);
void ota_abort(void);
bool ota_is_busy(void);

/* True if the running image awaits verification (ESP_OTA_IMG_PENDING_VERIFY).
 * Blank otadata (ESP_ERR_NOT_FOUND) counts as not pending. */
bool ota_pending_verify(void);

/* Cancel the pending rollback for the running image. */
esp_err_t ota_mark_valid(void);

/* Mark running image invalid and reboot into the previous one. Returns only when
 * no other valid image exists or the otadata rewrite failed; the caller decides
 * whether to keep the image or restart (check ota_rollback_possible). */
esp_err_t ota_rollback(void);

/* True if a valid image exists to roll back to (esp_ota_check_rollback_is_possible).
 * Validates that image (sha over ~1 MB of flash): call from a task with a 4 KB+
 * stack, not from an esp_timer callback or ISR. */
bool ota_rollback_possible(void);

/* True if the non-running OTA slot is INVALID or ABORTED. Sticky: stays true until a
 * later OTA overwrites that slot; it is not a per-boot signal. */
bool ota_previous_rolled_back(void);

/* Called from the updating task (httpd for push, "ota_pull" for pull) once an
 * update starts overwriting the inactive slot. Set it before any update can start. */
typedef void (*ota_begin_cb_t)(void);
void ota_set_begin_cb(ota_begin_cb_t cb);

/* Pull transport: download from a plain http URL with esp_https_ota, then restart.
 * https is not supported (no certificate bundle is configured).
 * The url is copied (max 256 chars, longer returns ESP_ERR_INVALID_ARG).
 * Returns ESP_ERR_INVALID_STATE if an update is active, ESP_ERR_NO_MEM if the
 * "ota_pull" task cannot be created. On success the board restarts after 500 ms. */
typedef enum { OTA_STATUS_DOWNLOADING, OTA_STATUS_FAILED } ota_status_t;

/* detail is NULL for DOWNLOADING and esp_err_to_name() text for FAILED. The callback is
 * invoked from the "ota_pull" task context, not the caller's. Set it before ota_pull_start. */
typedef void (*ota_status_cb_t)(ota_status_t status, const char *detail);

void ota_set_status_cb(ota_status_cb_t cb);
esp_err_t ota_pull_start(const char *url);

const char *ota_running_label(void);   /* e.g. "ota_0" */
const char *ota_running_version(void); /* app description version */

#ifdef __cplusplus
}
#endif
