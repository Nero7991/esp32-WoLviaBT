// WT32-ETH01: bring up LAN8720 Ethernet and send Wake-on-LAN magic packets on request.
//
// Board wiring (fixed on the WT32-ETH01):
//   LAN8720 PHY address 1, MDC GPIO23, MDIO GPIO18,
//   50 MHz RMII clock from an external oscillator into GPIO0,
//   oscillator enable on GPIO16 (must go high before the EMAC starts).

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/gpio.h"
#include "esp_check.h"
#include "esp_eth.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_http_server.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "ota.h"
#include "ota_util.h"
#include "nvs.h"
#include "bt_wake.h"
#include "ping/ping_sock.h"
#include "ota_httpd.h"
#include "netserial.h"
#include "nvs_flash.h"

#include "bringup.h"

// Optional, git-ignored: build-time Wi-Fi credentials used only to seed the settings on a board
// that has none (see settings_seed).
#if __has_include("wifi_creds_local.h")
#include "wifi_creds_local.h"
#endif

static const char *TAG = "wol";

#define PIN_OSC_ENABLE 16
#define PHY_ADDR       1
#define PIN_MDC        23
#define PIN_MDIO       18

static volatile bool s_got_ip;
static volatile bool s_link_up;
static char s_ip[16] = "none";
static esp_eth_handle_t s_eth;
static esp_netif_t *s_eth_netif;
static volatile int64_t s_host_ping_ok_ms = -1;
static esp_ping_handle_t s_ping;
static bool s_bt_running;

// Target MAC: NVS ("wol"/"mac") overrides CONFIG_WOL_TARGET_MAC. All zeros means unset.
static uint8_t s_target[6];

#define NVS_NS  "wol"
#define NVS_KEY "mac"

static int parse_mac(const char *s, uint8_t out[6])
{
    unsigned v[6];
    char tail;
    if (sscanf(s, "%x:%x:%x:%x:%x:%x%c", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &tail) != 6) {
        return -1;
    }
    for (int i = 0; i < 6; i++) {
        if (v[i] > 0xff) {
            return -1;
        }
        out[i] = (uint8_t)v[i];
    }
    return 0;
}

static bool mac_is_set(const uint8_t m[6])
{
    static const uint8_t zero[6];
    return memcmp(m, zero, 6) != 0;
}

static void mac_str(const uint8_t m[6], char out[18])
{
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
}

static void target_load(void)
{
    if (!bringup_get_mac("wol_mac", s_target)) {
        memset(s_target, 0, sizeof(s_target));
    }
}

static esp_err_t target_save(const uint8_t m[6])
{
    esp_err_t e = bringup_set_mac("wol_mac", m);
    if (e == ESP_OK) {
        memcpy(s_target, m, 6);
    }
    return e;
}

static bool nvs_get_n(const char *key, uint8_t *out, size_t n)
{
    nvs_handle_t h;
    size_t len = n;
    bool ok = false;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        ok = nvs_get_blob(h, key, out, &len) == ESP_OK && len == n;
        nvs_close(h);
    }
    return ok;
}

static int hexval(char c)
{
    return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10
         : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
}

static int parse_key(const char *s, uint8_t out[16])
{
    if (strlen(s) != 32) {
        return -1;
    }
    for (int i = 0; i < 16; i++) {
        int hi = hexval(s[2 * i]), lo = hexval(s[2 * i + 1]);
        if (hi < 0 || lo < 0) {
            return -1;
        }
        out[i] = (uint8_t)(hi << 4 | lo);
    }
    return 0;
}

static esp_err_t nvs_set_val(const char *key, const void *v, size_t len)
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NS, NVS_READWRITE, &h), TAG, "nvs_open");
    esp_err_t e = len ? nvs_set_blob(h, key, v, len) : ESP_ERR_INVALID_ARG;
    if (e == ESP_OK) {
        e = nvs_commit(h);
    }
    nvs_close(h);
    return e;
}

static uint8_t nvs_get_byte(const char *key, uint8_t def)
{
    uint8_t v = def;
    size_t len = 1;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_blob(h, key, &v, &len) != ESP_OK || len != 1) {
            v = def;
        }
        nvs_close(h);
    }
    return v;
}

