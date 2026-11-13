#include "display_service.h"
#include "od_cmd_reply.h"

#include <bb_epaper.h>
#include <string.h>
#include "od_hal_i2c.h"
#include "od_hal_i2c_esp.h"
#include "od_hal_gpio.h"
#include "od_hal_time.h"
#include "od_hal_sleep.h"
#include "od_hal_adc.h"
#include "structs.h"
/* shared/core: the 16-byte MSD encoding. This target no longer assembles those bytes. */
#include "od_advert.h"
#include "od_color.h"
/* OD-PORT: two panel IC values this file compares against are missing from
 * shared/protocol/ -- they were added to Firmware's VENDORED copy of the wire contract
 * and never propagated to canonical. See protocol_pending.h; it is a debt with a
 * documented removal sequence, not a second home for wire constants. */
#include "protocol_pending.h"
#include "od_log.h"
#include "od_watchdog_app.h"
#include "buzzer_control.h"
#include "od_sensor_sht40.h"
#include "od_sensor_bq27220.h"
#include "communication.h"
#include "boot_screen.h"
#include "link_owner.h"
#include "session_guard.h"
#include "touch_input.h"
#include "od_zlib_pump.h"
#include "od_xfer.h"
#include "od_xfer_app.h"
#if defined(OPENDISPLAY_FASTEPD)
#include "display_fastepd.h"
#endif

#include "wifi_service.h"
#include "od_bbep_stream.h"

#include "ble_transport.h"
#include "od_rxq.h"

extern BBEPDISP bbep;
extern struct od_config globalConfig;
extern uint8_t msd_payload[16];
extern uint8_t dynamicreturndata[11];
extern uint8_t rebootFlag;
extern uint8_t activeLedInstance;
extern bool connectionRequested;
extern uint8_t mloopcounter;
extern bool displayPowerState;
// EPD panel power state machine — variables DEFINED in main.h TU; enum +
// EPD_KEEPALIVE_MAX_S live in display_service.h.
extern volatile uint8_t pwrmgmState;
extern uint32_t pwrmgmOffDeadlineMs;
extern volatile uint8_t pwrmgmLock;
extern uint8_t decompressionChunk[OPENDISPLAY_DECOMPRESSION_CHUNK_SIZE];
volatile bool epdRefreshInProgress = false;

// The ONE place the refresh bracket is closed, on every path.
//
// Both bracket sites used to assign epdRefreshInProgress = false inline. Routing
// them through a helper is what makes the R4 refresh exclusion implementable: a
// loop-side edge detector cannot see this transition, because loop() does not run
// for the refresh's whole duration -- both edges happen inside the blocking
// handler while wall-clock time passes. The activity clock has to be re-stamped AT
// the transition or a naive od_hal_uptime_ms()-lastStamp accrues the entire refresh and drops
// an actively engaged client the instant loop() resumes.
//
// Re-stamping can only ever DELAY a drop, never cause a spurious one, which is why
// it is safe to apply unconditionally here. A future third refresh path gets the
// exclusion by calling this instead of remembering a second statement.
void endRefresh(void) {
    epdRefreshInProgress = false;
    linkStampRefreshEnd();
}

extern uint32_t displayed_etag;

void pwrmgm(bool onoff);
void bbepInitIO(BBEPDISP *pBBEP, uint8_t u8DC, uint8_t u8RST, uint8_t u8BUSY, uint8_t u8CS, uint8_t u8MOSI, uint8_t u8SCK, uint32_t u32Speed);
void bbepWakeUp(BBEPDISP *pBBEP);
void bbepSendCMDSequence(BBEPDISP *pBBEP, const uint8_t *pSeq);
void bbepRefresh(BBEPDISP *pBBEP, int iMode);
void bbepSleep(BBEPDISP *pBBEP, int iMode);
void bbepSetAddrWindow(BBEPDISP *pBBEP, int x, int y, int cx, int cy);
void bbepStartWrite(BBEPDISP *pBBEP, int iPlane);
void bbepWriteData(BBEPDISP *pBBEP, uint8_t *pData, int iLen);
void bbepFill(BBEPDISP *pBBEP, unsigned char ucColor, int iPlane);
void bbepWriteCmd(BBEPDISP *pBBEP, uint8_t cmd);
void bbepCMD2(BBEPDISP *pBBEP, uint8_t cmd1, uint8_t cmd2);
void bbepWaitBusy(BBEPDISP *pBBEP);
/* NOT an upstream bb_epaper function -- provided by panel/od_bbep_idf_io.inl, this target's own
 * IO backend. Upstream has no teardown at all, which is what made every re-init a workaround:
 * the bus was never released, so a second spi_bus_initialize() could only fail. See
 * docs/BBEPAPER_IO_BACKENDS.md. */
void bbepDeInitIO(void);
bool bbepIsBusy(BBEPDISP *pBBEP);
void flashLed(uint8_t color, uint8_t brightness);
bool waitforrefresh(int timeout);

static bool nrfVbusPresent() { return true; }

static void epdBsPinLowIfNrf() {
}

// Battery boot: power-cycle the panel rail once. pwrmgm(true) already waits ~900 ms
// per enable; extra delays here are only for rail discharge between off/on.
static void prepareEpdRailForBoot() {
    epdBsPinLowIfNrf();
    pwrmgm(true);
}

// bb_epaper 71f6e70 replaced EP397/EP426 full-init RAM windows with SET_ORIENTATION
// (flip180=0 → 0x11=0x02 on 800-wide) while part inits and our partial helpers still
// use the pre-change windows. Re-apply those so full and partial share one map.
static void epdAlignCustomPartialRamMode(void) {
    uint8_t uc[4];
    if (bbep.type == EP397_800x480 || bbep.type == EP397_800x480_4GRAY) {
        bbepCMD2(&bbep, SSD1608_DATA_MODE, 0x01);
        bbepWriteCmd(&bbep, SSD1608_SET_RAMXPOS);
        uc[0] = 0x00; uc[1] = 0x00; uc[2] = 0x1f; uc[3] = 0x03;
        bbepWriteData(&bbep, uc, 4);
        bbepWriteCmd(&bbep, SSD1608_SET_RAMYPOS);
        uc[0] = 0xdf; uc[1] = 0x01; uc[2] = 0x00; uc[3] = 0x00;
        bbepWriteData(&bbep, uc, 4);
        bbepWriteCmd(&bbep, SSD1608_SET_RAMXCOUNT);
        uc[0] = 0x00; uc[1] = 0x00;
        bbepWriteData(&bbep, uc, 2);
        bbepWriteCmd(&bbep, SSD1608_SET_RAMYCOUNT);
        uc[0] = 0x00; uc[1] = 0x00;
        bbepWriteData(&bbep, uc, 2);
    } else if (bbep.type == EP426_800x480 || bbep.type == EP426_800x480_4GRAY) {
        bbepCMD2(&bbep, SSD1608_DATA_MODE, 0x02);
        bbepWriteCmd(&bbep, SSD1608_SET_RAMXPOS);
        uc[0] = 0x1f; uc[1] = 0x03; uc[2] = 0x00; uc[3] = 0x00;
        bbepWriteData(&bbep, uc, 4);
        bbepWriteCmd(&bbep, SSD1608_SET_RAMYPOS);
        uc[0] = 0x00; uc[1] = 0x00; uc[2] = 0xdf; uc[3] = 0x01;
        bbepWriteData(&bbep, uc, 4);
        bbepWriteCmd(&bbep, SSD1608_SET_RAMXCOUNT);
        uc[0] = 0x1f; uc[1] = 0x03;
        bbepWriteData(&bbep, uc, 2);
        bbepWriteCmd(&bbep, SSD1608_SET_RAMYCOUNT);
        uc[0] = 0x00; uc[1] = 0x00;
        bbepWriteData(&bbep, uc, 2);
    }
}

static void initBbepPanelSession() {
    const DisplayConfig& d = globalConfig.displays[0];
    bbepInitIO(&bbep, d.dc_pin, d.reset_pin, d.busy_pin, d.cs_pin, d.data_pin, d.clk_pin, 8000000);
    bbepWakeUp(&bbep);
    bbepSendCMDSequence(&bbep, bbep.pInitFull);
    epdAlignCustomPartialRamMode();
    od_hal_delay_ms(200);
}

// ---------------------------------------------------------------------------
// EPD panel power session (keep-alive) — see the state-machine design.
// pwrmgm() owns the OFF<->(ACTIVE) rail transitions and is the sole rail actuator;
// these helpers own the ACTIVE<->WARM transitions plus the keep-alive timer.
// ---------------------------------------------------------------------------

// Which init sequence is loaded in the controller (partial vs full). Panel-init
// bookkeeping, not power state — stays file-static here (Phase 2a uses it).
static bool epdSessionInitWasPartial = false;
// Phase 2b plane-consistency flag: true after a successful partial refresh leaves
// both controller planes consistent. Cleared on ForceOff / full-frame acquire.
// Not consulted for fill-skip in Phase 1 (full-frame skip is unconditional-safe).
static bool epdPlanesPrepared = false;

static bool epdSessionUsesFastepd(void) {
#if defined(OPENDISPLAY_FASTEPD)
    return fastepd_driver_used();
#else
    return false;
#endif
}

// Keep-alive window from config: screen_timeout_seconds, clamped to EPD_KEEPALIVE_MAX_S;
// 0 (also the old-blob/factory default) -> Release powers the panel straight down.
// Forced to 0 on AXP2101 boards regardless of config (PMIC warm idle draw unmeasured) —
// announced on the log whenever the override suppresses a non-zero configured value.
static uint32_t epdKeepAliveWindowMs(void) {
    uint8_t s = globalConfig.power_option.screen_timeout_seconds;
    for (uint8_t i = 0; i < globalConfig.sensor_count; i++) {
        if (globalConfig.sensors[i].sensor_type == OD_SENSOR_TYPE_AXP2101) {
            if (s != 0) {
                od_log_info("[EPD session] AXP2101 present - keep-alive forced off (screen_timeout_seconds ignored)");
            }
            return 0;
        }
    }
    if (s > EPD_KEEPALIVE_MAX_S) s = EPD_KEEPALIVE_MAX_S;
    return (uint32_t)s * 1000;
}

// Session try-lock, now UNCONTENDED on both targets and kept as defence in depth.
//
// It existed because nRF dispatched commands on the Bluefruit write-callback
// task while the keep-alive tick ran on loop(): a transfer could Acquire on one
// task while ForceOff rail-cut on the other. Phase 3 moved nRF dispatch to
// loop(), so every Acquire/Release/ForceOff/tick caller is now that single task
// (see docs/PLAN_BLE_TRANSPORT_ABSTRACTION_2026-07-27.md).
//
// Kept rather than deleted: it is nearly free, it still guards against a future
// caller arriving from an ISR or another task, and the try-lock in the tick is
// what keeps a rail-cut from landing mid-init regardless of who calls it.
static void pwrmgmLockTake(void) {
    // The yield here is now belt-and-braces. It was load-bearing under the old
    // model: this ran on the Bluefruit callback task, which outranks the loop
    // task holding the lock during the tick's ForceOff (SPI ops + od_hal_delay_ms(50)), so
    // a bare busy-spin starved the lower-priority holder forever on the single
    // core (priority-inversion livelock). With one task there is nothing to spin
    // against, but od_hal_delay_ms(1) is vTaskDelay and stays correct if that ever changes.
    while (__atomic_exchange_n(&pwrmgmLock, 1, __ATOMIC_ACQUIRE)) { od_hal_delay_ms(1); }
}
static bool pwrmgmLockTryTake(void) {
    return __atomic_exchange_n(&pwrmgmLock, 1, __ATOMIC_ACQUIRE) == 0;
}
static void pwrmgmLockGive(void) {
    __atomic_store_n(&pwrmgmLock, 0, __ATOMIC_RELEASE);
}

// Lock-held core (callers must hold pwrmgmLock). Split out so Release/Tick can
// power off without re-taking the non-recursive lock.
static void epdSessionForceOffLocked(void) {
    if (pwrmgmState == PWR_OFF) return;   // idempotent
    od_log_info("[EPD session] force off");
    if (epdSessionUsesFastepd()) {
#if defined(OPENDISPLAY_FASTEPD)
        fastepd_direct_sleep();
        // Rail is about to drop: force the next push to fully re-init the TCON
        // rather than wake() a power-cycled IT8951 (garbled refresh otherwise).
        fastepd_mark_hw_deinitialized();
#endif
    } else {
        bbepSleep(&bbep, 1);
        od_hal_delay_ms(50);
        /* Release the SPI bus and device now that the controller is asleep and no further
         * bytes are going out. Must be AFTER bbepSleep(), which still needs the bus to send
         * the sleep command, and BEFORE pwrmgm(false), which cuts the rail and parks the pins.
         *
         * This is what makes the next cold bring-up a clean spi_bus_initialize() rather than a
         * re-init of a bus nobody owns -- and re-initialising is not optional, because
         * configureDisplayPinsLowPower() inside pwrmgm(false) takes SCLK/MOSI back as plain
         * GPIO and only spi_bus_initialize() re-attaches them to the peripheral. Idempotent,
         * so the abort paths that reach here twice cost nothing. */
        bbepDeInitIO();
    }
    pwrmgm(false);   // -> PWR_OFF, clears deadline
    epdPlanesPrepared = false;
}

/* Whether pwrmgm() will drive an AXP2101 PMIC on this board, i.e. whether its timing includes
 * initAXP2101() over I2C on top of the rail assert and the 800 ms settle. Same scan pwrmgm()
 * itself does -- duplicated rather than exported because it exists only to label a log line,
 * and a second caller of a one-line predicate is cheaper than a new cross-file dependency. */
