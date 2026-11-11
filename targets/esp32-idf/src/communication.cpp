#include "od_smp.h"
#include "communication.h"
#include "od_display_esp.h"
#include "structs.h"
#include "config_parser.h"
#include "encryption.h"
#include "device_control.h"
#include "buzzer_control.h"
#include "display_service.h"
#include "od_log.h"

#include "od_hal_time.h"
#include <stdio.h>
#include <string.h>

#include "ble_transport.h"
#include "od_rxq.h"

// wifi_service.h only, deliberately: this file talks to the LAN transport
// through opendisplay_lan_send_frame() and needs no WiFi stack types. <WiFi.h>
// was here solely for two extern declarations that nothing used.
#include "wifi_service.h"

/* Chunked CONFIG_WRITE reassembly, promoted to shared/core (F3). */
#include "od_config_asm.h"
#include "od_config_store.h"
#include "od_config_read.h"
#include "od_dispatch.h"   /* od_dispatch_budget, for the migration's legacy reservations */
#include "od_cmd_reply.h"

#include "od_sec2.h"
#include "od_span.h"
#include "od_xfer.h"
#include "encryption_state.h"

#include "link_owner.h"
#include "session_guard.h"

extern struct od_config globalConfig;

/* The chunked CONFIG_WRITE transfer in progress, if any. Replaces chunkedWriteState. Loop-task
 * only, like every other dispatcher-adjacent object in this file. */
struct od_config_asm g_configAsm;

/* Transport tag for the log lines below. Takes the origin rather than reading one: three
 * transports share this dispatcher, and a line that cannot say which one a frame took -- in
 * particular whether it used the TLS CCM-bypass path -- is a line that cannot be acted on. */
static const char* originTag(od_origin_t origin) {
    switch (origin) {
        case OD_ORIGIN_LAN_TLS:   return "LAN-TLS";
        case OD_ORIGIN_LAN_PLAIN: return "LAN";
        default:                  return "BLE";
    }
}

// --- auth-abuse drop (CONNECTION_POLICY R3 / freeze-hardening Phase 4) ---------
//
// Count CONSECUTIVE commands answered RESP_AUTH_REQUIRED and drop the link at the
// threshold, so a session that cannot authenticate stops holding the exclusive slot
// while it retries.
//
// This is an OPTIMISATION, not a hole-closer -- the distinction matters for how
// hard it should try. Phase 3 narrowed the activity clock so handshake/discovery
// opcodes and pre-auth commands no longer stamp it, which means such a peer already
// ages normally and the idle timeout reclaims the slot at OD_BLE_IDLE_TIMEOUT_MS.
// What this adds is speed and a reason: roughly one exchange instead of 120 s, and
// an explicit final RESP_AUTH_REQUIRED before a deliberate drop rather than a
// silent timeout. So it may fail safe (never dropping) without reopening anything.
#ifndef OD_AUTH_ABUSE_THRESHOLD
// CLIENT BEHAVIOUR THIS ASSUMES: py-opendisplay authenticates in ONE exchange, so a
// legitimate client never reaches 2, let alone 10.
//
// Chosen deliberately BELOW py-opendisplay's 16-frame pipe window, which is the one
// case where a well-behaved client trips it: if its session dies mid-upload, every
// in-flight frame bounces. Dropping at 10 rather than waiting out all 16 is the
// right outcome -- the session is already dead, every one of those frames is doomed,
// and the drop tells the client immediately instead of after a full window of
// pointless round trips. Recorded because an earlier prototype inherited this
// threshold by accident rather than deciding it.
#define OD_AUTH_ABUSE_THRESHOLD 10
#endif
#ifndef OD_AUTH_ABUSE_FLUSH_MS
// Hard bound on the whole best-effort delivery attempt below. On expiry the drop
// happens regardless, so a client that has stopped reading cannot keep the abuser
// attached by refusing to drain.
#define OD_AUTH_ABUSE_FLUSH_MS 500
#endif
#ifndef OD_AUTH_ABUSE_DWELL_FALLBACK_MS
// Used when the negotiated connection interval is not yet known. The central
// chooses that interval and this firmware requests none, so there is no constant to
// hard-code -- see BleTransport::connIntervalMs().
#define OD_AUTH_ABUSE_DWELL_FALLBACK_MS 50
#endif

