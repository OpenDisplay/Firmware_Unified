#ifndef SENSOR_AHT20_H
#define SENSOR_AHT20_H

#include <stdint.h>

/* Sample every configured AHT20 once per boot (wake) and pack the result into the MSD in the
 * SHT40 format. Call from updatemsdata(). */
void pollAht20SensorsForMsd(void);

/* The 3-byte MSD temperature/humidity packing, exposed for the host test. */
uint32_t aht20PackTempHumidity(int16_t temp_centi, uint16_t rh_centi);

#endif
