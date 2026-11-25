#pragma once

/* Private to the ota component. Shared busy guard for push (ota_begin) and
 * pull (ota_pull_start): esp_https_ota owns its own esp_ota handle, so the pull
 * path cannot use s_active and claims the guard separately. */

#include <stdbool.h>

/* Atomically claim the guard. Returns false if an update is active or already claimed. */
bool ota_core_claim(void);

/* Release a claim taken with ota_core_claim. */
void ota_core_release(void);

/* Invoke the ota_set_begin_cb hook. Call without holding the guard mutex, once the
 * inactive slot is about to be overwritten. */
void ota_core_notify_begin(void);
