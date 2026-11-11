/* aes128_test.c -- known-answer tests for the host AES-128 core.
 *
 * It guards the foundation: every expected byte the SEC2 suites compute -- the KDF, the
 * transcript tags, every CCM tag -- comes out of od_test_aes128_encrypt(). If it is wrong, those
 * suites still agree with themselves and prove nothing. So the vectors below are the standard's
 * own, not values this implementation produced.
 *
 * Vectors: FIPS-197 Appendix B (the worked example) and Appendix C.1 (AES-128 key expansion
 * example), plus NIST SP 800-38A F.1.1 ECB-AES128 block 1.
 */

#include "aes128.h"

#include <stdio.h>
#include <string.h>

static unsigned g_checks;
static unsigned g_failures;
static const char *g_case = "(none)";

#define CHECK(cond)                                                            \
    do {                                                                       \
        ++g_checks;                                                            \
        if (!(cond)) {                                                         \
            ++g_failures;                                                      \
            printf("FAIL %s:%d [%s] %s\n", __FILE__, __LINE__, g_case, #cond); \
        }                                                                      \
    } while (0)

#define CASE(name) (g_case = (name))

static void expect_block(const uint8_t key[16], const uint8_t in[16], const uint8_t want[16])
{
    uint8_t got[16];

    od_test_aes128_encrypt(key, in, got);
    CHECK(memcmp(got, want, 16) == 0);
    if (memcmp(got, want, 16) != 0) {
        unsigned i;
        printf("  want:");
        for (i = 0; i < 16u; ++i) { printf(" %02x", want[i]); }
        printf("\n  got :");
        for (i = 0; i < 16u; ++i) { printf(" %02x", got[i]); }
        printf("\n");
    }
}

static void test_fips197_appendix_b(void)
{
    static const uint8_t key[16] = {
        0x2b,0x7e,0x15,0x16,0x28,0xae,0xd2,0xa6,0xab,0xf7,0x15,0x88,0x09,0xcf,0x4f,0x3c
    };
    static const uint8_t pt[16] = {
        0x32,0x43,0xf6,0xa8,0x88,0x5a,0x30,0x8d,0x31,0x31,0x98,0xa2,0xe0,0x37,0x07,0x34
    };
    static const uint8_t ct[16] = {
        0x39,0x25,0x84,0x1d,0x02,0xdc,0x09,0xfb,0xdc,0x11,0x85,0x97,0x19,0x6a,0x0b,0x32
    };

    CASE("FIPS-197 Appendix B worked example");
    expect_block(key, pt, ct);
}

static void test_fips197_appendix_c1(void)
{
    static const uint8_t key[16] = {
        0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f
    };
    static const uint8_t pt[16] = {
        0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff
    };
    static const uint8_t ct[16] = {
        0x69,0xc4,0xe0,0xd8,0x6a,0x7b,0x04,0x30,0xd8,0xcd,0xb7,0x80,0x70,0xb4,0xc5,0x5a
    };

    CASE("FIPS-197 Appendix C.1 AES-128");
    expect_block(key, pt, ct);
}

static void test_sp800_38a_ecb_block1(void)
{
    static const uint8_t key[16] = {
        0x2b,0x7e,0x15,0x16,0x28,0xae,0xd2,0xa6,0xab,0xf7,0x15,0x88,0x09,0xcf,0x4f,0x3c
    };
    static const uint8_t pt[16] = {
        0x6b,0xc1,0xbe,0xe2,0x2e,0x40,0x9f,0x96,0xe9,0x3d,0x7e,0x11,0x73,0x93,0x17,0x2a
    };
    static const uint8_t ct[16] = {
        0x3a,0xd7,0x7b,0xb4,0x0d,0x7a,0x36,0x60,0xa8,0x9e,0xca,0xf3,0x24,0x66,0xef,0x97
    };

    CASE("SP 800-38A F.1.1 ECB-AES128 block 1");
    expect_block(key, pt, ct);
}

/* in and out are documented as allowed to alias; the CCM reference relies on it
 * (od_ccm_ecb(mac, mac)). A cipher that breaks under aliasing would fail only there, deep in a
 * tag comparison, so pin it here where the failure names itself. */
static void test_aliasing(void)
{
    static const uint8_t key[16] = {
        0x00,0x01,0x02,0x03,0x04,0x05,0x06,0x07,0x08,0x09,0x0a,0x0b,0x0c,0x0d,0x0e,0x0f
    };
    static const uint8_t pt[16] = {
        0x00,0x11,0x22,0x33,0x44,0x55,0x66,0x77,0x88,0x99,0xaa,0xbb,0xcc,0xdd,0xee,0xff
    };
    static const uint8_t ct[16] = {
        0x69,0xc4,0xe0,0xd8,0x6a,0x7b,0x04,0x30,0xd8,0xcd,0xb7,0x80,0x70,0xb4,0xc5,0x5a
    };
    uint8_t buf[16];

    CASE("in and out may alias");
    memcpy(buf, pt, 16u);
    od_test_aes128_encrypt(key, buf, buf);
    CHECK(memcmp(buf, ct, 16) == 0);
}

int main(void)
{
    test_fips197_appendix_b();
    test_fips197_appendix_c1();
    test_sp800_38a_ecb_block1();
    test_aliasing();

    printf("aes128: %u checks, %u failures\n", g_checks, g_failures);
    return (g_failures == 0u) ? 0 : 1;
}
