#include "ui.h"

#include <ctype.h>
#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

#include "book_font.h"
#include "bsp/esp-bsp.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "library.h"
#include "rmem.h"
#include "text.h"
#include "ui_internal.h"

static const char *TAG = "ui";

char ui_browser_path[512] = "/sdcard";

typedef struct {
    char name[256];
    char path[512];
    bool dir;
    size_t size;
} browser_entry_t;

/* 300 entries are ~230 KB; keep them in PSRAM, not internal .bss. */
#define BROWSER_MAX_ENTRIES 300
static browser_entry_t *s_entries;
static int s_entry_count;
static bool s_sd_ok;

static lv_obj_t *s_toast;
static lv_timer_t *s_toast_timer;
static lv_obj_t *s_settings_modal;

/* ------------------------------------------------------------- theme ----- */

/* ARGB colours: 0xRRGGBB.  LVGL's lv_color_t is a bitfield in RGB565 builds,
 * so convert at runtime instead of using static initialisers. */
static const uint32_t s_theme_hex[3][7] = {
    {0xF5F2EA, 0x25201A, 0x7A8288, 0xFFFFFF, 0xE8ECEF, 0xB5651D, 0xFBF8F2},
    {0xEDE0C8, 0x3A2F22, 0x776B5A, 0xFAF0E6, 0xF0E4D0, 0x9C6B3C, 0xF4E8D0},
    {0x0F1418, 0xD8DEE6, 0x928A80, 0x1A222A, 0x242D36, 0xE0A458, 0x141B21},
};

static ui_theme_t s_theme_cache;

static lv_color_t color_hex(uint32_t v)
{
    return lv_color_hex(v);
}

const ui_theme_t *ui_theme(void)
{
    int t = library_get_theme();
    if (t < 0 || t > 2) {
        t = 1;
    }
    const uint32_t *h = s_theme_hex[t];
    s_theme_cache.bg = lv_color_hex(h[0]);
    s_theme_cache.fg = lv_color_hex(h[1]);
    s_theme_cache.muted = lv_color_hex(h[2]);
    s_theme_cache.panel = lv_color_hex(h[3]);
    s_theme_cache.panel2 = lv_color_hex(h[4]);
    s_theme_cache.accent = lv_color_hex(h[5]);
    s_theme_cache.text_bg = lv_color_hex(h[6]);
    return &s_theme_cache;
}

const lv_font_t *ui_font(void)
{
    return book_font_ui();
}

