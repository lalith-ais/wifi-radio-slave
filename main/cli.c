#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "driver/uart.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_system.h"

#include "cli.h"
#include "config.h"
#include "wifi_mgr.h"
#include "stream_player.h"

#define FW_VERSION       "0.1.0-bench"
#define FACTORY_CONFIRM_TIMEOUT_MS 10000

typedef struct {
    bool used;
    char url[PLAYLIST_URL_MAX];
    char name[PLAYLIST_NAME_MAX];
} playlist_entry_t;

static playlist_entry_t s_playlist[PLAYLIST_MAX_ENTRIES];
static int s_playlist_count = 0;
static SemaphoreHandle_t s_out_mutex;
static int64_t s_factory_confirm_deadline_us = 0;

/* --- Output framing: SMTP-style "CCC text" / "CCC-text", and async "*CCC text" --- */

static void write_line(const char *buf, size_t len)
{
    xSemaphoreTake(s_out_mutex, portMAX_DELAY);
    uart_write_bytes(CLI_UART_NUM, buf, len);
    xSemaphoreGive(s_out_mutex);
}

static void out_line(char sep, int code, const char *fmt, va_list ap)
{
    char line[200];
    char body[176];
    vsnprintf(body, sizeof(body), fmt, ap);
    int len = snprintf(line, sizeof(line), "%03d%c%s\r\n", code, sep, body);
    if (len > 0) {
        write_line(line, (size_t)len < sizeof(line) ? (size_t)len : sizeof(line) - 1);
    }
}

/* Final (or only) line of a reply. */
static void reply(int code, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    out_line(' ', code, fmt, ap);
    va_end(ap);
}

/* Continuation line of a multi-line reply - more lines follow. */
static void reply_more(int code, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    out_line('-', code, fmt, ap);
    va_end(ap);
}

void cli_event(int code, const char *fmt, ...)
{
    char body[176];
    char line[200];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(body, sizeof(body), fmt, ap);
    va_end(ap);

    int len = snprintf(line, sizeof(line), "*%03d %s\r\n", code, body);
    if (len > 0) {
        write_line(line, (size_t)len < sizeof(line) ? (size_t)len : sizeof(line) - 1);
    }
}

/* --- Command handlers --- */

static void cmd_wifi_set(char *args)
{
    /* TODO next stage: persist ssid/pass to the SPIFFS-backed config
     * store. Until then this can't actually take effect at runtime. */
    if (args == NULL || strchr(args, ' ') == NULL) {
        reply(501, "bad syntax");
        return;
    }
    reply(501, "not implemented - no SPIFFS config store yet");
}

static void cmd_wifi_connect(char *args)
{
    (void)args;
    /* TODO next stage: WiFi connect currently only happens once, at
     * boot, from the bootstrap task. Wire this to trigger a fresh
     * connect attempt once WIFI.SET can actually change credentials. */
    reply(501, "not implemented - wifi is boot-time only in this build");
}

static void cmd_wifi_disconnect(char *args)
{
    (void)args;
    reply(501, "not implemented - wifi is boot-time only in this build");
}

static void cmd_wifi_status(char *args)
{
    (void)args;
    char ssid[33], ip[16];
    int8_t rssi;
    if (wifi_mgr_get_status(ssid, sizeof(ssid), ip, sizeof(ip), &rssi) != ESP_OK) {
        reply(211, "disconnected");
        return;
    }
    reply(211, "%s %s %d", ssid, ip, rssi);
}

static void cmd_pl_add(char *args)
{
    if (args == NULL) {
        reply(501, "usage: PL.ADD <url> [name]");
        return;
    }
    if (s_playlist_count >= PLAYLIST_MAX_ENTRIES) {
        reply(500, "playlist full");
        return;
    }
    char *url = strtok(args, " ");
    char *name = strtok(NULL, "");
    if (url == NULL) {
        reply(501, "usage: PL.ADD <url> [name]");
        return;
    }
    playlist_entry_t *e = &s_playlist[s_playlist_count];
    strncpy(e->url, url, sizeof(e->url) - 1);
    strncpy(e->name, name ? name : url, sizeof(e->name) - 1);
    e->used = true;
    reply(250, "added %d", s_playlist_count);
    s_playlist_count++;
}

static void cmd_pl_del(char *args)
{
    if (args == NULL) {
        reply(501, "usage: PL.DEL <index>");
        return;
    }
    int idx = atoi(args);
    if (idx < 0 || idx >= s_playlist_count || !s_playlist[idx].used) {
        reply(404, "bad index");
        return;
    }
    for (int i = idx; i < s_playlist_count - 1; i++) {
        s_playlist[i] = s_playlist[i + 1];
    }
    s_playlist_count--;
    reply(250, "ok");
}

