#pragma once

#include <stdint.h>
#include <stddef.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct zip zip_t;

typedef struct {
    char name[256];
    uint32_t comp_size;
    uint32_t uncomp_size;
    uint32_t local_offset;
    uint16_t method;
    uint16_t flags;
} zip_entry_t;

esp_err_t zip_open(const char *path, zip_t **out);
void zip_close(zip_t *zip);
int zip_count(const zip_t *zip);
const zip_entry_t *zip_entry_at(const zip_t *zip, int index);
int zip_find(const zip_t *zip, const char *name);

/* Read and decompress one entry.  *data is allocated in PSRAM and must be
 * released with free().  The returned buffer is NUL terminated. */
esp_err_t zip_read(const zip_t *zip, const char *name, uint8_t **data, size_t *size);
esp_err_t zip_read_index(const zip_t *zip, int index, uint8_t **data, size_t *size);

#ifdef __cplusplus
}
#endif