static uint8_t  s_authRejectRun = 0;        // consecutive RESP_AUTH_REQUIRED answers
static bool     s_authAbuseDropPending = false;
static uint32_t s_authAbuseDeadlineMs = 0;  // hard bound on the delivery attempt
static uint32_t s_authAbuseDwellUntil = 0;  // set once TX has drained; 0 = not yet

// Advance the consecutive-refusal run by one. Called ONLY from od_core_frame_done(), on
// OD_FRAME_AUTH_REQUIRED, which has already applied the BLE-only and still-the-owner scoping --
// so neither test is repeated here.
//
// One frame, one advance. The handlers that answer RESP_AUTH_REQUIRED used to call this
// themselves AND return OD_CMD_AUTH_REJECTED; keeping both would count the same refusal twice and
// drop a link at half the configured threshold.
static void authAbuseAdvance(void) {
    if (s_authAbuseDropPending) return;              // already decided
    if (s_authRejectRun < 255) s_authRejectRun++;
    if (s_authRejectRun < OD_AUTH_ABUSE_THRESHOLD) return;
    od_log_warn("Auth abuse: %u consecutive unauthenticated commands - dropping link",
                (unsigned)s_authRejectRun);
    s_authAbuseDropPending = true;
    s_authAbuseDeadlineMs = od_hal_uptime_ms() + OD_AUTH_ABUSE_FLUSH_MS;
    s_authAbuseDwellUntil = 0;
}

void resetAuthAbuseCounter(void) {
    s_authRejectRun = 0;
    s_authAbuseDropPending = false;
    s_authAbuseDeadlineMs = 0;
    s_authAbuseDwellUntil = 0;
}

void serviceBleAuthAbuseDisconnect(void) {
    if (!s_authAbuseDropPending) return;
    // Never mid-refresh: loop() is blocked throughout one, and the abort is
    // loop-task-only by contract.
    if (epdRefreshInProgress) return;

    const LinkId owner = linkOwnerId();
    if (owner.who != OWNER_BLE) {
        // The link went away, or LAN took the slot, while we were draining. Nothing
        // to drop -- and dropping on a stale identity is exactly what the epoch
        // exists to prevent.
        resetAuthAbuseCounter();
        return;
    }

    // BEST EFFORT, and deliberately not more than that. An empty TX ring proves the
    // stack ACCEPTED the notification, not that it went on air: the ring advances
    // when notify() returns true, and a BLE notification is unacknowledged. Without
    // an indication -- a wire change this plan forbids -- there is no delivery
    // signal to wait on, so this drains, dwells about one connection interval to
    // give the radio a chance to send, and then drops.
    (void)od_txq_process();
    const bool expired = (int32_t)(od_hal_uptime_ms() - s_authAbuseDeadlineMs) >= 0;
    if (od_txq_depth() == 0u && s_authAbuseDwellUntil == 0) {
        uint16_t intervalMs = ble.connIntervalMs(owner.handle);
        if (intervalMs == 0) intervalMs = OD_AUTH_ABUSE_DWELL_FALLBACK_MS;
        const uint32_t dwellEnd = od_hal_uptime_ms() + intervalMs + 5u;   // +margin
        // Never past the hard deadline: a drain landing just before it yields a
        // short or zero dwell, which is the expiry case behaving as specified
        // rather than a contradiction.
        s_authAbuseDwellUntil =
            ((int32_t)(dwellEnd - s_authAbuseDeadlineMs) > 0) ? s_authAbuseDeadlineMs : dwellEnd;
    }
    const bool dwelled = (s_authAbuseDwellUntil != 0) &&
                         ((int32_t)(od_hal_uptime_ms() - s_authAbuseDwellUntil) >= 0);
    if (!expired && !dwelled) return;   // keep draining next pass

    // One more pass if RX still holds frames. serviceBleRx() drains once per pass,
    // early, while this runs late -- so a frame that arrived on the callback task in
    // between is still queued, and the abort's ring reset would discard it unread.
    // That frame may be the client's authentication, which would cancel this drop
    // entirely. Bounded by the same hard deadline, so a client that keeps the ring
    // permanently non-empty cannot defer the drop indefinitely.
    if (!expired && od_rxq_pending()) return;

    resetAuthAbuseCounter();
    // dropLink=true. The abort's own step 10 is the R3a bounded wait for link-down
    // before its step 11 releases -- two bounded waits in sequence, composing rather
    // than conflicting: this one runs BEFORE the abort precisely because the abort
    // deliberately skips the client NACK when dropping, and asking one routine to
    // both hold the link open for a response and tear it down is contradictory.
    abortToKnownState("auth abuse", true, owner);
}

