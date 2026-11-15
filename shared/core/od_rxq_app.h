/* od_rxq_app.h -- the two callback-safe predicates the shared RX arrival line needs.
 *
 * The line itself lives in od_rxq.c, one text for every target. These two predicates do not,
 * because neither can be answered from the ring:
 *
 *   - whether session encryption is configured, which decides the ERX/URX token. ESP32 answers
 *     from its own encryption state and Nordic from its parsed security config; there is no
 *     shared symbol that means the same thing on both.
 *   - whether this opcode's per-frame line should be suppressed. od_xfer.c owns and publishes
 *     that policy; target implementations delegate to it so this independently selectable tier
 *     does not acquire a hidden APP_XFER source dependency.
 *
 * DELIBERATELY NOT ROUTED THROUGH od_sec2_app.h. APP_RXQ is an independently selectable
 * source tier (shared/sources.cmake); calling the session seam from od_rxq.c would make it
 * depend on APP_SESSION without saying so. A target that takes APP_RXQ implements these two.
 *
 * BOTH ARE CALLED FROM THE STACK CALLBACK TASK, so neither may block, take a lock the loop
 * holds, or dispatch.
 *
 * ONLY REFERENCED IN A BUILD COMPILED AT OD_LOG_DEBUG -- the arrival line is debug-gated, and at
 * INFO the whole block including these calls is preprocessed away. tests/host's rxq seam fixture
 * is built at DEBUG for exactly that reason: at INFO a missing implementation links fine.
 */

#ifndef OD_RXQ_APP_H
#define OD_RXQ_APP_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* True when the app-layer CCM envelope is in force, i.e. a configured session key exists. This
 * reports the frame's FORM, not its intent: the dispatcher is what rejects an unwrapped frame. */
bool od_rxq_app_encryption_enabled(void);

/* True when this opcode's per-frame arrival line should be suppressed. Mid-stream image data is
 * the bulk of a transfer and would drown the log; the transfer's own progress reporting covers
 * it. */
bool od_rxq_app_quiet(uint16_t cmd);

#ifdef __cplusplus
}
#endif

#endif /* OD_RXQ_APP_H */
