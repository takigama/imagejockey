#include "wifi.h"

#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "settings.h"

#define AP_MAX_CONN     4
#define STA_CONNECT_TIMEOUT_MS 10000
#define HOSTNAME        "imagejockey"

static const char *TAG = "wifi";

static EventGroupHandle_t s_event_group;
#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static char s_ap_ssid[24];
static bool s_sta_connected = false;
static esp_ip4_addr_t s_sta_ip;

static void event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_sta_connected = false;
        xEventGroupSetBits(s_event_group, WIFI_FAIL_BIT);
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        s_sta_ip = event->ip_info.ip;
        s_sta_connected = true;
        xEventGroupSetBits(s_event_group, WIFI_CONNECTED_BIT);
    }
}

void wifi_init(void)
{
    s_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_ap_ssid, sizeof(s_ap_ssid), "ImageJockey-%02X%02X", mac[4], mac[5]);

    esp_netif_t *netif_ap = esp_netif_create_default_wifi_ap();
    esp_netif_set_hostname(netif_ap, HOSTNAME);

    char ssid[33] = {0};
    char pass[65] = {0};
    bool have_creds = settings_load_wifi_creds(ssid, sizeof(ssid), pass, sizeof(pass));

    if (have_creds) {
        esp_netif_t *netif_sta = esp_netif_create_default_wifi_sta();
        esp_netif_set_hostname(netif_sta, HOSTNAME);
    }

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&init_cfg));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL));

    wifi_config_t ap_cfg = {0};
    strncpy((char *)ap_cfg.ap.ssid, s_ap_ssid, sizeof(ap_cfg.ap.ssid) - 1);
    ap_cfg.ap.ssid_len = strlen(s_ap_ssid);
    ap_cfg.ap.channel = 1;
    ap_cfg.ap.max_connection = AP_MAX_CONN;
    ap_cfg.ap.authmode = WIFI_AUTH_OPEN;

    ESP_ERROR_CHECK(esp_wifi_set_mode(have_creds ? WIFI_MODE_APSTA : WIFI_MODE_AP));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));

    if (have_creds) {
        wifi_config_t sta_cfg = {0};
        strncpy((char *)sta_cfg.sta.ssid, ssid, sizeof(sta_cfg.sta.ssid) - 1);
        strncpy((char *)sta_cfg.sta.password, pass, sizeof(sta_cfg.sta.password) - 1);
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    }

    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "SoftAP up: SSID=%s (open), http://192.168.4.1", s_ap_ssid);

    if (have_creds) {
        EventBits_t bits = xEventGroupWaitBits(s_event_group, WIFI_CONNECTED_BIT | WIFI_FAIL_BIT, pdFALSE, pdFALSE,
                                                pdMS_TO_TICKS(STA_CONNECT_TIMEOUT_MS));
        if (bits & WIFI_CONNECTED_BIT) {
            ESP_LOGI(TAG, "connected to %s, IP: " IPSTR, ssid, IP2STR(&s_sta_ip));
        } else {
            ESP_LOGW(TAG, "failed to connect to %s within %dms -- SoftAP still up", ssid, STA_CONNECT_TIMEOUT_MS);
        }
    }
}

bool wifi_is_connected(void)
{
    return s_sta_connected;
}

void wifi_get_status_string(char *out, size_t outlen)
{
    if (s_sta_connected) {
        snprintf(out, outlen, IPSTR, IP2STR(&s_sta_ip));
    } else {
        snprintf(out, outlen, "AP only: %s", s_ap_ssid);
    }
}

void wifi_apply_new_credentials(const char *ssid, const char *pass)
{
    settings_save_wifi_creds(ssid, pass);
    ESP_LOGI(TAG, "new WiFi credentials saved, rebooting to apply");
    esp_restart();
}
