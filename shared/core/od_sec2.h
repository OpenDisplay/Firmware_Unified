/* od_sec2.h -- SEC2: device key, claim, authenticated sessions, sealed records.
 *
 * The normative text is docs/SECURITY_V2.md (revision 2). This module implements sections 2-6,
 * 8, 9 and 11 for one BLE application connection:
 *   - the key records OS2K / OS2S and the claim evidence part of the boot decision (10.1);
 *   - SEC_STATUS 0x0054, SEC_CLAIM 0x0055, SEC_AUTH 0x0056 (plaintext, fragmented, 3.3);
 *   - SEC_KEY 0x0057 (sealed only);
 *   - the record layer: per-direction CCM keys and counters (9.4, 9.5);
 *   - the handshake rate limiter (8.7).
 * The KDF is od_sec2_kdf.h and the commitment/code od_sec2_code.h, each swappable on its own.
 *
 * NO KEY IN THIS HEADER'S STRUCTS. Record keys live in od_hal_crypto slots 0 (h2d) and 1 (d2h).
 *
 * WHAT IS NOT HERE: claim-mode entry, the gesture and the setup screens (WP3b/WP5, reached
 * through od_sec2_app.h), the config-side rules (WP4: secrets, 0x27 at rest, the boot rows that
 * need the secret scan), factory reset (WP6).
 *
 * CONTEXT: single loop/main task. No locks, no allocation.
 */
#ifndef OD_SEC2_H
#define OD_SEC2_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "od_cmd.h"
#include "od_hal_crypto.h"
#include "od_sec2_wire.h"
#include "od_span.h"
#include "od_txq.h"
#include "opendisplay_structs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ------------------------------------------------------------------------------- sizes --- */

#define OD_SEC2_UID_LEN        8u
#define OD_SEC2_POINT_LEN      64u
#define OD_SEC2_TC_LEN         179u   /* "OD2/claim"(9) 02 dflags uid Qc Qd Nc Nd */
#define OD_SEC2_TA_LEN         178u   /* "OD2/auth"(8)  02 dflags uid Qc Qd Nc Nd */
#define OD_SEC2_BODY_MAX       82u    /* HELLO, the largest fragmented body */
#define OD_SEC2_REC_HDR        4u     /* ctr:4 LE */
#define OD_SEC2_TAG_LEN        OD_HAL_CRYPTO_TAG_LEN
#define OD_SEC2_H2D_MIN        (OD_SEC2_REC_HDR + OD_SEC2_TAG_LEN)          /* 16 after cmd */
#define OD_SEC2_D2H_OVERHEAD   (1u + OD_SEC2_REC_HDR + OD_SEC2_TAG_LEN)     /* 17 */
#define OD_SEC2_SEALED_MAX     (OD_BLE_MAX_FRAME - 3u)                      /* 253 */
#define OD_SEC2_PLAIN_RESP_MAX (OD_SEC2_SEALED_MAX - OD_SEC2_D2H_OVERHEAD)  /* 236 */
#define OD_SEC2_PLAIN_MAX      (OD_SEC2_SEALED_MAX - 2u - OD_SEC2_H2D_MIN)  /* 235, H->D payload */
/* The last usable record counter (section 9.5). OD_SEC2_TEST_CTR_MAX exists only so a host
 * suite can reach exhaustion without four billion seals; no target sets it. */
#ifdef OD_SEC2_TEST_CTR_MAX
#define OD_SEC2_CTR_MAX        OD_SEC2_TEST_CTR_MAX
#else
#define OD_SEC2_CTR_MAX        0xFFFFFFFEu
#endif

#define OD_SEC2_OS2K_LEN       28u
#define OD_SEC2_OS2S_LEN       16u

