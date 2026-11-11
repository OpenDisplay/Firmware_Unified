/* od_cmd.c -- the outcome policy table from the dispatch plan section 5, as data.
 *
 * A switch per target is how the two of them drifted: ESP32 returned early from AUTHENTICATE and
 * FIRMWARE_VERSION so those never reached its activity block, which made "handshake is not
 * activity" true by control flow rather than by decision -- and invisible to anyone reading for
 * it. Written down here, each row is arguable and testable.
 */

#include "od_cmd.h"

od_frame_policy_t od_frame_policy(od_frame_outcome_t outcome)
{
    od_frame_policy_t p;

    p.stamp_activity = false;
    p.reset_abuse = false;
    p.increment_abuse = false;
    p.consume_rx = true;

    switch (outcome) {
    case OD_FRAME_ACCEPTED:
    case OD_FRAME_HANDLER_NACK:
        /* A HANDLER NACK IS STILL ACTIVITY. The client is talking to us correctly and got a real
         * answer; refusing to stamp would age out a peer that is working normally but happens to
         * be sending commands this device rejects. */
        p.stamp_activity = true;
        p.reset_abuse = true;
        break;

    case OD_FRAME_AUTH_ESTABLISHED:
        /* Clears the refusal run -- including a drop already pending, because a client that just
         * authenticated should not be dropped by a decision taken moments earlier. Still does NOT
         * stamp: authenticating is not work worth holding the exclusive link for. */
        p.reset_abuse = true;
        break;

    case OD_FRAME_AUTH_CONTROL:
    case OD_FRAME_DISCOVERY:
        /* Neither stamps. A version poll or a handshake retry must not hold the link, which is the
         * whole reason these are separate outcomes rather than ACCEPTED. */
        break;

    case OD_FRAME_AUTH_REQUIRED:
        /* The ONLY outcome that advances the run. A peer that cannot authenticate stops holding
         * the exclusive slot while it retries. */
        p.increment_abuse = true;
        break;

    case OD_FRAME_CRYPTO_FAILED:
    case OD_FRAME_CRYPTO_DROPPED:
        /* Neither touches the run. od_sec2 already applied its own strike policy -- only a bad
         * tag counts there -- and adding a link-level count would punish the same frame twice, or
         * punish ordinary packet loss. */
        break;

    case OD_FRAME_REJECTED_FRAME:
    case OD_FRAME_UNKNOWN_OPCODE:
    case OD_FRAME_STALE_TAG:
        break;

    case OD_FRAME_DEFERRED:
        /* THE ONE OUTCOME THAT DOES NOT CONSUME THE FRAME. It was never dispatched, so it is
         * neither activity nor abuse, and it must be re-offered byte-identical. */
        p.consume_rx = false;
        break;
    }
    /* No default, so -Wswitch fails the build when an outcome is added without a policy. */
    return p;
}
