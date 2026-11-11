#include "display_service.h"
#include "od_cmd_reply.h"

#include <stdio.h>
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
#include "sensor_axp2101.h"
#include "sensor_aht20.h"
#include "communication.h"
#include "boot_screen.h"
#include "link_owner.h"
#include "session_guard.h"
#include "od_session.h"
#include "od_session_app.h"
#include "touch_input.h"
#include "od_zlib_pump.h"
#include "od_xfer.h"
#include "od_xfer_app.h"
#include "od_display.h"
#include "od_controller_esp.h"
#include "od_display_esp.h"
#include "od_epd_esp.h"
#if defined(OPENDISPLAY_FASTEPD)
#include "display_fastepd.h"
#endif

#include "wifi_service.h"

#include "ble_transport.h"
#include "od_rxq.h"

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
void flashLed(uint8_t color, uint8_t brightness);

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
#define AXP2101_REG_ADC_DATA_VBUS_VOL_H 0x38
#define AXP2101_REG_ADC_DATA_SYS_VOL_H 0x3A
#define AXP2101_REG_BAT_PERCENT_DATA 0xA4
#define AXP2101_REG_PWRON_STATUS 0x20
#define AXP2101_REG_IRQ_ENABLE1 0x40
#define AXP2101_REG_IRQ_ENABLE2 0x41
#define AXP2101_REG_IRQ_ENABLE3 0x42
#define AXP2101_REG_IRQ_STATUS1 0x48
#define AXP2101_REG_IRQ_STATUS2 0x49
#define AXP2101_REG_IRQ_STATUS3 0x4A
#define AXP2101_REG_LDO_ONOFF_CTRL1 0x91

extern uint8_t staticRowBuffer[BOOT_ROW_BUFFER_SIZE];


/* IT8951 and parallel panels are on FastEPD's built-in drivers; od_controller_esp says which
 * (from 0xB3 only). Every other panel is driven from drive packets. */
bool displayConfigUsesNativeDriver(const struct DisplayConfig *d) {
#if !defined(OPENDISPLAY_FASTEPD)
    (void)d;
    return false;
#else
    if (d == nullptr) return false;
    if (globalConfig.display_count > 0 && d == &globalConfig.displays[0]) {
        return od_controller_esp()->cls != OD_CTRL_CLASS_COMMAND;
    }
    struct od_controller_desc desc;
    od_controller_esp_describe(d, &desc);
    return desc.cls != OD_CTRL_CLASS_COMMAND;
#endif
}

bool displayUsesNativeDriver(void) {
    return globalConfig.display_count > 0 && displayConfigUsesNativeDriver(&globalConfig.displays[0]);
}

