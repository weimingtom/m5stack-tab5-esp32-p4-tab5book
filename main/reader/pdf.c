#include "pdf_internal.h"

#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>

#include "doc_internal.h"
#include "esp_log.h"
#include "pdf_tables.h"
#include "rmem.h"
#include "text.h"
#include "zlib.h"

static const char *TAG = "pdf";

#define PDF_MAX_FILE (16u * 1024u * 1024u)
#define PDF_MAX_PAGES 10000
#define PDF_MAX_DEPTH 64

/* ------------------------------------------------------------- objects --- */

static pdf_obj_t *obj_new(pdf_type_t type)
{
    /* PDFs can contain thousands of small objects; keep them in PSRAM so the
     * internal SRAM budget stays available for IDF/LVGL. */
    pdf_obj_t *o = rmalloc(sizeof(*o));
    if (o) {
        memset(o, 0, sizeof(*o));
        o->type = type;
    }
    return o;
}

static void obj_free(pdf_obj_t *o)
{
    if (!o) {
        return;
    }
    switch (o->type) {
    case PDF_STRING:
        free(o->u.str.data);
        break;
    case PDF_NAME:
        free(o->u.name.name);
        break;
    case PDF_ARRAY:
        for (int i = 0; i < o->u.array.count; i++) {
            obj_free(o->u.array.items[i]);
        }
        free(o->u.array.items);
        break;
    case PDF_DICT:
        for (int i = 0; i < o->u.dict.count; i++) {
            free(o->u.dict.keys[i]);
            obj_free(o->u.dict.vals[i]);
        }
        free(o->u.dict.keys);
        free(o->u.dict.vals);
        break;
    case PDF_STREAM:
        obj_free(o->u.stream.dict);
        free(o->u.stream.decoded);
        break;
    default:
        break;
    }
    free(o);
}

static bool is_ws(uint8_t c)
{
    return c == 0 || c == 9 || c == 10 || c == 12 || c == 13 || c == 32;
}

static bool is_delim(uint8_t c)
{
    return c == '(' || c == ')' || c == '<' || c == '>' || c == '[' || c == ']' || c == '{' ||
           c == '}' || c == '/' || c == '%';
}

static void skip_ws(const uint8_t **pp, const uint8_t *end)
{
    const uint8_t *p = *pp;
    while (p < end) {
        if (is_ws(*p)) {
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

static bool keyword_at(const uint8_t *p, const uint8_t *end, const char *kw)
{
    size_t n = strlen(kw);
    if ((size_t)(end - p) < n || memcmp(p, kw, n) != 0) {
        return false;
    }
    if (p + n < end && !is_ws(p[n]) && !is_delim(p[n])) {
        return false;
    }
    return true;
}

static pdf_obj_t *parse_value(pdf_doc_t *doc, const uint8_t **pp, const uint8_t *end, int depth);

static pdf_obj_t *parse_name(const uint8_t **pp, const uint8_t *end)
{
    const uint8_t *p = *pp + 1;
    const uint8_t *start = p;
    while (p < end && !is_ws(*p) && !is_delim(*p)) {
        p++;
    }
    pdf_obj_t *o = obj_new(PDF_NAME);
    if (!o) {
        return NULL;
    }
    size_t cap = (size_t)(p - start) + 1;
    char *name = malloc(cap);
    if (!name) {
        obj_free(o);
        return NULL;
    }
    size_t n = 0;
    for (const uint8_t *q = start; q < p; q++) {
        if (*q == '#' && q + 2 < p && isxdigit(q[1]) && isxdigit(q[2])) {
            char hex[3] = {(char)q[1], (char)q[2], 0};
            name[n++] = (char)strtol(hex, NULL, 16);
            q += 2;
        } else {
            name[n++] = (char)*q;
        }
    }
    name[n] = '\0';
    o->u.name.name = name;
    *pp = p;
    return o;
}

static pdf_obj_t *parse_literal_string(const uint8_t **pp, const uint8_t *end)
{
    const uint8_t *p = *pp + 1;
    pdf_obj_t *o = obj_new(PDF_STRING);
    if (!o) {
        return NULL;
    }
    uint8_t *buf = malloc((size_t)(end - p) + 1);
    if (!buf) {
        obj_free(o);
        return NULL;
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
                buf[n++] = c;
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
                    int val = c - '0';
                    for (int k = 0; k < 2 && p < end && *p >= '0' && *p <= '7'; k++) {
                        val = val * 8 + (*p++ - '0');
                    }
                    buf[n++] = (uint8_t)val;
                } else {
                    buf[n++] = c;
                }
                break;
            }
        } else if (c == '(') {
            depth++;
            buf[n++] = c;
        } else if (c == ')') {
            depth--;
            if (depth == 0) {
                break;
            }
            buf[n++] = c;
        } else {
            buf[n++] = c;
        }
    }
    o->u.str.data = buf;
    o->u.str.len = n;
    *pp = p;
    return o;
}

static int hexval(uint8_t c)
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

static pdf_obj_t *parse_hex_string(const uint8_t **pp, const uint8_t *end)
{
    const uint8_t *p = *pp + 1;
    pdf_obj_t *o = obj_new(PDF_STRING);
    if (!o) {
        return NULL;
    }
    uint8_t *buf = malloc((size_t)(end - p) / 2 + 2);
    if (!buf) {
        obj_free(o);
        return NULL;
    }
    int n = 0;
    int hi = -1;
    while (p < end && *p != '>') {
        int v = hexval(*p++);
        if (v < 0) {
            continue;
        }
        if (hi < 0) {
            hi = v;
        } else {
            buf[n++] = (uint8_t)((hi << 4) | v);
            hi = -1;
        }
    }
    if (hi >= 0) {
        buf[n++] = (uint8_t)(hi << 4);
    }
    if (p < end) {
        p++;
    }
    o->u.str.data = buf;
    o->u.str.len = n;
    o->u.str.hex = true;
    *pp = p;
    return o;
}

static pdf_obj_t *parse_number(const uint8_t **pp, const uint8_t *end, bool ref_lookahead)
{
    const uint8_t *p = *pp;
    const uint8_t *start = p;
    if (p < end && (*p == '+' || *p == '-')) {
        p++;
    }
    bool real = false;
    while (p < end && (isdigit(*p) || *p == '.')) {
        if (*p == '.') {
            real = true;
        }
        p++;
    }
    if (p == start || (p == start + 1 && (start[0] == '+' || start[0] == '-'))) {
        return NULL;
    }
    char tmp[64];
    size_t n = (size_t)(p - start);
    if (n >= sizeof(tmp)) {
        n = sizeof(tmp) - 1;
    }
    memcpy(tmp, start, n);
    tmp[n] = '\0';
    if (!real && ref_lookahead) {
        const uint8_t *save = p;
        const uint8_t *q = p;
        skip_ws(&q, end);
        const uint8_t *gstart = q;
        while (q < end && isdigit(*q)) {
            q++;
        }
        if (q > gstart) {
            const uint8_t *r = q;
            skip_ws(&r, end);
            if (r < end && *r == 'R') {
                long long num = strtoll(tmp, NULL, 10);
                long long gen = strtoll((const char *)gstart, NULL, 10);
                pdf_obj_t *o = obj_new(PDF_REF);
                if (o) {
                    o->u.ref.num = (int)num;
                    o->u.ref.gen = (int)gen;
                }
                *pp = r + 1;
                return o;
            }
        }
        p = save;
    }
    pdf_obj_t *o = obj_new(real ? PDF_REAL : PDF_INT);
    if (!o) {
        return NULL;
    }
    if (real) {
        o->u.r = strtod(tmp, NULL);
    } else {
        o->u.i = strtoll(tmp, NULL, 10);
    }
    *pp = p;
    return o;
}

static pdf_obj_t *parse_array(pdf_doc_t *doc, const uint8_t **pp, const uint8_t *end, int depth)
{
    const uint8_t *p = *pp + 1;
    pdf_obj_t *o = obj_new(PDF_ARRAY);
    if (!o) {
        return NULL;
    }
    int cap = 8;
    o->u.array.items = calloc(cap, sizeof(pdf_obj_t *));
    if (!o->u.array.items) {
        obj_free(o);
        return NULL;
    }
    while (p < end) {
        skip_ws(&p, end);
        if (p < end && *p == ']') {
            p++;
            break;
        }
        pdf_obj_t *v = parse_value(doc, &p, end, depth + 1);
        if (!v) {
            break;
        }
        if (o->u.array.count >= cap) {
            cap *= 2;
            pdf_obj_t **ni = realloc(o->u.array.items, (size_t)cap * sizeof(pdf_obj_t *));
            if (!ni) {
                obj_free(v);
                break;
            }
            o->u.array.items = ni;
        }
        o->u.array.items[o->u.array.count++] = v;
    }
    *pp = p;
    return o;
}

