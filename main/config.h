#pragma once

/*
 * Shared tuning constants for the WiFi Radio Block firmware.
 *
 * WiFi credentials and the station playlist are still sourced from
 * Kconfig (bench-test fallbacks) in this pass. TODO next stage: replace
 * both with the SPIFFS-backed single-SSID config store and SPIFFS
 * playlist described in the serial CLI protocol spec (WIFI.SET, PL.ADD,
 * etc.) so the board can be reconfigured without reflashing.
 */

#define WIFI_CONNECT_TIMEOUT_MS 30000
#define WIFI_MAX_RETRY          5

#define HTTP_READ_BUF_SIZE      2048
#define HTTP_RECV_TIMEOUT_MS    5000
#define DECODE_OUT_BUF_SIZE     16384
#define REPORT_INTERVAL_MS      5000
#define YIELD_INTERVAL_MS       10

#define PLAYLIST_MAX_ENTRIES    32
#define PLAYLIST_URL_MAX        256
#define PLAYLIST_NAME_MAX       48

/* I2S output stage: ES9023 DAC, no control bus - just BCLK/WCLK/DATA. */
#define I2S_OUT_BCLK_GPIO       39
#define I2S_OUT_WCLK_GPIO       40
#define I2S_OUT_DATA_GPIO       41

/* CLI transport: dedicated UART1 via an external USB-UART bridge, kept
 * separate from the internal-USB console (which stays as the
 * programming/log interface) so protocol traffic is never interleaved
 * with ESP_LOGx noise. */
#define CLI_UART_NUM            UART_NUM_1
#define CLI_UART_TX_GPIO        17
#define CLI_UART_RX_GPIO        18
#define CLI_UART_BAUD           115200
