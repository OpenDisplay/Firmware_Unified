/* od_sec2_wire.h -- PENDING PROTOCOL VALUES for SEC2 (docs/SECURITY_V2.md, revision 2).
 *
 * *** PROVISIONAL. NOT IN THE CANONICAL HEADER YET. ***
 *
 * shared/protocol/ is a byte copy of ../opendisplay-protocol and is never hand-edited, so every
 * new SEC2 wire value lives here until the protocol owner adopts it. Each #ifndef lets the
 * canonical definition win the day it is synced; then delete the fallback. The full list the
 * protocol repo must receive is in impl_security-fw.md and SECURITY_V2_IMPLEMENTATION_PLAN.md
 * WP0. Nothing here is frozen: the cryptographer items E1-E3 and H11 may still move bytes.
 */
#ifndef OD_SEC2_WIRE_H
#define OD_SEC2_WIRE_H

#include "opendisplay_protocol.h"

/* Opcodes (section 3.1). */
#ifndef CMD_SEC_STATUS
#define CMD_SEC_STATUS                 0x0054u
#endif
#ifndef CMD_SEC_CLAIM
#define CMD_SEC_CLAIM                  0x0055u
#endif
#ifndef CMD_SEC_AUTH
#define CMD_SEC_AUTH                   0x0056u
#endif
#ifndef CMD_SEC_KEY
#define CMD_SEC_KEY                    0x0057u
#endif

/* 0x0050 on 3.0 only refuses (section 16.2). */
#ifndef AUTH_STATUS_V2_REQUIRED
#define AUTH_STATUS_V2_REQUIRED        0x05u
#endif

/* OD_SEC_ERR_* -- one namespace for 0x0054..0x0057 (section 3.2). */
#define OD_SEC_ERR_MALFORMED           0x01u
#define OD_SEC_ERR_BAD_STATE           0x02u
#define OD_SEC_ERR_RATE_LIMITED        0x03u
#define OD_SEC_ERR_NOT_IN_CLAIM_MODE   0x04u
#define OD_SEC_ERR_ALREADY_CLAIMED     0x05u
#define OD_SEC_ERR_NOT_CLAIMED         0x06u
#define OD_SEC_ERR_AUTH_FAILED         0x07u
#define OD_SEC_ERR_EXPIRED             0x08u
#define OD_SEC_ERR_BAD_POINT           0x09u
#define OD_SEC_ERR_CRYPTO_ERROR        0x0Au
#define OD_SEC_ERR_STORAGE_ERROR       0x0Bu
#define OD_SEC_ERR_BUSY                0x0Cu
#define OD_SEC_ERR_ATTEMPTS_EXHAUSTED  0x0Du
#define OD_SEC_ERR_LOCKED              0x0Eu
#define OD_SEC_ERR_WRONG_TRANSPORT     0x0Fu

/* Sealed record NACKs, [FF][cmd_lo][code] inside a sealed D->H frame (section 9.5). 0xF4 is
 * reserved and never sent. */
#define OD_SEC_NACK_RECORD             0xF1u
#define OD_SEC_NACK_REPLAY             0xF2u
#define OD_SEC_NACK_INTEGRITY          0xF3u

/* Dispatcher refusal [FF][cmd_lo][FD]: refused because the device is in claim mode. */
#define OD_GATE_CLAIM_MODE             0xFDu

/* The D->H sealed-frame status byte (section 9.4). */
#define OD_SEC2_SEALED_STATUS          0xE5u

/* CONFIG_WRITE 4g error bytes (section 10.3). Used from WP4 on; listed here so the namespace is
 * reserved in one place. */
#define OD_ERR_CONFIG_SECRET_NEEDS_CLAIM 0x10u
#define OD_ERR_CONFIG_KEY_MISMATCH       0x11u

/* READ_IDENT 3.0 tail flag bits 3-6 (section 5.2). */
#define OD_IDENT_FLAG_SEC_V2           0x08u
#define OD_IDENT_FLAG_CLAIMED          0x10u
#define OD_IDENT_FLAG_CLAIM_MODE       0x20u
#define OD_IDENT_FLAG_LEGACY_SECRETS   0x40u

/* 0x27 reserved[0] on a claimed device at rest (section 10.2). */
#define OD_SEC27_AT_REST_SEC2          0x02u

/* SEC_STATUS (section 5.1). */
#define OD_SEC2_VERSION                0x02u
#define OD_SEC2_STATUS_LEN             19u

#define OD_SEC2_STATE_UNCLAIMED        0u
#define OD_SEC2_STATE_CLAIM_PENDING    1u
#define OD_SEC2_STATE_CLAIMED          2u
#define OD_SEC2_STATE_LOCKED           3u

#define OD_SEC2_SFLAG_CLAIM_MODE       0x01u
#define OD_SEC2_SFLAG_SESSION_ACTIVE   0x02u
#define OD_SEC2_SFLAG_RESET_PENDING    0x04u
#define OD_SEC2_SFLAG_LEGACY_SECRETS   0x08u
#define OD_SEC2_SFLAG_ROTATION_SUGGESTED 0x10u
#define OD_SEC2_SFLAG_CLAIM_MODE_FRESH 0x20u
#define OD_SEC2_SFLAG_CONFIG_LOCKED    0x40u

/* dflags (section 4.4). */
#define OD_SEC2_DFLAG_HAS_PANEL        0x01u
#define OD_SEC2_DFLAG_HAS_RESET_PIN    0x02u
#define OD_SEC2_DFLAG_KEY_LEGACY       0x04u
#define OD_SEC2_DFLAG_CLAIM_MODE_FRESH 0x08u

/* Handshake subs (sections 8.1, 9.2, 11). */
#define OD_SEC2_CLAIM_BEGIN            0x01u
#define OD_SEC2_CLAIM_NONCE            0x02u
#define OD_SEC2_CLAIM_CONFIRM          0x03u
#define OD_SEC2_CLAIM_ABORT            0x04u
#define OD_SEC2_CLAIM_EXIT             0x05u
#define OD_SEC2_AUTH_HELLO             0x01u
#define OD_SEC2_AUTH_FINISH            0x02u
#define OD_SEC2_KEY_ROTATE             0x01u
#define OD_SEC2_KEY_ROTATE_CONFIRM     0x02u
#define OD_SEC2_KEY_UNCLAIM            0x03u

#endif /* OD_SEC2_WIRE_H */
