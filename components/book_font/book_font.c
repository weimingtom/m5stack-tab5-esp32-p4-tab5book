#include "book_font.h"

static lv_font_t s_body;
static lv_font_t s_ui;
static bool s_ready;

static void init_fonts(void)
{
    if (s_ready) {
        return;
    }
    s_body = ui_font_cjk24bpp4; //cjk_font_24;
    s_body.line_height = 32;
    s_body.base_line = 10;

    s_ui = ui_font_cjk24bpp4; //cjk_font_24;
    s_ui.line_height = 30;
    s_ui.base_line = 8;

    s_ready = true;
}

const lv_font_t *book_font_body(void)
{
    init_fonts();
    return &s_body;
}

const lv_font_t *book_font_ui(void)
{
    init_fonts();
    return &s_ui;
}
