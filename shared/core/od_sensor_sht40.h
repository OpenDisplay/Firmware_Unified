/* Shared SHT40 driver: device policy, not just conversion.
 *
 * It owns the config walk, address candidates and probing, the soft reset, the measurement
 * sequence and its retry policy, both CRC-8 checks, the conversion and clamps, the poll TTL and
 * the MSD packing. What it does NOT own is the bus (shared/hal/od_hal_i2c.h) and the three things
 * in od_sensor_app.h.
 *
 * MULTI-INSTANCE. Every SensorData entry of type SHT40 is walked, because both ports already
 * supported more than one and collapsing that to "the first" would be a silent capability loss.
 * (BQ27220 is the opposite and takes the first match only -- see od_sensor_bq27220.h.)
 *
 * `cfg` is not retained after return. Both ports kept their TTL and cache statics across an init
 * call, so this does too rather than silently making a config reload a state reset.
 */

#ifndef OD_SENSOR_SHT40_H
#define OD_SENSOR_SHT40_H

#include "od_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Soft-reset every configured SHT40 and probe its bus once. */
void od_sensor_sht40_init(const struct od_config *cfg);

/* Sample every configured SHT40 whose poll TTL has expired and pack the results into the MSD.
 * A sensor that cannot be read writes the invalid marker rather than leaving stale bytes. */
/* `now_ms` is the caller's clock, passed EXPLICITLY. shared/core does not sample the ambient
 * time HAL -- ratcheted by tools/check.sh -- for the same reason od_led and od_buzzer return a
 * delay instead of sleeping: policy that reads a clock it does not own cannot be tested against
 * a wrap, and cannot be driven by a target that schedules differently. */
void od_sensor_sht40_poll(const struct od_config *cfg, uint32_t now_ms);

/* A fresh reading of the first configured SHT40, in whole degrees C, for a panel waveform choice.
 * It bypasses the poll TTL and leaves the MSD alone. False when none is configured or it cannot
 * be read. */
bool od_sensor_sht40_read_temp_c(const struct od_config *cfg, int *temp_c_out);

#ifdef __cplusplus
}
#endif

#endif /* OD_SENSOR_SHT40_H */
