#include "pdf_internal.h"

#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "rmem.h"
#include "text.h"

static const char *TAG = "pdf_text";

#define MAX_TOKENS 32
#define MAX_RUNS   40000
#define MAX_CONTENT (16u * 1024u * 1024u)

typedef struct {
    double a, b, c, d, e, f;
} mat_t;

static mat_t mat_identity(void)
{
    return (mat_t){1, 0, 0, 1, 0, 0};
}

static mat_t mat_mul(mat_t m1, mat_t m2)
{
    mat_t r;
    r.a = m1.a * m2.a + m1.b * m2.c;
    r.b = m1.a * m2.b + m1.b * m2.d;
    r.c = m1.c * m2.a + m1.d * m2.c;
    r.d = m1.c * m2.b + m1.d * m2.d;
    r.e = m1.e * m2.a + m1.f * m2.c + m2.e;
    r.f = m1.e * m2.b + m1.f * m2.d + m2.f;
    return r;
}

static mat_t mat_translate(double tx, double ty)
{
    mat_t m = mat_identity();
    m.e = tx;
    m.f = ty;
    return m;
}

typedef struct {
    double x, y, w, size;
    char *text;
    int len;
    int cap;
} run_t;

typedef struct {
    run_t *runs;
    int count;
    int cap;
} run_list_t;

typedef struct {
    char name[64];
    pdf_font_t *font;
} font_cache_t;

typedef struct {
    pdf_doc_t *doc;
    pdf_obj_t *resources;
    font_cache_t fonts[32];
    int font_count;

    mat_t ctm;
    mat_t tm;
    mat_t tlm;
    mat_t ctm_stack[64];
    int q_depth;

    pdf_font_t *font;
    double fs;
    double tc;
    double tw;
    double th;
    double tl;
    double rise;

    run_list_t runs;
} gs_t;

/* ------------------------------------------------------------- tokens --- */

enum {
    TOK_NUM = 1,
    TOK_STR,
    TOK_NAME,
    TOK_ARRAY,
    TOK_OP,
};

typedef struct token token_t;

struct token {
    int type;
    double num;
    char *str;
    int str_len;
    char name[64];
    char op[8];
    token_t *items;
    int item_count;
};

static void token_free(token_t *t)
{
    if (!t) {
        return;
    }
    free(t->str);
    for (int i = 0; i < t->item_count; i++) {
        token_free(&t->items[i]);
    }
    free(t->items);
    memset(t, 0, sizeof(*t));
}

static void tokens_reset(token_t *tokens, int *count)
{
    for (int i = 0; i < *count; i++) {
        token_free(&tokens[i]);
    }
    *count = 0;
}

static void push_token(token_t *tokens, int *count, token_t *t)
{
    if (*count >= MAX_TOKENS) {
        token_free(&tokens[0]);
        memmove(&tokens[0], &tokens[1], sizeof(token_t) * (MAX_TOKENS - 1));
        (*count)--;
    }
    tokens[(*count)++] = *t;
    /* Ownership of t->str / t->items moved into the token array.  Keep the
     * scalar fields (notably op) readable by the caller until it resets the
     * token stack. */
}

static bool content_ws(uint8_t c)
{
    return c == 0 || c == 9 || c == 10 || c == 12 || c == 13 || c == 32;
}

static void content_skip(const uint8_t **pp, const uint8_t *end)
{
    const uint8_t *p = *pp;
    while (p < end) {
        if (content_ws(*p)) {
            p++;
        } else if (*p == '%') {
            while (p < end && *p != '\n' && *p != '\r') {
                p++;
            }
        } else {
            break;
        }
    }
    *pp = p;
}

static int hexv(uint8_t c)
{
    if (c >= '0' && c <= '9') {
        return c - '0';
    }
    if (c >= 'a' && c <= 'f') {
        return c - 'a' + 10;
    }
    if (c >= 'A' && c <= 'F') {
        return c - 'A' + 10;
    }
    return -1;
}

