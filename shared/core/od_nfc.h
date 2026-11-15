/* od_nfc.h -- the shared CMD_NFC_ENDPOINT (0x0083) machine.
 *
 * One state machine for both capability arms. Targets supply only the tag itself, through
 * od_nfc_app.h; everything on the wire -- sub-command parsing, record-type validation, length
 * bounds, chunk assembly, ownership, error codes and reply framing -- is here.
 *
 * ALL THREE SYMBOLS EXIST UNDER EITHER ARM. od_core_reset() names the reset, dispatch names the
 * frame entry point, and target adapters name the lifecycle reporter, so a capability-off target
 * must still link them. What OD_CAP_NFC=0 removes is state, formatting and seam references, not
 * the surface.
 */
#ifndef OD_NFC_H
#define OD_NFC_H

#include "od_caps.h"
#include "od_cmd.h"
#include "od_sec2.h"
#include "od_span.h"
#include "opendisplay_protocol.h"
#include "opendisplay_structs.h"

/* The read response is [status][cmd][0x80][rec_type][len:2][data]. 218 is wire-visible and stays
 * what 2.x sent; the assert keeps it inside one sealed SEC2 D->H frame. */
#define OD_NFC_READ_MAX ((uint16_t)218u)
OD_STATIC_ASSERT(2u + 4u + 218u <= OD_SEC2_PLAIN_RESP_MAX, "an NFC read fits one sealed frame");

/* A WIRE CONSTANT, not a target tunable. The canonical header documents NFC_ERR_BAD_TOTAL_LEN as
 * "total_len == 0 or > 512" and the client enforces the same number, so unlike OD_CONFIG_MAX_SIZE
 * this one may not become per-target: a cap a host cannot interrogate is a divergence it cannot
 * work around. */
#define OD_NFC_ASSEMBLY_MAX 512u

/* Target adapters detect hardware/configuration outcomes; shared owns their diagnostic meaning. */
enum od_nfc_log_event {
    OD_NFC_LOG_PAYLOAD_SET_FAILED = 0,
    OD_NFC_LOG_EMULATION_START_FAILED,
    OD_NFC_LOG_CONFIG_ABSENT,
    OD_NFC_LOG_CONFIG_DISABLED,
    OD_NFC_LOG_IC_UNSUPPORTED,
    OD_NFC_LOG_T2T_SETUP_FAILED,
    OD_NFC_LOG_T2T_ACTIVE
};

/* Both constants are asserted in od_nfc.c, not here. Under -std=c99 OD_STATIC_ASSERT falls back to
 * a typedef named only by __LINE__, so two headers carrying an assert on the same line collide in
 * any translation unit including both -- and the macro lives in a byte-for-byte copy of the
 * canonical protocol header, which is not ours to change. */

#ifdef __cplusplus
extern "C" {
#endif

/* Handle one 0x0083 frame. `body` is the payload after the two command bytes. */
od_cmd_result_t od_nfc_frame(const od_cmd_ctx_t *ctx, od_span_t body);

/* Drop any partial assembly. Called by od_core_reset() as part of a teardown, and a no-op under
 * OD_CAP_NFC=0. */
void od_nfc_reset(void);

/* Report a target-detected NFC lifecycle event. `detail` is the driver result, IC type or
 * advertising-byte index named by `event`. This is a no-op under OD_CAP_NFC=0. */
void od_nfc_log_event(enum od_nfc_log_event event, int detail);

#ifdef __cplusplus
}
#endif

#endif /* OD_NFC_H */
