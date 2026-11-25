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
#include "lwip/sockets.h"

static const char *TAG = "wol";

#define PIN_OSC_ENABLE 16
#define PHY_ADDR       1
#define PIN_MDC        23
#define PIN_MDIO       18

static int parse_mac(const char *s, uint8_t out[6])
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

static esp_err_t wol_send(const uint8_t mac[6])
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

void app_main(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    ESP_ERROR_CHECK(eth_start());
    ESP_LOGI(TAG, "ethernet started, waiting for DHCP");
}
