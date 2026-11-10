#ifndef DISPLAY_SERVICE_H
#define DISPLAY_SERVICE_H

#include "od_cmd.h"
#include "od_txq.h"   /* od_origin_t, via od_hal_radio.h */

#include <stdint.h>
#include <stdbool.h>

#define OPENDISPLAY_DECOMPRESSION_CHUNK_SIZE 2048

// EPD panel power state machine states. The enum type + keep-alive constant live
// in this shared header (included by both main.cpp via main.h and by
// display_service.cpp) so the type is visible to every TU. The single
// source-of-truth VARIABLES (pwrmgmState / pwrmgmOffDeadlineMs) are
// DEFINED in main.h next to displayPowerState and externed in display_service.cpp.
// PWR_OFF must be 0: BSS-zero after boot / ESP32 deep-sleep wake == rail off.
enum PwrMgmState : uint8_t { PWR_OFF = 0, PWR_WARM = 1, PWR_ACTIVE = 2 };
#define EPD_KEEPALIVE_MAX_S 30   // hard cap on power_option.screen_timeout_seconds (clamped, not rejected)

// EPD panel power session (keep-alive) cross-TU API, implemented in od_display_esp.cpp, which
// owns the ACTIVE<->WARM transitions.
void epdSessionForceOff(void);   // power the panel fully down now (idempotent)
void epdSessionTick(void);       // millis()-poll from loop()/idleDelay(): expire keep-alive
bool epdSessionIsWarm(void);     // true when the panel is powered-idle (PWR_WARM)

/* True when display 0 is on a built-in driver (IT8951 / FastEPD parallel); see od_display_esp.h. */
bool fastepd_driver_used(void);
float readBatteryVoltage();
float readChipTemperature();
void updatemsdata();
void initio();
void initDataBuses();
/** True when ANY configured DataBus record is a usable I2C bus (pin_1/pin_2 not 0xFF). */
bool openDisplayI2cBusConfigured(void);
/** Re-apply I2C from the first DataBus record when usable; else default pins. Call before
 *  TCON/touch on a shared bus. */
void initOrRestoreWireForOpenDisplay(void);
/** Select the bus whose DataBus.instance_number is bus_id -- NOT data_buses[bus_id], which is
 *  only the same while records arrive in order (DIVERGENCE_MATRIX 14). 0xFF means unassigned and
 *  is REFUSED, never resolved to bus 0 (DIVERGENCE_MATRIX 13). A duplicated instance_number is
 *  ambiguous and also refused. Switches pins when multiple I2C buses are configured. */
bool initOrRestoreWireForBus(uint8_t bus_id);
/** Call after Wire.end() so the next touch/sensor access re-inits the bus. */
void invalidateOpenDisplayWire(void);
void scanI2CDevices();
void initSensors();
void initAXP2101(uint8_t busId);
void readAXP2101Data();
void powerDownAXP2101();
void initDisplay();
void writeTextAndFill(const char* text);
// True while shared transfer hardware is active.
bool transferActive(void);
extern volatile bool epdRefreshInProgress;
/**
 * Close the refresh bracket: clears epdRefreshInProgress AND re-stamps the owner's
 * activity clock. Every refresh path must end through this rather than assigning
 * the flag, or that path silently loses the R4 refresh exclusion and can drop an
 * engaged client the moment loop() resumes.
 */
void endRefresh(void);
// Shared transfer timeout policy.
void checkTransferTimeouts(void);

/* Release the panel SPI bus. Called from main.cpp's deep-sleep teardown, which used to reach
 * for SPI.end() itself -- the last thing keeping <SPI.h> in that file (phase C step 12).
 *
 * It belongs here rather than there on ownership grounds: vendor/fastepd/SPI.h's end() releases the
 * device AND frees the bus using ownership state (_owns_bus) that only this layer sets up, and
 * whether the bus is owned at all depends on the FastEPD driver selection this layer makes.
 * main.cpp asking the display to release its own bus is the same shape as it asking
 * wifi_service for the link state rather than reaching into esp_wifi (step 9b-ii).
 *
 * No-op when the FastEPD parallel driver owns the pins -- that path never took the SPI bus. */
void displayReleaseSpiBus(void);
// Origin used by the target watchdog to select the link it may drop.
od_origin_t transferSessionOrigin(void);
int displayBootBitsPerPixel(uint8_t colorScheme);

#endif
