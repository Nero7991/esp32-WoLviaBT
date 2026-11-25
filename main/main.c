// WT32-ETH01: bring up LAN8720 Ethernet and send a Wake-on-LAN magic packet.
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
#include "lwip/sockets.h"
#include "ota.h"
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
static char s_ip[16] = "none";

__attribute__((unused)) static int parse_mac(const char *s, uint8_t out[6])
{
    unsigned v[6];
    if (sscanf(s, "%x:%x:%x:%x:%x:%x", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) != 6) {
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

__attribute__((unused)) static esp_err_t wol_send(const uint8_t mac[6])
{
    uint8_t pkt[102];
    memset(pkt, 0xff, 6);
    for (int i = 0; i < 16; i++) {
        memcpy(&pkt[6 + i * 6], mac, 6);
    }

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket: errno %d", errno);
        return ESP_FAIL;
    }
    int on = 1;
    setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &on, sizeof(on));

    struct sockaddr_in dst = {
        .sin_family = AF_INET,
        .sin_port = htons(CONFIG_WOL_UDP_PORT),
        .sin_addr.s_addr = htonl(INADDR_BROADCAST),
    };
    int n = sendto(sock, pkt, sizeof(pkt), 0, (struct sockaddr *)&dst, sizeof(dst));
    close(sock);
    if (n != sizeof(pkt)) {
        ESP_LOGE(TAG, "sendto: %d, errno %d", n, errno);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "magic packet sent to %02x:%02x:%02x:%02x:%02x:%02x",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    return ESP_OK;
}

static void on_eth_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    switch (id) {
    case ETHERNET_EVENT_CONNECTED:    ESP_LOGI(TAG, "link up"); break;
    case ETHERNET_EVENT_DISCONNECTED: ESP_LOGI(TAG, "link down"); break;
    default: break;
    }
}

static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    const ip_event_got_ip_t *ev = data;
    ESP_LOGI(TAG, "ip " IPSTR " gw " IPSTR, IP2STR(&ev->ip_info.ip), IP2STR(&ev->ip_info.gw));
    snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&ev->ip_info.ip));
    s_got_ip = true;

#if CONFIG_WOL_SEND_ON_BOOT
    static bool sent;
    uint8_t mac[6];
    if (!sent) {
        sent = true;
        if (parse_mac(CONFIG_WOL_TARGET_MAC, mac) == 0) {
            wol_send(mac);
        } else {
            ESP_LOGE(TAG, "bad CONFIG_WOL_TARGET_MAC: %s", CONFIG_WOL_TARGET_MAC);
        }
    }
#endif
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
    char body[192];
    snprintf(body, sizeof(body),
             "{\"app\":\"esp32-wol-via-bt\",\"version\":\"%s\",\"partition\":\"%s\","
             "\"ip\":\"%s\",\"uptime_s\":%lld}\n",
             ota_running_version(), ota_running_label(), s_ip,
             (long long)(esp_timer_get_time() / 1000000));
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, body);
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
    ESP_LOGI(TAG, "POST /ota enabled");
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
