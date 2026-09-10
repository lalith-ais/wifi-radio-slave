#include "nvs_flash.h"
#include "esp_err.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "wifi_mgr.h"
#include "stream_player.h"
#include "cli.h"

static const char *TAG = "main";

/*
 * Connects WiFi and brings up the player task. Runs as its own task so
 * the CLI (started first, in app_main) is never blocked by WiFi
 * connecting, retrying, or timing out - the "front panel never waits"
 * design decision applies here too, even though this bench build has
 * no real front panel yet.
 */
/*
 * TEMPORARY bench-test bypass: auto-plays a known-good stream at boot so
 * the decode -> I2S path can be exercised while the CLI is unresponsive.
 * Remove once PL.ADD/PLAY over UART1 are confirmed working - this skips
 * the playlist and CLI entirely.
 */
#define BENCH_TEST_AUTOPLAY_URL "http://mp3.harmonyfm.de/harmonyfm/hqlivestream.aac"

static void bootstrap_task(void *arg)
{
    esp_err_t err = wifi_mgr_connect();
    if (err == ESP_OK) {
        cli_event(601, "wifi connected");
    } else {
        cli_event(602, "wifi failed %d", err);
    }

    stream_player_start_task();

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Bench-test autoplay: %s", BENCH_TEST_AUTOPLAY_URL);
        stream_player_play(0, BENCH_TEST_AUTOPLAY_URL, "bench-test");
    }

    ESP_LOGI(TAG, "Bootstrap complete");
    vTaskDelete(NULL);
}

void app_main(void)
{
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES || ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    stream_player_init();
    cli_start_task(); /* up first - responsive even before WiFi connects */

    xTaskCreatePinnedToCore(bootstrap_task, "bootstrap", 4096, NULL, 5, NULL, tskNO_AFFINITY);
}
