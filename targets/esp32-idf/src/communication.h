#ifndef COMMUNICATION_H
#define COMMUNICATION_H

#include "od_cmd.h"
#include "od_txq.h"

#include <stdint.h>

uint16_t calculateCRC16CCITT(uint8_t* data, uint32_t len);
uint8_t getFirmwareMajor();
uint8_t getFirmwareMinor();
uint8_t getFirmwarePatch();
const char* getFirmwareShaString();
od_cmd_result_t handleFirmwareVersion(const od_cmd_ctx_t *ctx);
od_cmd_result_t handleReadMSD(const od_cmd_ctx_t *ctx);
od_cmd_result_t handleReadConfig(const od_cmd_ctx_t *ctx);
od_cmd_result_t handleWriteConfig(const od_cmd_ctx_t *ctx, uint8_t* data, uint16_t len);
od_cmd_result_t handleWriteConfigChunk(const od_cmd_ctx_t *ctx, uint8_t* data, uint16_t len);

/* Dispatch one inbound frame and apply its policy.
 *
 * ITS ONLY INPUTS ARE THE FRAME AND WHERE THE ANSWER GOES. The reply context is built by the
 * INGRESS that has it: BLE from the RX slot's own tag, LAN from the live owner word. It used to be
 * reconstructed here from a pair of globals the caller had set immediately beforehand, which is a
 * frame context that outlives its frame -- readable by any nested or later path after the caller
 * has moved on, and impossible for the compiler to check.
 *
 * Returns the outcome so the ingress can honour od_frame_policy().consume_rx: OD_FRAME_DEFERRED
 * means the frame was NOT consumed and must be re-offered unchanged, and an ingress that drops it
 * anyway turns backpressure into silent command loss. */
od_frame_outcome_t od_dispatch_app_frame(const od_reply_t *rp, uint8_t *frame, uint16_t len);

/**
 * Drop a BLE link that has answered OD_AUTH_ABUSE_THRESHOLD consecutive commands
 * with RESP_AUTH_REQUIRED. Loop-serviced, both targets, and it must run on the loop
 * task: it ends in abortToKnownState().
 *
 * Best-effort delivery of the final RESP_AUTH_REQUIRED before the drop -- it drains
 * TX and dwells about one connection interval, both inside a hard bound. An empty
 * ring proves stack acceptance of an unacknowledged notification, not receipt, so a
 * deadline-truncated attempt may forfeit it by design.
 */
void serviceBleAuthAbuseDisconnect(void);

/** Clear the consecutive-rejection run. Called by abortToKnownState() so every
 *  session end resets it -- otherwise a new client inherits its predecessor's
 *  rejections, which is a defect an earlier prototype shipped with. */
void resetAuthAbuseCounter(void);

// --- deferred work, serviced by loop() ---------------------------------------
// Implemented in main.cpp, which owns loop() and the flags behind these. They
// are requests, not commands: the work happens on a later pass, and main.cpp
// decides when it is safe (never mid-EPD-refresh, never while the owning
// transport is still live).
//
// Declared here rather than in ble_transport.h because they are application
// policy, not link state -- and because neither is BLE-specific: the cleanup
// request is raised by the LAN transport on a LAN disconnect.

/// Abort any in-flight transfer and tear down the panel session. Raised on a
/// BLE or LAN disconnect; honoured once no refresh is in flight and the
/// transport that OWNS the transfer is confirmed gone.
void requestTransferSessionCleanup(void);

/// Re-arm BLE advertising when it is safe to. A no-op on targets whose stack
/// re-arms itself (see BleTransport::restartsAdvertisingOnDisconnect), so
/// callers need no target guard.
void requestAdvertisingRestart(void);

/// Raise rebootFlag and refresh the advertised MSD after a config write, so a scanning hub
/// that did not do the write sees the flag and re-reads config instead of trusting a cached
/// copy (stale colour scheme, for one). Runs after the writer's ACK is queued.
void notifyConfigChangedOverBle(void);

#endif
