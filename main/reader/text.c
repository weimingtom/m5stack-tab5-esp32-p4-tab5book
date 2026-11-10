#include "text.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "rmem.h"
#include "gbk_table.h"

static const char *TAG = "text";

#define LINE_INITIAL_CAP 1024
#define LINE_MAX_CAP     400000

/* ------------------------------------------------------------- UTF-8 ----- */

static bool utf8_valid(const uint8_t *s, size_t len)
{
    size_t i = 0;
    while (i < len) {
        uint8_t c = s[i];
        if (c < 0x80) {
            i++;
            continue;
        }
        int n;
        uint32_t cp;
        if ((c & 0xE0) == 0xC0) {
            n = 1;
            cp = c & 0x1F;
        } else if ((c & 0xF0) == 0xE0) {
            n = 2;
            cp = c & 0x0F;
        } else if ((c & 0xF8) == 0xF0) {
            n = 3;
            cp = c & 0x07;
        } else {
            return false;
        }
        if (i + n >= len) {
            return false;
        }
        for (int k = 1; k <= n; k++) {
            uint8_t cc = s[i + k];
            if ((cc & 0xC0) != 0x80) {
                return false;
            }
            cp = (cp << 6) | (cc & 0x3F);
        }
        /* Reject overlong encodings, surrogates and out-of-range values. */
        if ((n == 1 && cp < 0x80) || (n == 2 && cp < 0x800) || (n == 3 && cp < 0x10000) ||
            cp > 0x10FFFF || (cp >= 0xD800 && cp <= 0xDFFF)) {
            return false;
        }
        i += n + 1;
    }
    return true;
}

