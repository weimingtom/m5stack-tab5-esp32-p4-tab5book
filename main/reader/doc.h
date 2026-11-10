#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    DOC_TYPE_TXT = 0,
    DOC_TYPE_EPUB = 1,
    DOC_TYPE_PDF = 2,
} doc_type_t;

typedef struct {
    doc_type_t type;
    char title[192];
    char author[96];
    int chapter_count;
    int page_count; /* PDF only, 0 otherwise */
    bool reflow;    /* text extracted and re-flowed rather than raster pages */
} doc_info_t;

typedef struct doc doc_t;

esp_err_t doc_open(const char *path, doc_t **out);
void doc_close(doc_t *doc);

const doc_info_t *doc_info(const doc_t *doc);
const char *doc_type_name(doc_type_t type);

/* Chapter titles: EPUB spine item titles, "第 N 页" for PDF, file name for TXT. */
esp_err_t doc_chapter_title(const doc_t *doc, int index, char *buf, size_t buf_len);

/* Return a newly allocated UTF-8 chapter text.  The caller frees it with free().
 * PDF chapters are single PDF pages; EPUB chapters are spine items; TXT has one
 * chapter containing the whole book. */
esp_err_t doc_chapter_text(const doc_t *doc, int index, char **text, size_t *len);

#ifdef __cplusplus
}
#endif
