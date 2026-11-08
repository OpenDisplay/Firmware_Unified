/* Shared SHT40 driver. See od_sensor_sht40.h. */

#include "od_sensor_sht40.h"

#include "od_hal_i2c.h"
#include "od_sensor_app.h"
#include "od_log.h"

#define SHT40_CMD_MEASURE_HIGH  0xFDu
#define SHT40_CMD_SOFT_RESET    0x94u
#define SHT40_MEASURE_DELAY_MS  12u
#define SHT40_MSD_POLL_TTL_MS   30000u

/* The dynamic block is 11 bytes and a sample occupies [start, start+2] with start+3 cleared when
 * it is inside the window. */
#define OD_MSD_DYNAMIC_LEN      11u

/* CRC-8, polynomial 0x31, init 0xFF -- the SHT4x datasheet's, one byte per 16-bit word. Both
 * words are checked: a sensor that answers with one good word and one bad is reporting a real
 * fault, not a value to round off. */
static uint8_t sht40_crc8(const uint8_t *data, uint8_t len)
{
    uint8_t crc = 0xFFu;

    for (uint8_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (uint8_t bit = 8; bit > 0; bit--) {
            if ((crc & 0x80u) != 0u) {
                crc = (uint8_t)((crc << 1) ^ 0x31u);
            } else {
                crc = (uint8_t)(crc << 1);
            }
        }
    }
    return crc;
}

/* 0 and 0xFF both mean "unset" for the address; 0x44 is the part's default. */
static uint8_t sht40_addr_7bit(const struct SensorData *s)
{
    uint8_t a = s->i2c_addr_7bit;

    return (a == 0u || a == 0xFFu) ? 0x44u : a;
}

static uint8_t sht40_msd_start(const struct SensorData *s)
{
    uint8_t st = s->msd_data_start_byte;

    return (st == 0xFFu || st == 0u) ? 7u : st;
}

/* err_out keeps the wire-visible encoding the ports shipped: 0xFB no bus, 0xFC second CRC,
 * 0xFD first CRC, 0xFE read failed. A host may already be matching on these. */
static bool sht40_write_cmd(uint8_t bus_id, uint8_t addr7, uint8_t cmd, uint8_t *err_out)
{
    const int rc = od_hal_i2c_write(bus_id, addr7, &cmd, 1u);

    if (err_out != NULL) {
        *err_out = (rc == OD_HAL_I2C_OK) ? 0u : ((rc == OD_HAL_I2C_EINVAL) ? 4u : 2u);
    }
    return rc == OD_HAL_I2C_OK;
}

static bool sht40_read_measurement(uint8_t bus_id, uint8_t addr7,
                                   int16_t *temp_centi, uint16_t *rh_centi, uint8_t *err_out)
{
    uint8_t b[6];
    uint8_t err = 0;

    if (!sht40_write_cmd(bus_id, addr7, SHT40_CMD_MEASURE_HIGH, &err)) {
        if (err_out != NULL) {
            *err_out = err;
        }
        return false;
    }
    /* A SEPARATE transaction from the read, with a STOP and this delay between: the part needs
     * the conversion time before it will answer. Not a repeated-START register read -- that is
     * the BQ27220's idiom, which is why od_hal_i2c exposes both shapes. */
    od_sensor_app_delay_ms(SHT40_MEASURE_DELAY_MS);

    if (od_hal_i2c_read(bus_id, addr7, b, sizeof b) != OD_HAL_I2C_OK) {
        if (err_out != NULL) {
            *err_out = 0xFEu;
        }
        return false;
    }
    if (sht40_crc8(b, 2u) != b[2]) {
        if (err_out != NULL) {
            *err_out = 0xFDu;
        }
        return false;
    }
    if (sht40_crc8(b + 3, 2u) != b[5]) {
        if (err_out != NULL) {
            *err_out = 0xFCu;
        }
        return false;
    }

    uint16_t raw_t = (uint16_t)(((uint16_t)b[0] << 8) | b[1]);
    uint16_t raw_rh = (uint16_t)(((uint16_t)b[3] << 8) | b[4]);
    float tc = -45.0f + 175.0f * ((float)raw_t / 65535.0f);
    float rh = -6.0f + 125.0f * ((float)raw_rh / 65535.0f);

    /* RH only. The datasheet transfer function can land slightly outside 0..100 at the rails,
     * and temperature is deliberately NOT clamped here -- the MSD packing clamps it, and doing it
     * twice would hide a genuinely out-of-range reading. */
    if (rh < 0.0f) {
        rh = 0.0f;
    }
    if (rh > 100.0f) {
        rh = 100.0f;
    }
    *temp_centi = (int16_t)(tc * 100.0f);
    *rh_centi = (uint16_t)(rh * 100.0f);
    return true;
}

