/* Freestanding <string.h> for tc32-elf, which ships no C library.
 *
 * memcpy/memset/memcmp/strlen are the Telink SDK's (common/string.c, linked into
 * every image); the prototypes here are ABI-identical to the SDK's own header,
 * whose `unsigned int` is size_t on this 32-bit target. The rest are od_libc.c. */
#ifndef OD_TLSR_STRING_H
#define OD_TLSR_STRING_H

#include <stddef.h>

/* The SDK's common/string.h declares these four with `unsigned int`; in a translation unit
 * that has read it (od_tlsr_sdk.h), its declarations stand. */
#ifndef COMMON_STRING_H_
void *memcpy(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
int memcmp(const void *a, const void *b, size_t n);
size_t strlen(const char *s);
#endif

void *memmove(void *dst, const void *src, size_t n);
void *memchr(const void *s, int c, size_t n);
int strcmp(const char *a, const char *b);
int strncmp(const char *a, const char *b, size_t n);
char *strncpy(char *dst, const char *src, size_t n);
char *strchr(const char *s, int c);
size_t strnlen(const char *s, size_t max);

#endif
