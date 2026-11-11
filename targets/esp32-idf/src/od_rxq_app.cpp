/* od_rxq_app.cpp -- this target's answers to shared/core/od_rxq_app.h.
 *
 * The arrival and drop lines are no longer here: od_rxq.c logs them, one wording for both
 * targets. What is left is the two facts the shared line cannot answer from the ring -- whether
 * the CCM envelope is in force, and whether an opcode's per-frame line should be suppressed.
 *
 * Both run on the STACK CALLBACK TASK and are only reached in a build compiled at OD_LOG_DEBUG.
 */

#include "od_rxq_app.h"

#include "od_sec2.h"
#include "od_xfer.h"

extern "C" bool od_rxq_app_encryption_enabled(void)
{
    return od_sec2_needs_session();
}

extern "C" bool od_rxq_app_quiet(uint16_t cmd)
{
    return od_xfer_log_quiet(cmd);
}
