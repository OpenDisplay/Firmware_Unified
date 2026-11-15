/* Freestanding <stdlib.h> for tc32-elf. No heap: shared/ must not assume one. */
#ifndef OD_TLSR_STDLIB_H
#define OD_TLSR_STDLIB_H

#include <stddef.h>

static inline int abs(int v) { return v < 0 ? -v : v; }
static inline long labs(long v) { return v < 0 ? -v : v; }

#endif