static bool epdRailUsesAxp2101(void) {
    for (uint8_t i = 0; i < globalConfig.sensor_count; i++) {
        if (globalConfig.sensors[i].sensor_type == OD_SENSOR_TYPE_AXP2101) return true;
    }
    return false;
}

// Bring the panel up for a transfer/refresh. Returns true iff it was COLD (rail
// was off) — callers may need to (re)open the address window regardless.
static bool epdSessionAcquire(bool partialInit) {
    pwrmgmLockTake();
    bool cold;
    const uint32_t tAcquire = od_hal_uptime_ms();
    if (pwrmgmState == PWR_OFF) {
        od_log_info("[EPD session] acquire: COLD bring-up");
        /* pwrmgm(true) FIRST, and timed: it asserts the panel rail, waits 800 ms for it to
         * settle, and -- when an AXP2101 PMIC is configured -- runs initAXP2101() over I2C.
         * That is the largest unattributed block on this path and it runs before any panel
         * byte moves, so a stall here must not be misread as a panel that will not answer. */
        uint32_t tRail = od_hal_uptime_ms();
        pwrmgm(true);   // -> PWR_ACTIVE (guarded; real transition)
        od_log_debug("[EPD cold] pwrmgm(on) %u ms (rail + 800 ms settle%s)",
                     (unsigned)(od_hal_uptime_ms() - tRail),
                     epdRailUsesAxp2101() ? " + AXP2101 I2C" : "");
        if (!epdSessionUsesFastepd()) {
            const DisplayConfig& d = globalConfig.displays[0];
            /* Per-step timing. A cold bring-up is three vendor calls and, when the panel
             * is not answering, each one silently burns a full BUSY timeout (5 s B/W,
             * 30 s multi-colour) inside bb_epaper and returns success. From the outside
             * that is one opaque 16-30 s stall between "COLD bring-up" and the next line
             * -- which blocks loop(), so queued BLE responses cannot be notified and the
             * host times out. Naming the step that costs the time is the difference
             * between reading a log and guessing. Cheap: four log lines per cold acquire,
             * not per transfer. */
            uint32_t tStep = od_hal_uptime_ms();
            bbepInitIO(&bbep, d.dc_pin, d.reset_pin, d.busy_pin, d.cs_pin, d.data_pin, d.clk_pin, 8000000);
            od_log_debug("[EPD cold] bbepInitIO %u ms", (unsigned)(od_hal_uptime_ms() - tStep));

            tStep = od_hal_uptime_ms();
            bbepWakeUp(&bbep);
            od_log_debug("[EPD cold] bbepWakeUp %u ms", (unsigned)(od_hal_uptime_ms() - tStep));

            const uint8_t* initSeq = partialInit ? (bbep.pInitPart ? bbep.pInitPart : bbep.pInitFull)
                                                 : bbep.pInitFull;
            tStep = od_hal_uptime_ms();
            bbepSendCMDSequence(&bbep, initSeq);
            od_log_debug("[EPD cold] initSeq (%s) %u ms",
                         partialInit ? "partial" : "full", (unsigned)(od_hal_uptime_ms() - tStep));
            tStep = od_hal_uptime_ms();
            epdAlignCustomPartialRamMode();
            od_log_debug("[EPD cold] alignRamMode %u ms", (unsigned)(od_hal_uptime_ms() - tStep));
            epdSessionInitWasPartial = partialInit;
        }
        cold = true;
    } else {
        // WARM re-acquire (or, defensively, an already-ACTIVE re-entry).
        od_log_info(pwrmgmState == PWR_ACTIVE ? "[EPD session] acquire: already ACTIVE (defensive)"
                                              : "[EPD session] acquire: WARM re-acquire");
        pwrmgmState = PWR_ACTIVE;
        pwrmgmOffDeadlineMs = 0;   // cancel keep-alive
        // Phase 1: full re-init on warm re-acquire (HW reset => registers identical
        // to cold, safest). Phase 2a will skip bbepWakeUp + resend only on change.
        if (!epdSessionUsesFastepd()) {
            /* Same breakdown as the cold path. A WARM re-acquire skips the rail and the
             * SPI bring-up, so if it ALSO stalls then the panel is not answering for a
             * reason unrelated to power -- which is worth being able to tell apart. */
            uint32_t tStep = od_hal_uptime_ms();
            bbepWakeUp(&bbep);
            od_log_debug("[EPD warm] bbepWakeUp %u ms", (unsigned)(od_hal_uptime_ms() - tStep));
            const uint8_t* initSeq = partialInit ? (bbep.pInitPart ? bbep.pInitPart : bbep.pInitFull)
                                                 : bbep.pInitFull;
            tStep = od_hal_uptime_ms();
            bbepSendCMDSequence(&bbep, initSeq);
            od_log_debug("[EPD warm] initSeq (%s) %u ms",
                         partialInit ? "partial" : "full", (unsigned)(od_hal_uptime_ms() - tStep));
            epdAlignCustomPartialRamMode();
            epdSessionInitWasPartial = partialInit;
        }
        cold = false;
    }
    /* The total, so the per-step lines above can be checked to add up. A large total with
     * small parts means the time is somewhere still uninstrumented -- which is exactly the
     * ambiguity the first instrumented build left, and the reason this line exists. */
    od_log_info("[EPD session] acquire done: %s, %u ms total",
                cold ? "COLD" : "WARM", (unsigned)(od_hal_uptime_ms() - tAcquire));
    pwrmgmLockGive();
    return cold;
}

// Finish a transfer/refresh. On success (and when keep-alive is enabled) the panel
// stays powered + AWAKE and enters PWR_WARM with an armed deadline; otherwise it is
// powered fully down now.
static void epdSessionRelease(bool refreshSuccess) {
    pwrmgmLockTake();
    if (pwrmgmState == PWR_OFF) { pwrmgmLockGive(); return; }   // nothing to release
    uint32_t window = epdKeepAliveWindowMs();
    if (window == 0 || !refreshSuccess) {
        od_log_info(refreshSuccess ? "[EPD session] release: keep-alive disabled, powering off"
                                   : "[EPD session] release: refresh failed, powering off");
        epdSessionForceOffLocked();
    } else {
        pwrmgmState = PWR_WARM;
        pwrmgmOffDeadlineMs = od_hal_uptime_ms() + window;
        // Controller stays AWAKE (no bbepSleep; is_awake stays 1); rail/SPI stay up.
        od_log_info("[EPD session] release: panel warm-idle, off in %u ms", (unsigned)window);
    }
    pwrmgmLockGive();
}

void epdSessionForceOff(void) {
    pwrmgmLockTake();
    epdSessionForceOffLocked();
    pwrmgmLockGive();
}

void epdSessionTick(void) {
    if (pwrmgmState != PWR_WARM) return;   // fast pre-check (only WARM arms the timer)
    if (!pwrmgmLockTryTake()) return;      // held by a transfer -> skip this pass
    // Re-check under the lock: a transfer may have moved us out of WARM meanwhile.
    if (pwrmgmState == PWR_WARM && (int32_t)(od_hal_uptime_ms() - pwrmgmOffDeadlineMs) >= 0) {
        od_log_info("[EPD session] keep-alive expired — powering panel off");
        epdSessionForceOffLocked();
    }
    pwrmgmLockGive();
}

bool epdSessionIsWarm(void) {
    return pwrmgmState == PWR_WARM;
}

static bool refreshBootScreenFull() {
    if (!writeBootScreenWithQr()) {
        od_log_warn("Boot screen render failed");
        return false;
    }
    od_log_info("EPD refresh: FULL (boot)");
    touchSuspendForEpdRefresh();
    bbepRefresh(&bbep, REFRESH_FULL);
    return waitforrefresh(60);
}

static bool panel_skips_bbep_set_addr_window(void);
static void partial_set_addr_window(BBEPDISP *pBBEP, int x, int y, int cx, int cy);
static void partial_prepare_panel_ram_for(uint16_t x, uint16_t y, uint16_t width,
                                          uint16_t height);
static bool partial_refresh_for(uint16_t x, uint16_t y, uint16_t width, uint16_t height,
                                int refreshMode);

enum XferAppMode : uint8_t {
    XFER_APP_IDLE = 0,
    XFER_APP_FULL,
    XFER_APP_PARTIAL,
};

struct XferAppHardwareState {
    XferAppMode mode;
    od_color_geometry_t geometry;
    uint16_t width;
    uint16_t height;
    uint16_t x;
    uint16_t y;
    uint32_t plane_bytes;
    uint8_t current_plane;
};

static XferAppHardwareState xferApp = {};

// od_txq's only drainer is the loop task, which is the same task running these handlers -- so
// anything queued here stays queued until we return. Call od_cmd_flush_before_refresh() before any
// multi-second blocking work (see the refresh tail).

// Shared by both watchdogs below. Measured from START, not from the last accepted
// frame, so it bounds the whole transfer rather than a stall -- see the residual
// note in docs/PLAN_WORK_GATE_TRANSFER_TERMS_2026-07-29.md before changing that.
static const uint32_t TRANSFER_WATCHDOG_MS = 900000UL;   // 15 min (upload + refresh window)

void checkTransferTimeouts(void) {
    // No "&& startTime > 0" sentinel on either watchdog: each START sets the active
    // flag and its od_hal_uptime_ms() stamp in straight-line setup with no return between, so
    // the flag already implies a valid stamp. Zero is a legitimate stamp -- treating
    // it as "unset" would permanently disable the watchdog for a transfer that began
    // in the ~1 ms window where od_hal_uptime_ms() wraps through zero. Of order one in 10^9
    // transfers, so this is removing a special case from the invariant rather than
    // fixing a live risk.
    // Both branches route through the ONE teardown routine (CONNECTION_POLICY R6's
    // teardown extended to a non-disconnect trigger). This function is cited in the
    // freeze-hardening plan as the very reason a shared routine is needed -- it is
    // where a watchdog once tore down a panel while leaving its pipe session live --
    // so exempting it would have argued for the routine while leaving the original
    // drift source untouched.
    //
    // Three deliberate behaviour changes come with it: crypto is now cleared (it
    // used to survive), the link is now dropped, and teardown is no longer selective
    // (each branch used to clean one transfer half). Dropping follows from clearing:
    // a retained link whose session is gone draws RESP_AUTH_REQUIRED with no event
    // to explain it. The client must restart the transfer either way, since the
    // transfer state is gone regardless.
    //
    // dropLink=true dispatches on the OWNER'S transport inside the abort -- this
    // watchdog is origin-agnostic (both tests below read transfer state, not
    // origin), so a timed-out LAN transfer must lose its socket, not some unrelated
    // BLE handle.
    // Drop the link only when the slot's owner is the transport that OWNS THIS
    // TRANSFER. Under the claim CAS the two agree in every sequence I can construct
    // -- a session that does not hold the slot is refused rather than admitted, and
    // every abort clears transfer state BEFORE releasing -- so this comparison is
    // defensive rather than load-bearing. It is kept because the cost is one test
    // and the failure it guards against (dropping an innocent client's link over
    // another transport's stuck transfer) is invisible from the log.
    const LinkId owner = linkOwnerId();
    const bool lanOwnsTransfer = (transferSessionOrigin() != OD_ORIGIN_BLE);
    const bool dropOwnersLink =
        (lanOwnsTransfer && owner.who == OWNER_LAN) ||
        (!lanOwnsTransfer && owner.who == OWNER_BLE);

    if (od_xfer_report_timeout(od_hal_uptime_ms(), TRANSFER_WATCHDOG_MS)) {
        abortToKnownState("shared transfer watchdog", dropOwnersLink, owner);
        return;
    }

}

#define AXP2101_SLAVE_ADDRESS 0x34
#define AXP2101_REG_POWER_STATUS 0x00
#define AXP2101_REG_DC_ONOFF_DVM_CTRL 0x80
#define AXP2101_REG_LDO_ONOFF_CTRL0 0x90
#define AXP2101_REG_DC_VOL0_CTRL 0x82
#define AXP2101_REG_LDO_VOL2_CTRL 0x94
#define AXP2101_REG_LDO_VOL3_CTRL 0x95
#define AXP2101_REG_POWER_WAKEUP_CTL 0x26
#define AXP2101_REG_ADC_CHANNEL_CTRL 0x30
#define AXP2101_REG_ADC_DATA_BAT_VOL_H 0x34
#define AXP2101_REG_ADC_DATA_VBUS_VOL_H 0x36
#define AXP2101_REG_ADC_DATA_SYS_VOL_H 0x38
#define AXP2101_REG_BAT_PERCENT_DATA 0xA4
#define AXP2101_REG_PWRON_STATUS 0x20
#define AXP2101_REG_IRQ_ENABLE1 0x40
#define AXP2101_REG_IRQ_ENABLE2 0x41
#define AXP2101_REG_IRQ_ENABLE3 0x42
#define AXP2101_REG_IRQ_ENABLE4 0x43
#define AXP2101_REG_IRQ_STATUS1 0x44
#define AXP2101_REG_IRQ_STATUS2 0x45
#define AXP2101_REG_IRQ_STATUS3 0x46
#define AXP2101_REG_IRQ_STATUS4 0x47
#define AXP2101_REG_LDO_ONOFF_CTRL1 0x91
#define FONT_BASE_WIDTH 8
#define FONT_BASE_HEIGHT 8
#define FONT_SMALL_THRESHOLD 264

extern const uint8_t writelineFont[] PROGMEM;
extern uint8_t staticRowBuffer[BOOT_ROW_BUFFER_SIZE];

int bbepSetPanelType(BBEPDISP *pBBEP, int iPanel);
void bbepSetRotation(BBEPDISP *pBBEP, int iRotation);

