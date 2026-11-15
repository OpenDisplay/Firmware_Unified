/* Compiler runtime helpers that tc32-elf-gcc 4.5.1's libgcc.a gets wrong.
 *
 * That libgcc was built with every lib1funcs member (_clzsi2.o, _muldi3.o,
 * _thumb1_case_*.o, ...) carrying its own strong __divsi3/__udivsi3/__modsi3/
 * __umodsi3. The Telink SDK defines those in RAM (common/div_mod.S) so that
 * ram_code can divide while flash is busy, so pulling any such member is a
 * multiple definition -- and with the duplicates allowed, a flash-resident
 * copy whose 11-bit jump to the RAM `div` cannot reach. Its _fixunssfsi.o and
 * _fixunsdfsi.o are also empty. The fp-bit float members (addsf3.o, muldf3.o,
 * ...) are plain C and are fine; they come from -lgcc after this object.
 *
 * Everything here is defined so that no lib1funcs member is ever needed. It is
 * linked before -lgcc. Build with -O2: -Os emits __gnu_thumb1_case_* switch
 * helpers, which live only in those members. */
#include <stdint.h>

int __clzsi2(uint32_t x);
uint64_t __muldi3(uint64_t a, uint64_t b);
uint32_t __fixunssfsi(float f);
uint32_t __fixunsdfsi(double d);

int __clzsi2(uint32_t x)
{
    int n = 0;
    if (x == 0u) {
        return 32;
    }
    if ((x & 0xFFFF0000u) == 0u) {
        n += 16;
        x <<= 16;
    }
    if ((x & 0xFF000000u) == 0u) {
        n += 8;
        x <<= 8;
    }
    if ((x & 0xF0000000u) == 0u) {
        n += 4;
        x <<= 4;
    }
    if ((x & 0xC0000000u) == 0u) {
        n += 2;
        x <<= 2;
    }
    if ((x & 0x80000000u) == 0u) {
        n += 1;
    }
    return n;
}

/* 32x32 -> 64 from 16-bit halves: Thumb-1 has no widening multiply, and a
 * 64-bit product written in C would be compiled into a call to __muldi3. */
static uint64_t mul32x32(uint32_t a, uint32_t b)
{
    uint32_t al = a & 0xFFFFu, ah = a >> 16;
    uint32_t bl = b & 0xFFFFu, bh = b >> 16;
    uint32_t ll = al * bl;
    uint32_t lh = al * bh;
    uint32_t hl = ah * bl;
    uint32_t hh = ah * bh;
    uint32_t mid = (ll >> 16) + (lh & 0xFFFFu) + (hl & 0xFFFFu);
    uint32_t lo = (ll & 0xFFFFu) | (mid << 16);
    uint32_t hi = hh + (lh >> 16) + (hl >> 16) + (mid >> 16);
    return ((uint64_t)hi << 32) | lo;
}

uint64_t __muldi3(uint64_t a, uint64_t b)
{
    uint32_t alo = (uint32_t)a, ahi = (uint32_t)(a >> 32);
    uint32_t blo = (uint32_t)b, bhi = (uint32_t)(b >> 32);
    uint64_t r = mul32x32(alo, blo);
    uint32_t cross = alo * bhi + ahi * blo; /* only the low 32 bits survive */
    return r + ((uint64_t)cross << 32);
}

/* Truncating float -> unsigned conversions, C semantics for in-range values;
 * negative and NaN give 0, too large saturates (C leaves both undefined). */
uint32_t __fixunssfsi(float f)
{
    union {
        float f;
        uint32_t u;
    } v;
    v.f = f;
    uint32_t sign = v.u >> 31;
    int exp = (int)((v.u >> 23) & 0xFFu) - 127;
    uint32_t mant = (v.u & 0x7FFFFFu) | 0x800000u;
    if (sign != 0u || exp < 0 || ((v.u >> 23) & 0xFFu) == 0xFFu) {
        return (sign == 0u && ((v.u >> 23) & 0xFFu) == 0xFFu && (v.u & 0x7FFFFFu) == 0u)
                   ? 0xFFFFFFFFu
                   : 0u;
    }
    if (exp >= 32) {
        return 0xFFFFFFFFu;
    }
    return exp >= 23 ? mant << (exp - 23) : mant >> (23 - exp);
}

uint32_t __fixunsdfsi(double d)
{
    union {
        double d;
        uint64_t u;
    } v;
    v.d = d;
    uint32_t hi = (uint32_t)(v.u >> 32);
    uint32_t lo = (uint32_t)v.u;
    uint32_t sign = hi >> 31;
    uint32_t rawexp = (hi >> 20) & 0x7FFu;
    int exp = (int)rawexp - 1023;
    if (rawexp == 0x7FFu) {
        return (sign == 0u && (hi & 0xFFFFFu) == 0u && lo == 0u) ? 0xFFFFFFFFu : 0u;
    }
    if (sign != 0u || exp < 0) {
        return 0u;
    }
    if (exp >= 32) {
        return 0xFFFFFFFFu;
    }
    /* mantissa is 1.(20 bits of hi)(32 bits of lo); take the top exp+1 bits */
    uint32_t mhi = (hi & 0xFFFFFu) | 0x100000u; /* bits 52..32 */
    if (exp <= 20) {
        return mhi >> (20 - exp);
    }
    return (mhi << (exp - 20)) | (lo >> (52 - exp));
}
