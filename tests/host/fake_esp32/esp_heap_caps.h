#ifndef FAKE_ESP_HEAP_CAPS_H
#define FAKE_ESP_HEAP_CAPS_H
#include "fake_esp32.h"
#ifdef __cplusplus
extern "C" {
#endif
#define MALLOC_CAP_DMA      (1u << 3)
#define MALLOC_CAP_SPIRAM   (1u << 10)
#define MALLOC_CAP_INTERNAL (1u << 11)
void *heap_caps_malloc(size_t size, uint32_t caps);
void *heap_caps_aligned_alloc(size_t align, size_t size, uint32_t caps);
void heap_caps_free(void *p);
#ifdef __cplusplus
}
#endif
#endif