/* Try the configured address, then both factory addresses, then do it all again after recovering
 * the bus. TWO PASSES is the authority's behaviour (Firmware/src/sensor_sht40.cpp); the Nordic
 * port had dropped the second, so a sensor that needed a bus recovery never got one. */
static bool sht40_sample(const struct SensorData *s, int16_t *temp_centi, uint16_t *rh_centi,
                         uint8_t *last_err)
{
    const uint8_t bus_id = s->bus_id;
    const uint8_t candidates[3] = { sht40_addr_7bit(s), 0x44u, 0x45u };

    /* 0xFF means this sensor was never assigned a bus. Refused, not resolved to bus 0: probing
     * it on another device's pins risks an address collision returning a plausible-but-wrong
     * temperature (DIVERGENCE_MATRIX 13). */
    if (bus_id == 0xFFu) {
        if (last_err != NULL) {
            *last_err = 0xFBu;
        }
        return false;
    }

    for (uint8_t pass = 0; pass < 2u; pass++) {
        if (pass > 0u) {
            od_sensor_app_bus_recover(bus_id);
        }
        for (uint8_t i = 0; i < 3u; i++) {
            bool duplicate = false;
            uint8_t err = 0;

            for (uint8_t j = 0; j < i; j++) {
                if (candidates[j] == candidates[i]) {
                    duplicate = true;
                    break;
                }
            }
            if (duplicate) {
                continue;
            }
            if (sht40_read_measurement(bus_id, candidates[i], temp_centi, rh_centi, &err)) {
                return true;
            }
            if (last_err != NULL) {
                *last_err = err;
            }
        }
    }
    return false;
}

static int round_centi_to_deci(int16_t c)
{
    return (c >= 0) ? (int)((c + 5) / 10) : (int)((c - 5) / 10);
}

/* MSD, 3 bytes little-endian: v = rh_deci | (tu << 10), where rh_deci is 0..1000 in 0.1% steps
 * and tu is temperature in 0.1 C plus 400. The host decodes it as
 * t_deci = (v >> 10 & 0x7FF) - 400. Changing any of this is a wire change. */
static void sht40_write_msd(uint8_t start, int16_t temp_centi, uint16_t rh_centi)
{
    int t_deci = round_centi_to_deci(temp_centi);
    uint32_t tu;
    uint32_t rh_d;
    uint32_t v;

    if (t_deci < -400) {
        t_deci = -400;
    }
    if (t_deci > 1250) {
        t_deci = 1250;
    }
    tu = (uint32_t)(t_deci + 400);
    rh_d = ((uint32_t)rh_centi + 5u) / 10u;
    if (rh_d > 1000u) {
        rh_d = 1000u;
    }
    v = (rh_d & 0x3FFu) | (tu << 10);

    od_sensor_app_msd_write(start, (uint8_t)(v & 0xFFu));
    od_sensor_app_msd_write((uint8_t)(start + 1u), (uint8_t)((v >> 8) & 0xFFu));
    od_sensor_app_msd_write((uint8_t)(start + 2u), (uint8_t)((v >> 16) & 0xFFu));
    if ((uint16_t)start + 3u < OD_MSD_DYNAMIC_LEN) {
        od_sensor_app_msd_write((uint8_t)(start + 3u), 0u);
    }
}

static void sht40_write_msd_invalid(uint8_t start)
{
    od_sensor_app_msd_write(start, 0xFFu);
    od_sensor_app_msd_write((uint8_t)(start + 1u), 0xFFu);
    od_sensor_app_msd_write((uint8_t)(start + 2u), 0xFFu);
    if ((uint16_t)start + 3u < OD_MSD_DYNAMIC_LEN) {
        od_sensor_app_msd_write((uint8_t)(start + 3u), 0u);
    }
}

/* Address-only probes of the addresses an SHT40-class part might answer on, logged. Diagnostic:
 * the result never changes sensor state. Kept because the authority does it and because "nothing
 * answered anywhere on this bus" is the single most useful line when a board comes up mute. */