static size_t utf8_encode(uint32_t cp, char *out)
{
    if (cp <= 0x7F) {
        out[0] = (char)cp;
        return 1;
    }
    if (cp <= 0x7FF) {
        out[0] = (char)(0xC0 | (cp >> 6));
        out[1] = (char)(0x80 | (cp & 0x3F));
        return 2;
    }
    if (cp <= 0xFFFF) {
        out[0] = (char)(0xE0 | (cp >> 12));
        out[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    out[0] = (char)(0xF0 | (cp >> 18));
    out[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    out[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    out[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

/* Decode one UTF-8 code point.  Invalid input yields U+FFFD and advances one
 * byte, so callers cannot get stuck. */
static uint32_t utf8_next(const char *s, size_t len, size_t *idx)
{
    size_t i = *idx;
    if (i >= len) {
        return 0;
    }
    uint8_t c = (uint8_t)s[i];
    if (c < 0x80) {
        *idx = i + 1;
        return c;
    }
    int n;
    uint32_t cp;
    if ((c & 0xE0) == 0xC0) {
        n = 1;
        cp = c & 0x1F;
    } else if ((c & 0xF0) == 0xE0) {
        n = 2;
        cp = c & 0x0F;
    } else if ((c & 0xF8) == 0xF0) {
        n = 3;
        cp = c & 0x07;
    } else {
        *idx = i + 1;
        return 0xFFFD;
    }
    if (i + n >= len) {
        *idx = i + 1;
        return 0xFFFD;
    }
    for (int k = 1; k <= n; k++) {
        uint8_t cc = (uint8_t)s[i + k];
        if ((cc & 0xC0) != 0x80) {
            *idx = i + 1;
            return 0xFFFD;
        }
        cp = (cp << 6) | (cc & 0x3F);
    }
    *idx = i + n + 1;
    return cp;
}

/* ------------------------------------------------------------- files ----- */

esp_err_t text_read_file(const char *path, char **out, size_t *out_len, size_t max_bytes)
{
    *out = NULL;
    *out_len = 0;

    FILE *f = fopen(path, "rb");
    if (!f) {
        ESP_LOGE(TAG, "cannot open %s", path);
        return ESP_ERR_NOT_FOUND;
    }
    if (fseek(f, 0, SEEK_END) != 0) {
        fclose(f);
        return ESP_FAIL;
    }
    long size = ftell(f);
    if (size < 0) {
        fclose(f);
        return ESP_FAIL;
    }
    if ((size_t)size > max_bytes) {
        ESP_LOGE(TAG, "%s is too large (%ld bytes)", path, size);
        fclose(f);
        return ESP_ERR_INVALID_SIZE;
    }
    rewind(f);

    char *buf = rmalloc((size_t)size + 1);
    if (!buf) {
        fclose(f);
        return ESP_ERR_NO_MEM;
    }
    size_t got = fread(buf, 1, (size_t)size, f);
    fclose(f);
    if (got != (size_t)size) {
        rfree(buf);
        return ESP_FAIL;
    }
    buf[got] = '\0';
    *out = buf;
    *out_len = got;
    return ESP_OK;
}

static char *gbk_to_utf8(const uint8_t *in, size_t in_len, size_t *out_len)
{
    /* A two-byte GBK character becomes at most three UTF-8 bytes. */
    char *out = rmalloc(in_len * 2 + 1);
    if (!out) {
        return NULL;
    }
    size_t o = 0;
    size_t i = 0;
    while (i < in_len) {
        uint8_t b = in[i];
        if (b < 0x80) {
            out[o++] = (char)b;
            i++;
            continue;
        }
        if (b == 0x80) { /* CP936 maps 0x80 to the Euro sign. */
            o += utf8_encode(0x20AC, out + o);
            i++;
            continue;
        }
        if (b >= 0x81 && b <= 0xFE && i + 1 < in_len) {
            uint8_t t = in[i + 1];
            /* Four-byte GB18030 sequence: lead digit digit.  We do not carry a
             * supplementary-plane table, so emit a replacement character. */
            if (t >= 0x30 && t <= 0x39) {
                o += utf8_encode(0xFFFD, out + o);
                i += (i + 3 < in_len) ? 4 : 2;
                continue;
            }
            if (t >= 0x40 && t <= 0xFE && t != 0x7F) {
                int idx = (b - 0x81) * 190 + (t < 0x7F ? t - 0x40 : t - 0x80 + 63);
                uint16_t u = (idx >= 0 && idx < GBK_TABLE_ENTRIES) ? gbk_to_unicode[idx] : 0;
                o += utf8_encode(u ? u : 0xFFFD, out + o);
                i += 2;
                continue;
            }
        }
        o += utf8_encode(0xFFFD, out + o);
        i++;
    }
    out[o] = '\0';
    *out_len = o;
    return out;
}

static char *utf16_to_utf8(const uint8_t *in, size_t in_len, bool little, size_t *out_len)
{
    /* Two UTF-16 bytes become at most three UTF-8 bytes (or four for a pair). */
    char *out = rmalloc(in_len * 2 + 1);
    if (!out) {
        return NULL;
    }
    size_t o = 0;
    for (size_t i = 0; i + 1 < in_len; i += 2) {
        uint32_t u = little ? (in[i] | (in[i + 1] << 8)) : ((in[i] << 8) | in[i + 1]);
        if (u >= 0xD800 && u <= 0xDBFF && i + 3 < in_len) {
            uint32_t lo = little ? (in[i + 2] | (in[i + 3] << 8)) : ((in[i + 2] << 8) | in[i + 3]);
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                u = 0x10000 + ((u - 0xD800) << 10) + (lo - 0xDC00);
                i += 2;
            } else {
                u = 0xFFFD;
            }
        } else if (u >= 0xD800 && u <= 0xDFFF) {
            u = 0xFFFD;
        }
        o += utf8_encode(u, out + o);
    }
    out[o] = '\0';
    *out_len = o;
    return out;
}

esp_err_t text_to_utf8(const uint8_t *in, size_t in_len, char **out, size_t *out_len)
{
    *out = NULL;
    *out_len = 0;

    if (in_len >= 3 && in[0] == 0xEF && in[1] == 0xBB && in[2] == 0xBF) {
        in += 3;
        in_len -= 3;
    }

    if (in_len >= 2 && in[0] == 0xFF && in[1] == 0xFE) {
        *out = utf16_to_utf8(in + 2, in_len - 2, true, out_len);
        return *out ? ESP_OK : ESP_ERR_NO_MEM;
    }
    if (in_len >= 2 && in[0] == 0xFE && in[1] == 0xFF) {
        *out = utf16_to_utf8(in + 2, in_len - 2, false, out_len);
        return *out ? ESP_OK : ESP_ERR_NO_MEM;
    }

    if (utf8_valid(in, in_len)) {
        char *copy = rmalloc(in_len + 1);
        if (!copy) {
            return ESP_ERR_NO_MEM;
        }
        memcpy(copy, in, in_len);
        copy[in_len] = '\0';
        *out = copy;
        *out_len = in_len;
        return ESP_OK;
    }

    ESP_LOGW(TAG, "input is not valid UTF-8, trying GBK/GB18030");
    *out = gbk_to_utf8(in, in_len, out_len);
    return *out ? ESP_OK : ESP_ERR_NO_MEM;
}

char *text_normalize(char *in, size_t in_len, size_t *out_len)
{
    if (!in) {
        return NULL;
    }
    size_t cap = in_len + in_len / 4 + 64;
    char *out = rmalloc(cap);
    if (!out) {
        rfree(in);
        return NULL;
    }
    size_t o = 0;
    int newlines = 0;
    for (size_t i = 0; i < in_len; i++) {
        unsigned char c = (unsigned char)in[i];
        if (c == '\r') {
            if (i + 1 < in_len && in[i + 1] == '\n') {
                i++;
            }
            c = '\n';
        } else if (c == '\t') {
            if (o + 4 >= cap) {
                cap *= 2;
                char *no = rrealloc(out, cap);
                if (!no) {
                    rfree(out);
                    rfree(in);
                    return NULL;
                }
                out = no;
            }
            for (int k = 0; k < 4; k++) {
                out[o++] = ' ';
            }
            continue;
        } else if (c == '\f') {
            c = '\n';
        } else if (c < 0x20 && c != '\n') {
            continue;
        }
        if (c == '\n') {
            if (newlines >= 2) {
                continue;
            }
            newlines++;
        } else {
            newlines = 0;
        }
        if (o + 1 >= cap) {
            cap *= 2;
            char *no = rrealloc(out, cap);
            if (!no) {
                rfree(out);
                rfree(in);
                return NULL;
            }
            out = no;
        }
        out[o++] = (char)c;
    }
    /* Trim leading blank lines. */
    size_t start = 0;
    while (start < o && out[start] == '\n') {
        start++;
    }
    if (start > 0) {
        memmove(out, out + start, o - start);
        o -= start;
    }
    while (o > 0 && (out[o - 1] == ' ' || out[o - 1] == '\n')) {
        o--;
    }
    out[o] = '\0';
    rfree(in);
    *out_len = o;
    return out;
}

/* ------------------------------------------------------------- paths ----- */

void text_path_stem(const char *path, char *out, size_t out_len)
{
    const char *base = path;
    const char *p = path;
    while (*p) {
        if (*p == '/' || *p == '\\') {
            base = p + 1;
        }
        p++;
    }
    const char *dot = strrchr(base, '.');
    size_t n = dot && dot > base ? (size_t)(dot - base) : strlen(base);
    if (n >= out_len) {
        n = out_len - 1;
    }
    memcpy(out, base, n);
    out[n] = '\0';
    if (out[0] == '\0') {
        snprintf(out, out_len, "未命名");
    }
}

const char *text_path_ext(const char *path)
{
    const char *base = path;
    const char *p = path;
    while (*p) {
        if (*p == '/' || *p == '\\') {
            base = p + 1;
        }
        p++;
    }
    const char *dot = strrchr(base, '.');
    return dot ? dot : "";
}

/* ------------------------------------------------------------- layout ---- */

static bool is_space(uint32_t cp)
{
    return cp == ' ' || cp == '\t' || cp == 0x3000 || cp == 0x00A0;
}

static bool is_cjk(uint32_t cp)
{
    return (cp >= 0x2E80 && cp <= 0x9FFF) || (cp >= 0xF900 && cp <= 0xFAFF) ||
           (cp >= 0xFF00 && cp <= 0xFFEF) || (cp >= 0x3000 && cp <= 0x303F);
}

static bool is_closing(uint32_t cp)
{
    switch (cp) {
    case ',':
    case '.':
    case '!':
    case '?':
    case ';':
    case ':':
    case 0x3001: /* 、 */
    case 0x3002: /* 。 */
    case 0xFF0C: /* ， */
    case 0xFF0E: /* ． */
    case 0xFF01: /* ！ */
    case 0xFF1F: /* ？ */
    case 0xFF1B: /* ； */
    case 0xFF1A: /* ： */
    case 0xFF09: /* ） */
    case 0x3009: /* 〉 */
    case 0x300B: /* 》 */
    case 0x300D: /* 」 */
    case 0x300F: /* 』 */
    case 0x3011: /* 】 */
    case 0x3015: /* 〕 */
    case 0x3017: /* 〗 */
    case 0x2019: /* ’ */
    case 0x201D: /* ” */
    case 0x2026: /* … */
    case 0x2014: /* — */
        return true;
    default:
        return false;
    }
}

static bool is_opening(uint32_t cp)
{
    switch (cp) {
    case '(':
    case '[':
    case '{':
    case 0xFF08: /* （ */
    case 0x3008: /* 〈 */
    case 0x300A: /* 《 */
    case 0x300C: /* 「 */
    case 0x300E: /* 『 */
    case 0x3010: /* 【 */
    case 0x3014: /* 〔 */
    case 0x3016: /* 〖 */
    case 0x2018: /* ‘ */
    case 0x201C: /* “ */
        return true;
    default:
        return false;
    }
}

static bool can_break_after(uint32_t cp)
{
    return is_space(cp) || (is_cjk(cp) && !is_opening(cp));
}

static void layout_add_line(text_layout_t *layout, int *cap, size_t start, size_t end)
{
    if (layout->line_count >= *cap) {
        int newcap = *cap * 2;
        if (newcap > LINE_MAX_CAP) {
            newcap = LINE_MAX_CAP;
        }
        if (newcap <= *cap) {
            return;
        }
        text_line_t *nl = rrealloc(layout->lines, (size_t)newcap * sizeof(text_line_t));
        if (!nl) {
            return;
        }
        layout->lines = nl;
        *cap = newcap;
    }
    while (end > start && layout->text[end - 1] == ' ') {
        end--;
    }
    layout->lines[layout->line_count].start = (uint32_t)start;
    layout->lines[layout->line_count].end = (uint32_t)end;
    layout->line_count++;
}

esp_err_t text_layout_build(char *text, size_t len, int width_px, int lines_per_page,
                            const lv_font_t *font, text_layout_t *out)
{
    memset(out, 0, sizeof(*out));
    out->text = text;
    out->len = len;
    out->lines_per_page = lines_per_page > 0 ? lines_per_page : 1;

    if (!text || len == 0) {
        out->lines = rcalloc(1, sizeof(text_line_t));
        if (!out->lines) {
            return ESP_ERR_NO_MEM;
        }
        out->line_count = 1;
        out->page_count = 1;
        return ESP_OK;
    }

    int cap = LINE_INITIAL_CAP;
    out->lines = rmalloc((size_t)cap * sizeof(text_line_t));
    if (!out->lines) {
        return ESP_ERR_NO_MEM;
    }

    size_t pos = 0;
    while (pos < len) {
        size_t line_start = pos;
        size_t last_break = pos;
        int width = 0;
        bool overhang_used = false;
        bool line_done = false;

        while (pos < len) {
            size_t ch_start = pos;
            uint32_t cp = utf8_next(text, len, &pos);
            if (cp == '\n') {
                layout_add_line(out, &cap, line_start, ch_start);
                line_done = true;
                break;
            }

            uint16_t w = lv_font_get_glyph_width(font, cp, 0);
            if (w == 0) {
                w = (uint16_t)(font->line_height / 3);
            }
            if (width + w > width_px && ch_start > line_start) {
                if (is_closing(cp) && !overhang_used) {
                    /* Hanging punctuation: let one closing mark stick out. */
                    width += w;
                    overhang_used = true;
                    last_break = pos;
                    continue;
                }
                size_t break_pos = last_break > line_start ? last_break : ch_start;
                layout_add_line(out, &cap, line_start, break_pos);
                pos = break_pos;
                while (pos < len && text[pos] == ' ') {
                    pos++;
                }
                line_done = true;
                break;
            }
            width += w;
            if (can_break_after(cp)) {
                last_break = pos;
            }
        }

        if (!line_done) {
            layout_add_line(out, &cap, line_start, len);
        }
        if (pos >= len) {
            break;
        }
    }

    if (out->line_count == 0) {
        layout_add_line(out, &cap, 0, 0);
    }
    out->page_count = (out->line_count + out->lines_per_page - 1) / out->lines_per_page;
    if (out->page_count < 1) {
        out->page_count = 1;
    }
    return ESP_OK;
}

void text_layout_free(text_layout_t *layout)
{
    if (!layout) {
        return;
    }
    rfree(layout->text);
    rfree(layout->lines);
    memset(layout, 0, sizeof(*layout));
}

int text_layout_line_of_page(const text_layout_t *layout, int page)
{
    if (!layout || page < 0) {
        return 0;
    }
    int line = page * layout->lines_per_page;
    return line < layout->line_count ? line : layout->line_count ? layout->line_count - 1 : 0;
}

char *text_layout_page(const text_layout_t *layout, int page, size_t *out_len)
{
    if (out_len) {
        *out_len = 0;
    }
    if (!layout || page < 0 || page >= layout->page_count) {
        return NULL;
    }
    int first = page * layout->lines_per_page;
    int last = first + layout->lines_per_page;
    if (last > layout->line_count) {
        last = layout->line_count;
    }
    size_t total = 0;
    for (int i = first; i < last; i++) {
        total += layout->lines[i].end - layout->lines[i].start + 1;
    }
    char *buf = rmalloc(total + 1);
    if (!buf) {
        return NULL;
    }
    size_t o = 0;
    for (int i = first; i < last; i++) {
        size_t n = layout->lines[i].end - layout->lines[i].start;
        if (n) {
            memcpy(buf + o, layout->text + layout->lines[i].start, n);
            o += n;
        }
        if (i + 1 < last) {
            buf[o++] = '\n';
        }
    }
    buf[o] = '\0';
    if (out_len) {
        *out_len = o;
    }
    return buf;
}