static void reloadConfigAfterSave(void) {
    if (!loadGlobalConfig()) {
        od_log_warn("Config was saved but reload from storage failed (see errors above). "
                    "Reboot may be required.");
        return;
    }
    od_log_info("Config reloaded from storage after save");
    // A warm panel was brought up under the old config, whose pins and drive packets may no
    // longer describe it: power it down now rather than at the stale deadline.
    if (epdSessionIsWarm()) {
        epdSessionForceOff();
    }
    // A boot screen skipped for want of drive packets is shown once a config brings them.
    displayRequestBootAfterConfigWrite();
#ifdef OPENDISPLAY_HAS_WIFI
    // Non-blocking: a config write arrives over a live BLE link, and the blocking
    // form stalls the loop task for up to 36 s of connect retries, freezing BLE
    // command processing. handleWiFiServer() starts the LAN server on association.
    initWiFi(false);
#endif
}
void secureEraseConfig();
// chunked_write_state_t comes from config_parser.h; this file used to redefine it
// with a hardcoded 4096 in place of OD_CONFIG_MAX_SIZE.
extern uint8_t configReadResponseBuffer[128];
extern uint8_t msd_payload[16];
float readBatteryVoltage();


#ifndef BUILD_VERSION
#include "od_fw_version.h"
#define BUILD_VERSION OD_FW_VERSION_DEFAULT
#endif
#ifndef SHA
#define SHA ""
#endif
#define STRINGIFY_LOCAL(x) #x
#define XSTRINGIFY_LOCAL(x) STRINGIFY_LOCAL(x)
#define SHA_STRING_LOCAL XSTRINGIFY_LOCAL(SHA)
// Always stringify so -DBUILD_VERSION=2.24.0 works: three-part tags are not
// valid C numeric literals (two-part tags like 2.23 accidentally compiled as floats).
#define BUILD_VERSION_STRING_LOCAL XSTRINGIFY_LOCAL(BUILD_VERSION)

static constexpr uint8_t FIRMWARE_SHA_HEX_BYTES = 40;
static const char kFirmwareShaPlaceholder[FIRMWARE_SHA_HEX_BYTES + 1] =
    "0000000000000000000000000000000000000000";

// BUILD_VERSION is major.minor or major.minor.patch (optional leading 'v').
// Two-part tags imply patch 0. Component index: 0=major, 1=minor, 2=patch.
static uint8_t parseFirmwareVersionComponent(unsigned index) {
    const char* v = BUILD_VERSION_STRING_LOCAL;
    if (v == nullptr || v[0] == '\0') {
        return 0;
    }
    // XSTRINGIFY of a quoted macro yields "\"1.0.0\""; of an unquoted
    // 2.24.0 yields "2.24.0". Strip one leading quote when present.
    if (v[0] == '"') {
        v++;
    }
    while (*v == ' ' || *v == 'v' || *v == 'V') {
        v++;
    }
    for (unsigned i = 0; i < index; i++) {
        while (*v >= '0' && *v <= '9') {
            v++;
        }
        if (*v != '.') {
            return 0;
        }
        v++;
    }
    if (*v < '0' || *v > '9') {
        return 0;
    }
    unsigned n = 0;
    while (*v >= '0' && *v <= '9') {
        n = n * 10U + (unsigned)(*v - '0');
        if (n > 255U) {
            return 255;
        }
        v++;
    }
    return (uint8_t)n;
}


od_cmd_result_t handleReadMSD(const od_cmd_ctx_t *ctx) {
    uint8_t response[2 + 16];
    uint16_t responseLen = 0;
    response[responseLen++] = RESP_ACK;
    response[responseLen++] = RESP_MSD_READ;
    memcpy(&response[responseLen], msd_payload, sizeof(msd_payload));
    responseLen += sizeof(msd_payload);
    (void)od_cmd_reply(ctx, response, responseLen);
    od_log_debug("MSD read response sent (%u bytes)", responseLen);
    return OD_CMD_OK;
}

uint16_t calculateCRC16CCITT(uint8_t* data, uint32_t len) {
    uint16_t crc = 0xFFFF;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= (data[i] << 8);
        for (int j = 0; j < 8; j++) {
            if (crc & 0x8000) {
                crc = (crc << 1) ^ 0x1021;
            } else {
                crc = crc << 1;
            }
            crc &= 0xFFFF;
        }
    }
    return crc;
}

