#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

/*
 * Connects to WiFi in station mode and blocks until connected, failed,
 * or timed out. Call this from a dedicated bootstrap task, never from
 * the CLI task - the CLI must stay responsive regardless of WiFi state.
 *
 * SSID/password currently come from Kconfig (CONFIG_WIFI_SSID /
 * CONFIG_WIFI_PASSWORD). TODO next stage: source these from the
 * SPIFFS-backed single-SSID store instead, written via the CLI's
 * WIFI.SET command.
 */
esp_err_t wifi_mgr_connect(void);

/* True once a successful IP_EVENT_STA_GOT_IP has been seen. */
bool wifi_mgr_is_connected(void);

/*
 * Fills in SSID / IP / RSSI for the WIFI.STATUS CLI reply.
 * Returns ESP_ERR_INVALID_STATE if not currently connected.
 */
esp_err_t wifi_mgr_get_status(char *ssid_out, size_t ssid_len,
                               char *ip_out, size_t ip_len,
                               int8_t *rssi_out);
