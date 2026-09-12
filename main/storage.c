#include <stdio.h>
#include <string.h>
#include "esp_spiffs.h"
#include "esp_log.h"
#include "storage.h"

static const char *TAG = "storage";

#define PLAYLIST_PATH    "/spiffs/playlist.txt"
#define LAST_INDEX_PATH  "/spiffs/last_index.txt"

esp_err_t storage_init(void)
{
    esp_vfs_spiffs_conf_t conf = {
        .base_path = "/spiffs",
        .partition_label = "storage",
        .max_files = 4,
        .format_if_mount_failed = true,
    };
    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SPIFFS mount failed: %s - playlist/last-station persistence disabled",
                  esp_err_to_name(err));
        return err;
    }

    size_t total = 0, used = 0;
    esp_spiffs_info("storage", &total, &used);
    ESP_LOGI(TAG, "SPIFFS mounted: %u/%u bytes used", (unsigned)used, (unsigned)total);
    return ESP_OK;
}

esp_err_t storage_save_playlist(const storage_playlist_entry_t *entries, int count)
{
    FILE *f = fopen(PLAYLIST_PATH, "w");
    if (f == NULL) {
        ESP_LOGE(TAG, "Failed to open %s for writing", PLAYLIST_PATH);
        return ESP_FAIL;
    }
    /* Tab-separated, one entry per line - simple and sufficient, and a
     * tab is not something PL.ADD/PL.RENAME ever accepts into a name or
     * URL, so it's a safe delimiter. */
    for (int i = 0; i < count; i++) {
        fprintf(f, "%s\t%s\n", entries[i].url, entries[i].name);
    }
    fclose(f);
    ESP_LOGI(TAG, "Saved %d playlist entries", count);
    return ESP_OK;
}

esp_err_t storage_load_playlist(storage_playlist_entry_t *out, int max_entries, int *out_count)
{
    *out_count = 0;
    FILE *f = fopen(PLAYLIST_PATH, "r");
    if (f == NULL) {
        return ESP_OK; /* nothing saved yet - not an error */
    }

    char line[PLAYLIST_URL_MAX + PLAYLIST_NAME_MAX + 4];
    int count = 0;
    while (count < max_entries && fgets(line, sizeof(line), f) != NULL) {
        line[strcspn(line, "\r\n")] = '\0';
        char *tab = strchr(line, '\t');
        if (tab == NULL) {
            continue; /* malformed line - skip rather than abort the whole load */
        }
        *tab = '\0';
        strncpy(out[count].url, line, sizeof(out[count].url) - 1);
        out[count].url[sizeof(out[count].url) - 1] = '\0';
        strncpy(out[count].name, tab + 1, sizeof(out[count].name) - 1);
        out[count].name[sizeof(out[count].name) - 1] = '\0';
        count++;
    }
    fclose(f);
    *out_count = count;
    ESP_LOGI(TAG, "Loaded %d playlist entries", count);
    return ESP_OK;
}

esp_err_t storage_save_last_index(int index)
{
    FILE *f = fopen(LAST_INDEX_PATH, "w");
    if (f == NULL) {
        return ESP_FAIL;
    }
    fprintf(f, "%d\n", index);
    fclose(f);
    return ESP_OK;
}

int storage_load_last_index(void)
{
    FILE *f = fopen(LAST_INDEX_PATH, "r");
    if (f == NULL) {
        return -1;
    }
    int idx = -1;
    if (fscanf(f, "%d", &idx) != 1) {
        idx = -1;
    }
    fclose(f);
    return idx;
}
