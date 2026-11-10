#pragma once

#include "doc.h"

#ifdef __cplusplus
extern "C" {
#endif

struct doc {
    doc_info_t info;
    void *impl;
    esp_err_t (*get_title)(const doc_t *doc, int index, char *buf, size_t buf_len);
    esp_err_t (*get_text)(const doc_t *doc, int index, char **text, size_t *len);
    void (*close)(doc_t *doc);
};

/* Module entry points. */
esp_err_t txt_doc_open(const char *path, doc_t **out);
esp_err_t epub_doc_open(const char *path, doc_t **out);
esp_err_t pdf_doc_open(const char *path, doc_t **out);

#ifdef __cplusplus
}
#endif