uint8_t getFirmwareMajor() {
    return parseFirmwareVersionComponent(0);
}

uint8_t getFirmwareMinor() {
    return parseFirmwareVersionComponent(1);
}

uint8_t getFirmwarePatch() {
    return parseFirmwareVersionComponent(2);
}

const char* getFirmwareShaString() {
    return SHA_STRING_LOCAL;
}

od_cmd_result_t handleFirmwareVersion(const od_cmd_ctx_t *ctx) {
    uint8_t major = getFirmwareMajor();
    uint8_t minor = getFirmwareMinor();
    uint8_t patch = getFirmwarePatch();
    // Was three String operations: strip surrounding quotes, trim, substitute a placeholder
    // when empty. Done in place on a fixed buffer instead -- the SHA is at most 40 bytes on the
    // wire and the response array below is sized for exactly that, so a growable string was
    // never buying anything here.
    char shaBuf[64];
    snprintf(shaBuf, sizeof(shaBuf), "%s", getFirmwareShaString());
    char* sha = shaBuf;
    size_t shaChars = strlen(sha);
    if (shaChars >= 2 && sha[0] == '"' && sha[shaChars - 1] == '"') {
        sha[shaChars - 1] = '\0';
        sha++;
        shaChars -= 2;
    }
    // trim(), both ends, matching Arduino's definition of whitespace closely enough for a
    // build-stamped hex string.
    while (shaChars > 0 && (unsigned char)sha[shaChars - 1] <= ' ') {
        sha[--shaChars] = '\0';
    }
    while (shaChars > 0 && (unsigned char)sha[0] <= ' ') {
        sha++;
        shaChars--;
    }
    if (shaChars == 0) {
        snprintf(shaBuf, sizeof(shaBuf), "%s", kFirmwareShaPlaceholder);
        sha = shaBuf;
        shaChars = strlen(sha);
    }
    od_log_info("Firmware version: %u.%u.%u", major, minor, patch);
    od_log_info("SHA: %s", sha);
    uint8_t shaLen = (uint8_t)(shaChars > 255 ? 255 : shaChars);
    if (shaLen > 40) shaLen = 40;
    // [ACK][0x43][major][minor][shaLen][sha…][patch] — patch is trailing so
    // old hosts that stop after SHA keep working.
    uint8_t response[2 + 1 + 1 + 1 + 40 + 1];
    uint16_t offset = 0;
    response[offset++] = RESP_ACK;
    response[offset++] = RESP_FIRMWARE_VERSION;
    response[offset++] = major;
    response[offset++] = minor;
    response[offset++] = shaLen;
    for (uint8_t i = 0; i < shaLen && i < 40; i++) {
        response[offset++] = (uint8_t)sha[i];
    }
    response[offset++] = patch;
    /* PLAIN. A client must be able to identify a device before it can authenticate, and one whose
     * key the host has lost must stay identifiable -- which is also why od_dispatch exempts this
     * opcode from the session gate. */
    (void)od_cmd_reply_plain(ctx, response, offset);
    return OD_CMD_OK;
}

/* Starts a read; it does not perform one. Chunk 0 goes out here and the rest is emitted by
 * od_config_read_pump() from the loop, one slot at a time as capacity appears.
 *
 * The synchronous loop this replaces could not survive shared egress: it queued every chunk in one
 * call, and the drain it depended on cannot run until the handler returns. It papered over that by
 * flushing the ring itself between chunks -- which is what made a config read hold the loop task
 * for the whole transfer. */
od_cmd_result_t handleReadConfig(const od_cmd_ctx_t *ctx) {
    // Shared scratch rather than a 4 KB stack array: this runs on the loop task, where a 4 KB
    // frame is a real overflow risk. The producer reads from it across many loop passes, which is
    // exactly why od_dispatch DEFERS any config-mutating opcode while a read is active -- a write
    // reloading through this same buffer mid-read would splice two configs into one CRC-valid
    // read-back.
    uint8_t* configData = getConfigScratch();
    uint32_t configLen = OD_CONFIG_MAX_SIZE;
    const bool loaded = loadConfig(configData, &configLen);

    /* The reservation is TRANSFERRED to the producer, which pays chunk 0 out of it and keeps the
     * rest; od_dispatch's release afterwards is a no-op because the token is already spent. A NULL
     * blob means the load failed and emits the 4-byte error frame -- a completed read, not a
     * pending one, which is why it is not a separate branch here. */
    const od_txq_status_t rc = od_config_read_start(&ctx->rp, ctx->r,
                                                    loaded ? configData : nullptr,
                                                    loaded ? configLen : 0u);
    return (rc == OD_TXQ_OK && loaded) ? OD_CMD_OK : OD_CMD_NACK;
}