static void parse_token_string(const uint8_t **pp, const uint8_t *end, token_t *t)
{
    const uint8_t *p = *pp + 1;
    int cap = (int)(end - p) + 1;
    char *buf = malloc(cap);
    if (!buf) {
        *pp = p;
        return;
    }
    int n = 0;
    int depth = 1;
    while (p < end) {
        uint8_t c = *p++;
        if (c == '\\') {
            if (p >= end) {
                break;
            }
            c = *p++;
            switch (c) {
            case 'n':
                buf[n++] = '\n';
                break;
            case 'r':
                buf[n++] = '\r';
                break;
            case 't':
                buf[n++] = '\t';
                break;
            case 'b':
                buf[n++] = '\b';
                break;
            case 'f':
                buf[n++] = '\f';
                break;
            case '(':
            case ')':
            case '\\':
                buf[n++] = (char)c;
                break;
            case '\n':
                break;
            case '\r':
                if (p < end && *p == '\n') {
                    p++;
                }
                break;
            default:
                if (c >= '0' && c <= '7') {
                    int v = c - '0';
                    for (int k = 0; k < 2 && p < end && *p >= '0' && *p <= '7'; k++) {
                        v = v * 8 + (*p++ - '0');
                    }
                    buf[n++] = (char)v;
                } else {
                    buf[n++] = (char)c;
                }
                break;
            }
        } else if (c == '(') {
            depth++;
            buf[n++] = (char)c;
        } else if (c == ')') {
            depth--;
            if (depth == 0) {
                break;
            }
            buf[n++] = (char)c;
        } else {
            buf[n++] = (char)c;
        }
    }
    t->type = TOK_STR;
    t->str = buf;
    t->str_len = n;
    *pp = p;
}

static void parse_token_hex(const uint8_t **pp, const uint8_t *end, token_t *t)
{
    const uint8_t *p = *pp + 1;
    char *buf = malloc((size_t)(end - p) / 2 + 2);
    if (!buf) {
        *pp = p;
        return;
    }
    int n = 0;
    int hi = -1;
    while (p < end && *p != '>') {
        int v = hexv(*p++);
        if (v < 0) {
            continue;
        }
        if (hi < 0) {
            hi = v;
        } else {
            buf[n++] = (char)((hi << 4) | v);
            hi = -1;
        }
    }
    if (hi >= 0) {
        buf[n++] = (char)(hi << 4);
    }
    if (p < end) {
        p++;
    }
    t->type = TOK_STR;
    t->str = buf;
    t->str_len = n;
    *pp = p;
}

static void parse_token_name(const uint8_t **pp, const uint8_t *end, token_t *t)
{
    const uint8_t *p = *pp + 1;
    int n = 0;
    while (p < end && !content_ws(*p) && *p != '/' && *p != '[' && *p != ']' && *p != '<' &&
           *p != '>' && *p != '(' && *p != ')') {
        if (*p == '#' && p + 2 < end && isxdigit(p[1]) && isxdigit(p[2])) {
            char h[3] = {(char)p[1], (char)p[2], 0};
            if (n < (int)sizeof(t->name) - 1) {
                t->name[n++] = (char)strtol(h, NULL, 16);
            }
            p += 3;
        } else {
            if (n < (int)sizeof(t->name) - 1) {
                t->name[n++] = (char)*p;
            }
            p++;
        }
    }
    t->name[n] = '\0';
    t->type = TOK_NAME;
    *pp = p;
}

static void parse_token_array(const uint8_t **pp, const uint8_t *end, token_t *t, int depth)
{
    const uint8_t *p = *pp + 1;
    t->type = TOK_ARRAY;
    int cap = 8;
    t->items = calloc(cap, sizeof(token_t));
    while (p < end && t->items) {
        content_skip(&p, end);
        if (p >= end) {
            break;
        }
        if (*p == ']') {
            p++;
            break;
        }
        token_t sub;
        memset(&sub, 0, sizeof(sub));
        if (*p == '(') {
            parse_token_string(&p, end, &sub);
        } else if (*p == '<') {
            parse_token_hex(&p, end, &sub);
        } else if (*p == '/') {
            parse_token_name(&p, end, &sub);
        } else if (*p == '[' && depth < 3) {
            parse_token_array(&p, end, &sub, depth + 1);
        } else if (*p == '-' || *p == '+' || *p == '.' || isdigit(*p)) {
            char *ep = NULL;
            sub.type = TOK_NUM;
            sub.num = strtod((const char *)p, &ep);
            if (ep == (char *)p) {
                p++;
                continue;
            }
            p = (const uint8_t *)ep;
        } else {
            p++;
            continue;
        }
        if (t->item_count >= cap) {
            cap *= 2;
            token_t *ni = realloc(t->items, (size_t)cap * sizeof(token_t));
            if (!ni) {
                token_free(&sub);
                break;
            }
            t->items = ni;
        }
        t->items[t->item_count++] = sub;
    }
    *pp = p;
}