int mapEpd(int id){
    switch(id) {
        case 0x0000: return EP_PANEL_UNDEFINED;
        case 0x0001: return EP42_400x300;
        case 0x0002: return EP42B_400x300;
        case 0x0003: return EP213_122x250;
        case 0x0004: return EP213B_122x250;
        case 0x0005: return EP293_128x296;
        case 0x0006: return EP294_128x296;
        case 0x0007: return EP295_128x296;
        case 0x0008: return EP295_128x296_4GRAY;
        case 0x0009: return EP266_152x296;
        case 0x000A: return EP102_80x128;
        case 0x000B: return EP27B_176x264;
        case 0x000C: return EP29R_128x296;
        case 0x000D: return EP122_192x176;
        case 0x000E: return EP154R_152x152;
        case 0x000F: return EP42R_400x300;
        case 0x0010: return EP42R2_400x300;
        case 0x0011: return EP37_240x416;
        case 0x0012: return EP37B_240x416;
        case 0x0013: return EP213_104x212;
        case 0x0014: return EP75_800x480;
        case 0x0015: return EP75_800x480_4GRAY;
        case 0x0016: return EP75_800x480_4GRAY_V2;
        case 0x0017: return EP29_128x296;
        case 0x0018: return EP29_128x296_4GRAY;
        case 0x0019: return EP213R_122x250;
        case 0x001A: return EP154_200x200;
        case 0x001B: return EP154B_200x200;
        case 0x001C: return EP266YR_184x360;
        case 0x001D: return EP29YR_128x296;
        case 0x001E: return EP29YR_168x384;
        case 0x001F: return EP583_648x480;
        case 0x0020: return EP296_128x296;
        case 0x0021: return EP26R_152x296;
        case 0x0022: return EP73_800x480;
        case 0x0023: return EP73_SPECTRA_800x480;
        case 0x0024: return EP74R_640x384;
        case 0x0025: return EP583R_600x448;
        case 0x0026: return EP75R_800x480;
        case 0x0027: return EP426_800x480;
        case 0x0028: return EP426_800x480_4GRAY;
        case 0x0029: return EP29R2_128x296;
        case 0x002A: return EP41_640x400;
        case 0x002B: return EP81_SPECTRA_1024x576;
        case 0x002C: return EP7_960x640;
        case 0x002D: return EP213R2_122x250;
        case 0x002E: return EP29Z_128x296;
        case 0x002F: return EP29Z_128x296_4GRAY;
        case 0x0030: return EP213Z_122x250;
        case 0x0031: return EP213Z_122x250_4GRAY;
        case 0x0032: return EP154Z_152x152;
        case 0x0033: return EP579_792x272;
        case 0x0034: return EP213YR_122x250;
        case 0x0035: return EP37YR_240x416;
        case 0x0036: return EP35YR_184x384;
        case 0x0037: return EP397YR_800x480;
        case 0x0038: return EP154YR_200x200;
        case 0x0039: return EP266YR2_184x360;
        case 0x003A: return EP42YR_400x300;
        case 0x003B: return EP75_800x480_GEN2;
        case 0x003C: return EP75_800x480_4GRAY_GEN2;
        case 0x003D: return EP215YR_160x296;
        case 0x003E: return EP1085_1360x480;
        case 0x003F: return EP31_240x320;
        case 0x0040: return EP75YR_800x480;
        case 0x0041: return EP_PANEL_UNDEFINED;
        case 0x0042: return EP_PANEL_UNDEFINED;
        case 0x0043: return EP154_200x200_4GRAY;
        case 0x0044: return EP42B_400x300_4GRAY;
        case 0x0045: return EP397_800x480;
        case 0x0046: return EP397_800x480_4GRAY;
        case 0x0047: return EP368_792x528;
        case 0x0048: return EP368_792x528_4GRAY;
        case 0x0049: return EP213ZZ_122x250;
        case 0x004A: return EP40_SPECTRA_400x600;
        case 0x004B: return EP27_176x264;
        case 0x004C: return EP27_176x264_4GRAY;
        default: return EP_PANEL_UNDEFINED;
    }
}

bool fastepd_driver_used(void) {
#if !defined(OPENDISPLAY_FASTEPD)
    return false;
#else
    if (globalConfig.display_count < 1) return false;
    const struct DisplayConfig& d = globalConfig.displays[0];
    // FastEPD IT8951 (SPI) path: E Ink ED103TC2 (Seeed reTerminal).
    const bool it8951 = (d.panel_ic_type == OD_PANEL_IC_ED103TC2_1872X1404 ||
                         d.panel_ic_type == OD_PANEL_IC_ED103TC2_1872X1404_4GRAY);
    // FastEPD native parallel path: Soldered Inkplate 5V2 / 10.
    const bool inkplate = (d.panel_ic_type == OD_PANEL_IC_INKPLATE5V2_1280X720 ||
                           d.panel_ic_type == OD_PANEL_IC_INKPLATE10_1200X825);
    if (!it8951 && !inkplate) return false;
    if (d.display_technology != 0 && d.display_technology != 1) return false;
    return true;
#endif
}

bool waitforrefresh(int timeout){
#if defined(OPENDISPLAY_FASTEPD)
    if (fastepd_driver_used()) return fastepd_wait_refresh(timeout);
#endif
    // Poll at 10 ms (was 100 ms) so a ~0.5 s refresh returns up to ~90 ms sooner.
    // BUSY asserts within µs of MASTER_ACTIVATE, so the i==0 "never went busy"
    // error check stays valid at a 10 ms first poll. Loop bound scales x10
    // (timeout*100 iterations of 10 ms); dot cadence every 50 iters keeps ~0.5 s/dot.
    for (size_t i = 0; i < (size_t)(timeout * 100); i++){
        od_hal_delay_ms(10);
        if(i % 50 == 0) od_log_raw(".");
        if(!bbepIsBusy(&bbep)){
            if(i == 0){
                od_log_error("Epaper not busy after refresh command - refresh may not have started");
                return false;
            }
            od_log_raw(".\n");
            od_log_info("Refresh took %.2f seconds", (float)i / 100);
//            od_hal_delay_ms(200);   // EXTRA DELAY HERE IS UNNEEDED AND JUST SLOWS THINGS DOWN
            return true;
        }
    }
    od_log_warn("Refresh busy remained asserted at timeout");
    return false;
}

static bool s_wire_open_display_ready = false;
static int8_t s_wire_sda_pin = -1;
static int8_t s_wire_scl_pin = -1;
static uint32_t s_wire_clock_hz = 0;

/* Bus bring-up, with compat/Wire.h's begin() semantics preserved EXACTLY (phase C step 14).
 *
 * A named three-line function over od_hal_i2c, not a re-implementation of the Arduino object:
 * the difference matters, because the two behaviours below are load-bearing at call sites that
 * were written against them.
 *
 *   1. ALREADY-UP WINS, whatever pins are asked for. The shim returned true without touching
 *      hardware when the bus was up, and callers rely on that to be idempotent.
 *   2. NO DEFAULT PINS. odI2cBegin(-1, -1, 100000u) with no arguments passed sda = scl = -1 and therefore
 *      returned FALSE unless the bus happened to be up already. Two call sites below are
 *      commented "Uses default I2C pins" -- THEY NEVER DID. That is a pre-existing defect,
 *      translated faithfully rather than fixed: on a board where those paths matter, giving
 *      them real pins changes which bus comes up and on which GPIOs, which is a hardware
 *      question. Flagged here so it is findable.
 */
static bool odI2cBegin(int sda, int scl, uint32_t hz) {
    if (od_hal_i2c_esp_is_up()) {
        return true;
    }
    if (sda < 0 || scl < 0) {
        return false;
    }
    return od_hal_i2c_esp_begin((uint8_t)sda, (uint8_t)scl, hz);
}

static bool wireBeginForOpenDisplay(int sda, int scl, uint32_t hz) {
    // Do not pinMode() before begin on ESP32 — periman must hand pins to the I2C driver.
    if (odI2cBegin(sda, scl, hz)) {
        od_hal_i2c_esp_set_clock(hz);
        s_wire_sda_pin = (int8_t)sda;
        s_wire_scl_pin = (int8_t)scl;
        s_wire_clock_hz = hz;
        s_wire_open_display_ready = true;
        return true;
    }
    if (hz > 100000u && odI2cBegin(sda, scl, 100000u)) {
        od_log_info("NOTE: I2C fallback to 100kHz (SDA=GPIO%d SCL=GPIO%d)", sda, scl);
        od_hal_i2c_esp_set_clock(100000u);
        s_wire_sda_pin = (int8_t)sda;
        s_wire_scl_pin = (int8_t)scl;
        s_wire_clock_hz = 100000u;
        s_wire_open_display_ready = true;
        return true;
    }
    od_log_error("I2C bus init failed (SDA=GPIO%d SCL=GPIO%d)", sda, scl);
    return false;
}

// Usable as an I2C bus, asked of a RECORD rather than a position: whether a bus can be driven is
// a property of its pins, and which bus a device meant is od_config_data_bus()'s question.
static bool dataBusUsable(const struct DataBus& bus) {
    return bus.bus_type == 0x01 && bus.pin_1 != 0xFF && bus.pin_2 != 0xFF;
}

// The first usable I2C record, or nullptr. For consumers that want "whichever bus exists" rather
// than a particular one -- the panel's own bring-up, which no config field names.
static const struct DataBus* firstUsableI2cBus(void) {
    for (uint8_t i = 0; i < globalConfig.data_bus_count; i++) {
        if (dataBusUsable(globalConfig.data_buses[i])) {
            return &globalConfig.data_buses[i];
        }
    }
    return nullptr;
}

bool openDisplayI2cBusConfigured(void) {
    return firstUsableI2cBus() != nullptr;
}

void invalidateOpenDisplayWire(void) {
    if (s_wire_open_display_ready) {
        od_hal_i2c_esp_deinit();
    }
    s_wire_open_display_ready = false;
}

// bus_id is a DataBus.instance_number. 0xFF means the device was never assigned one, and is
// REFUSED rather than resolved to bus 0: substituting a bus probes an unassigned device on
// somebody else's pins, where an address collision returns a plausible-but-wrong reading instead
// of a failure (DIVERGENCE_MATRIX 13).
static bool selectConfiguredBus(uint8_t bus_id);

/* The seam shared/hal/od_hal_i2c.h's four operations call before every transaction. It lives on
 * this side because resolving a bus_id means reading globalConfig; the HAL owns the IDF bus
 * handle and nothing above it.
 *
 * IT TAKES bus_id LITERALLY, including 0xFF, because that is what the shared header promises:
 * refusing the unassigned sentinel is the CONSUMER's rule -- sensors, touch and the PMIC each
 * apply it before they get here -- and a transport that applies it too makes a record genuinely
 * numbered 0xFF unreachable. */
extern "C" bool od_hal_i2c_esp_select(uint8_t bus_id) {
    return selectConfiguredBus(bus_id);
}

/* Refuses the unassigned sentinel, then selects. For the callers that pre-open a bus rather than
 * transacting through the shared operations. */
bool initOrRestoreWireForBus(uint8_t bus_id) {
    if (bus_id == 0xFF) {
        return false;
    }
    return selectConfiguredBus(bus_id);
}

static bool selectConfiguredBus(uint8_t bus_id) {
    const struct DataBus* found = od_config_data_bus(&globalConfig, bus_id);
    if (found == nullptr || !dataBusUsable(*found)) {
        return false;
    }
    const struct DataBus& bus = *found;
    uint32_t hz = bus.bus_speed_hz ? bus.bus_speed_hz : 100000u;
    int sda = (int)bus.pin_2;
    int scl = (int)bus.pin_1;
    /* Speed is part of the identity, not just the pins. IDF fixes the clock when a DEVICE handle
     * is attached, so two instances sharing pins at different rates would otherwise leave the
     * second running at the first one's rate -- and a cached device handle keeps it. */
    if (s_wire_open_display_ready && s_wire_sda_pin == sda && s_wire_scl_pin == scl &&
        s_wire_clock_hz == hz) {
        return true;
    }
    if (s_wire_open_display_ready) {
        od_hal_i2c_esp_deinit();
        s_wire_open_display_ready = false;
    }
    if (!wireBeginForOpenDisplay(sda, scl, hz)) {
        s_wire_open_display_ready = false;
        return false;
    }
    return true;
}

void initOrRestoreWireForOpenDisplay(void) {
    // The panel has no bus_id of its own and takes the FIRST RECORD, which is what this has
    // always done -- the change here is only that it asks for that record's instance_number
    // rather than for instance 0, because those are no longer the same question. Which record
    // the panel should prefer is a separate decision and not this cutover's to make.
    if (globalConfig.data_bus_count > 0 && dataBusUsable(globalConfig.data_buses[0])) {
        (void)initOrRestoreWireForBus(globalConfig.data_buses[0].instance_number);
        return;
    }
    if (!s_wire_open_display_ready) {
        if (odI2cBegin(-1, -1, 100000u)) {
            s_wire_open_display_ready = true;
        }
    }
}