// Outcome of "is this frame allowed to MUTATE stored configuration?".
enum ConfigWriteGate {
    CONFIG_WRITE_ALLOWED,        // proceed as-is
    CONFIG_WRITE_ALLOWED_ERASE,  // proceed, but wipe the old record first (rewrite policy)
    CONFIG_WRITE_DENIED,         // answer RESP_AUTH_REQUIRED
};

// THE single authorization predicate for config mutation. Both 0x0041 and its 0x0042
// continuations ask it, because a second authorization decision taken in a handler must
// apply the same origin rule the dispatcher applied -- and until this existed, it did
// not. The dispatcher exempts ORIGIN_LAN_TLS from the app-layer gate (SECTION 9 rule 4:
// the TLS handshake IS the authentication on that port and 0x0050 is NOT used there),
// but handleWriteConfig() then re-tested the raw security-enabled && !authenticated pair.
// A session is established only by a successful app-layer 0x0050 exchange, which a
// conforming TLS client never sends -- so a client that had
// already proved possession of the derived PSK could read config but not write it,
// unless REWRITE_ALLOWED happened to be set. Encrypted WiFi supported half the workflow.
//
// Origin ALONE is not sufficient authority: it is paired with lanTlsSessionEstablished()
// so the exemption tracks a completed handshake on the live socket rather than the
// global encryption-enabled bit.
static ConfigWriteGate configWriteGate(const od_cmd_ctx_t *ctx) {
#ifdef OPENDISPLAY_HAS_WIFI
    if (ctx->rp.origin == OD_ORIGIN_LAN_TLS && lanTlsSessionEstablished()) {
        return CONFIG_WRITE_ALLOWED;
    }
#endif
    // The SEC2 gate already admitted this frame: an unclaimed device, a claimed one with a
    // verified record, or LAN TLS. The 2.x REWRITE_ALLOWED erase-and-write path is gone; key
    // loss is recovered by factory reset (SECURITY_V2 13.5).
    return CONFIG_WRITE_ALLOWED;
}