static void sht40_probe_bus_once(uint8_t bus_id)
{
    static const uint8_t addrs[5] = { 0x44u, 0x45u, 0x51u, 0x55u, 0x6Au };

    for (uint8_t i = 0; i < 5u; i++) {
        const int rc = od_hal_i2c_probe(bus_id, addrs[i]);

        od_log_debug("SHT40: bus %u probe 0x%02X rc=%d%s", bus_id, addrs[i], rc,
                     (rc == OD_HAL_I2C_OK) ? " (present)" : "");
    }
}

void od_sensor_sht40_init(const struct od_config *cfg)
{
    bool probed = false;

    if (cfg == NULL) {
        return;
    }
    for (uint8_t i = 0; i < cfg->sensor_count; i++) {
        const struct SensorData *s = &cfg->sensors[i];

        if (s->sensor_type != OD_SENSOR_TYPE_SHT40 || s->bus_id == 0xFFu) {
            continue;
        }
        /* Soft reset the configured address; if that is not where the part answers, try both
         * factory addresses, because a reset is the one thing worth doing blind. */
        if (!sht40_write_cmd(s->bus_id, sht40_addr_7bit(s), SHT40_CMD_SOFT_RESET, NULL)) {
            (void)sht40_write_cmd(s->bus_id, 0x44u, SHT40_CMD_SOFT_RESET, NULL);
            (void)sht40_write_cmd(s->bus_id, 0x45u, SHT40_CMD_SOFT_RESET, NULL);
        }
        od_sensor_app_delay_ms(2u);
        if (!probed) {
            /* Once per init, on the first configured sensor's bus -- the authority probes one
             * bus, not one per sensor. */
            sht40_probe_bus_once(s->bus_id);
            probed = true;
        }
    }
}

void od_sensor_sht40_poll(const struct od_config *cfg, uint32_t now_ms)
{
    static uint32_t last_poll_ms;
    static bool have_polled;
    static bool logged_fail;

    if (cfg == NULL) {
        return;
    }
    /* Unsigned subtraction, so the TTL survives the 32-bit millisecond wrap. */
    if (have_polled && (uint32_t)(now_ms - last_poll_ms) < SHT40_MSD_POLL_TTL_MS) {
        return;
    }
    last_poll_ms = now_ms;
    have_polled = true;

    for (uint8_t i = 0; i < cfg->sensor_count; i++) {
        const struct SensorData *s = &cfg->sensors[i];
        int16_t temp_centi = 0;
        uint16_t rh_centi = 0;
        uint8_t err = 0;
        uint8_t start;

        if (s->sensor_type != OD_SENSOR_TYPE_SHT40) {
            continue;
        }
        start = sht40_msd_start(s);
        /* A sample needs three bytes; a start that cannot hold them is a config error, and
         * writing a partial sample would corrupt whatever else shares the block. */
        if ((uint16_t)start + 3u > OD_MSD_DYNAMIC_LEN) {
            continue;
        }
        if (sht40_sample(s, &temp_centi, &rh_centi, &err)) {
            sht40_write_msd(start, temp_centi, rh_centi);
            logged_fail = false;
        } else {
            /* Once per failure run, not once per poll: a mute sensor polled every 30 s would
             * otherwise fill the log. Reset on the next success. The code says WHICH step failed
             * -- 0xFB no bus, 0xFC humidity CRC, 0xFD temperature CRC, 0xFE read -- which is the
             * diagnostic both ports had and which nothing else reports. */
            if (!logged_fail) {
                /* No parenthesis in the format string: the "no nested call in a log argument"
                 * ratchet matches an identifier followed by "(" anywhere in the
                 * call, and cannot tell a format-string paren from a real call. It
                 * errs safe, so the message avoids one. */
                od_log_warn("SHT40: read failed err=0x%02X bus=%u", err, s->bus_id);
                logged_fail = true;
            }
            sht40_write_msd_invalid(start);
        }
    }
}

bool od_sensor_sht40_read_temp_c(const struct od_config *cfg, int *temp_c_out)
{
    if (cfg == NULL || temp_c_out == NULL) {
        return false;
    }
    for (uint8_t i = 0; i < cfg->sensor_count; i++) {
        int16_t temp_centi = 0;
        uint16_t rh_centi = 0;

        if (cfg->sensors[i].sensor_type != OD_SENSOR_TYPE_SHT40) {
            continue;
        }
        if (!sht40_sample(&cfg->sensors[i], &temp_centi, &rh_centi, NULL)) {
            return false;
        }
        *temp_c_out = (temp_centi >= 0) ? ((temp_centi + 50) / 100) : ((temp_centi - 50) / 100);
        return true;
    }
    return false;
}
