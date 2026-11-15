/* The C library pieces shared/ needs that neither tc32-elf-gcc nor the Telink
 * SDK provides. memcpy/memset/memcmp/strlen come from the SDK (common/string.c).
 *
 * vsnprintf covers the conversions shared/ uses: d i u x X c s p %, flags
 * '-' '0', field width, precision (also '*'), length h hh l ll z, and %f with
 * a precision of at most 6 (shared/ prints sensor values with .1f/.2f). There
 * is no %e/%g and no %n. Output is always NUL-terminated when size > 0, and
 * the return value is the C99 one: the length the full output would have. */
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

void *memmove(void *dst, const void *src, size_t n)
{
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    if (d == s || n == 0u) {
        return dst;
    }
    if (d < s || d >= s + n) {
        while (n--) {
            *d++ = *s++;
        }
    } else {
        d += n;
        s += n;
        while (n--) {
            *--d = *--s;
        }
    }
    return dst;
}

void *memchr(const void *s, int c, size_t n)
{
    const uint8_t *p = (const uint8_t *)s;
    for (; n != 0u; n--, p++) {
        if (*p == (uint8_t)c) {
            return (void *)p;
        }
    }
    return NULL;
}

int strcmp(const char *a, const char *b)
{
    while (*a != '\0' && *a == *b) {
        a++;
        b++;
    }
    return (int)(uint8_t)*a - (int)(uint8_t)*b;
}

int strncmp(const char *a, const char *b, size_t n)
{
    for (; n != 0u; n--, a++, b++) {
        if (*a != *b || *a == '\0') {
            return (int)(uint8_t)*a - (int)(uint8_t)*b;
        }
    }
    return 0;
}

char *strncpy(char *dst, const char *src, size_t n)
{
    size_t i = 0u;
    for (; i < n && src[i] != '\0'; i++) {
        dst[i] = src[i];
    }
    for (; i < n; i++) {
        dst[i] = '\0';
    }
    return dst;
}

char *strchr(const char *s, int c)
{
    for (;; s++) {
        if (*s == (char)c) {
            return (char *)s;
        }
        if (*s == '\0') {
            return NULL;
        }
    }
}

size_t strnlen(const char *s, size_t max)
{
    size_t n = 0u;
    while (n < max && s[n] != '\0') {
        n++;
    }
    return n;
}

struct od_out {
    char *buf;
    size_t size;
    size_t len;
};

static void out_ch(struct od_out *o, char c)
{
    if (o->len + 1u < o->size) {
        o->buf[o->len] = c;
    }
    o->len++;
}

static void out_pad(struct od_out *o, char c, int n)
{
    while (n-- > 0) {
        out_ch(o, c);
    }
}

/* One converted field: optional sign/prefix, then digits/text, padded to width. */
static void out_field(struct od_out *o, const char *prefix, const char *body, int body_len,
                      int zeros, int width, int left, int zero_pad)
{
    int plen = (int)strlen(prefix);
    int total = plen + zeros + body_len;
    int pad = width > total ? width - total : 0;
    if (!left && !zero_pad) {
        out_pad(o, ' ', pad);
    }
    while (*prefix != '\0') {
        out_ch(o, *prefix++);
    }
    if (!left && zero_pad) {
        out_pad(o, '0', pad);
    }
    out_pad(o, '0', zeros);
    for (int i = 0; i < body_len; i++) {
        out_ch(o, body[i]);
    }
    if (left) {
        out_pad(o, ' ', pad);
    }
}

static int utoa_rev(unsigned long long v, unsigned base, int upper, char *tmp)
{
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    int n = 0;
    do {
        tmp[n++] = digits[v % base];
        v /= base;
    } while (v != 0u);
    return n;
}

static void out_unsigned(struct od_out *o, unsigned long long v, unsigned base, int upper,
                         const char *prefix, int width, int prec, int left, int zero_pad)
{
    char rev[24];
    char body[24];
    int n;
    if (prec == 0 && v == 0u) {
        n = 0;
    } else {
        n = utoa_rev(v, base, upper, rev);
    }
    for (int i = 0; i < n; i++) {
        body[i] = rev[n - 1 - i];
    }
    int zeros = prec > n ? prec - n : 0;
    out_field(o, prefix, body, n, zeros, width, left, prec < 0 && zero_pad);
}

