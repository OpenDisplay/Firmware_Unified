/* boot_key_stub.c -- od_boot_screen_key() for suites that render the NORMAL boot screen through
 * od_display.c without linking od_sec2 (the real one, od_boot_key.c, needs the security state).
 * Always NOT_SET: these suites test the panel path, not the key policy (boot_screen_test does). */
#include "od_boot_payload.h"

#include <string.h>

enum od_boot_key_state od_boot_screen_key(uint8_t key[OD_BOOT_KEY_SIZE])
{
    memset(key, 0, OD_BOOT_KEY_SIZE);
    return OD_BOOT_KEY_NOT_SET;
}