void initDataBuses(){
    od_log_info("=== Initializing Data Buses ===");
    if(globalConfig.data_bus_count == 0){
        od_log_info("No data buses configured");
        return;
    }
    for(uint8_t i = 0; i < globalConfig.data_bus_count; i++){
        struct DataBus* bus = &globalConfig.data_buses[i];
        if(bus->bus_type == 0x01){ // I2C bus
            od_log_info("Initializing I2C bus %u (instance %u)", i, bus->instance_number);
            if(bus->pin_1 == 0xFF || bus->pin_2 == 0xFF){
                od_log_error("Invalid I2C pins for bus %u (SCL=%u, SDA=%u)", i, bus->pin_1, bus->pin_2);
                continue;
            }
            uint32_t busSpeed = (bus->bus_speed_hz > 0) ? bus->bus_speed_hz : 100000;
            if(i == 0){
                initOrRestoreWireForOpenDisplay();
                od_log_info("I2C bus %u initialized: SCL=pin%u, SDA=pin%u, Speed=%uHz", i, bus->pin_1, bus->pin_2, (unsigned)busSpeed);
            } else {
                od_log_info("I2C bus %u configured (init on demand): SCL=pin%u, SDA=pin%u, Speed=%uHz",
                    i, bus->pin_1, bus->pin_2, (unsigned)busSpeed);
            }
        }
        else if(bus->bus_type == 0x02){
            od_log_info("SPI bus %u detected (not yet implemented)", i);
            od_log_info("  Instance: %u", bus->instance_number);
        }
        else{
            od_log_warn("Unknown bus type 0x%02X for bus %u", bus->bus_type, i);
        }
    }
    od_log_info("=== Data Bus Initialization Complete ===");
}

void initio(){
    od_log_info("[initio] >> LEDs"); od_log_flush();
    if(globalConfig.led_count > 0){
        for (uint8_t i = 0; i < globalConfig.led_count; i++) {
            struct LedConfig* led = &globalConfig.leds[i];
            bool invertRed = (led->led_flags & 0x01) != 0;
            bool invertGreen = (led->led_flags & 0x02) != 0;
            bool invertBlue = (led->led_flags & 0x04) != 0;
            bool invertLed4 = (led->led_flags & 0x08) != 0;
                if (led->led_1_r != 0xFF) {
                    od_hal_gpio_config_output(led->led_1_r, (invertRed ? HIGH : LOW));
                }
                if (led->led_2_g != 0xFF) {
                    od_hal_gpio_config_output(led->led_2_g, (invertGreen ? HIGH : LOW));
                }
                if (led->led_3_b != 0xFF) {
                    od_hal_gpio_config_output(led->led_3_b, (invertBlue ? HIGH : LOW));
                }
                if (led->led_4 != 0xFF) {
                    od_hal_gpio_config_output(led->led_4, (invertLed4 ? HIGH : LOW));
                }
        }
        for (uint8_t i = 0; i < globalConfig.led_count; i++) {
            if (globalConfig.leds[i].led_type == 0) {
                activeLedInstance = i;
                {
                    flashLed(0xE0, 15);
                    flashLed(0x1C, 15);
                    flashLed(0x03, 15);
                    flashLed(0xFF, 15);
                }
            }
        }
    }
    od_log_info("[initio] >> initPassiveBuzzers"); od_log_flush();
    initPassiveBuzzers();
    od_log_info("[initio] >> pwr_pin"); od_log_flush();
    if(globalConfig.system_config.pwr_pin != 0xFF){
    od_hal_gpio_config_output(globalConfig.system_config.pwr_pin, false);
    }
    else{
        od_log_warn("Power pin not set");
    }
    od_log_info("[initio] >> initDataBuses"); od_log_flush();
    initDataBuses();
    od_log_info("[initio] >> initSensors"); od_log_flush();
    initSensors();
    od_log_info("[initio] << done"); od_log_flush();
}

void scanI2CDevices(){
    od_log_info("=== Scanning I2C Bus for Devices ===");
    initOrRestoreWireForOpenDisplay();
    uint8_t deviceCount = 0;
    uint8_t foundDevices[128];
    for(uint8_t address = 0x08; address < 0x78; address++){
        uint8_t error = (uint8_t)od_hal_i2c_esp_probe_current(address);
        if(error == 0){
            foundDevices[deviceCount] = address;
            deviceCount++;
            od_log_debug("I2C device found at address 0x%02X (%u)", address, address);
        }
        else if(error == 4){
            od_log_error("Unknown error at address 0x%02X", address);
        }
    }
    if(deviceCount == 0){
        od_log_warn("No I2C devices found on bus");
    } else {
        od_log_debug("Found %u I2C device(s)", deviceCount);
        od_log_debug("Device addresses: ");
        char addrList[700];
        int pos = snprintf(addrList, sizeof(addrList), "%s", "");
        if (pos < 0) {
            pos = 0;
            addrList[0] = '\0';
        }
        for(uint8_t i = 0; i < deviceCount && pos < (int)sizeof(addrList); i++){
            int n = snprintf(addrList + pos, sizeof(addrList) - pos, i > 0 ? ", 0x%02X" : "0x%02X", foundDevices[i]);
            if (n < 0) {
                break;
            }
            pos += n;
        }
        od_log_debug("%s", addrList);
    }
    od_log_info("=== I2C Scan Complete ===");
}

void initSensors(){
    od_log_info("=== Initializing Sensors ===");
    if(globalConfig.sensor_count == 0){
        od_log_warn("No sensors configured");
        return;
    }
    for(uint8_t i = 0; i < globalConfig.sensor_count; i++){
        struct SensorData* sensor = &globalConfig.sensors[i];
        od_log_debug("Initializing sensor %u (instance %u)", i, sensor->instance_number);
        od_log_debug("  Type: 0x%04X", sensor->sensor_type);
        od_log_debug("  Bus ID: %u", sensor->bus_id);
        if(sensor->sensor_type == OD_SENSOR_TYPE_AXP2101){
            od_log_debug("  Detected AXP2101 PMIC sensor");
        }
        else if(sensor->sensor_type == OD_SENSOR_TYPE_TEMPERATURE){
            od_log_debug("  Temperature sensor (initialization not implemented)");
        }
        else if(sensor->sensor_type == OD_SENSOR_TYPE_HUMIDITY){
            od_log_debug("  Humidity sensor (initialization not implemented)");
        }
        else if(sensor->sensor_type == OD_SENSOR_TYPE_SHT40){
            od_log_debug("  SHT40 (I2C + MSD slot)");
        }
        else if(sensor->sensor_type == OD_SENSOR_TYPE_BQ27220){
            od_log_debug("  BQ27220 fuel gauge (MSD voltage + optional dynamic SOC/status bytes)");
        }
        else{
            od_log_warn("  Unknown sensor type 0x%04X", sensor->sensor_type);
        }
    }
    od_sensor_sht40_init(&globalConfig);
    od_sensor_bq27220_init(&globalConfig);
    od_log_info("=== Sensor Initialization Complete ===");
}

/* The bus initAXP2101() was given. readAXP2101Data() has no argument and the PMIC does not
 * move, so the bus is remembered rather than guessed -- reading it on "whichever bus is up"
 * is the same class of defect as substituting bus 0. */
static uint8_t s_axp2101_bus_id = 0xFF;

void initAXP2101(uint8_t busId){
    od_hal_gpio_config_output(21, false);
    od_hal_delay_ms(100);
    od_hal_gpio_write(21, true);
    od_log_info("=== Initializing AXP2101 PMIC ===");
    // Resolved the same way initOrRestoreWireForBus() resolves it, or these prechecks can pass
    // on one record while the call below selects another (DIVERGENCE_MATRIX 14).
    const struct DataBus* bus = od_config_data_bus(&globalConfig, busId);
    if(bus == nullptr){
        od_log_error("Bus instance %u is absent or declared more than once (%u buses configured)",
                     busId, globalConfig.data_bus_count);
        return;
    }
    if(bus->bus_type != 0x01){
        od_log_error("Bus %u is not an I2C bus", busId);
        return;
    }
    s_axp2101_bus_id = busId;
    if(!initOrRestoreWireForBus(busId)){
        od_log_error("Failed to (re)init I2C bus %u for AXP2101", busId);
        return;
    }
    uint8_t error = (uint8_t)od_hal_i2c_probe(busId, AXP2101_SLAVE_ADDRESS);
    if(error != 0){
        od_log_error("AXP2101 not found at address 0x%02X (error: %u)", AXP2101_SLAVE_ADDRESS, error);
        return;
    }
    od_log_debug("AXP2101 detected at address 0x%02X", AXP2101_SLAVE_ADDRESS);
    const uint8_t od_i2c_tx2 = (uint8_t)(AXP2101_REG_POWER_STATUS);
    error = od_hal_i2c_write(busId, AXP2101_SLAVE_ADDRESS, &od_i2c_tx2, 1);
    if(error == 0){
        uint8_t od_i2c_rx0 = 0;
        if (od_hal_i2c_read(busId, AXP2101_SLAVE_ADDRESS, &od_i2c_rx0, 1) == OD_HAL_I2C_OK) {
            uint8_t status = od_i2c_rx0;
            od_log_debug("Power status: 0x%02X", status);
        }
    }
    const uint8_t od_i2c_tx3[2] = { (uint8_t)(AXP2101_REG_DC_VOL0_CTRL), (uint8_t)(0x12) };
    error = od_hal_i2c_write(busId, AXP2101_SLAVE_ADDRESS, od_i2c_tx3, 2);
    if(error == 0){
        od_log_debug("DCDC1 voltage set to 3.3V");
    } else {
        od_log_error("Failed to set DCDC1 voltage");
    }
    od_hal_delay_ms(10);
    const uint8_t od_i2c_tx4 = (uint8_t)(AXP2101_REG_DC_ONOFF_DVM_CTRL);
    error = od_hal_i2c_write(busId, AXP2101_SLAVE_ADDRESS, &od_i2c_tx4, 1);
    uint8_t dcEnable = 0x00;
    if(error == 0){
        uint8_t od_i2c_rx1 = 0;
        if (od_hal_i2c_read(busId, AXP2101_SLAVE_ADDRESS, &od_i2c_rx1, 1) == OD_HAL_I2C_OK) {
            dcEnable = od_i2c_rx1;
        }
    }
    dcEnable |= 0x01;
    const uint8_t od_i2c_tx5[2] = { (uint8_t)(AXP2101_REG_DC_ONOFF_DVM_CTRL), (uint8_t)(dcEnable) };
    error = od_hal_i2c_write(busId, AXP2101_SLAVE_ADDRESS, od_i2c_tx5, 2);
    if(error == 0){
        od_log_debug("DCDC1 enabled (3.3V)");
    } else {
        od_log_error("Failed to enable DCDC1");
    }
    od_hal_delay_ms(10);
    const uint8_t od_i2c_tx6 = (uint8_t)(AXP2101_REG_LDO_ONOFF_CTRL0);
    error = od_hal_i2c_write(busId, AXP2101_SLAVE_ADDRESS, &od_i2c_tx6, 1);
    uint8_t aldoEnable = 0x00;
    if(error == 0){
        uint8_t od_i2c_rx2 = 0;
        if (od_hal_i2c_read(busId, AXP2101_SLAVE_ADDRESS, &od_i2c_rx2, 1) == OD_HAL_I2C_OK) {
            aldoEnable = od_i2c_rx2;
        }
    }
    const uint8_t od_i2c_tx7 = (uint8_t)(AXP2101_REG_LDO_VOL2_CTRL);
    error = od_hal_i2c_write(busId, AXP2101_SLAVE_ADDRESS, &od_i2c_tx7, 1);
    uint8_t aldo3VolReg = 0x00;
    if(error == 0){
        uint8_t od_i2c_rx3 = 0;
        if (od_hal_i2c_read(busId, AXP2101_SLAVE_ADDRESS, &od_i2c_rx3, 1) == OD_HAL_I2C_OK) {
            aldo3VolReg = od_i2c_rx3;
        }
    }
    aldo3VolReg = (aldo3VolReg & 0xE0) | 0x1C;
    const uint8_t od_i2c_tx8[2] = { (uint8_t)(AXP2101_REG_LDO_VOL2_CTRL), (uint8_t)(aldo3VolReg) };
    error = od_hal_i2c_write(busId, AXP2101_SLAVE_ADDRESS, od_i2c_tx8, 2);
    if(error == 0){
        od_log_debug("ALDO3 voltage set to 3.3V");
    }
    const uint8_t od_i2c_tx9 = (uint8_t)(AXP2101_REG_LDO_VOL3_CTRL);
    error = od_hal_i2c_write(busId, AXP2101_SLAVE_ADDRESS, &od_i2c_tx9, 1);
    uint8_t aldo4VolReg = 0x00;
    if(error == 0){
        uint8_t od_i2c_rx4 = 0;
        if (od_hal_i2c_read(busId, AXP2101_SLAVE_ADDRESS, &od_i2c_rx4, 1) == OD_HAL_I2C_OK) {
            aldo4VolReg = od_i2c_rx4;
        }
    }
    aldo4VolReg = (aldo4VolReg & 0xE0) | 0x1C;
    const uint8_t od_i2c_tx10[2] = { (uint8_t)(AXP2101_REG_LDO_VOL3_CTRL), (uint8_t)(aldo4VolReg) };
    error = od_hal_i2c_write(busId, AXP2101_SLAVE_ADDRESS, od_i2c_tx10, 2);
    if(error == 0){
        od_log_debug("ALDO4 voltage set to 3.3V");
    }
    aldoEnable |= 0x0C;
    const uint8_t od_i2c_tx11[2] = { (uint8_t)(AXP2101_REG_LDO_ONOFF_CTRL0), (uint8_t)(aldoEnable) };
    error = od_hal_i2c_write(busId, AXP2101_SLAVE_ADDRESS, od_i2c_tx11, 2);
    if(error == 0){
        od_log_debug("ALDO3 and ALDO4 enabled (3.3V)");
    }
    const uint8_t od_i2c_tx12 = (uint8_t)(AXP2101_REG_POWER_WAKEUP_CTL);
    error = od_hal_i2c_write(busId, AXP2101_SLAVE_ADDRESS, &od_i2c_tx12, 1);
    if(error == 0){
        uint8_t od_i2c_rx5 = 0;
        if (od_hal_i2c_read(busId, AXP2101_SLAVE_ADDRESS, &od_i2c_rx5, 1) == OD_HAL_I2C_OK) {
            uint8_t wakeupCtl = od_i2c_rx5;
            od_log_debug("Wakeup control: 0x%02X", wakeupCtl);
            if(wakeupCtl & 0x01){
                od_log_debug("Wakeup already enabled");
            } else {
                const uint8_t od_i2c_tx13[2] = { (uint8_t)(AXP2101_REG_POWER_WAKEUP_CTL), (uint8_t)(wakeupCtl | 0x01) };
                error = od_hal_i2c_write(busId, AXP2101_SLAVE_ADDRESS, od_i2c_tx13, 2);
                if(error == 0){
                    od_log_debug("Wakeup enabled");
                }
            }
        }
    }
    od_log_info("=== AXP2101 PMIC Initialization Complete ===");
}

