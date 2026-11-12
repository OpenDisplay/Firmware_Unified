// ESP32 implementation of BleTransport, and this target's only one.
//
// PORTED FROM THE REFERENCE FIRMWARE, WITH ONE SUBSTITUTION. Upstream this file
// drives NimBLE-Arduino (h2zero) and keeps "every NimBLE object as file-static
// state" inside itself. ESP-IDF does not ship that library -- BLE here is
// NimBLE's own C API, wrapped by ../ble/od_ble.h -- so every stack call below
// goes through od_ble_* and the stack objects live one file down. The property
// the upstream comment was protecting is unchanged: nothing outside ble/ names a
// NimBLE type.
//
// Everything ELSE is upstream's, deliberately and near-verbatim: the instance
// table, the epoch/claim protocol, the atomics and their ordering, the write
// filter, the owner-targeted notify, and the event flags. Those encode
// CONNECTION_POLICY, which is target-independent -- re-deriving them for this
// target is exactly the "divergence settled by whichever repo was copied first"
// that docs/MIGRATION.md forbids. Where the mechanism genuinely differs it is
// marked OD-PORT with the reason.
//
// Threading contract, identical on both targets: a stack callback may copy bytes
// into the RX ring, publish its own instance-table entry, attempt the single
// ownership claim CAS, and set an event flag. Everything else -- dispatch,
// decrypt, EPD streaming, notify(), the connect/disconnect application work --
// runs on the loop() task. Here the callbacks arrive as the od_ble_evt_* hooks,
// which od_ble.h declares under exactly that contract.
//
// The claim is the one thing beyond "copy and flag", and it belongs here because
// the write filter must be able to test ownership before any loop pass runs.

#include <string.h>

#include "od_health_app.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "ble_transport.h"
#include "ble_transport_esp32.h"
#include "od_rxq.h"
#include "link_owner.h"
#include "structs.h"
#include "od_log.h"

BleTransport ble;

// --- event flags -------------------------------------------------------------
static volatile bool s_connectedEvent = false;
static volatile bool s_disconnectedEvent = false;
// NimBLE's reason is not a byte: it spans two ranges (HCI wrapped at 0x200+,
// host-layer 1..31), so a uint8_t here silently aliased them onto each other. See
// takeDisconnectedEvent().
static volatile uint16_t s_disconnectReason = 0;
static volatile uint32_t s_connectedWord = 0;      // identity of the last connect
static volatile uint32_t s_disconnectedWord = 0;   // identity of the last disconnect

// Set once od_ble_init() reports the stack up. Upstream tests `s_server != nullptr`
// for the same thing; there is no server object here to test.
static bool s_ready = false;

// --- the instance table (CONNECTION_POLICY R3 requirement 5) -----------------
// Sized by the connection cap.
//
// OD-PORT: upstream hardcodes 3 and explains why it must -- under Arduino,
// CONFIG_BT_NIMBLE_MAX_CONNECTIONS came from a PRECOMPILED sdkconfig.h that a -D
// override could not reach, so the cap was 3 whatever the build said and
// exclusivity had to be enforced in firmware rather than by config. Under ESP-IDF
// the value is a real Kconfig symbol this target sets (sdkconfig.defaults pins it
// to 1), so the table is derived from it and the firmware-side machinery keeps
// working at either setting.
//
// Deriving it does NOT make the machinery redundant, and it must not be deleted
// on the grounds that the cap is 1: at 1 the controller refuses a contender
// before any hook fires, which is a strictly stronger guarantee than R1's
// "transiently attached, then refused" -- but the policy is what makes the two
// interchangeable, and raising the cap must not silently un-enforce exclusivity.
// The floor of 1 keeps the array valid if the symbol is ever absent.
#ifndef OD_BLE_MAX_INSTANCES
#  if defined(CONFIG_BT_NIMBLE_MAX_CONNECTIONS) && CONFIG_BT_NIMBLE_MAX_CONNECTIONS > 0
#    define OD_BLE_MAX_INSTANCES CONFIG_BT_NIMBLE_MAX_CONNECTIONS
#  else
#    define OD_BLE_MAX_INSTANCES 1
#  endif
#endif

