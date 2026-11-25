#include <stdio.h>
#include <string.h>
#include "ota_util.h"

static int failures;
#define CHECK(cond) do { if (!(cond)) { \
    printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); failures++; } } while (0)

int main(void)
{
    char out[128];

    /* args: pending, rollback_possible, devid_ok, ws_registered, elapsed, timeout */
    CHECK(ota_selftest_decide(false, true, true, true, 0, 60000) == OTA_SELFTEST_NOT_NEEDED);
    CHECK(ota_selftest_decide(false, true, false, false, 99999, 60000) == OTA_SELFTEST_NOT_NEEDED);
    CHECK(ota_selftest_decide(false, false, false, false, 99999, 60000) == OTA_SELFTEST_NOT_NEEDED);
    CHECK(ota_selftest_decide(true, true, true, true, 5000, 60000) == OTA_SELFTEST_PASS);
    CHECK(ota_selftest_decide(true, true, true, false, 59999, 60000) == OTA_SELFTEST_WAIT);
    CHECK(ota_selftest_decide(true, true, true, false, 60000, 60000) == OTA_SELFTEST_FAIL);
    CHECK(ota_selftest_decide(true, true, false, true, 1000, 60000) == OTA_SELFTEST_FAIL);
    CHECK(ota_selftest_decide(true, true, false, false, 0, 60000) == OTA_SELFTEST_FAIL);
    /* No rollback target: never FAIL, DEVID ignored, keep at the window end. */
    CHECK(ota_selftest_decide(true, false, false, false, 1000, 60000) == OTA_SELFTEST_WAIT);
    CHECK(ota_selftest_decide(true, false, false, true, 1000, 60000) == OTA_SELFTEST_PASS);
    CHECK(ota_selftest_decide(true, false, true, true, 1000, 60000) == OTA_SELFTEST_PASS);
    CHECK(ota_selftest_decide(true, false, true, false, 59999, 60000) == OTA_SELFTEST_WAIT);
    CHECK(ota_selftest_decide(true, false, true, false, 60000, 60000) == OTA_SELFTEST_PASS);
    CHECK(ota_selftest_decide(true, false, false, false, 60000, 60000) == OTA_SELFTEST_PASS);

    CHECK(ota_selftest_kept_detail(true, true, true) == NULL);
    CHECK(ota_selftest_kept_detail(true, false, false) == NULL);
    CHECK(ota_selftest_kept_detail(false, true, true) == NULL);
    CHECK(strcmp(ota_selftest_kept_detail(false, false, true),
                 "devid bad, kept: no rollback target") == 0);
    CHECK(strcmp(ota_selftest_kept_detail(false, false, false),
                 "devid bad, kept: no rollback target") == 0);
    CHECK(strcmp(ota_selftest_kept_detail(false, true, false),
                 "ws timeout, kept: no rollback target") == 0);

    CHECK(ota_basic_auth_ok("Basic YWRtaW46cHc=", "admin", "pw") == true);
    CHECK(ota_basic_auth_ok("Basic YWRtaW46eHg=", "admin", "pw") == false);
    CHECK(ota_basic_auth_ok(NULL, "admin", "pw") == false);
    CHECK(ota_basic_auth_ok("Bearer x", "admin", "pw") == false);
    CHECK(ota_basic_auth_ok("Basic !!!", "admin", "pw") == false);
    CHECK(ota_basic_auth_ok("Basic YWRtaW4=", "admin", "pw") == false);

    CHECK(ota_basic_auth_ok("Basic YWRtaW46cHc=", "root", "pw") == false);
    CHECK(ota_basic_auth_ok("Basic YWRtaW46cHc=garbage", "admin", "pw") == false);
    CHECK(ota_basic_auth_ok("Basic YWRtaW46cHc==", "admin", "pw") == true);
    CHECK(ota_basic_auth_ok("basic YWRtaW46cHc=", "admin", "pw") == true);
    CHECK(ota_basic_auth_ok("BASIC YWRtaW46cHc=", "admin", "pw") == true);
    CHECK(ota_basic_auth_ok("Basic YWRtaW46", "admin", "") == true);
    CHECK(ota_basic_auth_ok("Basic YWRtaW46cHc=", "admin", "") == false);
    CHECK(ota_basic_auth_ok("Basic YQBiOnB3", "a", "pw") == false); /* embedded NUL */
    {
        char big[400];
        memcpy(big, "Basic ", 6);
        memset(big + 6, 'A', 300);
        big[306] = '\0';
        CHECK(ota_basic_auth_ok(big, "admin", "pw") == false);
    }

    CHECK(ota_resolve_url("/firmware/latest.bin", "ws://192.168.1.10:8080/ws", out, sizeof(out)) == 0);
    CHECK(strcmp(out, "http://192.168.1.10:8080/firmware/latest.bin") == 0);
    CHECK(ota_resolve_url("http://a/b.bin", "ws://h:1/ws", out, sizeof(out)) == 0);
    CHECK(strcmp(out, "http://a/b.bin") == 0);
    CHECK(ota_resolve_url("https://a/b.bin", "ws://h:1/ws", out, sizeof(out)) == 0);
    CHECK(strcmp(out, "https://a/b.bin") == 0);
    CHECK(ota_resolve_url("/x.bin", "wss://h:443/x", out, sizeof(out)) == 0);
    CHECK(strcmp(out, "https://h:443/x.bin") == 0);
    CHECK(ota_resolve_url("/x.bin", "ws://host/ws", out, sizeof(out)) == 0);
    CHECK(strcmp(out, "http://host/x.bin") == 0);
    CHECK(ota_resolve_url("/firmware/latest.bin", "ws://192.168.1.10:8080/ws", out, 10) == -1);
    CHECK(ota_resolve_url("http://a/b.bin", "ws://h:1/ws", out, 5) == -1);
    CHECK(ota_resolve_url("rel.bin", "ws://h:1/ws", out, sizeof(out)) == -1);
    CHECK(ota_resolve_url("/x", "garbage", out, sizeof(out)) == -1);
    CHECK(ota_resolve_url("/x", "ws://", out, sizeof(out)) == -1);

    CHECK(ota_wake_window_expired(0, 119999, 120000, false) == false);
    CHECK(ota_wake_window_expired(0, 120000, 120000, false) == true);
    CHECK(ota_wake_window_expired(0, 500000, 120000, true) == false);
    CHECK(ota_wake_window_expired(0xFFFFFF00u, 0x0001D3C0u, 120000, false) == true);
    CHECK(ota_wake_window_expired(0xFFFFFF00u, 0x0001D3BFu, 120000, false) == false);

    if (failures) {
        printf("%d failure(s)\n", failures);
        return 1;
    }
    printf("all tests passed\n");
    return 0;
}
