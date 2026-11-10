#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "doc.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

#define LIB_MAX_RECENT 8
#define LIB_PATH_LEN   256
#define LIB_TITLE_LEN  128

typedef struct {
    char path[LIB_PATH_LEN];
    char title[LIB_TITLE_LEN];
    uint8_t type;
    int chapter;
    int page;
} lib_recent_t;

esp_err_t library_init(void);

int library_recent_count(void);
const lib_recent_t *library_recent_at(int index);
const char *library_last_path(void);
bool library_get_position(const char *path, int *chapter, int *page);

/* Move/update an entry at the front of the recent list and persist it. */
void library_touch(const char *path, const char *title, doc_type_t type, int chapter, int page);

void library_set_theme(int theme);
int library_get_theme(void);
void library_set_brightness(int percent);
int library_get_brightness(void);
void library_clear_recent(void);

#ifdef __cplusplus
}
#endif
