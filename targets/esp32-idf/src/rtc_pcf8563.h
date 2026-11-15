#pragma once
/* PCF8563 / BM8563 RTC as a power-off wake source (OD_SENSOR_TYPE_PCF8563).
 *
 * Active only together with the MOSFET power latch (pwr_latch, pwr_pin_2): the RTC's INT line
 * switches the rail back on, so deep sleep can release the latch instead of holding it. The
 * sensor publishes no MSD data. */
#include <stdbool.h>
#include <stdint.h>

/* A PCF8563 sensor is configured and the MOSFET latch is in use. */
bool rtcWakeConfigured(void);

/* Call once at boot, after the power hold is asserted. Clears the RTC's interrupt flags, timer
 * and alarm (INT is level mode and holds the rail on until they clear) and returns true when the
 * timer or alarm had fired, i.e. this cold boot is an RTC wake. */
bool rtcConsumeWake(void);

/* Arm a wake `seconds` from now: <= 255 s a 1 Hz countdown, <= 255 min a 1/60 Hz countdown
 * (rounded up), longer a minute+hour alarm within the next 24 h. Returns false when no RTC wake
 * is configured or the I2C writes fail; the caller then keeps the rail held. */
bool rtcArmWake(uint32_t seconds);
