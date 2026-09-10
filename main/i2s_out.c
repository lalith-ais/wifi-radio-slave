#include "freertos/FreeRTOS.h"
#include "driver/i2s_std.h"
#include "esp_log.h"
#include "i2s_out.h"
#include "config.h"

static const char *TAG = "i2s_out";
static i2s_chan_handle_t s_tx_chan = NULL;
static int s_cur_rate = 0;
static int s_cur_channels = 0;
static int s_cur_bits = 0;

/* 8 descriptors x 1023 frames (the driver's per-descriptor cap) gives
 * ~185ms of buffering at 44.1kHz stereo 16-bit - deep enough to absorb
 * network jitter. i2s_out_flush() needs these same numbers to know how
 * much silence to write to fully overwrite the buffer's contents. */
#define I2S_DMA_DESC_NUM  8
#define I2S_DMA_FRAME_NUM 1023

esp_err_t i2s_out_init(void)
{
    /* The default DMA config only buffers roughly 30ms of audio - not
     * enough margin against network jitter or a slow decode iteration. */
    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO, I2S_ROLE_MASTER);
    chan_cfg.dma_desc_num = I2S_DMA_DESC_NUM;
    chan_cfg.dma_frame_num = I2S_DMA_FRAME_NUM; /* driver clamps 1024 to this anyway - set it exactly to avoid the warning */
    esp_err_t err = i2s_new_channel(&chan_cfg, &s_tx_chan, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_new_channel failed: %s", esp_err_to_name(err));
        return err;
    }

    /* Boots at 44.1kHz/16-bit/stereo; i2s_out_configure() adjusts this
     * per-stream once the decoder reports its actual output format. */
    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(44100),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_STEREO),
        .gpio_cfg = {
            .mclk = I2S_GPIO_UNUSED, /* ES9023 has no control bus and doesn't need MCLK wired */
            .bclk = I2S_OUT_BCLK_GPIO,
            .ws   = I2S_OUT_WCLK_GPIO,
            .dout = I2S_OUT_DATA_GPIO,
            .din  = I2S_GPIO_UNUSED,
            .invert_flags = { .mclk_inv = false, .bclk_inv = false, .ws_inv = false },
        },
    };

    err = i2s_channel_init_std_mode(s_tx_chan, &std_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_init_std_mode failed: %s", esp_err_to_name(err));
        return err;
    }

    err = i2s_channel_enable(s_tx_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_enable failed: %s", esp_err_to_name(err));
        return err;
    }

    s_cur_rate = 44100;
    s_cur_channels = 2;
    s_cur_bits = 16;
    ESP_LOGI(TAG, "I2S output ready: BCLK=%d WCLK=%d DATA=%d",
              I2S_OUT_BCLK_GPIO, I2S_OUT_WCLK_GPIO, I2S_OUT_DATA_GPIO);
    return ESP_OK;
}

esp_err_t i2s_out_configure(int sample_rate, int channels, int bits_per_sample)
{
    if (s_tx_chan == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (sample_rate == s_cur_rate && channels == s_cur_channels && bits_per_sample == s_cur_bits) {
        return ESP_OK;
    }

    i2s_data_bit_width_t bit_width = (bits_per_sample == 24) ? I2S_DATA_BIT_WIDTH_24BIT : I2S_DATA_BIT_WIDTH_16BIT;
    i2s_slot_mode_t slot_mode = (channels == 1) ? I2S_SLOT_MODE_MONO : I2S_SLOT_MODE_STEREO;

    i2s_std_clk_config_t clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(sample_rate);
    i2s_std_slot_config_t slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(bit_width, slot_mode);

    esp_err_t err = i2s_channel_disable(s_tx_chan);
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) { /* already-disabled is fine */
        ESP_LOGE(TAG, "i2s_channel_disable failed: %s", esp_err_to_name(err));
        return err;
    }

    err = i2s_channel_reconfig_std_clock(s_tx_chan, &clk_cfg);
    if (err == ESP_OK) {
        err = i2s_channel_reconfig_std_slot(s_tx_chan, &slot_cfg);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S reconfigure failed: %s", esp_err_to_name(err));
        i2s_channel_enable(s_tx_chan); /* best-effort: leave it running rather than silent */
        return err;
    }

    err = i2s_channel_enable(s_tx_chan);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "i2s_channel_enable after reconfig failed: %s", esp_err_to_name(err));
        return err;
    }

    s_cur_rate = sample_rate;
    s_cur_channels = channels;
    s_cur_bits = bits_per_sample;
    ESP_LOGI(TAG, "I2S reconfigured: %d Hz, %d ch, %d bit", sample_rate, channels, bits_per_sample);
    return ESP_OK;
}

esp_err_t i2s_out_write(const uint8_t *pcm, size_t len, size_t *bytes_written, uint32_t timeout_ms)
{
    if (s_tx_chan == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    return i2s_channel_write(s_tx_chan, pcm, len, bytes_written, pdMS_TO_TICKS(timeout_ms));
}

esp_err_t i2s_out_flush(void)
{
    if (s_tx_chan == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    /* Pausing/resuming the channel does not clear its contents - on
     * underrun the driver just keeps looping whatever it last had
     * queued, which is what produces the "old track repeats" symptom.
     * The only real fix is to overwrite the full buffer capacity with
     * silence, so there's nothing but zeros left for the hardware to
     * loop on if it starves again before the new track's first frame
     * arrives. This blocks for roughly the buffer's full duration
     * (~185ms) - a brief silence gap, but a correct one, unlike a
     * repeated fragment of the wrong song. */
    int bytes_per_frame = s_cur_channels * (s_cur_bits / 8);
    size_t total_bytes = (size_t)I2S_DMA_DESC_NUM * I2S_DMA_FRAME_NUM * bytes_per_frame;

    static const uint8_t silence[512] = {0};
    size_t written_total = 0;
    while (written_total < total_bytes) {
        size_t chunk = total_bytes - written_total;
        if (chunk > sizeof(silence)) {
            chunk = sizeof(silence);
        }
        size_t written = 0;
        esp_err_t err = i2s_channel_write(s_tx_chan, silence, chunk, &written, pdMS_TO_TICKS(1000));
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "I2S flush write failed: %s", esp_err_to_name(err));
            return err;
        }
        written_total += written;
    }
    return ESP_OK;
}