static void cmd_pl_list(char *args)
{
    (void)args;
    if (s_playlist_count == 0) {
        reply(210, "end");
        return;
    }
    for (int i = 0; i < s_playlist_count; i++) {
        reply_more(210, "%d %s %s", i, s_playlist[i].name, s_playlist[i].url);
    }
    reply(210, "end");
}

static void cmd_pl_clear(char *args)
{
    (void)args;
    s_playlist_count = 0;
    reply(250, "ok");
}

static void cmd_pl_rename(char *args)
{
    if (args == NULL) {
        reply(501, "usage: PL.RENAME <index> <name>");
        return;
    }
    char *idx_str = strtok(args, " ");
    char *name = strtok(NULL, "");
    int idx = idx_str ? atoi(idx_str) : -1;
    if (idx < 0 || idx >= s_playlist_count || name == NULL) {
        reply(404, "bad index");
        return;
    }
    strncpy(s_playlist[idx].name, name, sizeof(s_playlist[idx].name) - 1);
    reply(250, "ok");
}

static void cmd_play(char *args)
{
    int idx = args ? atoi(args) : -1;
    player_stats_t st;
    stream_player_get_stats(&st);

    if (idx < 0) {
        idx = st.current_index >= 0 ? st.current_index : 0;
    }
    if (idx < 0 || idx >= s_playlist_count) {
        reply(404, "bad index");
        return;
    }
    stream_player_play(idx, s_playlist[idx].url, s_playlist[idx].name);
    reply(220, "accepted");
}

static void cmd_next_prev(char *args, int delta)
{
    (void)args;
    if (s_playlist_count == 0) {
        reply(404, "playlist empty");
        return;
    }
    player_stats_t st;
    stream_player_get_stats(&st);
    int idx = st.current_index < 0 ? 0 : st.current_index;
    idx = (idx + delta + s_playlist_count) % s_playlist_count;
    stream_player_play(idx, s_playlist[idx].url, s_playlist[idx].name);
    reply(220, "accepted");
}

static void cmd_next(char *args) { cmd_next_prev(args, 1); }
static void cmd_prev(char *args) { cmd_next_prev(args, -1); }

static void cmd_stop(char *args)
{
    (void)args;
    stream_player_stop();
    reply(250, "ok");
}

static void cmd_pause(char *args) { (void)args; reply(501, "not implemented - no I2S stage yet"); }
static void cmd_vol(char *args)   { (void)args; reply(501, "not implemented - no I2S stage yet"); }
static void cmd_vol_q(char *args) { (void)args; reply(501, "not implemented - no I2S stage yet"); }

static const char *state_name(player_state_t s)
{
    switch (s) {
        case PLAYER_STATE_IDLE:      return "idle";
        case PLAYER_STATE_BUFFERING: return "buffering";
        case PLAYER_STATE_PLAYING:   return "playing";
        default:                     return "error";
    }
}

static void cmd_status(char *args)
{
    (void)args;
    player_stats_t st;
    stream_player_get_stats(&st);
    reply(211, "%s %d %dhz/%dch aac_or_mp3", state_name(st.state), st.current_index,
          st.sample_rate, st.channels);
}

static void cmd_stats(char *args)
{
    (void)args;
    player_stats_t st;
    stream_player_get_stats(&st);
    reply_more(212, "buffer %.1f", st.buffer_pct);
    reply_more(212, "cpu %.1f", st.cpu_pct);
    reply_more(212, "underruns 0"); /* TODO: not tracked yet */
    reply(212, "uptime %lld", esp_timer_get_time() / 1000000);
}

