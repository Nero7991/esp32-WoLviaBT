#include "ota_util.h"

#include <string.h>

ota_selftest_result_t ota_selftest_decide(bool pending, bool rollback_possible, bool devid_ok,
                                          bool ws_registered, uint32_t elapsed_ms,
                                          uint32_t timeout_ms)
{
    if (!pending) {
        return OTA_SELFTEST_NOT_NEEDED;
    }
    if (!rollback_possible) {
        /* No image to return to: failing would only strand the board on an
         * unverified image that esp_ota_begin refuses to replace. Keep it once
         * the server is reachable (DEVID ignored), or when the window ends. */
        return (ws_registered || elapsed_ms >= timeout_ms) ? OTA_SELFTEST_PASS
                                                           : OTA_SELFTEST_WAIT;
    }
    if (!devid_ok) {
        return OTA_SELFTEST_FAIL;
    }
    if (ws_registered) {
        return OTA_SELFTEST_PASS;
    }
    return (elapsed_ms >= timeout_ms) ? OTA_SELFTEST_FAIL : OTA_SELFTEST_WAIT;
}

const char *ota_selftest_kept_detail(bool rollback_possible, bool devid_ok, bool ws_registered)
{
    if (rollback_possible) {
        return NULL;
    }
    if (!devid_ok) {
        return "devid bad, kept: no rollback target";
    }
    if (!ws_registered) {
        return "ws timeout, kept: no rollback target";
    }
    return NULL;
}

static int b64_val(char c)
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

/* Decode base64 into out (NUL terminated). Returns length or -1 on invalid input,
 * overflow or an embedded NUL byte. After padding only more '=' may follow. */
static int b64_decode(const char *in, char *out, size_t out_len)
{
    size_t n = 0;
    uint32_t acc = 0;
    int bits = 0;

    for (; *in && *in != '='; in++) {
        int v = b64_val(*in);
        if (v < 0) {
            return -1;
        }
        acc = (acc << 6) | (uint32_t)v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            char c = (char)((acc >> bits) & 0xFF);
            if (c == '\0' || n + 1 >= out_len) {
                return -1;
            }
            out[n++] = c;
        }
    }
    for (; *in; in++) {
        if (*in != '=') {
            return -1;
        }
    }
    out[n] = '\0';
    return (int)n;
}

static bool scheme_is_basic(const char *h)
{
    static const char want[] = "basic ";
    for (size_t i = 0; i < sizeof(want) - 1; i++) {
        char c = h[i];
        if (c == '\0') {
            return false;
        }
        if (c >= 'A' && c <= 'Z') {
            c = (char)(c - 'A' + 'a');
        }
        if (c != want[i]) {
            return false;
        }
    }
    return true;
}

bool ota_basic_auth_ok(const char *auth_header, const char *user, const char *password)
{
    char decoded[160];
    char expected[160];

    if (!auth_header || !user || !password) {
        return false;
    }
    if (!scheme_is_basic(auth_header)) {
        return false;
    }
    int dli = b64_decode(auth_header + 6, decoded, sizeof(decoded));
    if (dli < 0) {
        return false;
    }
    size_t dl = (size_t)dli;
    if (memchr(decoded, ':', dl) == NULL) {
        return false;
    }
    size_t ul = strlen(user), pl = strlen(password);
    if (ul + 1 + pl + 1 > sizeof(expected)) {
        return false;
    }
    memcpy(expected, user, ul);
    expected[ul] = ':';
    memcpy(expected + ul + 1, password, pl + 1);

    /* constant-time compare */
    size_t el = ul + 1 + pl;
    unsigned diff = (unsigned)(dl ^ el);
    for (size_t i = 0; i < el && i < dl; i++) {
        diff |= (unsigned)((unsigned char)decoded[i] ^ (unsigned char)expected[i]);
    }
    return diff == 0;
}

int ota_resolve_url(const char *url, const char *server_uri, char *out, size_t out_len)
{
    if (!url || !server_uri || !out || out_len == 0) {
        return -1;
    }
    if (strncmp(url, "http://", 7) == 0 || strncmp(url, "https://", 8) == 0) {
        size_t l = strlen(url);
        if (l + 1 > out_len) {
            return -1;
        }
        memcpy(out, url, l + 1);
        return 0;
    }
    if (url[0] != '/') {
        return -1;
    }

    const char *scheme;
    const char *p;
    if (strncmp(server_uri, "wss://", 6) == 0) {
        scheme = "https://";
        p = server_uri + 6;
    } else if (strncmp(server_uri, "ws://", 5) == 0) {
        scheme = "http://";
        p = server_uri + 5;
    } else {
        return -1;
    }
    size_t hl = strcspn(p, "/?#");
    if (hl == 0) {
        return -1;
    }
    size_t sl = strlen(scheme), ul = strlen(url);
    if (sl + hl + ul + 1 > out_len) {
        return -1;
    }
    memcpy(out, scheme, sl);
    memcpy(out + sl, p, hl);
    memcpy(out + sl + hl, url, ul + 1);
    return 0;
}

bool ota_wake_window_expired(uint32_t woke_at_ms, uint32_t now_ms, uint32_t window_ms,
                             bool ota_received)
{
    if (ota_received) {
        return false;
    }
    return (uint32_t)(now_ms - woke_at_ms) >= window_ms;
}