OD_STATIC_ASSERT(OD_SEC2_TC_LEN == 9u + 1u + 1u + 8u + 64u + 64u + 16u + 16u, "Tc is 179 bytes");
OD_STATIC_ASSERT(OD_SEC2_TA_LEN == OD_SEC2_TC_LEN - 1u, "Ta is one byte shorter than Tc");
OD_STATIC_ASSERT(OD_SEC2_TAG_LEN == 12u, "SEC2 records carry a 12-byte tag");
OD_STATIC_ASSERT(OD_SEC2_PLAIN_RESP_MAX == 236u, "a sealed D->H frame carries 236 bytes");
OD_STATIC_ASSERT(OD_SEC2_STATUS_LEN == 2u + 1u + 1u + 1u + 2u + 8u + 2u + 1u + 1u,
                 "SEC_STATUS is 19 bytes");

/* Timing (section 17). */
#define OD_SEC2_BEGIN_TO_NONCE_MS   30000u
#define OD_SEC2_NONCE_TO_CONFIRM_MS 300000u
#define OD_SEC2_HELLO_TO_FINISH_MS  30000u
#define OD_SEC2_FRAGMENT_GAP_MS     5000u
#define OD_SEC2_RATE_WINDOW_MS      60000u
#define OD_SEC2_RATE_EVENTS         10u
#define OD_SEC2_STRIKES             3u
#define OD_SEC2_DEFAULT_TIMEOUT_S   3600u

/* --------------------------------------------------------------------------- key records --- */

#define OD_SEC2_ORIGIN_CLAIMED 0x01u
#define OD_SEC2_ORIGIN_LEGACY  0x02u

#define OD_SEC2_OS2S_FRESH          0x01u
#define OD_SEC2_OS2S_LEGACY_SECRETS 0x02u
#define OD_SEC2_OS2S_MIGRATED       0x04u

struct od_sec2_key_rec {
    uint8_t  origin;
    uint16_t gen;
    uint8_t  key[16];
};

struct od_sec2_state_rec {
    uint8_t  flags;
    uint8_t  short_boots;
    uint16_t key_gen;
};

void od_sec2_os2k_encode(const struct od_sec2_key_rec *k, uint8_t out[OD_SEC2_OS2K_LEN]);
bool od_sec2_os2k_decode(const uint8_t in[OD_SEC2_OS2K_LEN], struct od_sec2_key_rec *k);
void od_sec2_os2s_encode(const struct od_sec2_state_rec *s, uint8_t out[OD_SEC2_OS2S_LEN]);
bool od_sec2_os2s_decode(const uint8_t in[OD_SEC2_OS2S_LEN], struct od_sec2_state_rec *s);

/* X < p, Y < p, Y^2 = X^3 - 3X + b (section 2). */
bool od_sec2_point_valid(const uint8_t q[64]);

/* ------------------------------------------------------------------------------- state --- */

enum od_sec2_state {
    OD_SEC2_UNCLAIMED = 0,
    OD_SEC2_CLAIMED,
    OD_SEC2_LOCKED
};

/* The boot decision, the rows that need no config scan (section 10.1 rows 1-4 and 8). Reads
 * OS2K and OS2S; `sec` is the 0x27 as stored (NULL when there is none). Row 4 (an unmigrated
 * 2.x key) migrates by writing OS2S and then OS2K. Rows 5-7 (secrets without a key) are WP4 and
 * evaluate as row 8 here. Resets all RAM state. Call once per boot before the radio starts, and
 * from tests to start a case clean. */
enum od_sec2_state od_sec2_boot(const struct SecurityConfig *sec);

enum od_sec2_state od_sec2_state(void);

/* READ_IDENT tail flags bits 3-6 (SEC_V2 always set). */
uint8_t od_sec2_ident_flags(void);

/* MSD status bit 3 on 3.x: CLAIMED or LOCKED. */
bool od_sec2_needs_session(void);

/* A session exists, is within its lifetime (an expired one is torn down here) and belongs to
 * this reply target. */
bool od_sec2_session_live(const od_reply_t *rp);

