// WT32-ETH01: bring up LAN8720 Ethernet and send Wake-on-LAN magic packets on request.
//
// Board wiring (fixed on the WT32-ETH01):
//   LAN8720 PHY address 1, MDC GPIO23, MDIO GPIO18,
//   50 MHz RMII clock from an external oscillator into GPIO0,
//   oscillator enable on GPIO16 (must go high before the EMAC starts).

#include <stdio.h>
#include <string.h>
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
#include "ota_httpd.h"
#include "netserial.h"
#include "nvs_flash.h"

// Wi-Fi station is built in only when the git-ignored credentials header exists.
#if __has_include("wifi_creds_local.h")
#include "wifi_creds_local.h"
#include "esp_wifi.h"
#define HAVE_WIFI 1
#else
#define HAVE_WIFI 0
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
    nvs_handle_t h;
    size_t len = sizeof(s_target);
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        esp_err_t e = nvs_get_blob(h, NVS_KEY, s_target, &len);
        nvs_close(h);
        if (e == ESP_OK && len == 6) {
            return;
        }
    }
    if (parse_mac(CONFIG_WOL_TARGET_MAC, s_target) != 0) {
        memset(s_target, 0, sizeof(s_target));
    }
}

static esp_err_t target_save(const uint8_t m[6])
{
    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NVS_NS, NVS_READWRITE, &h), TAG, "nvs_open");
    esp_err_t e = nvs_set_blob(h, NVS_KEY, m, 6);
    if (e == ESP_OK) {
        e = nvs_commit(h);
    }
    nvs_close(h);
    if (e == ESP_OK) {
        memcpy(s_target, m, 6);
    }
    return e;
}

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
    case ETHERNET_EVENT_CONNECTED:    s_link_up = true;  ESP_LOGI(TAG, "link up"); break;
    case ETHERNET_EVENT_DISCONNECTED: s_link_up = false; ESP_LOGI(TAG, "link down"); break;
    default: break;
    }
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
    ESP_RETURN_ON_ERROR(esp_netif_attach(netif, esp_eth_new_netif_glue(eth)), TAG, "netif attach");

    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, on_eth_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, on_got_ip, NULL));
    return esp_eth_start(eth);
}

#if HAVE_WIFI
static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (id == WIFI_EVENT_STA_CONNECTED) {
        ESP_LOGI(TAG, "wifi associated");
    } else if (id == WIFI_EVENT_STA_DISCONNECTED) {
        const wifi_event_sta_disconnected_t *d = data;
        ESP_LOGW(TAG, "wifi disconnected (reason %d), retrying", d->reason);
        vTaskDelay(pdMS_TO_TICKS(2000));
        esp_wifi_connect();
    }
}

static void wifi_start(void)
{
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t init = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_got_ip, NULL));
    wifi_config_t cfg = { 0 };
    strncpy((char *)cfg.sta.ssid, WIFI_SSID, sizeof(cfg.sta.ssid));
    strncpy((char *)cfg.sta.password, WIFI_PASSWORD, sizeof(cfg.sta.password));
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &cfg));
    ESP_ERROR_CHECK(esp_wifi_start());
    ESP_LOGI(TAG, "wifi station started, ssid %s", WIFI_SSID);
}
#endif

static esp_err_t status_get(httpd_req_t *req)
{
    char body[256], m[18];
    mac_str(s_target, m);
    snprintf(body, sizeof(body),
             "{\"app\":\"esp32-wol-via-bt\",\"version\":\"%s\",\"partition\":\"%s\","
             "\"ip\":\"%s\",\"eth_link\":%s,\"wol_target\":\"%s\",\"uptime_s\":%lld}\n",
             ota_running_version(), ota_running_label(), s_ip, s_link_up ? "true" : "false",
             mac_is_set(s_target) ? m : "unset", (long long)(esp_timer_get_time() / 1000000));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
}

// POST /wol?mac=aa:bb:cc:dd:ee:ff  stores the target MAC in NVS (does not send).
// POST /wol                        sends a magic packet to the stored target.
// Same HTTP Basic credentials as /ota.
static esp_err_t wol_post(httpd_req_t *req)
{
    char hdr[256];
    size_t hl = httpd_req_get_hdr_value_len(req, "Authorization");
    if (hl == 0 || hl >= sizeof(hdr) ||
        httpd_req_get_hdr_value_str(req, "Authorization", hdr, sizeof(hdr)) != ESP_OK ||
        !ota_basic_auth_ok(hdr, CONFIG_OTA_USER, CONFIG_OTA_PASSWORD)) {
        httpd_resp_set_status(req, "401 Unauthorized");
        httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"wol\"");
        return httpd_resp_sendstr(req, "Unauthorized\n");
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
    cfg.stack_size = 6144;
    httpd_handle_t server = NULL;
    ESP_ERROR_CHECK(httpd_start(&server, &cfg));

    static const httpd_uri_t status = { .uri = "/", .method = HTTP_GET, .handler = status_get };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &status));

    if (CONFIG_OTA_PASSWORD[0] == '\0') {
        ESP_LOGW(TAG, "CONFIG_OTA_PASSWORD is empty: POST /ota disabled, serial updates only");
        return;
    }
    ESP_ERROR_CHECK(ota_httpd_register(server, CONFIG_OTA_USER, CONFIG_OTA_PASSWORD));
    static const httpd_uri_t wol = { .uri = "/wol", .method = HTTP_POST, .handler = wol_post };
    ESP_ERROR_CHECK(httpd_register_uri_handler(server, &wol));
    ESP_LOGI(TAG, "POST /ota and POST /wol enabled");
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
    target_load();
    ESP_ERROR_CHECK(esp_netif_init());
    // Copies esp_log output into a RAM ring from here on; a host connects with
    // components/netserial/host/netserial.py <board-ip>.
    ESP_ERROR_CHECK(netserial_start(NULL));
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(eth_start());
#if HAVE_WIFI
    wifi_start();
#endif
    http_start();
    if (ota_pending_verify()) {
        xTaskCreate(selftest_task, "ota_selftest", 4096, NULL, 5, NULL);
    }
    ESP_LOGI(TAG, "ethernet started, waiting for DHCP");
}