// Each entry's packed identity word IS its liveness: all-zero means empty. There is
// no separate `state` field, so a reader can never see a live identity with a stale
// state. Written on the NimBLE host task, read on the loop task; the word is the
// synchronisation point, and `subscribed`/`decidedWord` are only read once it says
// down or under owner comparison.
struct BleInstance {
    volatile uint32_t word;         // packed (OWNER_BLE, handle, epoch); 0 = empty
    // Per-link CCCD state (requirement 2). Written on the NimBLE host task by the
    // subscribe hook, read on the loop task by notifyReady, so every access goes
    // through __atomic_*: `volatile` orders nothing and does not make a concurrent
    // read/write anything but a data race in C++.
    volatile uint8_t  subscribed;
    // Claim disposition, published with RELEASE after the CAS resolves: 0 while the
    // claim is in flight, otherwise the IDENTITY WORD the claim was decided for.
    //
    // It carries the identity rather than a bare flag because the loop-side scan
    // reads several fields and cannot get them atomically. A boolean lets two
    // distinct hazards through: the scan could pair entry w1 with a disposition that
    // actually belongs to w2 after the slot was retired and reused (ABA), and it
    // could not tell "resolved for THIS instance" from "resolved for whoever holds
    // this slot now". Matching decidedWord against the entry word proves both.
    //
    // The distinction itself is load-bearing: refusing an in-flight instance can
    // disconnect the connection that is winning the slot, while never refusing one
    // leaves a decided loser attached forever.
    volatile uint32_t decidedWord;
};
static BleInstance s_instances[OD_BLE_MAX_INSTANCES];

// Search by handle rather than indexing by it. NimBLE allocates from 0 upward in
// practice, so direct indexing usually works, but a short linear search costs the
// same at this size and cannot be broken by a stack change that hands out sparse
// handles.
static int instanceIndexOf(uint16_t handle) {
    for (int i = 0; i < OD_BLE_MAX_INSTANCES; i++) {
        const uint32_t w = __atomic_load_n(&s_instances[i].word, __ATOMIC_ACQUIRE);
        if (w != 0 && linkUnpackWord(w).handle == handle) return i;
    }
    return -1;
}

static uint32_t instancePublish(uint16_t handle, uint16_t epoch) {
    const uint32_t w = linkPackWord(OWNER_BLE, handle, epoch);
    for (int i = 0; i < OD_BLE_MAX_INSTANCES; i++) {
        uint32_t expected = 0;
        if (__atomic_compare_exchange_n(&s_instances[i].word, &expected, w,
                                        false, __ATOMIC_RELEASE, __ATOMIC_RELAXED)) {
            __atomic_store_n(&s_instances[i].subscribed, (uint8_t)0, __ATOMIC_RELAXED);
            __atomic_store_n(&s_instances[i].decidedWord, (uint32_t)0, __ATOMIC_RELEASE);
            return w;
        }
    }
    // Cannot happen while the table is sized at the connection cap; if the stack
    // ever exceeds it, the link is unrepresentable and therefore unserviceable.
    od_log_error("BLE instance table full, handle %u unrepresentable", (unsigned)handle);
    return 0;
}

static void instanceRetire(uint16_t handle) {
    const int i = instanceIndexOf(handle);
    if (i < 0) return;
    __atomic_store_n(&s_instances[i].subscribed, (uint8_t)0, __ATOMIC_RELAXED);
    __atomic_store_n(&s_instances[i].decidedWord, (uint32_t)0, __ATOMIC_RELAXED);
    __atomic_store_n(&s_instances[i].word, (uint32_t)0, __ATOMIC_RELEASE);
}

static void instancesClear() {
    for (int i = 0; i < OD_BLE_MAX_INSTANCES; i++) {
        __atomic_store_n(&s_instances[i].subscribed, (uint8_t)0, __ATOMIC_RELAXED);
        __atomic_store_n(&s_instances[i].decidedWord, (uint32_t)0, __ATOMIC_RELAXED);
        __atomic_store_n(&s_instances[i].word, (uint32_t)0, __ATOMIC_RELEASE);
    }
}