static void parse_content(const uint8_t *data, size_t len, gs_t *g);

/* ------------------------------------------------------------- runs ----- */

static run_t *run_new(run_list_t *list, double x, double y, double size)
{
    if (list->count >= MAX_RUNS) {
        return NULL;
    }
    if (list->count >= list->cap) {
        int nc = list->cap ? list->cap * 2 : 128;
        run_t *nr = realloc(list->runs, (size_t)nc * sizeof(run_t));
        if (!nr) {
            return NULL;
        }
        list->runs = nr;
        list->cap = nc;
    }
    run_t *r = &list->runs[list->count++];
    memset(r, 0, sizeof(*r));
    r->x = x;
    r->y = y;
    r->size = size;
    return r;
}

static void run_append(run_t *r, const char *s, int len)
{
    if (!r || len <= 0) {
        return;
    }
    if (r->len + len + 1 > r->cap) {
        int nc = r->cap ? r->cap * 2 : 64;
        while (nc < r->len + len + 1) {
            nc *= 2;
        }
        char *nt = realloc(r->text, (size_t)nc);
        if (!nt) {
            return;
        }
        r->text = nt;
        r->cap = nc;
    }
    memcpy(r->text + r->len, s, len);
    r->len += len;
    r->text[r->len] = '\0';
}

/* ------------------------------------------------------------- text ------ */

static pdf_font_t *lookup_font(gs_t *g, const char *name)
{
    for (int i = 0; i < g->font_count; i++) {
        if (strcmp(g->fonts[i].name, name) == 0) {
            return g->fonts[i].font;
        }
    }
    if (!g->resources) {
        return NULL;
    }
    pdf_obj_t *font_dict = pdf_dict_get(g->doc, g->resources, "Font");
    if (!font_dict) {
        return NULL;
    }
    pdf_obj_t *fd = pdf_dict_get(g->doc, font_dict, name);
    pdf_font_t *font = pdf_font_get(g->doc, fd);
    if (font && g->font_count < (int)(sizeof(g->fonts) / sizeof(g->fonts[0]))) {
        snprintf(g->fonts[g->font_count].name, sizeof(g->fonts[g->font_count].name), "%s", name);
        g->fonts[g->font_count].font = font;
        g->font_count++;
    }
    return font;
}

static void text_position(gs_t *g, double *x, double *y)
{
    *x = g->ctm.a * g->tm.e + g->ctm.c * g->tm.f + g->ctm.e;
    *y = g->ctm.b * g->tm.e + g->ctm.d * g->tm.f + g->ctm.f;
}

static double effective_size(gs_t *g)
{
    double det = fabs(g->ctm.a * g->ctm.d - g->ctm.b * g->ctm.c);
    double scale = sqrt(det);
    if (scale <= 0) {
        scale = 1;
    }
    double s = g->fs * scale;
    if (s < 1) {
        s = 1;
    }
    if (s > 300) {
        s = 300;
    }
    return s;
}

