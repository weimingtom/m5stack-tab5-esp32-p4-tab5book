#pragma once

#include <stdbool.h>

#include "doc.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    lv_color_t bg;
    lv_color_t fg;
    lv_color_t muted;
    lv_color_t panel;
    lv_color_t panel2;
    lv_color_t accent;
    lv_color_t text_bg;
} ui_theme_t;

const ui_theme_t *ui_theme(void);
const lv_font_t *ui_font(void);

void ui_apply_bg(lv_obj_t *obj);
lv_obj_t *ui_button(lv_obj_t *parent, const char *text, int w, int h, lv_event_cb_t cb,
                    void *user_data);
lv_obj_t *ui_panel(lv_obj_t *parent, int w, int h);
lv_obj_t *ui_label(lv_obj_t *parent, const char *text, lv_color_t color);

void ui_toast(const char *msg);
lv_obj_t *ui_loading(const char *msg);
void ui_loading_done(lv_obj_t *loading);

/* Load a new screen and delete the previous one asynchronously. */
void ui_replace_screen(lv_obj_t *scr);

void ui_show_settings(void);
void ui_show_about(void);

/* Implemented in ui_reader.c. */
void ui_reader_create(doc_t *doc, const char *path);
void ui_reader_apply_theme(void);
bool ui_reader_active(void);

extern char ui_browser_path[512];

#ifdef __cplusplus
}
#endif