// --- link diagnostics (implementation-private) ------------------------------
static const char* phyName(uint8_t phy) {
    switch (phy) {
        case 1:  return "1M";        // BLE_GAP_LE_PHY_1M
        case 2:  return "2M";        // BLE_GAP_LE_PHY_2M
        case 3:  return "Coded";     // BLE_GAP_LE_PHY_CODED
        default: return "?";
    }
}

// --- stack hooks (NimBLE host task -- flag-only) -----------------------------
// These are od_ble.h's od_ble_evt_* declarations; upstream they are
// NimBLEServerCallbacks / NimBLECharacteristicCallbacks methods. Same contract,
// same task, same restrictions.

void od_ble_evt_connect(uint16_t conn_handle) {
    // ORDER IS NORMATIVE (R2): allocate the epoch, publish the table entry,
    // THEN attempt the claim -- so a successful claim never names an instance
    // the loop cannot yet see.
    //
    // The epoch is allocated for EVERY instance, admitted or not. Allocating on
    // successful claim instead (an earlier draft) would leave a refused
    // contender with no epoch, making 7a row 4 -- a contender that reuses the
    // incumbent's handle after a stale link -- indistinguishable from the
    // incumbent. Identity is what the admission decision is MADE on, so it must
    // precede the decision.
    const uint16_t epoch = linkNextEpoch();
    const uint32_t word = instancePublish(conn_handle, epoch);
    // The claim itself, here rather than on the loop task: this is the earliest
    // transport hook, and R7d makes it the authoritative arbitration point. A
    // CAS win IS admission; a loss makes this instance a contender, whose writes
    // the filter below drops from its very first frame.
    const LinkId id = { OWNER_BLE, conn_handle, epoch };
    const bool admitted = (word != 0) && linkClaim(id);
    // The claim has resolved; publish that fact so the loop-side refusal scan
    // can tell this instance from one whose CAS has not run yet.
    {
        const int di = instanceIndexOf(conn_handle);
        // Publish the identity the claim resolved FOR, not merely that it
        // resolved. NimBLE serialises host callbacks, so this handle cannot be
        // retired and reused underneath us here.
        if (di >= 0) __atomic_store_n(&s_instances[di].decidedWord, word, __ATOMIC_RELEASE);
    }
    od_log_info("=== BLE CLIENT CONNECTED (ESP32) h=%u e=%u %s ===",
                (unsigned)conn_handle, (unsigned)epoch,
                admitted ? "[owner]" : "[contender - refused service]");
    // Payload before flag, flag RELEASE-stored, so a consumer that sees the flag
    // is guaranteed to see this word. A plain store here against the consumer's
    // atomic exchange would be a data race with nothing for its acquire to pair
    // against.
    __atomic_store_n(&s_connectedWord, word, __ATOMIC_RELAXED);
    // Flag-only beyond the claim. The app work a connect implies (rebootFlag
    // reset, updatemsdata() -- which polls I2C and mutates the shared
    // advertisement vector that loop() also drives on a 60 s cadence) would
    // corrupt the heap if run here on the NimBLE host task. loop() consumes the
    // event instead. The claim is exempt because it is one CAS on one word.
    __atomic_store_n(&s_connectedEvent, true, __ATOMIC_RELEASE);
    od_health_app_link_opened();
}

void od_ble_evt_disconnect(uint16_t conn_handle, uint16_t reason) {
    const int idx = instanceIndexOf(conn_handle);
    const uint32_t word = (idx >= 0) ? __atomic_load_n(&s_instances[idx].word, __ATOMIC_ACQUIRE) : 0;
    od_log_info("=== BLE CLIENT DISCONNECTED (ESP32) h=%u reason=0x%03X ===",
                (unsigned)conn_handle, (unsigned)reason);
    // Full width: NimBLE's reason spans two ranges and truncation aliases them.
    __atomic_store_n(&s_disconnectReason, reason, __ATOMIC_RELAXED);
    __atomic_store_n(&s_disconnectedWord, word, __ATOMIC_RELAXED);
    // Retiring the entry is what makes the link's death observable per handle --
    // the predicate bleDropAndWait() polls, and the comparison that lets the loop
    // notice a departed owner even when every event edge was lost.
    //
    // No RX-boundary capture here any more: frames carry their writer's identity
    // (od_rxq_item_t::tag), so a departed session's frames self-discard at
    // dispatch instead of needing a boundary that handle reuse could destroy.
    instanceRetire(conn_handle);
    // The token is deliberately NOT released here. Releasing on this callback
    // would admit a new owner while the departed session's state is still live,
    // and RX/TX run before the deferred cleanup. Release stays the abort's last
    // step (R3a); a reconnecting client that loses its claim is reaped by
    // contender refusal.
    // Flag-only. The session teardown this implies (EPD force-off with
    // SPI.end()/rail cut, partial + pipe cleanup) is heavyweight,
    // state-mutating work that races loop()'s SPI streaming and pipe-frame
    // processing. loop() consumes the event and applies its own deferral
    // policy (see serviceBleDisconnectCleanup in main.cpp).
    __atomic_store_n(&s_disconnectedEvent, true, __ATOMIC_RELEASE);
    od_health_app_link_closed();
}

