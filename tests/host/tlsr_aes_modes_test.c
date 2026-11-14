/* tlsr_aes_modes_test.c -- targets/telink-tlsr/src/od_aes_modes.c against published vectors
 * and against the RFC 3610 soft CCM the Nordic target shipped (session_ccm_reference.inc).
 *
 * The block cipher is the host's FIPS-197 core (aes128.c); on the tag it is the TLSR825x AES
 * engine. So a green run proves the MODES -- CMAC subkeys and padding, CCM's B_0/AAD/counter
 * layout, tag truncation, in-place operation -- not that the engine is driven correctly. The
 * target's boot-time FIPS-197 self-test covers the engine. */

#include "od_aes_modes.h"

#include "aes128.h"
#include "od_check.h"

#include <stdbool.h>
#include <stdint.h>
#include <string.h>

static bool host_block(const uint8_t key[16], const uint8_t in[16], uint8_t out[16])
{
    od_test_aes128_encrypt(key, in, out);
    return true;
}

static int s_fail_after = -1;

static bool failing_block(const uint8_t key[16], const uint8_t in[16], uint8_t out[16])
{
    if (s_fail_after == 0) {
        return false;
    }
    if (s_fail_after > 0) {
        --s_fail_after;
    }
    od_test_aes128_encrypt(key, in, out);
    return true;
}

/* ---------------------------------------------- reference CCM, byte-identical to shipped --- */
static bool aes_ecb_encrypt_16(const uint8_t key[16], const uint8_t in[16], uint8_t out[16])
{
    od_test_aes128_encrypt(key, in, out);
    return true;
}
static struct { uint8_t session_key[16]; } s_session;
#include "session_ccm_reference.inc"

static unsigned hex(const char *s, uint8_t *out)
{
    unsigned n = 0;
    while (s[0] && s[1]) {
        unsigned v;
        if (s[0] == ' ') { ++s; continue; }
        sscanf(s, "%2x", &v);
        out[n++] = (uint8_t)v;
        s += 2;
    }
    return n;
}

static void test_cmac_rfc4493(void)
{
    static const char *msgs[4] = {
        "",
        "6bc1bee22e409f96e93d7e117393172a",
        "6bc1bee22e409f96e93d7e117393172a ae2d8a571e03ac9c9eb76fac45af8e51 30c81c46a35ce411",
        ("6bc1bee22e409f96e93d7e117393172a ae2d8a571e03ac9c9eb76fac45af8e51"
         " 30c81c46a35ce411e5fbc1191a0a52ef f69f2445df4f9b17ad2b417be66c3710"),
    };
    static const char *macs[4] = {
        "bb1d6929e95937287fa37d129b756746",
        "070a16b46b4d4144f79bdd9dd04a287c",
        "dfa66747de9ae63030ca32611497c827",
        "51f0bebf7e3b9d92fc49741779363cfe",
    };
    uint8_t key[16], msg[64], want[16], got[16];
    unsigned i, n;

    CASE("cmac rfc4493");
    hex("2b7e151628aed2a6abf7158809cf4f3c", key);
    for (i = 0; i < 4u; ++i) {
        n = hex(msgs[i], msg);
        hex(macs[i], want);
        CHECK(od_aes_cmac(host_block, key, n ? msg : NULL, n, got));
        CHECK(memcmp(got, want, 16) == 0);
    }
    CHECK(!od_aes_cmac(host_block, key, NULL, 5u, got));
}

/* RFC 3610 section 8, packet vectors #1 and #2: 13-byte nonce, 8-byte AAD, M = 8. */
static void test_ccm_rfc3610(void)
{
    static const struct { const char *nonce, *plain, *ct_tag; } v[2] = {
        { "00000003020100a0a1a2a3a4a5",
          "08090a0b0c0d0e0f101112131415161718191a1b1c1d1e",
          "588c979a61c663d2f066d0c2c0f989806d5f6b61dac38417e8d12cfdf926e0" },
        { "00000004030201a0a1a2a3a4a5",
          "08090a0b0c0d0e0f101112131415161718191a1b1c1d1e1f",
          "72c91a36e135f8cf291ca894085c87e3cc15c439c9e43a3ba091d56e10400916" },
    };
    uint8_t key[16], nonce[13], aad[8], plain[32], want[48], out[48], back[48];
    unsigned i, pn;
    bool ok;

    CASE("ccm rfc3610");
    hex("c0c1c2c3c4c5c6c7c8c9cacbcccdcecf", key);
    hex("0001020304050607", aad);
    for (i = 0; i < 2u; ++i) {
        hex(v[i].nonce, nonce);
        pn = hex(v[i].plain, plain);
        hex(v[i].ct_tag, want);
        CHECK(od_aes_ccm_encrypt(host_block, key, nonce, 13u, aad, 8u, plain, (uint16_t)pn,
                                 8u, out));
        CHECK(memcmp(out, want, pn + 8u) == 0);
        CHECK(od_aes_ccm_decrypt(host_block, key, nonce, 13u, aad, 8u, out, (uint16_t)(pn + 8u),
                                 8u, back, &ok));
        CHECK(ok && memcmp(back, plain, pn) == 0);
    }
}