// ---- settings (esp-bringup) ----

#define SET_NS "wolbt"

static const bringup_field_t s_fields[] = {
    { .key = "bt_host", .label = "Host Bluetooth adapter", .group = "Bluetooth",
      .help = "The host's adapter address: bluetoothctl show", .type = BRINGUP_T_MAC,
      .required = true },
    { .key = "bt_pad", .label = "Controller address", .group = "Bluetooth",
      .help = "The DualSense paired with the host: bluetoothctl devices Paired",
      .type = BRINGUP_T_MAC, .required = true },
    { .key = "bt_lk", .label = "Controller link key", .group = "Bluetooth",
      .help = "Only for the accept-mode experiments. From /var/lib/bluetooth/<adapter>/<pad>/info",
      .type = BRINGUP_T_HEX, .max = 16, .secret = true },
    { .key = "wol_mac", .label = "Host wired MAC (Wake-on-LAN)", .group = "Wake",
      .help = "The host's Ethernet MAC, the one cabled to this board", .type = BRINGUP_T_MAC,
      .required = true },
};

static void mac_to_str(const uint8_t m[6], char out[18])
{
    snprintf(out, 18, "%02x:%02x:%02x:%02x:%02x:%02x", m[0], m[1], m[2], m[3], m[4], m[5]);
}

// Fill settings on a board that has none: build-time Wi-Fi and admin password, and values the
// firmware before esp-bringup kept as blobs in the "wol" namespace.
static void settings_seed(void)
{
#ifdef WIFI_SSID
    bringup_seed(BRINGUP_KEY_WIFI_SSID, WIFI_SSID);
    bringup_seed(BRINGUP_KEY_WIFI_PASS, WIFI_PASSWORD);
#endif
    if (strlen(CONFIG_OTA_PASSWORD) >= 8) {
        bringup_seed(BRINGUP_KEY_ADMIN_PASS, CONFIG_OTA_PASSWORD);
    }
    uint8_t b[16];
    char s[40];
    if (nvs_get_n("bt_host", b, 6)) { mac_to_str(b, s); bringup_seed("bt_host", s); }
    if (nvs_get_n("bt_pad", b, 6))  { mac_to_str(b, s); bringup_seed("bt_pad", s); }
    if (nvs_get_n("mac", b, 6))     { mac_to_str(b, s); bringup_seed("wol_mac", s); }
    if (nvs_get_n("bt_lk", b, 16)) {
        for (int i = 0; i < 16; i++) {
            snprintf(&s[2 * i], 3, "%02x", b[i]);
        }
        bringup_seed("bt_lk", s);
        memset(s, 0, sizeof(s));
    }
    memset(b, 0, sizeof(b));
}

static const bringup_config_t s_setup = {
    .product = "WoL via BT",
    .ap_ssid_prefix = "WoLviaBT",
    .ap_password = "wolviabt",
    .hostname_prefix = "wolviabt",
    .nvs_namespace = SET_NS,
    .fields = s_fields,
    .n_fields = sizeof(s_fields) / sizeof(s_fields[0]),
    .on_seed = settings_seed,
};

// Magic packet as a raw Ethernet frame (EtherType 0x0842) to the broadcast address.
// Needs only the Ethernet link: no IP, DHCP or route, so it works when the target is off
// and cannot leave through Wi-Fi.
static esp_err_t wol_send(const uint8_t target[6])
{
    if (!s_eth) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!s_link_up) {
        ESP_LOGW(TAG, "magic packet not sent: ethernet link down");
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t frame[14 + 102];
    memset(frame, 0xff, 6);                                  // dst: broadcast
    ESP_RETURN_ON_ERROR(esp_eth_ioctl(s_eth, ETH_CMD_G_MAC_ADDR, &frame[6]), TAG, "own mac");
    frame[12] = 0x08;                                        // EtherType 0x0842 (Wake-on-LAN)
    frame[13] = 0x42;
    uint8_t *p = &frame[14];
    memset(p, 0xff, 6);
    for (int i = 0; i < 16; i++) {
        memcpy(&p[6 + i * 6], target, 6);
    }
    ESP_RETURN_ON_ERROR(esp_eth_transmit(s_eth, frame, sizeof(frame)), TAG, "transmit");
    char m[18];
    mac_str(target, m);
    ESP_LOGI(TAG, "magic packet sent to %s", m);
    return ESP_OK;
}

