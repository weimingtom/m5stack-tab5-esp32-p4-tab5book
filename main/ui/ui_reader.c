#include "ui.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "book_font.h"
#include "doc.h"
#include "esp_log.h"
#include "library.h"
#include "text.h"
#include "ui_internal.h"

static const char *TAG = "reader";

#define SCREEN_W 1280
#define SCREEN_H 720
#define CONTENT_X 40
#define CONTENT_Y 66
#define CONTENT_W 1200
#define CONTENT_H 556
#define TEXT_X 24
#define TEXT_Y 8
#define TEXT_W 1128
#define TEXT_H 540

static lv_obj_t *s_screen;
static doc_t *s_doc;
static doc_info_t s_info;
static char s_path[512];
static text_layout_t s_layout;
static int s_chapter;
static int s_page;
static int s_turn_count;
static bool s_bars_visible = true;

static lv_obj_t *s_top;
static lv_obj_t *s_bottom;
static lv_obj_t *s_text;
static lv_obj_t *s_page_label;
static lv_obj_t *s_chapter_label;
static lv_obj_t *s_progress;

static void reader_save(void);

static int reader_lines_per_page(void)
{
    int lh = book_font_body()->line_height;
    if (lh < 8) {
        lh = 32;
    }
    int lines = TEXT_H / lh;
    return lines > 0 ? lines : 1;
}

static int clamp_int(int v, int lo, int hi)
{
    if (v < lo) {
        return lo;
    }
    if (v > hi) {
        return hi;
    }
    return v;
}

/* ------------------------------------------------------------- render ---- */

static void reader_update_ui(void)
{
    if (!s_screen) {
        return;
    }
    char buf[128];
    int pages = s_layout.page_count > 0 ? s_layout.page_count : 1;

    if (s_info.type == DOC_TYPE_PDF) {
        snprintf(buf, sizeof(buf), "PDF %d/%d · %d/%d 屏", s_chapter + 1,
                 s_info.page_count, s_page + 1, pages);
    } else {
        snprintf(buf, sizeof(buf), "第 %d/%d 页", s_page + 1, pages);
    }
    lv_label_set_text(s_page_label, buf);

    char title[192];
    if (doc_chapter_title(s_doc, s_chapter, title, sizeof(title)) != ESP_OK) {
        snprintf(title, sizeof(title), "%s", s_info.title);
    }
    lv_label_set_text(s_chapter_label, title);

    double frac;
    if (s_info.chapter_count <= 1) {
        frac = (double)(s_page + 1) / pages;
    } else {
        frac = ((double)s_chapter + (double)(s_page + 1) / pages) / s_info.chapter_count;
    }
    if (frac < 0) {
        frac = 0;
    }
    if (frac > 1) {
        frac = 1;
    }
    lv_bar_set_value(s_progress, (int)(frac * 1000.0), LV_ANIM_OFF);
}

static void reader_render_page(void)
{
    if (!s_text) {
        return;
    }
    size_t len = 0;
    char *page = text_layout_page(&s_layout, s_page, &len);
    if (!page) {
        lv_label_set_text(s_text, "…");
    } else if (len == 0) {
        lv_label_set_text(s_text, "（本页为空）");
        free(page);
    } else {
        lv_label_set_text(s_text, page);
        free(page);
    }
    reader_update_ui();
}

static void reader_show_message(const char *msg)
{
    if (s_text) {
        lv_label_set_text(s_text, msg);
    }
}

static void reader_load_chapter(int chapter, int page_hint)
{
    if (!s_doc) {
        return;
    }
    if (chapter < 0) {
        chapter = 0;
    }
    if (chapter >= s_info.chapter_count) {
        chapter = s_info.chapter_count - 1;
    }

    char *text = NULL;
    size_t len = 0;
    esp_err_t err = doc_chapter_text(s_doc, chapter, &text, &len);
    if (err != ESP_OK || !text) {
        ESP_LOGE(TAG, "chapter %d load failed: %s", chapter, esp_err_to_name(err));
        reader_show_message("本章读取失败");
        return;
    }

    text_layout_free(&s_layout);
    err = text_layout_build(text, len, TEXT_W, reader_lines_per_page(), book_font_body(), &s_layout);
    if (err != ESP_OK) {
        free(text);
        reader_show_message("内存不足，无法排版");
        return;
    }
    if (s_layout.len == 0) {
        reader_show_message("（空文件）");
    }

    s_chapter = chapter;
    s_page = clamp_int(page_hint, 0, s_layout.page_count - 1);
    reader_render_page();
    ESP_LOGI(TAG, "chapter %d page %d/%d (%u bytes)", s_chapter + 1, s_page + 1,
             s_layout.page_count, (unsigned)len);
}