/* Drop the session, the pending claim / handshake / rotation, reassembly and fragment output.
 * For disconnect and od_core_reset(). */
void od_sec2_disconnect(void);

/* K_lan = KDF(K_dev, "OD2/k-lan", uid), the LAN TLS-PSK. false unless CLAIMED. */
bool od_sec2_k_lan(uint8_t out[16]);

/* K_v1 (section 10.2), the 0x27 key at rest. false unless CLAIMED. */
bool od_sec2_k_v1(uint8_t out[16]);

/* ---------------------------------------------------------------------------- commands --- */

/* 0x0050, 0x0054, 0x0055, 0x0056: always plaintext (section 6 rule 1). Spends one unit of `r`
 * at most; further fragments of a reply leave through od_sec2_pump(). Never ACCEPTED. */
od_frame_outcome_t od_sec2_command(od_tx_reservation_t *r, const od_reply_t *rp, uint16_t cmd,
                                   od_span_t body);

/* 0x0057, reached only through the gate with a verified record. */
od_cmd_result_t od_sec2_key_cmd(const od_cmd_ctx_t *ctx, od_span_t body);

/* Emit the next D->H fragment of a multi-fragment reply, one slot per pass, like
 * od_config_read_pump(). Call from the loop. */
void od_sec2_pump(void);

/* A reply is still being fragmented out. The dispatcher defers further SEC_CLAIM / SEC_AUTH
 * frames while it is. */
bool od_sec2_tx_active(void);

/* -------------------------------------------------------------------------- record layer --- */

enum od_sec2_open {
    OD_SEC2_OPEN_OK = 0,
    OD_SEC2_OPEN_NO_SESSION,       /* FE */
    OD_SEC2_OPEN_RECORD,           /* shorter than a record: sealed F1, no strike */
    OD_SEC2_OPEN_REPLAY,           /* counter 0 / 0xFFFFFFFF / seen / out of window: F2, no strike */
    OD_SEC2_OPEN_BAD_TAG,          /* F3, one strike */
    OD_SEC2_OPEN_BAD_TAG_TEARDOWN, /* the third strike: F3 sealed, then the session goes */
    OD_SEC2_OPEN_CRYPTO_ERROR,     /* engine fault: no strike */
    OD_SEC2_OPEN_NO_ROOM
};

/* Open one H->D record: rec is [ctr:4 LE][C][T:12], the frame after the opcode. out receives
 * the payload (rec.n - 16 bytes); ON ANY NON-OK RESULT out IS UNDEFINED. The counter is checked
 * before decrypt and committed only after the tag verified. */
enum od_sec2_open od_sec2_open(uint16_t cmd, od_span_t rec, uint8_t *out, size_t out_cap,
                               uint16_t *out_len);

enum od_sec2_seal {
    OD_SEC2_SEAL_OK = 0,
    OD_SEC2_SEAL_NO_SESSION,
    OD_SEC2_SEAL_TOO_LONG,         /* above OD_SEC2_PLAIN_RESP_MAX */
    OD_SEC2_SEAL_NO_ROOM,
    OD_SEC2_SEAL_CRYPTO_ERROR,     /* the counter value is spent */
    OD_SEC2_SEAL_EXHAUSTED         /* 0xFFFFFFFE already used: the session is torn down */
};

/* Seal one complete plaintext response frame: out = [E5][ctr:4 LE][C][T:12], frame.n + 17
 * bytes. Capacity is checked before a counter is spent. Sealing under 0xFFFFFFFE tears the
 * session down after the frame is produced. */
enum od_sec2_seal od_sec2_seal(od_span_t frame, uint8_t *out, size_t out_cap, uint16_t *out_len);

/* Tear the session down now (third strike after its NACK, rotation, unclaim). */
void od_sec2_teardown(void);

#ifdef __cplusplus
}
#endif

#endif /* OD_SEC2_H */