static void dict_set(pdf_obj_t *dict, char *key, pdf_obj_t *val)
{
    for (int i = 0; i < dict->u.dict.count; i++) {
        if (strcmp(dict->u.dict.keys[i], key) == 0) {
            obj_free(dict->u.dict.vals[i]);
            dict->u.dict.vals[i] = val;
            free(key);
            return;
        }
    }
    int n = dict->u.dict.count;
    char **keys = realloc(dict->u.dict.keys, (size_t)(n + 1) * sizeof(char *));
    if (!keys) {
        free(key);
        obj_free(val);
        return;
    }
    dict->u.dict.keys = keys;
    pdf_obj_t **vals = realloc(dict->u.dict.vals, (size_t)(n + 1) * sizeof(pdf_obj_t *));
    if (!vals) {
        free(key);
        obj_free(val);
        return;
    }
    dict->u.dict.vals = vals;
    dict->u.dict.keys[n] = key;
    dict->u.dict.vals[n] = val;
    dict->u.dict.count = n + 1;
}

static pdf_obj_t *parse_dict(pdf_doc_t *doc, const uint8_t **pp, const uint8_t *end, int depth)
{
    const uint8_t *p = *pp + 2;
    pdf_obj_t *o = obj_new(PDF_DICT);
    if (!o) {
        return NULL;
    }
    while (p < end) {
        skip_ws(&p, end);
        if (p + 1 < end && p[0] == '>' && p[1] == '>') {
            p += 2;
            break;
        }
        if (p >= end || *p != '/') {
            /* Malformed dict: skip one byte to avoid a loop. */
            if (p < end) {
                p++;
            }
            continue;
        }
        pdf_obj_t *key = parse_name(&p, end);
        if (!key) {
            break;
        }
        pdf_obj_t *val = parse_value(doc, &p, end, depth + 1);
        if (!val) {
            obj_free(key);
            break;
        }
        dict_set(o, key->u.name.name, val);
        key->u.name.name = NULL;
        obj_free(key);
    }

    /* A dict immediately followed by "stream" is a stream object. */
    const uint8_t *q = p;
    skip_ws(&q, end);
    if (keyword_at(q, end, "stream")) {
        q += 6;
        if (q < end && *q == '\r') {
            q++;
        }
        if (q < end && *q == '\n') {
            q++;
        }
        const uint8_t *start = q;
        long len = -1;
        for (int i = 0; i < o->u.dict.count; i++) {
            if (strcmp(o->u.dict.keys[i], "Length") == 0 && o->u.dict.vals[i]->type == PDF_INT) {
                len = (long)o->u.dict.vals[i]->u.i;
                break;
            }
        }
        const uint8_t *stream_end = NULL;
        if (len >= 0 && start + len <= end) {
            stream_end = start + len;
        } else {
            /* Find "endstream" as a fallback. */
            for (const uint8_t *r = start; r + 9 <= end; r++) {
                if (memcmp(r, "endstream", 9) == 0) {
                    stream_end = r;
                    break;
                }
            }
            if (!stream_end) {
                stream_end = end;
            }
        }
        pdf_obj_t *s = obj_new(PDF_STREAM);
        if (!s) {
            obj_free(o);
            return NULL;
        }
        s->u.stream.dict = o;
        s->u.stream.raw = (uint8_t *)start;
        s->u.stream.raw_len = (size_t)(stream_end - start);
        p = stream_end;
        if (p + 9 <= end && memcmp(p, "endstream", 9) == 0) {
            p += 9;
        }
        *pp = p;
        return s;
    }
    *pp = p;
    return o;
}

static pdf_obj_t *parse_value(pdf_doc_t *doc, const uint8_t **pp, const uint8_t *end, int depth)
{
    if (depth > 100) {
        return NULL;
    }
    const uint8_t *p = *pp;
    skip_ws(&p, end);
    if (p >= end) {
        return NULL;
    }
    pdf_obj_t *o = NULL;
    if (*p == '[') {
        o = parse_array(doc, &p, end, depth);
    } else if (*p == '<') {
        if (p + 1 < end && p[1] == '<') {
            o = parse_dict(doc, &p, end, depth);
        } else {
            o = parse_hex_string(&p, end);
        }
    } else if (*p == '(') {
        o = parse_literal_string(&p, end);
    } else if (*p == '/') {
        o = parse_name(&p, end);
    } else if (keyword_at(p, end, "true")) {
        o = obj_new(PDF_BOOL);
        if (o) {
            o->u.b = true;
        }
        p += 4;
    } else if (keyword_at(p, end, "false")) {
        o = obj_new(PDF_BOOL);
        if (o) {
            o->u.b = false;
        }
        p += 5;
    } else if (keyword_at(p, end, "null")) {
        o = obj_new(PDF_NULL);
        p += 4;
    } else {
        o = parse_number(&p, end, true);
        if (!o) {
            /* Unknown token: consume it and return null. */
            while (p < end && !is_ws(*p) && !is_delim(*p)) {
                p++;
            }
            if (p < end && !is_ws(*p)) {
                p++;
            }
            o = obj_new(PDF_NULL);
        }
    }
    *pp = p;
    (void)doc;
    return o;
}

static pdf_obj_t *parse_indirect(pdf_doc_t *doc, size_t offset, int *out_num)
{
    if (offset >= doc->size) {
        return NULL;
    }
    const uint8_t *p = doc->data + offset;
    const uint8_t *end = doc->data + doc->size;
    skip_ws(&p, end);
    char *ep = NULL;
    long num = strtol((const char *)p, &ep, 10);
    if (ep == (char *)p) {
        return NULL;
    }
    p = (const uint8_t *)ep;
    long gen = strtol((const char *)p, &ep, 10);
    if (ep == (char *)p) {
        return NULL;
    }
    p = (const uint8_t *)ep;
    skip_ws(&p, end);
    if (!keyword_at(p, end, "obj")) {
        return NULL;
    }
    p += 3;
    pdf_obj_t *o = parse_value(doc, &p, end, 0);
    if (o && out_num) {
        *out_num = (int)num;
    }
    (void)gen;
    return o;
}

/* ------------------------------------------------------------- xref ------ */

static void xref_grow(pdf_doc_t *doc, int count)
{
    if (count <= doc->xref_count) {
        return;
    }
    int newcap = doc->xref_count ? doc->xref_count : 64;
    while (newcap < count) {
        newcap *= 2;
    }
    pdf_xref_entry_t *ne = realloc(doc->xref, (size_t)newcap * sizeof(pdf_xref_entry_t));
    if (!ne) {
        return;
    }
    for (int i = doc->xref_count; i < newcap; i++) {
        ne[i].type = 3; /* unset */
        ne[i].a = 0;
        ne[i].b = 0;
    }
    doc->xref = ne;
    doc->xref_count = newcap;
    pdf_obj_t **nc = realloc(doc->cache, (size_t)newcap * sizeof(pdf_obj_t *));
    if (nc) {
        for (int i = 0; i < newcap; i++) {
            if (i >= doc->cache_count) {
                nc[i] = NULL;
            }
        }
        doc->cache = nc;
        doc->cache_count = newcap;
    }
}

static void xref_set(pdf_doc_t *doc, int num, int type, uint32_t a, uint32_t b, bool force)
{
    if (num < 0 || num > 500000) {
        return;
    }
    xref_grow(doc, num + 1);
    if (num >= doc->xref_count) {
        return;
    }
    if (!force && doc->xref[num].type != 3) {
        return;
    }
    doc->xref[num].type = (uint8_t)type;
    doc->xref[num].a = a;
    doc->xref[num].b = b;
}

static pdf_obj_t *dict_get_raw(pdf_obj_t *dict, const char *key)
{
    if (!dict) {
        return NULL;
    }
    if (dict->type == PDF_STREAM) {
        dict = dict->u.stream.dict;
    }
    if (!dict || dict->type != PDF_DICT) {
        return NULL;
    }
    for (int i = 0; i < dict->u.dict.count; i++) {
        if (strcmp(dict->u.dict.keys[i], key) == 0) {
            return dict->u.dict.vals[i];
        }
    }
    return NULL;
}

static long raw_int(pdf_obj_t *dict, const char *key, long def)
{
    pdf_obj_t *v = dict_get_raw(dict, key);
    if (v && v->type == PDF_INT) {
        return (long)v->u.i;
    }
    return def;
}

