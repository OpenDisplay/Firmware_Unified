// The shared session-teardown routine and the R3a link-down wait.
// See session_guard.h for the caller set and CONNECTION_POLICY R3a/R6 for the rules.

// OD-PORT: <Arduino.h> in the reference tree, for millis() and delay(). Both come from
// od_hal_time now, so this file is not one of the files the shim ratchet counts
// (the shim was a demolition schedule, not a portability layer), and it
// names no vendor header at all. That matters beyond the ratchet: this file is a shared/core
// candidate, and shared/ may call od_hal_* but not esp_* or freertos/*.
#include "od_spi_raw_app.h"
#include "od_hal_time.h"
#include "od_hal_sleep.h"

#include "session_guard.h"

#include "od_config_read.h"
#include "od_txq.h"
#include "ble_transport.h"
#include "communication.h"
#include "od_core.h"
#include "od_rxq.h"
#include "od_xfer.h"
#include "config_parser.h"
#include "display_service.h"
#include "encryption.h"
#include "link_owner.h"
#include "od_log.h"
#include "structs.h"
#include "touch_input.h"
#ifdef OPENDISPLAY_HAS_WIFI
#include "wifi_service.h"
#endif


// How long to wait for a requested drop to actually take the link down.
//
// Sized against CLIENT BEHAVIOUR, not against a supervision timeout: an alive peer
// terminates within a few connection intervals -- tens of ms, since this firmware
// requests no interval and the central's negotiated value applies. A peer already
// gone is reaped by the link layer at ~4-6 s, far outside this bound, and that is
// deliberate: expiry here is not a failure needing recovery, just an early exit
// into an abort that runs regardless (R3a). This is the least load-bearing
// threshold in the freeze-hardening plan.
#ifndef OD_BLE_LINK_DOWN_WAIT_MS
#define OD_BLE_LINK_DOWN_WAIT_MS 150
#endif

// Tick granularity for the wait below. Deliberately a plain delay() rather than
// idleDelay(): see bleDropAndWait().
#ifndef OD_BLE_LINK_DOWN_POLL_MS
#define OD_BLE_LINK_DOWN_POLL_MS 2
#endif

bool bleDropAndWait(uint16_t handle, uint16_t epoch) {
    if (!ble.instanceLive(handle, epoch)) return true;   // already down
    ble.disconnect(handle, epoch);
    const uint32_t deadline = od_hal_uptime_ms() + OD_BLE_LINK_DOWN_WAIT_MS;
    while ((int32_t)(od_hal_uptime_ms() - deadline) < 0) {
        // Per-handle, so a refused contender still attached cannot mask the owner's
        // departure (the aggregate count would read 1 and never reach 0).
        if (!ble.instanceLive(handle, epoch)) return true;
        // A plain delay, NOT idleDelay(). idleDelay() early-outs on
        // ble.eventPending(), and mid-teardown there is always an event pending --
        // the owner's own disconnect, deliberately left unconsumed so
        // serviceBleEvents() still sees it -- so every call would return instantly
        // and this loop would degrade into a busy spin for its full bound. A plain
        // tick also services neither RX nor transport events, which is precisely the
        // safety property wanted here.
        //
        // Through the HAL rather than a raw vTaskDelay(pdMS_TO_TICKS(2)), which is what this
        // was until 2026-08-04. At the 100 Hz tick this target had inherited, that expression
        // is ZERO ticks -- vTaskDelay(0) yields without blocking -- so this loop busy-spun at
        // loop-task priority through every teardown, for its full bound. Restoring the 1000 Hz
        // tick makes the raw form correct again; od_hal_delay_ms() makes it stay correct, since
        // it rounds up and never to zero.
        od_hal_delay_ms(OD_BLE_LINK_DOWN_POLL_MS);
    }
    const bool down = !ble.instanceLive(handle, epoch);
    if (!down) {
        od_log_warn("Link-down wait expired for h=%u (%u ms); proceeding, stale link is inert",
                    (unsigned)handle, (unsigned)OD_BLE_LINK_DOWN_WAIT_MS);
    }
    return down;
}