void od_ble_evt_subscribe(uint16_t conn_handle, bool enabled) {
    // Requirement 2: per-link subscribe state. This used to (void) its connInfo and
    // write one global, so a contender's subscribe cleared or overwrote the
    // incumbent's apparent notify-readiness and stalled its TX.
    od_health_app_att_seen();   /* a CCC write is ATT traffic */
    const int idx = instanceIndexOf(conn_handle);
    if (idx < 0) return;
    __atomic_store_n(&s_instances[idx].subscribed, (uint8_t)(enabled ? 1 : 0), __ATOMIC_RELAXED);
    od_log_info("BLE notify subscription h=%u: %s",
                (unsigned)conn_handle, enabled ? "enabled" : "disabled");
}

void od_ble_evt_write(uint16_t conn_handle, const uint8_t* data, uint16_t len) {
    // Requirement 1: drop a non-owner's write BEFORE it reaches the RX ring.
    // This must happen here, not on the loop task -- during a ~16 s refresh no
    // loop-side decision runs at all, which is long enough for a gatecrasher to
    // inject a full transfer's worth of commands into the incumbent's stream.
    //
    // One atomic load of the owner word, compared against this instance's
    // identity. That comparison is only possible because the token is a single
    // published word; a loop-task-only owner would give this callback nothing to
    // test against.
    const int idx = instanceIndexOf(conn_handle);
    const uint32_t word = (idx >= 0) ? __atomic_load_n(&s_instances[idx].word, __ATOMIC_ACQUIRE) : 0;
    if (word == 0 || word != linkOwnerWord()) {
        od_log_debug("Dropped write from non-owner h=%u", (unsigned)conn_handle);
        od_health_app_att_seen();   /* ATT works; nothing was served */
        return;
    }
    od_health_app_served();   /* evidence that this image serves GATT: confirms an OTA image */
    // The payload is the flattened mbuf od_ble handed us, still binary: converting
    // it to a string uses the C-string (strlen) constructor, which truncates at the
    // first 0x00 byte, and pipe-write frames start with 0x00 (00 70 / 00 71 /
    // 00 81), so it would report length 0.
    //
    // Copy-and-enqueue is all this callback may do; loop() dispatches.
    // od_rxq_push() owns the arrival log and every drop reason (empty, too
    // large, ring full) so this hook and nRF's onWriteCb() cannot report the
    // same frame differently. Add no logging here.
    //
    // The frame carries `word` as its tag: the dispatcher re-checks it against
    // the live owner word, so a frame that was legitimate on arrival but whose
    // session ended before it was drained never executes in the next session.
    (void)od_rxq_push(data, len, word);
}

// Report the whole negotiated picture on every event rather than one field per
// callback, so a single log line is enough to judge the link. Logged at INFO:
// this is the answer to "did the 2M PHY / big MTU actually get granted", which
// is worth having in a default-level capture from the bench.
//
// DLE (max LL PDU octets) is absent: neither NimBLE's C API nor NimBLEConnInfo
// exposes the negotiated data length, unlike Bluefruit's
// BLEConnection::getDataLength(). Not an oversight -- there is no accessor.
void od_ble_evt_link_negotiated(uint16_t conn_handle, const char* trigger) {
    uint8_t txPhy = 0, rxPhy = 0;
    uint16_t mtu = 0, itvlUnits = 0;
    od_ble_link_params(conn_handle, &txPhy, &rxPhy, &mtu, &itvlUnits);
    od_log_info("[LINK negotiated after %s] PHY tx=%s rx=%s  ATT_MTU=%u  connInterval=%.2f ms",
                trigger, phyName(txPhy), phyName(rxPhy),
                (unsigned)mtu, itvlUnits * 1.25f);
}

