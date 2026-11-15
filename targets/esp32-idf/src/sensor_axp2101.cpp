#include "sensor_axp2101.h"

#include "opendisplay_structs.h"
#include "display_service.h"
#include "od_hal_i2c.h"
#include "od_hal_sleep.h"
#include "od_log.h"

static const struct SensorData* axp2101_config(const struct SensorData* sensors, uint8_t count) {
    if (sensors == nullptr) {
        return nullptr;
    }
    for (uint8_t i = 0; i < count; i++) {
        if (sensors[i].sensor_type == OD_SENSOR_TYPE_AXP2101) {
            return &sensors[i];
        }
    }
    return nullptr;
}

// bus_id 0xFF means "no data bus assigned"; the PhotoPainter preset relies on bus 0 then.
static uint8_t axp2101_bus_id(const struct SensorData* s) {
    return (s->bus_id == 0xFF) ? 0u : s->bus_id;
}

static bool axp2101_read_block(uint8_t bus, uint8_t addr, uint8_t reg, uint8_t* out, uint8_t len) {
    return od_hal_i2c_write_read(bus, addr, &reg, 1u, out, len) == 0;
}

static bool axp2101_write_reg(uint8_t bus, uint8_t addr, uint8_t reg, uint8_t value) {
    const uint8_t tx[2] = {reg, value};
    return od_hal_i2c_write(bus, addr, tx, 2u) == 0;
}

float axp2101BatteryVoltageVolts(const struct SensorData* sensors, uint8_t sensor_count) {
    const struct SensorData* s = axp2101_config(sensors, sensor_count);
    if (s == nullptr) {
        return -1.0f;
    }
    const uint8_t bus = axp2101_bus_id(s);
    if (!initOrRestoreWireForBus(bus)) {
        od_log_warn("AXP2101: bus %u init failed", bus);
        return -1.0f;
    }
    const uint8_t addr = axp2101_resolve_addr(s->i2c_addr_7bit);

    uint8_t status = 0;
    if (!axp2101_read_block(bus, addr, AXP2101_REG_POWER_STATUS_ADDR, &status, 1)) {
        od_log_warn("AXP2101: power-status read failed @0x%02X", addr);
        return -1.0f;
    }
    od_log_debug("AXP2101: power_status=0x%02X batt=%d vbus=%d", status,
                 (int)axp2101_batt_present(status), (int)axp2101_vbus_present(status));
    if (!axp2101_batt_present(status)) {
        return -1.0f;
    }

    uint8_t adc_ctrl = 0;
    if (!axp2101_read_block(bus, addr, AXP2101_REG_ADC_CHANNEL_CTRL_ADDR, &adc_ctrl, 1)) {
        od_log_warn("AXP2101: ADC ctrl read failed @0x%02X", addr);
        return -1.0f;
    }
    bool channel_changed = false;
    const uint8_t adc_ctrl_new = axp2101_adc_enable_bit0(adc_ctrl, &channel_changed);
    if (channel_changed) {
        if (!axp2101_write_reg(bus, addr, AXP2101_REG_ADC_CHANNEL_CTRL_ADDR, adc_ctrl_new)) {
            od_log_warn("AXP2101: ADC ctrl enable failed @0x%02X", addr);
            return -1.0f;
        }
        od_hal_delay_ms(AXP2101_ADC_SETTLING_MS);
    }

    uint8_t vbat_raw[2] = {0, 0};
    if (!axp2101_read_block(bus, addr, AXP2101_REG_VBAT_H_ADDR, vbat_raw, 2)) {
        od_log_warn("AXP2101: VBAT read failed @0x%02X", addr);
        return -1.0f;
    }
    const uint16_t mv = axp2101_decode_vbat_mv(vbat_raw[0], vbat_raw[1]);
    od_log_debug("AXP2101: VBAT raw=[0x%02X,0x%02X] -> %u mV", vbat_raw[0], vbat_raw[1],
                 (unsigned)mv);
    return (float)mv / 1000.0f;
}