static void show_string(gs_t *g, const char *s, int len)
{
    if (!g->font || len <= 0) {
        return;
    }
    int code_len = pdf_font_code_len(g->font);
    double x, y;
    text_position(g, &x, &y);
    double size = effective_size(g);
    run_t *run = run_new(&g->runs, x, y, size);
    if (!run) {
        return;
    }
    for (int i = 0; i + code_len <= len; i += code_len) {
        uint32_t code;
        if (code_len == 2) {
            code = ((uint8_t)s[i] << 8) | (uint8_t)s[i + 1];
        } else {
            code = (uint8_t)s[i];
        }
        char utf8[8];
        int n = pdf_font_decode(g->font, code, utf8);
        if (n > 0) {
            run_append(run, utf8, n);
        }
        int w = pdf_font_width(g->doc, g->font, code);
        double adv = ((double)w / 1000.0 * g->fs + g->tc +
                      (code == 32 ? g->tw : 0.0)) * g->th;
        run->w += adv;
        if (adv != 0) {
            g->tm = mat_mul(mat_translate(adv, 0), g->tm);
        }
    }
    if (run->len == 0) {
        free(run->text);
        run->text = NULL;
        g->runs.count--;
    }
}

static void tj_advance(gs_t *g, double amount)
{
    double adv = -amount / 1000.0 * g->fs * g->th;
    if (adv != 0) {
        g->tm = mat_mul(mat_translate(adv, 0), g->tm);
    }
}

static int run_cmp_yx(const void *a, const void *b)
{
    const run_t *ra = a;
    const run_t *rb = b;
    if (fabs(ra->y - rb->y) > 1.2) {
        return ra->y < rb->y ? 1 : -1;
    }
    return (ra->x > rb->x) - (ra->x < rb->x);
}

static int run_cmp_x(const void *a, const void *b)
{
    const run_t *ra = *(const run_t *const *)a;
    const run_t *rb = *(const run_t *const *)b;
    return (ra->x > rb->x) - (ra->x < rb->x);
}

static char *runs_to_text(run_list_t *list, size_t *out_len)
{
    if (!list || list->count == 0) {
        return NULL;
    }
    qsort(list->runs, list->count, sizeof(run_t), run_cmp_yx);

    size_t cap = 1024;
    for (int i = 0; i < list->count; i++) {
        cap += (size_t)list->runs[i].len + 4;
    }
    char *out = rmalloc(cap);
    if (!out) {
        return NULL;
    }
    size_t o = 0;

    int i = 0;
    double prev_line_y = 0;
    double prev_line_size = 0;
    bool first_line = true;
    while (i < list->count) {
        double line_y = list->runs[i].y;
        double line_size = list->runs[i].size;
        int j = i + 1;
        while (j < list->count) {
            double tol = (line_size > list->runs[j].size ? line_size : list->runs[j].size) * 0.65;
            if (tol < 2) {
                tol = 2;
            }
            if (fabs(list->runs[j].y - line_y) > tol) {
                break;
            }
            if (list->runs[j].size > line_size) {
                line_size = list->runs[j].size;
            }
            j++;
        }
        int line_count = j - i;
        run_t **line = malloc((size_t)line_count * sizeof(run_t *));
        if (!line) {
            break;
        }
        for (int k = 0; k < line_count; k++) {
            line[k] = &list->runs[i + k];
        }
        qsort(line, line_count, sizeof(run_t *), run_cmp_x);

        if (!first_line) {
            double gap = prev_line_y - line_y;
            int newlines = gap > prev_line_size * 1.7 ? 2 : 1;
            for (int k = 0; k < newlines && o + 1 < cap; k++) {
                out[o++] = '\n';
            }
        }
        first_line = false;

        double prev_end = -1e9;
        const char *prev_text = NULL;
        int prev_len = 0;
        for (int k = 0; k < line_count; k++) {
            run_t *r = line[k];
            if (r->len == 0 || !r->text) {
                continue;
            }
            if (prev_text && prev_len == r->len && memcmp(prev_text, r->text, r->len) == 0 &&
                fabs(prev_end - r->x) < 2.0) {
                continue;
            }
            if (o > 0 && out[o - 1] != '\n' && out[o - 1] != ' ') {
                double gap = r->x - prev_end;
                if (gap > (r->size > 4 ? r->size * 0.18 : 1.5)) {
                    if (o + 1 < cap) {
                        out[o++] = ' ';
                    }
                }
            }
            if (o + (size_t)r->len < cap) {
                memcpy(out + o, r->text, r->len);
                o += r->len;
            }
            prev_end = r->x + r->w;
            prev_text = r->text;
            prev_len = r->len;
        }
        free(line);
        prev_line_y = line_y;
        prev_line_size = line_size;
        i = j;
    }
    out[o] = '\0';
    *out_len = o;
    return out;
}

