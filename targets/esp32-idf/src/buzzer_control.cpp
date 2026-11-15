/* ESP32 adapter for the shared buzzer melody runner. */

#include "buzzer_control.h"
#include "buzzer_hw.h"
#include "structs.h"

#include "od_buzzer.h"
#include "od_buzzer_app.h"
#include "od_cmd_reply.h"
#include "od_hal_gpio.h"
#include "od_hal_sleep.h"
#include "od_hal_time.h"

extern struct od_config globalConfig;

static_assert(sizeof(BuzzerConfig) == 32, "BuzzerConfig must be 32 bytes");

static bool s_buzzer_running;
static uint32_t s_buzzer_due_ms;

/* ---------------------------------------------------------------- the hardware seam --- */

extern "C" bool od_buzzer_app_tone_start(uint8_t drive_pin, uint32_t centihz,
                                          uint8_t duty_percent)
{
    return buzzer_hw_tone_start(drive_pin, centihz, duty_percent);
}

extern "C" void od_buzzer_app_tone_stop(uint8_t drive_pin)
{
    buzzer_hw_tone_stop(drive_pin);
}

extern "C" void od_buzzer_app_enable_write(uint8_t enable_pin, bool level_high)
{
    if (enable_pin != OD_PIN_UNUSED) {
        od_hal_gpio_write(enable_pin, level_high);
    }
}

static void buzzer_set_enable(const BuzzerConfig *b, bool on)
{
    bool high;

    if (b->enable_pin == OD_PIN_UNUSED) {
        return;
    }
    high = on;
    if ((b->flags & OD_BUZZER_FLAG_ENABLE_ACTIVE_HIGH) == 0u) {
        high = !high;
    }
    od_buzzer_app_enable_write(b->enable_pin, high);
}

/* -------------------------------------------------------------------- scheduling --- */

static void buzzer_pump(void)
{
    const uint32_t now = od_hal_uptime_ms();
    const uint32_t delay_ms = od_buzzer_service(now);

    if (delay_ms == OD_BUZZER_IDLE) {
        s_buzzer_running = false;
        return;
    }
    s_buzzer_due_ms = now + delay_ms;
}

void buzzerService(void)
{
    if (!s_buzzer_running) {
        return;
    }
    if ((int32_t)(od_hal_uptime_ms() - s_buzzer_due_ms) < 0) {
        return;
    }
    buzzer_pump();
}

void buzzerStopForSleep(void)
{
    od_buzzer_stop();
    s_buzzer_running = false;
}

/* ------------------------------------------------------------------------- setup --- */

void initPassiveBuzzers(void)
{
    for (uint8_t i = 0u; i < globalConfig.passive_buzzer_count; i++) {
        const BuzzerConfig *b = &globalConfig.passive_buzzers[i];

        if (b->drive_pin == OD_PIN_UNUSED) {
            continue;
        }
        od_hal_gpio_config_output(b->drive_pin, false);
        if (b->enable_pin != OD_PIN_UNUSED) {
            od_hal_gpio_config_output(b->enable_pin, false);
            buzzer_set_enable(b, false);
        }
    }
}

/* ---------------------------------------------------------------------- wire hook --- */

od_cmd_result_t handleBuzzerActivate(const od_cmd_ctx_t *ctx, uint8_t *data, uint16_t len)
{
    BuzzerConfig *b;
    struct od_buzzer_config config;
    uint8_t instance;
    int rc;

    if (data == nullptr || len < 3u) {
        uint8_t err[] = {RESP_NACK, RESP_BUZZER_ACK, 0x01u, 0x00u};
        (void)od_cmd_reply_plain(ctx, err, sizeof err);
        return OD_CMD_NACK;
    }
    instance = data[0];
    if (instance >= globalConfig.passive_buzzer_count) {
        uint8_t err[] = {RESP_NACK, RESP_BUZZER_ACK, 0x02u, 0x00u};
        (void)od_cmd_reply_plain(ctx, err, sizeof err);
        return OD_CMD_NACK;
    }
    b = &globalConfig.passive_buzzers[instance];
    if (b->drive_pin == OD_PIN_UNUSED) {
        uint8_t err[] = {RESP_NACK, RESP_BUZZER_ACK, 0x03u, 0x00u};
        (void)od_cmd_reply_plain(ctx, err, sizeof err);
        return OD_CMD_NACK;
    }

    config.drive_pin = b->drive_pin;
    config.enable_pin = b->enable_pin;
    config.flags = b->flags;
    config.duty_percent = b->duty_percent;
    rc = od_buzzer_activate(&config, data, len, od_hal_uptime_ms());
    if (rc != 0) {
        uint8_t err[] = {RESP_NACK, RESP_BUZZER_ACK, (uint8_t)rc, 0x00u};
        (void)od_cmd_reply_plain(ctx, err, sizeof err);
        return OD_CMD_NACK;
    }

    s_buzzer_running = true;
    buzzer_pump();
    {
        uint8_t ok[] = {RESP_ACK, RESP_BUZZER_ACK, 0x00u, 0x00u};
        (void)od_cmd_reply(ctx, ok, sizeof ok);
    }
    return OD_CMD_OK;
}

/* ---------------------------------------------------------------- shutdown chirp --- */

/* Short A5 chirp on a physical button press, from the first buzzer that opted in with
 * OD_BUZZER_FLAG_BUTTON_PRESS. Blocking (60 ms); interrupts any playback. */
void passiveBuzzerButtonPressAlert(void)
{
    const BuzzerConfig *b = nullptr;

    for (uint8_t i = 0u; i < globalConfig.passive_buzzer_count; i++) {
        const BuzzerConfig *cand = &globalConfig.passive_buzzers[i];

        if (cand->drive_pin == 0u || cand->drive_pin == OD_PIN_UNUSED) continue;
        if ((cand->flags & OD_BUZZER_FLAG_BUTTON_PRESS) == 0u) continue;
        b = cand;
        break;
    }
    if (b == nullptr) {
        return;
    }
    od_buzzer_stop();
    s_buzzer_running = false;
    buzzer_set_enable(b, true);
    (void)buzzer_hw_tone_start(b->drive_pin, od_buzzer_index_centihz(nA5), b->duty_percent);
    od_hal_delay_ms(60u);
    buzzer_hw_tone_stop(b->drive_pin);
    buzzer_set_enable(b, false);
}

void passiveBuzzerPowerOffAlert(void)
{
    const BuzzerConfig *b = nullptr;

    od_buzzer_stop();
    s_buzzer_running = false;
    for (uint8_t i = 0u; i < globalConfig.passive_buzzer_count; i++) {
        const uint8_t pin = globalConfig.passive_buzzers[i].drive_pin;

        if (pin != 0u && pin != OD_PIN_UNUSED) {
            b = &globalConfig.passive_buzzers[i];
            break;
        }
    }
    if (b == nullptr) {
        return;
    }

    const uint32_t centihz = od_buzzer_index_centihz(nG8);
    buzzer_set_enable(b, true);
    (void)buzzer_hw_tone_start(b->drive_pin, centihz, b->duty_percent);
    od_hal_delay_ms(80u);
    buzzer_hw_tone_stop(b->drive_pin);
    od_hal_delay_ms(80u);
    (void)buzzer_hw_tone_start(b->drive_pin, centihz, b->duty_percent);
    od_hal_delay_ms(80u);
    buzzer_hw_tone_stop(b->drive_pin);
    buzzer_set_enable(b, false);
}