od_cmd_result_t handleWriteConfig(const od_cmd_ctx_t *ctx, uint8_t* data, uint16_t len) {
    if (len == 0) return OD_CMD_NACK;
    const ConfigWriteGate gate = configWriteGate(ctx);
    if (gate == CONFIG_WRITE_DENIED) {
        /* AUTH_REJECTED, not NACK. Section 5 separates them because only this advances the
         * link's abuse run -- collapsing it lets a TLS client repeat a refused CONFIG_WRITE and
         * hold the exclusive link forever. The run itself is advanced once, by
         * od_core_frame_done(), off this returned outcome -- never here. */
        uint8_t response[] = {RESP_ACK, (uint8_t)(CMD_CONFIG_WRITE & 0xFF), RESP_AUTH_REQUIRED};
        (void)od_cmd_reply_plain(ctx, response, sizeof(response));
        return OD_CMD_AUTH_REJECTED;
    }
    if (gate == CONFIG_WRITE_ALLOWED_ERASE) {
        secureEraseConfig();
    }

    uint8_t responseOk[]  = {RESP_ACK,  RESP_CONFIG_WRITE, 0x00, 0x00};
    uint8_t responseErr[] = {RESP_NACK, RESP_CONFIG_WRITE, 0x00, 0x00};

    /* Shape, declared-total bounds and reassembly now live in shared/core/od_config_asm.c
     * (correctness review F3). This handler is reduced to policy + I/O, which is the point:
     * the checks that were missing are missing in ONE place for every target, and they are
     * covered by host vectors rather than by whichever malformed frame someone thought of. */
    switch (od_config_asm_start(&g_configAsm, od_span_make(data, len))) {
    case OD_CONFIG_ASM_SINGLE: {
        const bool ok = saveConfig(data, len);
        /* SPLIT, not a ternary: the ACK is an application reply and the NACK is a hard NACK, so
         * the two branches take different paths. A ternary chose one path for both. */
        if (ok) {
        /* THE ACK IS SEALED AND QUEUED BEFORE THE RELOAD, and the order is load-bearing.
         * reloadConfigAfterSave() clears the session -- the new config may carry a new key -- so a
         * reply attempted after it finds no live session, and od_reply substitutes a plaintext hard
         * NACK: the host is told a write FAILED that has already been persisted, and cannot tell
         * that from a real failure. od_reply seals at commit, so queueing first makes the bytes
         * final while the session that sent the write is still the one answering it. */
            const od_txq_status_t rc = od_cmd_reply(ctx, responseOk, sizeof(responseOk));
            reloadConfigAfterSave();
            notifyConfigChangedOverBle();
            return (rc == OD_TXQ_OK) ? OD_CMD_OK : OD_CMD_NACK;
        }
        responseErr[2] = od_config_store_save_error();
        (void)od_cmd_reply_plain(ctx, responseErr, sizeof(responseErr));
        return OD_CMD_NACK;
    }
    case OD_CONFIG_ASM_ACCEPTED:
        (void)od_cmd_reply(ctx, responseOk, sizeof(responseOk));
        return OD_CMD_OK;
    case OD_CONFIG_ASM_COMPLETE: {
        /* Not reachable today -- a chunked start never completes on its own frame -- but
         * handled rather than assumed, so a future single-frame-with-prefix shape cannot fall
         * through to the NACK below and look like a malformed write. */
        const bool ok = saveConfig(g_configAsm.buffer, (uint16_t)g_configAsm.total_size);
        od_config_asm_reset(&g_configAsm);
        if (ok) {
        /* THE ACK IS SEALED AND QUEUED BEFORE THE RELOAD, and the order is load-bearing.
         * reloadConfigAfterSave() clears the session -- the new config may carry a new key -- so a
         * reply attempted after it finds no live session, and od_reply substitutes a plaintext hard
         * NACK: the host is told a write FAILED that has already been persisted, and cannot tell
         * that from a real failure. od_reply seals at commit, so queueing first makes the bytes
         * final while the session that sent the write is still the one answering it. */
            const od_txq_status_t rc = od_cmd_reply(ctx, responseOk, sizeof(responseOk));
            reloadConfigAfterSave();
            notifyConfigChangedOverBle();
            return (rc == OD_TXQ_OK) ? OD_CMD_OK : OD_CMD_NACK;
        }
        responseErr[2] = od_config_store_save_error();
        (void)od_cmd_reply_plain(ctx, responseErr, sizeof(responseErr));
        return OD_CMD_NACK;
    }
    case OD_CONFIG_ASM_REJECTED:
    default:
        /* NOTHING WAS STORED. That is guaranteed by construction: the assembler has no
         * storage symbol to reach, so a rejection cannot have altered NVS. */
        od_log_error("[%s] malformed CONFIG_WRITE (%u B) -- nothing stored",
                     originTag(ctx->rp.origin), (unsigned)len);
        (void)od_cmd_reply_plain(ctx, responseErr, sizeof(responseErr));
        return OD_CMD_NACK;
    }
}

od_cmd_result_t handleClearConfig(const od_cmd_ctx_t *ctx) {
    uint8_t responseOk[] = {RESP_ACK, RESP_CONFIG_CLEAR, 0x00, 0x00};
    uint8_t responseErr[] = {RESP_NACK, RESP_CONFIG_CLEAR, 0x00, 0x00};

    if (!clearStoredConfig()) {
        (void)od_cmd_reply_plain(ctx, responseErr, sizeof(responseErr));
        return OD_CMD_NACK;
    }

    od_log_info("Stored config cleared");
    (void)od_cmd_reply(ctx, responseOk, sizeof(responseOk));
    return OD_CMD_OK;
}

