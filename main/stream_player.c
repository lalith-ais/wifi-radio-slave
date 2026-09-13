#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/semphr.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "esp_audio_dec_default.h"
#include "esp_audio_simple_dec_default.h"
#include "esp_audio_simple_dec.h"
#include "esp_aac_dec.h"

#include "stream_player.h"
#include "cli.h"
#include "i2s_out.h"
#include "icy_meta.h"

static const char *TAG = "stream_player";

typedef struct {
    int index;
    char url[PLAYLIST_URL_MAX];
    char name[PLAYLIST_NAME_MAX];
} play_request_t;

static QueueHandle_t s_play_queue;
static SemaphoreHandle_t s_stats_mutex;
static player_stats_t s_stats = { .state = PLAYER_STATE_IDLE, .current_index = -1 };
static volatile bool s_stop_requested = false;

/* --- ADTS header parse (unchanged from the bench harness) --- */
static const int ADTS_SAMPLE_RATES[13] = {
    96000, 88200, 64000, 48000, 44100, 32000, 24000, 22050,
    16000, 12000, 11025, 8000, 7350,
};

typedef struct {
    bool valid;
    int core_sample_rate;
    int channel_config;
} adts_header_info_t;

static adts_header_info_t parse_first_adts_header(const uint8_t *data, size_t len)
{
    adts_header_info_t info = {0};
    if (len < 7 || data[0] != 0xFF || (data[1] & 0xF0) != 0xF0) {
        return info;
    }
    uint8_t samp_idx = (data[2] >> 2) & 0x0F;
    uint8_t chan_cfg = (uint8_t)(((data[2] & 0x01) << 2) | ((data[3] >> 6) & 0x03));
    if (samp_idx >= 13) {
        return info;
    }
    info.valid = true;
    info.core_sample_rate = ADTS_SAMPLE_RATES[samp_idx];
    info.channel_config = chan_cfg;
    return info;
}

static const char *classify_profile(const adts_header_info_t *adts,
                                     const esp_audio_simple_dec_info_t *decoded)
{
    if (!adts->valid) {
        return "unknown (no ADTS sync)";
    }
    bool rate_doubled = (decoded->sample_rate == adts->core_sample_rate * 2);
    bool mono_to_stereo = (adts->channel_config == 1 && decoded->channel == 2);

    if (rate_doubled && mono_to_stereo) {
        return "HE-AACv2 (SBR+PS)";
    }
    if (rate_doubled) {
        return "HE-AAC / HE-AACv1 (SBR)";
    }
    if (decoded->sample_rate == adts->core_sample_rate) {
        return "AAC-LC (or explicit-signalled HE-AAC)";
    }
    return "ambiguous";
}

/* Case-insensitive substring check - avoids relying on strcasestr's
 * availability, which varies across newlib configs. */
static bool str_contains_nocase(const char *haystack, const char *needle)
{
    if (haystack == NULL || needle == NULL) {
        return false;
    }
    size_t needle_len = strlen(needle);
    for (const char *p = haystack; *p; p++) {
        if (strncasecmp(p, needle, needle_len) == 0) {
            return true;
        }
    }
    return false;
}