// --- BleTransport ------------------------------------------------------------
bool BleTransport::begin(const char* deviceName) {
    instancesClear();
    s_ready = false;
    od_log_info("=== Initializing ESP32 BLE ===");
    od_log_info("Device name will be: %s", deviceName);
    // Preferred only: the central drives the exchange and may settle lower. Set
    // BEFORE init so the value is in place for the first connection rather than
    // applied on the second.
    od_log_info("Setting preferred BLE ATT MTU to %u...", (unsigned)OD_BLE_PREFERRED_ATT_MTU);
    od_ble_set_preferred_mtu(OD_BLE_PREFERRED_ATT_MTU);
    // Brings up the controller, host and GATT server, and starts the host task.
    // The GATT layout (service and characteristic both 0x2446, READ | NOTIFY |
    // WRITE | WRITE_NR, auto CCCD, maximum value OD_BLE_MAX_FRAME - 3) is
    // built in ble/od_ble_nimble.cpp and is a wire contract -- see od_ble.h.
    if (!od_ble_init(deviceName)) {
        od_log_error("Failed to initialise the BLE stack");
        return false;
    }
    s_ready = true;
    /* OD-INSTRUMENTATION 2026-08-05 -- TEMPORARY. Marks the instant od_gap_event is live and
     * the GATT server is registered. A GAP CONNECT stamped BEFORE this line would confirm the
     * "link formed during bring-up" hypothesis for the first connection after a wake. */
    od_log_warn("[instr] BleTransport::begin() complete -- gap callback live");
    return true;
}

void BleTransport::startAdvertising() {
    if (!s_ready) {
        od_log_warn("=== BLE advertising NOT started: transport not ready ===");
        return;
    }
    /* od_ble_restart_advertising() only records intent; the pump makes the stack call. Drive it
     * to an outcome so the log reports what happened, not what was asked for. Bounded so a start
     * that cannot complete does not hang setup(); the pump's stall report escalates from there. */
    od_ble_restart_advertising();
    for (unsigned pass = 0; pass < 8u && !od_ble_advertising_active(); ++pass) {
        (void)od_ble_service_advertising(true);
    }
    if (od_ble_advertising_active()) {
        od_log_info("=== BLE advertising started successfully ===");
    } else {
        od_log_warn("=== BLE advertising requested but NOT running after 8 passes ===");
    }
}

void BleTransport::restartAdvertising() {
    // Unconditional by contract: the caller owns the "still connected / mid-EPD
    // refresh / stack not up" deferral policy (see serviceBleAdvertisingRestart
    // in main.cpp). The delay mirrors the historical sequence.
    //
    // THE 100 ms SETTLE IS GONE, and its absence is the point. It existed because this used
    // to issue ble_gap_adv_stop() and ble_gap_adv_start() back to back on the caller's task,
    // racing the host. od_ble_restart_advertising() now only records INTENT; the stop/program/
    // start sequence is reconciled one step per loop pass by od_ble_service_advertising(), so
    // there is no back-to-back pair left to settle and nothing to sleep for. Blocking the loop
    // task for 100 ms to no purpose was also stalling BLE RX and the panel.
    od_ble_restart_advertising();
    od_log_info("BLE advertising requested");
}

void BleTransport::stopAdvertising() {
    if (!s_ready) return;
    od_ble_stop_advertising();
    od_log_info("BLE advertising stopped");
}

bool BleTransport::end() {
    // F7: CLEAR STATE ONLY ON CONFIRMED SUCCESS. This used to drop s_ready and the whole
    // instance table unconditionally, while od_ble_deinit() correctly left ITS state intact
    // when nimble_port_stop() failed. The two layers then disagreed: this one reporting BLE
    // down, the one that actually knows reporting it up. Every later lifecycle decision --
    // isReady(), a re-init, a connection callback arriving against an emptied instance table
    // -- reasoned from whichever answer it happened to ask.
    if (!od_ble_deinit()) {
        od_log_error("BLE teardown failed -- stack still UP, transport state unchanged");
        return false;
    }
    s_ready = false;
    instancesClear();
    return true;
}

