/* snprintf/vsnprintf for the TLSR825x target.
 *
 * tests/host/tlsr_fmt_test.c builds this file with -Dsnprintf=... -Dvsnprintf=... and checks it
 * against the host libc, 400k cases including every float the boot screen can pass. */
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>

/* The SDK's libc has no snprintf; the boot screen and its payload builder use it. Supported:
 * %s %c %d %u %x %X %f %%, the '0' flag, a width, a precision (strings and %f), and the 'l'
 * modifier. %f (precision <= 3) decomposes the double's bits with integer arithmetic: tc32's libgcc
 * double-precision helpers are the colliding division object (see od_tlsr_rt.c), so no FP operation
 * may appear here. Values past 2^32 or asking for more than 3 decimals print as "?". */

struct sink { char *p; size_t cap, n; };

static void put(struct sink *o, char c)
{
    if (o->n + 1u < o->cap) o->p[o->n] = c;
    o->n++;
}

static void put_uint(struct sink *o, uint32_t v, unsigned base, int upper, int width, char pad)
{
    char buf[12];
    int len = 0;
    const char *digits = upper ? "0123456789ABCDEF" : "0123456789abcdef";
    do {
        buf[len++] = digits[v % base];
        v /= base;
    } while (v != 0u);
    while (width-- > len) put(o, pad);
    while (len > 0) put(o, buf[--len]);
}

static void put_double(struct sink *o, uint64_t bits, int prec)
{
    union { uint64_t u; struct { uint32_t lo, hi; } w; } d;
    int exp;
    uint64_t mant, ipart, frac, scale = 1u;
    uint32_t pow10 = 1u;
    int i;

    d.u = bits;
    if (d.w.hi & 0x80000000u) put(o, '-');
    exp = (int)((d.w.hi >> 20) & 0x7FFu) - 1023;
    if (exp == 1024 || exp >= 32 || prec > 3) { put(o, '?'); return; }
    for (i = 0; i < prec; ++i) pow10 *= 10u;
    if (exp < -60) { ipart = 0u; frac = 0u; }
    else {
        mant = ((uint64_t)(d.w.hi & 0xFFFFFu) << 32) | d.w.lo | ((uint64_t)1u << 52);
        /* value = mant * 2^(exp-52); split into integer and a fraction scaled by 10^prec, rounded */
        if (exp >= 0) { ipart = mant >> (52 - exp); frac = mant & ((((uint64_t)1u) << (52 - exp)) - 1u); scale = (uint64_t)1u << (52 - exp); }
        else { ipart = 0u; if (exp < -11) { mant >>= (-11 - exp); exp = -11; } frac = mant; scale = (uint64_t)1u << (52 - exp); }
        {   /* frac/scale * 10^prec, rounded half to even like libc */
            uint64_t prod = frac * pow10, rem = prod & (scale - 1u), half = scale >> 1;
            frac = prod >> (52 - exp);
            if (rem > half || (rem == half && (frac & 1u))) frac++;
        }
        if (frac >= pow10) { frac -= pow10; ipart++; }
    }
    put_uint(o, (uint32_t)ipart, 10u, 0, 1, '0');
    if (prec > 0) {
        put(o, '.');
        put_uint(o, (uint32_t)frac, 10u, 0, prec, '0');
    }
}

int vsnprintf(char *out, size_t cap, const char *fmt, va_list ap)
{
    struct sink o = { out, cap, 0u };

    for (; *fmt; ++fmt) {
        char pad = ' ';
        int width = 0, prec = -1;

        if (*fmt != '%') { put(&o, *fmt); continue; }
        if (*++fmt == '0') { pad = '0'; ++fmt; }
        while (*fmt >= '0' && *fmt <= '9') width = width * 10 + (*fmt++ - '0');
        if (*fmt == '.') { prec = 0; while (*++fmt >= '0' && *fmt <= '9') prec = prec * 10 + (*fmt - '0'); }
        while (*fmt == 'l') ++fmt;
        switch (*fmt) {
        case 's': {
            const char *str = va_arg(ap, const char *);
            int len = 0;
            if (str == NULL) str = "(null)";
            while (str[len] && (prec < 0 || len < prec)) ++len;
            while (width-- > len) put(&o, ' ');
            while (len-- > 0) put(&o, *str++);
            break;
        }
        case 'c': put(&o, (char)va_arg(ap, int)); break;
        case 'd': {
            int v = va_arg(ap, int);
            if (v < 0) { put(&o, '-'); put_uint(&o, (uint32_t)(-(v + 1)) + 1u, 10u, 0, width - 1, pad); }
            else put_uint(&o, (uint32_t)v, 10u, 0, width, pad);
            break;
        }
        case 'u': put_uint(&o, va_arg(ap, unsigned int), 10u, 0, width, pad); break;
        case 'x': put_uint(&o, va_arg(ap, unsigned int), 16u, 0, width, pad); break;
        case 'X': put_uint(&o, va_arg(ap, unsigned int), 16u, 1, width, pad); break;
        case 'f': {
            union { double d; uint64_t u; } v;
            v.d = va_arg(ap, double);           /* a copy, not arithmetic */
            put_double(&o, v.u, prec < 0 ? 6 : prec);
            break;
        }
        case '%': put(&o, '%'); break;
        default:  put(&o, '%'); if (*fmt) put(&o, *fmt); else --fmt; break;
        }
    }
    if (cap != 0u) out[o.n < cap ? o.n : cap - 1u] = '\0';
    return (int)o.n;
}

int snprintf(char *out, size_t cap, const char *fmt, ...)
{
    va_list ap;
    int n;

    va_start(ap, fmt);
    n = vsnprintf(out, cap, fmt, ap);
    va_end(ap);
    return n;
}
