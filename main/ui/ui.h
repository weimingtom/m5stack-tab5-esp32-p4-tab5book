#pragma once

#include <stdbool.h>

#include "doc.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

void ui_init(bool sd_ok);
void ui_show_home(void);
void ui_show_browser(const char *path);
void ui_open_book(const char *path);

#ifdef __cplusplus
}
#endif
