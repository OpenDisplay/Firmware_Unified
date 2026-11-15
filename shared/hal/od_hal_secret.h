/* od_hal_secret.h -- three small security records kept OUTSIDE the config record.
 *
 * SECURITY_V2 section 4.1: OS2K (the device key, 28 B), OS2S (state, 16 B) and OS2R (the factory
 * reset marker, 12 B). The record bytes, their magic and CRC are built and checked in shared code
 * (od_sec2_rec.c); the target only stores opaque blobs under three ids.
 *
 * READS SAY ONLY "GOT EXACTLY len BYTES" OR NOT. A missing object, a short one, a read error and a
 * backend that silently dropped a corrupt entry are deliberately the same answer: the boot decision
 * (section 10.1) is written so that it never needs to tell them apart.
 *
 * WRITES ARE ATOMIC in the od_hal_nvs_write sense: after a power cut the id holds either the old
 * blob or the new one, never a mix. Erase of an absent id succeeds.
 *
 * Media: ESP32 NVS namespace "od_sec"; Nordic its own NVS ids (see the target file for the
 * H13 caveat); BG22 three NVM3 keys outside the config object.
 *
 * CONTEXT: main loop only, like od_hal_nvs. May block for a flash write.
 */
#ifndef OD_HAL_SECRET_H
#define OD_HAL_SECRET_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

enum od_hal_secret_id {
    OD_HAL_SECRET_KEY = 0,     /* OS2K */
    OD_HAL_SECRET_STATE = 1,   /* OS2S */
    OD_HAL_SECRET_MARKER = 2   /* OS2R */
};

bool od_hal_secret_read(enum od_hal_secret_id id, uint8_t *buf, uint16_t len);
bool od_hal_secret_write(enum od_hal_secret_id id, const uint8_t *buf, uint16_t len);
bool od_hal_secret_erase(enum od_hal_secret_id id);

#ifdef __cplusplus
}
#endif

#endif /* OD_HAL_SECRET_H */
