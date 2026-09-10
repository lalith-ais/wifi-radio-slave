#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_wifi.h"
#include "esp_event.h"
#include "esp_log.h"
#include "wifi_mgr.h"
#include "config.h"

static const char *TAG = "wifi_mgr";

#define WIFI_CONNECTED_BIT BIT0
#define WIFI_FAIL_BIT      BIT1

static EventGroupHandle_t s_wifi_event_group;
static int s_retry_num = 0;
static volatile bool s_connected = false;

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        if (s_retry_num < WIFI_MAX_RETRY) {
            ESP_LOGI(TAG, "WiFi disconnected, retrying (%d/%d)...", s_retry_num + 1, WIFI_MAX_RETRY);
            esp_wifi_connect();
            s_retry_num++;
        } else {
            xEventGroupSetBits(s_wifi_event_group, WIFI_FAIL_BIT);
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *)event_data;
        ESP_LOGI(TAG, "WiFi connected, IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_retry_num = 0;
        s_connected = true;
        xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
    }
}

esp_err_t wifi_mgr_connect(void)
{
    s_wifi_event_group = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    esp_event_handler_instance_t instance_any_id;
    esp_event_handler_instance_t instance_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID,
                                                          &wifi_event_handler, NULL, &instance_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                                          &wifi_event_handler, NULL, &instance_got_ip));

    /* TODO next stage: read ssid/pass from the SPIFFS-backed config
     * store (WIFI.SET), falling back to Kconfig only if none is set. */
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = CONFIG_WIFI_SSID,
            .password = CONFIG_WIFI_PASSWORD,
            .threshold.authmode = WIFI_AUTH_WPA2_PSK,
            .sae_pwe_h2e = WPA3_SAE_PWE_BOTH,
        },
    };

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* Power-save (modem sleep) periodically stalls incoming data for
     * long enough to underrun the I2S DMA buffer, heard as a rhythmic
     * crackle roughly on the listen-interval period. Irrelevant on a
     * mains-powered radio anyway - disable it outright. */
    ESP_ERROR_CHECK(esp_wifi_set_ps(WIFI_PS_NONE));

    ESP_LOGI(TAG, "Waiting for WiFi connection...");
    EventBits_t bits = xEventGroupWaitBits(s_wifi_event_group,
                                            WIFI_CONNECTED_BIT | WIFI_FAIL_BIT,
                                            pdFALSE, pdFALSE,
                                            pdMS_TO_TICKS(WIFI_CONNECT_TIMEOUT_MS));

    if (bits & WIFI_CONNECTED_BIT) {
        ESP_LOGI(TAG, "Connected to SSID:%s", CONFIG_WIFI_SSID);
        return ESP_OK;
    } else if (bits & WIFI_FAIL_BIT) {
        ESP_LOGE(TAG, "Failed to connect to SSID:%s", CONFIG_WIFI_SSID);
        return ESP_FAIL;
    }
    ESP_LOGE(TAG, "WiFi connection timed out");
    return ESP_ERR_TIMEOUT;
}

bool wifi_mgr_is_connected(void)
{
    return s_connected;
}

esp_err_t wifi_mgr_get_status(char *ssid_out, size_t ssid_len,
                               char *ip_out, size_t ip_len,
                               int8_t *rssi_out)
{
    if (!s_connected) {
        return ESP_ERR_INVALID_STATE;
    }

    wifi_ap_record_t ap_info;
    if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        strncpy(ssid_out, (const char *)ap_info.ssid, ssid_len - 1);
        ssid_out[ssid_len - 1] = '\0';
        *rssi_out = ap_info.rssi;
    } else {
        ssid_out[0] = '\0';
        *rssi_out = 0;
    }

    esp_netif_t *netif = esp_netif_get_handle_from_ifkey("WIFI_STA_DEF");
    esp_netif_ip_info_t ip_info;
    if (netif != NULL && esp_netif_get_ip_info(netif, &ip_info) == ESP_OK) {
        snprintf(ip_out, ip_len, IPSTR, IP2STR(&ip_info.ip));
    } else {
        ip_out[0] = '\0';
    }

    return ESP_OK;
}
