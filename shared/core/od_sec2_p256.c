/* od_sec2_p256.c -- P-256 public-key validation (SECURITY_V2 section 2, A18.9).
 *
 * A received point is valid only if X < p, Y < p and Y^2 == X^3 - 3X + b (mod p). P-256 has
 * cofactor 1, so these three checks are full public-key validation (SP 800-56A 5.6.2.3.3), and
 * (0, 0) -- the only encoding a sloppy client might send for "infinity" -- fails the equation.
 * The check runs here, before any vendor ECC call, because the spec does not let any target
 * rely on its backend to validate (section 19 E7).
 *
 * Montgomery multiplication over eight 32-bit limbs, little-endian limb order. p == -1 mod 2^32,
 * so the per-word Montgomery factor -p^-1 mod 2^32 is 1 and m is simply t[0]. Everything is
 * compared in the Montgomery domain (each value times R = 2^256), which needs two constants:
 * R^2 mod p to enter the domain and b*R mod p for the curve constant.
 *
 * NOT constant time, deliberately: every input is a public key that went over the air.
 */
#include "od_sec2.h"

#include <string.h>

#define LIMBS 8u

static const uint32_t P[LIMBS] = {
    0xFFFFFFFFu, 0xFFFFFFFFu, 0xFFFFFFFFu, 0x00000000u,
    0x00000000u, 0x00000000u, 0x00000001u, 0xFFFFFFFFu
};
static const uint32_t RR[LIMBS] = {
    0x00000003u, 0x00000000u, 0xFFFFFFFFu, 0xFFFFFFFBu,
    0xFFFFFFFEu, 0xFFFFFFFFu, 0xFFFFFFFDu, 0x00000004u
};
static const uint32_t BR[LIMBS] = {
    0x29C4BDDFu, 0xD89CDF62u, 0x78843090u, 0xACF005CDu,
    0xF7212ED6u, 0xE5A220ABu, 0x04874834u, 0xDC30061Du
};

static void from_be(uint32_t out[LIMBS], const uint8_t in[32])
{
    unsigned i;

    for (i = 0; i < LIMBS; ++i) {
        const uint8_t *b = &in[28u - 4u * i];
        out[i] = ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
    }
}

/* a >= p ? */
static int ge_p(const uint32_t a[LIMBS])
{
    unsigned i = LIMBS;

    while (i-- > 0u) {
        if (a[i] != P[i]) {
            return a[i] > P[i];
        }
    }
    return 1;
}

/* r -= p, modulo 2^256. */
static void sub_p(uint32_t r[LIMBS])
{
    uint64_t borrow = 0u;
    unsigned i;

    for (i = 0; i < LIMBS; ++i) {
        const uint64_t d = (uint64_t)r[i] - P[i] - borrow;
        r[i] = (uint32_t)d;
        borrow = (d >> 63) & 1u;
    }
}

/* r = a - b (+ p if it borrowed); r may alias a. */
static void sub_mod(uint32_t r[LIMBS], const uint32_t a[LIMBS], const uint32_t b[LIMBS])
{
    uint64_t borrow = 0u;
    uint64_t carry = 0u;
    unsigned i;

    for (i = 0; i < LIMBS; ++i) {
        const uint64_t d = (uint64_t)a[i] - b[i] - borrow;
        r[i] = (uint32_t)d;
        borrow = (d >> 63) & 1u;
    }
    if (borrow != 0u) {
        for (i = 0; i < LIMBS; ++i) {
            carry += (uint64_t)r[i] + P[i];
            r[i] = (uint32_t)carry;
            carry >>= 32;
        }
    }
}

/* r = a + b mod p, inputs < p. */
static void add_mod(uint32_t r[LIMBS], const uint32_t a[LIMBS], const uint32_t b[LIMBS])
{
    uint64_t carry = 0u;
    unsigned i;

    for (i = 0; i < LIMBS; ++i) {
        carry += (uint64_t)a[i] + b[i];
        r[i] = (uint32_t)carry;
        carry >>= 32;
    }
    if (carry != 0u || ge_p(r)) {
        sub_p(r);   /* a + b < 2p; the borrow cancels the dropped carry */
    }
}

/* r = a * b * R^-1 mod p (CIOS), inputs < p, output < p. r may alias a or b. */
static void mont_mul(uint32_t r[LIMBS], const uint32_t a[LIMBS], const uint32_t b[LIMBS])
{
    uint32_t t[LIMBS + 2u];
    unsigned i;
    unsigned j;

    memset(t, 0, sizeof t);
    for (i = 0; i < LIMBS; ++i) {
        uint64_t c = 0u;
        uint32_t m;

        for (j = 0; j < LIMBS; ++j) {
            c += (uint64_t)t[j] + (uint64_t)a[j] * b[i];
            t[j] = (uint32_t)c;
            c >>= 32;
        }
        c += t[LIMBS];
        t[LIMBS] = (uint32_t)c;
        t[LIMBS + 1u] = (uint32_t)(c >> 32);

        m = t[0];  /* -p^-1 mod 2^32 == 1 */
        c = (uint64_t)t[0] + (uint64_t)m * P[0];
        c >>= 32;
        for (j = 1; j < LIMBS; ++j) {
            c += (uint64_t)t[j] + (uint64_t)m * P[j];
            t[j - 1u] = (uint32_t)c;
            c >>= 32;
        }
        c += t[LIMBS];
        t[LIMBS - 1u] = (uint32_t)c;
        t[LIMBS] = t[LIMBS + 1u] + (uint32_t)(c >> 32);
    }
    /* t < 2p: subtract p once if t[8] carries or t >= p. */
    if (t[LIMBS] != 0u || ge_p(t)) {
        sub_p(t);
    }
    memcpy(r, t, LIMBS * sizeof(uint32_t));
}

bool od_sec2_point_valid(const uint8_t q[64])
{
    uint32_t x[LIMBS];
    uint32_t y[LIMBS];
    uint32_t t[LIMBS];

    if (q == NULL) {
        return false;
    }
    from_be(x, q);
    from_be(y, q + 32);
    if (ge_p(x) || ge_p(y)) {
        return false;
    }
    mont_mul(x, x, RR);          /* xR */
    mont_mul(y, y, RR);          /* yR */
    mont_mul(y, y, y);           /* y^2 R */
    mont_mul(t, x, x);           /* x^2 R */
    mont_mul(t, t, x);           /* x^3 R */
    sub_mod(t, t, x);
    sub_mod(t, t, x);
    sub_mod(t, t, x);            /* (x^3 - 3x) R */
    add_mod(t, t, BR);           /* (x^3 - 3x + b) R */
    return memcmp(t, y, sizeof t) == 0;
}
