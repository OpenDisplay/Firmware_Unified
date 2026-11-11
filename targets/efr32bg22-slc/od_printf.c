/* printf, snprintf, vsnprintf, puts and putchar for the BG22 image.
 *
 * These replace newlib-nano's: the linker takes a symbol from an object before it searches
 * libc, so nano's printf.o, snprintf.o and the FILE layer behind them (vfprintf, svfprintf,
 * _printf_i, fflush, findfp, makebuf, wsetup, wbuf, fwalk and the _*_r syscall shims) are
 * never pulled in. That is about 3 KB of flash, plus the 1,024 B stdout buffer nano mallocs on
 * the first printf.
 *
 * Output is byte-identical to nano for every conversion this firmware uses:
 *     %d %i %u %x %X %c %s %%   flags - 0 + space   width and precision (digits or *)
 *     length l (long is int-sized on this target, so it changes nothing)
 * Anything else (%p, %f, %ll, %h, #) is copied to the output verbatim and consumes no
 * argument. tests/host/printf_bg22_test.c checks this file against the host C library for every
 * supported combination.
 *
 * printf streams through a 32 B chunk to _write(1, ...) -- the same sink nano's stdout used
 * (od_rtt.c with RTT, sl_syscalls' stub without). Chunks are written as they fill and at the
 * end of each call, so a line reaches RTT when printf returns instead of when a 1 KB buffer
 * fills; the byte stream is the same. */
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>

int _write(int file, const char *ptr, int len);

typedef struct {
  char *buf;
  size_t cap;    /* snprintf: caller's size; stream: chunk size */
  size_t pos;
  size_t total;
  bool stream;
} od_sink_t;

static void sink_flush(od_sink_t *s)
{
  if (s->pos != 0u) {
    (void)_write(1, s->buf, (int)s->pos);
    s->pos = 0u;
  }
}

static void put(od_sink_t *s, char c)
{
  s->total++;
  if (s->stream) {
    s->buf[s->pos++] = c;
    if (s->pos == s->cap) {
      sink_flush(s);
    }
  } else if (s->pos + 1u < s->cap) {
    s->buf[s->pos++] = c;
  }
}

static void pad(od_sink_t *s, char c, int n)
{
  while (n-- > 0) {
    put(s, c);
  }
}

static int read_num(const char **f, va_list *ap)
{
  int n = 0;
  if (**f == '*') {
    (*f)++;
    return va_arg(*ap, int);
  }
  while (**f >= '0' && **f <= '9') {
    n = n * 10 + (*(*f)++ - '0');
  }
  return n;
}

static void format(od_sink_t *s, const char *f, va_list *ap)
{
  while (*f != '\0') {
    const char *spec = f;
    bool left = false;
    bool zero = false;
    char sign = 0;
    int width;
    int prec = -1;
    char tmp[11];
    const char *p = tmp;
    int len;
    int zeros = 0;

    if (*f != '%') {
      put(s, *f++);
      continue;
    }
    f++;
    for (;; f++) {
      if (*f == '-') {
        left = true;
      } else if (*f == '0') {
        zero = true;
      } else if (*f == '+') {
        sign = '+';
      } else if (*f == ' ') {
        if (sign == 0) sign = ' ';
      } else {
        break;
      }
    }
    width = read_num(&f, ap);
    if (width < 0) {
      left = true;
      width = -width;
    }
    if (*f == '.') {
      f++;
      prec = read_num(&f, ap);
      if (prec < 0) prec = -1;
    }
    if (*f == 'l') {
      f++;
    }

    switch (*f) {
    case '%':
      put(s, '%');
      f++;
      continue;
    case 'c':
      tmp[0] = (char)va_arg(*ap, int);
      len = 1;
      sign = 0;
      zero = false;
      break;
    case 's':
      p = va_arg(*ap, const char *);
      if (p == NULL) p = "(null)";
      for (len = 0; p[len] != '\0' && (prec < 0 || len < prec); len++) {
      }
      sign = 0;
      zero = false;
      break;
    case 'd':
    case 'i':
    case 'u':
    case 'x':
    case 'X': {
      unsigned v = va_arg(*ap, unsigned);
      unsigned base = (*f == 'x' || *f == 'X') ? 16u : 10u;
      const char *digits = (*f == 'x') ? "0123456789abcdef" : "0123456789ABCDEF";
      if (*f == 'd' || *f == 'i') {
        if ((int)v < 0) {
          sign = '-';
          v = 0u - v;
        }
      } else {
        sign = 0;
      }
      len = 0;
      while (v != 0u || (len == 0 && prec != 0)) {
        tmp[sizeof tmp - 1u - (size_t)len++] = digits[v % base];
        v /= base;
      }
      p = &tmp[sizeof tmp - (size_t)len];
      if (prec >= 0) {
        zero = false;
        if (prec > len) zeros = prec - len;
      }
      break;
    }
    default:
      /* Unsupported conversion: copy the spec as written. */
      if (*f != '\0') f++;
      while (spec < f) {
        put(s, *spec++);
      }
      continue;
    }
    f++;

    width -= len + zeros + (sign != 0 ? 1 : 0);
    if (!left && !zero) pad(s, ' ', width);
    if (sign != 0) put(s, sign);
    if (!left && zero) pad(s, '0', width);
    pad(s, '0', zeros);
    while (len-- > 0) {
      put(s, *p++);
    }
    if (left) pad(s, ' ', width);
  }
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
  od_sink_t s = { buf, size, 0u, 0u, false };
  va_list cp;
  va_copy(cp, ap);
  format(&s, fmt, &cp);
  va_end(cp);
  if (size != 0u) {
    buf[s.pos] = '\0';
  }
  return (int)s.total;
}

int snprintf(char *buf, size_t size, const char *fmt, ...)
{
  va_list ap;
  int n;
  va_start(ap, fmt);
  n = vsnprintf(buf, size, fmt, ap);
  va_end(ap);
  return n;
}

int vprintf(const char *fmt, va_list ap)
{
  char chunk[32];
  od_sink_t s = { chunk, sizeof chunk, 0u, 0u, true };
  va_list cp;
  va_copy(cp, ap);
  format(&s, fmt, &cp);
  va_end(cp);
  sink_flush(&s);
  return (int)s.total;
}

int printf(const char *fmt, ...)
{
  va_list ap;
  int n;
  va_start(ap, fmt);
  n = vprintf(fmt, ap);
  va_end(ap);
  return n;
}

int puts(const char *str)
{
  size_t n = strlen(str);
  (void)_write(1, str, (int)n);
  (void)_write(1, "\n", 1);
  return (int)n + 1;
}

int putchar(int c)
{
  char ch = (char)c;
  (void)_write(1, &ch, 1);
  return (unsigned char)ch;
}