/* ------------------------------------------------------------- content --- */

static void execute_op(gs_t *g, token_t *tokens, int count, const char *op)
{
    if (strcmp(op, "BT") == 0) {
        g->tm = mat_identity();
        g->tlm = mat_identity();
    } else if (strcmp(op, "Tf") == 0) {
        if (count >= 2 && tokens[count - 2].type == TOK_NAME && tokens[count - 1].type == TOK_NUM) {
            g->font = lookup_font(g, tokens[count - 2].name);
            g->fs = tokens[count - 1].num;
        }
    } else if (strcmp(op, "Tm") == 0) {
        if (count >= 6) {
            token_t *t = &tokens[count - 6];
            bool ok = true;
            for (int i = 0; i < 6; i++) {
                if (t[i].type != TOK_NUM) {
                    ok = false;
                }
            }
            if (ok) {
                g->tm = (mat_t){t[0].num, t[1].num, t[2].num, t[3].num, t[4].num, t[5].num};
                g->tlm = g->tm;
            }
        }
    } else if (strcmp(op, "Td") == 0) {
        if (count >= 2 && tokens[count - 2].type == TOK_NUM && tokens[count - 1].type == TOK_NUM) {
            g->tlm = mat_mul(mat_translate(tokens[count - 2].num, tokens[count - 1].num), g->tlm);
            g->tm = g->tlm;
        }
    } else if (strcmp(op, "TD") == 0) {
        if (count >= 2 && tokens[count - 2].type == TOK_NUM && tokens[count - 1].type == TOK_NUM) {
            g->tl = -tokens[count - 1].num;
            g->tlm = mat_mul(mat_translate(tokens[count - 2].num, tokens[count - 1].num), g->tlm);
            g->tm = g->tlm;
        }
    } else if (strcmp(op, "T*") == 0) {
        g->tlm = mat_mul(mat_translate(0, -g->tl), g->tlm);
        g->tm = g->tlm;
    } else if (strcmp(op, "TL") == 0) {
        if (count >= 1 && tokens[count - 1].type == TOK_NUM) {
            g->tl = tokens[count - 1].num;
        }
    } else if (strcmp(op, "Tc") == 0) {
        if (count >= 1 && tokens[count - 1].type == TOK_NUM) {
            g->tc = tokens[count - 1].num;
        }
    } else if (strcmp(op, "Tw") == 0) {
        if (count >= 1 && tokens[count - 1].type == TOK_NUM) {
            g->tw = tokens[count - 1].num;
        }
    } else if (strcmp(op, "Tz") == 0) {
        if (count >= 1 && tokens[count - 1].type == TOK_NUM) {
            g->th = tokens[count - 1].num / 100.0;
        }
    } else if (strcmp(op, "Ts") == 0) {
        if (count >= 1 && tokens[count - 1].type == TOK_NUM) {
            g->rise = tokens[count - 1].num;
        }
    } else if (strcmp(op, "Tj") == 0) {
        if (count >= 1 && tokens[count - 1].type == TOK_STR) {
            show_string(g, tokens[count - 1].str, tokens[count - 1].str_len);
        }
    } else if (strcmp(op, "TJ") == 0) {
        if (count >= 1 && tokens[count - 1].type == TOK_ARRAY) {
            token_t *arr = &tokens[count - 1];
            for (int i = 0; i < arr->item_count; i++) {
                if (arr->items[i].type == TOK_STR) {
                    show_string(g, arr->items[i].str, arr->items[i].str_len);
                } else if (arr->items[i].type == TOK_NUM) {
                    tj_advance(g, arr->items[i].num);
                }
            }
        }
    } else if (strcmp(op, "'") == 0) {
        g->tlm = mat_mul(mat_translate(0, -g->tl), g->tlm);
        g->tm = g->tlm;
        if (count >= 1 && tokens[count - 1].type == TOK_STR) {
            show_string(g, tokens[count - 1].str, tokens[count - 1].str_len);
        }
    } else if (strcmp(op, "\"") == 0) {
        if (count >= 3 && tokens[count - 3].type == TOK_NUM && tokens[count - 2].type == TOK_NUM &&
            tokens[count - 1].type == TOK_STR) {
            g->tw = tokens[count - 3].num;
            g->tc = tokens[count - 2].num;
            g->tlm = mat_mul(mat_translate(0, -g->tl), g->tlm);
            g->tm = g->tlm;
            show_string(g, tokens[count - 1].str, tokens[count - 1].str_len);
        }
    } else if (strcmp(op, "cm") == 0) {
        if (count >= 6) {
            token_t *t = &tokens[count - 6];
            bool ok = true;
            for (int i = 0; i < 6; i++) {
                if (t[i].type != TOK_NUM) {
                    ok = false;
                }
            }
            if (ok) {
                mat_t m = {t[0].num, t[1].num, t[2].num, t[3].num, t[4].num, t[5].num};
                g->ctm = mat_mul(m, g->ctm);
            }
        }
    } else if (strcmp(op, "q") == 0) {
        if (g->q_depth < 64) {
            g->ctm_stack[g->q_depth++] = g->ctm;
        }
    } else if (strcmp(op, "Q") == 0) {
        if (g->q_depth > 0) {
            g->ctm = g->ctm_stack[--g->q_depth];
        }
    }
}