static void on_eth_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    switch (id) {
    case ETHERNET_EVENT_CONNECTED:
        s_link_up = true;
        ESP_LOGI(TAG, "link up");
        esp_netif_create_ip6_linklocal(s_eth_netif);
        break;
    case ETHERNET_EVENT_DISCONNECTED: s_link_up = false; ESP_LOGI(TAG, "link down"); break;
    default: break;
    }
}

// Host check: ping the IPv6 all-nodes address ff02::1 on the Ethernet link. A running Linux
// host answers from its link-local address with no network configuration at all (it needs
// only the interface up); a powered-off host cannot. Replies from our own address are ignored.
static void on_ping_ok(esp_ping_handle_t h, void *arg)
{
    ip_addr_t from;
    esp_ping_get_profile(h, ESP_PING_PROF_IPADDR, &from, sizeof(from));
    esp_ip6_addr_t own;
    if (from.type == IPADDR_TYPE_V6 && esp_netif_get_ip6_linklocal(s_eth_netif, &own) == ESP_OK &&
        memcmp(from.u_addr.ip6.addr, own.addr, sizeof(own.addr)) == 0) {
        return;
    }
    s_host_ping_ok_ms = esp_timer_get_time() / 1000;
}

static void on_got_ip6(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    const ip_event_got_ip6_t *ev = data;
    if (ev->esp_netif != s_eth_netif || s_ping) {
        return;
    }
    ESP_LOGI(TAG, "ethernet ipv6 " IPV6STR, IPV62STR(ev->ip6_info.ip));
    esp_ping_config_t pc = ESP_PING_DEFAULT_CONFIG();
    pc.target_addr.type = IPADDR_TYPE_V6;
    pc.target_addr.u_addr.ip6.addr[0] = PP_HTONL(0xff020000UL);
    pc.target_addr.u_addr.ip6.addr[1] = 0;
    pc.target_addr.u_addr.ip6.addr[2] = 0;
    pc.target_addr.u_addr.ip6.addr[3] = PP_HTONL(1UL);
    pc.count = ESP_PING_COUNT_INFINITE;
    pc.interval_ms = 2000;
    pc.timeout_ms = 1000;
    pc.interface = esp_netif_get_netif_impl_index(s_eth_netif);
    esp_ping_callbacks_t cb = { .on_ping_success = on_ping_ok };
    if (esp_ping_new_session(&pc, &cb, &s_ping) == ESP_OK && esp_ping_start(s_ping) == ESP_OK) {
        ESP_LOGI(TAG, "host check: pinging ff02::1 on ethernet");
    } else {
        ESP_LOGE(TAG, "host check: ping session failed");
    }
}

static void on_press(void)
{
    if (!mac_is_set(s_target)) {
        ESP_LOGW(TAG, "press detected but no WoL target set");
        return;
    }
    wol_send(s_target);
}

