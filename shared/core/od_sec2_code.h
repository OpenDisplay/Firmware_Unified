/* od_sec2_code.h -- the claim commitment and the 6-digit confirmation code, behind one interface.
 *
 * SECURITY_V2 sections 2.3 and 8.4, modelled on Bluetooth LE SC f4/g2. AWAITING A CRYPTOGRAPHER
 * (section 19 E3: input order, truncation): change the construction here only.
 *
 *   Cd       = CMAC(Nd, "OD2/commit" || uid || Qd || Qc)        146-byte input
 *   code_raw = CMAC(Nc, "OD2/code"   || uid || Qc || Qd || Nd)  160-byte input
 *   code     = BE32(code_raw[12..15]) mod 1 000 000
 */
#ifndef OD_SEC2_CODE_H
#define OD_SEC2_CODE_H

#include "od_hal_crypto.h"

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum od_hal_crypto_status od_sec2_commit(const uint8_t nd[16], const uint8_t uid[8],
                                         const uint8_t qd[64], const uint8_t qc[64],
                                         uint8_t cd[16]);

enum od_hal_crypto_status od_sec2_code(const uint8_t nc[16], const uint8_t uid[8],
                                       const uint8_t qc[64], const uint8_t qd[64],
                                       const uint8_t nd[16], uint32_t *code);

/* "123 456": six zero-padded digits with a space after the third, NUL-terminated. */
void od_sec2_code_format(uint32_t code, char out[8]);

#ifdef __cplusplus
}
#endif

#endif /* OD_SEC2_CODE_H */
