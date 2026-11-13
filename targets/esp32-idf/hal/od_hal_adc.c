/* od_hal_adc for ESP-IDF. See od_hal_adc.h.
 *
 * Moved from compat/arduino_compat.cpp's analogRead/analogSetPinAttenuation/
 * analogReadResolution, unchanged in behaviour -- including the ADC1-only restriction, the
 * per-channel attenuation table, the lazy channel configuration, the warn-once bitmap and the
 * resolution shift. The shim's three functions now forward here, so there is exactly one owner
 * of the ADC1 unit handle: adc_oneshot_new_unit() on a unit that is already open fails, and
 * two owners would mean whichever driver initialised second silently read nothing.
 */

#include "od_hal_adc.h"
#include "od_log.h"

#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"

static adc_oneshot_unit_handle_t s_adc1 = NULL;
static int  s_adc_bits = 12;                        /* the SoC's native width */
static uint8_t s_atten[SOC_ADC_CHANNEL_NUM(0)];     /* per-channel, indexed by channel */
static bool s_atten_set[SOC_ADC_CHANNEL_NUM(0)];
static bool s_configured[SOC_ADC_CHANNEL_NUM(0)];
/* One calibration handle per attenuation, created on first use: IDF calibrates a (unit,
 * attenuation) pair, not a channel. [0] = 0 dB, [1] = 12 dB. */
static adc_cali_handle_t s_cali[2];
static bool s_cali_tried[2];

static bool adc_unit_ready(void)
{
    if (s_adc1) {
        return true;
    }
    adc_oneshot_unit_init_cfg_t cfg = { .unit_id = ADC_UNIT_1 };
    if (adc_oneshot_new_unit(&cfg, &s_adc1) != ESP_OK) {
        s_adc1 = NULL;
        return false;
    }
    return true;
}

/* Maps a GPIO to an ADC1 channel. False for pins that are not ADC1-capable. */
static bool adc_channel_for_pin(uint8_t pin, adc_channel_t *chan_out)
{
    adc_unit_t unit;
    adc_channel_t chan;
    if (adc_oneshot_io_to_channel((int)pin, &unit, &chan) != ESP_OK) {
        return false;
    }
    if (unit != ADC_UNIT_1) {
        return false;
    }
    if ((int)chan >= (int)SOC_ADC_CHANNEL_NUM(0)) {
        return false;
    }
    *chan_out = chan;
    return true;
}

bool od_hal_adc_pin_readable(uint8_t pin)
{
    adc_channel_t chan;
    return adc_channel_for_pin(pin, &chan);
}

void od_hal_adc_set_atten(uint8_t pin, od_hal_adc_atten_t atten)
{
    adc_channel_t chan;
    if (!adc_channel_for_pin(pin, &chan)) {
        return;
    }
    const adc_atten_t a = (atten == OD_ADC_ATTEN_0DB) ? ADC_ATTEN_DB_0 : ADC_ATTEN_DB_12;
    if (s_atten_set[chan] && s_atten[chan] == (uint8_t)a) {
        return;
    }
    s_atten[chan]      = (uint8_t)a;
    s_atten_set[chan]  = true;
    s_configured[chan] = false;   /* force a re-config on the next read */
}

void od_hal_adc_set_resolution(uint8_t bits)
{
    if (bits >= 9 && bits <= 12) {
        s_adc_bits = (int)bits;
    }
}

/* Native-width raw count for an ADC1 pin, configuring the channel on first use. False if the
 * pin is not readable or the driver failed; *chan_out names the channel either way it is
 * known. Shared by od_hal_adc_read() and od_hal_adc_read_mv() so both see the same channel
 * configuration and the same warn-once. */
