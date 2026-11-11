/* od_hal_radio.cpp -- ESP32's implementation of shared/hal/od_hal_radio.h.
 *
 * One notify, and one liveness question. Everything about WHEN to send, in what order, and what to
 * do about a refusal belongs to od_txq; this file only knows how to hand bytes to NimBLE.
 */

#include "od_hal_radio.h"

#include "od_log.h"
#include "od_txq.h"
#include "od_xfer.h"

#include <stdio.h>

#include "ble_transport.h"
#include "link_owner.h"
#ifdef OPENDISPLAY_HAS_WIFI
#include "wifi_service.h"
#endif

/* THE SINGLE TX LOG LINE, and this is the only place it can honestly live now: every response
 * leaves through this function, so a frame cannot reach the wire without having been logged.
 *
 * The depth is read BEFORE the entry is dequeued, so a healthy path reads [Q:0] and a rising Q
 * flags the drain falling behind the producer. It is not conditioned on the frame having been
 * sealed: od_reply decides that, and the bytes here cannot show it. */
static void logTxFrame(od_origin_t origin, const uint8_t *frame, uint16_t len)
{
    const uint16_t cmd = (len >= 2u) ? (uint16_t)((frame[0] << 8) | frame[1]) : frame[0];
    const char *tag = (origin == OD_ORIGIN_BLE) ? "BLE"
                    : (origin == OD_ORIGIN_LAN_TLS) ? "LAN-TLS" : "LAN";
    char label[64];
    char line[192];

    snprintf(label, sizeof(label), "[%s][Q:%u] TX 0x%04X (%u B): ",
             tag, (unsigned)od_txq_depth(), cmd, (unsigned)len);
    od_log_hex_line(line, sizeof(line), label, frame, len);
    od_log_debug("%s", line);
}

extern "C" od_radio_result_t od_hal_radio_send(od_origin_t origin, uint32_t tag,
                                               const uint8_t *frame, uint16_t len)
{
    if (frame == nullptr || len == 0u) {
        return OD_RADIO_ERROR;
    }
    const uint16_t opcode = len >= 2u ? (uint16_t)((frame[0] << 8) | frame[1]) : frame[0];
    if (len < 2u || !od_xfer_log_quiet(opcode)) {
        logTxFrame(origin, frame, len);
    }
    if (origin != OD_ORIGIN_BLE) {
#ifdef OPENDISPLAY_HAS_WIFI
        /* LAN responses DO come through here: od_reply() routes a TLS-LAN frame to the queue
         * plain (SECTION 9 rule 4 -- TLS already protects it), so the queue must be able to
         * deliver it. Dropping it as a routing error, which an earlier version of this file did,
         * loses every TLS client's response silently: the send succeeds from the core's point of
         * view and nothing ever reaches the socket.
         *
         * The sender reports its own outcome and honours MUST NOT BLOCK -- the accepted socket
         * carries SO_SNDTIMEO -- so this arm forwards the result rather than asserting success.
         * Its RETRY means nothing reached the wire, which is what makes re-offering the same
         * queue head safe; anything that stranded part of a length-prefixed frame comes back GONE
         * with the session already dropped. */
        return opendisplay_lan_send_frame(frame, len);
#else
        /* No LAN transport compiled in, so a LAN-origin frame is genuinely a routing bug. Never
         * retried: retrying a permanent error turns the drain into a spin. */
        return OD_RADIO_ERROR;
#endif
    }
    if (!linkIsOwnerWord(tag)) {
        return OD_RADIO_GONE;
    }
    if (!ble.notifyReady()) {
        /* Connected but not yet subscribed, or mid-teardown. Retryable: the CCCD write may still
         * arrive, and dropping here would lose the first response of every session. */
        return OD_RADIO_RETRY;
    }
    /* BleTransport::notify() returns false for BACKPRESSURE specifically -- its own comment says
     * "retry next pass", not a hard failure -- so it maps to RETRY, never ERROR. Mapping it to
     * ERROR would drop a response every time the stack's TX buffers filled, which under a PIPE
     * upload is routine rather than exceptional. */
    return ble.notify(frame, len) ? OD_RADIO_SENT : OD_RADIO_RETRY;
}

extern "C" bool od_hal_radio_tag_is_live(od_origin_t origin, uint32_t tag)
{
    if (origin != OD_ORIGIN_BLE) {
#ifdef OPENDISPLAY_HAS_WIFI
        /* A LAN frame can now sit in the queue across a RETRY, so the stale-instance window the
         * synchronous sender did not have is real: the peer can go and a new one arrive before the
         * drain, and the old client's response would land on the new client's socket.
         *
         * LAN commands DO carry a real owner word (wifi_service.cpp builds the reply context from
         * linkIdWord(lanOwner)), so that is checkable -- but only when there is one. The same site
         * uses 0 whenever the link owner is not OWNER_LAN, and linkIsOwnerWord(0) is false, so
         * testing unconditionally would discard those responses as GONE. Absent a word, staleness
         * is unprovable and the frame goes; that is the pre-existing behaviour, now confined to
         * the case that actually requires it. */
        return tag == 0u ? true : linkIsOwnerWord(tag);
#else
        return false;
#endif
    }
    /* An instance identity, not a connection handle: handles are reused, so a frame queued by a
     * dead instance must never be delivered to whoever inherited its slot. */
    return linkIsOwnerWord(tag);
}