static void parse_content(const uint8_t *data, size_t len, gs_t *g)
{
    const uint8_t *p = data;
    const uint8_t *end = data + len;
    token_t tokens[MAX_TOKENS];
    memset(tokens, 0, sizeof(tokens));
    int count = 0;

    while (p < end) {
        content_skip(&p, end);
        if (p >= end) {
            break;
        }
        uint8_t c = *p;
        token_t t;
        memset(&t, 0, sizeof(t));
        if (c == '(') {
            parse_token_string(&p, end, &t);
        } else if (c == '<') {
            if (p + 1 < end && p[1] == '<') {
                /* Skip a dictionary such as BDC properties. */
                p += 2;
                int depth = 1;
                while (p < end && depth > 0) {
                    if (*p == '<' && p + 1 < end && p[1] == '<') {
                        depth++;
                        p += 2;
                    } else if (*p == '>' && p + 1 < end && p[1] == '>') {
                        depth--;
                        p += 2;
                    } else {
                        p++;
                    }
                }
                continue;
            }
            parse_token_hex(&p, end, &t);
        } else if (c == '[') {
            parse_token_array(&p, end, &t, 0);
        } else if (c == '/') {
            parse_token_name(&p, end, &t);
        } else if (c == ']' || c == '>' || c == ')' || c == '{' || c == '}') {
            p++;
            continue;
        } else if ((c == '-' || c == '+' || c == '.' || isdigit(c))) {
            char *ep = NULL;
            t.type = TOK_NUM;
            t.num = strtod((const char *)p, &ep);
            if (ep == (char *)p) {
                p++;
                continue;
            }
            p = (const uint8_t *)ep;
        } else if (c == '\'' || c == '"') {
            t.type = TOK_OP;
            t.op[0] = (char)c;
            t.op[1] = '\0';
            p++;
            push_token(tokens, &count, &t);
            execute_op(g, tokens, count, t.op);
            tokens_reset(tokens, &count);
            continue;
        } else {
            const uint8_t *start = p;
            while (p < end && !content_ws(*p) && *p != '/' && *p != '[' && *p != ']' &&
                   *p != '<' && *p != '>' && *p != '(' && *p != ')' && *p != '%') {
                p++;
            }
            size_t n = (size_t)(p - start);
            if (n == 0) {
                p++;
                continue;
            }
            t.type = TOK_OP;
            if (n > sizeof(t.op) - 1) {
                n = sizeof(t.op) - 1;
            }
            memcpy(t.op, start, n);
            t.op[n] = '\0';
            push_token(tokens, &count, &t);
            execute_op(g, tokens, count, t.op);
            tokens_reset(tokens, &count);
            if (strcmp(t.op, "BI") == 0) {
                /* Inline image: skip to the EI operator. */
                while (p < end) {
                    if (content_ws(*p)) {
                        const uint8_t *q = p;
                        while (q < end && content_ws(*q)) {
                            q++;
                        }
                        if (q + 2 <= end && q[0] == 'E' && q[1] == 'I' &&
                            (q + 2 == end || content_ws(q[2]))) {
                            p = q + 2;
                            break;
                        }
                    }
                    p++;
                }
            }
            continue;
        }
        push_token(tokens, &count, &t);
    }
    tokens_reset(tokens, &count);
}