static void cmd_info(char *args)
{
    (void)args;
    reply_more(213, "fw %s", FW_VERSION);
    reply_more(213, "heap %u", (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    reply(213, "spiffs_free n/a"); /* TODO: no SPIFFS partition mounted yet */
}

/* ICY station info (name/genre/bitrate, static per-connection) and the
 * most recent in-stream StreamTitle, if the station supports it and one
 * has arrived yet. */
static void cmd_streaminfo(char *args)
{
    (void)args;
    player_stats_t st;
    stream_player_get_stats(&st);

    reply_more(214, "name %s", st.station_name[0] ? st.station_name : "unknown");
    reply_more(214, "genre %s", st.station_genre[0] ? st.station_genre : "unknown");
    reply_more(214, "bitrate %d", st.station_bitrate_kbps);
    reply(214, "now_playing %s", st.now_playing[0] ? st.now_playing : "unknown");
}

static void cmd_save(char *args) { (void)args; reply(501, "not implemented - no SPIFFS config store yet"); }

static void cmd_reset(char *args)
{
    (void)args;
    reply(221, "bye");
    vTaskDelay(pdMS_TO_TICKS(100)); /* let the reply flush before reboot */
    esp_restart();
}

static void cmd_factory(char *args)
{
    (void)args;
    s_factory_confirm_deadline_us = esp_timer_get_time() + (int64_t)FACTORY_CONFIRM_TIMEOUT_MS * 1000;
    reply(450, "confirm required within %ds", FACTORY_CONFIRM_TIMEOUT_MS / 1000);
}

static void cmd_factory_confirm(char *args)
{
    (void)args;
    if (s_factory_confirm_deadline_us == 0 || esp_timer_get_time() > s_factory_confirm_deadline_us) {
        reply(450, "confirm expired, send FACTORY again");
        s_factory_confirm_deadline_us = 0;
        return;
    }
    s_factory_confirm_deadline_us = 0;
    /* TODO next stage: actually wipe the SPIFFS config partition once
     * it exists. Nothing persistent to wipe yet in this build. */
    reply(250, "ok");
}

/* --- Dispatch table --- */

typedef void (*cmd_fn_t)(char *args);
typedef struct { const char *verb; cmd_fn_t fn; } cmd_entry_t;

static const cmd_entry_t s_commands[] = {
    { "WIFI.SET",        cmd_wifi_set },
    { "WIFI.CONNECT",    cmd_wifi_connect },
    { "WIFI.DISCONNECT", cmd_wifi_disconnect },
    { "WIFI.STATUS",     cmd_wifi_status },
    { "PL.ADD",          cmd_pl_add },
    { "PL.DEL",          cmd_pl_del },
    { "PL.LIST",         cmd_pl_list },
    { "PL.CLEAR",        cmd_pl_clear },
    { "PL.RENAME",       cmd_pl_rename },
    { "PLAY",            cmd_play },
    { "STOP",            cmd_stop },
    { "PAUSE",           cmd_pause },
    { "NEXT",            cmd_next },
    { "PREV",            cmd_prev },
    { "VOL",             cmd_vol },
    { "VOL?",            cmd_vol_q },
    { "STATUS",          cmd_status },
    { "STATS",           cmd_stats },
    { "INFO",            cmd_info },
    { "STREAMINFO",      cmd_streaminfo },
    { "SAVE",            cmd_save },
    { "RESET",           cmd_reset },
    { "FACTORY",         cmd_factory },
    { "FACTORY CONFIRM", cmd_factory_confirm },
};

static void dispatch(char *line)
{
    /* Strip trailing CR/LF. */
    size_t len = strlen(line);
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n')) {
        line[--len] = '\0';
    }
    if (len == 0) {
        return;
    }

    /* "FACTORY CONFIRM" is the one two-word verb; check it before the
     * generic single-token split. */
    if (strncmp(line, "FACTORY CONFIRM", 15) == 0) {
        cmd_factory_confirm(NULL);
        return;
    }

    char *args = strchr(line, ' ');
    if (args != NULL) {
        *args = '\0';
        args++;
    }

    for (size_t i = 0; i < sizeof(s_commands) / sizeof(s_commands[0]); i++) {
        if (strcmp(line, s_commands[i].verb) == 0) {
            s_commands[i].fn(args);
            return;
        }
    }
    reply(500, "unknown command");
}

static void cli_task(void *arg)
{
    char line[256];
    size_t line_len = 0;
    uint8_t byte;

    reply(220, "wifi radio block ready");

    for (;;) {
        int n = uart_read_bytes(CLI_UART_NUM, &byte, 1, portMAX_DELAY);
        if (n <= 0) {
            continue;
        }
        /* Accept CR, LF, or CRLF as the line terminator - some terminals
         * (notably several Windows serial tools) send a bare CR with no
         * LF at all. Dispatching on whichever arrives first, and
         * silently ignoring an empty line, means a lone CR, a lone LF,
         * or a CRLF pair all behave the same way. */
        if (byte == '\r' || byte == '\n') {
            if (line_len > 0) {
                line[line_len] = '\0';
                dispatch(line);
                line_len = 0;
            }
        } else if (line_len < sizeof(line) - 1) {
            line[line_len++] = (char)byte;
            /* else: silently drop overlong input rather than overrun -
             * the next terminator still resets line_len for the next
             * command. */
        }
    }
}

void cli_start_task(void)
{
    s_out_mutex = xSemaphoreCreateMutex();

    uart_config_t uart_cfg = {
        .baud_rate = CLI_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_param_config(CLI_UART_NUM, &uart_cfg));
    ESP_ERROR_CHECK(uart_set_pin(CLI_UART_NUM, CLI_UART_TX_GPIO, CLI_UART_RX_GPIO,
                                   UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
    ESP_ERROR_CHECK(uart_driver_install(CLI_UART_NUM, 512, 512, 0, NULL, 0));

    xTaskCreate(cli_task, "cli", 4096, NULL, 8, NULL);
}