static bool parse_xref_table(pdf_doc_t *doc, size_t offset, pdf_obj_t **out_trailer)
{
    const uint8_t *p = doc->data + offset;
    const uint8_t *end = doc->data + doc->size;
    if (!keyword_at(p, end, "xref")) {
        return false;
    }
    p += 4;
    while (p < end) {
        skip_ws(&p, end);
        if (keyword_at(p, end, "trailer")) {
            p += 7;
            pdf_obj_t *tr = parse_value(doc, &p, end, 0);
            if (tr && tr->type == PDF_DICT) {
                *out_trailer = tr;
                return true;
            }
            obj_free(tr);
            return false;
        }
        char *ep = NULL;
        long start = strtol((const char *)p, &ep, 10);
        if (ep == (char *)p) {
            break;
        }
        p = (const uint8_t *)ep;
        long count = strtol((const char *)p, &ep, 10);
        if (ep == (char *)p) {
            break;
        }
        p = (const uint8_t *)ep;
        for (long i = 0; i < count; i++) {
            skip_ws(&p, end);
            long off = strtol((const char *)p, &ep, 10);
            if (ep == (char *)p) {
                break;
            }
            p = (const uint8_t *)ep;
            long gen = strtol((const char *)p, &ep, 10);
            if (ep == (char *)p) {
                break;
            }
            p = (const uint8_t *)ep;
            skip_ws(&p, end);
            if (p >= end) {
                break;
            }
            char type = (char)*p++;
            if (type == 'n') {
                xref_set(doc, (int)(start + i), 1, (uint32_t)off, (uint32_t)gen, false);
            } else {
                xref_set(doc, (int)(start + i), 0, 0, 0, false);
            }
        }
    }
    return false;
}

static bool parse_xref_stream(pdf_doc_t *doc, size_t offset, pdf_obj_t **out_trailer)
{
    int objnum = 0;
    pdf_obj_t *o = parse_indirect(doc, offset, &objnum);
    if (!o || o->type != PDF_STREAM) {
        obj_free(o);
        return false;
    }
    pdf_obj_t *dict = o->u.stream.dict;
    pdf_obj_t *w = dict_get_raw(dict, "W");
    if (!w || w->type != PDF_ARRAY || w->u.array.count < 3) {
        obj_free(o);
        return false;
    }
    int widths[3];
    for (int i = 0; i < 3; i++) {
        pdf_obj_t *wi = w->u.array.items[i];
        widths[i] = (wi && wi->type == PDF_INT) ? (int)wi->u.i : 0;
    }
    size_t len = 0;
    uint8_t *data = pdf_stream_data(doc, o, &len);
    if (!data) {
        obj_free(o);
        return false;
    }

    pdf_obj_t *index = dict_get_raw(dict, "Index");
    long pairs = 1;
    if (index && index->type == PDF_ARRAY) {
        pairs = index->u.array.count / 2;
    }
    long size = raw_int(dict, "Size", 0);
    size_t pos = 0;
    for (long pi = 0; pi < pairs; pi++) {
        long start = 0;
        long count = size;
        if (index && index->type == PDF_ARRAY && pi * 2 + 1 < index->u.array.count) {
            pdf_obj_t *a = index->u.array.items[pi * 2];
            pdf_obj_t *b = index->u.array.items[pi * 2 + 1];
            start = (a && a->type == PDF_INT) ? (long)a->u.i : 0;
            count = (b && b->type == PDF_INT) ? (long)b->u.i : 0;
        }
        for (long i = 0; i < count; i++) {
            uint64_t fields[3] = {0, 0, 0};
            for (int f = 0; f < 3; f++) {
                for (int b = 0; b < widths[f]; b++) {
                    if (pos < len) {
                        fields[f] = (fields[f] << 8) | data[pos++];
                    }
                }
            }
            int type = widths[0] == 0 ? 1 : (int)fields[0];
            int num = (int)(start + i);
            if (type == 1) {
                xref_set(doc, num, 1, (uint32_t)fields[1], (uint32_t)fields[2], false);
            } else if (type == 2) {
                xref_set(doc, num, 2, (uint32_t)fields[1], (uint32_t)fields[2], false);
            } else {
                xref_set(doc, num, 0, 0, 0, false);
            }
        }
    }
    *out_trailer = dict;
    /* Detach the dict so it survives after the stream wrapper is released;
     * the trailer is owned by pdf->trailer and freed in pdf_free(). */
    o->u.stream.dict = NULL;
    free(o->u.stream.decoded);
    free(o);
    (void)objnum;
    return true;
}

static bool load_xref_at(pdf_doc_t *doc, size_t offset, int depth)
{
    if (depth > PDF_MAX_DEPTH || offset >= doc->size) {
        return false;
    }
    pdf_obj_t *trailer = NULL;
    const uint8_t *p = doc->data + offset;
    const uint8_t *end = doc->data + doc->size;
    skip_ws(&p, end);
    bool ok;
    if (keyword_at(p, end, "xref")) {
        ok = parse_xref_table(doc, (size_t)(p - doc->data), &trailer);
    } else {
        ok = parse_xref_stream(doc, (size_t)(p - doc->data), &trailer);
    }
    if (!ok || !trailer) {
        return false;
    }
    if (!doc->trailer) {
        doc->trailer = trailer;
    }

    /* Hybrid files carry a second xref stream referenced from the trailer. */
    long xref_stm = raw_int(trailer, "XRefStm", -1);
    if (xref_stm > 0 && (size_t)xref_stm < doc->size) {
        load_xref_at(doc, (size_t)xref_stm, depth + 1);
    }
    long prev = raw_int(trailer, "Prev", -1);
    if (prev > 0 && (size_t)prev < doc->size) {
        load_xref_at(doc, (size_t)prev, depth + 1);
    }
    return true;
}

static void scan_objects(pdf_doc_t *doc)
{
    const uint8_t *d = doc->data;
    size_t n = doc->size;
    for (size_t i = 0; i + 3 < n; i++) {
        if (d[i] != 'o' || d[i + 1] != 'b' || d[i + 2] != 'j') {
            continue;
        }
        if (i + 3 < n && !is_ws(d[i + 3]) && !is_delim(d[i + 3])) {
            continue;
        }
        /* Walk backwards over "gen num " before "obj". */
        size_t j = i;
        while (j > 0 && is_ws(d[j - 1])) {
            j--;
        }
        size_t gen_end = j;
        while (j > 0 && isdigit(d[j - 1])) {
            j--;
        }
        if (j == gen_end) {
            continue;
        }
        while (j > 0 && is_ws(d[j - 1])) {
            j--;
        }
        size_t num_end = j;
        while (j > 0 && isdigit(d[j - 1])) {
            j--;
        }
        if (j == num_end) {
            continue;
        }
        char tmp[32];
        size_t len = num_end - j;
        if (len >= sizeof(tmp)) {
            continue;
        }
        memcpy(tmp, d + j, len);
        tmp[len] = '\0';
        int num = atoi(tmp);
        xref_set(doc, num, 1, (uint32_t)j, 0, false);
    }
}

/* ------------------------------------------------------------- stream ---- */

static uint8_t *inflate_all(const uint8_t *in, size_t in_len, size_t *out_len)
{
    size_t cap = in_len * 4 + 1024;
    uint8_t *out = rmalloc(cap);
    if (!out) {
        return NULL;
    }
    z_stream zs;
    memset(&zs, 0, sizeof(zs));
    bool raw = false;
    if (inflateInit(&zs) != Z_OK) {
        if (inflateInit2(&zs, -MAX_WBITS) != Z_OK) {
            rfree(out);
            return NULL;
        }
        raw = true;
    }
    zs.next_in = (Bytef *)in;
    zs.avail_in = (uInt)in_len;
    size_t used = 0;
    int ret = Z_OK;
    while (ret != Z_STREAM_END) {
        if (used == cap) {
            size_t ncap = cap * 2;
            uint8_t *no = rrealloc(out, ncap);
            if (!no) {
                inflateEnd(&zs);
                rfree(out);
                return NULL;
            }
            out = no;
            cap = ncap;
        }
        zs.next_out = out + used;
        zs.avail_out = (uInt)(cap - used);
        ret = inflate(&zs, Z_NO_FLUSH);
        used = cap - zs.avail_out;
        if (ret == Z_STREAM_END) {
            break;
        }
        if (ret != Z_OK) {
            break;
        }
        if (zs.avail_in == 0 && zs.avail_out > 0) {
            break;
        }
    }
    inflateEnd(&zs);
    if (ret != Z_STREAM_END && used == 0) {
        rfree(out);
        return NULL;
    }
    if (used == cap) {
        uint8_t *no = rrealloc(out, cap + 1);
        if (no) {
            out = no;
        }
    }
    out[used] = '\0';
    *out_len = used;
    (void)raw;
    return out;
}

