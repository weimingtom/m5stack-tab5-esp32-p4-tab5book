#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ---------------------------------------------------------------- file ---- */

/* Read a whole file into a PSRAM buffer.  Returns an ESP error code. */
esp_err_t text_read_file(const char *path, char **out, size_t *out_len, size_t max_bytes);

/* Convert UTF-8 / UTF-16 (BOM) / GBK / GB18030-2byte input into UTF-8. */
esp_err_t text_to_utf8(const uint8_t *in, size_t in_len, char **out, size_t *out_len);

/* Normalize line endings, tabs and control characters.  Takes ownership of
 * `in` and returns a newly allocated string (the input is released). */
char *text_normalize(char *in, size_t in_len, size_t *out_len);

/* File name without directories, and without the last extension. */
void text_path_stem(const char *path, char *out, size_t out_len);
const char *text_path_ext(const char *path);

/* ---------------------------------------------------------------- layout --- */

typedef struct {
    uint32_t start; /* byte offset into text */
    uint32_t end;   /* byte offset (exclusive), trailing spaces trimmed */
} text_line_t;

typedef struct {
    char *text;      /* owned, normalized UTF-8 */
    size_t len;
    text_line_t *lines;
    int line_count;
    int lines_per_page;
    int page_count;
} text_layout_t;

/* Lay out `text` (ownership transferred) into lines no wider than width_px.
 * `lines_per_page` is the number of lines that fit in the content area. */
esp_err_t text_layout_build(char *text, size_t len, int width_px, int lines_per_page,
                            const lv_font_t *font, text_layout_t *out);

void text_layout_free(text_layout_t *layout);

/* Build the text of one page (lines joined by '\n'), allocated in PSRAM.
 * Returns NULL for an out-of-range page.  The caller frees the result. */
char *text_layout_page(const text_layout_t *layout, int page, size_t *out_len);

/* Map a page to its first line, used when restoring progress. */
int text_layout_line_of_page(const text_layout_t *layout, int page);

#ifdef __cplusplus
}
#endif