// Host is up while something other than us answered ff02::1 on the Ethernet link in the last 8 s.
static void host_watch_task(void *arg)
{
    for (;;) {
        int64_t ok = s_host_ping_ok_ms;
        bool up = ok >= 0 && esp_timer_get_time() / 1000 - ok < 8000;
        if (s_bt_running) {
            bt_wake_set_host_up(up);
            bt_wake_tick();
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void bt_start_from_nvs(void)
{
    uint8_t host[6], pad[6];
    if (!bringup_get_mac("bt_host", host) || !bringup_get_mac("bt_pad", pad)) {
        ESP_LOGW(TAG, "bluetooth listener off: set the host adapter and controller in /setup");
        return;
    }
    bt_wake_mode_t mode = (bt_wake_mode_t)nvs_get_byte("bt_mode", BT_WAKE_REJECT);
    bt_scan_policy_t pol = (bt_scan_policy_t)nvs_get_byte("bt_scan", BT_SCAN_AUTO);
    uint8_t ho = nvs_get_byte("bt_ho_s", 0xFF); // 0xFF: automatic handover off
    bt_wake_set_handover(ho == 0xFF ? -1 : ho, (bt_drop_t)nvs_get_byte("bt_ho_m", BT_DROP_RESET));
    bt_wake_set_scan_policy(pol);
    uint8_t key[16];
    bool have_key = bringup_get_hex("bt_lk", key, 16);
    esp_err_t e = bt_wake_start(host, pad, have_key ? key : NULL, mode, on_press);
    memset(key, 0, sizeof(key));
    if (e != ESP_OK) {
        ESP_LOGE(TAG, "bluetooth listener failed: %s", esp_err_to_name(e));
        return;
    }
    s_bt_running = true;
}

static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    const ip_event_got_ip_t *ev = data;
    ESP_LOGI(TAG, "ip " IPSTR " gw " IPSTR, IP2STR(&ev->ip_info.ip), IP2STR(&ev->ip_info.gw));
    snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ev->ip_info.ip));
    s_got_ip = true;

}

static esp_err_t eth_start(void)
{
    // The PHY's 50 MHz oscillator drives GPIO0, which is also the boot strap pin.
    // It stays off through reset (so IO0 can be strapped low to flash) and is enabled here.
    gpio_reset_pin(PIN_OSC_ENABLE);
    gpio_set_direction(PIN_OSC_ENABLE, GPIO_MODE_OUTPUT);
    gpio_set_level(PIN_OSC_ENABLE, 1);
    vTaskDelay(pdMS_TO_TICKS(10));

    eth_mac_config_t mac_cfg = ETH_MAC_DEFAULT_CONFIG();
    eth_esp32_emac_config_t emac_cfg = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    emac_cfg.smi_gpio.mdc_num = PIN_MDC;
    emac_cfg.smi_gpio.mdio_num = PIN_MDIO;
    emac_cfg.clock_config.rmii.clock_mode = EMAC_CLK_EXT_IN;
    emac_cfg.clock_config.rmii.clock_gpio = (emac_rmii_clock_gpio_t)EMAC_CLK_IN_GPIO;

    eth_phy_config_t phy_cfg = ETH_PHY_DEFAULT_CONFIG();
    phy_cfg.phy_addr = PHY_ADDR;
    phy_cfg.reset_gpio_num = -1;

    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_cfg, &mac_cfg);
    esp_eth_phy_t *phy = esp_eth_phy_new_lan87xx(&phy_cfg);
    ESP_RETURN_ON_FALSE(mac && phy, ESP_FAIL, TAG, "mac/phy alloc failed");

    esp_eth_config_t cfg = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth = NULL;
    ESP_RETURN_ON_ERROR(esp_eth_driver_install(&cfg, &eth), TAG, "driver install");
    s_eth = eth;

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *netif = esp_netif_new(&netif_cfg);
    s_eth_netif = netif;
    ESP_RETURN_ON_ERROR(esp_netif_attach(netif, esp_eth_new_netif_glue(eth)), TAG, "netif attach");

    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, on_eth_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, on_got_ip, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_GOT_IP6, on_got_ip6, NULL));
    return esp_eth_start(eth);
}


static esp_err_t status_get(httpd_req_t *req)
{
    char body[1152], m[18], bt[768] = "null";
    if (s_bt_running) {
        bt_wake_status_json(bt, sizeof(bt));
    }
    mac_str(s_target, m);
    snprintf(body, sizeof(body),
             "{\"app\":\"esp32-wol-via-bt\",\"version\":\"%s\",\"partition\":\"%s\","
             "\"ip\":\"%s\",\"eth_link\":%s,\"wol_target\":\"%s\",\"uptime_s\":%lld,"
             "\"setup\":\"%s\",\"bt\":%s}\n",
             ota_running_version(), ota_running_label(), s_ip, s_link_up ? "true" : "false",
             mac_is_set(s_target) ? m : "unset", (long long)(esp_timer_get_time() / 1000000),
             bringup_active() ? bringup_reason() : "", bt);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
}