bool BleTransport::isReady() const {
    return s_ready && od_ble_is_ready();
}

uint8_t BleTransport::connectedCount() const {
    return od_ble_connected_count();
}

bool BleTransport::notifyReady() const {
    if (!s_ready || od_ble_connected_count() == 0) {
        return false;
    }
    // The OWNER's subscription, not "whoever subscribed last". NimBLE auto-creates
    // the 0x2902 CCCD; the subscribe hook tracks each client's toggle per instance.
    const uint32_t owner = linkOwnerWord();
    if (owner == 0) return false;
    const LinkId id = linkUnpackWord(owner);
    if (id.who != OWNER_BLE) return false;
    const int idx = instanceIndexOf(id.handle);
    if (idx < 0) return false;
    if (__atomic_load_n(&s_instances[idx].word, __ATOMIC_ACQUIRE) != owner) return false;
    return __atomic_load_n(&s_instances[idx].subscribed, __ATOMIC_RELAXED) != 0;
}

bool BleTransport::notify(const uint8_t* data, uint16_t len) {
    if (!s_ready) return false;
    // Requirement 3, and a LIVE LEAK FIX rather than a new feature. This used to
    // call notify(data, len) -- the two-argument overload, whose third parameter
    // defaults to BLE_HS_CONN_HANDLE_NONE, documented as "send to ALL subscribed
    // clients". A second central that connected and subscribed therefore received
    // every response the incumbent was sent, authentication traffic included,
    // before loop() ran at all and with no policy decision having been made.
    //
    // Targeting the owner's handle closes it. With no owner there is nobody to
    // notify, and returning false leaves the entry queued rather than dropping it.
    const uint32_t owner = linkOwnerWord();
    if (owner == 0) return false;
    const LinkId id = linkUnpackWord(owner);
    if (id.who != OWNER_BLE) return false;
    // Re-validate the handle against the table immediately before sending, rather
    // than trusting a readiness check that ran earlier in the pass. Between
    // notifyReady() and here the host task can retire the owner and hand the SAME
    // numeric handle to a contender -- and a bare handle carries no epoch, so
    // NimBLE would happily deliver the departed owner's queued response to the
    // newcomer, reopening exactly the leak this function exists to close. The
    // full-word comparison catches it because the epoch differs.
    //
    // RESIDUAL, stated rather than papered over: this narrows the window to the few
    // instructions between the check and NimBLE's send, but cannot close it. The
    // stack can retire a link and reassign its numeric handle at any point on the
    // host task, and a notify addresses whatever connection the stack currently
    // calls `handle` -- there is no epoch to pass it. Closing it fully needs the
    // send to be serialised with the host's connection lifecycle, which this API
    // does not offer. What IS closed is the systematic leak: the previous
    // two-argument call broadcast every response to ALL subscribed clients, for the
    // whole life of a contender's subscription.
    const int idx = instanceIndexOf(id.handle);
    if (idx < 0) return false;
    if (__atomic_load_n(&s_instances[idx].word, __ATOMIC_ACQUIRE) != owner) return false;
    // od_ble_notify_handle() copies the payload into an mbuf immediately, so a
    // concurrent client WRITE_NR on this shared RX/TX characteristic cannot
    // corrupt the outgoing frame. On mbuf exhaustion it returns false --
    // backpressure, not failure.
    return od_ble_notify_handle(id.handle, data, len);
}

bool BleTransport::instanceLive(uint16_t handle, uint16_t epoch) const {
    const int idx = instanceIndexOf(handle);
    if (idx < 0) return false;
    return __atomic_load_n(&s_instances[idx].word, __ATOMIC_ACQUIRE) ==
           linkPackWord(OWNER_BLE, handle, epoch);
}

uint8_t BleTransport::liveInstanceCount() const {
    uint8_t n = 0;
    for (int i = 0; i < OD_BLE_MAX_INSTANCES; i++) {
        if (__atomic_load_n(&s_instances[i].word, __ATOMIC_ACQUIRE) != 0) n++;
    }
    return n;
}

