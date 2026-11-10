#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    PDF_NULL = 0,
    PDF_BOOL,
    PDF_INT,
    PDF_REAL,
    PDF_STRING,
    PDF_NAME,
    PDF_ARRAY,
    PDF_DICT,
    PDF_STREAM,
    PDF_REF,
} pdf_type_t;

typedef struct pdf_obj pdf_obj_t;

struct pdf_obj {
    pdf_type_t type;
    union {
        bool b;
        int64_t i;
        double r;
        struct {
            uint8_t *data;
            int len;
            bool hex;
        } str;
        struct {
            char *name;
        } name;
        struct {
            pdf_obj_t **items;
            int count;
        } array;
        struct {
            char **keys;
            pdf_obj_t **vals;
            int count;
        } dict;
        struct {
            int num;
            int gen;
        } ref;
        struct {
            pdf_obj_t *dict;
            uint8_t *raw;
            size_t raw_len;
            uint8_t *decoded;
            size_t decoded_len;
            bool decode_attempted;
        } stream;
    } u;
};

typedef struct {
    uint32_t code;
    uint16_t len;
    char utf8[8];
} pdf_map_entry_t;

typedef struct {
    pdf_map_entry_t *entries;
    int count;
    int cap;
} pdf_cmap_t;

typedef struct {
    const uint16_t *base;
    uint16_t diff[256];
    bool has_diff;
} pdf_encoding_t;

typedef struct pdf_font pdf_font_t;

struct pdf_font {
    pdf_obj_t *dict;
    bool type0;
    int default_width;
    int first_char;
    uint16_t widths[256];
    pdf_cmap_t cmap;
    pdf_encoding_t encoding;
    struct {
        uint32_t code;
        uint16_t width;
    } *wide;
    int wide_count;
};

typedef struct {
    uint8_t type; /* 1 = file offset, 2 = object stream */
    uint32_t a;
    uint32_t b;
} pdf_xref_entry_t;

typedef struct {
    pdf_obj_t *dict;
    pdf_obj_t *resources;
    pdf_obj_t *contents;
    int rotate;
    double mediabox[4];
} pdf_page_t;

typedef struct {
    int num;
    uint8_t *data;
    size_t len;
    int n;
    int first;
    int *objnums;
    int *offsets;
} pdf_objstm_t;

typedef struct {
    uint8_t *data;
    size_t size;
    char path[256];
    pdf_objstm_t *objstms;
    int objstm_count;
    int objstm_cap;
    pdf_xref_entry_t *xref;
    int xref_count;
    pdf_obj_t **cache;
    int cache_count;
    pdf_obj_t *trailer;
    bool encrypted;
    char error[160];
    pdf_page_t *pages;
    int page_count;
    pdf_font_t *fonts;
    int font_count;
    int font_cap;
} pdf_doc_t;

/* pdf.c */
pdf_obj_t *pdf_resolve(pdf_doc_t *doc, pdf_obj_t *obj);
pdf_obj_t *pdf_get_obj(pdf_doc_t *doc, int num);
pdf_obj_t *pdf_dict_get(pdf_doc_t *doc, pdf_obj_t *dict, const char *key);
bool pdf_name_eq(pdf_obj_t *obj, const char *name);
int pdf_dict_int(pdf_doc_t *doc, pdf_obj_t *dict, const char *key, int def);
double pdf_dict_real(pdf_doc_t *doc, pdf_obj_t *dict, const char *key, double def);
pdf_obj_t *pdf_array_get(pdf_doc_t *doc, pdf_obj_t *array, int index);
int pdf_array_count(pdf_doc_t *doc, pdf_obj_t *array);
uint8_t *pdf_stream_data(pdf_doc_t *doc, pdf_obj_t *stream, size_t *len);
pdf_font_t *pdf_font_get(pdf_doc_t *doc, pdf_obj_t *font_dict);
int pdf_font_decode(const pdf_font_t *font, uint32_t code, char out[8]);
int pdf_font_width(pdf_doc_t *doc, const pdf_font_t *font, uint32_t code);
int pdf_font_code_len(const pdf_font_t *font);

/* pdf_text.c */
char *pdf_page_text(pdf_doc_t *doc, int page_index, size_t *out_len);

#ifdef __cplusplus
}
#endif