static bool req_auth_ok(httpd_req_t *req)
{
    return bringup_auth(req); // admin:<admin_pass setting>; sends the 401 itself
}

// POST /bt with any subset of:
//   host=..  pad=..            addresses (a new host address restarts: BD_ADDR is latched at init)
//   mode=reject|silent|accept|probe  how to answer the pad's page (probe: measurement only)
//   scan=auto|on|off           page scan policy
//   lk=<32 hex>                link key for the pad, as in BlueZ's info file (never echoed)
//   handover=<s>|off  method=detach|reset|radio   automatic drop after the host comes up
//   drop=detach[&reason=hex]|reset|radio|l2cap   let go of a held pad now (radio=on re-enables)
//   role=central|peripheral    role requested when accepting (not stored)
//   reject_reason=<hex>        reason for reject mode, default 0F (not stored)
//   cutoff=0..6                accept-mode measurement stage, see bt_wake.h (not stored)
// Everything except drop and radio is stored in NVS.
static esp_err_t bt_post(httpd_req_t *req)
{
    if (!req_auth_ok(req)) {
        return ESP_OK;
    }
    char q[200], v[32];
    if (httpd_req_get_url_query_str(req, q, sizeof(q)) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "see the comment above bt_post");
        return ESP_OK;
    }
    bool restart = false;
    uint8_t m[6];
    if (httpd_query_key_value(q, "pad", v, sizeof(v)) == ESP_OK) {
        if (parse_mac(v, m) != 0 || bringup_set_mac("bt_pad", m) != ESP_OK) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad pad");
            return ESP_OK;
        }
        if (s_bt_running) {
            bt_wake_set_pad(m);
        } else {
            restart = true;
        }
    }
    if (httpd_query_key_value(q, "host", v, sizeof(v)) == ESP_OK) {
        uint8_t old[6];
        bool had = bringup_get_mac("bt_host", old);
        if (parse_mac(v, m) != 0 || bringup_set_mac("bt_host", m) != ESP_OK) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "bad host");
            return ESP_OK;
        }
        if (!had || !s_bt_running || memcmp(old, m, 6) != 0) {
            restart = true;
        }
    }
    char kv[40];
    if (httpd_query_key_value(q, "lk", kv, sizeof(kv)) == ESP_OK) {
        uint8_t key[16];
        if (parse_key(kv, key) != 0 || bringup_set_hex("bt_lk", key, 16) != ESP_OK) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "lk must be 32 hex digits");
            return ESP_OK;
        }
        if (s_bt_running) {
            bt_wake_set_link_key(key);
        }
        memset(key, 0, sizeof(key));
        memset(kv, 0, sizeof(kv));
        memset(q, 0, sizeof(q));
        return httpd_resp_sendstr(req, "link key saved\n");
    }
    if (httpd_query_key_value(q, "handover", v, sizeof(v)) == ESP_OK) {
        int sec = strcmp(v, "off") == 0 ? -1 : atoi(v);
        uint8_t b = sec < 0 || sec > 254 ? 0xFF : (uint8_t)sec;
        nvs_set_val("bt_ho_s", &b, 1);
    }
    if (httpd_query_key_value(q, "method", v, sizeof(v)) == ESP_OK) {
        uint8_t mt = strcmp(v, "detach") == 0 ? BT_DROP_DETACH
                   : strcmp(v, "radio") == 0  ? BT_DROP_RADIO : BT_DROP_RESET;
        nvs_set_val("bt_ho_m", &mt, 1);
    }
    if (httpd_query_key_value(q, "handover", v, sizeof(v)) == ESP_OK ||
        httpd_query_key_value(q, "method", v, sizeof(v)) == ESP_OK) {
        uint8_t ho = nvs_get_byte("bt_ho_s", 0xFF);
        bt_wake_set_handover(ho == 0xFF ? -1 : ho, (bt_drop_t)nvs_get_byte("bt_ho_m", BT_DROP_RESET));
    }
    if (httpd_query_key_value(q, "cutoff", v, sizeof(v)) == ESP_OK && s_bt_running) {
        bt_wake_set_cutoff((uint8_t)atoi(v));
    }
    if (httpd_query_key_value(q, "reject_reason", v, sizeof(v)) == ESP_OK && s_bt_running) {
        bt_wake_set_reject_reason((uint8_t)strtoul(v, NULL, 16));
    }
    if (httpd_query_key_value(q, "role", v, sizeof(v)) == ESP_OK && s_bt_running) {
        bt_wake_set_accept_central(strcmp(v, "peripheral") != 0);
    }
    if (httpd_query_key_value(q, "drop", v, sizeof(v)) == ESP_OK && s_bt_running) {
        char rs[8];
        esp_err_t e;
        if (strcmp(v, "detach") == 0 && httpd_query_key_value(q, "reason", rs, sizeof(rs)) == ESP_OK) {
            e = bt_wake_detach((uint8_t)strtoul(rs, NULL, 16));
        } else {
            bt_drop_t mt = strcmp(v, "detach") == 0 ? BT_DROP_DETACH
                         : strcmp(v, "radio") == 0  ? BT_DROP_RADIO
                         : strcmp(v, "l2cap") == 0  ? BT_DROP_L2CAP : BT_DROP_RESET;
            e = bt_wake_drop(mt);
        }
        return httpd_resp_sendstr(req, e == ESP_OK ? "dropped\n" : "drop failed\n");
    }
    if (httpd_query_key_value(q, "radio", v, sizeof(v)) == ESP_OK && s_bt_running) {
        esp_err_t e = bt_wake_radio_on();
        return httpd_resp_sendstr(req, e == ESP_OK ? "radio on\n" : "radio on failed\n");
    }
    if (httpd_query_key_value(q, "mode", v, sizeof(v)) == ESP_OK) {
        uint8_t md = strcmp(v, "silent") == 0 ? BT_WAKE_SILENT
                   : strcmp(v, "accept") == 0 ? BT_WAKE_ACCEPT
                   : strcmp(v, "probe") == 0  ? BT_WAKE_PROBE : BT_WAKE_REJECT;
        nvs_set_val("bt_mode", &md, 1);
        bt_wake_set_mode((bt_wake_mode_t)md);
    }
    if (httpd_query_key_value(q, "scan", v, sizeof(v)) == ESP_OK) {
        uint8_t sp = strcmp(v, "on") == 0 ? BT_SCAN_ON : strcmp(v, "off") == 0 ? BT_SCAN_OFF
                                                                                 : BT_SCAN_AUTO;
        nvs_set_val("bt_scan", &sp, 1);
        bt_wake_set_scan_policy((bt_scan_policy_t)sp);
    }
    if (restart && bringup_get_mac("bt_host", m) && bringup_get_mac("bt_pad", m)) {
        httpd_resp_sendstr(req, "saved, restarting to apply the bluetooth address\n");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    }
    return httpd_resp_sendstr(req, "saved\n");
}

