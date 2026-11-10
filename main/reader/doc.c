#include "doc.h"

#include <ctype.h>
#include <stdlib.h>
#include <string.h>

#include "doc_internal.h"
#include "esp_log.h"
#include "text.h"

static const char *TAG = "doc";

static bool ext_is(const char *path, const char *ext)
{
    const char *e = text_path_ext(path);
    if (!e || e[0] == '\0') {
        return false;
    }
    e++;
    while (*e && *ext) {
        if (tolower((unsigned char)*e) != tolower((unsigned char)*ext)) {
            return false;
        }
        e++;
        ext++;
    }
    return *e == '\0' && *ext == '\0';
}

esp_err_t doc_open(const char *path, doc_t **out)
{
    *out = NULL;
    if (!path) {
        return ESP_ERR_INVALID_ARG;
    }
    esp_err_t err;
    if (ext_is(path, "txt") || ext_is(path, "text") || ext_is(path, "md")) {
        err = txt_doc_open(path, out);
    } else if (ext_is(path, "epub")) {
        err = epub_doc_open(path, out);
    } else if (ext_is(path, "pdf")) {
        err = pdf_doc_open(path, out);
    } else {
        ESP_LOGE(TAG, "unsupported file: %s", path);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (err == ESP_OK && out && *out) {
        ESP_LOGI(TAG, "opened %s: type=%s chapters=%d pages=%d title=%s", path,
                 doc_type_name((*out)->info.type), (*out)->info.chapter_count,
                 (*out)->info.page_count, (*out)->info.title);
    }
    return err;
}

void doc_close(doc_t *doc)
{
    if (!doc) {
        return;
    }
    if (doc->close) {
        doc->close(doc);
    }
    free(doc);
}

const doc_info_t *doc_info(const doc_t *doc)
{
    return doc ? &doc->info : NULL;
}

const char *doc_type_name(doc_type_t type)
{
    switch (type) {
    case DOC_TYPE_TXT:
        return "TXT";
    case DOC_TYPE_EPUB:
        return "EPUB";
    case DOC_TYPE_PDF:
        return "PDF";
    default:
        return "?";
    }
}

esp_err_t doc_chapter_title(const doc_t *doc, int index, char *buf, size_t buf_len)
{
    if (!doc || !buf || buf_len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (doc->get_title) {
        return doc->get_title(doc, index, buf, buf_len);
    }
    snprintf(buf, buf_len, "%s", doc->info.title);
    return ESP_OK;
}

esp_err_t doc_chapter_text(const doc_t *doc, int index, char **text, size_t *len)
{
    if (!doc || !doc->get_text) {
        return ESP_ERR_INVALID_STATE;
    }
    return doc->get_text(doc, index, text, len);
}