static uint8_t *ascii_hex_decode(const uint8_t *in, size_t len, size_t *out_len)
{
    uint8_t *out = rmalloc(len / 2 + 2);
    if (!out) {
        return NULL;
    }
    size_t n = 0;
    int hi = -1;
    for (size_t i = 0; i < len; i++) {
        uint8_t c = in[i];
        if (c == '>') {
            break;
        }
        int v = hexval(c);
        if (v < 0) {
            continue;
        }
        if (hi < 0) {
            hi = v;
        } else {
            out[n++] = (uint8_t)((hi << 4) | v);
            hi = -1;
        }
    }
    if (hi >= 0) {
        out[n++] = (uint8_t)(hi << 4);
    }
    out[n] = '\0';
    *out_len = n;
    return out;
}

static uint8_t *ascii85_decode(const uint8_t *in, size_t len, size_t *out_len)
{
    uint8_t *out = rmalloc(len * 4 / 5 + 8);
    if (!out) {
        return NULL;
    }
    size_t n = 0;
    uint32_t tuple = 0;
    int count = 0;
    for (size_t i = 0; i < len; i++) {
        uint8_t c = in[i];
        if (is_ws(c)) {
            continue;
        }
        if (c == '~') {
            break;
        }
        if (c == 'z' && count == 0) {
            out[n++] = 0;
            out[n++] = 0;
            out[n++] = 0;
            out[n++] = 0;
            continue;
        }
        if (c < '!' || c > 'u') {
            continue;
        }
        tuple = tuple * 85 + (uint32_t)(c - '!');
        if (++count == 5) {
            out[n++] = (uint8_t)(tuple >> 24);
            out[n++] = (uint8_t)(tuple >> 16);
            out[n++] = (uint8_t)(tuple >> 8);
            out[n++] = (uint8_t)tuple;
            tuple = 0;
            count = 0;
        }
    }
    if (count > 0) {
        for (int i = count; i < 5; i++) {
            tuple = tuple * 85 + 84;
        }
        uint8_t tmp[4] = {(uint8_t)(tuple >> 24), (uint8_t)(tuple >> 16), (uint8_t)(tuple >> 8),
                          (uint8_t)tuple};
        for (int i = 0; i < count - 1; i++) {
            out[n++] = tmp[i];
        }
    }
    out[n] = '\0';
    *out_len = n;
    return out;
}

static uint8_t *run_length_decode(const uint8_t *in, size_t len, size_t *out_len)
{
    uint8_t *out = rmalloc(len * 4 + 8);
    if (!out) {
        return NULL;
    }
    size_t n = 0;
    size_t i = 0;
    while (i < len) {
        int c = in[i++];
        if (c == 128) {
            break;
        }
        if (c < 128) {
            int count = c + 1;
            while (count-- > 0 && i < len) {
                out[n++] = in[i++];
            }
        } else {
            int count = 257 - c;
            if (i < len) {
                uint8_t v = in[i++];
                while (count-- > 0) {
                    out[n++] = v;
                }
            }
        }
    }
    out[n] = '\0';
    *out_len = n;
    return out;
}

static int paeth(int a, int b, int c)
{
    int p = a + b - c;
    int pa = abs(p - a);
    int pb = abs(p - b);
    int pc = abs(p - c);
    if (pa <= pb && pa <= pc) {
        return a;
    }
    if (pb <= pc) {
        return b;
    }
    return c;
}

static uint8_t *apply_predictor(const uint8_t *in, size_t len, pdf_obj_t *parms, size_t *out_len)
{
    int predictor = (int)raw_int(parms, "Predictor", 1);
    int colors = (int)raw_int(parms, "Colors", 1);
    int bpc = (int)raw_int(parms, "BitsPerComponent", 8);
    int columns = (int)raw_int(parms, "Columns", 1);
    if (predictor <= 1) {
        uint8_t *copy = rmalloc(len + 1);
        if (copy) {
            memcpy(copy, in, len);
            copy[len] = '\0';
            *out_len = len;
        }
        return copy;
    }
    if (colors < 1 || colors > 32 || bpc < 1 || bpc > 16 || columns < 1 || columns > 65536) {
        return NULL;
    }
    size_t row_bytes = ((size_t)colors * bpc * columns + 7) / 8;
    if (row_bytes == 0) {
        return NULL;
    }

    if (predictor == 2) {
        uint8_t *out = rmalloc(len + 1);
        if (!out) {
            return NULL;
        }
        memcpy(out, in, len);
        size_t rows = len / row_bytes;
        for (size_t r = 1; r < rows; r++) {
            uint8_t *row = out + r * row_bytes;
            uint8_t *prev = row - row_bytes;
            for (size_t b = 0; b < row_bytes; b++) {
                row[b] = (uint8_t)(row[b] + prev[b]);
            }
        }
        out[len] = '\0';
        *out_len = len;
        return out;
    }

    if (predictor < 10) {
        return NULL;
    }
    size_t rows = len / (row_bytes + 1);
    uint8_t *out = rmalloc(rows * row_bytes + 1);
    if (!out) {
        return NULL;
    }
    for (size_t r = 0; r < rows; r++) {
        const uint8_t *src = in + r * (row_bytes + 1);
        uint8_t filter = src[0];
        const uint8_t *row = src + 1;
        uint8_t *dst = out + r * row_bytes;
        const uint8_t *prev = r > 0 ? out + (r - 1) * row_bytes : NULL;
        for (size_t b = 0; b < row_bytes; b++) {
            int x = row[b];
            int a = b > 0 ? dst[b - 1] : 0;
            int up = prev ? prev[b] : 0;
            int c = (prev && b > 0) ? prev[b - 1] : 0;
            switch (filter) {
            case 0:
                dst[b] = (uint8_t)x;
                break;
            case 1:
                dst[b] = (uint8_t)(x + a);
                break;
            case 2:
                dst[b] = (uint8_t)(x + up);
                break;
            case 3:
                dst[b] = (uint8_t)(x + ((a + up) >> 1));
                break;
            case 4:
                dst[b] = (uint8_t)(x + paeth(a, up, c));
                break;
            default:
                dst[b] = (uint8_t)x;
                break;
            }
        }
    }
    out[rows * row_bytes] = '\0';
    *out_len = rows * row_bytes;
    return out;
}

static pdf_obj_t *decode_parms_at(pdf_obj_t *parms, int index)
{
    if (!parms) {
        return NULL;
    }
    if (parms->type == PDF_DICT) {
        return parms;
    }
    if (parms->type == PDF_ARRAY && index < parms->u.array.count) {
        return parms->u.array.items[index];
    }
    return NULL;
}

uint8_t *pdf_stream_data(pdf_doc_t *doc, pdf_obj_t *stream, size_t *len)
{
    *len = 0;
    if (!stream || stream->type != PDF_STREAM) {
        return NULL;
    }
    if (stream->u.stream.decoded) {
        *len = stream->u.stream.decoded_len;
        return stream->u.stream.decoded;
    }
    if (stream->u.stream.raw_len == 0) {
        stream->u.stream.decoded = rmalloc(1);
        if (stream->u.stream.decoded) {
            stream->u.stream.decoded[0] = '\0';
        }
        return stream->u.stream.decoded;
    }

    uint8_t *data = stream->u.stream.raw;
    size_t data_len = stream->u.stream.raw_len;
    bool owned = false;

    pdf_obj_t *filter = pdf_resolve(doc, dict_get_raw(stream->u.stream.dict, "Filter"));
    pdf_obj_t *parms = pdf_resolve(doc, dict_get_raw(stream->u.stream.dict, "DecodeParms"));
    int count = 0;
    if (filter) {
        count = filter->type == PDF_ARRAY ? filter->u.array.count : 1;
    }
    for (int i = 0; i < count; i++) {
        pdf_obj_t *f = (filter->type == PDF_ARRAY) ? filter->u.array.items[i] : filter;
        if (!f || f->type != PDF_NAME) {
            continue;
        }
        const char *name = f->u.name.name;
        uint8_t *next = NULL;
        size_t next_len = 0;
        if (strcmp(name, "FlateDecode") == 0 || strcmp(name, "Fl") == 0) {
            next = inflate_all(data, data_len, &next_len);
            pdf_obj_t *p = decode_parms_at(parms, i);
            if (next && p) {
                uint8_t *pred = apply_predictor(next, next_len, p, &next_len);
                if (pred) {
                    rfree(next);
                    next = pred;
                }
            }
        } else if (strcmp(name, "ASCIIHexDecode") == 0 || strcmp(name, "AHx") == 0) {
            next = ascii_hex_decode(data, data_len, &next_len);
        } else if (strcmp(name, "ASCII85Decode") == 0 || strcmp(name, "A85") == 0) {
            next = ascii85_decode(data, data_len, &next_len);
        } else if (strcmp(name, "RunLengthDecode") == 0 || strcmp(name, "RL") == 0) {
            next = run_length_decode(data, data_len, &next_len);
        } else {
            /* Image filters (DCT/JPX/CCITT/JBIG2) are kept as-is. */
            next = rmalloc(data_len + 1);
            if (next) {
                memcpy(next, data, data_len);
                next[data_len] = '\0';
                next_len = data_len;
            }
        }
        if (owned) {
            rfree(data);
        }
        if (!next) {
            return NULL;
        }
        data = next;
        data_len = next_len;
        owned = true;
        /* Only the first filter is commonly paired with DecodeParms; keep the
         * same predictor settings for subsequent filters is wrong, so clear. */
        parms = NULL;
    }

    if (!owned) {
        *len = data_len;
        return data;
    }
    stream->u.stream.decoded = data;
    stream->u.stream.decoded_len = data_len;
    *len = data_len;
    return data;
}