// POST /wol?mac=aa:bb:cc:dd:ee:ff  stores the target MAC in NVS (does not send).
// POST /wol                        sends a magic packet to the stored target.
// Same HTTP Basic credentials as /ota.
static esp_err_t wol_post(httpd_req_t *req)
{
    if (!req_auth_ok(req)) {
        return ESP_OK;
    }

    char query[64], val[32];
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
        httpd_query_key_value(query, "mac", val, sizeof(val)) == ESP_OK) {
        uint8_t m[6];
        if (parse_mac(val, m) != 0 || !mac_is_set(m)) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "mac must be aa:bb:cc:dd:ee:ff");
            return ESP_OK;
        }
        if (target_save(m) != ESP_OK) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "NVS write failed");
            return ESP_OK;
        }
        char ms[18];
        mac_str(m, ms);
        ESP_LOGI(TAG, "wol target set to %s", ms);
        httpd_resp_sendstr(req, "target saved\n");
        return ESP_OK;
    }

    if (!mac_is_set(s_target)) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "no target: POST /wol?mac=... first");
        return ESP_OK;
    }
    esp_err_t e = wol_send(s_target);
    if (e != ESP_OK) {
        httpd_resp_set_status(req, "503 Service Unavailable");
        httpd_resp_sendstr(req, s_link_up ? "send failed\n" : "ethernet link down\n");
        return ESP_OK;
    }
    return httpd_resp_sendstr(req, "magic packet sent\n");
}