static void out_double(struct od_out *o, double v, int width, int prec, int left, int zero_pad)
{
    static const uint32_t scale[] = {1u, 10u, 100u, 1000u, 10000u, 100000u, 1000000u};
    char body[32];
    int n = 0;
    const char *prefix = "";
    if (prec < 0) {
        prec = 6;
    }
    if (prec > 6) {
        prec = 6;
    }
    if (v != v) {
        out_field(o, "", "nan", 3, 0, width, left, 0);
        return;
    }
    if (v < 0.0) {
        prefix = "-";
        v = -v;
    }
    if (v >= 4294967295.0) {
        out_field(o, prefix, "inf", 3, 0, width, left, 0);
        return;
    }
    uint32_t ip = (uint32_t)v;
    double frac = (v - (double)ip) * (double)scale[prec];
    uint32_t fp = (uint32_t)frac;
    double rem = frac - (double)fp;
    /* Ties go to even. A tie is judged after scaling in double, so a value a
     * few ulp off halfway (0.005) can round the other way from glibc. */
    if (rem > 0.5 || (rem == 0.5 && ((prec > 0 ? fp : ip) & 1u) != 0u)) {
        fp++;
    }
    if (fp >= scale[prec]) {
        fp -= scale[prec];
        ip++;
    }
    char rev[12];
    int k = utoa_rev(ip, 10u, 0, rev);
    while (k > 0) {
        body[n++] = rev[--k];
    }
    if (prec > 0) {
        body[n++] = '.';
        for (int i = prec - 1; i >= 0; i--) {
            body[n + i] = (char)('0' + fp % 10u);
            fp /= 10u;
        }
        n += prec;
    }
    out_field(o, prefix, body, n, 0, width, left, zero_pad);
}

int vsnprintf(char *buf, size_t size, const char *fmt, va_list ap)
{
    struct od_out o = {buf, size, 0u};

    while (*fmt != '\0') {
        if (*fmt != '%') {
            out_ch(&o, *fmt++);
            continue;
        }
        fmt++;
        int left = 0;
        int zero_pad = 0;
        for (;; fmt++) {
            if (*fmt == '-') {
                left = 1;
            } else if (*fmt == '0') {
                zero_pad = 1;
            } else if (*fmt == '+' || *fmt == ' ' || *fmt == '#') {
                /* accepted, not implemented: shared/ does not use them */
            } else {
                break;
            }
        }
        int width = 0;
        if (*fmt == '*') {
            width = va_arg(ap, int);
            if (width < 0) {
                left = 1;
                width = -width;
            }
            fmt++;
        } else {
            while (*fmt >= '0' && *fmt <= '9') {
                width = width * 10 + (*fmt++ - '0');
            }
        }
        int prec = -1;
        if (*fmt == '.') {
            fmt++;
            prec = 0;
            if (*fmt == '*') {
                prec = va_arg(ap, int);
                fmt++;
            } else {
                while (*fmt >= '0' && *fmt <= '9') {
                    prec = prec * 10 + (*fmt++ - '0');
                }
            }
        }
        int lng = 0; /* 0 int, 1 long, 2 long long, 3 size_t */
        if (*fmt == 'h') {
            fmt++;
            if (*fmt == 'h') {
                fmt++;
            }
        } else if (*fmt == 'l') {
            fmt++;
            lng = 1;
            if (*fmt == 'l') {
                fmt++;
                lng = 2;
            }
        } else if (*fmt == 'z') {
            fmt++;
            lng = 3;
        }

        char c = *fmt;
        if (c == '\0') {
            break;
        }
        fmt++;
        switch (c) {
        case 'd':
        case 'i': {
            long long v;
            if (lng == 2) {
                v = va_arg(ap, long long);
            } else if (lng == 1) {
                v = va_arg(ap, long);
            } else {
                v = va_arg(ap, int);
            }
            unsigned long long mag = v < 0 ? (unsigned long long)(-(v + 1)) + 1u
                                           : (unsigned long long)v;
            out_unsigned(&o, mag, 10u, 0, v < 0 ? "-" : "", width, prec, left, zero_pad);
            break;
        }
        case 'u':
        case 'x':
        case 'X': {
            unsigned long long v;
            if (lng == 2) {
                v = va_arg(ap, unsigned long long);
            } else if (lng == 1) {
                v = va_arg(ap, unsigned long);
            } else if (lng == 3) {
                v = va_arg(ap, size_t);
            } else {
                v = va_arg(ap, unsigned int);
            }
            out_unsigned(&o, v, c == 'u' ? 10u : 16u, c == 'X', "", width, prec, left,
                         zero_pad);
            break;
        }
        case 'p':
            out_unsigned(&o, (uintptr_t)va_arg(ap, void *), 16u, 0, "0x", width, -1, left, 0);
            break;
        case 'c': {
            char ch = (char)va_arg(ap, int);
            out_field(&o, "", &ch, 1, 0, width, left, 0);
            break;
        }
        case 's': {
            const char *s = va_arg(ap, const char *);
            if (s == NULL) {
                s = "(null)";
            }
            size_t n = prec >= 0 ? strnlen(s, (size_t)prec) : strlen(s);
            out_field(&o, "", s, (int)n, 0, width, left, 0);
            break;
        }
        case 'f':
        case 'F':
            out_double(&o, va_arg(ap, double), width, prec, left, zero_pad);
            break;
        case '%':
            out_ch(&o, '%');
            break;
        default:
            out_ch(&o, '%');
            out_ch(&o, c);
            break;
        }
    }

    if (size != 0u) {
        o.buf[o.len < size ? o.len : size - 1u] = '\0';
    }
    return (int)o.len;
}

int snprintf(char *buf, size_t size, const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(buf, size, fmt, ap);
    va_end(ap);
    return n;
}