bool fastepd_driver_used(void) {
    return displayUsesNativeDriver();
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
                    od_hal_gpio_config_output(led->led_1_r, invertRed);
                }
                if (led->led_2_g != 0xFF) {
                    od_hal_gpio_config_output(led->led_2_g, invertGreen);
                }
                if (led->led_3_b != 0xFF) {
                    od_hal_gpio_config_output(led->led_3_b, invertBlue);
                }
                if (led->led_4 != 0xFF) {
                    od_hal_gpio_config_output(led->led_4, invertLed4);
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
        else if(sensor->sensor_type == OD_SENSOR_TYPE_PCF8563){
            od_log_debug("  PCF8563/BM8563 RTC (power-off wake source with pwr_latch)");
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
    // 0x3F, not 0xFF: bits 5:0 are the channel enables, bits 7:6 are
    // reserved (R0) and must be written as zero.
    const uint8_t od_i2c_tx15[2] = { (uint8_t)(AXP2101_REG_ADC_CHANNEL_CTRL), (uint8_t)(0x3F) };
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
            bool batteryPresent = (status1 & 0x08) != 0;
            bool charging = ((status2 >> 5) & 0x03) == 0x01;
            bool vbusPresent = (status1 & 0x20) != 0;
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
            uint16_t batVolRaw = ((uint16_t)(batVolH & 0x3F) << 8) | batVolL;
            float batVoltage = batVolRaw;  // 1 mV per count
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
            uint16_t vbusVolRaw = ((uint16_t)(vbusVolH & 0x3F) << 8) | vbusVolL;
            float vbusVoltage = vbusVolRaw;  // 1 mV per count
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
            uint16_t sysVolRaw = ((uint16_t)(sysVolH & 0x3F) << 8) | sysVolL;
            float sysVoltage = sysVolRaw;  // 1 mV per count
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
        const uint8_t od_i2c_tx29[2] = { (uint8_t)(AXP2101_REG_IRQ_STATUS1), (uint8_t)(0xFF) };
        error = od_hal_i2c_write(busId, AXP2101_SLAVE_ADDRESS, od_i2c_tx29, 2);
    }
    if(error == 0){
        const uint8_t od_i2c_tx30[2] = { (uint8_t)(AXP2101_REG_IRQ_STATUS2), (uint8_t)(0xFF) };
        error = od_hal_i2c_write(busId, AXP2101_SLAVE_ADDRESS, od_i2c_tx30, 2);
    }
    if(error == 0){
        // 0xDF, not 0xFF: bit 5 of the third IRQ-status register is reserved.
        const uint8_t od_i2c_tx31[2] = { (uint8_t)(AXP2101_REG_IRQ_STATUS3), (uint8_t)(0xDF) };
        error = od_hal_i2c_write(busId, AXP2101_SLAVE_ADDRESS, od_i2c_tx31, 2);
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
        od_log_debug("DLDO2 disabled");
    } else {
        od_log_error("Failed to disable DLDO2");
    }
    const uint8_t od_i2c_tx37[2] = { (uint8_t)(AXP2101_REG_LDO_ONOFF_CTRL0), (uint8_t)(0x00) };
    error = od_hal_i2c_write(busId, AXP2101_SLAVE_ADDRESS, od_i2c_tx37, 2);
    if(error == 0){
        od_log_debug("ALDO1-4, BLDO1-2, CPUSLDO, DLDO1 disabled");
    } else {
        od_log_error("Failed to disable ALDO/BLDO/CPUSLDO/DLDO1 rails");
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
    if (globalConfig.display_count == 0) {
        od_log_warn("No display found");
        od_watchdog_app_phase(OD_WDT_PHASE_IDLE);
        return;
    }
    const DisplayConfig& d = globalConfig.displays[0];
    od_log_info("Display: panel_ic %u, %ux%u", (unsigned)d.panel_ic_type,
                (unsigned)d.pixel_width, (unsigned)d.pixel_height);
#if defined(OPENDISPLAY_FASTEPD)
    if (displayUsesNativeDriver()) {
        int bitsPerPixel = displayBootBitsPerPixel(d.color_scheme);
        if (bitsPerPixel == 0) {
            od_log_warn("Display: unsupported boot color scheme %u", (unsigned)d.color_scheme);
            od_watchdog_app_phase(OD_WDT_PHASE_IDLE);
            return;
        }
        displayRailAcquire();
        od_log_info("Display: built-in driver, %d bpp", bitsPerPixel);
        fastepd_epaper_begin();
        if (fastepd_init_failed()) {
            od_log_warn("FastEPD init failed — skipping boot refresh");
            fastepd_mark_hw_deinitialized();
            pwrmgm(false);
            od_watchdog_app_phase(OD_WDT_PHASE_IDLE);
            return;
        }
        if (!(d.transmission_modes & OD_TRANSMISSION_MODE_CLEAR_ON_BOOT)) {
            if (!writeBootScreenWithQr()) {
                od_log_warn("FastEPD boot screen render failed");
                epdSessionForceOff();
                od_watchdog_app_phase(OD_WDT_PHASE_IDLE);
                return;
            }
            od_log_info("EPD refresh: FULL (boot, FastEPD)");
            touchSuspendForEpdRefresh();
            fastepd_full_update();
            fastepd_wait_refresh(60);
            epdSessionForceOff();
            touchResumeAfterEpdRefresh();
        } else {
            epdSessionForceOff();
        }
        od_watchdog_app_phase(OD_WDT_PHASE_IDLE);
        return;
    }
#endif
    if (!displayBootConfigDriven()) {
        od_log_warn("Boot screen refresh did not complete");
    }
    od_watchdog_app_phase(OD_WDT_PHASE_IDLE);
}

/* Bits per pixel the boot renderer draws for the built-in drivers' framebuffers. */
int displayBootBitsPerPixel(uint8_t colorScheme) {
    if (colorScheme == OD_COLOR_SCHEME_GRAY8) return 0;
#if defined(OPENDISPLAY_FASTEPD)
    if (displayUsesNativeDriver() && (od_controller_esp()->flags & OD_CTRL_FLAG_GRAY16) != 0u) {
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
    const float pmicV = axp2101BatteryVoltageVolts(globalConfig.sensors, globalConfig.sensor_count);
    if (pmicV >= 0.0f) {
        return pmicV;
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
    const int numSamples = 10;
    uint32_t adcSum = 0;
    for (int i = 0; i < numSamples; i++) {
        adcSum += od_hal_adc_read(sensePin);
        od_hal_delay_ms(2);
    }
    uint32_t adcAverage = adcSum / numSamples;
    if (enablePin != 0xFF) {
        od_hal_gpio_write(enablePin, false);
    }
    if (scalingFactor > 0) return (adcAverage * scalingFactor) / (100000.0);
    return -1.0;
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
    pollAht20SensorsForMsd();
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
    // No display configured: the dynamic area is all 0xFF, which hosts read as "unprovisioned".
    static const uint8_t kDynamicUnconfigured[11] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
                                                     0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    adv.dynamic = (globalConfig.display_count == 0) ? kDynamicUnconfigured : dynamicreturndata;
    adv.chip_temperature_c = chipTemperature;
    adv.battery_10mv = batteryVoltage10mv;
    adv.reboot_flag = rebootFlag;
    adv.connection_requested = connectionRequested;
    adv.loop_counter = mloopcounter;
    od_advert_build(&adv, msd_payload);
    // Status bit 3 = application-layer encryption active (enabled and a non-zero key), as the
    // Firmware 2.x fleet advertises it. od_advert_build() has no input for it, so it is set here.
    if (od_session_security_enabled(od_session_app_security())) {
        msd_payload[15] |= OD_MSD_STATUS_ENCRYPTION_ENABLED;
    }
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

/* --- od_xfer_app ------------------------------------------------------------------------------
 *
 * SPI panels go through od_display, driven entirely by the config's drive packets. The panels on
 * a built-in controller driver (IT8951, FastEPD parallel) keep their FastEPD paths below. */

static bool directWriteTouchSuspended = false;

static void touchSuspendForUpload(void) {
    if (!directWriteTouchSuspended) {
        touchSuspendForEpdRefresh();
        directWriteTouchSuspended = true;
    }
}

static void touchResumeAfterUpload(void) {
    if (directWriteTouchSuspended) {
        touchResumeAfterEpdRefresh();
        directWriteTouchSuspended = false;
    }
}

#if defined(OPENDISPLAY_FASTEPD)
enum NativeXferMode : uint8_t {
    NATIVE_XFER_IDLE = 0,
    NATIVE_XFER_FULL,
    NATIVE_XFER_PARTIAL,
};

struct NativeXferState {
    NativeXferMode mode;
    uint32_t total_bytes;
    uint16_t x;
    uint16_t y;
    uint16_t width;
    uint16_t height;
};

static NativeXferState nativeXfer = {};

/* A GRAY16-flagged controller takes a FastEPD 4-bpp framebuffer whatever the scheme says. */
static od_color_status_t nativeGeometry(const struct DisplayConfig& d, od_color_geometry_t* geometry) {
    if ((od_controller_esp()->flags & OD_CTRL_FLAG_GRAY16) != 0u) {
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
    return od_color_direct_geometry(d.color_scheme, d.pixel_width, d.pixel_height, geometry);
}
#endif

void displayXferClear(bool forceOff) {
    bool nativeActive = false;
#if defined(OPENDISPLAY_FASTEPD)
    nativeActive = nativeXfer.mode != NATIVE_XFER_IDLE;
    memset(&nativeXfer, 0, sizeof(nativeXfer));
#endif
    if (od_display_active()) {
        od_display_abort();
    }
    touchResumeAfterUpload();
    if (forceOff) {
        epdSessionForceOff();
    } else if (nativeActive && pwrmgmState == PWR_ACTIVE) {
        displayRailRelease(true);
    }
}

extern "C" void od_xfer_app_prepare_start(void) {
    bool busy = od_display_active();
#if defined(OPENDISPLAY_FASTEPD)
    busy = busy || nativeXfer.mode != NATIVE_XFER_IDLE;
#endif
    if (busy) displayXferClear(false);
}

extern "C" bool od_xfer_app_panel_info(od_xfer_panel_info_t *out) {
    if (out == nullptr || globalConfig.display_count == 0) return false;
    const struct DisplayConfig& d = globalConfig.displays[0];
    memset(out, 0, sizeof(*out));
    out->width = d.pixel_width;
    out->height = d.pixel_height;
#if defined(OPENDISPLAY_FASTEPD)
    if (displayUsesNativeDriver()) {
        if (d.color_scheme == OD_COLOR_SCHEME_GRAY8) return false;
        if (nativeGeometry(d, &out->geometry) != OD_COLOR_OK) return false;
        out->partial_enabled = true;
        return true;
    }
#endif
    out->needs_migration = od_display_needs_migration();
    const bool geometryOk = od_color_direct_geometry(d.color_scheme, d.pixel_width,
                                                     d.pixel_height, &out->geometry) == OD_COLOR_OK;
    if (!geometryOk && !out->needs_migration) return false;
    /* Partial needs a "partial" mode to drive it, whatever 0x20 claims. */
    out->partial_enabled = !out->needs_migration && od_display_partial_available();
    /* The drive stream walks a split frame by the sequence's IMAGE_SLICE markers, one CS each. */
    out->split_halves_ok = true;
    return true;
}

static bool displayStarted(od_display_rc_t rc, const char *what) {
    if (rc != OD_DISPLAY_OK) {
        if (rc == OD_DISPLAY_PANEL_BUSY) {
            od_xfer_app_note_start_error(OD_ERR_PANEL_BUSY);
        }
        od_log_error("display: %s refused rc=%d", what, (int)rc);
        touchResumeAfterUpload();
        return false;
    }
    return true;
}

extern "C" bool od_xfer_app_begin_full(const od_color_geometry_t *geometry) {
    if (geometry == nullptr || geometry->total_bytes == 0u) return false;
    touchSuspendForUpload();
#if defined(OPENDISPLAY_FASTEPD)
    if (displayUsesNativeDriver()) {
        nativeXfer.mode = NATIVE_XFER_FULL;
        nativeXfer.total_bytes = geometry->total_bytes;
        fastepd_prepare_hardware();
        displayRailAcquire();
        fastepd_direct_write_reset();
        return true;
    }
#endif
    return displayStarted(od_display_begin_full(geometry), "full upload");
}

extern "C" bool od_xfer_app_begin_partial(uint16_t x, uint16_t y, uint16_t width,
                                            uint16_t height, uint32_t planeBytes) {
    if (planeBytes == 0u) return false;
    touchSuspendForUpload();
#if defined(OPENDISPLAY_FASTEPD)
    if (displayUsesNativeDriver()) {
        nativeXfer.mode = NATIVE_XFER_PARTIAL;
        nativeXfer.total_bytes = planeBytes * 2u;
        nativeXfer.x = x;
        nativeXfer.y = y;
        nativeXfer.width = width;
        nativeXfer.height = height;
        displayRailAcquire();
        fastepd_partial_prepare(x, y, width, height);
        return true;
    }
#endif
    return displayStarted(od_display_begin_partial(x, y, width, height, planeBytes),
                          "partial upload");
}

extern "C" uint32_t od_xfer_app_write(uint32_t streamOffset, od_span_t data) {
    if (!od_span_valid(data) || data.n == 0u || data.n > UINT32_MAX) return 0u;
#if defined(OPENDISPLAY_FASTEPD)
    if (nativeXfer.mode != NATIVE_XFER_IDLE) {
        if (streamOffset > nativeXfer.total_bytes ||
            data.n > nativeXfer.total_bytes - streamOffset) {
            return 0u;
        }
        if (nativeXfer.mode == NATIVE_XFER_FULL) {
            fastepd_direct_write_chunk(data.p, (uint32_t)data.n);
            return (uint32_t)data.n;
        }
        return fastepd_partial_write_chunk(data.p, (uint32_t)data.n) ? (uint32_t)data.n : 0u;
    }
#endif
    od_watchdog_app_phase(OD_WDT_PHASE_STREAM);
    const bool accepted = od_display_write(streamOffset, data.p, data.n);
    return accepted && !od_epd_esp_faulted() ? (uint32_t)data.n : 0u;
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
    displayXferClear(forceOff);
}

extern "C" od_xfer_barrier_t od_xfer_app_before_refresh(const od_reply_t *owner) {
    (void)owner;
    od_cmd_flush_before_refresh();
    od_hal_delay_ms(20);
    return OD_XFER_BARRIER_PROCEED;
}

extern "C" void od_xfer_app_barrier_abort(const od_reply_t *owner) {
    (void)owner;
    displayXferClear(true);
}

#if defined(OPENDISPLAY_FASTEPD)
static bool nativeRefresh(uint8_t mode) {
    bool ok;
    epdRefreshInProgress = true;
    if (nativeXfer.mode == NATIVE_XFER_PARTIAL) {
        od_log_info("EPD refresh: PARTIAL (rect %u,%u %ux%u)", nativeXfer.x, nativeXfer.y,
                    nativeXfer.width, nativeXfer.height);
        ok = fastepd_partial_refresh(mode);
    } else {
        od_log_info("EPD refresh: %s (mode=%u)", mode == 1u ? "FAST" : "FULL",
                    (unsigned)mode);
        fastepd_direct_refresh(mode);
        ok = fastepd_wait_refresh(60);
    }
    endRefresh();
    memset(&nativeXfer, 0, sizeof(nativeXfer));
    touchResumeAfterUpload();
    displayRailRelease(ok);
    return ok;
}
#endif

extern "C" bool od_xfer_app_refresh(uint8_t mode, bool *completed) {
    if (completed == nullptr) return false;
#if defined(OPENDISPLAY_FASTEPD)
    if (nativeXfer.mode != NATIVE_XFER_IDLE) {
        *completed = nativeRefresh(mode);
        requestAdvertisingRestart();
        return true;
    }
#endif
    if (!od_display_active() || od_epd_esp_faulted()) return false;
    od_log_info("EPD refresh: mode=%u", (unsigned)mode);
    epdRefreshInProgress = true;
    const bool ok = od_display_refresh(mode);
    endRefresh();
    if (!ok) {
        epdSessionForceOff();
    }
    touchResumeAfterUpload();
    requestAdvertisingRestart();
    *completed = ok;
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


// See display_service.h for why this exists rather than main.cpp calling SPI.end() itself.
//
// The REAL implementation is in display_fastepd.cpp, which is the file that legitimately owns
// the FastEPD vendor adapter. This is the fallback for boards that compile no FastEPD at all,
// and it is a NO-OP BY PROOF rather than by assumption: the adapter's bus is only ever brought
// up by SPI.beginTransaction(), whose only callers are FastEPD.inl and display_fastepd.cpp --
// both compiled out here -- so compat SPI's _bus_ok is always false and the SPI.end() this
// replaces could never have freed anything on such a board.
//
// display_service.cpp has no FastEPD/vendor adapter dependency at all. The SPI panel bus is
// released by od_epd_esp_close() on every power-down path.
#if !defined(OPENDISPLAY_FASTEPD)
void displayReleaseSpiBus(void) { }
#endif