void abortToKnownState(const char* reason, bool dropLink, LinkId ownerId) {
    // 1. Log first, one line, so a teardown is always attributable even if a later
    //    step wedges.
    od_log_info("[abort] %s (dropLink=%d owner=%u/h%u/e%u)", reason ? reason : "?",
                dropLink ? 1 : 0, (unsigned)ownerId.who, (unsigned)ownerId.handle,
                (unsigned)ownerId.epoch);

    // 2. Optional client NACK -- deliberately NOT implemented, and the plan's step
    //    list should be read with this note. It is specified as "skip when
    //    dropLink", and every caller either drops the link or is called because the
    //    link is ALREADY gone (disconnect cleanup) or about to be (deep sleep). So
    //    no caller is in a position to deliver one, and adding a NACK nobody can
    //    receive would only add a failure mode. Revisit only if a future caller
    //    aborts a session while intending to keep the link up.
    //
    //    Note the asymmetry with Phase 4's auth-abuse drop, which must DELIVER its
    //    final FE -- it runs its own bounded TX barrier BEFORE calling this, rather
    //    than asking the abort to hold the link open. Two contradictory jobs in one
    //    routine is what that split avoids.

    // 3. Config chunked upload -- previously had no reset function at all, only
    //    open-coded inline assignments, so no teardown or watchdog touched it.
    resetChunkedWriteState();

    // 4. Touch: assert the suspend counter reaches 0 even when teardown bypasses the ordinary
    //    completion path, so a partial-path teardown cannot leave touch suspended forever.
    touchForceResume();

    // 5. The auth-abuse run. Every session end clears it, so a new client can never inherit its
    //    predecessor's rejections.
    resetAuthAbuseCounter();

    // 6. Both rings. R6 requires RX and TX drained of the departed session's traffic;
    //    an earlier draft flushed TX only, which left the teardown window open.
    //    Draining RX is sound because callback filtering means every frame in it
    //    passed the owner check when written. A frame the owner writes after this, during step 8's
    //    wait, is deliberately not re-flushed: it carries the departing instance's tag and fails
    //    the dispatch check once step 9 releases.
    const uint8_t droppedRx = od_rxq_reset();
    if (droppedRx > 0) {
        od_log_warn("[abort] dropped %u queued command(s) from the departed session",
                    (unsigned)droppedRx);
    }

    // 7. Every shared object that outlives a dispatch, in one call: transfer state, the
    //    config-read producer, egress, and the session. Cancelling the read is not tidiness -- it holds the config
    //    scratch, and od_dispatch DEFERS every config-mutating opcode while it is active, so a
    //    client that vanishes mid-read would otherwise defer every later config write forever.
    //    The session clear belongs here; nothing between the RX reset and this call reads it,
    //    and it is NEW on the disconnect path either way -- crypto state used to survive a BLE
    //    link drop entirely, cleared on session-timeout-at-command, a new auth, config reload and
    //    LAN teardown, but never on disconnect.
    //
    //    RX is NOT in it, deliberately: od_core_reset() cannot own a ring whose producer differs
    //    per target, and this target's connection policy has stricter ordering around its own --
    //    hence step 6 above, where the drop count is also worth reporting.
    od_core_reset();
    // A raw SPI session is not a transfer, so od_core_reset() does not end it. The protocol
    // says a disconnect force-closes it.
    opendisplay_display_spi_raw_close();

    // 8. The drop, dispatched on the OWNER'S TRANSPORT. This routine is not
    //     BLE-only: the transfer watchdog that calls it is origin-agnostic, so
    //     dropping a BLE handle for a timed-out LAN owner would leave the owning
    //     socket alive while its token was released -- an R1 violation.
    if (dropLink) {
        if (ownerId.who == OWNER_BLE) {
            (void)bleDropAndWait(ownerId.handle, ownerId.epoch);
        } else if (ownerId.who == OWNER_LAN) {
#ifdef OPENDISPLAY_HAS_WIFI
            // A TCP close is synchronous, so no wait bound applies on LAN -- the
            // asynchrony R3a exists for is BLE-only.
            wifiLanDropOwnedSocket();
#endif
        }
    }

    // 9. Release, STRICTLY after step 8. Releasing at request time would let a new connection be
    //    admitted while the old link was still physically up. If the wait expired the release
    //    still happens -- the stale link is inert by construction.
    //
    //     For the terminal caller this is naturally inert: the word was exchanged to
    //     OWNER_TERMINAL before the abort, so this full-identity CAS does not match
    //     and the admission gate stays shut.
    linkRelease(ownerId);
}

void abortToKnownState(const char* reason, bool dropLink) {
    abortToKnownState(reason, dropLink, linkOwnerId());
}
