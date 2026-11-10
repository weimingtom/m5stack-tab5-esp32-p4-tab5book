#include "rmem.h"

#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"

static void *psram_malloc(size_t size)
{
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) {
        p = malloc(size);
    }
    return p;
}

void *rmalloc(size_t size)
{
    return psram_malloc(size);
}

void *rcalloc(size_t count, size_t size)
{
    size_t total = count * size;
    void *p = psram_malloc(total);
    if (p) {
        memset(p, 0, total);
    }
    return p;
}

void *rrealloc(void *ptr, size_t size)
{
    if (!ptr) {
        return psram_malloc(size);
    }
    void *p = heap_caps_realloc(ptr, size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!p) {
        p = realloc(ptr, size);
    }
    return p;
}

void rfree(void *ptr)
{
    free(ptr);
}