od_cmd_result_t handleWriteConfigChunk(const od_cmd_ctx_t *ctx, uint8_t* data, uint16_t len) {
    uint8_t ok_resp[]  = {RESP_ACK,  RESP_CONFIG_CHUNK, 0x00, 0x00};
    uint8_t err_resp[] = {RESP_NACK, RESP_CONFIG_CHUNK, 0x00, 0x00};

    /* The authorization re-check stays HERE, not in shared/: it is policy about who may mutate
     * configuration, and it needs frame origin and session state the assembler has no business
     * knowing. Still gated on the first continuation only, exactly as before. */
    if (g_configAsm.chunks == 1u && g_configAsm.active) {
        const ConfigWriteGate gate = configWriteGate(ctx);
        if (gate == CONFIG_WRITE_DENIED) {
            od_config_asm_reset(&g_configAsm);
            uint8_t response[] = {RESP_ACK, (uint8_t)(CMD_CONFIG_CHUNK & 0xFF), RESP_AUTH_REQUIRED};
            (void)od_cmd_reply_plain(ctx, response, sizeof(response));
            return OD_CMD_AUTH_REJECTED;
        }
        if (gate == CONFIG_WRITE_ALLOWED_ERASE) {
            secureEraseConfig();
        }
    }

    switch (od_config_asm_chunk(&g_configAsm, od_span_make(data, len))) {
    case OD_CONFIG_ASM_ACCEPTED:
        (void)od_cmd_reply(ctx, ok_resp, sizeof(ok_resp));
        return OD_CMD_OK;
    case OD_CONFIG_ASM_COMPLETE: {
        /* Committed on an EXACT byte count, never a chunk count -- the F3 fix. */
        const bool saved = saveConfig(g_configAsm.buffer, (uint16_t)g_configAsm.total_size);
        od_config_asm_reset(&g_configAsm);
        if (saved) {
        /* THE ACK IS SEALED AND QUEUED BEFORE THE RELOAD, and the order is load-bearing.
         * reloadConfigAfterSave() clears the session -- the new config may carry a new key -- so a
         * reply attempted after it finds no live session, and od_reply substitutes a plaintext hard
         * NACK: the host is told a write FAILED that has already been persisted, and cannot tell
         * that from a real failure. od_reply seals at commit, so queueing first makes the bytes
         * final while the session that sent the write is still the one answering it. */
            const od_txq_status_t rc = od_cmd_reply(ctx, ok_resp, sizeof(ok_resp));
            reloadConfigAfterSave();
            notifyConfigChangedOverBle();
            return (rc == OD_TXQ_OK) ? OD_CMD_OK : OD_CMD_NACK;
        }
        err_resp[2] = od_config_store_save_error();
        (void)od_cmd_reply_plain(ctx, err_resp, sizeof(err_resp));
        return OD_CMD_NACK;
    }
    case OD_CONFIG_ASM_SINGLE:
    case OD_CONFIG_ASM_REJECTED:
    default:
        od_log_error("[%s] bad CONFIG_CHUNK (%u B) -- transfer dropped, nothing stored",
                     originTag(ctx->rp.origin), (unsigned)len);
        (void)od_cmd_reply_plain(ctx, err_resp, sizeof(err_resp));
        return OD_CMD_NACK;
    }
}

// Human-readable name for a command opcode, used for the single dispatch banner
// emitted by od_dispatch_app_frame() (this target's ingress adapter for BLE and LAN).
// Returns nullptr for opcodes not dispatched here
// (incl. CMD_NFC_ENDPOINT 0x0083, which this Firmware does not implement on any
// target) — the switch default logs those as unknown. Single source of truth for
// the banner text: keep in sync with the dispatch switch below; individual
// cases/handlers must NOT log their own "=== ... COMMAND ... ===" banner.
static const char* commandName(uint16_t cmd) {
    switch (cmd) {
        case CMD_REBOOT:              return "REBOOT";              // 0x000F
        case CMD_CONFIG_READ:         return "READ CONFIG";         // 0x0040
        case CMD_CONFIG_WRITE:        return "WRITE CONFIG";        // 0x0041
        case CMD_CONFIG_CHUNK:        return "WRITE CONFIG CHUNK";  // 0x0042
        case CMD_FIRMWARE_VERSION:    return "FIRMWARE VERSION";    // 0x0043
        case CMD_READ_MSD:            return "READ MSD";            // 0x0044
        case CMD_CONFIG_CLEAR:        return "CLEAR CONFIG";        // 0x0045
        case CMD_AUTHENTICATE:        return "AUTHENTICATE";        // 0x0050
        case CMD_ENTER_DFU:           return "ENTER DFU MODE";      // 0x0051
        case CMD_POWER_OFF:           return "POWER OFF";           // 0x0052
        case CMD_DEEP_SLEEP:          return "DEEP SLEEP";          // 0x0053
        case CMD_DIRECT_WRITE_START:  return "DIRECT WRITE START";  // 0x0070
        case CMD_DIRECT_WRITE_DATA:   return "DIRECT WRITE DATA";   // 0x0071
        case CMD_DIRECT_WRITE_END:    return "DIRECT WRITE END";    // 0x0072
        case CMD_LED_ACTIVATE:        return "LED ACTIVATE";        // 0x0073
        case CMD_LED_STOP:            return "LED STOP";            // 0x0075
        case CMD_PARTIAL_WRITE_START: return "PARTIAL WRITE START"; // 0x0076
        case CMD_BUZZER:              return "BUZZER ACTIVATE";     // 0x0077
        case CMD_PIPE_WRITE_START:    return "PIPE WRITE START";    // 0x0080
        case CMD_PIPE_WRITE_DATA:     return "PIPE WRITE DATA";     // 0x0081
        case CMD_PIPE_WRITE_END:      return "PIPE WRITE END";      // 0x0082
        default:                      return nullptr;
    }
}