void readAXP2101Data(){
    const uint8_t busId = s_axp2101_bus_id;
    if (busId == 0xFF) {
        return;   /* never initialised: no bus, so nothing to read */
    }
    od_log_info("=== Reading AXP2101 PMIC Data ===");
    uint8_t error = (uint8_t)od_hal_i2c_probe(busId, AXP2101_SLAVE_ADDRESS);
    if(error != 0){
        od_log_error("AXP2101 not found at address 0x%02X", AXP2101_SLAVE_ADDRESS);
        return;
    }
    const uint8_t od_i2c_tx15[2] = { (uint8_t)(AXP2101_REG_ADC_CHANNEL_CTRL), (uint8_t)(0xFF) };
    error = od_hal_i2c_write(busId, AXP2101_SLAVE_ADDRESS, od_i2c_tx15, 2);
    od_hal_delay_ms(10);
    const uint8_t od_i2c_tx16 = (uint8_t)(AXP2101_REG_POWER_STATUS);
    error = od_hal_i2c_write(busId, AXP2101_SLAVE_ADDRESS, &od_i2c_tx16, 1);
    if(error == 0){
        uint8_t od_i2c_rx100[2] = { 0 };
        if (od_hal_i2c_read(busId, AXP2101_SLAVE_ADDRESS, od_i2c_rx100, 2) == OD_HAL_I2C_OK) {
            uint8_t status1 = od_i2c_rx100[0];
            uint8_t status2 = od_i2c_rx100[1];
            od_log_debug("Power Status 1: 0x%02X", status1);
            od_log_debug("Power Status 2: 0x%02X", status2);
            bool batteryPresent = (status1 & 0x20) != 0;
            bool charging = (status1 & 0x04) != 0;
            bool vbusPresent = (status1 & 0x08) != 0;
            od_log_debug("Battery Present: %s", batteryPresent ? "Yes" : "No");
            od_log_debug("Charging: %s", charging ? "Yes" : "No");
            od_log_debug("VBUS Present: %s", vbusPresent ? "Yes" : "No");
        }
    }
    const uint8_t od_i2c_tx17 = (uint8_t)(AXP2101_REG_PWRON_STATUS);
    error = od_hal_i2c_write(busId, AXP2101_SLAVE_ADDRESS, &od_i2c_tx17, 1);
    if(error == 0){
        uint8_t od_i2c_rx6 = 0;
        if (od_hal_i2c_read(busId, AXP2101_SLAVE_ADDRESS, &od_i2c_rx6, 1) == OD_HAL_I2C_OK) {
            uint8_t pwronStatus = od_i2c_rx6;
            od_log_debug("Power On Status: 0x%02X", pwronStatus);
        }
    }
    const uint8_t od_i2c_tx18 = (uint8_t)(AXP2101_REG_ADC_DATA_BAT_VOL_H);
    error = od_hal_i2c_write(busId, AXP2101_SLAVE_ADDRESS, &od_i2c_tx18, 1);
    if(error == 0){
        uint8_t od_i2c_rx101[2] = { 0 };
        if (od_hal_i2c_read(busId, AXP2101_SLAVE_ADDRESS, od_i2c_rx101, 2) == OD_HAL_I2C_OK) {
            uint8_t batVolH = od_i2c_rx101[0];
            uint8_t batVolL = od_i2c_rx101[1];
            uint16_t batVolRaw = ((uint16_t)batVolH << 4) | (batVolL & 0x0F);
            float batVoltage = batVolRaw * 0.5;
            od_log_debug("Battery Voltage: %.1f mV (%.2f V)", batVoltage, batVoltage / 1000.0);
        }
    }
    const uint8_t od_i2c_tx19 = (uint8_t)(AXP2101_REG_ADC_DATA_VBUS_VOL_H);
    error = od_hal_i2c_write(busId, AXP2101_SLAVE_ADDRESS, &od_i2c_tx19, 1);
    if(error == 0){
        uint8_t od_i2c_rx102[2] = { 0 };
        if (od_hal_i2c_read(busId, AXP2101_SLAVE_ADDRESS, od_i2c_rx102, 2) == OD_HAL_I2C_OK) {
            uint8_t vbusVolH = od_i2c_rx102[0];
            uint8_t vbusVolL = od_i2c_rx102[1];
            uint16_t vbusVolRaw = ((uint16_t)vbusVolH << 4) | (vbusVolL & 0x0F);
            float vbusVoltage = vbusVolRaw * 1.7;
            od_log_debug("VBUS Voltage: %.1f mV (%.2f V)", vbusVoltage, vbusVoltage / 1000.0);
        }
    }
    const uint8_t od_i2c_tx20 = (uint8_t)(AXP2101_REG_ADC_DATA_SYS_VOL_H);
    error = od_hal_i2c_write(busId, AXP2101_SLAVE_ADDRESS, &od_i2c_tx20, 1);
    if(error == 0){
        uint8_t od_i2c_rx103[2] = { 0 };
        if (od_hal_i2c_read(busId, AXP2101_SLAVE_ADDRESS, od_i2c_rx103, 2) == OD_HAL_I2C_OK) {
            uint8_t sysVolH = od_i2c_rx103[0];
            uint8_t sysVolL = od_i2c_rx103[1];
            uint16_t sysVolRaw = ((uint16_t)sysVolH << 4) | (sysVolL & 0x0F);
            float sysVoltage = sysVolRaw * 1.4;
            od_log_debug("System Voltage: %.1f mV (%.2f V)", sysVoltage, sysVoltage / 1000.0);
        }
    }
    const uint8_t od_i2c_tx21 = (uint8_t)(AXP2101_REG_BAT_PERCENT_DATA);
    error = od_hal_i2c_write(busId, AXP2101_SLAVE_ADDRESS, &od_i2c_tx21, 1);
    if(error == 0){
        uint8_t od_i2c_rx7 = 0;
        if (od_hal_i2c_read(busId, AXP2101_SLAVE_ADDRESS, &od_i2c_rx7, 1) == OD_HAL_I2C_OK) {
            uint8_t batPercent = od_i2c_rx7;
            if(batPercent <= 100){
                od_log_debug("Battery Percentage: %u%%", batPercent);
            } else {
                od_log_debug("Battery Percentage: Not available (fuel gauge may be disabled)");
            }
        }
    }
    const uint8_t od_i2c_tx22 = (uint8_t)(AXP2101_REG_DC_ONOFF_DVM_CTRL);
    error = od_hal_i2c_write(busId, AXP2101_SLAVE_ADDRESS, &od_i2c_tx22, 1);
    if(error == 0){
        uint8_t od_i2c_rx8 = 0;
        if (od_hal_i2c_read(busId, AXP2101_SLAVE_ADDRESS, &od_i2c_rx8, 1) == OD_HAL_I2C_OK) {
            uint8_t dcEnable = od_i2c_rx8;
            od_log_debug("DC Enable Status: 0x%02X", dcEnable);
            od_log_debug("  DCDC1: %s", (dcEnable & 0x01) ? "ON" : "OFF");
            od_log_debug("  DCDC2: %s", (dcEnable & 0x02) ? "ON" : "OFF");
            od_log_debug("  DCDC3: %s", (dcEnable & 0x04) ? "ON" : "OFF");
            od_log_debug("  DCDC4: %s", (dcEnable & 0x08) ? "ON" : "OFF");
            od_log_debug("  DCDC5: %s", (dcEnable & 0x10) ? "ON" : "OFF");
        }
    }
    const uint8_t od_i2c_tx23 = (uint8_t)(AXP2101_REG_LDO_ONOFF_CTRL0);
    error = od_hal_i2c_write(busId, AXP2101_SLAVE_ADDRESS, &od_i2c_tx23, 1);
    if(error == 0){
        uint8_t od_i2c_rx9 = 0;
        if (od_hal_i2c_read(busId, AXP2101_SLAVE_ADDRESS, &od_i2c_rx9, 1) == OD_HAL_I2C_OK) {
            uint8_t aldoEnable = od_i2c_rx9;
            od_log_debug("ALDO Enable Status: 0x%02X", aldoEnable);
            od_log_debug("  ALDO1: %s", (aldoEnable & 0x01) ? "ON" : "OFF");
            od_log_debug("  ALDO2: %s", (aldoEnable & 0x02) ? "ON" : "OFF");
            od_log_debug("  ALDO3: %s", (aldoEnable & 0x04) ? "ON" : "OFF");
            od_log_debug("  ALDO4: %s", (aldoEnable & 0x08) ? "ON" : "OFF");
        }
    }
    od_log_info("=== AXP2101 Data Read Complete ===");
}

void powerDownAXP2101(){
    const uint8_t busId = s_axp2101_bus_id;

    od_log_info("=== Powering Down AXP2101 PMIC Rails ===");
    if (busId == 0xFF) {
        return;   /* never initialised: no bus, so nothing to power down over I2C */
    }
    uint8_t error = (uint8_t)od_hal_i2c_probe(busId, AXP2101_SLAVE_ADDRESS);
    if(error != 0){
        od_log_error("AXP2101 not found at address 0x%02X (error: %u)", AXP2101_SLAVE_ADDRESS, error);
        return;
    }
    const uint8_t od_i2c_tx25[2] = { (uint8_t)(AXP2101_REG_IRQ_ENABLE1), (uint8_t)(0x00) };
    error = od_hal_i2c_write(busId, AXP2101_SLAVE_ADDRESS, od_i2c_tx25, 2);
    if(error == 0){
        const uint8_t od_i2c_tx26[2] = { (uint8_t)(AXP2101_REG_IRQ_ENABLE2), (uint8_t)(0x00) };
        error = od_hal_i2c_write(busId, AXP2101_SLAVE_ADDRESS, od_i2c_tx26, 2);
    }
    if(error == 0){
        const uint8_t od_i2c_tx27[2] = { (uint8_t)(AXP2101_REG_IRQ_ENABLE3), (uint8_t)(0x00) };
        error = od_hal_i2c_write(busId, AXP2101_SLAVE_ADDRESS, od_i2c_tx27, 2);
    }
    if(error == 0){
        const uint8_t od_i2c_tx28[2] = { (uint8_t)(AXP2101_REG_IRQ_ENABLE4), (uint8_t)(0x00) };
        error = od_hal_i2c_write(busId, AXP2101_SLAVE_ADDRESS, od_i2c_tx28, 2);
    }
    if(error == 0){
        const uint8_t od_i2c_tx29[2] = { (uint8_t)(AXP2101_REG_IRQ_STATUS1), (uint8_t)(0xFF) };
        error = od_hal_i2c_write(busId, AXP2101_SLAVE_ADDRESS, od_i2c_tx29, 2);
    }
    if(error == 0){
        const uint8_t od_i2c_tx30[2] = { (uint8_t)(AXP2101_REG_IRQ_STATUS2), (uint8_t)(0xFF) };
        error = od_hal_i2c_write(busId, AXP2101_SLAVE_ADDRESS, od_i2c_tx30, 2);
    }
    if(error == 0){
        const uint8_t od_i2c_tx31[2] = { (uint8_t)(AXP2101_REG_IRQ_STATUS3), (uint8_t)(0xFF) };
        error = od_hal_i2c_write(busId, AXP2101_SLAVE_ADDRESS, od_i2c_tx31, 2);
    }
    if(error == 0){
        const uint8_t od_i2c_tx32[2] = { (uint8_t)(AXP2101_REG_IRQ_STATUS4), (uint8_t)(0xFF) };
        error = od_hal_i2c_write(busId, AXP2101_SLAVE_ADDRESS, od_i2c_tx32, 2);
        if(error == 0){
            od_log_debug("All IRQs disabled and status cleared");
        }
    }
    const uint8_t od_i2c_tx33 = (uint8_t)(AXP2101_REG_DC_ONOFF_DVM_CTRL);
    error = od_hal_i2c_write(busId, AXP2101_SLAVE_ADDRESS, &od_i2c_tx33, 1);
    uint8_t dcEnable = 0x00;
    if(error == 0){
        uint8_t od_i2c_rx10 = 0;
        if (od_hal_i2c_read(busId, AXP2101_SLAVE_ADDRESS, &od_i2c_rx10, 1) == OD_HAL_I2C_OK) {
            dcEnable = od_i2c_rx10;
        }
    }
    dcEnable &= 0x01;
    const uint8_t od_i2c_tx34[2] = { (uint8_t)(AXP2101_REG_DC_ONOFF_DVM_CTRL), (uint8_t)(dcEnable) };
    error = od_hal_i2c_write(busId, AXP2101_SLAVE_ADDRESS, od_i2c_tx34, 2);
    if(error == 0){
        od_log_debug("DC2-5 disabled (DC1 kept enabled)");
    } else {
        od_log_error("Failed to disable DC2-5");
    }
    const uint8_t od_i2c_tx35[2] = { (uint8_t)(AXP2101_REG_LDO_ONOFF_CTRL1), (uint8_t)(0x00) };
    error = od_hal_i2c_write(busId, AXP2101_SLAVE_ADDRESS, od_i2c_tx35, 2);
    if(error == 0){
        od_log_debug("BLDO1-2, CPUSLDO, DLDO1-2 disabled");
    } else {
        od_log_error("Failed to disable BLDO/CPUSLDO/DLDO rails");
    }
    const uint8_t od_i2c_tx36 = (uint8_t)(AXP2101_REG_LDO_ONOFF_CTRL0);
    error = od_hal_i2c_write(busId, AXP2101_SLAVE_ADDRESS, &od_i2c_tx36, 1);
    uint8_t aldoEnable = 0x00;
    if(error == 0){
        uint8_t od_i2c_rx11 = 0;
        if (od_hal_i2c_read(busId, AXP2101_SLAVE_ADDRESS, &od_i2c_rx11, 1) == OD_HAL_I2C_OK) {
            aldoEnable = od_i2c_rx11;
        }
    }
    aldoEnable &= ~0x0F;
    const uint8_t od_i2c_tx37[2] = { (uint8_t)(AXP2101_REG_LDO_ONOFF_CTRL0), (uint8_t)(aldoEnable) };
    error = od_hal_i2c_write(busId, AXP2101_SLAVE_ADDRESS, od_i2c_tx37, 2);
    if(error == 0){
        od_log_debug("ALDO1-4 disabled");
    } else {
        od_log_error("Failed to disable ALDO rails");
    }
    const uint8_t od_i2c_tx38 = (uint8_t)(AXP2101_REG_POWER_WAKEUP_CTL);
    error = od_hal_i2c_write(busId, AXP2101_SLAVE_ADDRESS, &od_i2c_tx38, 1);
    uint8_t wakeupCtrl = 0x00;
    if(error == 0){
        uint8_t od_i2c_rx12 = 0;
        if (od_hal_i2c_read(busId, AXP2101_SLAVE_ADDRESS, &od_i2c_rx12, 1) == OD_HAL_I2C_OK) {
            wakeupCtrl = od_i2c_rx12;
        }
    }
    if(!(wakeupCtrl & 0x04)) {
        wakeupCtrl |= 0x04;
    }
    if(wakeupCtrl & 0x08) {
        wakeupCtrl &= ~0x08;
    }
    if(!(wakeupCtrl & 0x10)) {
        wakeupCtrl |= 0x10;
    }
    wakeupCtrl |= 0x80;
    const uint8_t od_i2c_tx39[2] = { (uint8_t)(AXP2101_REG_POWER_WAKEUP_CTL), (uint8_t)(wakeupCtrl) };
    error = od_hal_i2c_write(busId, AXP2101_SLAVE_ADDRESS, od_i2c_tx39, 2);
    if(error == 0){
        od_log_debug("AXP2101 wake-up configured and sleep mode enabled");
    } else {
        od_log_error("Failed to configure AXP2101 sleep mode");
    }
    const uint8_t od_i2c_tx40[2] = { (uint8_t)(AXP2101_REG_ADC_CHANNEL_CTRL), (uint8_t)(0x00) };
    error = od_hal_i2c_write(busId, AXP2101_SLAVE_ADDRESS, od_i2c_tx40, 2);
    if(error == 0){
        od_log_debug("All ADC channels disabled");
    } else {
        od_log_error("Failed to disable ADC channels");
    }
    od_log_info("=== AXP2101 PMIC Rails Powered Down ===");
}

