#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "doc_internal.h"
#include "esp_log.h"
#include "rmem.h"
#include "text.h"

#define TXT_MAX_BYTES (8u * 1024u * 1024u)

typedef struct {
    char *text;
    size_t len;
} txt_impl_t;

static esp_err_t txt_get_title(const doc_t *doc, int index, char *buf, size_t buf_len)
{
    (void)index;
    snprintf(buf, buf_len, "%s", doc->info.title);
    return ESP_OK;
}

static esp_err_t txt_get_text(const doc_t *doc, int index, char **text, size_t *len)
{
    if (index != 0) {
        return ESP_ERR_INVALID_ARG;
    }
    txt_impl_t *impl = doc->impl;
    char *copy = rmalloc(impl->len + 1);
    if (!copy) {
        return ESP_ERR_NO_MEM;
    }
    memcpy(copy, impl->text, impl->len);
    copy[impl->len] = '\0';
    *text = copy;
    *len = impl->len;
    return ESP_OK;
}

static void txt_close(doc_t *doc)
{
    if (!doc) {
        return;
    }
    txt_impl_t *impl = doc->impl;
    if (impl) {
        rfree(impl->text);
        free(impl);
    }
}

esp_err_t txt_doc_open(const char *path, doc_t **out)
{
    *out = NULL;

    char *raw = NULL;
    size_t raw_len = 0;
    esp_err_t err = text_read_file(path, &raw, &raw_len, TXT_MAX_BYTES);
    if (err != ESP_OK) {
        return err;
    }

    char *utf8 = NULL;
    size_t utf8_len = 0;
    err = text_to_utf8((const uint8_t *)raw, raw_len, &utf8, &utf8_len);
    rfree(raw);
    if (err != ESP_OK) {
        return err;
    }

    char *text = text_normalize(utf8, utf8_len, &utf8_len);
    if (!text) {
        return ESP_ERR_NO_MEM;
    }

    txt_impl_t *impl = calloc(1, sizeof(*impl));
    doc_t *doc = calloc(1, sizeof(*doc));
    if (!impl || !doc) {
        free(impl);
        free(doc);
        rfree(text);
        return ESP_ERR_NO_MEM;
    }
    impl->text = text;
    impl->len = utf8_len;

    doc->impl = impl;
    doc->info.type = DOC_TYPE_TXT;
    text_path_stem(path, doc->info.title, sizeof(doc->info.title));
    doc->info.chapter_count = 1;
    doc->info.reflow = true;
    doc->get_title = txt_get_title;
    doc->get_text = txt_get_text;
    doc->close = txt_close;

    ESP_LOGI("txt", "%s: %u bytes UTF-8", path, (unsigned)utf8_len);
    *out = doc;
    return ESP_OK;
}
