/* od_sec2_hooks.c -- the od_sec2_app.h hooks that shared modules answer, not the target.
 *
 * A separate translation unit on purpose: a host test that supplies its own claim-mode or scrub
 * hooks links od_claim_mode.o and od_cfg_policy.o for their other symbols without a duplicate
 * definition, because nothing it needs lives in this object.
 */
#include "od_cfg_policy.h"
#include "od_claim_mode.h"
#include "od_sec2_app.h"

void od_sec2_app_claim_mode(struct od_sec2_claim_mode *out)
{
    od_claim_mode_status(out);
}

void od_sec2_app_claim_event(enum od_sec2_claim_event ev, uint32_t arg)
{
    od_claim_mode_event(ev, arg);
}

bool od_sec2_app_scrub(void)
{
    return od_cfg_policy_scrub();
}

void od_sec2_app_rest_sync(void)
{
    od_cfg_policy_rest_sync();
}
