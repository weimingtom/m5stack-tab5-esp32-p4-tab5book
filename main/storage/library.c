#include "library.h"

#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "library";

#define NVS_NS       "book"
#define KEY_RECENT   "recent"
#define KEY_LAST     "last"
#define KEY_THEME    "theme"
#define KEY_BRIGHT   "bright"

static nvs_handle_t s_nvs;
static lib_recent_t s_recent[LIB_MAX_RECENT];
static int s_count;
static char s_last[LIB_PATH_LEN];
static int s_theme = 1; /* 0 light, 1 sepia, 2 dark */
static int s_brightness = 80;

static void save_recent(void)
{
    if (!s_nvs) {
        return;
    }
    esp_err_t err = nvs_set_blob(s_nvs, KEY_RECENT, s_recent, sizeof(s_recent));
    if (err == ESP_OK) {
        err = nvs_commit(s_nvs);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "save recent failed: %s", esp_err_to_name(err));
    }
}

esp_err_t library_init(void)
{
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &s_nvs);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "nvs_open: %s", esp_err_to_name(err));
        return err;
    }

    size_t blob_len = sizeof(s_recent);
    err = nvs_get_blob(s_nvs, KEY_RECENT, s_recent, &blob_len);
    if (err == ESP_OK && blob_len == sizeof(s_recent)) {
        s_count = 0;
        for (int i = 0; i < LIB_MAX_RECENT; i++) {
            if (s_recent[i].path[0]) {
                s_count++;
            }
        }
    } else {
        memset(s_recent, 0, sizeof(s_recent));
        s_count = 0;
    }

    size_t last_len = sizeof(s_last);
    if (nvs_get_str(s_nvs, KEY_LAST, s_last, &last_len) != ESP_OK) {
        s_last[0] = '\0';
    }
    int32_t v = 0;
    if (nvs_get_i32(s_nvs, KEY_THEME, &v) == ESP_OK && v >= 0 && v <= 2) {
        s_theme = (int)v;
    }
    if (nvs_get_i32(s_nvs, KEY_BRIGHT, &v) == ESP_OK && v >= 5 && v <= 100) {
        s_brightness = (int)v;
    }
    return ESP_OK;
}

int library_recent_count(void)
{
    return s_count;
}

const lib_recent_t *library_recent_at(int index)
{
    if (index < 0 || index >= s_count) {
        return NULL;
    }
    return &s_recent[index];
}

const char *library_last_path(void)
{
    return s_last[0] ? s_last : NULL;
}

bool library_get_position(const char *path, int *chapter, int *page)
{
    if (!path) {
        return false;
    }
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_recent[i].path, path) == 0) {
            if (chapter) {
                *chapter = s_recent[i].chapter;
            }
            if (page) {
                *page = s_recent[i].page;
            }
            return true;
        }
    }
    return false;
}

void library_touch(const char *path, const char *title, doc_type_t type, int chapter, int page)
{
    if (!path || !path[0]) {
        return;
    }
    int found = -1;
    for (int i = 0; i < s_count; i++) {
        if (strcmp(s_recent[i].path, path) == 0) {
            found = i;
            break;
        }
    }

    lib_recent_t entry;
    memset(&entry, 0, sizeof(entry));
    snprintf(entry.path, sizeof(entry.path), "%s", path);
    snprintf(entry.title, sizeof(entry.title), "%s", title ? title : "未命名");
    entry.type = (uint8_t)type;
    entry.chapter = chapter;
    entry.page = page;

    int insert = found >= 0 ? found : (s_count < LIB_MAX_RECENT ? s_count : LIB_MAX_RECENT - 1);
    if (found < 0 && s_count < LIB_MAX_RECENT) {
        s_count++;
    }
    if (insert > 0) {
        memmove(&s_recent[1], &s_recent[0], (size_t)insert * sizeof(lib_recent_t));
    }
    s_recent[0] = entry;

    snprintf(s_last, sizeof(s_last), "%s", path);
    if (s_nvs) {
        nvs_set_str(s_nvs, KEY_LAST, s_last);
        nvs_commit(s_nvs);
    }
    save_recent();
}

void library_set_theme(int theme)
{
    if (theme < 0 || theme > 2) {
        return;
    }
    s_theme = theme;
    if (s_nvs) {
        nvs_set_i32(s_nvs, KEY_THEME, theme);
        nvs_commit(s_nvs);
    }
}

int library_get_theme(void)
{
    return s_theme;
}

void library_set_brightness(int percent)
{
    if (percent < 5) {
        percent = 5;
    }
    if (percent > 100) {
        percent = 100;
    }
    s_brightness = percent;
    if (s_nvs) {
        nvs_set_i32(s_nvs, KEY_BRIGHT, percent);
        nvs_commit(s_nvs);
    }
}

int library_get_brightness(void)
{
    return s_brightness;
}

void library_clear_recent(void)
{
    memset(s_recent, 0, sizeof(s_recent));
    s_count = 0;
    memset(s_last, 0, sizeof(s_last));
    if (s_nvs) {
        nvs_erase_key(s_nvs, KEY_LAST);
        save_recent();
        nvs_commit(s_nvs);
    }
}