static void reader_save(void)
{
    if (!s_doc || !s_path[0]) {
        return;
    }
    library_touch(s_path, s_info.title, s_info.type, s_chapter, s_page);
}

/* ------------------------------------------------------------- nav ------- */

static void reader_next(void)
{
    if (!s_doc) {
        return;
    }
    if (s_page + 1 < s_layout.page_count) {
        s_page++;
        reader_render_page();
        if (++s_turn_count % 10 == 0) {
            reader_save();
        }
        return;
    }
    if (s_chapter + 1 < s_info.chapter_count) {
        reader_save();
        reader_load_chapter(s_chapter + 1, 0);
        reader_save();
        return;
    }
    ui_toast("已经是最后一页");
}

static void reader_prev(void)
{
    if (!s_doc) {
        return;
    }
    if (s_page > 0) {
        s_page--;
        reader_render_page();
        if (++s_turn_count % 10 == 0) {
            reader_save();
        }
        return;
    }
    if (s_chapter > 0) {
        reader_save();
        reader_load_chapter(s_chapter - 1, 0);
        /* Open the previous chapter at its last page. */
        s_page = s_layout.page_count - 1;
        reader_render_page();
        reader_save();
        return;
    }
    ui_toast("已经是第一页");
}

static void reader_toggle_bars(void)
{
    s_bars_visible = !s_bars_visible;
    if (s_bars_visible) {
        lv_obj_clear_flag(s_top, LV_OBJ_FLAG_HIDDEN);
        lv_obj_clear_flag(s_bottom, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_top, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_bottom, LV_OBJ_FLAG_HIDDEN);
    }
}

/* ------------------------------------------------------------- modal ----- */