uint32_t BleTransport::instanceWordAt(uint8_t index) const {
    if (index >= OD_BLE_MAX_INSTANCES) return 0;
    return __atomic_load_n(&s_instances[index].word, __ATOMIC_ACQUIRE);
}

uint32_t BleTransport::instanceClaimDecidedWordAt(uint8_t index) const {
    if (index >= OD_BLE_MAX_INSTANCES) return 0;
    return __atomic_load_n(&s_instances[index].decidedWord, __ATOMIC_ACQUIRE);
}

uint8_t BleTransport::instanceCapacity() { return OD_BLE_MAX_INSTANCES; }

bool BleTransport::disconnect(uint16_t handle, uint16_t epoch) {
    if (!s_ready) return false;
    // Re-validate the identity as late as possible: the caller's decision to drop
    // this link may have been made a few loads ago, and a numeric handle alone does
    // not identify a connection over time.
    if (!instanceLive(handle, epoch)) return true;   // already gone, or reassigned
    // od_ble_disconnect() sends HCI reason 0x13 (REMOTE_USER_TERMINATED), the only
    // value this seam ever sends; see ble_transport.h for why 0x09 must not be used.
    const bool ok = od_ble_disconnect(handle);
    if (!ok) {
        // od_ble_disconnect() already treats "the link is gone" as success
        // (BLE_HS_ENOTCONN / BLE_HS_EALREADY), so a false here is a genuine failure
        // to ask, not a benign race with a client that left first.
        od_log_warn("BLE disconnect request failed for handle %u", (unsigned)handle);
    }
    return ok;
}

uint16_t BleTransport::connIntervalMs(uint16_t handle) const {
    const uint16_t units = od_ble_conn_interval_units(handle);   // 1.25 ms units
    if (units == 0) return 0;
    return (uint16_t)((units * 5 + 3) / 4);                      // ceil(units * 1.25)
}

void BleTransport::setManufacturerData(const uint8_t* msd, uint8_t len) {
    od_ble_set_manufacturer_data(msd, len);
    if (!s_ready || connectedCount() > 0) {
        // Only restart advertising while disconnected -- restarting under a live
        // connection would drop it. Upstream's connected branch rebuilt the
        // advertisement data but never pushed it, so it was dead work; here the
        // payload is already stored and takes effect on the next restart, so there
        // is genuinely nothing left to do.
        return;
    }
    // OD-PORT: no piecemeal rebuild. Upstream has to reconstruct the whole
    // NimBLEAdvertisementData (name, flags, MSD) and re-push it, because
    // setAdvertisementData() must be the LAST advertising-data call or NimBLE
    // discards the payload -- and it has to re-derive the device name from
    // getChipIdHex() to do it, since the object does not retain one. The C API has
    // neither problem: ble_gap_adv_set_fields() takes the whole record at once and
    // od_ble owns the name it was initialised with, so storing the MSD and
    // restarting is the entire operation. The 50 ms settle upstream needs between
    // stop() and start() is likewise gone -- od_ble_restart_advertising() issues
    // ble_gap_adv_stop() and ble_gap_adv_start() synchronously on the caller.
    od_ble_restart_advertising();
}

// Match nRF's link tuning: 2M PHY + 251-octet DLE. Like nRF, the peripheral only
// auto-accepts what the central asks for, so without this the link stays at
// 1M / 27 octets whenever the phone does not request better.
//
// Called from loop() when the connect event is consumed, not from the connect
// callback -- these are host-stack calls, which the callback contract excludes.
void BleTransport::requestFastLink() {
    // Tune the OWNER's link. This used to read the single s_connHandle scalar,
    // which the newest connect overwrote -- so with a contender attached, link
    // tuning targeted the wrong link (one of the shared-scalar defects R3 names).
    if (!s_ready) return;
    const uint32_t owner = linkOwnerWord();
    if (owner == 0) return;
    const LinkId id = linkUnpackWord(owner);
    if (id.who != OWNER_BLE) return;
    od_ble_request_fast_link(id.handle);
    // Unlike upstream, the granted values ARE reported: BLE_GAP_EVENT_PHY_UPDATE_-
    // COMPLETE and BLE_GAP_EVENT_MTU both land on od_ble_evt_link_negotiated()
    // above, so no delayed one-shot is needed to see what the peer agreed to.
}

