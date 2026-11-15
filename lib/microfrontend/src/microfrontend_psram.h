/* Not part of TensorFlow Lite Micro: this project's, and force-included into
 * every file of this library by library.json (see README.md).
 *
 * The front end sets itself up with malloc and calloc, a dozen buffers of a
 * few hundred bytes to a few KB -- small enough that the framework would put
 * them in internal RAM, about 12 KB of what a TLS session needs. Here they go
 * to PSRAM first, and to internal RAM only when PSRAM has none, for this
 * library alone: everything else's mallocs are left as they were. */
#ifndef MICROFRONTEND_PSRAM_H_
#define MICROFRONTEND_PSRAM_H_

/* Declared before the macros below exist, so that a later #include of either
 * finds them declared and is not rewritten by them. */
#include <stdlib.h>
#include <esp_heap_caps.h>

static inline void* microfrontend_malloc(size_t size) {
  return heap_caps_malloc_prefer(size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

static inline void* microfrontend_calloc(size_t n, size_t size) {
  return heap_caps_calloc_prefer(n, size, 2,
                                 MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
                                 MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
}

#define malloc(size) microfrontend_malloc(size)
#define calloc(n, size) microfrontend_calloc((n), (size))
#define free(p) heap_caps_free(p)

#endif  /* MICROFRONTEND_PSRAM_H_ */
