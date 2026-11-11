/* od_sec2_pending.c -- the od_sec2_app.h hooks owned by later work packages, as "not there yet".
 *
 * Claim mode (WP3b) is never active, so an unclaimed device answers SEC_CLAIM NOT_IN_CLAIM_MODE
 * and cannot be claimed on this build. The scrub (WP4) reports failure, so UNCLAIM answers
 * STORAGE_ERROR and keeps the key. The 0x27 rewrite at rest (WP4) does nothing; boot row 2
 * repairs it once it exists. Each function moves to its owning module when that lands.
 */
#include "od_sec2_app.h"

#include <string.h>

void od_sec2_app_claim_mode(struct od_sec2_claim_mode *out)
{
    memset(out, 0, sizeof *out);
}

void od_sec2_app_claim_event(enum od_sec2_claim_event ev, uint32_t arg)
{
    (void)ev;
    (void)arg;
}

bool od_sec2_app_scrub(void)
{
    return false;
}

void od_sec2_app_rest_sync(void)
{
}