static bool adc_read_native(uint8_t pin, adc_channel_t *chan_out, int *raw_out)
{
    adc_channel_t chan;
    if (!adc_channel_for_pin(pin, &chan)) {
        /* Once per pin, not once per sample: the battery path averages ten samples per call
         * and every MSD refresh drives it, so per-sample warnings buried the rest of the log.
         *
         * An unprovisioned device lands here with pin 0 -- globalConfig is memset to zero, so
         * battery_sense_pin reads 0 rather than the 0xFF "unset" sentinel the caller checks,
         * and GPIO0 is not an ADC1 input on any variant here. Worth saying exactly once. */
        static uint64_t s_warned = 0;   /* bitmap, one bit per GPIO */
        if (pin < 64 && !(s_warned & (1ULL << pin))) {
            s_warned |= (1ULL << pin);
            od_log_warn("GPIO %u is not an ADC1 input; reads return 0 (-1 calibrated) "
                        "(pin 0 usually means no battery_sense_pin is configured)",
                        (unsigned)pin);
        }
        return false;
    }
    if (!adc_unit_ready()) {
        return false;
    }
    if (!s_configured[chan]) {
        adc_oneshot_chan_cfg_t ccfg = {
            .atten    = s_atten_set[chan] ? (adc_atten_t)s_atten[chan] : ADC_ATTEN_DB_12,
            .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        if (adc_oneshot_config_channel(s_adc1, chan, &ccfg) != ESP_OK) {
            return false;
        }
        s_configured[chan] = true;
    }
    if (adc_oneshot_read(s_adc1, chan, raw_out) != ESP_OK) {
        return false;
    }
    *chan_out = chan;
    return true;
}

int od_hal_adc_read(uint8_t pin)
{
    adc_channel_t chan;
    int raw = 0;
    if (!adc_read_native(pin, &chan, &raw)) {
        return 0;
    }
    /* The driver yields the SoC's native width; a lower requested resolution is a shift.
     * Shifting rather than rescaling keeps 12 -> 12 exact. */
    if (s_adc_bits < 12) {
        raw >>= (12 - s_adc_bits);
    }
    return raw;
}

static adc_cali_handle_t cali_for(adc_atten_t atten)
{
    const int i = (atten == ADC_ATTEN_DB_0) ? 0 : 1;
    if (s_cali_tried[i]) {
        return s_cali[i];
    }
    s_cali_tried[i] = true;
    esp_err_t err = ESP_ERR_NOT_SUPPORTED;
#if ADC_CALI_SCHEME_CURVE_FITTING_SUPPORTED
    adc_cali_curve_fitting_config_t cfg = {
        .unit_id  = ADC_UNIT_1,
        .atten    = atten,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    err = adc_cali_create_scheme_curve_fitting(&cfg, &s_cali[i]);
#elif ADC_CALI_SCHEME_LINE_FITTING_SUPPORTED
    adc_cali_line_fitting_config_t cfg = {
        .unit_id  = ADC_UNIT_1,
        .atten    = atten,
        .bitwidth = ADC_BITWIDTH_DEFAULT,
    };
    err = adc_cali_create_scheme_line_fitting(&cfg, &s_cali[i]);
#endif
    if (err != ESP_OK) {
        /* Once per attenuation: tried is set above, so this path is not re-entered. */
        s_cali[i] = NULL;
        od_log_warn("ADC calibration unavailable for %s (err %d); calibrated reads return -1",
                    i ? "12 dB" : "0 dB", (int)err);
    }
    return s_cali[i];
}

int od_hal_adc_read_mv(uint8_t pin)
{
    adc_channel_t chan;
    int raw = 0;
    if (!adc_read_native(pin, &chan, &raw)) {
        return -1;
    }
    const adc_atten_t atten = s_atten_set[chan] ? (adc_atten_t)s_atten[chan] : ADC_ATTEN_DB_12;
    adc_cali_handle_t cali = cali_for(atten);
    int mv = 0;
    if (!cali || adc_cali_raw_to_voltage(cali, raw, &mv) != ESP_OK) {
        return -1;
    }
    return mv;
}

/* ------------------------------------------------------------------ die temperature
 *
 * Moved here verbatim from compat/arduino_compat.cpp (phase C step 14) so the peripheral has
 * ONE owner. Behaviour, including the lazy install and the -999.0f sentinel, is unchanged. */
#include "soc/soc_caps.h"
#if SOC_TEMP_SENSOR_SUPPORTED
#include "driver/temperature_sensor.h"
#endif

float od_hal_adc_die_temp_c(void)
{
#if SOC_TEMP_SENSOR_SUPPORTED
    static temperature_sensor_handle_t s_tsens = NULL;
    if (!s_tsens) {
        /* -10..80 C covers the panel's rated operating range with margin; the driver picks
         * the matching internal range setting. */
        temperature_sensor_config_t cfg = TEMPERATURE_SENSOR_CONFIG_DEFAULT(-10, 80);
        if (temperature_sensor_install(&cfg, &s_tsens) != ESP_OK) {
            s_tsens = NULL;
            return -999.0f;
        }
        if (temperature_sensor_enable(s_tsens) != ESP_OK) {
            temperature_sensor_uninstall(s_tsens);
            s_tsens = NULL;
            return -999.0f;
        }
    }
    float c = 0.0f;
    if (temperature_sensor_get_celsius(s_tsens, &c) != ESP_OK) {
        return -999.0f;
    }
    return c;
#else
    /* Classic ESP32 has no usable die sensor. -999.0 is the sentinel the NRF path uses for
     * "no reading", and readChipTemperature()'s callers already understand it. */
    return -999.0f;
#endif
}
