#pragma once

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Large reader buffers live in PSRAM on Tab5; small boards fall back to the
 * internal heap.  All returned pointers can be released with rfree(). */
void *rmalloc(size_t size);
void *rcalloc(size_t count, size_t size);
void *rrealloc(void *ptr, size_t size);
void rfree(void *ptr);

#ifdef __cplusplus
}
#endif
