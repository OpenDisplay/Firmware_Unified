/* od_hal_adc -- one-shot ADC reads.
 *
 * Not in docs/SHARED_API_DESIGN.md at all. The core does not read analogue inputs; this exists
 * because two TARGET drivers do -- device_control.cpp's ADC button ladder and
 * display_service.cpp's battery sense -- and IDF's oneshot driver has one unit handle that
 * both must share.
 *
 * ADC1 ONLY, deliberately. ADC2 shares its hardware with the WiFi radio on ESP32/S2/S3 and
 * reads fail with ESP_ERR_TIMEOUT whenever WiFi is up -- and this target's boards run WiFi. A
 * pin that maps to ADC2 is reported unreadable rather than read unreliably.
 *
 * WHY A ZERO RETURN IS DANGEROUS HERE, and why this interface keeps a separate "is it
 * readable" question: both callers turn a reading into something a host believes.
 * readBatteryVoltageUncached() multiplies by a scaling factor and publishes the result in the
 * MSD advert, so 0 becomes a plausible 0.0 V rather than the -1.0 "unknown" sentinel it uses
 * when no sense pin is configured. The button ladder classifies 0 into its catch-all bottom
 * bucket, which reads as the last button permanently pressed. That is the exact failure the
 * original `return 0` stub produced, and it is why od_hal_adc_pin_readable() exists rather
 * than leaving every caller to infer failure from a value that is also a legal reading.
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Attenuation. Two values, because two are used: 0 dB for the narrow span and 12 dB for the
 * wide one. Named for the silicon rather than for Arduino's ADC_11db, which IDF 5.x deprecated
 * in favour of DB_12 -- the same setting under a name that matches the hardware. */
typedef enum {
    OD_ADC_ATTEN_0DB = 0,
    OD_ADC_ATTEN_12DB,
} od_hal_adc_atten_t;

/* True if the pin maps to an ADC1 channel, i.e. if a read from it can mean anything. Ask this
 * before believing a zero -- see the header note. */
bool od_hal_adc_pin_readable(uint8_t pin);

/* Takes effect on the next read of that pin. Ignored for a pin that is not ADC1-capable. */
void od_hal_adc_set_atten(uint8_t pin, od_hal_adc_atten_t atten);

/* Output width, 9..12 bits. The driver always samples at the SoC's native width (12 on every
 * variant here) and the result is shifted down, so 12 is exact and anything lower discards
 * low bits rather than resampling. Out-of-range values are ignored. */
void od_hal_adc_set_resolution(uint8_t bits);

/* Raw count, or 0 if the pin is not readable or the driver failed. Warns once per GPIO, not
 * once per sample: the battery path averages ten samples per call and is driven by every MSD
 * refresh, so per-sample warnings buried the rest of the log. */
int od_hal_adc_read(uint8_t pin);

/* Millivolts at the pin through IDF's eFuse ADC calibration (curve fitting on S3/C3/C6, line
 * fitting on classic ESP32), or -1 if the pin is not readable, the driver failed, or the chip
 * has no calibration data for the pin's attenuation. -1, not 0, for the reason in the header
 * note: the battery path publishes what it computes, and 0 mV would read as a dead battery.
 * Uses the same channel configuration, attenuation and warn-once handling as
 * od_hal_adc_read(); the resolution setting does not apply. */
int od_hal_adc_read_mv(uint8_t pin);

/* Die temperature in degrees Celsius, or -999.0f when this part has no usable sensor or the
 * driver refuses. It lives with the ADC because it IS an on-chip analog read, and because the
 * same "one owner of the peripheral" rule applies: IDF's temperature_sensor driver installs a
 * single handle, so two callers installing it independently is the failure this HAL exists to
 * prevent.
 *
 * -999.0f rather than NaN or a status out-parameter: that is the sentinel the callers already
 * understand (readChipTemperature() returns it on the NRF path too), and changing it would
 * change what the device reports over the wire. */
float od_hal_adc_die_temp_c(void);

#ifdef __cplusplus
}
#endif