static lv_obj_t *reader_modal(int w, int h)
{
    const ui_theme_t *th = ui_theme();
    lv_obj_t *m = lv_obj_create(s_screen);
    lv_obj_set_size(m, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(m, 0, 0);
    lv_obj_set_style_bg_color(m, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(m, LV_OPA_60, 0);
    lv_obj_set_style_border_width(m, 0, 0);
    lv_obj_set_style_radius(m, 0, 0);
    lv_obj_clear_flag(m, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *p = ui_panel(m, w, h);
    lv_obj_center(p);
    lv_obj_t *line = lv_obj_create(p);
    lv_obj_set_size(line, w - 48, 2);
    lv_obj_set_pos(line, 24, 64);
    lv_obj_set_style_bg_color(line, th->panel2, 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(line, 0, 0);
    lv_obj_set_style_radius(line, 0, 0);
    return m;
}

static void modal_close_cb(lv_event_t *e)
{
    lv_obj_t *m = (lv_obj_t *)lv_event_get_user_data(e);
    if (m) {
        lv_obj_del(m);
    }
}

static void chapter_row_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e) - 1;
    lv_obj_t *m = (lv_obj_t *)lv_obj_get_user_data(lv_event_get_target(e));
    if (m) {
        lv_obj_del(m);
    }
    if (idx >= 0) {
        reader_load_chapter(idx, 0);
        reader_save();
    }
}

static void reader_show_toc(void)
{
    if (!s_doc) {
        return;
    }
    const ui_theme_t *th = ui_theme();
    int chapter_count = s_info.chapter_count;
    int show = chapter_count > 150 ? 150 : chapter_count;
    lv_obj_t *m = reader_modal(920, 620);
    lv_obj_t *title = lv_label_create(m);
    lv_label_set_text(title, "目录");
    lv_obj_set_style_text_font(title, book_font_body(), 0);
    lv_obj_set_style_text_color(title, th->fg, 0);
    lv_obj_set_pos(title, 46, 20);

    lv_obj_t *close = ui_button(m, "关闭", 120, 44, modal_close_cb, m);
    lv_obj_set_pos(close, 754, 12);

    lv_obj_t *list = lv_obj_create(m);
    lv_obj_set_size(list, 860, 500);
    lv_obj_set_pos(list, 30, 82);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_style_pad_row(list, 6, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);

    for (int i = 0; i < show; i++) {
        char name[192];
        doc_chapter_title(s_doc, i, name, sizeof(name));
        char label[256];
        snprintf(label, sizeof(label), "%d. %s", i + 1, name);
        lv_obj_t *row = lv_button_create(list);
        lv_obj_set_size(row, 840, 52);
        lv_obj_set_style_bg_color(row, i == s_chapter ? th->panel2 : th->panel, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, 8, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_t *l = lv_label_create(row);
        lv_label_set_text(l, label);
        lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
        lv_obj_set_width(l, 800);
        lv_obj_set_style_text_font(l, ui_font(), 0);
        lv_obj_set_style_text_color(l, i == s_chapter ? th->accent : th->fg, 0);
        lv_obj_set_pos(l, 18, 10);
        lv_obj_set_user_data(row, m);
        lv_obj_add_event_cb(row, chapter_row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)(i + 1));
    }
    if (chapter_count > show) {
        lv_obj_t *more = ui_label(list, "（目录过长，仅显示前 150 章）", th->muted);
        lv_obj_set_pos(more, 10, 4);
    }
}

/* ------------------------------------------------------------- jump ------ */

static void jump_slider_cb(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    lv_obj_t *label = (lv_obj_t *)lv_event_get_user_data(e);
    int v = (int)lv_slider_get_value(slider);
    char buf[96];
    if (s_info.type == DOC_TYPE_PDF) {
        snprintf(buf, sizeof(buf), "第 %d / %d 页", v, s_info.page_count);
    } else {
        int pages = s_layout.page_count > 0 ? s_layout.page_count : 1;
        int target = (int)((double)(pages - 1) * v / 1000.0) + 1;
        snprintf(buf, sizeof(buf), "%d%% · 约第 %d 页", (int)(v / 10), target);
    }
    lv_label_set_text(label, buf);
}

static void jump_apply_cb(lv_event_t *e)
{
    lv_obj_t *m = (lv_obj_t *)lv_event_get_user_data(e);
    lv_obj_t *slider = (lv_obj_t *)lv_obj_get_user_data(m);
    int v = slider ? (int)lv_slider_get_value(slider) : 0;
    if (m) {
        lv_obj_del(m);
    }
    if (s_info.type == DOC_TYPE_PDF) {
        reader_load_chapter(clamp_int(v - 1, 0, s_info.chapter_count - 1), 0);
    } else {
        int pages = s_layout.page_count > 0 ? s_layout.page_count : 1;
        int target = (int)((double)(pages - 1) * v / 1000.0);
        reader_load_chapter(0, target);
    }
    reader_save();
}

static void reader_show_jump(void)
{
    const ui_theme_t *th = ui_theme();
    int pdf = s_info.type == DOC_TYPE_PDF;
    lv_obj_t *m = reader_modal(800, 360);
    lv_obj_t *title = lv_label_create(m);
    lv_label_set_text(title, pdf ? "跳转到 PDF 页" : "跳转到全书位置");
    lv_obj_set_style_text_font(title, book_font_body(), 0);
    lv_obj_set_style_text_color(title, th->fg, 0);
    lv_obj_set_pos(title, 40, 18);

    lv_obj_t *close = ui_button(m, "关闭", 110, 42, modal_close_cb, m);
    lv_obj_set_pos(close, 650, 12);

    lv_obj_t *label = ui_label(m, "", th->fg);
    lv_obj_set_pos(label, 40, 100);

    lv_obj_t *slider = lv_slider_create(m);
    lv_obj_set_size(slider, 720, 18);
    lv_obj_set_pos(slider, 40, 154);
    lv_obj_set_style_bg_color(slider, th->panel2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, th->accent, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, th->accent, LV_PART_KNOB);
    if (pdf) {
        lv_slider_set_range(slider, 1, s_info.page_count > 1 ? s_info.page_count : 1);
        lv_slider_set_value(slider, s_chapter + 1, LV_ANIM_OFF);
    } else {
        int pages = s_layout.page_count > 0 ? s_layout.page_count : 1;
        int pct = pages > 1 ? (int)((double)s_page / (pages - 1) * 1000.0) : 0;
        lv_slider_set_range(slider, 0, 1000);
        lv_slider_set_value(slider, pct, LV_ANIM_OFF);
    }
    lv_obj_set_user_data(m, slider);
    lv_obj_add_event_cb(slider, jump_slider_cb, LV_EVENT_VALUE_CHANGED, label);

    char buf[96];
    if (pdf) {
        snprintf(buf, sizeof(buf), "第 %d / %d 页", s_chapter + 1, s_info.page_count);
    } else {
        int pages = s_layout.page_count > 0 ? s_layout.page_count : 1;
        int pct = pages > 1 ? (int)((double)s_page / (pages - 1) * 100.0) : 100;
        snprintf(buf, sizeof(buf), "%d%% · 约第 %d 页", pct, s_page + 1);
    }
    lv_label_set_text(label, buf);

    lv_obj_t *go = ui_button(m, "跳转", 720, 56, jump_apply_cb, m);
    lv_obj_set_pos(go, 40, 238);
    lv_obj_set_style_bg_color(go, th->accent, 0);
    lv_obj_set_style_text_color(lv_obj_get_child(go, 0), lv_color_hex(0xFFFFFF), 0);
}

/* ------------------------------------------------------------- events ---- */

static void reader_prev_cb(lv_event_t *e)
{
    (void)e;
    reader_prev();
}

static void reader_next_cb(lv_event_t *e)
{
    (void)e;
    reader_next();
}

static void reader_toggle_cb(lv_event_t *e)
{
    (void)e;
    reader_toggle_bars();
}

static void reader_toc_cb(lv_event_t *e)
{
    (void)e;
    if (s_info.type == DOC_TYPE_PDF) {
        reader_show_jump();
    } else if (s_info.type == DOC_TYPE_TXT) {
        reader_show_jump();
    } else {
        reader_show_toc();
    }
}

static void reader_back_cb(lv_event_t *e)
{
    (void)e;
    reader_save();
    lv_obj_t *old = s_screen;
    s_screen = NULL;
    text_layout_free(&s_layout);
    doc_close(s_doc);
    s_doc = NULL;
    (void)old;
    ui_show_browser(ui_browser_path);
}

static void reader_theme_cb(lv_event_t *e)
{
    (void)e;
    int t = (library_get_theme() + 1) % 3;
    library_set_theme(t);
    ui_reader_apply_theme();
}

static void reader_gesture_cb(lv_event_t *e)
{
    lv_indev_t *indev = lv_indev_active();
    if (!indev) {
        return;
    }
    lv_dir_t dir = lv_indev_get_gesture_dir(indev);
    if (dir == LV_DIR_LEFT) {
        reader_next();
    } else if (dir == LV_DIR_RIGHT) {
        reader_prev();
    }
    (void)e;
}

/* ------------------------------------------------------------- build ----- */

static void reader_build_screen(void)
{
    const ui_theme_t *th = ui_theme();
    s_screen = lv_obj_create(NULL);
    ui_apply_bg(s_screen);

    s_top = ui_panel(s_screen, SCREEN_W, 58);
    lv_obj_set_style_radius(s_top, 0, 0);
    lv_obj_set_pos(s_top, 0, 0);

    lv_obj_t *back = ui_button(s_top, "返回", 88, 40, reader_back_cb, NULL);
    lv_obj_set_pos(back, 12, 9);

    lv_obj_t *title = lv_label_create(s_top);
    lv_label_set_text(title, s_info.title);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_obj_set_width(title, 760);
    lv_obj_set_style_text_font(title, ui_font(), 0);
    lv_obj_set_style_text_color(title, th->fg, 0);
    lv_obj_set_pos(title, 112, 14);

    const char *tag = doc_type_name(s_info.type);
    lv_obj_t *type = lv_label_create(s_top);
    lv_label_set_text(type, tag);
    lv_obj_set_style_text_font(type, ui_font(), 0);
    lv_obj_set_style_text_color(type, th->accent, 0);
    lv_obj_set_pos(type, 890, 16);

    lv_obj_t *theme = ui_button(s_top, "主题", 82, 40, reader_theme_cb, NULL);
    lv_obj_set_pos(theme, 1030, 9);
    lv_obj_t *toc = ui_button(s_top, s_info.type == DOC_TYPE_EPUB ? "目录" : "跳转",
                              110, 40, reader_toc_cb, NULL);
    lv_obj_set_pos(toc, 1124, 9);

    lv_obj_t *content = ui_panel(s_screen, CONTENT_W, CONTENT_H);
    lv_obj_set_style_bg_color(content, th->text_bg, 0);
    lv_obj_set_style_radius(content, 12, 0);
    lv_obj_set_pos(content, CONTENT_X, CONTENT_Y);

    s_text = lv_label_create(content);
    lv_label_set_text(s_text, "");
    lv_obj_set_size(s_text, TEXT_W, TEXT_H);
    lv_obj_set_pos(s_text, TEXT_X, TEXT_Y);
    lv_label_set_long_mode(s_text, LV_LABEL_LONG_CLIP);
    lv_obj_set_style_text_font(s_text, book_font_body(), 0);
    lv_obj_set_style_text_color(s_text, th->fg, 0);
    lv_obj_set_style_text_line_space(s_text, 0, 0);
    lv_obj_set_style_text_align(s_text, LV_TEXT_ALIGN_LEFT, 0);

    lv_obj_t *zones[3];
    int zx[3] = {0, 240, 960};
    int zw[3] = {240, 720, 240};
    for (int i = 0; i < 3; i++) {
        zones[i] = lv_obj_create(content);
        lv_obj_set_size(zones[i], zw[i], CONTENT_H);
        lv_obj_set_pos(zones[i], zx[i], 0);
        lv_obj_set_style_bg_opa(zones[i], LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(zones[i], 0, 0);
        lv_obj_set_style_radius(zones[i], 0, 0);
        lv_obj_set_style_pad_all(zones[i], 0, 0);
        lv_obj_clear_flag(zones[i], LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(zones[i], LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_event_cb(zones[i], i == 0 ? reader_prev_cb : i == 2 ? reader_next_cb
                                                                     : reader_toggle_cb,
                            LV_EVENT_CLICKED, NULL);
        lv_obj_add_event_cb(zones[i], reader_gesture_cb, LV_EVENT_GESTURE, NULL);
    }

    s_bottom = ui_panel(s_screen, SCREEN_W, 98);
    lv_obj_set_style_radius(s_bottom, 0, 0);
    lv_obj_set_pos(s_bottom, 0, 622);

    s_progress = lv_bar_create(s_bottom);
    lv_obj_set_size(s_progress, SCREEN_W, 4);
    lv_obj_set_pos(s_progress, 0, 0);
    lv_bar_set_range(s_progress, 0, 1000);
    lv_obj_set_style_bg_color(s_progress, th->panel2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_progress, th->accent, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_progress, 0, LV_PART_MAIN);
    lv_obj_set_style_radius(s_progress, 0, LV_PART_INDICATOR);

    s_page_label = lv_label_create(s_bottom);
    lv_label_set_text(s_page_label, "");
    lv_obj_set_style_text_font(s_page_label, ui_font(), 0);
    lv_obj_set_style_text_color(s_page_label, th->fg, 0);
    lv_obj_set_pos(s_page_label, 24, 18);

    s_chapter_label = lv_label_create(s_bottom);
    lv_label_set_text(s_chapter_label, "");
    lv_label_set_long_mode(s_chapter_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(s_chapter_label, 520);
    lv_obj_set_style_text_font(s_chapter_label, ui_font(), 0);
    lv_obj_set_style_text_color(s_chapter_label, th->muted, 0);
    lv_obj_set_pos(s_chapter_label, 330, 18);

    lv_obj_t *prev = ui_button(s_bottom, "上一页", 150, 50, reader_prev_cb, NULL);
    lv_obj_set_pos(prev, 890, 34);
    lv_obj_t *next = ui_button(s_bottom, "下一页", 150, 50, reader_next_cb, NULL);
    lv_obj_set_pos(next, 1100, 34);
}

/* ------------------------------------------------------------- API ------- */

void ui_reader_create(doc_t *doc, const char *path)
{
    if (!doc) {
        return;
    }
    s_doc = doc;
    s_info = *doc_info(doc);
    snprintf(s_path, sizeof(s_path), "%s", path);
    memset(&s_layout, 0, sizeof(s_layout));
    s_chapter = 0;
    s_page = 0;
    s_turn_count = 0;
    s_bars_visible = true;

    reader_build_screen();

    int ch = 0, pg = 0;
    library_get_position(path, &ch, &pg);
    reader_load_chapter(ch, pg);
    ui_replace_screen(s_screen);
    reader_save();
}

bool ui_reader_active(void)
{
    return s_screen != NULL;
}

void ui_reader_apply_theme(void)
{
    if (!s_screen) {
        return;
    }
    int ch = s_chapter;
    int pg = s_page;
    reader_save();
    lv_obj_t *old = s_screen;
    s_screen = NULL;
    reader_build_screen();
    reader_load_chapter(ch, pg);
    lv_screen_load(s_screen);
    lv_obj_del_async(old);
}