void BleTransport::tick() {
    // No-op: nothing periodic to restore. This target has no advertising-interval states --
    // see od_adv_app_boost() in ble/od_ble_nimble.cpp.
}

bool BleTransport::eventPending() const {
    // RELAXED: a non-destructive peek used to decide whether to return to loop(),
    // never to establish ordering. Reading one pass stale is harmless -- the next
    // pass sees it.
    return __atomic_load_n(&s_connectedEvent, __ATOMIC_RELAXED) ||
           __atomic_load_n(&s_disconnectedEvent, __ATOMIC_RELAXED);
}

bool BleTransport::takeConnectedEvent(uint32_t* instanceWord) {
    // Atomic exchange, not check-then-clear. The old form could lose an event that
    // arrived inside the gap. ACQUIRE pairs with the callback's RELEASE store of
    // the flag, which it makes after writing the payload, so the payload we read is
    // at least fully written.
    //
    // It does NOT bind the payload to the flag we just consumed: acquire/release
    // orders writes that PRECEDE the release, and nothing freezes the payload
    // afterwards, so a second connect landing between the exchange and the load
    // below hands us ITS word instead. That is tolerable only because no decision
    // depends on it -- teardown and connect-side work are both derived from the
    // owner token and the instance table, which are authoritative. The payload is
    // diagnostic. Do not build a decision on it without binding it properly.
    if (!__atomic_exchange_n(&s_connectedEvent, false, __ATOMIC_ACQUIRE)) return false;
    if (instanceWord != nullptr) {
        *instanceWord = __atomic_load_n(&s_connectedWord, __ATOMIC_RELAXED);
    }
    return true;
}

bool BleTransport::takeDisconnectedEvent(uint16_t* reason, uint32_t* instanceWord) {
    // See takeConnectedEvent(), including the caveat: the exchange stops events
    // being lost in the old check-then-clear gap, but does not bind this payload to
    // the flag just consumed. The reason code below is therefore diagnostic -- a
    // burst of disconnects can report the latest reason twice. Teardown decides on
    // table state, not on this.
    if (!__atomic_exchange_n(&s_disconnectedEvent, false, __ATOMIC_ACQUIRE)) return false;
    if (reason != nullptr) *reason = __atomic_load_n(&s_disconnectReason, __ATOMIC_RELAXED);
    if (instanceWord != nullptr) {
        *instanceWord = __atomic_load_n(&s_disconnectedWord, __ATOMIC_RELAXED);
    }
    return true;
}

bool BleTransport::restartsAdvertisingOnDisconnect() const {
    // NimBLE does not re-advertise by itself here; the application schedules it
    // via requestAdvertisingRestart() so the restart can be held off while an
    // EPD refresh is mid-flight.
    return false;
}

const char* BleTransport::addressString() {
    // The advertised BLE address, lowercase colon-separated (SECTION 9 rule 6,
    // key `mac`).
    //
    // Sourced from od_ble, which knows which address type ble_hs_id_infer_auto()
    // actually selected. Upstream reads NimBLEDevice::getAddress() and carries a
    // "HARDWARE VALIDATION REQUIRED: confirm this is the advertised AdvA (public vs
    // static-random)" note against it; that question is answered here rather than
    // deferred, because the C API makes the resolved type readable. Reading
    // BLE_ADDR_PUBLIC unconditionally publishes the wrong MAC on any unit that
    // advertises a static-random address, and returns empty on one with no public
    // address at all -- and the LAN mDNS TXT record has to publish the SAME address
    // the advertisement uses or a host cannot correlate BLE with LAN.
    static char cached[18] = {0};
    uint8_t addr[6] = {0};
    uint8_t addr_type = 0;
    if (!od_ble_get_identity_addr(addr, &addr_type)) {
        cached[0] = '\0';
        return cached;
    }
    // NimBLE stores the address little-endian; print it MSB-first, lower-case,
    // which is the form Home Assistant matches on.
    snprintf(cached, sizeof cached, "%02x:%02x:%02x:%02x:%02x:%02x",
             addr[5], addr[4], addr[3], addr[2], addr[1], addr[0]);
    return cached;
}
