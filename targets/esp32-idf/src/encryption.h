#ifndef ENCRYPTION_H
#define ENCRYPTION_H

#include <stddef.h>
#include <stdint.h>

void getAuthDeviceIdBytes(uint8_t* device_id);

/* The device identity string: exactly OD_CHIP_ID_HEX_LEN uppercase hex digits, zero-padded,
 * NUL-terminated. `out` needs OD_CHIP_ID_HEX_LEN + 1 bytes; a smaller buffer yields an empty
 * string rather than a truncated id, because a truncated device id is worse than none -- it is
 * a DIFFERENT device as far as the host is concerned.
 *
 * Was `String getChipIdHex()`. Arduino's String is forbidden in shared/ outright (CLAUDE.md),
 * and this was the only one left on an interface rather than inside a function body, which is
 * why it is its own phase C step. Every caller already padded to six digits by hand; that is
 * now the contract instead of a convention. */
#define OD_CHIP_ID_HEX_LEN 6
void getChipIdHex(char* out, size_t out_size);

#endif