/* ------------------------------------------------------------- page ------ */

static uint8_t *page_content(pdf_doc_t *doc, pdf_page_t *page, size_t *out_len)
{
    *out_len = 0;
    pdf_obj_t *contents = pdf_resolve(doc, page->contents);
    if (!contents) {
        return NULL;
    }
    if (contents->type == PDF_STREAM) {
        size_t n = 0;
        uint8_t *d = pdf_stream_data(doc, contents, &n);
        if (!d) {
            return NULL;
        }
        uint8_t *copy = rmalloc(n + 1);
        if (copy) {
            memcpy(copy, d, n);
            copy[n] = '\0';
            *out_len = n;
        }
        return copy;
    }
    if (contents->type != PDF_ARRAY) {
        return NULL;
    }
    int count = contents->u.array.count;
    size_t total = 0;
    for (int i = 0; i < count; i++) {
        pdf_obj_t *s = pdf_resolve(doc, contents->u.array.items[i]);
        if (!s || s->type != PDF_STREAM) {
            continue;
        }
        size_t n = 0;
        if (pdf_stream_data(doc, s, &n)) {
            total += n + 1;
        }
    }
    if (total == 0 || total > MAX_CONTENT) {
        return NULL;
    }
    uint8_t *buf = rmalloc(total + 1);
    if (!buf) {
        return NULL;
    }
    size_t o = 0;
    for (int i = 0; i < count; i++) {
        pdf_obj_t *s = pdf_resolve(doc, contents->u.array.items[i]);
        if (!s || s->type != PDF_STREAM) {
            continue;
        }
        size_t n = 0;
        uint8_t *d = pdf_stream_data(doc, s, &n);
        if (!d) {
            continue;
        }
        memcpy(buf + o, d, n);
        o += n;
        buf[o++] = '\n';
    }
    buf[o] = '\0';
    *out_len = o;
    return buf;
}

char *pdf_page_text(pdf_doc_t *doc, int page_index, size_t *out_len)
{
    *out_len = 0;
    if (!doc || page_index < 0 || page_index >= doc->page_count) {
        return NULL;
    }
    pdf_page_t *page = &doc->pages[page_index];
    size_t content_len = 0;
    uint8_t *content = page_content(doc, page, &content_len);
    if (!content || content_len == 0) {
        rfree(content);
        const char *empty = "（本页没有可提取的文字，可能是扫描版 PDF）";
        char *copy = rmalloc(strlen(empty) + 1);
        if (copy) {
            strcpy(copy, empty);
            *out_len = strlen(copy);
        }
        return copy;
    }

    gs_t g;
    memset(&g, 0, sizeof(g));
    g.doc = doc;
    g.resources = page->resources;
    g.ctm = mat_identity();
    g.tm = mat_identity();
    g.tlm = mat_identity();
    g.th = 1.0;
    g.fs = 12.0;

    parse_content(content, content_len, &g);
    rfree(content);

    size_t text_len = 0;
    char *text = runs_to_text(&g.runs, &text_len);
    for (int i = 0; i < g.runs.count; i++) {
        free(g.runs.runs[i].text);
    }
    free(g.runs.runs);

    if (!text || text_len == 0) {
        rfree(text);
        const char *empty = "（本页没有可提取的文字，可能是扫描版 PDF）";
        char *copy = rmalloc(strlen(empty) + 1);
        if (copy) {
            strcpy(copy, empty);
            *out_len = strlen(copy);
        }
        return copy;
    }

    /* Normalize whitespace while keeping paragraph breaks. */
    char *norm = text_normalize(text, text_len, out_len);
    if (!norm) {
        return NULL;
    }
    ESP_LOGD(TAG, "page %d: %u bytes of text", page_index + 1, (unsigned)*out_len);
    return norm;
}