static void renderChar_4BPP(uint8_t* rowBuffer, const uint8_t* fontData, int fontRow, int charIdx, int startX, int charWidth, int pitch, int fontScale) {
    for (int col = 0; col < charWidth; col += fontScale) {
        uint8_t fontByte;
        int fontCol = col / fontScale;
        if (fontCol == 0 || fontCol > 7) {
            fontByte = 0x00;
        } else {
            fontByte = fontData[fontCol - 1];
        }
        uint8_t pixelBit = (fontByte >> fontRow) & 0x01;
        uint8_t pixelNibble = (pixelBit == 1) ? 0x0 : 0xF;
        for (int s = 0; s < fontScale; s++) {
            int pixelX = startX + charIdx * charWidth + col + s;
            if (pixelX >= globalConfig.displays[0].pixel_width) break;
            int bytePos = pixelX / 2;
            if (bytePos >= pitch) break;
            if ((pixelX % 2) == 0) {
                rowBuffer[bytePos] = (rowBuffer[bytePos] & 0x0F) | (pixelNibble << 4);
            } else {
                rowBuffer[bytePos] = (rowBuffer[bytePos] & 0xF0) | pixelNibble;
            }
        }
    }
}

static void renderChar_2BPP(uint8_t* rowBuffer, const uint8_t* fontData, int fontRow, int charIdx, int startX, int charWidth, int pitch, uint8_t colorScheme, int fontScale) {
    uint8_t whiteCode = (colorScheme == OD_COLOR_SCHEME_GRAY4) ? 0x03 : 0x01;
    int pixelsPerByte = 4;
    for (int col = 0; col < charWidth; col += pixelsPerByte) {
        uint8_t pixelByte = 0;
        for (int p = 0; p < pixelsPerByte; p++) {
            int pixelX = startX + charIdx * charWidth + col + p;
            if (pixelX >= globalConfig.displays[0].pixel_width) break;
            uint8_t fontByte;
            int fontCol = (col + p) / fontScale;
            if (fontCol == 0 || fontCol > 7) {
                fontByte = 0x00;
            } else {
                fontByte = fontData[fontCol - 1];
            }
            uint8_t pixelBit = (fontByte >> fontRow) & 0x01;
            uint8_t pixelValue = (pixelBit == 1) ? 0x00 : whiteCode;
            pixelByte |= (pixelValue << (6 - p * 2));
        }
        int bytePos = (startX + charIdx * charWidth + col) / 4;
        if (bytePos < pitch) {
            rowBuffer[bytePos] = pixelByte;
        }
    }
}

static void renderChar_1BPP(uint8_t* rowBuffer, const uint8_t* fontData, int fontRow, int charIdx, int startX, int charWidth, int pitch, int fontScale) {
    for (int col = 0; col < charWidth; col += fontScale) {
        uint8_t fontByte;
        int fontCol = col / fontScale;
        if (fontCol == 0 || fontCol > 7) {
            fontByte = 0x00;
        } else {
            fontByte = fontData[fontCol - 1];
        }
        uint8_t pixelBit = (fontByte >> fontRow) & 0x01;
        for (int s = 0; s < fontScale; s++) {
            int pixelX = startX + charIdx * charWidth + col + s;
            if (pixelX >= globalConfig.displays[0].pixel_width) break;
            int bytePos = pixelX / 8;
            int bitPos = 7 - (pixelX % 8);
            if (bytePos < pitch) {
                if (pixelBit == 1) {
                    rowBuffer[bytePos] &= ~(1 << bitPos);
                }
            }
        }
    }
}

void initDisplay(){
    /* Three consecutive watchdog resets say the panel path is what wedges, so a fourth attempt
     * is another reset. Returning before the rail is powered leaves the device up on BLE, which
     * is the only way a bad config or a bad image can be replaced. */
    if (od_watchdog_app_safe_mode()) {
        od_log_warn("Display init skipped: watchdog safe mode");
        return;
    }
    od_log_info("=== Initializing Display ===");
    /* The boot refresh is the longest uninterruptible span in the firmware and the likeliest
     * place to wedge, so it is the one phase worth breadcrumbing even on its own: the boot after
     * a watchdog reset can then say whether the panel or something else stopped the last run. */
    od_watchdog_app_phase(OD_WDT_PHASE_BOOT_REFRESH);
    if(globalConfig.display_count > 0){
#if defined(OPENDISPLAY_FASTEPD)
    if (fastepd_driver_used()) {
        int bitsPerPixel = displayBootBitsPerPixel(globalConfig.displays[0].color_scheme);
        if (bitsPerPixel == 0) {
            od_log_warn("Display: unsupported boot color scheme %u",
                        (unsigned)globalConfig.displays[0].color_scheme);
            return;
        }
        pwrmgm(true);
        od_log_info("Display: FastEPD (panel_ic %u, %ux%u, %d bpp)",
                    globalConfig.displays[0].panel_ic_type,
                    globalConfig.displays[0].pixel_width, globalConfig.displays[0].pixel_height,
                    bitsPerPixel);
        fastepd_epaper_begin();
        if (fastepd_init_failed()) {
            od_log_warn("FastEPD init failed — skipping boot refresh");
            fastepd_mark_hw_deinitialized();
            pwrmgm(false);
            return;
        }
        od_log_info("Height: %u", globalConfig.displays[0].pixel_height);
        od_log_info("Width: %u", globalConfig.displays[0].pixel_width);
        if (! (globalConfig.displays[0].transmission_modes & OD_TRANSMISSION_MODE_CLEAR_ON_BOOT)){
            if (!writeBootScreenWithQr()) {
                od_log_warn("FastEPD boot screen render failed");
                epdSessionForceOff();
                return;
            }
            od_log_info("EPD refresh: FULL (boot, FastEPD)");
            touchSuspendForEpdRefresh();
            fastepd_full_update();
            waitforrefresh(60);
            epdSessionForceOff();
            touchResumeAfterEpdRefresh();
        } else {
            epdSessionForceOff();
        }
    } else
#endif
    {
        prepareEpdRailForBoot();
        memset(&bbep, 0, sizeof(BBEPDISP));
        int panelType = mapEpd(globalConfig.displays[0].panel_ic_type);
        bbepSetPanelType(&bbep, panelType);
        if ((bbep.iFlags & BBEP_SPLIT_BUFFER) != 0u) {
            od_log_error("Split-panel transport is not hardware-qualified on this target");
            pwrmgm(false);
            return;
        }
        int rotation = globalConfig.displays[0].rotation * 90;
        bbepSetRotation(&bbep, rotation);
        od_log_info("Height: %u", globalConfig.displays[0].pixel_height);
        od_log_info("Width: %u", globalConfig.displays[0].pixel_width);
        initBbepPanelSession();
        if (! (globalConfig.displays[0].transmission_modes & OD_TRANSMISSION_MODE_CLEAR_ON_BOOT)){
            bool bootOk = refreshBootScreenFull();
            if (!bootOk && !nrfVbusPresent()) {
                od_log_warn("Boot refresh failed on battery — re-powering panel and retrying");
                touchResumeAfterEpdRefresh();
                pwrmgm(false);
                od_hal_delay_ms(200);
                prepareEpdRailForBoot();
                initBbepPanelSession();
                bootOk = refreshBootScreenFull();
            }
            if (!bootOk) {
                od_log_warn("Boot screen refresh did not complete");
            }
            // Boot ends PWR_OFF (no keep-alive at boot). pwrmgm(true) in boot set
            // PWR_ACTIVE, so ForceOff sleeps the controller + cuts the rail cleanly.
            epdSessionForceOff();
            touchResumeAfterEpdRefresh();
        } else {
            // CLEAR_ON_BOOT: initBbepPanelSession left the controller awake —
            // ForceOff sleeps it before the rail cut (raw pwrmgm(false) skipped that).
            epdSessionForceOff();
        }
    }
    }
    else{
        od_log_warn("No display found");
    }
    od_watchdog_app_phase(OD_WDT_PHASE_IDLE);
}


int displayBootPlane(uint8_t colorScheme) {
    if (colorScheme == OD_COLOR_SCHEME_MONO || colorScheme == OD_COLOR_SCHEME_GRAY16) return PLANE_0;
    if (colorScheme == OD_COLOR_SCHEME_BWR || colorScheme == OD_COLOR_SCHEME_BWY) return PLANE_0;
    return PLANE_1;
}

int displayBootBitsPerPixel(uint8_t colorScheme) {
    if (colorScheme == OD_COLOR_SCHEME_GRAY8) return 0;
#if defined(OPENDISPLAY_FASTEPD)
    if (globalConfig.display_count > 0 &&
        globalConfig.displays[0].panel_ic_type == OD_PANEL_IC_ED103TC2_1872X1404_4GRAY) {
        return 4;
    }
#endif
    if (colorScheme == OD_COLOR_SCHEME_BWGBRY ||
        colorScheme == OD_COLOR_SCHEME_BWGBRY_SPLIT ||
        colorScheme == OD_COLOR_SCHEME_GRAY16) return 4;
    if (colorScheme == OD_COLOR_SCHEME_BWRY || colorScheme == OD_COLOR_SCHEME_GRAY4) return 2;
    if (colorScheme == OD_COLOR_SCHEME_MONO || colorScheme == OD_COLOR_SCHEME_BWR ||
        colorScheme == OD_COLOR_SCHEME_BWY || colorScheme == OD_COLOR_SCHEME_SEVEN_COLOR) return 1;
    return 0;
}