static void log_heap_snapshot(const char *when)
{
    ESP_LOGI(TAG, "heap[%s]: internal free=%u min-ever=%u | psram free=%u min-ever=%u",
              when,
              (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
              (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
              (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
              (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM));
}

/* --- Format sniffing: AAC (ADTS) vs MP3, at connect time --- */
typedef enum {
    STREAM_FMT_UNKNOWN,
    STREAM_FMT_AAC,
    STREAM_FMT_MP3,
} stream_format_t;

static stream_format_t sniff_stream_format(const uint8_t *data, size_t len)
{
    if (len >= 3 && memcmp(data, "ID3", 3) == 0) {
        return STREAM_FMT_MP3;
    }
    /* A live stream can cut in mid-frame, so scan forward rather than
     * assuming byte[0] is the sync word. layer==0 disambiguates ADTS-AAC
     * from MPEG audio (MP3), which never uses layer field value 0. */
    for (size_t i = 0; i + 1 < len; i++) {
        if (data[i] != 0xFF || (data[i + 1] & 0xE0) != 0xE0) {
            continue;
        }
        uint8_t layer_bits = (data[i + 1] >> 1) & 0x03;
        return (layer_bits == 0) ? STREAM_FMT_AAC : STREAM_FMT_MP3;
    }
    return STREAM_FMT_UNKNOWN;
}

/*
 * Finds the byte offset of the next valid frame sync word for `fmt` in
 * `data`, or -1 if none is found in this buffer. Used to resynchronize
 * after a decode error: the decoder tells us it failed but not how many
 * bytes of the input it actually consumed first, so simply resuming at
 * the next network read's arbitrary starting byte is not guaranteed to
 * land on a frame boundary - feeding it mid-frame produces garbage
 * header fields (e.g. a bogus channel count) and reliably fails again,
 * cascading into repeated near-instant failures. Scanning for a real
 * sync word before resuming breaks that cascade.
 */
static int find_sync_word(const uint8_t *data, size_t len, stream_format_t fmt)
{
    for (size_t i = 0; i + 1 < len; i++) {
        if (data[i] != 0xFF) {
            continue;
        }
        uint8_t b1 = data[i + 1];
        if (fmt == STREAM_FMT_AAC && (b1 & 0xF0) == 0xF0) {
            return (int)i;
        }
        if (fmt == STREAM_FMT_MP3 && (b1 & 0xE0) == 0xE0) {
            return (int)i;
        }
    }
    return -1;
}

static void set_stats_locked(player_state_t state, const player_stats_t *merge)
{
    xSemaphoreTake(s_stats_mutex, portMAX_DELAY);
    if (merge != NULL) {
        /* Copy only the decode-related fields the caller actually sets -
         * NOT a whole-struct assignment, which would wipe the ICY
         * station_name/genre/bitrate/now_playing fields back to zero on
         * every periodic status snapshot (they're populated separately,
         * by set_station_info_locked() / on_icy_title()). */
        s_stats.current_index = merge->current_index;
        strncpy(s_stats.current_name, merge->current_name, sizeof(s_stats.current_name) - 1);
        s_stats.sample_rate = merge->sample_rate;
        s_stats.channels = merge->channels;
        s_stats.bits_per_sample = merge->bits_per_sample;
        s_stats.cpu_pct = merge->cpu_pct;
        s_stats.buffer_pct = merge->buffer_pct;
    }
    s_stats.state = state;
    xSemaphoreGive(s_stats_mutex);
}

/* Station-level ICY headers - static for the life of one connection.
 * Also clears now_playing, since a fresh connection means no in-stream
 * title has arrived yet for it. */
static void set_station_info_locked(const char *name, const char *genre, int bitrate_kbps)
{
    xSemaphoreTake(s_stats_mutex, portMAX_DELAY);
    strncpy(s_stats.station_name, name ? name : "", sizeof(s_stats.station_name) - 1);
    strncpy(s_stats.station_genre, genre ? genre : "", sizeof(s_stats.station_genre) - 1);
    s_stats.station_bitrate_kbps = bitrate_kbps;
    s_stats.now_playing[0] = '\0';
    xSemaphoreGive(s_stats_mutex);
}

/* icy_meta_cb_t callback - fires whenever a StreamTitle metadata block
 * is parsed out of the stream. Updates the shared now_playing field and
 * emits it as an async event so a listening CLI client sees song
 * changes in real time, not just on the next STATUS/STREAMINFO poll. */
static void on_icy_title(const char *stream_title, void *user_ctx)
{
    (void)user_ctx;
    xSemaphoreTake(s_stats_mutex, portMAX_DELAY);
    /* Some stations repeat the identical StreamTitle in every metadata
     * block (especially ones with a short metaint, e.g. every ~0.5s of
     * audio) rather than only sending it on an actual song change. Only
     * fire the event - and only log it - when it's genuinely new. */
    bool changed = strncmp(s_stats.now_playing, stream_title, sizeof(s_stats.now_playing) - 1) != 0;
    if (changed) {
        strncpy(s_stats.now_playing, stream_title, sizeof(s_stats.now_playing) - 1);
        s_stats.now_playing[sizeof(s_stats.now_playing) - 1] = '\0';
    }
    xSemaphoreGive(s_stats_mutex);

    if (changed) {
        cli_event(620, "now playing %s", stream_title);
    }
}

/* Feeds one buffer's worth of compressed bytes through the decoder;
 * shared between the initial sniff-buffer and the main read loop. */
static esp_audio_err_t feed_decode_buffer(esp_audio_simple_dec_handle_t decoder,
                                           uint8_t *buf, size_t len, uint8_t *out_buf,
                                           uint64_t *total_decode_us, size_t *total_decoded_bytes,
                                           esp_audio_simple_dec_info_t *info, bool *info_captured,
                                           stream_format_t fmt, adts_header_info_t *adts)
{
    esp_audio_simple_dec_raw_t raw = { .buffer = buf, .len = len, .eos = false };
    esp_audio_err_t ret = ESP_AUDIO_ERR_OK;

    if (fmt == STREAM_FMT_AAC && !adts->valid && len >= 7) {
        *adts = parse_first_adts_header(buf, len);
        if (adts->valid) {
            ESP_LOGI(TAG, "ADTS header: %d Hz core, channel_config=%d",
                      adts->core_sample_rate, adts->channel_config);
        }
    }

    while (raw.len > 0) {
        esp_audio_simple_dec_out_t out_frame = { .buffer = out_buf, .len = DECODE_OUT_BUF_SIZE };

        uint64_t t0 = esp_timer_get_time();
        ret = esp_audio_simple_dec_process(decoder, &raw, &out_frame);
        *total_decode_us += esp_timer_get_time() - t0;

        if (ret == ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
            ESP_LOGE(TAG, "PCM buffer too small (needed %d)", out_frame.needed_size);
            return ret;
        }
        if (ret != ESP_AUDIO_ERR_OK) {
            ESP_LOGE(TAG, "Decode error %d", ret);
            return ret;
        }

        if (out_frame.decoded_size > 0) {
            *total_decoded_bytes += out_frame.decoded_size;

            if (!*info_captured) {
                esp_audio_simple_dec_get_info(decoder, info);
                const char *profile_str = (fmt == STREAM_FMT_AAC) ? classify_profile(adts, info) : "MP3";
                ESP_LOGI(TAG, "Decoded stream info: %d Hz, %d ch, %d bps",
                          info->sample_rate, info->channel, info->bits_per_sample);
                ESP_LOGI(TAG, "Classified as: %s", profile_str);
                cli_event(616, "format %s %dhz %dch %dbit",
                          profile_str, info->sample_rate, info->channel, info->bits_per_sample);
                *info_captured = true;
            }

            /* Track the decoded rate, not the stream's nominal rate -
             * HE-AAC's SBR means these can differ (see classify_profile
             * above and the codec characterization notes). */
            i2s_out_configure(info->sample_rate, info->channel, info->bits_per_sample);
            size_t i2s_written = 0;
            esp_err_t i2s_err = i2s_out_write(out_frame.buffer, out_frame.decoded_size,
                                               &i2s_written, 1000);
            if (i2s_err != ESP_OK) {
                ESP_LOGW(TAG, "I2S write failed/timed out: %s", esp_err_to_name(i2s_err));
            }
        }

        raw.len -= raw.consumed;
        raw.buffer += raw.consumed;
    }
    return ESP_AUDIO_ERR_OK;
}

/*
 * esp_http_client_get_header() proved unreliable in our open/
 * fetch_headers/read usage pattern: response headers were confirmed
 * present via HTTP_EVENT_ON_HEADER (icy-metaint, icy-name, etc. all
 * arrive correctly) yet get_header() returned nothing for any of them.
 * Rather than chase why, we capture the handful of headers we actually
 * need directly in the event callback, which is proven to see every
 * header correctly as it streams in.
 */
typedef struct {
    char content_type[64];
    int icy_metaint;
    char icy_name[64];
    char icy_genre[32];
    int icy_br;
} http_headers_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    if (evt->event_id != HTTP_EVENT_ON_HEADER || evt->user_data == NULL
        || evt->header_key == NULL || evt->header_value == NULL) {
        return ESP_OK;
    }
    http_headers_t *h = (http_headers_t *)evt->user_data;
    if (strcasecmp(evt->header_key, "Content-Type") == 0) {
        strncpy(h->content_type, evt->header_value, sizeof(h->content_type) - 1);
    } else if (strcasecmp(evt->header_key, "icy-metaint") == 0) {
        h->icy_metaint = atoi(evt->header_value);
    } else if (strcasecmp(evt->header_key, "icy-name") == 0) {
        strncpy(h->icy_name, evt->header_value, sizeof(h->icy_name) - 1);
    } else if (strcasecmp(evt->header_key, "icy-genre") == 0) {
        strncpy(h->icy_genre, evt->header_value, sizeof(h->icy_genre) - 1);
    } else if (strcasecmp(evt->header_key, "icy-br") == 0) {
        h->icy_br = atoi(evt->header_value);
    }
    return ESP_OK;
}

/* --- Stream decode for one playlist entry --- */
static void play_one(const play_request_t *req)
{
    cli_event(610, "track start %d %s", req->index, req->name);
    /* Drop whatever's still queued from the previous track before this
     * one produces its first frame - see i2s_out_flush()'s comment for
     * why this matters. */
    i2s_out_flush();
    set_stats_locked(PLAYER_STATE_BUFFERING, NULL);

    ESP_LOGI(TAG, "Opening stream: %s", req->url);

    http_headers_t hdrs = {0};

    esp_http_client_config_t http_cfg = {
        .url = req->url,
        .timeout_ms = HTTP_RECV_TIMEOUT_MS,
        .buffer_size = HTTP_READ_BUF_SIZE,
        .buffer_size_tx = 1024,
        /* Needed so https:// redirect targets (very common - directories
         * often point at an http:// vanity URL that 30x's to an https://
         * CDN edge) can complete a TLS handshake. */
        .crt_bundle_attach = esp_crt_bundle_attach,
        /* Redirects and playlist responses are handled manually below via
         * open()/fetch_headers()/read(), so auto-redirect is disabled to
         * avoid any ambiguity with that manual handling. */
        .disable_auto_redirect = true,
        .event_handler = http_event_handler,
        .user_data = &hdrs,
    };

    esp_http_client_handle_t client = esp_http_client_init(&http_cfg);
    if (client == NULL) {
        ESP_LOGE(TAG, "HTTP client init failed");
        cli_event(613, "track error 1");
        set_stats_locked(PLAYER_STATE_ERROR, NULL);
        return;
    }

    /* Asks the server to interleave StreamTitle metadata into the
     * audio stream (see icy_meta.h) - most Icecast/Shoutcast stations
     * honor this; ones that don't simply omit icy-metaint from the
     * response, which icy_meta_init() treats as pass-through. */
    esp_http_client_set_header(client, "Icy-Metadata", "1");

    /* Declared up front (NULL) so the early stop-check below can safely
     * jump to `stopped:`, which frees/closes all three - a goto that
     * skipped past these declarations would free garbage pointers. */
    uint8_t *http_buf = NULL;
    uint8_t *out_buf = NULL;
    esp_audio_simple_dec_handle_t decoder = NULL;

    int max_redirects = 5; /* real URLs can chain an HTTP redirect and a playlist hop */
    int redirect_count = 0, status = 0, content_len = 0;

    while (redirect_count < max_redirects) {
        if (s_stop_requested) {
            goto stopped;
        }
        memset(&hdrs, 0, sizeof(hdrs)); /* only this iteration's response should count */
        esp_err_t err = esp_http_client_open(client, 0);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "HTTP open failed: %s", esp_err_to_name(err));
            esp_http_client_cleanup(client);
            cli_event(613, "track error 3");
            set_stats_locked(PLAYER_STATE_ERROR, NULL);
            return;
        }

        content_len = esp_http_client_fetch_headers(client);
        status = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "HTTP %d, content-length=%d", status, content_len);

        if (status == 0) {
            ESP_LOGE(TAG, "HTTP fetch headers failed (no valid response)");
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            cli_event(613, "track error 3");
            set_stats_locked(PLAYER_STATE_ERROR, NULL);
            return;
        }

        if (status >= 300 && status < 400) {
            esp_err_t redir_err = esp_http_client_set_redirection(client);
            if (redir_err == ESP_OK) {
                char new_url[256];
                esp_http_client_get_url(client, new_url, sizeof(new_url));
                ESP_LOGI(TAG, "Following redirect (%d) -> %s", status, new_url);
                esp_http_client_close(client);
                redirect_count++;
                continue;
            }
            ESP_LOGE(TAG, "Redirect %d but no Location available: %s", status, esp_err_to_name(redir_err));
            esp_http_client_close(client);
            esp_http_client_cleanup(client);
            cli_event(613, "track error 3");
            set_stats_locked(PLAYER_STATE_ERROR, NULL);
            return;
        }

        /* Many "stream URLs" from radio directories are actually
         * .m3u/.pls playlist wrappers rather than an HTTP-level redirect. */
        if (status >= 200 && status < 300) {
            bool is_playlist = hdrs.content_type[0] != '\0' &&
                (str_contains_nocase(hdrs.content_type, "mpegurl") ||
                 str_contains_nocase(hdrs.content_type, "scpls"));

            if (is_playlist) {
                char playlist_buf[1024];
                int total = 0, r;
                while (total < (int)sizeof(playlist_buf) - 1 &&
                       (r = esp_http_client_read(client, playlist_buf + total,
                                                  sizeof(playlist_buf) - 1 - total)) > 0) {
                    total += r;
                }
                playlist_buf[total] = '\0';
                esp_http_client_close(client);
                ESP_LOGI(TAG, "Got a playlist (%s), looking for a stream URL inside it", hdrs.content_type);

                char *found_url = NULL;
                char *line = strtok(playlist_buf, "\r\n");
                while (line != NULL) {
                    char *http_pos = strstr(line, "http://");
                    char *https_pos = strstr(line, "https://");
                    char *url_start = http_pos ? http_pos : https_pos;
                    if (url_start != NULL) {
                        found_url = url_start;
                        break;
                    }
                    line = strtok(NULL, "\r\n");
                }

                if (found_url != NULL) {
                    ESP_LOGI(TAG, "Playlist -> following embedded URL -> %s", found_url);
                    esp_http_client_set_url(client, found_url);
                    redirect_count++;
                    continue;
                }
                ESP_LOGE(TAG, "Playlist response but no http(s):// URL found inside it");
                esp_http_client_cleanup(client);
                cli_event(613, "track error 3");
                set_stats_locked(PLAYER_STATE_ERROR, NULL);
                return;
            }
        }
        break; /* real 2xx audio stream, or a 4xx/5xx to report as an error below */
    }

    if (status < 200 || status >= 400) {
        ESP_LOGE(TAG, "HTTP error status %d", status);
        esp_http_client_close(client);
        esp_http_client_cleanup(client);
        cli_event(613, "track error 3");
        set_stats_locked(PLAYER_STATE_ERROR, NULL);
        return;
    }

    /* Station-level ICY headers (static for the life of this connection)
     * and the in-stream metadata interval, if the station supports it -
     * already captured by http_event_handler as the final response's
     * headers streamed in (see http_headers_t's comment for why we don't
     * use esp_http_client_get_header() for these). */
    int metaint = hdrs.icy_metaint;
    set_station_info_locked(hdrs.icy_name[0] ? hdrs.icy_name : NULL,
                             hdrs.icy_genre[0] ? hdrs.icy_genre : NULL, hdrs.icy_br);
    ESP_LOGI(TAG, "ICY: name='%s' genre='%s' br=%d metaint=%d",
              hdrs.icy_name, hdrs.icy_genre, hdrs.icy_br, metaint);

    icy_meta_ctx_t icy_ctx;
    icy_meta_init(&icy_ctx, metaint, on_icy_title, NULL);

    log_heap_snapshot("before open");

    http_buf = malloc(HTTP_READ_BUF_SIZE);
    if (http_buf == NULL) {
        ESP_LOGE(TAG, "Out of memory for HTTP read buffer");
        goto fail_alloc;
    }

    int sniff_len = esp_http_client_read(client, (char *)http_buf, HTTP_READ_BUF_SIZE);
    if (sniff_len <= 0) {
        ESP_LOGE(TAG, "Failed to read enough of the stream to sniff its format");
        goto fail_alloc;
    }

    /* Format sniffing runs on raw bytes before filtering - safe, since a
     * real metaint is always far larger (typically 8-16KB) than this
     * first ~2KB read, so nothing but pure audio has arrived yet. */
    stream_format_t fmt = sniff_stream_format(http_buf, (size_t)sniff_len);

    size_t sniff_audio_len = 0;
    icy_meta_process(&icy_ctx, http_buf, (size_t)sniff_len, http_buf, &sniff_audio_len);
    sniff_len = (int)sniff_audio_len;

    esp_aac_dec_cfg_t aac_cfg = { .aac_plus_enable = true };
    esp_audio_simple_dec_cfg_t dec_cfg = { .use_frame_dec = false };
    if (fmt == STREAM_FMT_MP3) {
        ESP_LOGI(TAG, "Detected format: MP3");
        dec_cfg.dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_MP3;
    } else {
        if (fmt == STREAM_FMT_UNKNOWN) {
            ESP_LOGI(TAG, "No sync pattern found in the first %d bytes - assuming AAC/ADTS", sniff_len);
        } else {
            ESP_LOGI(TAG, "Detected format: AAC (ADTS)");
        }
        dec_cfg.dec_type = ESP_AUDIO_SIMPLE_DEC_TYPE_AAC;
        dec_cfg.dec_cfg = &aac_cfg;
        dec_cfg.cfg_size = sizeof(aac_cfg);
    }

    esp_audio_err_t aret = esp_audio_simple_dec_open(&dec_cfg, &decoder);
    if (aret != ESP_AUDIO_ERR_OK) {
        ESP_LOGE(TAG, "Decoder open failed: %d", aret);
        goto fail_alloc;
    }
    log_heap_snapshot("after open");

    out_buf = malloc(DECODE_OUT_BUF_SIZE);
    if (out_buf == NULL) {
        ESP_LOGE(TAG, "Out of memory for PCM buffer");
        goto fail_alloc;
    }

    esp_audio_simple_dec_info_t info = {0};
    bool info_captured = false;
    adts_header_info_t adts = {0};
    uint64_t total_decode_us = 0;
    size_t total_decoded_bytes = 0;
    uint64_t stream_start_us = esp_timer_get_time();
    uint64_t last_report_us = stream_start_us;
    uint64_t last_yield_us = stream_start_us;
    int read_err_count = 0;
    const int MAX_READ_ERRORS = 5;
    int decode_err_count = 0;
    const int MAX_DECODE_ERRORS = 20; /* transient (network-jitter-induced) bad frames are
                                        * tolerated; this many in a row means something's
                                        * genuinely wrong, not just an occasional glitch. */
    bool resyncing = false; /* true after a decode error, until a real frame sync word is found again */

    /* The sniff read above already consumed real stream bytes - feed them
     * in first or every station silently drops its first ~2KB. A
     * failure here IS treated as fatal (unlike in the main loop below) -
     * if the very first frame won't decode, the format was likely
     * misdetected, which isn't something retrying will fix. */
    aret = feed_decode_buffer(decoder, http_buf, (size_t)sniff_len, out_buf,
                               &total_decode_us, &total_decoded_bytes, &info, &info_captured, fmt, &adts);
    if (aret != ESP_AUDIO_ERR_OK) {
        goto cleanup;
    }
    set_stats_locked(PLAYER_STATE_PLAYING, NULL);
    cli_event(612, "playing");

    while (!s_stop_requested) {
        int rlen = esp_http_client_read(client, (char *)http_buf, HTTP_READ_BUF_SIZE);
        if (rlen < 0) {
            ESP_LOGE(TAG, "HTTP read error %d", rlen);
            if (++read_err_count >= MAX_READ_ERRORS) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (rlen == 0) {
            ESP_LOGI(TAG, "HTTP EOF");
            break;
        }
        read_err_count = 0;

        size_t audio_len = 0;
        icy_meta_process(&icy_ctx, http_buf, (size_t)rlen, http_buf, &audio_len);
        if (audio_len == 0) {
            continue; /* chunk was entirely (or started with) a metadata block */
        }

        uint8_t *feed_ptr = http_buf;
        if (resyncing) {
            int sync_off = find_sync_word(feed_ptr, audio_len, fmt);
            if (sync_off < 0) {
                /* No sync word anywhere in this chunk - discard it whole
                 * and keep looking in the next one, rather than feeding
                 * the decoder bytes we already know aren't a frame start. */
                continue;
            }
            feed_ptr += sync_off;
            audio_len -= (size_t)sync_off;
            resyncing = false;
            ESP_LOGI(TAG, "Resynced after %d byte(s)", sync_off);
        }

        aret = feed_decode_buffer(decoder, feed_ptr, audio_len, out_buf,
                                   &total_decode_us, &total_decoded_bytes, &info, &info_captured, fmt, &adts);
        if (aret != ESP_AUDIO_ERR_OK) {
            /* A single bad frame - most often a network-jitter-induced
             * corrupted or misaligned chunk - shouldn't tear down the
             * whole track. Reopen the decoder (its internal bit-reservoir/
             * SBR state may now be corrupted, not just the input bytes)
             * and resync to a real frame boundary before resuming, rather
             * than feeding the next arbitrary byte offset - which is what
             * was previously cascading into repeated near-instant
             * failures every time this path was hit. */
            ESP_LOGW(TAG, "Decode error %d (tolerated %d/%d), reopening decoder and resyncing",
                      aret, decode_err_count + 1, MAX_DECODE_ERRORS);
            if (++decode_err_count >= MAX_DECODE_ERRORS) {
                ESP_LOGE(TAG, "Too many consecutive decode errors, giving up on this track");
                goto cleanup;
            }
            esp_audio_simple_dec_close(decoder);
            decoder = NULL;
            esp_audio_err_t reopen_ret = esp_audio_simple_dec_open(&dec_cfg, &decoder);
            if (reopen_ret != ESP_AUDIO_ERR_OK) {
                ESP_LOGE(TAG, "Decoder reopen failed: %d", reopen_ret);
                goto cleanup;
            }
            resyncing = true;
            continue;
        }
        decode_err_count = 0;

        uint64_t now = esp_timer_get_time();
        if (now - last_report_us >= (REPORT_INTERVAL_MS * 1000ULL)) {
            if (info_captured && total_decoded_bytes > 0) {
                int sample_size = info.channel * (info.bits_per_sample >> 3);
                double audio_sec = (double)total_decoded_bytes / (sample_size * info.sample_rate);
                double wall_sec = (double)(now - stream_start_us) / 1e6;
                double cpu_pct = ((double)total_decode_us / 1e6) / audio_sec * 100.0;
                double buf_pct = (audio_sec / wall_sec) * 100.0;

                ESP_LOGI(TAG, "Status: %.2fs audio | %.2fs wall | CPU=%.2f%% | buffer-health=%.1f%%",
                          audio_sec, wall_sec, cpu_pct, buf_pct);

                player_stats_t snap = {
                    .current_index = req->index, .sample_rate = info.sample_rate,
                    .channels = info.channel, .bits_per_sample = info.bits_per_sample,
                    .cpu_pct = cpu_pct, .buffer_pct = buf_pct,
                };
                strncpy(snap.current_name, req->name, sizeof(snap.current_name) - 1);
                set_stats_locked(PLAYER_STATE_PLAYING, &snap);
            }
            log_heap_snapshot("running");
            last_report_us = now;
        }

        if (now - last_yield_us >= (YIELD_INTERVAL_MS * 1000ULL)) {
            vTaskDelay(1);
            last_yield_us = esp_timer_get_time();
        }
    }

cleanup:
    free(http_buf);
    free(out_buf);
    if (decoder) {
        esp_audio_simple_dec_close(decoder);
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    log_heap_snapshot("after close");
    ESP_LOGI(TAG, "Stack high-water: %u words", (unsigned)uxTaskGetStackHighWaterMark(NULL));

    /* Buffers/decoder/client are already released above - just report
     * which outcome this was. Do not jump to `stopped:` below, which
     * would free/close everything a second time. */
    cli_event(614, "stopped"); /* covers both a requested stop and natural stream end */
    set_stats_locked(PLAYER_STATE_IDLE, NULL);
    return;

fail_alloc:
    free(http_buf);
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    cli_event(613, "track error 5");
    set_stats_locked(PLAYER_STATE_ERROR, NULL);
    return;

stopped:
    /* Reached only from the early stop-check before http_buf/out_buf/
     * decoder are allocated - they're still NULL here, so free()/the
     * NULL-guarded close are no-ops. This path does NOT fall through
     * from `cleanup:` above, so nothing here is freed twice. */
    free(http_buf);
    free(out_buf);
    if (decoder) {
        esp_audio_simple_dec_close(decoder);
    }
    esp_http_client_close(client);
    esp_http_client_cleanup(client);
    cli_event(614, "stopped");
    set_stats_locked(PLAYER_STATE_IDLE, NULL);
}

static void player_task(void *arg)
{
    play_request_t req;
    for (;;) {
        if (xQueueReceive(s_play_queue, &req, portMAX_DELAY) == pdTRUE) {
            s_stop_requested = false;
            play_one(&req);
        }
    }
}

void stream_player_init(void)
{
    esp_audio_dec_register_default();
    esp_audio_simple_dec_register_default();
    s_play_queue = xQueueCreate(1, sizeof(play_request_t));
    s_stats_mutex = xSemaphoreCreateMutex();

    esp_err_t err = i2s_out_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "I2S output init failed: %s - playback will decode but produce no audio",
                  esp_err_to_name(err));
    }
}