/* ------------------------------------------------------------- fonts ----- */

static int hex_bytes_to_code(const uint8_t *b, int n)
{
    uint32_t v = 0;
    for (int i = 0; i < n && i < 4; i++) {
        v = (v << 8) | b[i];
    }
    return (int)v;
}

static int utf16be_to_utf8(const uint8_t *b, int n, char out[8])
{
    int o = 0;
    for (int i = 0; i + 1 < n; i += 2) {
        uint32_t cp = (b[i] << 8) | b[i + 1];
        if (cp >= 0xD800 && cp <= 0xDBFF && i + 3 < n) {
            uint32_t lo = (b[i + 2] << 8) | b[i + 3];
            if (lo >= 0xDC00 && lo <= 0xDFFF) {
                cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00);
                i += 2;
            }
        }
        if (cp <= 0x7F) {
            out[o++] = (char)cp;
        } else if (cp <= 0x7FF) {
            out[o++] = (char)(0xC0 | (cp >> 6));
            out[o++] = (char)(0x80 | (cp & 0x3F));
        } else if (cp <= 0xFFFF) {
            out[o++] = (char)(0xE0 | (cp >> 12));
            out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (cp & 0x3F));
        } else {
            out[o++] = (char)(0xF0 | (cp >> 18));
            out[o++] = (char)(0x80 | ((cp >> 12) & 0x3F));
            out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
            out[o++] = (char)(0x80 | (cp & 0x3F));
        }
        if (o >= 7) {
            break;
        }
    }
    out[o] = '\0';
    return o;
}

static void cmap_add(pdf_cmap_t *cmap, uint32_t code, const uint8_t *dst, int dst_len)
{
    if (cmap->count > 0 && cmap->entries[cmap->count - 1].code == code) {
        return;
    }
    if (cmap->count >= cmap->cap) {
        int nc = cmap->cap ? cmap->cap * 2 : 128;
        pdf_map_entry_t *ne = realloc(cmap->entries, (size_t)nc * sizeof(pdf_map_entry_t));
        if (!ne) {
            return;
        }
        cmap->entries = ne;
        cmap->cap = nc;
    }
    pdf_map_entry_t *e = &cmap->entries[cmap->count++];
    e->code = code;
    e->len = (uint16_t)utf16be_to_utf8(dst, dst_len, e->utf8);
}

static const uint8_t *cmap_hex(const uint8_t *p, const uint8_t *end, uint8_t *out, int *out_len)
{
    while (p < end && *p != '<') {
        p++;
    }
    if (p >= end) {
        return NULL;
    }
    p++;
    int n = 0;
    int hi = -1;
    while (p < end && *p != '>') {
        int v = hexval(*p++);
        if (v < 0) {
            continue;
        }
        if (hi < 0) {
            hi = v;
        } else {
            if (n < 16) {
                out[n++] = (uint8_t)((hi << 4) | v);
            }
            hi = -1;
        }
    }
    if (p < end) {
        p++;
    }
    *out_len = n;
    return p;
}

static int cmap_cmp(const void *a, const void *b)
{
    const pdf_map_entry_t *ea = a;
    const pdf_map_entry_t *eb = b;
    return (ea->code > eb->code) - (ea->code < eb->code);
}

static void parse_tounicode(pdf_cmap_t *cmap, const uint8_t *data, size_t len)
{
    char *buf = rmalloc(len + 1);
    if (!buf) {
        return;
    }
    memcpy(buf, data, len);
    buf[len] = '\0';
    const uint8_t *p = (const uint8_t *)buf;
    const uint8_t *end = p + len;

    while ((p = (const uint8_t *)strstr((const char *)p, "beginbfchar")) != NULL) {
        p += 11;
        while (p < end) {
            const uint8_t *save = p;
            while (p < end && isspace(*p)) {
                p++;
            }
            if ((size_t)(end - p) >= 9 && memcmp(p, "endbfchar", 9) == 0) {
                p += 9;
                break;
            }
            uint8_t src[8], dst[32];
            int src_len = 0, dst_len = 0;
            const uint8_t *q = cmap_hex(p, end, src, &src_len);
            if (!q) {
                p = save + 1;
                continue;
            }
            q = cmap_hex(q, end, dst, &dst_len);
            if (!q) {
                p = save + 1;
                continue;
            }
            cmap_add(cmap, (uint32_t)hex_bytes_to_code(src, src_len), dst, dst_len);
            p = q;
        }
    }

    p = (const uint8_t *)buf;
    while ((p = (const uint8_t *)strstr((const char *)p, "beginbfrange")) != NULL) {
        p += 12;
        while (p < end) {
            const uint8_t *save = p;
            while (p < end && isspace(*p)) {
                p++;
            }
            if ((size_t)(end - p) >= 10 && memcmp(p, "endbfrange", 10) == 0) {
                p += 10;
                break;
            }
            uint8_t lo_b[8], hi_b[8];
            int lo_len = 0, hi_len = 0;
            const uint8_t *q = cmap_hex(p, end, lo_b, &lo_len);
            if (!q) {
                p = save + 1;
                continue;
            }
            q = cmap_hex(q, end, hi_b, &hi_len);
            if (!q) {
                p = save + 1;
                continue;
            }
            uint32_t lo = (uint32_t)hex_bytes_to_code(lo_b, lo_len);
            uint32_t hi = (uint32_t)hex_bytes_to_code(hi_b, hi_len);
            while (q < end && isspace(*q)) {
                q++;
            }
            if (q < end && *q == '[') {
                q++;
                uint32_t code = lo;
                while (q < end && *q != ']') {
                    uint8_t dst[32];
                    int dst_len = 0;
                    const uint8_t *nq = cmap_hex(q, end, dst, &dst_len);
                    if (!nq) {
                        break;
                    }
                    cmap_add(cmap, code, dst, dst_len);
                    code++;
                    q = nq;
                }
                if (q < end && *q == ']') {
                    q++;
                }
            } else {
                uint8_t dst[32];
                int dst_len = 0;
                q = cmap_hex(q, end, dst, &dst_len);
                if (!q) {
                    p = save + 1;
                    continue;
                }
                uint32_t code = lo;
                while (code <= hi && code - lo < 65536) {
                    uint8_t d[32];
                    memcpy(d, dst, dst_len);
                    /* Increment the last UTF-16 code unit. */
                    if (dst_len >= 2) {
                        int k = dst_len - 1;
                        while (k > 0 && d[k] == 0xFF) {
                            d[k] = 0;
                            k--;
                        }
                        d[k]++;
                    }
                    cmap_add(cmap, code, d, dst_len);
                    code++;
                }
            }
            p = q;
        }
    }
    rfree(buf);
    if (cmap->count > 1) {
        qsort(cmap->entries, cmap->count, sizeof(pdf_map_entry_t), cmap_cmp);
    }
}

static const pdf_map_entry_t *cmap_lookup(const pdf_cmap_t *cmap, uint32_t code)
{
    int lo = 0, hi = cmap->count - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (cmap->entries[mid].code == code) {
            return &cmap->entries[mid];
        }
        if (cmap->entries[mid].code < code) {
            lo = mid + 1;
        } else {
            hi = mid - 1;
        }
    }
    return NULL;
}

static int glyph_name_to_unicode(const char *name, int len)
{
    for (int i = 0; i < PDF_GLYPH_NAME_COUNT; i++) {
        if (strncmp(pdf_glyph_names[i].name, name, len) == 0 && pdf_glyph_names[i].name[len] == '\0') {
            return pdf_glyph_names[i].uni;
        }
    }
    if (len > 3 && strncmp(name, "uni", 3) == 0) {
        char tmp[8];
        size_t n = (size_t)len - 3;
        if (n > 6) {
            n = 6;
        }
        memcpy(tmp, name + 3, n);
        tmp[n] = '\0';
        return (int)strtol(tmp, NULL, 16);
    }
    if (len > 1 && name[0] == 'u') {
        char tmp[8];
        size_t n = (size_t)len - 1;
        if (n > 6) {
            n = 6;
        }
        memcpy(tmp, name + 1, n);
        tmp[n] = '\0';
        return (int)strtol(tmp, NULL, 16);
    }
    return 0;
}