static float readBatteryVoltageUncached() {
    if (od_sensor_bq27220_is_configured(&globalConfig)) {
        float gaugeV = od_sensor_bq27220_voltage_volts();
        if (gaugeV >= 0.0f) {
            return gaugeV;
        }
    }
    if (globalConfig.power_option.battery_sense_pin == 0xFF) return -1.0;
    uint8_t sensePin = globalConfig.power_option.battery_sense_pin;
    uint8_t enablePin = globalConfig.power_option.battery_sense_enable_pin;
    uint16_t scalingFactor = globalConfig.power_option.voltage_scaling_factor;
    od_hal_gpio_config_input(sensePin, false, false);
    if (enablePin != 0xFF) {
        od_hal_gpio_config_output(enablePin, true);
        od_hal_delay_ms(10);
    }
    // CALIBRATED_MV: average millivolts at the pin through the chip's eFuse ADC calibration;
    // voltage_scaling_factor is then the divider ratio x1000. Otherwise raw counts, scaled by
    // a per-board factor that also absorbs that chip's ADC error.
    const bool calibrated =
        (globalConfig.power_option.battery_sense_flags & OD_BATTERY_SENSE_FLAG_CALIBRATED_MV) != 0;
    const int numSamples = 10;
    uint32_t adcSum = 0;
    bool readFailed = false;
    for (int i = 0; i < numSamples; i++) {
        if (calibrated) {
            const int mv = od_hal_adc_read_mv(sensePin);
            if (mv < 0) {
                readFailed = true;
                break;
            }
            adcSum += (uint32_t)mv;
        } else {
            adcSum += od_hal_adc_read(sensePin);
        }
        od_hal_delay_ms(2);
    }
    if (enablePin != 0xFF) {
        od_hal_gpio_write(enablePin, false);
    }
    // A failed calibrated read reports "unknown", not a plausible 0.0 V (see od_hal_adc.h).
    if (readFailed || scalingFactor == 0) return -1.0;
    uint32_t adcAverage = adcSum / numSamples;
    if (calibrated) return (adcAverage * scalingFactor) / (1000000.0);
    return (adcAverage * scalingFactor) / (100000.0);
}

static constexpr uint32_t kBatteryVoltageTtlMs = 30000u;
float readBatteryVoltage() {
    static uint32_t lastReadMs = 0;
    static float cachedVoltage = -1.0f;
    static bool haveReading = false;
    if (haveReading && (uint32_t)(od_hal_uptime_ms() - lastReadMs) < kBatteryVoltageTtlMs) {
        return cachedVoltage;
    }
    cachedVoltage = readBatteryVoltageUncached();
    lastReadMs = od_hal_uptime_ms();
    haveReading = true;
    return cachedVoltage;
}

float readChipTemperature() {
    return od_hal_adc_die_temp_c();
}

void updatemsdata(){
    // od_log_debug("updatemsdata() called (mloopcounter: %u)", mloopcounter);
    od_sensor_sht40_poll(&globalConfig, od_hal_uptime_ms());
    od_sensor_bq27220_poll(&globalConfig, od_hal_uptime_ms());
    float batteryVoltage = readBatteryVoltage();
    float chipTemperature = readChipTemperature();
    // The 16 bytes are encoded by shared/core/od_advert.c, not here. What stays in this file is
    // ACQUISITION -- the sensor polls above and the two conversions below -- because that is the
    // part that is genuinely this target's: readBatteryVoltage() returns volts from an ADC this
    // chip owns, and the negative sentinel is this target's way of saying "no reading".
    //
    // The clamps, the (t + 40) * 2 step encoding, the 10-bit battery split across
    // battery_voltage_low and status bit0, and the company id all left with the encoder. Nordic
    // and Silabs open-coded the same arithmetic; a host that mis-reads a temperature now has one
    // file to look at rather than three that agree only by inspection.
    uint16_t batteryVoltage10mv = 0;
    if (batteryVoltage >= 0.0f) {
        batteryVoltage10mv = od_advert_battery_10mv_from_mv((uint16_t)(batteryVoltage * 1000.0f));
    }
    struct od_advert_inputs adv;
    memset(&adv, 0, sizeof adv);
    adv.dynamic = dynamicreturndata;
    adv.chip_temperature_c = chipTemperature;
    adv.battery_10mv = batteryVoltage10mv;
    adv.reboot_flag = rebootFlag;
    adv.connection_requested = connectionRequested;
    adv.loop_counter = mloopcounter;
    od_advert_build(&adv, msd_payload);
    // Skip the (relatively expensive) advertisement rebuild when nothing changed;
    // the loop counter still advances so successive advertisements stay
    // distinguishable. Both targets used to keep their own copy of this check
    // inside their own #ifdef -- it is transport-independent, so it lives once
    // here and only the push below is platform-specific.
    static uint8_t prev_msd_payload[16] = {0xFF};
    if (memcmp(prev_msd_payload, msd_payload, 16) == 0) {
        mloopcounter = od_advert_advance_counter(mloopcounter);
        return;
    }
    memcpy(prev_msd_payload, msd_payload, 16);
    // The only record of what actually reaches the air. Without it, "the button
    // event is logged but the host never sees it" cannot be split into a firmware
    // publish failure vs a host-side one without a BLE sniffer.
    {
        char line[96];
        od_log_hex_line(line, sizeof(line), "MSD publish: ", msd_payload, 16);
        od_log_debug("%s", line);
    }
    ble.setManufacturerData(msd_payload, 16);
#ifdef OPENDISPLAY_HAS_WIFI
    opendisplay_mdns_update_msd_txt();
#endif
    mloopcounter = od_advert_advance_counter(mloopcounter);
}

static bool directWriteTouchSuspended = false;

/* Resolve the one target-owned direct format exception. ED103TC2 4-gray uses a FastEPD
 * 4-bpp framebuffer regardless of its legacy scheme metadata. Reserved Gray8 is explicitly
 * excluded: this adapter does not define an eight-gray wire format. */
static od_color_status_t directWriteResolveGeometry(od_color_geometry_t* geometry) {
    const struct DisplayConfig& d = globalConfig.displays[0];
    if (d.color_scheme == OD_COLOR_SCHEME_GRAY8) {
        memset(geometry, 0, sizeof(*geometry));
        return OD_COLOR_UNSUPPORTED;
    }
#if defined(OPENDISPLAY_FASTEPD)
    if (fastepd_driver_used() &&
        d.panel_ic_type == OD_PANEL_IC_ED103TC2_1872X1404_4GRAY) {
        memset(geometry, 0, sizeof(*geometry));
        if (d.pixel_width == 0u || d.pixel_height == 0u) return OD_COLOR_BAD_GEOMETRY;
        const uint64_t rowBytes = ((uint64_t)d.pixel_width + 1u) / 2u;
        const uint64_t totalBytes = rowBytes * d.pixel_height;
        if (totalBytes > UINT32_MAX) return OD_COLOR_OVERFLOW;
        geometry->layout = OD_COLOR_LAYOUT_PACKED_ROWS;
        geometry->bits_per_pixel = 4u;
        geometry->part_count = 1u;
        geometry->initial_plane = OD_COLOR_PLANE_0;
        geometry->part_width[0] = d.pixel_width;
        geometry->row_bytes[0] = (uint32_t)rowBytes;
        geometry->part_bytes[0] = (uint32_t)totalBytes;
        geometry->total_bytes = (uint32_t)totalBytes;
        return OD_COLOR_OK;
    }
#endif
    return od_color_direct_geometry(d.color_scheme, d.pixel_width, d.pixel_height, geometry);
}

static void xferAppClear(bool forceOff) {
    if (xferApp.mode != XFER_APP_IDLE && pwrmgmState == PWR_ACTIVE) {
        if (forceOff) epdSessionForceOff();
        else          epdSessionRelease(true);
    }
    if (directWriteTouchSuspended) {
        touchResumeAfterEpdRefresh();
        directWriteTouchSuspended = false;
    }
    memset(&xferApp, 0, sizeof(xferApp));
}

static bool xferAppWriteFull(uint32_t streamOffset, od_span_t data) {
    if (streamOffset > xferApp.geometry.total_bytes ||
        data.n > xferApp.geometry.total_bytes - streamOffset) {
        return false;
    }
#if defined(OPENDISPLAY_FASTEPD)
    if (fastepd_driver_used()) {
        fastepd_direct_write_chunk((uint8_t *)(uintptr_t)data.p, (uint32_t)data.n);
        return true;
    }
#endif
    if (xferApp.geometry.layout != OD_COLOR_LAYOUT_CONTROLLER_PLANES) {
        bbepWriteData(&bbep, (uint8_t *)(uintptr_t)data.p, (int)data.n);
        return true;
    }

    uint32_t consumed = 0u;
    while (consumed < data.n) {
        const uint32_t logical = streamOffset + consumed;
        const uint8_t plane = logical < xferApp.plane_bytes ? PLANE_0 : PLANE_1;
        if (xferApp.current_plane != plane) {
            if (logical != 0u && logical != xferApp.plane_bytes) return false;
            bbepSetAddrWindow(&bbep, 0, 0, xferApp.width, xferApp.height);
            bbepStartWrite(&bbep, plane);
            xferApp.current_plane = plane;
        }
        const uint32_t planeEnd = plane == PLANE_0
            ? xferApp.plane_bytes : xferApp.geometry.total_bytes;
        uint32_t chunk = planeEnd - logical;
        if (chunk > data.n - consumed) chunk = (uint32_t)data.n - consumed;
        bbepWriteData(&bbep, (uint8_t *)(uintptr_t)(data.p + consumed), (int)chunk);
        consumed += chunk;
    }
    return true;
}

static bool xferAppWritePartial(uint32_t streamOffset, od_span_t data) {
    const uint32_t total = xferApp.plane_bytes * 2u;
    if (streamOffset > total || data.n > total - streamOffset) return false;
#if defined(OPENDISPLAY_FASTEPD)
    if (fastepd_driver_used()) {
        return fastepd_partial_write_chunk((uint8_t *)(uintptr_t)data.p, (uint32_t)data.n);
    }
#endif

    uint32_t consumed = 0u;
    while (consumed < data.n) {
        const uint32_t logical = streamOffset + consumed;
        const uint8_t plane = logical < xferApp.plane_bytes ? PLANE_1 : PLANE_0;
        if (xferApp.current_plane != plane) {
            if (logical != 0u && logical != xferApp.plane_bytes) return false;
            partial_set_addr_window(&bbep, xferApp.x, xferApp.y,
                                    xferApp.width, xferApp.height);
            bbepStartWrite(&bbep, plane);
            xferApp.current_plane = plane;
        }
        const uint32_t planeEnd = plane == PLANE_1 ? xferApp.plane_bytes : total;
        uint32_t chunk = planeEnd - logical;
        if (chunk > data.n - consumed) chunk = (uint32_t)data.n - consumed;
        bbepWriteData(&bbep, (uint8_t *)(uintptr_t)(data.p + consumed), (int)chunk);
        consumed += chunk;
    }
    return true;
}

extern "C" void od_xfer_app_prepare_start(void) {
    if (xferApp.mode != XFER_APP_IDLE) xferAppClear(false);
}

extern "C" bool od_xfer_app_panel_info(od_xfer_panel_info_t *out) {
    if (out == nullptr) return false;
    memset(out, 0, sizeof(*out));
    if (directWriteResolveGeometry(&out->geometry) != OD_COLOR_OK) return false;
    out->width = globalConfig.displays[0].pixel_width;
    out->height = globalConfig.displays[0].pixel_height;
    out->partial_enabled = true;
    xferApp.width = out->width;
    xferApp.height = out->height;
    return true;
}

extern "C" bool od_xfer_app_begin_full(const od_color_geometry_t *geometry) {
    if (geometry == nullptr || geometry->total_bytes == 0u) return false;
    xferApp.mode = XFER_APP_FULL;
    xferApp.geometry = *geometry;
    xferApp.plane_bytes = geometry->layout == OD_COLOR_LAYOUT_CONTROLLER_PLANES
        ? geometry->part_bytes[0] : 0u;
    xferApp.current_plane = 0xFFu;
    touchSuspendForEpdRefresh();
    directWriteTouchSuspended = true;
#if defined(OPENDISPLAY_FASTEPD)
    if (fastepd_driver_used()) {
        fastepd_prepare_hardware();
    }
#endif
    epdSessionAcquire(false);
    epdPlanesPrepared = false;
#if defined(OPENDISPLAY_FASTEPD)
    if (fastepd_driver_used()) {
        fastepd_direct_write_reset();
    } else
#endif
    {
        bbepSetAddrWindow(&bbep, 0, 0, xferApp.width, xferApp.height);
        const uint8_t plane = geometry->initial_plane == OD_COLOR_PLANE_0 ? PLANE_0 : PLANE_1;
        bbepStartWrite(&bbep, plane);
        if (geometry->layout == OD_COLOR_LAYOUT_CONTROLLER_PLANES) {
            xferApp.current_plane = plane;
        }
    }
    return true;
}

extern "C" bool od_xfer_app_begin_partial(uint16_t x, uint16_t y, uint16_t width,
                                            uint16_t height, uint32_t planeBytes) {
    if (planeBytes == 0u) return false;
    xferApp.mode = XFER_APP_PARTIAL;
    xferApp.x = x;
    xferApp.y = y;
    xferApp.width = width;
    xferApp.height = height;
    xferApp.plane_bytes = planeBytes;
    xferApp.current_plane = 0xFFu;
    partial_prepare_panel_ram_for(x, y, width, height);
    return true;
}

extern "C" uint32_t od_xfer_app_write(uint32_t streamOffset, od_span_t data) {
    if (!od_span_valid(data) || data.n == 0u || data.n > UINT32_MAX) return 0u;
    const bool accepted = xferApp.mode == XFER_APP_FULL
        ? xferAppWriteFull(streamOffset, data)
        : xferApp.mode == XFER_APP_PARTIAL && xferAppWritePartial(streamOffset, data);
    if (!accepted) return 0u;
    return (uint32_t)data.n;
}

extern "C" od_mut_span_t od_xfer_app_inflate_scratch(void) {
    return od_mut_span_make(decompressionChunk, OPENDISPLAY_DECOMPRESSION_CHUNK_SIZE);
}

