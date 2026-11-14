/* Runtime pieces that neither the Telink SDK's minimal libc nor tc32's libgcc provide cleanly.
 *
 * tc32's libgcc packs __ashldi3/__lshrdi3 into one assembly object together with its own
 * __divsi3 family, which collides with the SDK's RAM-resident copies in common/div_mod.S.
 * Defining the two 64-bit shifts here means the linker never pulls that object in.
 *
 * The same libgcc's _arm_*sf* members are that division object again rather than the soft-float
 * code their names promise, so float-to-unsigned conversion (od_advert's temperature encoding)
 * has no working provider either. The real soft-float objects (addsf3.o, gesf2.o, ...) are fine,
 * but addsf3.o needs __clzsi2, whose only libgcc provider is the division object again -- so it
 * is defined here as well, and nothing ever resolves to that object. */
#include <stddef.h>
#include <stdint.h>

typedef union {
    uint64_t u;
    struct { uint32_t lo, hi; } w;    /* TC32 is little-endian */
} dw_t;

uint64_t __ashldi3(uint64_t a, int b);
uint64_t __lshrdi3(uint64_t a, int b);

uint64_t __ashldi3(uint64_t a, int b)
{
    dw_t in, out;

    in.u = a;
    if (b == 0) {
        return a;
    }
    if (b >= 32) {
        out.w.lo = 0u;
        out.w.hi = in.w.lo << (b - 32);
    } else {
        out.w.lo = in.w.lo << b;
        out.w.hi = (in.w.hi << b) | (in.w.lo >> (32 - b));
    }
    return out.u;
}

uint64_t __lshrdi3(uint64_t a, int b)
{
    dw_t in, out;

    in.u = a;
    if (b == 0) {
        return a;
    }
    if (b >= 32) {
        out.w.hi = 0u;
        out.w.lo = in.w.hi >> (b - 32);
    } else {
        out.w.hi = in.w.hi >> b;
        out.w.lo = (in.w.lo >> b) | (in.w.hi << (32 - b));
    }
    return out.u;
}

/* Soft-float float -> uint32 (C semantics: truncate toward zero). Negative input and NaN give 0,
 * values past 2^32 saturate; C leaves both undefined and od_advert clamps before converting. */
unsigned int __fixunssfsi(float a);

unsigned int __fixunssfsi(float a)
{
    union { float f; uint32_t u; } v;
    int32_t exp;
    uint32_t mant;

    v.f = a;
    if ((v.u & 0x80000000u) != 0u || (v.u & 0x7F800000u) == 0x7F800000u) {
        return (v.u == 0x7F800000u) ? 0xFFFFFFFFu : 0u;   /* +inf saturates; -x and NaN give 0 */
    }
    exp = (int32_t)((v.u >> 23) & 0xFFu) - 127;
    if (exp < 0) {
        return 0u;
    }
    if (exp >= 32) {
        return 0xFFFFFFFFu;
    }
    mant = (v.u & 0x007FFFFFu) | 0x00800000u;
    return (exp > 23) ? (mant << (exp - 23)) : (mant >> (23 - exp));
}

int __clzsi2(uint32_t x);

int __clzsi2(uint32_t x)
{
    int n = 0;

    if (x == 0u) {
        return 32;
    }
    while ((x & 0x80000000u) == 0u) {
        x <<= 1;
        ++n;
    }
    return n;
}

void *memmove(void *dst, const void *src, size_t n)
{
    unsigned char *d = dst;
    const unsigned char *s = src;

    if (d == s || n == 0u) {
        return dst;
    }
    if (d < s) {
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