void ui_apply_bg(lv_obj_t *obj)
{
    const ui_theme_t *th = ui_theme();
    lv_obj_set_style_bg_color(obj, th->bg, 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_radius(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

lv_obj_t *ui_panel(lv_obj_t *parent, int w, int h)
{
    const ui_theme_t *th = ui_theme();
    lv_obj_t *p = lv_obj_create(parent);
    lv_obj_set_size(p, w, h);
    lv_obj_set_style_bg_color(p, th->panel, 0);
    lv_obj_set_style_bg_opa(p, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(p, 0, 0);
    lv_obj_set_style_radius(p, 14, 0);
    lv_obj_set_style_pad_all(p, 0, 0);
    lv_obj_clear_flag(p, LV_OBJ_FLAG_SCROLLABLE);
    return p;
}

lv_obj_t *ui_label(lv_obj_t *parent, const char *text, lv_color_t color)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, ui_font(), 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_obj_clear_flag(l, LV_OBJ_FLAG_SCROLLABLE);
    return l;
}

lv_obj_t *ui_button(lv_obj_t *parent, const char *text, int w, int h, lv_event_cb_t cb,
                    void *user_data)
{
    const ui_theme_t *th = ui_theme();
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, w, h);
    lv_obj_set_style_bg_color(btn, th->panel2, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(btn, 12, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_style_pad_all(btn, 0, 0);
    lv_obj_t *l = lv_label_create(btn);
    lv_label_set_text(l, text);
    lv_obj_set_style_text_font(l, ui_font(), 0);
    lv_obj_set_style_text_color(l, th->fg, 0);
    lv_obj_set_width(l, w - 16);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(l);
    if (cb) {
        lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    }
    return btn;
}

void ui_replace_screen(lv_obj_t *scr)
{
    lv_obj_t *old = lv_screen_active();
    lv_screen_load(scr);
    if (old && old != scr) {
        lv_obj_del_async(old);
    }
}

/* ------------------------------------------------------------- toast ----- */

static void toast_timer_cb(lv_timer_t *timer)
{
    if (s_toast) {
        lv_obj_del(s_toast);
        s_toast = NULL;
    }
    s_toast_timer = NULL;
    lv_timer_del(timer);
}

void ui_toast(const char *msg)
{
    if (s_toast_timer) {
        lv_timer_del(s_toast_timer);
        s_toast_timer = NULL;
    }
    if (s_toast) {
        lv_obj_del(s_toast);
        s_toast = NULL;
    }
    const ui_theme_t *th = ui_theme();
    lv_obj_t *t = lv_obj_create(lv_layer_top());
    lv_obj_set_size(t, LV_SIZE_CONTENT, 52);
    lv_obj_set_style_max_width(t, 900, 0);
    lv_obj_set_style_bg_color(t, th->panel, 0);
    lv_obj_set_style_bg_opa(t, LV_OPA_90, 0);
    lv_obj_set_style_border_width(t, 0, 0);
    lv_obj_set_style_radius(t, 12, 0);
    lv_obj_set_style_pad_hor(t, 24, 0);
    lv_obj_set_style_pad_ver(t, 8, 0);
    lv_obj_clear_flag(t, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *l = lv_label_create(t);
    lv_label_set_text(l, msg);
    lv_obj_set_style_text_font(l, ui_font(), 0);
    lv_obj_set_style_text_color(l, th->fg, 0);
    lv_obj_center(l);
    lv_obj_align(t, LV_ALIGN_BOTTOM_MID, 0, -110);
    s_toast = t;
    s_toast_timer = lv_timer_create(toast_timer_cb, 1900, NULL);
}

/* ------------------------------------------------------------- loading --- */

lv_obj_t *ui_loading(const char *msg)
{
    const ui_theme_t *th = ui_theme();
    lv_obj_t *m = lv_obj_create(lv_screen_active());
    lv_obj_set_size(m, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(m, 0, 0);
    lv_obj_set_style_bg_color(m, color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(m, LV_OPA_60, 0);
    lv_obj_set_style_border_width(m, 0, 0);
    lv_obj_set_style_radius(m, 0, 0);
    lv_obj_clear_flag(m, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *p = ui_panel(m, 520, 200);
    lv_obj_center(p);
    lv_obj_t *sp = lv_spinner_create(p);
    lv_obj_set_size(sp, 52, 52);
    lv_obj_align(sp, LV_ALIGN_TOP_MID, 0, 38);
    lv_obj_set_style_arc_color(sp, th->accent, LV_PART_INDICATOR);
    lv_obj_set_style_arc_width(sp, 5, LV_PART_INDICATOR);
    lv_obj_set_style_arc_color(sp, th->panel2, LV_PART_MAIN);
    lv_obj_set_style_arc_width(sp, 5, LV_PART_MAIN);
    lv_obj_t *l = ui_label(p, msg, th->fg);
    lv_obj_set_width(l, 460);
    lv_obj_set_style_text_align(l, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(l, LV_ALIGN_BOTTOM_MID, 0, -36);
    lv_refr_now(NULL);
    return m;
}

void ui_loading_done(lv_obj_t *loading)
{
    if (loading) {
        lv_obj_del(loading);
    }
}

/* ------------------------------------------------------------- home ------ */

static void home_open_cb(lv_event_t *e)
{
    (void)e;
    if (!s_sd_ok) {
        ui_toast("未检测到 SD 卡");
        return;
    }
    ui_show_browser(ui_browser_path);
}

static void home_continue_cb(lv_event_t *e)
{
    (void)e;
    const char *last = library_last_path();
    if (!last) {
        ui_toast("还没有阅读记录");
        return;
    }
    ui_open_book(last);
}

static void home_recent_cb(lv_event_t *e)
{
    (void)e;
    if (library_recent_count() == 0) {
        ui_toast("还没有阅读记录");
        return;
    }
    /* Rebuild the home screen so the recent panel reflects the latest NVS data. */
    ui_show_home();
}

static void home_settings_cb(lv_event_t *e)
{
    (void)e;
    ui_show_settings();
}

static void home_about_cb(lv_event_t *e)
{
    (void)e;
    ui_show_about();
}

static void recent_row_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e);
    const lib_recent_t *r = library_recent_at(idx);
    if (r) {
        ui_open_book(r->path);
    }
}

void ui_show_home(void)
{
    const ui_theme_t *th = ui_theme();
    lv_obj_t *scr = lv_obj_create(NULL);
    ui_apply_bg(scr);

    lv_obj_t *title = lv_label_create(scr);
    lv_label_set_text(title, "Tab5 阅读器");
    lv_obj_set_style_text_font(title, book_font_body(), 0);
    lv_obj_set_style_text_color(title, th->fg, 0);
    lv_obj_set_pos(title, 48, 34);

	//·
    lv_obj_t *sub = ui_label(scr, "TXT  ,  EPUB  ,  PDF", th->accent);
    lv_obj_set_pos(sub, 50, 74);

    lv_obj_t *line = lv_obj_create(scr);
    lv_obj_set_size(line, 1184, 2);
    lv_obj_set_pos(line, 48, 112);
    lv_obj_set_style_bg_color(line, th->panel2, 0);
    lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(line, 0, 0);
    lv_obj_set_style_radius(line, 0, 0);

    lv_obj_t *left = ui_panel(scr, 520, 500);
    lv_obj_set_pos(left, 48, 140);
    lv_obj_t *menu = ui_label(left, "阅读", th->muted);
    lv_obj_set_pos(menu, 28, 22);

    struct {
        const char *text;
        lv_event_cb_t cb;
    } items[] = {
        {"打开 SD 卡", home_open_cb},
        {"继续阅读", home_continue_cb},
        {"刷新最近", home_recent_cb},
        {"设置", home_settings_cb},
        {"关于", home_about_cb},
    };
    for (int i = 0; i < 5; i++) {
        lv_obj_t *b = ui_button(left, items[i].text, 464, 64, items[i].cb, NULL);
        lv_obj_set_pos(b, 28, 62 + i * 78);
        if (i == 0 && !s_sd_ok) {
            lv_obj_set_style_opa(b, LV_OPA_50, 0);
        }
    }

    lv_obj_t *right = ui_panel(scr, 640, 500);
    lv_obj_set_pos(right, 592, 140);
    lv_obj_t *rt = ui_label(right, "最近阅读", th->muted);
    lv_obj_set_pos(rt, 28, 22);

    int count = library_recent_count();
    if (count == 0) {
        lv_obj_t *empty = ui_label(right, s_sd_ok ? "打开一本 TXT / EPUB / PDF 开始阅读"
                                                 : "SD 卡未挂载，请插入书籍卡",
                                  th->muted);
        lv_obj_set_width(empty, 580);
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_align(empty, LV_ALIGN_CENTER, 0, 0);
    } else {
        if (count > 5) {
            count = 5;
        }
        for (int i = 0; i < count; i++) {
            const lib_recent_t *r = library_recent_at(i);
            lv_obj_t *row = lv_button_create(right);
            lv_obj_set_size(row, 584, 76);
            lv_obj_set_pos(row, 28, 58 + i * 86);
            lv_obj_set_style_bg_color(row, th->panel2, 0);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
            lv_obj_set_style_radius(row, 10, 0);
            lv_obj_set_style_border_width(row, 0, 0);
            lv_obj_set_style_pad_all(row, 0, 0);
            lv_obj_t *t = lv_label_create(row);
            lv_label_set_text(t, r->title);
            lv_label_set_long_mode(t, LV_LABEL_LONG_DOT);
            lv_obj_set_width(t, 430);
            lv_obj_set_style_text_font(t, ui_font(), 0);
            lv_obj_set_style_text_color(t, th->fg, 0);
            lv_obj_set_pos(t, 18, 10);
            const char *tn = r->type == DOC_TYPE_EPUB ? "EPUB"
                             : r->type == DOC_TYPE_PDF ? "PDF"
                                                       : "TXT";
            lv_obj_t *tag = lv_label_create(row);
            lv_label_set_text(tag, tn);
            lv_obj_set_style_text_font(tag, ui_font(), 0);
            lv_obj_set_style_text_color(tag, th->accent, 0);
            lv_obj_set_pos(tag, 18, 44);
            lv_obj_t *pg = lv_label_create(row);
            char pgbuf[48];
			//·
            snprintf(pgbuf, sizeof(pgbuf), "%d 章 , %d 页", r->chapter + 1, r->page + 1);
            lv_label_set_text(pg, pgbuf);
            lv_obj_set_style_text_font(pg, ui_font(), 0);
            lv_obj_set_style_text_color(pg, th->muted, 0);
            lv_obj_set_pos(pg, 420, 24);
            lv_obj_add_event_cb(row, recent_row_cb, LV_EVENT_CLICKED, (void *)(intptr_t)i);
        }
    }

    lv_obj_t *status = ui_label(scr, s_sd_ok ? "SD 卡：已挂载  /sdcard" : "SD 卡：未挂载",
                                s_sd_ok ? th->muted : th->accent);
    lv_obj_set_pos(status, 50, 680);

    ui_replace_screen(scr);
}

/* ------------------------------------------------------------- browser --- */

static int entry_cmp(const void *a, const void *b)
{
    const browser_entry_t *ea = a;
    const browser_entry_t *eb = b;
    if (ea->dir != eb->dir) {
        return ea->dir ? -1 : 1;
    }
    return strcasecmp(ea->name, eb->name);
}

static bool book_ext(const char *name)
{
    const char *dot = strrchr(name, '.');
    if (!dot) {
        return false;
    }
    dot++;
    return strcasecmp(dot, "txt") == 0 || strcasecmp(dot, "md") == 0 ||
           strcasecmp(dot, "epub") == 0 || strcasecmp(dot, "pdf") == 0 ||
           strcasecmp(dot, "text") == 0;
}

static void browser_scan(const char *path)
{
    s_entry_count = 0;
    if (!s_entries) {
        s_entries = rmalloc(sizeof(browser_entry_t) * BROWSER_MAX_ENTRIES);
        if (!s_entries) {
            return;
        }
    }
    DIR *dir = opendir(path);
    if (!dir) {
        return;
    }
    struct dirent *ent;
    while ((ent = readdir(dir)) != NULL && s_entry_count < BROWSER_MAX_ENTRIES) {
        if (ent->d_name[0] == '.') {
            continue;
        }
        browser_entry_t *e = &s_entries[s_entry_count];
        memset(e, 0, sizeof(*e));
        if (strlen(path) + strlen(ent->d_name) + 2 >= sizeof(e->path)) {
            continue;
        }
        size_t plen = strlen(path);
        size_t nlen = strlen(ent->d_name);
        if (strcmp(path, "/") == 0) {
            if (nlen + 2 > sizeof(e->path)) {
                continue;
            }
            e->path[0] = '/';
            memcpy(e->path + 1, ent->d_name, nlen + 1);
        } else {
            if (plen + 1 + nlen + 1 > sizeof(e->path)) {
                continue;
            }
            memcpy(e->path, path, plen);
            e->path[plen] = '/';
            memcpy(e->path + plen + 1, ent->d_name, nlen + 1);
        }
        if (nlen >= sizeof(e->name)) {
            continue;
        }
        memcpy(e->name, ent->d_name, nlen + 1);
        struct stat st;
        bool is_dir = false;
        if (stat(e->path, &st) == 0) {
            is_dir = S_ISDIR(st.st_mode);
            e->size = (size_t)st.st_size;
        }
        e->dir = is_dir;
        if (!is_dir && !book_ext(e->name)) {
            continue;
        }
        s_entry_count++;
    }
    closedir(dir);
    qsort(s_entries, s_entry_count, sizeof(browser_entry_t), entry_cmp);
}

static void browser_row_cb(lv_event_t *e)
{
    int idx = (int)(intptr_t)lv_event_get_user_data(e) - 1;
    if (idx < 0 || idx >= s_entry_count) {
        return;
    }
    browser_entry_t *entry = &s_entries[idx];
    if (entry->dir) {
        ui_show_browser(entry->path);
    } else {
        ui_open_book(entry->path);
    }
}

static void browser_home_cb(lv_event_t *e)
{
    (void)e;
    ui_show_home();
}

static void browser_up_cb(lv_event_t *e)
{
    (void)e;
    if (strcmp(ui_browser_path, "/sdcard") == 0 || strcmp(ui_browser_path, "/") == 0) {
        ui_toast("已经在顶层目录");
        return;
    }
    char up[512];
    snprintf(up, sizeof(up), "%s", ui_browser_path);
    char *slash = strrchr(up, '/');
    if (slash && slash != up) {
        *slash = '\0';
    } else {
        snprintf(up, sizeof(up), "/sdcard");
    }
    ui_show_browser(up);
}

static void browser_refresh_cb(lv_event_t *e)
{
    (void)e;
    ui_show_browser(ui_browser_path);
}

static void format_size(size_t size, char *buf, size_t len)
{
    if (size >= 1024 * 1024) {
        snprintf(buf, len, "%.1f MB", (double)size / (1024.0 * 1024.0));
    } else if (size >= 1024) {
        snprintf(buf, len, "%u KB", (unsigned)(size / 1024));
    } else {
        snprintf(buf, len, "%u B", (unsigned)size);
    }
}

static void browse_build_list(lv_obj_t *list)
{
    const ui_theme_t *th = ui_theme();
    if (s_entry_count == 0) {
        lv_obj_t *empty = ui_label(list, "此文件夹没有可阅读的书籍", th->muted);
        lv_obj_set_width(empty, 1160);
        lv_obj_set_style_text_align(empty, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_pos(empty, 0, 180);
        return;
    }
    for (int i = 0; i < s_entry_count; i++) {
        browser_entry_t *entry = &s_entries[i];
        lv_obj_t *row = lv_button_create(list);
        lv_obj_set_size(row, 1184, 64);
        lv_obj_set_style_bg_color(row, i % 2 ? th->panel : th->panel2, 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(row, 0, 0);
        lv_obj_set_style_radius(row, 8, 0);
        lv_obj_set_style_pad_all(row, 0, 0);
        lv_obj_set_style_pad_hor(row, 18, 0);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

        const char *tag_text = entry->dir ? "目录"
                               : strcasecmp(text_path_ext(entry->name), ".epub") == 0
                                   ? "EPUB"
                               : strcasecmp(text_path_ext(entry->name), ".pdf") == 0
                                   ? "PDF"
                                   : "TXT";
        lv_obj_t *tag = lv_label_create(row);
        lv_label_set_text(tag, tag_text);
        lv_obj_set_style_text_font(tag, ui_font(), 0);
        lv_obj_set_style_text_color(tag, entry->dir ? th->muted : th->accent, 0);
        lv_obj_set_width(tag, 72);

        lv_obj_t *name = lv_label_create(row);
        lv_label_set_text(name, entry->name);
        lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
        lv_obj_set_width(name, 900);
        lv_obj_set_style_text_font(name, ui_font(), 0);
        lv_obj_set_style_text_color(name, th->fg, 0);
        lv_obj_set_flex_grow(name, 1);

        lv_obj_t *size = lv_label_create(row);
        char sbuf[32];
        if (entry->dir) {
            strcpy(sbuf, ">");
        } else {
            format_size(entry->size, sbuf, sizeof(sbuf));
        }
        lv_label_set_text(size, sbuf);
        lv_obj_set_style_text_font(size, ui_font(), 0);
        lv_obj_set_style_text_color(size, th->muted, 0);
        lv_obj_set_width(size, 110);
        lv_obj_set_style_text_align(size, LV_TEXT_ALIGN_RIGHT, 0);

        lv_obj_add_event_cb(row, browser_row_cb, LV_EVENT_CLICKED,
                            (void *)(intptr_t)(i + 1));
    }
}

void ui_show_browser(const char *path)
{
    if (path && path[0]) {
        snprintf(ui_browser_path, sizeof(ui_browser_path), "%s", path);
    }
    const ui_theme_t *th = ui_theme();
    if (!s_sd_ok) {
        lv_obj_t *scr = lv_obj_create(NULL);
        ui_apply_bg(scr);
        lv_obj_t *header = ui_panel(scr, 1280, 96);
        lv_obj_set_style_radius(header, 0, 0);
        lv_obj_set_pos(header, 0, 0);
        lv_obj_t *home = ui_button(header, "主页", 100, 48, browser_home_cb, NULL);
        lv_obj_set_pos(home, 24, 24);
        lv_obj_t *msg = ui_label(scr, "SD 卡未挂载\n请插入 TF 卡后重新启动", th->muted);
        lv_obj_set_width(msg, 900);
        lv_obj_set_style_text_align(msg, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_line_space(msg, 8, 0);
        lv_obj_center(msg);
        ui_replace_screen(scr);
        return;
    }
    lv_obj_t *scr = lv_obj_create(NULL);
    ui_apply_bg(scr);

    lv_obj_t *header = ui_panel(scr, 1280, 96);
    lv_obj_set_style_radius(header, 0, 0);
    lv_obj_set_pos(header, 0, 0);

    lv_obj_t *home = ui_button(header, "主页", 100, 48, browser_home_cb, NULL);
    lv_obj_set_pos(home, 24, 24);
    lv_obj_t *up = ui_button(header, "上一级", 110, 48, browser_up_cb, NULL);
    lv_obj_set_pos(up, 136, 24);
    lv_obj_t *refresh = ui_button(header, "刷新", 90, 48, browser_refresh_cb, NULL);
    lv_obj_set_pos(refresh, 1166, 24);

    lv_obj_t *path_label = lv_label_create(header);
    lv_label_set_text(path_label, ui_browser_path);
    lv_label_set_long_mode(path_label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(path_label, 900);
    lv_obj_set_style_text_font(path_label, ui_font(), 0);
    lv_obj_set_style_text_color(path_label, th->fg, 0);
    lv_obj_set_pos(path_label, 260, 36);

    browser_scan(ui_browser_path);
    lv_obj_t *list = lv_obj_create(scr);
    lv_obj_set_size(list, 1200, 590);
    lv_obj_set_pos(list, 40, 118);
    lv_obj_set_style_bg_opa(list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(list, 0, 0);
    lv_obj_set_style_pad_all(list, 0, 0);
    lv_obj_set_style_pad_row(list, 6, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_AUTO);
    browse_build_list(list);

    ui_replace_screen(scr);
}

/* ------------------------------------------------------------- open ------ */

void ui_open_book(const char *path)
{
    if (!path) {
        return;
    }
    lv_obj_t *loading = ui_loading("正在打开书籍…");
    doc_t *doc = NULL;
    esp_err_t err = doc_open(path, &doc);
    if (err != ESP_OK || !doc) {
        ui_loading_done(loading);
        char msg[128];
        if (err == ESP_ERR_NOT_SUPPORTED) {
            snprintf(msg, sizeof(msg), "暂不支持此格式或 PDF 已加密");
        } else if (err == ESP_ERR_NOT_FOUND) {
            snprintf(msg, sizeof(msg), "文件不存在或无法读取");
        } else if (err == ESP_ERR_NO_MEM) {
            snprintf(msg, sizeof(msg), "内存不足，无法打开");
        } else {
            snprintf(msg, sizeof(msg), "打开失败：%s", esp_err_to_name(err));
        }
        ui_toast(msg);
        return;
    }
    ui_reader_create(doc, path);
}

/* ------------------------------------------------------------- settings -- */

static void settings_close_cb(lv_event_t *e)
{
    lv_obj_t *modal = (lv_obj_t *)lv_event_get_user_data(e);
    if (modal) {
        lv_obj_del(modal);
    }
    if (modal == s_settings_modal) {
        s_settings_modal = NULL;
    }
}

static void theme_btn_cb(lv_event_t *e)
{
    int theme = (int)(intptr_t)lv_event_get_user_data(e);
    library_set_theme(theme);
    if (s_settings_modal) {
        lv_obj_del(s_settings_modal);
        s_settings_modal = NULL;
    }
    if (ui_reader_active()) {
        ui_reader_apply_theme();
    } else {
        ui_show_home();
    }
}

static void brightness_cb(lv_event_t *e)
{
    lv_obj_t *slider = lv_event_get_target(e);
    int v = (int)lv_slider_get_value(slider);
    lv_obj_t *label = (lv_obj_t *)lv_event_get_user_data(e);
    char buf[32];
    snprintf(buf, sizeof(buf), "%d%%", v);
    lv_label_set_text(label, buf);
    bsp_display_brightness_set(v);
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) {
        library_set_brightness(v);
    }
}

static void clear_recent_cb(lv_event_t *e)
{
    (void)e;
    library_clear_recent();
    ui_toast("阅读记录已清除");
}

void ui_show_settings(void)
{
    const ui_theme_t *th = ui_theme();
    lv_obj_t *modal = lv_obj_create(lv_screen_active());
    lv_obj_set_size(modal, LV_PCT(100), LV_PCT(100));
    lv_obj_set_pos(modal, 0, 0);
    lv_obj_set_style_bg_color(modal, color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(modal, LV_OPA_60, 0);
    lv_obj_set_style_border_width(modal, 0, 0);
    lv_obj_set_style_radius(modal, 0, 0);
    lv_obj_clear_flag(modal, LV_OBJ_FLAG_SCROLLABLE);
    s_settings_modal = modal;

    lv_obj_t *p = ui_panel(modal, 800, 560);
    lv_obj_center(p);

    lv_obj_t *title = lv_label_create(p);
    lv_label_set_text(title, "设置");
    lv_obj_set_style_text_font(title, book_font_body(), 0);
    lv_obj_set_style_text_color(title, th->fg, 0);
    lv_obj_set_pos(title, 32, 24);

    lv_obj_t *tl = ui_label(p, "阅读主题", th->muted);
    lv_obj_set_pos(tl, 32, 86);
    const char *names[3] = {"日间", "护眼", "夜间"};
    int current = library_get_theme();
    for (int i = 0; i < 3; i++) {
        lv_obj_t *b = ui_button(p, names[i], 220, 60, theme_btn_cb, (void *)(intptr_t)i);
        lv_obj_set_pos(b, 32 + i * 244, 122);
        if (i == current) {
            lv_obj_set_style_bg_color(b, th->accent, 0);
            lv_obj_set_style_text_color(lv_obj_get_child(b, 0), color_hex(0xFFFFFF), 0);
        }
    }

    lv_obj_t *bl = ui_label(p, "屏幕亮度", th->muted);
    lv_obj_set_pos(bl, 32, 216);
    lv_obj_t *value = ui_label(p, "80%", th->fg);
    lv_obj_set_pos(value, 700, 216);
    lv_obj_t *slider = lv_slider_create(p);
    lv_obj_set_size(slider, 736, 16);
    lv_obj_set_pos(slider, 32, 256);
    lv_slider_set_range(slider, 5, 100);
    lv_slider_set_value(slider, library_get_brightness(), LV_ANIM_OFF);
    char bbuf[16];
    snprintf(bbuf, sizeof(bbuf), "%d%%", library_get_brightness());
    lv_label_set_text(value, bbuf);
    lv_obj_set_style_bg_color(slider, th->panel2, LV_PART_MAIN);
    lv_obj_set_style_bg_color(slider, th->accent, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(slider, th->accent, LV_PART_KNOB);
    lv_obj_add_event_cb(slider, brightness_cb, LV_EVENT_VALUE_CHANGED, value);
    lv_obj_add_event_cb(slider, brightness_cb, LV_EVENT_RELEASED, value);

    lv_obj_t *clear = ui_button(p, "清除阅读记录", 260, 56, clear_recent_cb, NULL);
    lv_obj_set_pos(clear, 32, 320);
    lv_obj_t *about = ui_button(p, "关于", 180, 56, home_about_cb, NULL);
    lv_obj_set_pos(about, 312, 320);

    lv_obj_t *close = ui_button(p, "关闭", 736, 64, settings_close_cb, modal);
    lv_obj_set_pos(close, 32, 444);
    lv_obj_set_style_bg_color(close, th->accent, 0);
    lv_obj_set_style_text_color(lv_obj_get_child(close, 0), color_hex(0xFFFFFF), 0);
}

void ui_show_about(void)
{
    const ui_theme_t *th = ui_theme();
    lv_obj_t *modal = lv_obj_create(lv_screen_active());
    lv_obj_set_size(modal, LV_PCT(100), LV_PCT(100));
    lv_obj_set_style_bg_color(modal, color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(modal, LV_OPA_60, 0);
    lv_obj_set_style_border_width(modal, 0, 0);
    lv_obj_set_style_radius(modal, 0, 0);
    lv_obj_clear_flag(modal, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *p = ui_panel(modal, 760, 560);
    lv_obj_center(p);
    lv_obj_t *title = lv_label_create(p);
    lv_label_set_text(title, "Tab5 阅读器");
    lv_obj_set_style_text_font(title, book_font_body(), 0);
    lv_obj_set_style_text_color(title, th->fg, 0);
    lv_obj_set_pos(title, 32, 28);

    lv_obj_t *body = lv_label_create(p);
    lv_label_set_text(body,
                      "面向 M5Stack Tab5（ESP32-P4）的本地阅读器。\n\n"
                      "支持格式：TXT、EPUB、PDF（文本模式）\n"
                      "存储位置：SD 卡 /sdcard\n"
                      "操作：点击屏幕左右两侧翻页，中间区域显示/隐藏工具栏。\n\n"
                      "EPUB 保留章节结构；PDF 逐页提取文字并重新排版，\n"
                      "扫描版 PDF 没有文字层时无法显示内容。");
    lv_obj_set_width(body, 696);
    lv_obj_set_style_text_font(body, ui_font(), 0);
    lv_obj_set_style_text_color(body, th->fg, 0);
    lv_obj_set_style_text_line_space(body, 6, 0);
    lv_obj_set_pos(body, 32, 86);

    lv_obj_t *close = ui_button(p, "关闭", 696, 60, settings_close_cb, modal);
    lv_obj_set_pos(close, 32, 480);
    lv_obj_set_style_bg_color(close, th->accent, 0);
    lv_obj_set_style_text_color(lv_obj_get_child(close, 0), color_hex(0xFFFFFF), 0);
}

/* ------------------------------------------------------------- init ------ */

void ui_init(bool sd_ok)
{
    s_sd_ok = sd_ok;
    library_init();
    int brightness = library_get_brightness();
    bsp_display_brightness_set(brightness);
    ESP_LOGI(TAG, "ui init: sd=%d theme=%d brightness=%d", sd_ok, library_get_theme(), brightness);
    ui_show_home();
}
