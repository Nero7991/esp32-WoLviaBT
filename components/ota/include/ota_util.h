#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef enum {
    OTA_SELFTEST_WAIT,
    OTA_SELFTEST_PASS,
    OTA_SELFTEST_FAIL,
    OTA_SELFTEST_NOT_NEEDED
} ota_selftest_result_t;

/* Decide the post-OTA self-test outcome.
 * rollback_possible: a valid image exists to return to. When false the result is
 * never FAIL: PASS once ws_registered (devid_ok ignored) or once elapsed_ms reaches
 * timeout_ms, WAIT before that. */
ota_selftest_result_t ota_selftest_decide(bool pending, bool rollback_possible, bool devid_ok,
                                          bool ws_registered, uint32_t elapsed_ms,
                                          uint32_t timeout_ms);

/* Detail for the "verified" status of an image that passed the self-test. NULL
 * for a normal pass. With no rollback target it names what the image did not
 * prove ("devid bad, kept: no rollback target", "ws timeout, kept: ..."), so the
 * operator sees an image that was kept only because there was nowhere to go. */
const char *ota_selftest_kept_detail(bool rollback_possible, bool devid_ok, bool ws_registered);

/* Check an HTTP Authorization header value ("Basic <b64>") against user/password. */
bool ota_basic_auth_ok(const char *auth_header, const char *user, const char *password);

/* Resolve a firmware url against the WebSocket server uri. Returns 0 or -1.
 * A wss:// server yields an https url, which the pull transport cannot fetch
 * (no certificate bundle); only ws:// / http:// deployments are supported. */
int ota_resolve_url(const char *url, const char *server_uri, char *out, size_t out_len);

/* Passive tag OTA wake window. True when no OTA was received and at least window_ms has
 * elapsed since woke_at_ms. Uses unsigned subtraction so a uint32 tick wrap is handled. */
bool ota_wake_window_expired(uint32_t woke_at_ms, uint32_t now_ms, uint32_t window_ms,
                             bool ota_received);