static void parse_encoding(pdf_doc_t *doc, pdf_font_t *font, pdf_obj_t *enc)
{
    if (!enc) {
        font->encoding.base = pdf_win_ansi;
        return;
    }
    if (enc->type == PDF_NAME) {
        if (strcmp(enc->u.name.name, "MacRomanEncoding") == 0) {
            font->encoding.base = pdf_mac_roman;
        } else if (strcmp(enc->u.name.name, "StandardEncoding") == 0) {
            font->encoding.base = pdf_standard_encoding;
        } else {
            font->encoding.base = pdf_win_ansi;
        }
        return;
    }
    if (enc->type != PDF_DICT) {
        font->encoding.base = pdf_win_ansi;
        return;
    }
    font->encoding.base = pdf_win_ansi;
    pdf_obj_t *base = dict_get_raw(enc, "BaseEncoding");
    if (base && base->type == PDF_NAME) {
        if (strcmp(base->u.name.name, "MacRomanEncoding") == 0) {
            font->encoding.base = pdf_mac_roman;
        } else if (strcmp(base->u.name.name, "StandardEncoding") == 0) {
            font->encoding.base = pdf_standard_encoding;
        }
    }
    pdf_obj_t *diff = dict_get_raw(enc, "Differences");
    if (diff && diff->type == PDF_ARRAY) {
        int code = 0;
        font->encoding.has_diff = true;
        for (int i = 0; i < diff->u.array.count; i++) {
            pdf_obj_t *v = diff->u.array.items[i];
            if (!v) {
                continue;
            }
            if (v->type == PDF_INT) {
                code = (int)v->u.i;
            } else if (v->type == PDF_NAME) {
                int uni = glyph_name_to_unicode(v->u.name.name, (int)strlen(v->u.name.name));
                if (code >= 0 && code < 256 && uni > 0) {
                    font->encoding.diff[code] = (uint16_t)uni;
                }
                code++;
            }
        }
    }
    (void)doc;
}

static void parse_wide_widths(pdf_doc_t *doc, pdf_font_t *font, pdf_obj_t *w)
{
    if (!w || w->type != PDF_ARRAY) {
        return;
    }
    int cap = 64;
    font->wide = calloc(cap, sizeof(*font->wide));
    if (!font->wide) {
        return;
    }
    int i = 0;
    while (i < w->u.array.count) {
        pdf_obj_t *first = w->u.array.items[i++];
        if (!first || first->type != PDF_INT) {
            continue;
        }
        uint32_t c1 = (uint32_t)first->u.i;
        if (i >= w->u.array.count) {
            break;
        }
        pdf_obj_t *next = w->u.array.items[i++];
        if (next && next->type == PDF_ARRAY) {
            for (int k = 0; k < next->u.array.count; k++) {
                pdf_obj_t *wv = next->u.array.items[k];
                if (!wv || (wv->type != PDF_INT && wv->type != PDF_REAL)) {
                    continue;
                }
                int width = wv->type == PDF_INT ? (int)wv->u.i : (int)wv->u.r;
                if (font->wide_count >= cap) {
                    cap *= 2;
                    void *nw = realloc(font->wide, (size_t)cap * sizeof(*font->wide));
                    if (!nw) {
                        return;
                    }
                    font->wide = nw;
                }
                font->wide[font->wide_count].code = c1 + k;
                font->wide[font->wide_count].width = (uint16_t)width;
                font->wide_count++;
            }
        } else if (next && (next->type == PDF_INT || next->type == PDF_REAL)) {
            uint32_t c2 = (uint32_t)(next->type == PDF_INT ? next->u.i : next->u.r);
            if (i >= w->u.array.count) {
                break;
            }
            pdf_obj_t *wv = w->u.array.items[i++];
            if (!wv || (wv->type != PDF_INT && wv->type != PDF_REAL)) {
                continue;
            }
            int width = wv->type == PDF_INT ? (int)wv->u.i : (int)wv->u.r;
            for (uint32_t c = c1; c <= c2 && c - c1 < 65536; c++) {
                if (font->wide_count >= cap) {
                    cap *= 2;
                    void *nw = realloc(font->wide, (size_t)cap * sizeof(*font->wide));
                    if (!nw) {
                        return;
                    }
                    font->wide = nw;
                }
                font->wide[font->wide_count].code = c;
                font->wide[font->wide_count].width = (uint16_t)width;
                font->wide_count++;
            }
        }
    }
    (void)doc;
}

pdf_font_t *pdf_font_get(pdf_doc_t *doc, pdf_obj_t *font_dict)
{
    font_dict = pdf_resolve(doc, font_dict);
    if (!font_dict || (font_dict->type != PDF_DICT && font_dict->type != PDF_STREAM)) {
        return NULL;
    }
    for (int i = 0; i < doc->font_count; i++) {
        if (doc->fonts[i].dict == font_dict) {
            return &doc->fonts[i];
        }
    }
    if (doc->font_count >= doc->font_cap) {
        int nc = doc->font_cap ? doc->font_cap * 2 : 16;
        pdf_font_t *nf = realloc(doc->fonts, (size_t)nc * sizeof(pdf_font_t));
        if (!nf) {
            return NULL;
        }
        doc->fonts = nf;
        doc->font_cap = nc;
    }
    pdf_font_t *font = &doc->fonts[doc->font_count++];
    memset(font, 0, sizeof(*font));
    font->dict = font_dict;
    font->default_width = 500;
    font->encoding.base = pdf_win_ansi;
    font->first_char = 0;

    pdf_obj_t *dict = font_dict->type == PDF_STREAM ? font_dict->u.stream.dict : font_dict;
    pdf_obj_t *sub = dict_get_raw(dict, "Subtype");
    font->type0 = sub && sub->type == PDF_NAME && strcmp(sub->u.name.name, "Type0") == 0;

    pdf_obj_t *tu = dict_get_raw(dict, "ToUnicode");
    tu = pdf_resolve(doc, tu);
    if (tu && tu->type == PDF_STREAM) {
        size_t len = 0;
        uint8_t *data = pdf_stream_data(doc, tu, &len);
        if (data && len) {
            parse_tounicode(&font->cmap, data, len);
        }
    }

    if (font->type0) {
        font->default_width = (int)raw_int(dict, "DW", 1000);
        pdf_obj_t *df = dict_get_raw(dict, "DescendantFonts");
        df = pdf_resolve(doc, df);
        if (df && df->type == PDF_ARRAY && df->u.array.count > 0) {
            pdf_obj_t *desc = pdf_resolve(doc, df->u.array.items[0]);
            if (desc && desc->type == PDF_DICT) {
                font->default_width = (int)raw_int(desc, "DW", font->default_width);
                parse_wide_widths(doc, font, pdf_resolve(doc, dict_get_raw(desc, "W")));
            }
        }
    } else {
        pdf_obj_t *first = dict_get_raw(dict, "FirstChar");
        if (first && first->type == PDF_INT) {
            font->first_char = (int)first->u.i;
        }
        pdf_obj_t *widths = dict_get_raw(dict, "Widths");
        widths = pdf_resolve(doc, widths);
        if (widths && widths->type == PDF_ARRAY) {
            for (int i = 0; i < widths->u.array.count && i < 256; i++) {
                pdf_obj_t *wv = widths->u.array.items[i];
                if (wv && wv->type == PDF_INT) {
                    font->widths[i] = (uint16_t)wv->u.i;
                } else if (wv && wv->type == PDF_REAL) {
                    font->widths[i] = (uint16_t)wv->u.r;
                }
            }
        }
        parse_encoding(doc, font, dict_get_raw(dict, "Encoding"));
    }
    return font;
}

int pdf_font_code_len(const pdf_font_t *font)
{
    return font && font->type0 ? 2 : 1;
}

