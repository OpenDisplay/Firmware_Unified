#include "od_smp.h"
#include "main.h"
#include "od_display_esp.h"
#include "boot_screen.h"
#include "buzzer_control.h"
#include "communication.h"
#include "device_control.h"
#include "display_service.h"
#include "power_latch.h"
#include "wake_button.h"
#include "rtc_pcf8563.h"
#include "touch_input.h"
#include "encryption.h"
#include "ble_transport.h"
#include "link_owner.h"
#include "session_guard.h"
#include "od_log.h"
#include "od_watchdog_app.h"
#include "od_config_read.h"
#include "od_txq.h"

/* The log port itself lives in hal/od_hal_log.c now -- including the UART-number choice and
 * the reason for it, which moved there with the code it describes. This file used to hold a
 * `static HardwareSerial LogSerialPort` and hand it to od_log_init() as a `Stream *`; the
 * pin/UART defines and compat/HardwareSerial.h went with it. */
#include "od_hal_log.h"
/* od_ble_service_advertising(): the loop-side advertising pump (F4). ESP32-only -- the nRF
 * target's stack re-arms advertising itself and has no equivalent seam yet. */
#include "od_ble.h"
#include "od_hal_i2c.h"
#include "od_hal_i2c_esp.h"
#include "od_hal_gpio.h"
#include "driver/gpio.h"
#include "od_hal_time.h"
#include "od_hal_sleep.h"

#include "esp_heap_caps.h"
#include "sdkconfig.h"
#if CONFIG_PM_ENABLE
#include "esp_pm.h"
#endif

// Distinguishes a hidden mid-cycle reset (PANIC/WDT/BROWNOUT/SW) from a real
// power-on or deep-sleep wake; any reset here clears the wake cause, so the
// next boot takes the NORMAL BOOT branch and redraws the boot screen.
static const char* resetReasonName(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_POWERON:   return "POWERON";
        case ESP_RST_EXT:       return "EXT";
        case ESP_RST_SW:        return "SW";
        case ESP_RST_PANIC:     return "PANIC";
        case ESP_RST_INT_WDT:   return "INT_WDT";
        case ESP_RST_TASK_WDT:  return "TASK_WDT";
        case ESP_RST_WDT:       return "WDT";
        case ESP_RST_DEEPSLEEP: return "DEEPSLEEP";
        case ESP_RST_BROWNOUT:  return "BROWNOUT";
        case ESP_RST_SDIO:      return "SDIO";
        default:                return "UNKNOWN";
    }
}

// Defined with the sleep helpers below loop()'s activity poller; setup() logs
// the window length when arming the button-wake hold.
static uint32_t minWakeTimeMs();

/* One heap report, two lines, DRAM and PSRAM measured identically.
 *
 * THE TWO POOLS MUST NOT BE SUMMED. Internal DRAM is the scarce one -- ~170 KB free at boot on
 * an S3 against 8 MB of PSRAM -- so any combined figure is a PSRAM figure with the interesting
 * number rounded away. Before this was split, the log read "free=8437776 min=8437724", which is
 * how internal exhaustion measured at 48-264 B upstream stayed invisible for a week.
 *
 * FOUR FIELDS PER POOL, because "free" alone does not say whether an allocation will succeed:
 *   free     currently available
 *   min      low-water mark since boot -- the only field that survives a transient squeeze,
 *            and the one dc60c8a's open item asks about ("expect ~16 KB where it was 48-264 B")
 *   largest  largest CONTIGUOUS block; exhaustion shows up here as fragmentation first, so a
 *            healthy `free` with a small `largest` still fails a big malloc
 *   total    pool capacity, so the other three are interpretable without knowing the variant
 *
 * The PSRAM line is SUPPRESSED where there is no PSRAM (C3, C6, classic ESP32 here) rather than
 * printed as zeros: a line of zeros reads as "PSRAM is broken" on a part that never had any. */
/* heap_caps_* directly rather than ESP.getFreeHeap()/getMinFreeHeap()/getFreePsram(): those
 * shim accessors were DEFINED as exactly these three calls (see compat/arduino_compat.cpp,
 * which documents why they are MALLOC_CAP_INTERNAL and not MALLOC_CAP_DEFAULT). Two of the four
 * fields on each line already called heap_caps_* directly, so this also stops one log line
 * sourcing the same pool through two different spellings. */