/* Applies od_frame_policy() with the origin and ownership scoping the table deliberately leaves
 * to the target -- see od_cmd.h. This is the ONLY place activity and abuse move, which is what
 * makes the three earlier positions for this test unrepeatable: each predicted acceptance at a
 * layer that could still reject, and each was wrong at whatever rejected next. An outcome cannot
 * predict; it is the answer.
 *
 * OWNERSHIP FIRST, for both. A frame whose tag no longer owns the link must neither hold that link
 * alive nor spend its abuse budget. LAN frames carry tag 0 whenever the link owner is not
 * OWNER_LAN, so they fall out here exactly as they did before. */
extern "C" void od_core_frame_done(const od_reply_t *rp, od_frame_outcome_t outcome)
{
    const od_frame_policy_t p = od_frame_policy(outcome);

    if (rp == nullptr || !linkIsOwnerWord(rp->tag)) {
        return;
    }
    if (p.stamp_activity) {
        linkStampOwnerCommand();
    }
    /* BLE ONLY, and the origin gate is not decoration: the same auth gate is reachable from
     * plaintext LAN, and counting those would let LAN traffic drop a BLE client. */
    if (rp->origin != OD_ORIGIN_BLE) {
        return;
    }
    if (p.reset_abuse) {
        resetAuthAbuseCounter();
    }
    if (p.increment_abuse) {
        authAbuseAdvance();
    }
}

od_frame_outcome_t od_dispatch_app_frame(const od_reply_t *rp, uint8_t* data, uint16_t len) {
    // Single per-command banner for the whole dispatch, emitted before the dispatcher so a frame
    // it refuses structurally is still attributable. Named via commandName(); an opcode the shared
    // map does not route (nullptr) gets no banner and no reply at all. Handlers must not
    // log their own banner. Carries no encryption token: the ERX/URX line from od_rxq_push()
    // already reports it for this frame, and stating it twice is how the two spellings drift.
    if (len < 2) {
        od_log_error("Command too short (%u bytes)", len);
    } else {
        const uint16_t command = (uint16_t)((data[0] << 8) | data[1]);
        // Silence the per-frame command spam for image-write data once the stream is past its
        // first chunk; the display handler's 5% meter reports it.
        const bool quietCmd = od_xfer_log_quiet(command);
        if (!quietCmd) {
            const char* name = commandName(command);
            if (name != nullptr) {
                od_log_info("=== [%s] %s COMMAND (0x%04X) ===", originTag(rp->origin), name,
                            command);
            }
        }
    }

    const od_frame_outcome_t outcome = od_dispatch_frame(rp, od_span_make(data, len));
    od_core_frame_done(rp, outcome);

    /* EVERY unit taken must be back. A leak here is invisible on the wire and starves the very
     * next command, so it is asserted rather than trusted -- od_dispatch releases on every exit
     * path, including the ones that never reach a handler. It does NOT catch an over-budget
     * handler: releasing an exhausted token leaves zero either way. That is what the per-opcode
     * budget cases in dispatch_test.c are for. */
    if (od_txq_reserved() != 0u) {
        od_log_error("%u reservation unit(s) leaked by 0x%04X",
                     (unsigned)od_txq_reserved(),
                     (unsigned)(len >= 2 ? ((data[0] << 8) | data[1]) : 0));
        od_txq_reset();
    }
    return outcome;
}

extern "C" bool od_smp_encryption_on(void) {
    return securityConfig.encryption_enabled != 0u;
}
