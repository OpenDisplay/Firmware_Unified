/* od_advert_device_state_now() -- the live inputs of od_advert_apply_device_state() (od_advert.h).
 * Not in od_advert.c, which is the PURE tier: this reads the security state. Lives in the
 * APP_IDENT tier, which every target composes alongside APP_SESSION. */
#include "od_advert.h"

#include "od_claim_mode.h"
#include "od_sec2.h"

#include <string.h>

void od_advert_device_state_now(struct od_advert_device_state *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof *out);
    out->claim_mode = od_claim_mode_active();
    out->session_required = od_sec2_needs_session();
}
