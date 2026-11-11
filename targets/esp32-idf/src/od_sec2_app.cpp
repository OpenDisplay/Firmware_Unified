/* od_session_app.cpp -- this target's half of the session seam (shared/core/od_session_app.h).
 *
 * Four accessors and two seam stubs. The accessors exist because shared egress and the dispatcher
 * need the session, the config, the clock and the device id, and none of the four can be shared.
 * The session's own wording is not here: od_session.c logs its outcomes directly, and this
 * target's implementation of the report callback is empty. See the note above it.
 */

#include "od_session_app.h"
#include "od_txq.h"   /* od_reply_t / od_radio_result_t, for the drop seam at the end */

#include "encryption_state.h"
#include "od_hal_time.h"

#include "opendisplay_protocol.h"

/* C++ linkage, matching its definition in encryption.cpp -- this is a target function, not part
 * of any C ABI. It was declared extern "C" here and linked anyway only because nothing referenced
 * od_session_app_device_id() until the dispatcher did: --gc-sections dropped the section and the
 * mismatch with it. */
void getAuthDeviceIdBytes(uint8_t* device_id);

/* THIS TARGET'S ONE SESSION, and it is file-static so that it is reachable only through the seam
 * below. An exported singleton is what lets a caller memset it, and memset is not teardown here:
 * the key lives in an od_hal_crypto slot, so clearing the struct by hand strands a prepared key in
 * a finite pool and loses the slot index with it. od_session_clear() is the only teardown.
 *
 * Zero-init == od_session_init(&s_session, 0). */
static struct od_session s_session;

struct od_session *od_session_app_state(void)
{
    return &s_session;
}

const struct SecurityConfig *od_session_app_security(void)
{
    return &securityConfig;
}

uint32_t od_session_app_now_ms(void)
{
    return od_hal_uptime_ms();
}

void od_session_app_device_id(uint8_t out[OD_SESSION_DEVICE_ID_LEN])
{
    getAuthDeviceIdBytes(out);
}

/* THE SESSION'S OWN WORDING NOW LIVES IN shared/core/od_session.c, which logs each outcome at
 * the point it decides it -- one text for every target instead of one per target, which is how
 * Nordic came to be missing the NOT_CONFIGURED and EXPIRED cases entirely. The five-second
 * nonce-rejection throttle moved with it.
 *
 * The callback itself stays because od_gate.c and od_reply.c still call it and BG22 implements
 * it with printf, which is that target's only auth and decrypt diagnostic. Removing the call
 * would strand BG22; implementing anything here would duplicate the shared line. */
void od_session_app_report(enum od_session_app_op op, int result, uint16_t cmd,
                           const struct od_session_report *report)
{
    (void)op;
    (void)result;
    (void)cmd;
    (void)report;
}

/* od_txq's drop seam, empty here. od_txq.c logs the discard itself, one wording for both
 * targets, at INFO because a normal disconnect mid-upload discards every frame still queued for
 * that link. The callback survives for BG22, whose printf is its only diagnostic for the event. */
extern "C" void od_txq_app_dropped(const od_reply_t *rp, uint16_t len, od_radio_result_t why)
{
    (void)rp;
    (void)len;
    (void)why;
}