static void http_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.stack_size = 8192;
    cfg.max_uri_handlers = 6 + BRINGUP_HTTP_HANDLERS + 2;
    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &cfg));

    static const httpd_uri_t status = { .uri = "/", .method = HTTP_GET, .handler = status_get };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &status));

    ESP_ERROR_CHECK(bringup_register_http(server));
    char admin[64];
    if (!bringup_get_str(BRINGUP_KEY_ADMIN_PASS, admin, sizeof(admin))) {
        ESP_LOGW(TAG, "no admin password yet: POST /ota, /wol and /bt disabled until /setup");
        return;
    }
    ESP_ERROR_CHECK(ota_httpd_register(server, BRINGUP_ADMIN_USER, admin));
    memset(admin, 0, sizeof(admin));
    static const httpd_uri_t wol = { .uri = "/wol", .method = HTTP_POST, .handler = wol_post };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &wol));
    static const httpd_uri_t bt = { .uri = "/bt", .method = HTTP_POST, .handler = bt_post };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &bt));
    ESP_LOGI(TAG, "POST /ota, /wol and /bt enabled; settings at /setup");
}

// A freshly updated image is PENDING_VERIFY. It proves itself by getting an IP address
// (with the HTTP server already up, so the next OTA can reach it). Otherwise roll back.
static void selftest_task(void *arg)
{
    bool rollback_possible = ota_rollback_possible();
    uint32_t elapsed = 0;
    for (;;) {
        ota_selftest_result_t r = ota_selftest_decide(true, rollback_possible, true, s_got_ip,
                                                      elapsed, CONFIG_OTA_SELFTEST_TIMEOUT_MS);
        if (r == OTA_SELFTEST_PASS) {
            const char *kept = ota_selftest_kept_detail(rollback_possible, true, s_got_ip);
            ESP_LOGI(TAG, "self-test passed, image marked valid%s%s", kept ? ": " : "",
                     kept ? kept : "");
            ota_mark_valid();
            break;
        }
        if (r == OTA_SELFTEST_FAIL) {
            ESP_LOGE(TAG, "self-test failed (no IP in %d ms), rolling back",
                     CONFIG_OTA_SELFTEST_TIMEOUT_MS);
            ota_rollback();
            esp_restart(); // only reached if the rollback could not proceed
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
        elapsed += 1000;
    }
    vTaskDelete(NULL);
}

void app_main(void)
{
    ESP_LOGI(TAG, "version %s, running from %s%s", ota_running_version(), ota_running_label(),
             ota_previous_rolled_back() ? " (the other slot was rolled back)" : "");
    esp_err_t nvs = nvs_flash_init();
    if (nvs == ESP_ERR_NVS_NO_FREE_PAGES || nvs == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs);
    ESP_ERROR_CHECK(esp_netif_init());
    // Copies esp_log output into a RAM ring from here on; a host connects with
    // components/netserial/host/netserial.py <board-ip>.
    ESP_ERROR_CHECK(netserial_start(NULL));
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_got_ip, NULL));
    ESP_ERROR_CHECK(bringup_init(&s_setup)); // decides bring-up mode, starts Wi-Fi
    target_load();
    ESP_ERROR_CHECK(eth_start());
    http_start();
    bt_start_from_nvs();
    xTaskCreate(host_watch_task, "host_watch", 3072, NULL, 4, NULL);
    if (ota_pending_verify()) {
        xTaskCreate(selftest_task, "ota_selftest", 4096, NULL, 5, NULL);
    }
    ESP_LOGI(TAG, "ethernet started, waiting for DHCP");
}
