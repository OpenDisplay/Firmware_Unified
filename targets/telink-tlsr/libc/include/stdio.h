/* Freestanding <stdio.h> for tc32-elf: formatting only, no streams.
 * snprintf/vsnprintf are od_libc.c (C99 return value: the length that would
 * have been written). */
#ifndef OD_TLSR_STDIO_H
#define OD_TLSR_STDIO_H

#include <stdarg.h>
#include <stddef.h>

int snprintf(char *buf, size_t size, const char *fmt, ...)
    __attribute__((format(printf, 3, 4)));
int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
    __attribute__((format(printf, 3, 0)));

#endif
