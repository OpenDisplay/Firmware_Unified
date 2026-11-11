#ifndef OD_FMT_H
#define OD_FMT_H

/* The two number formats this target prints into fixed strings (device name, boot screen).
 * Hand-written so the image links no libc formatter when OD_BG22_DIAG is 0. */

#include <stdint.h>

/* `digits` upper-case hex digits of v, most significant first; NOT terminated. */
static inline char *od_fmt_hex(char *out, uint32_t v, unsigned digits)
{
  static const char hex[] = "0123456789ABCDEF";
  while (digits > 0u) {
    digits--;
    *out++ = hex[(v >> (4u * digits)) & 0xFu];
  }
  return out;
}

/* Decimal 0..255 without leading zeros; NOT terminated. */
static inline char *od_fmt_u8(char *out, unsigned v)
{
  v &= 0xFFu;
  if (v >= 100u) {
    *out++ = (char)('0' + v / 100u);
  }
  if (v >= 10u) {
    *out++ = (char)('0' + (v / 10u) % 10u);
  }
  *out++ = (char)('0' + v % 10u);
  return out;
}

#endif /* OD_FMT_H */
