/* od_boot_screen_key() (od_boot_payload.h): what the NORMAL boot screen shows about the key, from
 * the security state and the parsed 0x27 (SECURITY_V2 10.7). Its own translation unit so a host
 * renderer test can define it without linking od_sec2. */
#include "od_boot_payload.h"

#include "od_sec2.h"
#include "od_sec2_app.h"
#include "od_sec2_trace.h"

#include <string.h>

enum od_boot_key_state od_boot_screen_key(uint8_t key[OD_BOOT_KEY_SIZE])
{
    const struct SecurityConfig *sec = od_sec2_app_security();
    const enum od_sec2_state st = od_sec2_state();
    enum od_boot_key_state ks = od_boot_key_policy(st == OD_SEC2_CLAIMED, st == OD_SEC2_LOCKED,
                                                   sec != NULL ? sec->flags : 0u);

    memset(key, 0, OD_BOOT_KEY_SIZE);
    if (ks == OD_BOOT_KEY_SHOWN && !od_sec2_k_dev(key)) {
        memset(key, 0, OD_BOOT_KEY_SIZE);
        ks = OD_BOOT_KEY_HIDDEN;
    }
    OD_SEC2_TRACE("od_boot_screen_key() called: state=%u sec_state=%u flags=0x%02x",
                  (unsigned)ks, (unsigned)st, (unsigned)(sec != NULL ? sec->flags : 0u));
    return ks;
}
