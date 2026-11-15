/* od_sec2_code.c -- see od_sec2_code.h. */
#include "od_sec2_code.h"

#include <string.h>

/* "OD2/commit"(10) or "OD2/code"(8), then uid(8) and three or four fixed-size fields. */
static enum od_hal_crypto_status cmac_chain(const uint8_t key[16], const char *prefix,
                                            const uint8_t uid[8], const uint8_t a[64],
                                            const uint8_t b[64], const uint8_t *c16,
                                            uint8_t out[16])
{
    uint8_t in[8u + 8u + 64u + 64u + 16u];
    const size_t plen = strlen(prefix);
    size_t n = 0u;
    enum od_hal_crypto_status cs;

    memcpy(&in[n], prefix, plen); n += plen;
    memcpy(&in[n], uid, 8u);      n += 8u;
    memcpy(&in[n], a, 64u);       n += 64u;
    memcpy(&in[n], b, 64u);       n += 64u;
    if (c16 != NULL) {
        memcpy(&in[n], c16, 16u); n += 16u;
    }
    cs = od_hal_crypto_cmac(key, in, (uint32_t)n, out);
    memset(in, 0, sizeof in);
    return cs;
}

enum od_hal_crypto_status od_sec2_commit(const uint8_t nd[16], const uint8_t uid[8],
                                         const uint8_t qd[64], const uint8_t qc[64],
                                         uint8_t cd[16])
{
    /* "OD2/commit" is 10 bytes: the buffer in cmac_chain is sized for the longer code input
     * (8 + 8 + 64 + 64 + 16 = 160 >= 10 + 8 + 64 + 64 = 146). */
    return cmac_chain(nd, "OD2/commit", uid, qd, qc, NULL, cd);
}

enum od_hal_crypto_status od_sec2_code(const uint8_t nc[16], const uint8_t uid[8],
                                       const uint8_t qc[64], const uint8_t qd[64],
                                       const uint8_t nd[16], uint32_t *code)
{
    uint8_t raw[16];
    enum od_hal_crypto_status cs = cmac_chain(nc, "OD2/code", uid, qc, qd, nd, raw);

    if (cs == OD_HAL_CRYPTO_OK) {
        *code = (((uint32_t)raw[12] << 24) | ((uint32_t)raw[13] << 16) |
                 ((uint32_t)raw[14] << 8) | raw[15]) % 1000000u;
    }
    return cs;
}

void od_sec2_code_format(uint32_t code, char out[8])
{
    unsigned i;

    code %= 1000000u;
    for (i = 7u; i-- > 0u;) {
        if (i == 3u) {
            out[i] = ' ';
            continue;
        }
        out[i] = (char)('0' + (code % 10u));
        code /= 10u;
    }
    out[7] = '\0';
}