/* The shape od_session uses (13-byte nonce, 2-byte AAD, 12-byte tag) against the reference,
 * swept across every length that crosses a block boundary, both directions. */
static void test_ccm_differential(void)
{
    uint8_t nonce[13], aad[2], plain[300], ref_ct[300], ref_tag[12], out[312], back[312];
    uint16_t len;
    unsigned seed = 0x1234u, i;
    bool ok;

    CASE("ccm differential vs session_ccm_reference.inc");
    for (len = 0; len < 260u; ++len) {
        for (i = 0; i < 16u; ++i) s_session.session_key[i] = (uint8_t)(seed = seed * 1103515245u + 12345u) ;
        for (i = 0; i < 13u; ++i) nonce[i] = (uint8_t)(seed = seed * 1103515245u + 12345u);
        aad[0] = (uint8_t)len; aad[1] = (uint8_t)(len >> 8);
        for (i = 0; i < len; ++i) plain[i] = (uint8_t)(seed = seed * 1103515245u + 12345u);

        CHECK(od_ccm_encrypt(nonce, aad, plain, len, ref_ct, ref_tag));
        CHECK(od_aes_ccm_encrypt(host_block, s_session.session_key, nonce, 13u, aad, 2u,
                                 plain, len, 12u, out));
        CHECK(memcmp(out, ref_ct, len) == 0 && memcmp(out + len, ref_tag, 12) == 0);

        /* Reference decrypts ours, ours decrypts the reference's. */
        CHECK(od_ccm_decrypt(nonce, aad, out, len, out + len, back));
        CHECK(memcmp(back, plain, len) == 0);
        memcpy(out, ref_ct, len);
        memcpy(out + len, ref_tag, 12);
        CHECK(od_aes_ccm_decrypt(host_block, s_session.session_key, nonce, 13u, aad, 2u,
                                 out, (uint16_t)(len + 12u), 12u, back, &ok));
        CHECK(ok && memcmp(back, plain, len) == 0);
    }
}

static void test_ccm_negative(void)
{
    uint8_t key[16] = { 1 }, nonce[13] = { 2 }, aad[2] = { 3, 4 }, buf[64], back[64];
    uint8_t plain[40];
    bool ok = true;

    CASE("ccm tamper and params");
    memset(plain, 0xA5, sizeof(plain));
    CHECK(od_aes_ccm_encrypt(host_block, key, nonce, 13u, aad, 2u, plain, 40u, 12u, buf));

    buf[5] ^= 1u;                                             /* flipped ciphertext bit */
    CHECK(od_aes_ccm_decrypt(host_block, key, nonce, 13u, aad, 2u, buf, 52u, 12u, back, &ok));
    CHECK(!ok);
    CHECK(back[0] == 0u && back[39] == 0u);                   /* plaintext withheld */
    buf[5] ^= 1u;
    aad[1] ^= 1u;                                             /* AAD is authenticated */
    CHECK(od_aes_ccm_decrypt(host_block, key, nonce, 13u, aad, 2u, buf, 52u, 12u, back, &ok));
    CHECK(!ok);
    aad[1] ^= 1u;

    /* In place: out aliases plain, plain aliases in. */
    memcpy(back, plain, 40u);
    CHECK(od_aes_ccm_encrypt(host_block, key, nonce, 13u, aad, 2u, back, 40u, 12u, back));
    CHECK(memcmp(back, buf, 52u) == 0);
    CHECK(od_aes_ccm_decrypt(host_block, key, nonce, 13u, aad, 2u, back, 52u, 12u, back, &ok));
    CHECK(ok && memcmp(back, plain, 40u) == 0);

    CHECK(!od_aes_ccm_encrypt(host_block, key, nonce, 6u, aad, 2u, plain, 4u, 12u, buf));
    CHECK(!od_aes_ccm_encrypt(host_block, key, nonce, 14u, aad, 2u, plain, 4u, 12u, buf));
    CHECK(!od_aes_ccm_encrypt(host_block, key, nonce, 13u, aad, 2u, plain, 4u, 11u, buf));
    CHECK(!od_aes_ccm_decrypt(host_block, key, nonce, 13u, aad, 2u, buf, 11u, 12u, back, &ok));

    /* An engine failure anywhere is a failure, never a partial result. */
    for (s_fail_after = 0; s_fail_after < 8; ) {
        int budget = s_fail_after;
        CHECK(!od_aes_ccm_encrypt(failing_block, key, nonce, 13u, aad, 2u, plain, 40u, 12u, buf));
        s_fail_after = budget + 1;
    }
    s_fail_after = -1;
}

int main(void)
{
    test_cmac_rfc4493();
    test_ccm_rfc3610();
    test_ccm_differential();
    test_ccm_negative();
    return OD_CHECK_REPORT_NONEMPTY("tlsr_aes_modes", 1000u);
}
