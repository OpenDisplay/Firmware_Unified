#ifndef OD_RUNTIME_TYPES_H
#define OD_RUNTIME_TYPES_H

/* Firmware-local RAM-only types, plus the canonical wire contract they are built from.
 *
 * WHY THIS FILE EXISTS. shared/protocol/opendisplay_structs.h is the single source of truth for
 * every config-packet struct, and it deliberately carries NO in-memory types -- its own header
 * says so: "NO repo-specific values (GPIO pin values, buffer sizes, GlobalConfig,
 * EncryptionSession, image-transfer state, ButtonState). Those move to a repo-local header
 * on adoption." This is that header for targets/nordic-zephyr.
 *
 * It replaces src/opendisplay_structs.h, which was a hand-written 319-line SUBSET of the
 * canonical 1242-line contract: 14 wire structs re-declared by hand, plus these two RAM-only
 * aggregates, plus unprefixed spellings of flag macros the canonical header carries with an OD_
 * prefix. Every one of those re-declarations was a copy that could drift silently -- and three
 * had: FlashConfig gained miso_pin/wp_pin/hold_pin names for bytes canonical still reserves,
 * DisplayConfig kept tag_type where canonical renamed it legacy_tag_type, and BinaryInputs kept
 * reserved_pin_N where canonical named them input_pin_N.
 *
 * The adoption was verified, not assumed: all 14 shared structs compile to identical sizes, and
 * all 150 common fields to identical offsets and sizes, under both headers. The only offset
 * differences were the reserved[] blocks that necessarily shrank as canonical named bytes out
 * of them -- a backward-compatible carve, per that header's own MINOR-bump policy.
 *
 * NAMING. targets/esp32-idf calls its equivalent src/structs.h. This one is not called that
 * because this target's include path also carries third_party/ and the panel and uzlib
 * directories, where a header named structs.h is a collision waiting to happen; the od_ prefix
 * matches the rest of this target's local headers.
 */

#include <stdbool.h>
#include <stdint.h>

/* The canonical wire contract: config + message payload structs, OD_-prefixed enums and flag
 * macros, and (transitively) opendisplay_protocol.h framing constants -- CMD_*, RESP_*, PIPE_*,
 * and the config limits. Resolved from shared/protocol via OD_SHARED_INCLUDE_DIRS. */
#include "opendisplay_structs.h"

/* The parsed config aggregate -- `struct od_config`, the instance caps, and the storage rules --
 * is shared/core/od_config.h. This file carried a copy of it, field for field, alongside the
 * copies in targets/esp32-idf and targets/efr32bg22-slc. It also brings the security packet
 * inside the aggregate, which is where the zero-key normalisation now lives. */
#include "od_config.h"

/* ------------------------------------------------------------------- RAM-only aggregates --- */

/* Session state lives in shared/core/od_session.h (struct od_session). The session key is not
 * in it at all -- it lives in an od_hal_crypto slot -- so the struct stays trivially zeroable.
 * Never memset one: od_session_clear() is the only teardown, because it also releases the slot. */

#endif /* OD_RUNTIME_TYPES_H */