int pdf_font_decode(const pdf_font_t *font, uint32_t code, char out[8])
{
    out[0] = '\0';
    if (!font) {
        return 0;
    }
    const pdf_map_entry_t *e = cmap_lookup(&font->cmap, code);
    if (e && e->len) {
        memcpy(out, e->utf8, e->len);
        out[e->len] = '\0';
        return e->len;
    }
    if (font->type0) {
        return 0;
    }
    int uni = 0;
    if (code < 256) {
        if (font->encoding.has_diff && font->encoding.diff[code]) {
            uni = font->encoding.diff[code];
        } else if (font->encoding.base) {
            uni = font->encoding.base[code];
        }
    }
    if (uni <= 0) {
        if (code == 32) {
            out[0] = ' ';
            out[1] = '\0';
            return 1;
        }
        return 0;
    }
    if (uni <= 0x7F) {
        out[0] = (char)uni;
        out[1] = '\0';
        return 1;
    }
    if (uni <= 0x7FF) {
        out[0] = (char)(0xC0 | (uni >> 6));
        out[1] = (char)(0x80 | (uni & 0x3F));
        out[2] = '\0';
        return 2;
    }
    out[0] = (char)(0xE0 | (uni >> 12));
    out[1] = (char)(0x80 | ((uni >> 6) & 0x3F));
    out[2] = (char)(0x80 | (uni & 0x3F));
    out[3] = '\0';
    return 3;
}

int pdf_font_width(pdf_doc_t *doc, const pdf_font_t *font, uint32_t code)
{
    (void)doc;
    if (!font) {
        return 500;
    }
    if (font->type0) {
        int lo = 0, hi = font->wide_count - 1;
        while (lo <= hi) {
            int mid = (lo + hi) / 2;
            if (font->wide[mid].code == code) {
                return font->wide[mid].width;
            }
            if (font->wide[mid].code < code) {
                lo = mid + 1;
            } else {
                hi = mid - 1;
            }
        }
        return font->default_width;
    }
    int idx = (int)code - font->first_char;
    if (idx >= 0 && idx < 256) {
        return font->widths[idx] ? font->widths[idx] : font->default_width;
    }
    return font->default_width;
}

/* ------------------------------------------------------------- objects --- */

pdf_obj_t *pdf_get_obj(pdf_doc_t *doc, int num)
{
    if (num < 0 || num >= doc->xref_count) {
        return NULL;
    }
    if (doc->cache && doc->cache[num]) {
        return doc->cache[num];
    }
    if (!doc->cache) {
        return NULL;
    }
    pdf_xref_entry_t *e = &doc->xref[num];
    if (e->type == 1) {
        int objnum = 0;
        pdf_obj_t *o = parse_indirect(doc, e->a, &objnum);
        doc->cache[num] = o;
        return o;
    }
    if (e->type == 2) {
        /* Compressed object: load and cache its object stream. */
        int stm_num = (int)e->a;
        int index = (int)e->b;
        pdf_objstm_t *stm = NULL;
        for (int i = 0; i < doc->objstm_count; i++) {
            if (doc->objstms[i].num == stm_num) {
                stm = &doc->objstms[i];
                break;
            }
        }
        if (!stm) {
            if (doc->objstm_count >= doc->objstm_cap) {
                int nc = doc->objstm_cap ? doc->objstm_cap * 2 : 16;
                pdf_objstm_t *ns = realloc(doc->objstms, (size_t)nc * sizeof(pdf_objstm_t));
                if (!ns) {
                    return NULL;
                }
                doc->objstms = ns;
                doc->objstm_cap = nc;
            }
            stm = &doc->objstms[doc->objstm_count++];
            memset(stm, 0, sizeof(*stm));
            stm->num = stm_num;
            if (stm_num >= doc->xref_count || doc->xref[stm_num].type != 1) {
                return NULL;
            }
            int tmp = 0;
            pdf_obj_t *so = parse_indirect(doc, doc->xref[stm_num].a, &tmp);
            if (!so || so->type != PDF_STREAM) {
                obj_free(so);
                return NULL;
            }
            size_t len = 0;
            uint8_t *data = pdf_stream_data(doc, so, &len);
            if (!data) {
                obj_free(so);
                return NULL;
            }
            stm->data = data;
            stm->len = len;
            stm->n = (int)raw_int(so->u.stream.dict, "N", 0);
            stm->first = (int)raw_int(so->u.stream.dict, "First", 0);
            stm->objnums = malloc((size_t)stm->n * sizeof(int));
            stm->offsets = malloc((size_t)stm->n * sizeof(int));
            if (stm->objnums && stm->offsets) {
                const uint8_t *p = data;
                for (int i = 0; i < stm->n; i++) {
                    char *ep = NULL;
                    long on = strtol((const char *)p, &ep, 10);
                    if (ep == (char *)p) {
                        break;
                    }
                    p = (const uint8_t *)ep;
                    long off = strtol((const char *)p, &ep, 10);
                    if (ep == (char *)p) {
                        break;
                    }
                    p = (const uint8_t *)ep;
                    stm->objnums[i] = (int)on;
                    stm->offsets[i] = (int)off;
                }
            }
            /* Keep `so` alive only through its stream data; decoded data is
             * borrowed above, so retain the object in the cache. */
            doc->cache[stm_num] = so;
        }
        if (!stm || index < 0 || index >= stm->n || !stm->data) {
            return NULL;
        }
        if (stm->first + stm->offsets[index] < 0 ||
            (size_t)(stm->first + stm->offsets[index]) >= stm->len) {
            return NULL;
        }
        const uint8_t *p = stm->data + stm->first + stm->offsets[index];
        const uint8_t *end = stm->data + stm->len;
        pdf_obj_t *o = parse_value(doc, &p, end, 0);
        doc->cache[num] = o;
        return o;
    }
    return NULL;
}

pdf_obj_t *pdf_resolve(pdf_doc_t *doc, pdf_obj_t *obj)
{
    if (obj && obj->type == PDF_REF) {
        return pdf_get_obj(doc, obj->u.ref.num);
    }
    return obj;
}

static pdf_obj_t *dict_of(pdf_obj_t *obj)
{
    if (!obj) {
        return NULL;
    }
    if (obj->type == PDF_STREAM) {
        return obj->u.stream.dict;
    }
    return obj->type == PDF_DICT ? obj : NULL;
}

pdf_obj_t *pdf_dict_get(pdf_doc_t *doc, pdf_obj_t *dict, const char *key)
{
    dict = dict_of(pdf_resolve(doc, dict));
    if (!dict) {
        return NULL;
    }
    for (int i = 0; i < dict->u.dict.count; i++) {
        if (strcmp(dict->u.dict.keys[i], key) == 0) {
            return pdf_resolve(doc, dict->u.dict.vals[i]);
        }
    }
    return NULL;
}

bool pdf_name_eq(pdf_obj_t *obj, const char *name)
{
    return obj && obj->type == PDF_NAME && strcmp(obj->u.name.name, name) == 0;
}

int pdf_dict_int(pdf_doc_t *doc, pdf_obj_t *dict, const char *key, int def)
{
    pdf_obj_t *v = pdf_dict_get(doc, dict, key);
    if (v && v->type == PDF_INT) {
        return (int)v->u.i;
    }
    return def;
}

double pdf_dict_real(pdf_doc_t *doc, pdf_obj_t *dict, const char *key, double def)
{
    pdf_obj_t *v = pdf_dict_get(doc, dict, key);
    if (v && v->type == PDF_REAL) {
        return v->u.r;
    }
    if (v && v->type == PDF_INT) {
        return (double)v->u.i;
    }
    return def;
}

int pdf_array_count(pdf_doc_t *doc, pdf_obj_t *array)
{
    array = pdf_resolve(doc, array);
    return array && array->type == PDF_ARRAY ? array->u.array.count : 0;
}

pdf_obj_t *pdf_array_get(pdf_doc_t *doc, pdf_obj_t *array, int index)
{
    array = pdf_resolve(doc, array);
    if (!array || array->type != PDF_ARRAY || index < 0 || index >= array->u.array.count) {
        return NULL;
    }
    return pdf_resolve(doc, array->u.array.items[index]);
}

/* ------------------------------------------------------------- pages ----- */

static void add_page(pdf_doc_t *doc, pdf_obj_t *dict, pdf_obj_t *resources, pdf_obj_t *contents,
                     double mb[4], int rotate)
{
    if (doc->page_count >= PDF_MAX_PAGES) {
        return;
    }
    pdf_page_t *np = realloc(doc->pages, (size_t)(doc->page_count + 1) * sizeof(pdf_page_t));
    if (!np) {
        return;
    }
    doc->pages = np;
    pdf_page_t *p = &doc->pages[doc->page_count++];
    p->dict = dict;
    p->resources = resources;
    p->contents = contents;
    p->rotate = rotate;
    memcpy(p->mediabox, mb, sizeof(p->mediabox));
}

