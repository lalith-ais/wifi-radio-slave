#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
#include "config.h"

typedef enum {
    PLAYER_STATE_IDLE,
    PLAYER_STATE_BUFFERING,
    PLAYER_STATE_PLAYING,
    PLAYER_STATE_ERROR,
} player_state_t;

typedef struct {
    player_state_t state;
    int current_index;                    /* -1 if nothing loaded */
    char current_name[PLAYLIST_NAME_MAX];
    int sample_rate;
    int channels;
    int bits_per_sample;
    double cpu_pct;
    double buffer_pct;
} player_stats_t;

/* Registers the esp_audio_codec decoders and creates the internal queue. */
void stream_player_init(void);

/* Starts the player task. Call once, after stream_player_init(). */
void stream_player_start_task(void);

/*
 * Queues a URL for playback, replacing whatever is currently playing.
 * Called from the CLI's PLAY handler. Non-blocking.
 *
 * TODO next stage: this only decodes and measures - there is still no
 * I2S output stage. Wire the decoded PCM into the WM8960 I2S driver
 * once that's brought up, per the pure-internet-radio hardware plan.
 */
esp_err_t stream_player_play(int index, const char *url, const char *name);

/*
 * Requests the current stream stop after its next read iteration.
 * Checked cooperatively inside the read loop, so it isn't instant.
 */
void stream_player_stop(void);

/* Snapshot of current state for the CLI's STATUS/STATS replies. */
void stream_player_get_stats(player_stats_t *out);