extern "C" void od_xfer_app_abort(od_xfer_abort_reason_t reason) {
    const bool forceOff = reason == OD_XFER_ABORT_STREAM_FAILED ||
                          reason == OD_XFER_ABORT_PIPE_INCOMPLETE ||
                          reason == OD_XFER_ABORT_REPLY_FAILED ||
                          reason == OD_XFER_ABORT_REFRESH_FAILED ||
                          reason == OD_XFER_ABORT_RESET;
    xferAppClear(forceOff);
}

extern "C" od_xfer_barrier_t od_xfer_app_before_refresh(const od_reply_t *owner) {
    (void)owner;
    od_cmd_flush_before_refresh();
    if (xferApp.mode == XFER_APP_FULL) od_hal_delay_ms(20);
    return OD_XFER_BARRIER_PROCEED;
}

extern "C" void od_xfer_app_barrier_abort(const od_reply_t *owner) {
    (void)owner;
    xferAppClear(true);
}

extern "C" bool od_xfer_app_refresh(uint8_t mode, bool *completed) {
    if (completed == nullptr || xferApp.mode == XFER_APP_IDLE) return false;
    bool refreshSuccess = false;
    if (xferApp.mode == XFER_APP_PARTIAL) {
        refreshSuccess = partial_refresh_for(xferApp.x, xferApp.y, xferApp.width,
                                             xferApp.height, mode);
        memset(&xferApp, 0, sizeof(xferApp));
    } else {
        const char *modeName = mode == REFRESH_FAST ? "FAST" : "FULL";
        od_log_info("EPD refresh: %s (mode=%u)", modeName, (unsigned)mode);
        epdRefreshInProgress = true;
#if defined(OPENDISPLAY_FASTEPD)
        if (fastepd_driver_used()) {
            fastepd_direct_refresh(mode);
            refreshSuccess = waitforrefresh(60);
        } else
#endif
        {
            bbepRefresh(&bbep, mode);
            refreshSuccess = waitforrefresh(60);
        }
        endRefresh();
        xferAppClear(false);
        requestAdvertisingRestart();
    }
    *completed = refreshSuccess;
    return true;
}

extern "C" uint32_t od_xfer_app_displayed_etag(void) {
    return displayed_etag;
}

extern "C" void od_xfer_app_set_displayed_etag(uint32_t etag) {
    displayed_etag = etag;
}

extern "C" uint32_t od_xfer_app_now_ms(void) {
    return od_hal_uptime_ms();
}

od_origin_t transferSessionOrigin(void) {
    od_reply_t owner;
    return od_xfer_owner(&owner) ? owner.origin : OD_ORIGIN_BLE;
}

bool transferActive(void) {
    return od_xfer_owns_hardware();
}

static bool panel_skips_bbep_set_addr_window(void) {
    return bbep.type == EP397_800x480 || bbep.type == EP397_800x480_4GRAY ||
           bbep.type == EP426_800x480 || bbep.type == EP426_800x480_4GRAY;
}

static bool panel_uses_pixel_ram_x(BBEPDISP *pBBEP) {
    return pBBEP->type == EP397_800x480 || pBBEP->type == EP397_800x480_4GRAY ||
           pBBEP->type == EP426_800x480 || pBBEP->type == EP426_800x480_4GRAY;
}

static bool panel_uses_ep397_y_decrement(BBEPDISP *pBBEP) {
    return pBBEP->type == EP397_800x480 || pBBEP->type == EP397_800x480_4GRAY;
}

static bool panel_uses_ep426_x_decrement(BBEPDISP *pBBEP) {
    return pBBEP->type == EP426_800x480 || pBBEP->type == EP426_800x480_4GRAY;
}

static bool panel_skips_reinit_on_partial_refresh(BBEPDISP *pBBEP) {
    return panel_uses_ep397_y_decrement(pBBEP) || panel_uses_ep426_x_decrement(pBBEP);
}

static void partial_set_ep397_ram_y(BBEPDISP *pBBEP, int ty, int cy) {
    uint8_t uc[4];
    int yLast = ty + cy - 1;
    int ramYStart = (pBBEP->native_height - 1) - ty;
    int ramYEnd = (pBBEP->native_height - 1) - yLast;

    bbepWriteCmd(pBBEP, SSD1608_SET_RAMYPOS);
    uc[0] = (uint8_t)(ramYStart & 0xff);
    uc[1] = (uint8_t)(ramYStart >> 8);
    uc[2] = (uint8_t)(ramYEnd & 0xff);
    uc[3] = (uint8_t)(ramYEnd >> 8);
    bbepWriteData(pBBEP, uc, 4);

    bbepWriteCmd(pBBEP, SSD1608_SET_RAMYCOUNT);
    uc[0] = (uint8_t)(ramYStart & 0xff);
    uc[1] = (uint8_t)(ramYStart >> 8);
    bbepWriteData(pBBEP, uc, 2);
}

static void partial_set_ep426_ram_y(BBEPDISP *pBBEP, int ty, int cy) {
    uint8_t uc[4];
    int yLast = ty + cy - 1;

    // Match epd426_init_* 0x45 wire order: Y start in bytes 0-1, Y end in bytes 2-3.
    bbepWriteCmd(pBBEP, SSD1608_SET_RAMYPOS);
    uc[0] = (uint8_t)ty;
    uc[1] = (uint8_t)(ty >> 8);
    uc[2] = (uint8_t)yLast;
    uc[3] = (uint8_t)(yLast >> 8);
    bbepWriteData(pBBEP, uc, 4);

    bbepWriteCmd(pBBEP, SSD1608_SET_RAMYCOUNT);
    uc[0] = (uint8_t)ty;
    uc[1] = (uint8_t)(ty >> 8);
    bbepWriteData(pBBEP, uc, 2);
}

static void partial_set_pixel_ram_x(BBEPDISP *pBBEP, int x, int cx) {
    uint8_t uc[4];
    int px0 = x;
    int px1 = x + cx - 1;
    if (panel_uses_ep426_x_decrement(pBBEP)) {
        px0 = (pBBEP->native_width - 1) - x;
        px1 = (pBBEP->native_width - 1) - (x + cx - 1);
    }

    bbepWriteCmd(pBBEP, SSD1608_SET_RAMXPOS);
    uc[0] = (uint8_t)(px0 & 0xff);
    uc[1] = (uint8_t)((px0 >> 8) & 0xff);
    uc[2] = (uint8_t)(px1 & 0xff);
    uc[3] = (uint8_t)(px1 >> 8);
    bbepWriteData(pBBEP, uc, 4);

    bbepWriteCmd(pBBEP, SSD1608_SET_RAMXCOUNT);
    uc[0] = (uint8_t)(px0 & 0xff);
    uc[1] = (uint8_t)(px0 >> 8);
    bbepWriteData(pBBEP, uc, 2);
}

static void partial_set_addr_window(BBEPDISP *pBBEP, int x, int y, int cx, int cy) {
    if (!panel_skips_bbep_set_addr_window()) {
        bbepSetAddrWindow(pBBEP, x, y, cx, cy);
        return;
    }
    if (!pBBEP) return;

    uint8_t uc[4];
    int ty = y;
    cx = (cx + 7) & 0xfff8;

    if (panel_uses_pixel_ram_x(pBBEP)) {
        partial_set_pixel_ram_x(pBBEP, x, cx);
    } else {
        int tx = x / 8;
        bbepWriteCmd(pBBEP, SSD1608_SET_RAMXPOS);
        uc[0] = (uint8_t)tx;
        uc[1] = (uint8_t)(tx + ((cx - 1) >> 3));
        bbepWriteData(pBBEP, uc, 2);
        bbepCMD2(pBBEP, SSD1608_SET_RAMXCOUNT, (uint8_t)tx);
    }

    if (panel_uses_ep426_x_decrement(pBBEP)) {
        partial_set_ep426_ram_y(pBBEP, ty, cy);
    } else if (panel_uses_ep397_y_decrement(pBBEP)) {
        partial_set_ep397_ram_y(pBBEP, ty, cy);
    } else {
        bbepWriteCmd(pBBEP, SSD1608_SET_RAMYPOS);
        uc[0] = (uint8_t)ty;
        uc[1] = (uint8_t)(ty >> 8);
        uc[2] = (uint8_t)(ty + cy - 1);
        uc[3] = (uint8_t)((ty + cy - 1) >> 8);
        bbepWriteData(pBBEP, uc, 4);
        uc[0] = (uint8_t)ty;
        uc[1] = (uint8_t)(ty >> 8);
        bbepWriteCmd(pBBEP, SSD1608_SET_RAMYCOUNT);
        bbepWriteData(pBBEP, uc, 2);
    }
    bbepWaitBusy(pBBEP);
}

static bool partial_trigger_refresh(int refreshMode) {
    if (refreshMode < 0 || refreshMode > 3) refreshMode = REFRESH_PARTIAL;
    if (panel_skips_reinit_on_partial_refresh(&bbep)) {
        if (panel_uses_ep397_y_decrement(&bbep)) {
            static const uint8_t u8CMDz3[4] = {0xf7, 0xd7, 0xff, 0};
            bbepCMD2(&bbep, SSD1608_DISP_CTRL2, u8CMDz3[refreshMode]);
        } else {
            static const uint8_t u8CMD[4] = {0xf7, 0xc7, 0xff, 0xc0};
            bbepCMD2(&bbep, SSD1608_DISP_CTRL2, u8CMD[refreshMode]);
        }
        bbepWriteCmd(&bbep, SSD1608_MASTER_ACTIVATE);
        return waitforrefresh(60);
    }
    bbepRefresh(&bbep, refreshMode);
    return waitforrefresh(60);
}

static void partial_prepare_panel_ram_for(uint16_t x, uint16_t y, uint16_t width,
                                          uint16_t height) {
    // Delta in ms since function entry, to profile where prep wall-clock goes.
    uint32_t t0 = od_hal_uptime_ms();
    od_log_debug("[+%ums] EPD partial start: acquire panel session", (unsigned)(od_hal_uptime_ms() - t0));
    // Acquire subsumes pwrmgm(true) + bbepInitIO + bbepWakeUp + init-seq resend.
    // Warm re-acquire skips the ~900 ms rail bring-up + bbepInitIO (Phase 1).
    bool cold = epdSessionAcquire(true);
    od_log_debug("[+%ums] after epdSessionAcquire (%s)", (unsigned)(od_hal_uptime_ms() - t0), cold ? "cold" : "warm");
#if defined(OPENDISPLAY_FASTEPD)
    if (fastepd_driver_used()) {
        fastepd_partial_prepare(x, y, width, height);
        od_log_debug("[+%ums] FastEPD partial prepare done", (unsigned)(od_hal_uptime_ms() - t0));
        return;
    }
#endif
    // The two white fills guarantee PLANE_0 == PLANE_1 OUTSIDE the rect so uninit
    // controller RAM can't flash noise during MASTER_ACTIVATE. A full-frame rect's
    // enforced plane_size*2 stream overwrites 100% of both planes, so there is no
    // "outside the rect" to protect — provably safe to skip even on a cold panel
    // (Phase 1 skip condition 1). Sub-rects still fill.
    bool fullFrame = x == 0 && y == 0 &&
                     width == globalConfig.displays[0].pixel_width &&
                     height == globalConfig.displays[0].pixel_height;
    if (!fullFrame) {
        bbepFill(&bbep, BBEP_WHITE, PLANE_1);
        bbepFill(&bbep, BBEP_WHITE, PLANE_0);
        od_log_debug("[+%ums] after fills (ran: sub-rect)", (unsigned)(od_hal_uptime_ms() - t0));
    } else {
        od_log_debug("[+%ums] fills skipped (full-frame rect)", (unsigned)(od_hal_uptime_ms() - t0));
    }
}

static bool partial_refresh_for(uint16_t x, uint16_t y, uint16_t width, uint16_t height,
                                int refreshMode) {
    od_log_info("EPD refresh: PARTIAL (raw rect %u,%u %ux%u)",
                x, y, width, height);
    epdRefreshInProgress = true;
    bool refreshSuccess = false;
#if defined(OPENDISPLAY_FASTEPD)
    if (fastepd_driver_used()) {
        refreshSuccess = fastepd_partial_refresh(refreshMode);
    } else
#endif
    {
        refreshSuccess = partial_trigger_refresh(refreshMode);
    }
    endRefresh();
    // A successful partial refresh leaves both controller planes consistent.
    if (refreshSuccess) epdPlanesPrepared = true;
    // Release keeps the panel warm (rail/SPI up, controller awake) on success;
    // powers it fully down on failure or on AXP2101 (window 0) boards.
    epdSessionRelease(refreshSuccess);
    return refreshSuccess;
}

// See display_service.h for why this exists rather than main.cpp calling SPI.end() itself.
//
// The REAL implementation is in display_fastepd.cpp, which is the file that legitimately owns
// the FastEPD vendor adapter. This is the fallback for boards that compile no FastEPD at all,
// and it is a NO-OP BY PROOF rather than by assumption: the adapter's bus is only ever brought
// up by SPI.beginTransaction(), whose only callers are FastEPD.inl and display_fastepd.cpp --
// both compiled out here -- so compat SPI's _bus_ok is always false and the SPI.end() this
// replaces could never have freed anything on such a board.
//
// display_service.cpp has no FastEPD/vendor adapter dependency at all, which is why <SPI.h> is
// gone from it. The bb_epaper bus is released by bbepDeInitIO(), called from the panel
// force-off path above.
#if !defined(OPENDISPLAY_FASTEPD)
void displayReleaseSpiBus(void) { }
#endif
