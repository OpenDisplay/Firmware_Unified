/* od_sec2_kdf.c -- see od_sec2_kdf.h. The one place the KDF input bytes are laid out. */
#include "od_sec2_kdf.h"

#include <string.h>

#define OD_SEC2_KDF_IN_MAX (1u + OD_SEC2_LABEL_MAX + 1u + OD_SEC2_CTX_MAX + 2u)

enum od_hal_crypto_status od_sec2_extract(const uint8_t salt[16], const uint8_t z[32],
                                          uint8_t prk[16])
{
    static const uint8_t zero_salt[16];

    return od_hal_crypto_cmac(salt != NULL ? salt : zero_salt, z, 32u, prk);
}

enum od_hal_crypto_status od_sec2_kdf(const uint8_t k_in[16], const char *label,
                                      const uint8_t *ctx, uint16_t ctx_len, uint8_t out[16])
{
    uint8_t in[OD_SEC2_KDF_IN_MAX];
    const size_t label_len = strlen(label);
    size_t n = 0u;
    enum od_hal_crypto_status cs;

    if (label_len > OD_SEC2_LABEL_MAX || ctx_len > OD_SEC2_CTX_MAX ||
        (ctx == NULL && ctx_len != 0u)) {
        return OD_HAL_CRYPTO_ERROR;
    }
    in[n++] = 0x01u;                       /* [i]_8, constant: one output block */
    memcpy(&in[n], label, label_len);
    n += label_len;
    in[n++] = 0x00u;
    if (ctx_len != 0u) {
        memcpy(&in[n], ctx, ctx_len);
        n += ctx_len;
    }
    in[n++] = 0x00u;                       /* [L]_16 = 128, big-endian */
    in[n++] = 0x80u;
    cs = od_hal_crypto_cmac(k_in, in, (uint32_t)n, out);
    memset(in, 0, sizeof in);
    return cs;
}