static void walk_pages(pdf_doc_t *doc, pdf_obj_t *node, pdf_obj_t *resources, double mb[4],
                       int rotate, int depth)
{
    if (depth > PDF_MAX_DEPTH || doc->page_count >= PDF_MAX_PAGES) {
        return;
    }
    node = pdf_resolve(doc, node);
    pdf_obj_t *dict = dict_of(node);
    if (!dict) {
        return;
    }
    pdf_obj_t *r = pdf_dict_get(doc, dict, "Resources");
    if (r) {
        resources = r;
    }
    pdf_obj_t *m = pdf_dict_get(doc, dict, "MediaBox");
    if (m && m->type == PDF_ARRAY && m->u.array.count >= 4) {
        for (int i = 0; i < 4; i++) {
            pdf_obj_t *v = pdf_resolve(doc, m->u.array.items[i]);
            mb[i] = v && v->type == PDF_REAL ? v->u.r : (v && v->type == PDF_INT ? (double)v->u.i : mb[i]);
        }
    }
    pdf_obj_t *rot = pdf_dict_get(doc, dict, "Rotate");
    if (rot && rot->type == PDF_INT) {
        rotate = (int)rot->u.i;
    }

    pdf_obj_t *type = pdf_dict_get(doc, dict, "Type");
    bool is_pages = pdf_name_eq(type, "Pages");
    pdf_obj_t *kids = pdf_dict_get(doc, dict, "Kids");
    if (is_pages || (kids && kids->type == PDF_ARRAY)) {
        if (kids && kids->type == PDF_ARRAY) {
            for (int i = 0; i < kids->u.array.count; i++) {
                walk_pages(doc, kids->u.array.items[i], resources, mb, rotate, depth + 1);
            }
        }
        return;
    }
    pdf_obj_t *contents = pdf_dict_get(doc, dict, "Contents");
    add_page(doc, dict, resources, contents, mb, rotate);
}

static pdf_obj_t *find_catalog(pdf_doc_t *doc)
{
    if (doc->trailer) {
        pdf_obj_t *root = pdf_dict_get(doc, doc->trailer, "Root");
        if (root) {
            return root;
        }
    }
    /* Fallback: look for an object whose /Type is /Catalog. */
    for (int i = 0; i < doc->xref_count; i++) {
        if (doc->xref[i].type != 1) {
            continue;
        }
        pdf_obj_t *o = pdf_get_obj(doc, i);
        o = pdf_resolve(doc, o);
        pdf_obj_t *dict = dict_of(o);
        if (!dict) {
            continue;
        }
        pdf_obj_t *type = pdf_dict_get(doc, dict, "Type");
        if (pdf_name_eq(type, "Catalog")) {
            return o;
        }
    }
    return NULL;
}

static bool load_pages(pdf_doc_t *doc)
{
    pdf_obj_t *catalog = find_catalog(doc);
    if (!catalog) {
        snprintf(doc->error, sizeof(doc->error), "找不到 PDF 目录对象");
        return false;
    }
    pdf_obj_t *pages = pdf_dict_get(doc, catalog, "Pages");
    if (!pages) {
        snprintf(doc->error, sizeof(doc->error), "PDF 缺少页面树");
        return false;
    }
    double mb[4] = {0, 0, 612, 792};
    walk_pages(doc, pages, NULL, mb, 0, 0);
    if (doc->page_count == 0) {
        snprintf(doc->error, sizeof(doc->error), "PDF 没有页面");
        return false;
    }
    return true;
}

/* ------------------------------------------------------------- doc ------- */

typedef struct {
    pdf_doc_t *pdf;
} pdf_impl_t;

static esp_err_t pdf_get_title(const doc_t *doc, int index, char *buf, size_t buf_len)
{
    snprintf(buf, buf_len, "第 %d 页", index + 1);
    (void)doc;
    return ESP_OK;
}

static esp_err_t pdf_get_text(const doc_t *doc, int index, char **text, size_t *len)
{
    pdf_impl_t *impl = doc->impl;
    char *out = pdf_page_text(impl->pdf, index, len);
    if (!out) {
        return ESP_FAIL;
    }
    *text = out;
    return ESP_OK;
}

static void pdf_free(pdf_doc_t *pdf)
{
    if (pdf) {
        for (int i = 0; i < pdf->font_count; i++) {
            free(pdf->fonts[i].cmap.entries);
            free(pdf->fonts[i].wide);
        }
        free(pdf->fonts);
        for (int i = 0; i < pdf->objstm_count; i++) {
            free(pdf->objstms[i].objnums);
            free(pdf->objstms[i].offsets);
        }
        free(pdf->objstms);
        free(pdf->pages);
        if (pdf->cache) {
            for (int i = 0; i < pdf->cache_count; i++) {
                if (pdf->cache[i]) {
                    obj_free(pdf->cache[i]);
                }
            }
            free(pdf->cache);
        }
        /* The trailer / xref-stream dicts are inside the cache when possible;
         * free a standalone trailer only if it is not cached. */
        if (pdf->trailer) {
            bool cached = false;
            for (int i = 0; i < pdf->cache_count; i++) {
                if (pdf->cache[i] == pdf->trailer) {
                    cached = true;
                    break;
                }
            }
            if (!cached) {
                obj_free(pdf->trailer);
            }
        }
        free(pdf->xref);
        rfree(pdf->data);
        free(pdf);
    }
}

static void pdf_close(doc_t *doc)
{
    pdf_impl_t *impl = doc->impl;
    if (!impl) {
        return;
    }
    pdf_free(impl->pdf);
    free(impl);
}

esp_err_t pdf_doc_open(const char *path, doc_t **out)
{
    *out = NULL;
    FILE *fp = fopen(path, "rb");
    if (!fp) {
        return ESP_ERR_NOT_FOUND;
    }
    fseek(fp, 0, SEEK_END);
    long fsize = ftell(fp);
    if (fsize <= 0 || (size_t)fsize > PDF_MAX_FILE) {
        fclose(fp);
        ESP_LOGE(TAG, "%s: file too large (%ld)", path, fsize);
        return ESP_ERR_INVALID_SIZE;
    }
    rewind(fp);
    uint8_t *data = rmalloc((size_t)fsize + 1);
    if (!data) {
        fclose(fp);
        return ESP_ERR_NO_MEM;
    }
    if (fread(data, 1, (size_t)fsize, fp) != (size_t)fsize) {
        rfree(data);
        fclose(fp);
        return ESP_FAIL;
    }
    fclose(fp);
    data[fsize] = '\0';

    pdf_doc_t *pdf = calloc(1, sizeof(*pdf));
    if (!pdf) {
        rfree(data);
        return ESP_ERR_NO_MEM;
    }
    pdf->data = data;
    pdf->size = (size_t)fsize;
    snprintf(pdf->path, sizeof(pdf->path), "%s", path);

    /* Locate startxref. */
    size_t tail = (size_t)fsize > 4096 ? 4096 : (size_t)fsize;
    const char *sx = NULL;
    for (size_t i = (size_t)fsize - tail; i + 9 <= (size_t)fsize; i++) {
        if (memcmp(data + i, "startxref", 9) == 0) {
            sx = (const char *)data + i + 9;
        }
    }
    long xref_off = -1;
    if (sx) {
        xref_off = strtol(sx, NULL, 10);
    }
    if (xref_off > 0 && (size_t)xref_off < pdf->size) {
        load_xref_at(pdf, (size_t)xref_off, 0);
    }
    if (!pdf->trailer) {
        ESP_LOGW(TAG, "xref unavailable, scanning objects");
        scan_objects(pdf);
    }

    if (pdf->trailer) {
        pdf_obj_t *enc = pdf_dict_get(pdf, pdf->trailer, "Encrypt");
        if (enc) {
            pdf->encrypted = true;
            snprintf(pdf->error, sizeof(pdf->error), "PDF 已加密，暂不支持");
            ESP_LOGE(TAG, "%s: encrypted PDF", path);
        }
    }
    if (pdf->encrypted) {
        pdf_free(pdf);
        return ESP_ERR_NOT_SUPPORTED;
    }

    if (!load_pages(pdf)) {
        ESP_LOGE(TAG, "%s: %s", path, pdf->error);
        pdf_free(pdf);
        return ESP_ERR_INVALID_STATE;
    }

    pdf_impl_t *impl = calloc(1, sizeof(*impl));
    doc_t *doc = calloc(1, sizeof(*doc));
    if (!impl || !doc) {
        free(impl);
        free(doc);
        pdf_free(pdf);
        return ESP_ERR_NO_MEM;
    }
    impl->pdf = pdf;
    doc->impl = impl;
    doc->info.type = DOC_TYPE_PDF;
    text_path_stem(path, doc->info.title, sizeof(doc->info.title));
    doc->info.chapter_count = pdf->page_count;
    doc->info.page_count = pdf->page_count;
    doc->info.reflow = true;
    doc->get_title = pdf_get_title;
    doc->get_text = pdf_get_text;
    doc->close = pdf_close;

    ESP_LOGI(TAG, "%s: %d pages", path, pdf->page_count);
    *out = doc;
    return ESP_OK;
}