void stream_player_start_task(void)
{
    xTaskCreatePinnedToCore(player_task, "player", 24 * 1024, NULL, 10, NULL, tskNO_AFFINITY);
}

esp_err_t stream_player_play(int index, const char *url, const char *name)
{
    play_request_t req = { .index = index };
    strncpy(req.url, url, sizeof(req.url) - 1);
    strncpy(req.name, name ? name : "", sizeof(req.name) - 1);

    /* Update current_index synchronously, right now - not just via the
     * player loop's periodic (~5s) status snapshot. Callers like
     * NEXT/PREV compute the next index as current_index +/- 1
     * immediately after calling this; without this update, a second
     * NEXT/PREV pressed within that 5s window would still read the
     * *previous* track's index and compute the same "next" index all
     * over again, appearing to get stuck. */
    xSemaphoreTake(s_stats_mutex, portMAX_DELAY);
    s_stats.current_index = index;
    strncpy(s_stats.current_name, name ? name : "", sizeof(s_stats.current_name) - 1);
    s_stats.current_name[sizeof(s_stats.current_name) - 1] = '\0';
    xSemaphoreGive(s_stats_mutex);

    s_stop_requested = true; /* nudge any current playback to wind down */
    xQueueOverwrite(s_play_queue, &req);
    return ESP_OK;
}

void stream_player_stop(void)
{
    s_stop_requested = true;
}

void stream_player_get_stats(player_stats_t *out)
{
    xSemaphoreTake(s_stats_mutex, portMAX_DELAY);
    *out = s_stats;
    xSemaphoreGive(s_stats_mutex);
}
