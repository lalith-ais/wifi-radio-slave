#pragma once

#include "esp_err.h"
#include "config.h"

/*
 * SPIFFS-backed persistence for the playlist and "what was last
 * playing", so both survive a reboot.
 *
 * TODO next: WiFi credentials still aren't persisted here (WIFI.SET
 * remains a stub) - this module only covers the playlist and last-index
 * for now.
 */

/* Mounts (formatting if necessary) the "storage" SPIFFS partition at
 * /spiffs. Call once at boot, before any other storage_* call. */
esp_err_t storage_init(void);

typedef struct {
    char url[PLAYLIST_URL_MAX];
    char name[PLAYLIST_NAME_MAX];
} storage_playlist_entry_t;

/* Saves the given playlist to SPIFFS, overwriting whatever was there
 * before. Called explicitly (PL.SAVE) - playlist edits are a deliberate
 * user action, not auto-persisted on every PL.ADD/DEL. */
esp_err_t storage_save_playlist(const storage_playlist_entry_t *entries, int count);

/* Loads the saved playlist into `out` (capacity `max_entries`).
 * *out_count is set to how many were loaded - 0 with ESP_OK means no
 * saved playlist exists yet, which is not an error. */
esp_err_t storage_load_playlist(storage_playlist_entry_t *out, int max_entries, int *out_count);

/*
 * Persists which playlist index was last played. Unlike the playlist
 * itself, this is written automatically on every successful PLAY/NEXT/
 * PREV, not gated behind an explicit save - "what's currently playing"
 * is small, frequent state, and the whole point is surviving an
 * unexpected power loss, not just a deliberate save.
 */
esp_err_t storage_save_last_index(int index);

/* Returns the last-played index, or -1 if none was ever saved. */
int storage_load_last_index(void);
