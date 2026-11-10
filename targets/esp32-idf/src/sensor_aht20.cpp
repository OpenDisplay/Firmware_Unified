/* AHT20 temperature/humidity sensor (OD_SENSOR_TYPE_AHT20, I2C 0x38).
 *
 * Sampled once per boot: on battery boards every wake is a boot, and the NM-EPD-420 gates the
 * sensor's supply on GPIO40, which is held off between samples. The MSD packing is the SHT40's
 * (shared/core/od_sensor_sht40.c), so a host decodes both the same way; it moves to shared/ with
 * this driver if the driver is promoted. */
#include "sensor_aht20.h"

#include "display_service.h"
#include "od_hal_gpio.h"
#include "od_hal_i2c.h"
#include "od_hal_sleep.h"
#include "od_log.h"
#include "protocol_pending.h"
#include "structs.h"

extern struct od_config globalConfig;
extern uint8_t dynamicreturndata[11];

static uint8_t aht20Crc(const uint8_t* data, uint8_t length) {
    uint8_t crc = 0xFF;
    for (uint8_t index = 0; index < length; index++) {
        crc ^= data[index];
        for (uint8_t bit = 0; bit < 8; bit++) {
            crc = (crc & 0x80) ? (uint8_t)((crc << 1) ^ 0x31) : (uint8_t)(crc << 1);
        }
    }
    return crc;
}

static bool aht20Read(uint8_t bus, uint8_t address, int16_t* temperatureCenti,
                      uint16_t* humidityCenti) {
    const uint8_t initialize[] = {0xBE, 0x08, 0x00};
    if (od_hal_i2c_write(bus, address, initialize, sizeof(initialize)) != 0) return false;
    od_hal_delay_ms(10);

    const uint8_t trigger[] = {0xAC, 0x33, 0x00};
    for (uint8_t attempt = 0; attempt < 2; attempt++) {
        if (od_hal_i2c_write(bus, address, trigger, sizeof(trigger)) != 0) return false;
        od_hal_delay_ms(85);
        uint8_t sample[7];
        if (od_hal_i2c_read(bus, address, sample, sizeof(sample)) != 0) continue;
        if ((sample[0] & 0x88) != 0x08 || aht20Crc(sample, 6) != sample[6]) continue;

        const uint32_t humidityRaw = ((uint32_t)sample[1] << 12) |
                                     ((uint32_t)sample[2] << 4) | (sample[3] >> 4);
        const uint32_t temperatureRaw = ((uint32_t)(sample[3] & 0x0F) << 16) |
                                        ((uint32_t)sample[4] << 8) | sample[5];
        *humidityCenti = (uint16_t)(((uint64_t)humidityRaw * 10000u + 524288u) / 1048576u);
        *temperatureCenti =
            (int16_t)((int32_t)(((uint64_t)temperatureRaw * 20000u + 524288u) / 1048576u) - 5000);
        return true;
    }
    return false;
}

uint32_t aht20PackTempHumidity(int16_t temp_centi, uint16_t rh_centi) {
    int t_deci = (temp_centi >= 0) ? (temp_centi + 5) / 10 : (temp_centi - 5) / 10;
    if (t_deci < -400) t_deci = -400;
    if (t_deci > 1250) t_deci = 1250;
    uint32_t rh_d = ((uint32_t)rh_centi + 5u) / 10u;
    if (rh_d > 1000u) rh_d = 1000u;
    return (rh_d & 0x3FFu) | ((uint32_t)(t_deci + 400) << 10);
}

static void writeMsd(uint8_t start, const uint8_t b[3]) {
    for (uint8_t i = 0; i < 3; i++) dynamicreturndata[start + i] = b[i];
    if (start + 3u < sizeof(dynamicreturndata)) dynamicreturndata[start + 3] = 0;
}

void pollAht20SensorsForMsd(void) {
    static bool sampledThisBoot = false;
    if (sampledThisBoot) return;

    for (uint8_t index = 0; index < globalConfig.sensor_count; index++) {
        const struct SensorData* sensor = &globalConfig.sensors[index];
        if (sensor->sensor_type != OD_SENSOR_TYPE_AHT20) continue;
        sampledThisBoot = true;

        uint8_t start = sensor->msd_data_start_byte;
        if (start == 0 || start == 0xFF) start = 7;
        if (start > 8) return;

#if defined(CONFIG_IDF_TARGET_ESP32S3)
        const bool gated = (globalConfig.system_config.device_flags & OD_DEVICE_FLAG_NM420_INIT) != 0;
#else
        const bool gated = false;
#endif
        if (gated) {
            od_hal_gpio_write(40, true);   // NM-EPD-420 TEMP_CTL
            od_hal_delay_ms(100);
        }
        int16_t temperatureCenti = 0;
        uint16_t humidityCenti = 0;
        const uint8_t busId = sensor->bus_id == 0xFF ? 0 : sensor->bus_id;
        const uint8_t address = (sensor->i2c_addr_7bit == 0 || sensor->i2c_addr_7bit == 0xFF)
                                    ? 0x38 : sensor->i2c_addr_7bit;
        const bool valid = initOrRestoreWireForBus(busId) &&
                           aht20Read(busId, address, &temperatureCenti, &humidityCenti);
        if (gated) {
            // The gated rail also powers the bus pull-ups on that board; drop the bus with it
            // when nothing else lives on it.
            if (globalConfig.sensor_count == 1 && globalConfig.touch_controller_count == 0) {
                invalidateOpenDisplayWire();
            }
            od_hal_gpio_write(40, false);
        }
        if (valid) {
            const uint32_t v = aht20PackTempHumidity(temperatureCenti, humidityCenti);
            const uint8_t b[3] = {(uint8_t)(v & 0xFF), (uint8_t)((v >> 8) & 0xFF),
                                  (uint8_t)((v >> 16) & 0xFF)};
            writeMsd(start, b);
            od_log_info("AHT20: wake-time sample %d.%02d C, %u.%02u %%RH (bus %u, addr 0x%02X)",
                        temperatureCenti / 100, (temperatureCenti < 0 ? -temperatureCenti : temperatureCenti) % 100,
                        humidityCenti / 100u, humidityCenti % 100u, busId, address);
        } else {
            const uint8_t b[3] = {0xFF, 0xFF, 0xFF};
            writeMsd(start, b);
            od_log_warn("AHT20: wake-time sample failed (bus %u, addr 0x%02X)", busId, address);
        }
        return;
    }
}