static void logHeapUsage(const char *when)
{
    od_log_info("Heap %s DRAM : free=%u min=%u largest=%u of %u", when,
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
                (unsigned)heap_caps_get_total_size(MALLOC_CAP_INTERNAL));

    const size_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    if (psram_total == 0) {
        return;   /* no PSRAM on this part -- see above */
    }
    od_log_info("Heap %s PSRAM: free=%u min=%u largest=%u of %u", when,
                (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_SPIRAM),
                (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
                (unsigned)psram_total);
}

void setup() {
    #if defined(OPENDISPLAY_LOG_UART)
    od_hal_log_open();
    od_hal_delay_ms(100);
    #elif !defined(DISABLE_USB_SERIAL)
    od_hal_delay_ms(100);
    #endif
    #if defined(OPENDISPLAY_LOG_UART)
    od_log_init();
    #elif !defined(DISABLE_USB_SERIAL)
    /* The USB-CDC port needs no begin() -- the IDF console driver is already up, which is why
     * the Serial.begin(115200) above was a no-op too -- but od_hal_log still has to be told it
     * may write, so that a DISABLE_USB_SERIAL build (which reaches neither branch) stays a
     * genuine no-op rather than writing to a port nobody asked for. */
    od_hal_log_open();
    od_log_init();
    #endif
    od_log_info("=== FIRMWARE INFO ===");
    uint8_t fwMajor = getFirmwareMajor();
    uint8_t fwMinor = getFirmwareMinor();
    uint8_t fwPatch = getFirmwarePatch();
    od_log_info("Firmware Version: %u.%u.%u", fwMajor, fwMinor, fwPatch);
    /* SHA_STRING is a build stamp of at most 40 hex characters, arriving with or without
     * surrounding quotes depending on how -DSHA was passed. Two heap-allocating String
     * operations (construct, substring) to strip at most two quote characters bought nothing;
     * the same job on a fixed buffer cannot fail or allocate. Same rewrite communication.cpp's
     * handleFirmwareVersion() got in step 9a, and for the same reason. */
    const char* shaCStr = SHA_STRING;
    char shaStr[48];
    snprintf(shaStr, sizeof(shaStr), "%s", shaCStr ? shaCStr : "");
    size_t shaLen = strlen(shaStr);
    if (shaLen >= 2 && shaStr[0] == '"' && shaStr[shaLen - 1] == '"') {
        memmove(shaStr, shaStr + 1, shaLen - 2);
        shaStr[shaLen - 2] = '\0';
        shaLen -= 2;
    }
    if (shaLen > 0) {
        od_log_info("Git SHA: %s", shaStr);
    } else {
        od_log_info("Git SHA: (not set)");
    }
    // Set only by the ESP32 wake-cause check below; NRF has no deep-sleep wake path.
    bool is_deep_sleep_wake = false;
    bool woke_by_button = false;
    esp_reset_reason_t reset_reason = esp_reset_reason();
    const char* resetReasonStr = resetReasonName(reset_reason);
    od_log_info("Reset reason: %s (%d)", resetReasonStr, (int)reset_reason);
    esp_sleep_wakeup_cause_t wakeup_reason = esp_sleep_get_wakeup_cause();
    is_deep_sleep_wake = (wakeup_reason != ESP_SLEEP_WAKEUP_UNDEFINED);
    if (is_deep_sleep_wake) {
        woke_from_deep_sleep = true;
        deep_sleep_count++;
        od_log_info("=== WOKE FROM DEEP SLEEP ===");
        woke_by_button = detectButtonWake(wakeup_reason);  // logs the named cause + pin(s)
        od_log_info("Deep sleep count: %u", (unsigned)deep_sleep_count);
    } else {
        woke_from_deep_sleep = false;
        od_log_info("=== NORMAL BOOT ===");
        // The bootloader reloads RTC memory segments from the app image on every
        // reset except a deep-sleep wake, so RTC_DATA_ATTR does NOT survive
        // panic/WDT/SW/brownout resets: a hidden mid-cycle reset lands here with
        // count 0, indistinguishable from a true first boot (captured on hardware
        // in docs/FINDINGS_DEEP_SLEEP_WAKE_BOOT_SCREEN_2026-07-07.md).
        od_log_info("Deep sleep count (RTC): %u", (unsigned)deep_sleep_count);
    }
    /* Before anything can touch the panel, and before the watchdog is armed: this decides
     * whether the previous run ended in a watchdog reset, and whether three of them in a row
     * mean this boot must skip the panel entirely. */
    od_watchdog_app_boot();
    od_watchdog_app_arm();
    od_log_info("Starting setup...");
    if (is_deep_sleep_wake) { od_log_info("[wake] >> full_config_init"); od_log_flush(); }
    full_config_init();
#ifdef OPENDISPLAY_HAS_WIFI
    // Reserve mbedTLS's two ~16.7 KB record buffers HERE and nowhere else: config is
    // loaded (so we know whether TLS is even used) but ble.begin() and initWiFi() have not
    // yet taken their ~100 KB, so internal DRAM is still contiguous. mbedtls_ssl_setup()
    // needs both buffers contiguous and cannot be satisfied later on a churned heap --
    // observed failing with -0x7f00 at 51 KB free / 31.7 KB largest block. No-op when
    // encryption is disabled.
    od_tls_reserve_records();
    // Strictly after the TLS slots: this prefers PSRAM and normally costs no internal
    // DRAM at all, but on a board whose PSRAM is absent or dead it falls back to 16 KB
    // of internal -- which must not land in the middle of the contiguous region the
    // reservation above depends on. One call site serves both the normal-boot and the
    // deep-sleep-wake path, which share this stretch of setup().
    odLanReserveRxBuffer();
#endif
    // A cold boot raised by the RTC (power-off sleep, see enterDeepSleep()) is a
    // timer wake in all but the ESP32's own wake cause. Always called when an RTC
    // is configured: it also clears the flag that holds INT (and the rail) on.
    // RTC memory died with the rail, so displayed_etag is 0 (the host sends a
    // full image) and rebootFlag stays clear -- this is not a reboot.
    if (rtcConsumeWake() && !is_deep_sleep_wake) {
        is_deep_sleep_wake = true;
        woke_from_deep_sleep = true;
        deep_sleep_count++;
        rebootFlag = 0;
        od_log_info("=== WOKE FROM RTC POWER-OFF ===");
    }
    if (is_deep_sleep_wake) { od_log_info("[wake] << full_config_init >> initio"); od_log_flush(); }
    initio();
    if (!is_deep_sleep_wake) {
        // Arm here rather than at declaration: this branch is the boot screen
        // redraw, and every real reset (power-on, panic, WDT, SW) clears the
        // wake cause and lands here. A deep-sleep wake skips it and keeps the
        // pre-sleep flag, so a wake never advertises as a reboot.
        rebootFlag = 1;
        // Wake keeps the panel image; skipping initDisplay() (EPD rail power +
        // full refresh) is the wake path's main energy saving.
        initDisplay();
        od_log_info("Display initialized");
    }
    // Full BLE after display: ESP32 queues commands for loop() until setup returns.
    if (is_deep_sleep_wake) { od_log_info("[wake] >> ble_begin"); od_log_flush(); }
    {
        char bleDeviceName[3 + OD_CHIP_ID_HEX_LEN + 1] = "OD";
        getChipIdHex(bleDeviceName + 2, sizeof(bleDeviceName) - 2);
        if (ble.begin(bleDeviceName)) {
            // Historical order: build the manufacturer data into the advertisement
            // BEFORE the first start(), since setAdvertisementData() must be the
            // last data call before start() (see ble_transport_esp32.cpp).
            updatemsdata();
            ble.startAdvertising();
            od_log_info("Device ready: %s", bleDeviceName);
            od_smp_confirm_running();
            od_log_info("Waiting for BLE connections...");
        }
    }
    if (is_deep_sleep_wake) { od_log_info("[wake] << ble_begin"); od_log_flush(); }
    #ifdef OPENDISPLAY_HAS_WIFI
    if (!is_deep_sleep_wake) {
        initWiFi(false);  // wake: WiFi stays deferred to fullSetupAfterConnection()
    }
    #endif
    updatemsdata();
    if (is_deep_sleep_wake) { od_log_info("[wake] >> initButtons"); od_log_flush(); }
    initButtons();
    if (woke_by_button) {
        buttonWakeDeliverSyntheticClick();
    }
    if (is_deep_sleep_wake) { od_log_info("[wake] >> initTouchInput"); od_log_flush(); }
    initTouchInput();
    if (is_deep_sleep_wake) {
        // Arm the awake window LAST so buttons/GT911 bring-up doesn't shrink the
        // host's connection window. Without this, loop() falls into the idle
        // branch and re-enters deep sleep almost immediately.
        od_log_info("Advertising for %u ms (sleep_timeout_ms), waiting for connection...", globalConfig.power_option.sleep_timeout_ms);
        advertising_timeout_active = true;
        advertising_start_time = od_hal_uptime_ms();
        if (woke_by_button) {
            // A button press means a user is present: hold awake for at least
            // the minimum window so they (or a host) get time to interact.
            minWakeWindowActive = true;
            minWakeWindowStartMs = od_hal_uptime_ms();
            uint32_t minWakeMs = minWakeTimeMs();
            od_log_info("Button wake: holding awake >= %u ms", (unsigned)minWakeMs);
        }
    } else if (deep_sleep_count == 0) {
        // First boot — or a hidden mid-cycle reset, which reloads the RTC count
        // to 0 (see the NORMAL BOOT comment above). Inert on wired devices:
        // every consumer of the hold is power_mode/deep-sleep gated.
        minWakeWindowActive = true;
        minWakeWindowStartMs = od_hal_uptime_ms();
    }
    // Both sleep paths measure quiet time from here, not from power-on.
    lastActivityMs = od_hal_uptime_ms();
    od_log_info("=== Setup completed successfully ===");
    logHeapUsage("(setup done)");
}

extern "C" uint32_t od_hal_log_cycle_count(void) {
    return deep_sleep_count;
}

// Deferred work, serviced by loop(). File-static on purpose: these encode
// application policy, so nothing outside this file reads them, and the two that
// other translation units need to RAISE do so through the request functions
// below (declared in communication.h) rather than by touching the state.
//
// Declared here, above pollActivity(), because that reads the advertising flag
// as its only trace of a connect+drop landing entirely inside one loop pass.
//
// Not volatile: every writer runs on the loop task. That became true in Phase 3,
// when nRF's stack callbacks stopped running application code -- before that the
// disconnect path executed on the SoftDevice callback task.
static bool s_disconnectCleanupPending = false;
// The owner word as it stood when the cleanup was requested. The flag alone is not
// enough: it is a bare boolean shared by the BLE and LAN teardown paths, so by the
// time it is serviced -- deferred past a refresh, possibly several passes later --
// the session it was raised for may already have been torn down by some OTHER path
// (the idle timeout or the transfer watchdog, both of which abort and release).
// Servicing it then would run a destructive teardown against whoever holds the slot
// NOW, resetting a freshly admitted client's crypto and rings and stalling it.
//
// Recording the identity turns "something disconnected" into "THIS session
// disconnected", which is the only form the abort can act on safely.
static uint32_t s_cleanupForOwner = 0;
static bool s_advertisingRestartPending = false;
static bool s_msdUpdatePending = false;

void requestTransferSessionCleanup(void) {
    // Capture the owner here rather than at service time: this is the moment the
    // departing session is still identifiable. Callers on the LAN path still hold
    // the token at this point (release is the abort's final step), and the BLE
    // raise site does the same.
    s_cleanupForOwner = linkOwnerWord();
    s_disconnectCleanupPending = true;
}

void requestAdvertisingRestart(void) {
    s_advertisingRestartPending = true;
}

void notifyConfigChangedOverBle(void) {
    // A config write changes the same on-device state a reboot does, so it raises the same flag.
    rebootFlag = 1;
    updatemsdata();
}

// Minimum awake window (first boot / button wake). A floor layered UNDER the
// quiet-window logic, not a replacement: sleep requires both the existing
// idle/advertising quiet condition AND this hold expired, so interaction keeps
// extending the quiet window inside and beyond the floor. Timer wakes never
// arm the hold — their behavior is unchanged.
static uint32_t minWakeTimeMs() {
    uint16_t s = globalConfig.power_option.min_wake_time_seconds;
    return (uint32_t)(s ? s : DEFAULT_MIN_WAKE_TIME_SECONDS) * 1000UL;
}

static bool minWakeHoldActive() {
    if (!minWakeWindowActive) return false;
    if (od_hal_uptime_ms() - minWakeWindowStartMs >= minWakeTimeMs()) {
        minWakeWindowActive = false;
        od_log_info("Minimum wake window elapsed, deep sleep permitted");
        return false;
    }
    return true;
}

// Single point of activity detection. Rather than have every producer (BLE host
// task, buttons, touch, LAN) stamp a timestamp, sample the state they already
// mutate and treat any change since the previous pass as activity.
//
// Runs at the top of loop() so an event raised during the previous pass always
// lands before this pass decides to sleep.
static void pollActivity() {
    static bool activityPrimed = false;
    static uint8_t prevCommandHead = 0;
    static uint16_t prevResponseHead = 0;
    static uint8_t prevConnCount = 0;
    static bool prevLanSession = false;
    static uint8_t prevDynamic[sizeof(dynamicreturndata)] = {0};

    // The RX head is producer-side, so a command that arrived and drained within a single pass
    // still registers. It wraps mod OD_RXQ_SLOTS, but aliasing needs a whole queue of
    // traffic inside one pass, and queues only fill while a client is connected — which stamps
    // below regardless. od_txq's depth does not wrap, so the TX term needs no such argument.
    const uint8_t commandHead = od_rxq_head();
    const uint16_t responseHead = od_txq_depth();
    // Covers connect and disconnect. The disconnect edge is what re-arms the
    // window so a dropped client gets a full reconnect opportunity.
    const uint8_t connCount = ble.connectedCount();
#ifdef OPENDISPLAY_HAS_WIFI
    const bool lanSession = wifiInitialized && wifiLanClientConnected();
#else
    const bool lanSession = false;
#endif

    if (!activityPrimed) {
        activityPrimed = true;
    } else if (commandHead != prevCommandHead ||
               responseHead != prevResponseHead ||
               connCount != prevConnCount ||
               lanSession != prevLanSession ||
               // Button presses and touch events land here before advertising.
               memcmp(prevDynamic, dynamicreturndata, sizeof(prevDynamic)) != 0 ||
               // Set by onDisconnect and cleared further down this loop, so it is
               // the only trace of a connect+drop that lands entirely between two
               // passes — connCount reads 0 on both sides of such a blip.
               s_advertisingRestartPending ||
               // A live link or unfinished work is activity in itself, not just its edges.
               connCount > 0 || lanSession ||
               od_rxq_pending() || od_txq_depth() > 0u) {
        lastActivityMs = od_hal_uptime_ms();
    }

    prevCommandHead = commandHead;
    prevResponseHead = responseHead;
    prevConnCount = connCount;
    prevLanSession = lanSession;
    memcpy(prevDynamic, dynamicreturndata, sizeof(prevDynamic));
}

// The BLE session helpers below are portable as of Phase 3: both targets now
// dispatch commands and service connect/disconnect from loop().

// Services the deferred BLE-disconnect session teardown flagged by
// the BLE transport's disconnect event. Runs on the loop() task so the heavyweight
// EPD force-off (sleep sequence, bleed, bus release, rail cut) and partial/pipe cleanup never
// race SPI streaming or pipe-frame processing on the stack callback task.
// Deferred while a refresh is mid-flight; the flag stays set until a later pass
// clears it. Also raised by the LAN transport, so it is not a BLE-only path.
static void serviceBleDisconnectCleanup() {
    if (!s_disconnectCleanupPending || epdRefreshInProgress) return;
    const uint32_t forOwner = s_cleanupForOwner;
    s_disconnectCleanupPending = false;
    s_cleanupForOwner = 0;
    // Act only if the slot still holds exactly what it held when this was raised.
    // Any difference means that session has already been torn down and released by
    // another path -- the idle timeout and the transfer watchdog both abort and
    // release -- so there is nothing left for this to do, and proceeding would
    // apply the teardown to whoever holds the slot now.
    if (forOwner == 0) {
        // Raised while nothing owned the slot -- restartWiFiLanAfterReconnect()
        // calls disconnectWiFiServer() unconditionally, so this is routine. Zero is
        // not a session identity and must not authorise a destructive teardown: a
        // BLE claim landing between the test below and the abort would have its
        // fresh crypto and rings reset, and the abort's release (passed NONE) would
        // not even free the slot afterwards. There is by definition no session to
        // tear down here; genuinely orphaned transfer state is healed by the
        // orphaned-pipe repair in checkTransferTimeouts().
        od_log_debug("Disconnect cleanup skipped: raised with no owner");
        return;
    }
    if (linkOwnerWord() != forOwner) {
        od_log_debug("Disconnect cleanup skipped: slot changed hands since it was raised");
        return;
    }
    // BLE and LAN both raise this flag, so tear down only when the transport that
    // OWNS THE SLOT is the one that went away. Otherwise a BLE disconnect kills a
    // live LAN push (and a LAN disconnect kills a BLE push) purely because the
    // other link dropped.
    //
    // The question is asked of the OWNER TOKEN and the instance table, not of
    // transferSessionOrigin() plus ble.isConnected() as it used to be. Two reasons,
    // and the second is a live defect the aggregate count would reintroduce:
    //
    //  - The token is authoritative about who holds the slot, whereas
    //    transferSessionOrigin() only says who started the last transfer -- so the
    //    old test could not answer the question at all when no transfer was running,
    //    which is exactly when a stranded token matters most.
    //  - ble.isConnected() is the stack's TOTAL peer count. With a refused contender
    //    transiently attached (which R1 explicitly permits), it stays true after the
    //    owner leaves, so the old guard would skip the cleanup and the slot would
    //    never be released -- every later client refused service until reboot. The
    //    per-instance instanceLive() test is immune to that.
    //
    // The comparison also means a LOST disconnect edge cannot strand the slot: if
    // the owner's instance is no longer live in the table, the owner is gone,
    // however many events coalesced away while loop() sat in a refresh.
    const LinkId owner = linkOwnerId();
    bool ownerStillUp = false;
    if (owner.who == OWNER_BLE) {
        ownerStillUp = ble.instanceLive(owner.handle, owner.epoch);
#ifdef OPENDISPLAY_HAS_WIFI
    } else if (owner.who == OWNER_LAN) {
        ownerStillUp = wifiLanClientConnected();
#endif
    }
    if (ownerStillUp) {
        od_log_info("Disconnect cleanup skipped: slot still held by a live %s session",
                    owner.who == OWNER_BLE ? "BLE" : "LAN");
        return;
    }
    // Route the teardown through the ONE shared routine (R6). Keeping a second
    // teardown path here is exactly how the direct-write watchdog once tore down a
    // panel while leaving its pipe session live.
    //
    // dropLink=false: the link is already gone, which is what the test above establishes. The
    // abort's own release is its last step, so the slot frees here rather than at the event. With
    // the slot already unowned this is still worth running: it is idempotent, and it clears a
    // transfer left behind by a session whose token was released on some other path.
    abortToKnownState("owner disconnected", false, owner);
}

// Deferral policy for re-arming the radio, formerly buried inside
// esp32_restart_ble_advertising(). BleTransport::restartAdvertising() is
// unconditional by contract; deciding *when* to call it is an application
// concern (mid-refresh, already reconnected, stack not up), so it lives here
// alongside the other loop()-serviced BLE helpers. The flag stays raised on the
// "not yet" paths so a later pass retries.
static void serviceBleAdvertisingRestart() {
    if (!s_advertisingRestartPending) return;
    // Capability gate, and the reason this helper is safe for ANY caller to
    // raise the flag: where the stack re-arms advertising itself (nRF's
    // restartOnDisconnect(true)), driving our own stop()/start() would fight it.
    // Refusing here rather than at each raise site means a new raiser -- the
    // post-refresh hook in display_service.cpp, or a future portable
    // requestAdvertisingRestart() -- cannot reintroduce that conflict by
    // forgetting a target guard.
    if (ble.restartsAdvertisingOnDisconnect()) {
        s_advertisingRestartPending = false;
        return;
    }
    if (!ble.isReady()) return;                              // stack down; retry later
    if (ble.isConnected()) {                                 // a client beat us to it
        s_advertisingRestartPending = false;
        return;
    }
    if (epdRefreshInProgress) return;                        // never mid-refresh
    s_advertisingRestartPending = false;
    ble.restartAdvertising();
    updatemsdata();
}

// Translate the transport's consume-once connect/disconnect events into the
// application's deferred-work flags, and do the connect-side work that used to
// run inline on the stack callback task. Must run before anything that reads
// those flags -- including pollActivity(), which uses
// s_advertisingRestartPending as its only trace of a connect+drop landing
// entirely between two passes.
static void serviceBleEvents() {
    uint32_t connectedWord = 0;
    (void)ble.takeConnectedEvent(&connectedWord);
    // Connect-side work is driven by OWNERSHIP, not by the event.
    //
    // Only the owner's connect may drive it: a refused contender must not perturb
    // incumbent-visible state (R3), and every item below is exactly that -- reboot
    // state, an MSD rebuild that polls I2C and republishes the advertisement, and
    // link tuning aimed at the owner's link.
    //
    // But gating on the EVENT's identity is not enough either. The event word is a
    // single slot: if the owner connects and a contender connects before the loop
    // consumes the flag, the word holds only the contender, and the owner's connect
    // work would be lost for the life of the session -- no fast link, no MSD update.
    // Comparing against the last owner this work ran for is immune to any number of
    // coalesced edges, which is the same "state, not events" argument that made the
    // instance table a table.
    static uint32_t s_connectWorkDoneFor = 0;
    const uint32_t owner = linkOwnerWord();
    const LinkId ownerId = linkUnpackWord(owner);
    if (owner != 0 && ownerId.who == OWNER_BLE && owner != s_connectWorkDoneFor &&
        ble.instanceLive(ownerId.handle, ownerId.epoch)) {
        s_connectWorkDoneFor = owner;
        rebootFlag = 0;
        s_msdUpdatePending = true;
        // SoftDevice PHY/DLE calls on nRF, no-op on ESP32. Deliberately here and
        // not in the connect callback: the callback contract is copy-and-flag only
        // (plus the one claim CAS, which is a single word write).
        ble.requestFastLink();
    }
    uint16_t disconnectReason = 0;
    uint32_t disconnectedWord = 0;
    if (ble.takeDisconnectedEvent(&disconnectReason, &disconnectedWord)) {
        // 0x%03X so a wrapped NimBLE HCI reason (0x213) and a host-layer one (0x007)
        // are visibly distinct. Printed as decimal %u from a truncated byte, they
        // used to collide on screen as well as in storage.
        od_log_info("Disconnect reason: 0x%03X", (unsigned)disconnectReason);
        // No RX flush here any more. Frames carry their writer's identity, so
        // serviceBleRx() drops a departed session's frames at dispatch -- which also
        // covers the case this site could never handle: a boundary lost because the
        // stack reused the handle before the loop got here.
        //
        // Raise the flag; do NOT tear the session down here. The teardown belongs
        // in serviceBleDisconnectCleanup(), which holds it off while an EPD
        // refresh is mid-flight and checks whether LAN still owns the transfer.
        // Doing it inline would reintroduce the mid-refresh SPI teardown that
        // moving nRF off the callback task was meant to eliminate.
        //
        // Raised on OWNER DEPARTURE, not on any disconnect event. R3 requires a
        // refused contender's disconnect to be inert, and refusal now produces a
        // real disconnect event of its own -- so an unconditional raise here would
        // make every refusal schedule a session teardown. That teardown is skipped
        // a pass later by the live-owner guard, but "correct because something
        // downstream catches it" is exactly the coupling R3 forbids.
        //
        // The test is state, not the event's identity, so it survives coalescing:
        // if the token's BLE owner no longer has a live table entry, the owner is
        // gone however many edges were lost. A LAN owner's departure raises the
        // same flag from its own path (requestTransferSessionCleanup).
        const LinkId tokenOwner = linkOwnerId();
        const bool ownerDeparted =
            (tokenOwner.who == OWNER_BLE &&
             !ble.instanceLive(tokenOwner.handle, tokenOwner.epoch));
        if (ownerDeparted) {
            requestTransferSessionCleanup();   // records the identity it is for
        } else {
            od_log_debug("Disconnect event from a non-owner instance; no cleanup scheduled");
        }
        // Raised unconditionally: serviceBleAdvertisingRestart() owns the
        // capability decision, so this site does not need to know whether the
        // stack re-arms the radio by itself. On such a target the flag is simply
        // cleared unserviced, later in this same pass.
        s_advertisingRestartPending = true;
    }
}

// Disconnect every live BLE instance that is not the owner (CONNECTION_POLICY R3).
//
// SCOPE NOTE. The freeze-hardening plan assigns refusal to Phase 3, and this is
// Phase 2. It is here because Phase 2 is not safely shippable without it: admission
// is decided once per instance, at its connect hook, and never revisited, so a
// client that reconnects into a still-held slot -- the ordinary case when loop()
// was blocked in a ~16 s refresh -- becomes a permanent contender. On nRF, whose
// single peripheral link it now occupies, nothing else can connect either, so the
// device is unreachable until that client happens to leave. The two alternatives
// were worse: releasing the token in the disconnect callback admits a new owner
// while the departed session's transfer, crypto and TX ring are still live, and
// promoting a contender from this scan is exactly what 7a row 10 forbids.
//
// What is NOT here is the rest of Phase 3: no idle timeout, no reclaim of a held
// slot. This only makes refusal actually happen, which is what the "decided once"
// rule assumes.
//
// A TABLE SCAN, not an event handler: a refusal missed because two connects
// coalesced self-corrects on the next pass, where an event-driven version would
// leak the contender permanently. Refusal is idempotent and inert -- re-refusing an
// entry already tearing down costs nothing, and NimBLE reports "already gone" as
// success -- so no bookkeeping is needed to avoid repeats.
//
// Refusal touches NOTHING but the contender's own link: no abort, no
// s_disconnectCleanupPending, no linkRelease. The incumbent must be unable to
// observe that a contender arrived, which is why this is a separate helper rather
// than a branch inside the disconnect path it would otherwise resemble.
static void serviceContenderRefusal() {
    // Refuse an entry only once its claim has been DECIDED and it is not the owner.
    //
    // Both halves of that test are load-bearing, and each replaces a wrong rule:
    //
    //  - Testing ownership alone would disconnect the winner. The connect callback
    //    publishes its table entry BEFORE attempting the claim (R2's normative
    //    order), so an entry can be visible while its CAS has not run; comparing it
    //    against an owner word snapshotted moments earlier can refuse the very
    //    connection that is taking the slot.
    //  - Skipping the scan whenever the slot is unowned -- an intermediate fix --
    //    leaves a decided loser attached forever. That is exactly the sequence this
    //    whole helper exists for: the owner departs, the abort releases the token,
    //    and the client that reconnected and lost its one-shot claim is then never
    //    reaped, because by the time anyone looks the slot is free. On nRF it holds
    //    the only peripheral link, so the device stops accepting clients entirely.
    //
    // An undecided entry is simply skipped; the next pass sees it resolved. That is
    // safe because refusal has no deadline -- a contender's writes are already
    // filtered and its frames already fail the dispatch tag check.
    // ORDER OF THE THREE LOADS IS THE CORRECTNESS ARGUMENT. They cannot be taken
    // atomically, so each is re-derived in the order that makes a stale read safe:
    //
    //   1. the entry word          -- the candidate's identity
    //   2. its decided-for word    -- must EQUAL (1), which proves the claim
    //                                 resolved for this exact instance and that the
    //                                 slot was not retired and reused underneath us
    //   3. the owner word, read FRESH and last -- if the candidate just won, this
    //                                 now names it and the entry is skipped
    //
    // Reading the owner once up front (as an earlier version did) is the bug: an
    // entry can publish, win its claim and resolve between that snapshot and the
    // per-entry loads, and would then be refused despite being the new owner. After
    // step 3 the candidate can no longer become owner, because each instance
    // attempts its claim exactly once and step 2 proved that attempt is over.
    const uint8_t cap = BleTransport::instanceCapacity();
    for (uint8_t i = 0; i < cap; i++) {
        const uint32_t w = ble.instanceWordAt(i);
        if (w == 0) continue;
        if (ble.instanceClaimDecidedWordAt(i) != w) continue;   // in flight, or slot reused
        if (w == linkOwnerWord()) continue;                     // it won; not a contender
        const LinkId id = linkUnpackWord(w);
        od_log_info("Refusing contender h=%u e=%u (slot held)", (unsigned)id.handle,
                    (unsigned)id.epoch);
        (void)ble.disconnect(id.handle, id.epoch);
    }
}

// How long an ADMITTED client may stay silent before its link is dropped and the
// slot reclaimed (CONNECTION_POLICY R4, 7c row 1).
//
// CLIENT BEHAVIOUR THIS ASSUMES: py-opendisplay authenticates within one exchange
// of connecting and, during a transfer, never leaves more than a few seconds
// between frames. 120 s is therefore two orders of magnitude above any legitimate
// inter-command gap. If a client change ever pushes real silence toward this
// value, the assertion in py-opendisplay's suite is what should fail first, not a
// field report.
//
// WHY SO GENEROUS -- the direction of the error inverted when R4 landed, so this
// is not an oversight in the other direction:
//   - While the drop was gated on !transferActive(), a short timeout could only
//     ever kill an idle session, so erring short was cheap.
//   - R4 removed that gate. This can now terminate an in-progress UPLOAD whose
//     client went quiet, so erring short costs a legitimate transfer.
// The accepted cost is bounded and lands on one case: a returning client waits up
// to 120 s behind a stale-but-ALIVE incumbent. A client that is genuinely gone is
// reaped by the link layer in ~4-6 s (this firmware requests no supervision
// timeout, so the central's negotiated value applies), so the lockout never
// applies to a crashed or out-of-range peer.
//
// Firmware-local rather than a wire field, unlike its LAN cousin: OD_LAN_READ_TIMEOUT_S
// lives in opendisplay_protocol.h because it is a documented client-visible
// contract, and this plan may not touch that header. The asymmetry is deliberate.
#ifndef OD_BLE_IDLE_TIMEOUT_MS
#define OD_BLE_IDLE_TIMEOUT_MS 120000UL
#endif

// Reclaim a slot held by a client that has gone silent (7c). This is the ONLY way
// a held slot is ever released short of the client leaving, because admission
// never evicts -- refusal and reclaim are deliberately independent mechanisms, so
// an incumbent's fate never depends on whether someone else happened to knock.
//
// Runs LAST in the pass (7d step 4) so traffic parsed earlier this pass counts.
// That ordering is load-bearing for LAN, where inbound bytes can be sitting in the
// socket when the deadline is evaluated.
static void serviceIdleTimeout() {
    // 7c row 3: a refresh is not idleness. loop() does not run for its duration
    // while wall-clock time passes, so this would otherwise fire the instant a
    // long refresh ended. endRefresh() re-stamps the clock at the transition; this
    // guard covers the pass in which the refresh is still running.
    if (epdRefreshInProgress) return;
    const LinkId owner = linkOwnerId();
    // 7c row 4: no owner, no timer. Also excludes OWNER_TERMINAL, where the device
    // is on its way into deep sleep and there is nothing left to reclaim.
    if (owner.who != OWNER_BLE && owner.who != OWNER_LAN) return;

    // R4's "each transport enforces its own timer and constant" is satisfied by the
    // CONSTANTS differing, not by duplicating the clock -- one clock is what keeps
    // the two from drifting apart in what they consider activity.
    uint32_t limitMs = OD_BLE_IDLE_TIMEOUT_MS;
#ifdef OPENDISPLAY_HAS_WIFI
    if (owner.who == OWNER_LAN) limitMs = (uint32_t)OD_LAN_READ_TIMEOUT_S * 1000UL;
#endif

    const uint32_t idleMs = linkMsSinceOwnerCommand();
    if (idleMs <= limitMs) return;   // 7c row 2

    // NO transferActive() GATE, and that is the whole point of R4. A client that
    // goes silent DURING an upload is precisely the case that wedges the device,
    // and a transfer gate would exempt exactly it. The partial transfer is
    // discarded by the abort; partial upload state is never preserved across a
    // drop. What remains uncaught is narrower -- a client that keeps sending
    // recognised commands while its transfer never completes -- and that is still
    // bounded only by the from-START transfer watchdog.
    //
    // WARN with the measured value, so field tuning has data rather than guesses.
    od_log_warn("Idle timeout: %s owner silent %u ms (limit %u ms) - dropping",
                owner.who == OWNER_BLE ? "BLE" : "LAN",
                (unsigned)idleMs, (unsigned)limitMs);
    abortToKnownState("idle timeout", true, owner);
}

// Bounded drain: service up to OD_RXQ_SLOTS commands per pass so a
// sustained W-deep PIPE_WRITE window burst isn't starved at one-per-loop, while
// the rest of loop() still runs each pass. Responses are drained BETWEEN commands
// so pipe ACKs generated by this drain do not fill od_txq and start deferring the
// very commands that would refund the window.
//
// This is the only place commands are dispatched, on either target. Nothing may
// call it from inside a command handler: doing so would make handlers reentrant
// and corrupt multi-frame transfer state mid-stream.
static bool bleRxTagIsLive(uint32_t tag, void*) {
    return linkIsOwnerWord(tag);
}

static void serviceBleRx() {
    uint8_t drained = 0;
    uint16_t staleDropped = 0;
    while (drained < OD_RXQ_SLOTS) {
        staleDropped = (uint16_t)(staleDropped +
                                  od_rxq_discard_stale(bleRxTagIsLive, nullptr));
        od_rxq_item_t* item = od_rxq_peek();
        if (item == nullptr) break;
        // CONNECTION_POLICY R3 requirement 6: od_rxq_discard_stale() leaves only a frame whose
        // writer is STILL the owner. The write callback already refused non-owner writes; this
        // catches a formerly legitimate frame left queued across disconnect and handle reuse.
        // Recheck immediately before dispatch as well: the owner can depart after the shared
        // helper accepts this head. The next loop iteration consumes it against fresh liveness.
        if (!bleRxTagIsLive(item->tag, nullptr)) continue;
        // THE FRAME'S OWN IDENTITY, from its RX slot rather than a global the dispatcher
        // reads behind us: it must be answered on the connection that sent it, not on
        // whatever the link has become. The dispatch banner (commandName() in
        // communication.cpp) already logs which command runs, so no drain-start/-end
        // framing line is needed here.
        const od_reply_t rp = { OD_ORIGIN_BLE, item->tag };
        const od_frame_outcome_t outcome = od_dispatch_app_frame(&rp, item->data, item->len);
        // DEFERRED LEAVES THE FRAME WHERE IT IS. The dispatcher refused it for a reason that will
        // clear on its own -- no response capacity, or a config read holding the scratch -- so the
        // entry must be re-offered byte-identical. Consuming it here would turn ordinary
        // backpressure into silent command loss, which is the one failure DEFERRED exists to
        // prevent. Stop the drain too: the head is unchanged, so another pass this tick would
        // re-dispatch the same frame and reach the same answer.
        if (!od_frame_policy(outcome).consume_rx) {
            break;
        }
        od_rxq_consume();
        drained++;
        (void)od_txq_process();
    }
    if (staleDropped > 0) {
        od_log_warn("Dropped %u queued command(s) from a departed session", (unsigned)staleDropped);
    }
}

// Platform policy hook 1: work this target does before the shared body, with the
// option to claim the whole pass. Only ESP32 has any -- the deep-sleep wake
// window is a real capability difference, so the plan says hook it rather than
// merge it. Returns true when the pass is finished and loop() must return.
static bool platformLoopPrologue() {
    pollActivity();
    // THIS IS THE MAIN (FIRST) LOOP FOR A DEEP SLEEP ENABLED ESP32
    if (woke_from_deep_sleep && advertising_timeout_active) {
        // An ADMITTED client, not merely a physical link. ble.isConnected() is the
        // stack's aggregate peer count, so a contender -- including a client that
        // reconnected into a still-held slot and lost its claim -- would trip this
        // branch, run fullSetupAfterConnection(), close the wake window and return
        // before the refusal scan ever gets to reap it. Application-visible setup
        // work and a changed sleep decision, both driven by a connection that is
        // never going to be serviced.
        const LinkId prologueOwner = linkOwnerId();
        if (prologueOwner.who == OWNER_BLE &&
            ble.instanceLive(prologueOwner.handle, prologueOwner.epoch)) {
            od_log_info("BLE connection established - switching to full mode");
            advertising_timeout_active = false;
            fullSetupAfterConnection();
            woke_from_deep_sleep = false;
            return true;
        }
        // A connect+drop entirely inside one poll gap leaves the radio dark for the
        // rest of the window; the flags are otherwise only serviced past this return.
        serviceBleDisconnectCleanup();   // tear down before re-advertising
        serviceContenderRefusal();       // reap a contender rather than idling behind it
        serviceBleAdvertisingRestart();
        uint32_t advertising_timeout_ms = globalConfig.power_option.sleep_timeout_ms;
        if (advertising_timeout_ms == 0) {
            advertising_timeout_ms = DEFAULT_IDLE_HOLD_MS;
        }
        // Measured from the last activity, not from window start: a client that
        // connects and drops re-arms the full window instead of inheriting it.
        uint32_t idle_duration = od_hal_uptime_ms() - lastActivityMs;
        // On a button wake the min-wake hold keeps this window open past the
        // quiet timeout; idleDelay(50) below services buttons/touch throughout.
        if (idle_duration >= advertising_timeout_ms && !minWakeHoldActive()) {
            uint32_t advertisingElapsedMs = od_hal_uptime_ms() - advertising_start_time;
            od_log_info("BLE advertising timeout (idle %u ms of %u ms window) - no connection, returning to deep sleep",
                        (unsigned)idle_duration, (unsigned)advertisingElapsedMs);
            advertising_timeout_active = false;
            enterDeepSleep();
            return true;
        }
        // idleDelay() services buttons + touch (and LED flash) while it waits, so a
        // wake-time touch is polled during this window even though the branch returns
        // on every pass until a client connects. It lands in dynamicreturndata, reaches
        // a mid-window client, and pollActivity picks it up next pass to hold the window
        // open — none of which happens if we just od_hal_delay_ms() here without servicing input.
        idleDelay(50); // idleDelay() polls touch and buttons while waiting
        return true;
    }
    return false;
}

// Platform policy hook 2: what this target does when nothing is in flight.
// ESP32 owns the deep-sleep decision; nRF just idles at its configured cadence.
// Never reached while work is outstanding -- loop() handles that case itself.
static void platformIdle() {
    if (globalConfig.power_option.deep_sleep_time_seconds > 0 && globalConfig.power_option.power_mode == 1) {
        uint32_t idleHoldMs = globalConfig.power_option.sleep_timeout_ms;
        if (idleHoldMs == 0) {
            idleHoldMs = DEFAULT_IDLE_HOLD_MS;
        }
        uint32_t idleMs = od_hal_uptime_ms() - lastActivityMs;
        // The min-wake hold covers first boot and connect-then-drop during a
        // button-wake window (woke_from_deep_sleep cleared on connect above).
        if (idleMs < idleHoldMs || minWakeHoldActive()) {
            idleDelay(5);
        } else {
            od_log_info("Idle %u ms (hold %u ms) - entering deep sleep", (unsigned)idleMs, (unsigned)idleHoldMs);
            enterDeepSleep();
        }
    } else {
        // Non-battery (USB) idle: keep the loop responsive. A 2000 ms idle here
        // stalls BLE command/response servicing for up to 2 s when a client
        // connects mid-delay (the queued write waits out the delay before the
        // loop re-evaluates), which reads as a sluggish/unreliable first
        // exchange. Use the same short cadence as the battery idle-hold path.
        idleDelay(5);
    }
    static uint32_t lastMsdUpdate = 0;
    if (od_hal_uptime_ms() - lastMsdUpdate >= 60000) {
        lastMsdUpdate = od_hal_uptime_ms();
        updatemsdata();
    }
}

// One loop body for both targets. The per-target policy that genuinely differs
// lives in the two hooks above; everything here is shared.
void loop() {
    /* First statement in the pass, so no early return below can skip it and so the feed is
     * evidence that loop() itself is running rather than that some subset of it completed. */
    od_watchdog_app_service();
    serviceBleEvents();
    processLedFlash();
    epdSessionTick();   // od_hal_uptime_ms()-poll: power the panel down screen_timeout_seconds after last release
    displayServiceDeferredBoot();   // boot screen owed by a config write that brought drive packets
    buzzerService();

    // Reconcile advertising before any branch can claim the pass: this is the only path to
    // ble_gap_adv_start(), so a policy branch that returns early must not be able to skip it.
    // Policy belongs in the argument -- while a refresh runs a NEW start waits, but a running
    // advertisement is never withdrawn for one.
    (void)od_ble_service_advertising(!epdRefreshInProgress);

    if (platformLoopPrologue()) return;

    // WITHIN-PASS ORDER IS NORMATIVE (CONNECTION_POLICY R7d), not incidental:
    //
    //   1. owner disconnects  -- release + abort FIRST, so a slot freed this pass
    //                            is available to an admission decision in the same
    //                            pass, and so a departed session's state is gone
    //                            before any frame is dispatched against it
    //   2. refusals           -- contenders reaped before they can linger
    //   3. inbound traffic    -- stamps the activity clock
    //   (4. idle timeout      -- Phase 3, and it must run last so traffic parsed in
    //                            step 3 counts)
    //
    // The cleanup used to run AFTER the RX drain. That ordering is what made the
    // frame tag load-bearing rather than merely defensive, and reversing it removes
    // a whole class of "old session's frames meet new session's state" hazard
    // instead of relying on the tag to catch every instance of it.
    //
    // Note this order resolves ties WITHIN a pass only. The authoritative
    // arbitration point is the earliest transport hook -- the connect callback's
    // claim CAS -- because a BLE connect during a refresh and a LAN socket sitting
    // in the listen backlog are not comparable by the time loop() resumes.
#ifdef OPENDISPLAY_HAS_WIFI
    // Reap a LAN socket the peer has closed BEFORE the cleanup below, so the token
    // is released in THIS pass and handleWiFiServer()'s accept -- later in the same
    // pass -- sees a free slot (7d step 1 before step 2).
    //
    // Doing this inside handleWiFiServer, where it was first placed, is too late:
    // the reap only raises the deferred cleanup, so the accept a few lines further
    // on still tested the corpse's token and refused an ordinary reconnect. A
    // client that closes and immediately reopens between two pushes is the common
    // case, and one that does not retry would simply not be served.
    wifiLanReapClosedSession();
#endif
    serviceBleDisconnectCleanup();
    serviceContenderRefusal();
    serviceBleRx();
    // Egress and the resumable producer, once per pass, AFTER RX so a reply queued by this pass's
    // dispatch goes out in the same pass. The producer is what makes a config read more than its
    // first chunk: od_config_read_start() emits chunk 0 and nothing else, and without a pump the
    // read stops at 100 bytes and stays active forever -- deferring every later config write
    // behind it permanently.
    (void)od_txq_process();
    (void)od_config_read_pump();
    if (s_msdUpdatePending) {
        s_msdUpdatePending = false;
        updatemsdata();
    }
    serviceBleAdvertisingRestart();   // no-op where the stack re-arms itself
    // Advertising is reconciled at the top of loop(), above platformLoopPrologue(), so no early
    // return can skip it. serviceBleAdvertisingRestart() above owns whether we WANT to
    // advertise; the pump owns what to do about it.

    // Session watchdogs. Shared as of Phase 4: these are transport-agnostic and
    // were ESP32-only for no reason other than living in the ESP32 loop arm, so
    // nRF gains them. A hung transfer there used to sit until disconnect.
    // Both now live in display_service.cpp, beside the transfer state they tear
    // down. Splitting them across files is how the direct-write one came to
    // release the panel while leaving its enclosing PIPE session running.
    checkTransferTimeouts();

    #ifdef OPENDISPLAY_HAS_WIFI
    // WiFi handling runs after BLE queue processing to avoid blocking
    // BLE command responses (moved from top of loop in v1.6 fix).
    handleWiFiServer();
    static uint32_t lastWiFiCheck = 0;
    if (wifiInitialized && (od_hal_uptime_ms() - lastWiFiCheck > 10000)) {
        lastWiFiCheck = od_hal_uptime_ms();
        /* OD: was WiFi.status() != WL_CONNECTED through the Arduino shim. The WiFi stack is
         * wifi_service.cpp's business; this file only needs the boolean. */
        const bool linkUp = wifiLinkIsUp();
        if (!linkUp && wifiConnected) {
            od_log_warn("WiFi connection lost");
            wifiConnected = false;
            if (wifiServerConnected) {
                disconnectWiFiServer();
            }
        } else if (linkUp && !wifiConnected) {
            char wifiIp[16];
            wifiLocalIpStr(wifiIp, sizeof(wifiIp));
            od_log_info("WiFi reconnected (IP: %s)", wifiIp);
            wifiConnected = true;
            restartWiFiLanAfterReconnect();
        }
    }
    const bool wifiLanSession = wifiInitialized && wifiLanClientConnected();
    #else
    const bool wifiLanSession = false;
    #endif

    // 7d step 4, and it must stay LAST of the four. Traffic parsed earlier in this
    // pass -- BLE in serviceBleRx(), LAN in handleWiFiServer() just above -- has
    // already stamped the activity clock, so a client whose command arrived this
    // pass is never judged idle on the strength of it not having been read yet.
    // Moving this above handleWiFiServer() would reintroduce exactly that for LAN.
    serviceIdleTimeout();
    // After the idle check, and last of the session-policy steps. It ends in the
    // abort like the idle drop does, so it must not run before inbound traffic has
    // been parsed this pass -- an accepted command clears the rejection run, and
    // dropping a client that just authenticated would be the worst possible
    // outcome for a mechanism whose entire value is speed.
    serviceBleAuthAbuseDisconnect();

    // Work in flight *this iteration* only. Every term is transient and most are
    // cleared earlier in this same pass, so this must never be the sole gate on
    // deep sleep — lastActivityMs supplies the quiet window. The terms that only
    // one target can ever raise (s_advertisingRestartPending, wifiLanSession)
    // are simply false on the other, so one expression serves both.
    // eventPending() closes the callback-timing hole: an event raised after
    // serviceBleEvents() ran in this pass is otherwise invisible until the next
    // pass -- and this pass is about to park. It is transient like the rest;
    // take*Event() clears the peeked flag at the next loop top.
    //
    // No transfer-state term belongs here, and its absence is deliberate. A live
    // transfer already has its connected BLE or LAN owner holding the gate, and
    // one whose transport is gone cannot progress, so refusing to sleep on it
    // would burn power for work that will never happen. That state is healed in
    // checkTransferTimeouts() instead. See
    // docs/PLAN_WORK_GATE_TRANSFER_TERMS_2026-07-29.md.
    const bool workInFlight = od_rxq_pending() || od_txq_depth() > 0u ||
                              od_config_read_active() ||
                              ble.isConnected() ||
                              ble.eventPending() ||
                              s_advertisingRestartPending ||
                              epdRefreshInProgress ||
                              wifiLanSession;
    if (workInFlight) {
        od_hal_delay_ms(1);
    } else {
        platformIdle();
    }
    ble.tick();          // no-op on ESP32
    processButtonEvents();
    processTouchInput();
    buzzerService();
}

// Button/LED runtime moved to device_control.cpp

// Cooperative delay: services the things that must keep ticking while loop()
// waits. Called ONLY from loop() -- never from a command handler, which is what
// makes the RX rule below safe to state as a hard invariant.
void idleDelay(uint32_t delayMs) {
    const uint32_t CHECK_INTERVAL_MS = 100;
    uint32_t remainingDelay = delayMs;
    while (remainingDelay > 0) {
        ble.tick();   // no-op on ESP32
        processButtonEvents();
        processTouchInput();
        processLedFlash();
        epdSessionTick();   // expire the keep-alive window while a long idleDelay blocks
        buzzerService();
        // Keep responses moving: loop() is the queue's only drainer, so a long
        // idleDelay would otherwise hold queued ACKs for its full duration.
        // Draining TX is safe here because it only notifies; it dispatches nothing.
        //
        // The config-read producer runs for the same reason and with the same safety: it emits at
        // most one already-loaded chunk per pass and dispatches nothing. Omitting it would stall a
        // read for the whole delay while its consumer sat here draining an empty queue.
        (void)od_txq_process();
        (void)od_config_read_pump();
        // RX and transport events are deliberately NOT serviced here -- return to
        // loop() and let serviceBleEvents()/serviceBleRx() handle them at top
        // level. Dispatching RX inside idleDelay would make command handlers
        // reentrant the moment anything calls idleDelay from a handler, corrupting
        // multi-frame transfer state; consuming events here would move the single
        // consumer out of loop() and break the ordering serviceBleEvents() relies
        // on. Returning early also caps latency at one CHECK_INTERVAL_MS rather
        // than the caller's full delay, which is what makes nRF's move to
        // loop()-side dispatch viable: its idle waits are 500 ms and up.
        //
        // This break set answers "did work appear while we were parked?", which is
        // NOT the question workInFlight answers ("is there work?"). A term belongs
        // here only if (i) it can go false->true asynchronously, on a task other
        // than loop(), and (ii) idleDelay cannot service that work itself. RX and
        // transport events are the only two that qualify. A non-empty od_txq is
        // excluded because od_txq_process() already runs every chunk above;
        // epdRefreshInProgress, the transfer-state flags, s_advertisingRestartPending
        // and wifiLanSession are excluded because only loop() itself raises them,
        // so none can change while loop() is sitting inside this function.
        if (od_rxq_pending() || ble.eventPending()) return;
        uint32_t chunkDelay = (remainingDelay > CHECK_INTERVAL_MS) ? CHECK_INTERVAL_MS : remainingDelay;
        od_hal_delay_ms(chunkDelay);
        remainingDelay -= chunkDelay;
    }
}


void fullSetupAfterConnection() {
    od_log_info("=== Full Setup After Connection ===");
#ifdef OPENDISPLAY_HAS_WIFI
    initWiFi(false);
#endif
    od_log_info("=== Full setup completed ===");
}

void enterDeepSleep(bool force, uint16_t overrideSleepSeconds) {
    if (globalConfig.power_option.power_mode != 1) {
        od_log_debug("Skipping deep sleep - not battery powered (power_mode: %u)", globalConfig.power_option.power_mode);
        return;
    }
    if (globalConfig.power_option.deep_sleep_time_seconds == 0) {
        od_log_debug("Skipping deep sleep - deep_sleep_time_seconds is 0");
        return;
    }
    // Callers sample their idle state before getting here; a central can connect in
    // that gap. Re-check so we never tear down the stack on a live link.
    if (!force && ble.isConnected()) {
        od_log_debug("Skipping deep sleep - BLE client connected");
        lastActivityMs = od_hal_uptime_ms();
        return;
    }
    // Defense in depth for the min-wake hold (first boot / button wake). MUST
    // stay ahead of the advertising stop below: everything past that point
    // commits to esp_deep_sleep_start(), so a late abort would leave the device
    // awake with the radio dark. force (host 0x0053) bypasses the hold.
    if (!force && minWakeHoldActive()) {
        od_log_debug("Skipping deep sleep - minimum wake window active");
        return;
    }
    // Panel power-down MUST sit below every early-return above (including the
    // ORDER IS NORMATIVE (CONNECTION_POLICY 7e row 3): gate admission, THEN abort,
    // THEN take the stack down.
    //
    // linkMarkTerminal() first, or there is a race: the abort's last step frees the
    // owner word while this link may still be up and advertising is still on, so a
    // connect on the host task could win the freed word and the new owner would be
    // destroyed by ble.end() below with no abort ever run for it. The terminal
    // exchange makes every later claim fail, and it returns the identity it
    // displaced -- which the abort must be handed, because from here on
    // linkOwnerId() reads terminal rather than the departing owner.
    //
    // Why the abort at all, given wake reloads RAM: not because state survives
    // (it does not -- only RTC_DATA_ATTR does), but because deep sleep is a
    // MID-SESSION exit whose path otherwise hand-rolls a private teardown subset
    // that has to be kept in sync with the real one forever. Forced sleep bypasses
    // the live-link guard above and this path never arbitrates a LAN owner, so it
    // can begin with a transfer in flight. dropLink=false because ble.end() takes
    // the stack down immediately: there is no link left to drop politely, and no
    // loop pass will service the resulting event.
    const LinkId displaced = linkMarkTerminal();
    abortToKnownState("deep sleep", false, displaced);
    // Panel power-down MUST sit below every early-return above (including the
    // min-wake hold): on mains (power_mode != 1) enterDeepSleep bails before here,
    // so a WARM panel stays warm and the keep-alive tick in idleDelay(2000) expires
    // it after the configured window. On battery this is the routine
    // WARM-at-idle-hold-expiry path (idle-hold default 10 s often < the keep-alive
    // window) and also closes the pre-existing "deep sleep never powers the panel
    // down" hazard. Net effect on battery ESP32: effective keep-alive =
    // min(configured window, idle-hold).
    //
    // Stays HERE, in the sleep path, and must never move into the abort: this kills
    // every power state including PWR_WARM, whereas the abort deliberately lets a
    // WARM keep-alive panel survive. Sleep is the one transition where no panel may
    // stay powered.
    epdSessionForceOff();
    // Sleep quiescing, not session teardown: the abort deliberately leaves buzzer
    // and LED running (they are user-facing effects, and a client that fires a buzz
    // then drops the link is a normal pattern). But deep sleep cuts the clocks they
    // run on -- buzzerService() never ticks again from here -- so a tone left on is
    // not a melody finishing, it is a driven pin held through teardown and into
    // sleep, sounding continuously and drawing current until the next wake.
    // Silencing here rather than waiting means sleep is never delayed by an effect.
    //
    // Deep sleep ONLY: power-latch off deliberately plays a chirp on the way down.
    buzzerStopForSleep();
    ledStopForSleep();
    woke_from_deep_sleep = true; // Will be true on next boot
    ble.stopAdvertising();
    od_hal_delay_ms(200);
    // F7: report the truth. Deep sleep powers the digital core down, so a failed teardown does
    // not strand the controller here the way it would before esp_restart() -- but "BLE
    // deinitialized" printed unconditionally is how a failure this path CAN have became
    // invisible. Sleep proceeds either way; the log now says which happened.
    if (ble.end()) {
        od_log_info("BLE deinitialized");
    } else {
        od_log_warn("BLE teardown FAILED -- sleeping anyway; the core powers down regardless");
    }
    od_hal_delay_ms(100);
    // Host override (0x0053 payload) applies to this one cycle only: it is a
    // parameter, never stored, so an aborted or later sleep reverts to config.
    uint16_t sleepSeconds = overrideSleepSeconds ? overrideSleepSeconds
                                                 : globalConfig.power_option.deep_sleep_time_seconds;
    uint64_t sleep_timeout_us = (uint64_t)sleepSeconds * 1000000ULL;
    esp_sleep_enable_timer_wakeup(sleep_timeout_us);
    // After the timer arm, before powerLatchHoldForSleep(): the latch-hold
    // manipulation then cannot disturb freshly configured RTC pulls, and its
    // gpio_hold_en() touches only the latch pin, never the wake pads.
    armButtonWakeSources();
    // RTC power-off (PCF8563/BM8563 + MOSFET latch): the RTC switches the board
    // back on, so the latch is released instead of held. On an I2C failure the
    // rail stays held and this is a plain timer deep sleep. The ESP32 timer
    // above stays armed either way: it is what wakes a USB-powered board.
    const bool rtcPowerOff = rtcArmWake(sleepSeconds);
    od_log_info("Entering deep sleep for %u seconds%s%s", sleepSeconds,
                overrideSleepSeconds ? " (host override, one cycle)" : " (config)",
                rtcPowerOff ? ", RTC power-off" : "");
    logHeapUsage("(pre-sleep)");
    od_log_flush(); // drain UART/Serial prior to deep sleep
    od_hal_delay_ms(100); // Brief delay to ensure serial output is sent
    if (rtcPowerOff) {
        powerLatchReleaseForRtcSleep();
    } else {
        powerLatchHoldForSleep();
    }
    touchPrepareForDeepSleep();
    nm420HoldForSleep();
    esp_deep_sleep_start();
}

// Panel rail is cut after this — drive control lines LOW; BUSY stays an input.
static void configureDisplayPinsLowPower() {
    const DisplayConfig& d = globalConfig.displays[0];
    const uint8_t pins[] = {
        d.cs_pin, d.cs_pin_2, d.clk_pin, d.data_pin, d.dc_pin, d.reset_pin,
    };
    for (uint8_t pin : pins) {
        if (pin == 0xFF) continue;
        od_hal_gpio_config_output(pin, false);
    }
    if (d.busy_pin != 0xFF) {
        od_hal_gpio_config_input(d.busy_pin, false, false);
    }

    if (!(globalConfig.system_config.device_flags &
          (DEVICE_FLAG_BATTERY_LATCH | DEVICE_FLAG_PWR_LATCH_DFF))) {
        const uint8_t auxPins[] = {
            globalConfig.system_config.pwr_pin_2,
            globalConfig.system_config.pwr_pin_3,
        };
        for (uint8_t pin : auxPins) {
            if (pin == 0xFF || pin == 0) continue;
            od_hal_gpio_config_output(pin, false);
        }
    }
}

void pwrmgm(bool onoff){
    if(globalConfig.display_count == 0){
        od_log_warn("No display configured");
        return;
    }
    // Idempotency guard keyed on the panel power state machine (the single source
    // of truth). Makes every legacy caller safe: a same-state call becomes a no-op,
    // while a real transition (true-after-false, or the boot true/false/true rail
    // cycle) always proceeds because each call flips the state. pwrmgm owns the
    // OFF<->(ACTIVE) transitions; od_display_esp.cpp owns ACTIVE<->WARM.
    if (onoff  && pwrmgmState != PWR_OFF) return;   // already powered (WARM or ACTIVE)
    if (!onoff && pwrmgmState == PWR_OFF) return;   // already off
    displayPowerState = onoff;
    pwrmgmState = onoff ? PWR_ACTIVE : PWR_OFF;
    if (!onoff) pwrmgmOffDeadlineMs = 0;
    uint8_t axp2101_bus_id = 0xFF;
    bool axp2101_found = false;
    for(uint8_t i = 0; i < globalConfig.sensor_count; i++){
        if(globalConfig.sensors[i].sensor_type == OD_SENSOR_TYPE_AXP2101){
            axp2101_bus_id = globalConfig.sensors[i].bus_id;
            axp2101_found = true;
            break;
        }
    }
    if(axp2101_found){
        if(onoff){
        od_log_info("Powering up AXP2101 PMIC...");
            initAXP2101(axp2101_bus_id);
        }
        else{
            od_log_info("Powering down AXP2101 PMIC...");
            powerDownAXP2101();
            od_hal_i2c_esp_deinit();
            invalidateOpenDisplayWire();
            od_hal_gpio_config_output(47, true);
            od_hal_gpio_config_output(48, true);
        }
    }
#if defined(OPENDISPLAY_FASTEPD)
    const bool fastepd_driver_spi = fastepd_driver_used();
#else
    const bool fastepd_driver_spi = false;
#endif
    const DisplayConfig& disp = globalConfig.displays[0];
    if (onoff) {
        if (globalConfig.system_config.pwr_pin != 0xFF) {
            od_hal_gpio_write(globalConfig.system_config.pwr_pin, true);
            od_hal_delay_ms(800);
        } else {
            od_log_warn("Power pin not set");
        }
        if (!fastepd_driver_spi) {
            if (disp.reset_pin != 0xFF) {
                od_hal_gpio_config_output(disp.reset_pin, true);
            }
            if (disp.cs_pin != 0xFF) {
                od_hal_gpio_config_output(disp.cs_pin, true);
            }
            if (disp.cs_pin_2 != 0xFF) {
                od_hal_gpio_config_output(disp.cs_pin_2, true);
            }
            if (disp.dc_pin != 0xFF) {
                od_hal_gpio_config_output(disp.dc_pin, false);
            }
            if (disp.clk_pin != 0xFF) {
                od_hal_gpio_config_output(disp.clk_pin, false);
            }
            if (disp.data_pin != 0xFF) {
                od_hal_gpio_config_output(disp.data_pin, false);
            }
            if (disp.busy_pin != 0xFF) {
                od_hal_gpio_config_input(disp.busy_pin, false, false);
            }
            od_hal_delay_ms(100);
        } else {
            if (disp.reset_pin != 0xFF) {
                od_hal_gpio_config_output(disp.reset_pin, true);
            }
            od_hal_delay_ms(200);
        }
        initOrRestoreWireForOpenDisplay();
    } else {
        displayReleaseSpiBus();
        // Keep I2C alive when sensors/touch use data_bus[0] (e.g. reTerminal MISC_I2C on GPIO0/1).
        if (!openDisplayI2cBusConfigured()) {
            od_hal_i2c_esp_deinit();
            invalidateOpenDisplayWire();
        }
        if (globalConfig.system_config.pwr_pin != 0xFF) {
            configureDisplayPinsLowPower();
            od_hal_gpio_write(globalConfig.system_config.pwr_pin, false);
        }
    }
}

void xiaoinit(){
    powerDownExternalFlash(20,24,21,25,22,23);
    //od_hal_gpio_config_input(31, false, false);
    //od_hal_gpio_config_input(14, false, false);
    od_hal_gpio_config_output(13, false); //that actually does something
    //od_hal_gpio_config_input(17, false, false);
}

// RockBase IoT NM-EPD-420 (ESP32-S3 + 4.2in panel), DEVICE_FLAG_NM420_INIT.
//
// The board carries a LoRa module, an ES8311 codec, an audio PA and an AHT20 power gate whose
// enable lines no config packet can describe. Left at the reset default (input, no pull) each
// rail floats to whatever the board's resistor decides, so drive them OFF. All active HIGH:
//   GPIO47 MODULE_EN (LoRa)   GPIO44 CODEC_EN   GPIO41 PA_CTRL   GPIO40 TEMP_CTL (AHT20 gate)
//   GPIO43 ADC_EN (battery divider; pulsed HIGH only while measuring)
// Panel 1/2/4/5/6/46, I2C 38/39 and the USER key 45 belong to their own init blocks.
#if defined(CONFIG_IDF_TARGET_ESP32S3)
static constexpr uint8_t kNm420OffPins[] = {47, 44, 41, 40, 43};
#endif

void nm420_init(){
#if defined(CONFIG_IDF_TARGET_ESP32S3)
    for (uint8_t pin : kNm420OffPins) {
        gpio_hold_dis((gpio_num_t)pin);
        od_hal_gpio_config_output(pin, false);
    }
    od_log_info("NM-EPD-420 init: peripheral enables and battery ADC gate LOW");
#endif
}

void nm420HoldForSleep() {
#if defined(CONFIG_IDF_TARGET_ESP32S3)
    if (!globalConfig.loaded || !(globalConfig.system_config.device_flags & DEVICE_FLAG_NM420_INIT)) return;
    for (uint8_t pin : kNm420OffPins) {
        od_hal_gpio_write(pin, false);
        gpio_hold_en((gpio_num_t)pin);
    }
    gpio_deep_sleep_hold_en();
#endif
}

// Original M5Stack M5Paper (classic ESP32 + IT8951), selected by panel IC
// OD_PANEL_IC_M5PAPER_960X540 (the panel only ships on this board). The power
// button only switches the board on while it is held: GPIO2 (main-power hold)
// must be driven HIGH to keep it on, and GPIO5 enables the Grove 5 V boost.
// GPIO23 (IT8951 power) is driven by the panel driver as its EN pin. Deep-sleep
// hold and power-off of GPIO2 go through the MOSFET latch
// (DEVICE_FLAG_BATTERY_LATCH, pwr_pin_2 = 2), so the preset sets both flags.
//
// The on-board BM8563 RTC (0x51 on I2C bus 0) runs from VBAT_IN, and its INT
// line also pulls the power-switch gate low: an alarm or timer left armed (the
// M5Stack factory firmware leaves a repeating timer) switches the board back on
// after every power-off, and it survives a flash erase. m5paper_init() therefore
// turns both interrupts off, clears the flags, stops the timer and disables the
// alarm. With RTC power-off configured it leaves that to rtcConsumeWake(),
// which needs the flags to recognise an RTC wake.
//
// The CPU is clamped to 80 MHz once the hold is up: BLE needs no more, and the
// wake window is mostly idle polling. The clamp goes through esp_pm_configure()
// (max = min = 80, no light sleep), so it needs CONFIG_PM_ENABLE in the board's
// sdkconfig; a build without it keeps the default clock.
#if defined(CONFIG_IDF_TARGET_ESP32)
static void m5paperRtcQuiesce() {
    static const uint8_t kCtrl2[] = {0x01, 0x00};                    // AIE/TIE off, AF/TF cleared
    static const uint8_t kTimer[] = {0x0E, 0x03};                    // timer off, 1/60 Hz
    static const uint8_t kAlarm[] = {0x09, 0x80, 0x80, 0x80, 0x80};  // every alarm field disabled
    if (!initOrRestoreWireForBus(0)) {
        od_log_warn("M5Paper init: no I2C bus 0, RTC left as is");
        return;
    }
    const bool ok = od_hal_i2c_write(0, 0x51, kCtrl2, sizeof(kCtrl2)) == 0 &&
                    od_hal_i2c_write(0, 0x51, kTimer, sizeof(kTimer)) == 0 &&
                    od_hal_i2c_write(0, 0x51, kAlarm, sizeof(kAlarm)) == 0;
    if (ok) {
        od_log_info("M5Paper init: RTC interrupts, timer and alarm cleared");
    } else {
        od_log_warn("M5Paper init: RTC not reachable at 0x51 on bus 0, left as is");
    }
}
#endif

void m5paper_init(){
#if defined(CONFIG_IDF_TARGET_ESP32)
    // Level before direction so the hold pin never glitches LOW.
    od_hal_gpio_write(2, true);
    od_hal_gpio_config_output(2, true);
    gpio_hold_dis(GPIO_NUM_2);
    gpio_hold_dis(GPIO_NUM_5);
    od_hal_gpio_config_output(5, false);
    od_log_info("M5Paper init: main-power hold HIGH, Grove boost off");
#if CONFIG_PM_ENABLE
    // After the GPIO2 hold, so the power-button hold time does not grow.
    const esp_pm_config_t pm = {.max_freq_mhz = 80, .min_freq_mhz = 80, .light_sleep_enable = false};
    const esp_err_t pm_err = esp_pm_configure(&pm);
    if (pm_err == ESP_OK) {
        od_log_info("M5Paper init: CPU clamped to 80 MHz");
    } else {
        od_log_warn("M5Paper init: CPU clamp failed (%d), clock left as is", (int)pm_err);
    }
#endif
    if (!rtcWakeConfigured()) m5paperRtcQuiesce();
#endif
}

void ws_pp_init(){
    od_log_info("===  Photo Printer Initialization ===");
    od_hal_gpio_config_output(21, true);
    od_hal_gpio_config_input(1, false, false);
    od_hal_gpio_config_input(2, false, false);
    od_hal_gpio_config_input(3, false, false);
    od_hal_gpio_config_input(4, false, false);
    od_hal_gpio_config_output(5, true);
    od_hal_gpio_config_input(6, false, false);
    /* WAS `pinMode(7, LOW)` -- a LEVEL passed where a MODE belongs, and a pre-existing bug.
     * LOW is 0, and the shim's pinMode() treats anything that is not OUTPUT as input, so pin 7
     * has been configured as an INPUT on shipped firmware, not driven low as the surrounding
     * code and the write on the next line plainly intend.
     *
     * TRANSLATED FAITHFULLY, NOT FIXED. This is the deep-sleep low-power pin pass: flipping a
     * pin from input to output changes what it does to the rail it is attached to, and on the
     * wrong pin that is backfeed or extra current draw, not a tidier line of code. Whether it
     * should be config_output(7, false) is a HARDWARE question for whoever owns this board.
     * The following write is left in place for the same reason -- on an input pin it only sets
     * the output latch, which is what it did before. */
    od_hal_gpio_config_input(7, false, false);
    od_hal_gpio_write(7, false);
    od_hal_gpio_config_input(14, false, false);
    od_hal_gpio_config_input(15, false, false);
    od_hal_gpio_config_input(16, false, false);
    od_hal_gpio_config_input(17, false, false);
    od_hal_gpio_config_input(18, false, false);
    od_hal_gpio_config_output(38, true);
    od_hal_gpio_config_output(39, true);
    od_hal_gpio_config_output(40, true);
    od_hal_gpio_config_output(41, true);
    od_hal_gpio_config_output(42, true);
    od_hal_gpio_config_output(45, true);
    od_log_info("Photo Printer initialized");
}

void powerDownExternalFlashFromConfig(void) {}

bool powerDownExternalFlash(uint8_t mosiPin, uint8_t misoPin, uint8_t sckPin, uint8_t csPin, uint8_t wpPin, uint8_t holdPin) {
    od_log_warn("External flash power-down not implemented for ESP32");
    return false;
    return false;
}

extern "C" void od_smp_note_activity(void) {
    linkStampOwnerCommand();
    lastActivityMs = od_hal_uptime_ms();
}
