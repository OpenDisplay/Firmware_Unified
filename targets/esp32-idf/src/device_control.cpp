#include "od_smp.h"
#include "device_control.h"
#include "od_adv_app.h"
#include "structs.h"
#include "touch_input.h"
#include "power_latch.h"
#include "buzzer_control.h"
#include "od_log.h"
#include <string.h>

void enterDeepSleep(bool force = false, uint16_t overrideSleepSeconds = 0);

#include <esp_system.h>
#include "driver/gpio.h"
#include "od_hal_adc.h"
#include "od_hal_gpio.h"
#include "od_led.h"
#include "od_led_app.h"
#include "od_hal_time.h"
#include "od_hal_sleep.h"
#include "od_txq.h"
#include "wifi_service.h"      // OPENDISPLAY_HAS_WIFI + opendisplay_lan_teardown()

#include "ble_transport.h"

extern uint8_t rebootFlag;
extern struct od_config globalConfig;
extern uint8_t activeLedInstance;
extern uint8_t dynamicreturndata[11];
extern uint8_t buttonStateCount;
extern volatile bool buttonEventPending;
extern volatile uint8_t lastChangedButtonIndex;
void updatemsdata();
#include "od_cmd_reply.h"

extern ButtonState buttonStates[MAX_BUTTONS];

// The two powerLatch*Configured() predicates return false unless the board actually
// declares a latch (DEVICE_FLAG_BATTERY_LATCH / DEVICE_FLAG_PWR_LATCH_DFF plus the
// pins), so this runs on every board and does nothing on those without one.
static bool s_pwrOffReleased[MAX_BUTTONS];
static bool s_pwrOffPressing[MAX_BUTTONS];
static bool s_pwrOffDone[MAX_BUTTONS];
static uint32_t s_pwrOffStartMs[MAX_BUTTONS];

static void pollConfiguredPowerOffButtons() {
    if (!powerLatchDffConfigured() && !powerLatchMosfetConfigured()) {
        return;
    }
    for (uint8_t i = 0; i < buttonStateCount; i++) {
        ButtonState* btn = &buttonStates[i];
        if (!btn->initialized || !btn->power_off) {
            continue;
        }
        bool pinState = (od_hal_gpio_read(btn->pin) != 0);
        bool down = btn->inverted ? !pinState : pinState;
        if (!down) {
            s_pwrOffReleased[i] = true;
            s_pwrOffPressing[i] = false;
            s_pwrOffDone[i] = false;
            continue;
        }
        if (!s_pwrOffReleased[i]) {
            continue;
        }
        if (!s_pwrOffPressing[i]) {
            s_pwrOffPressing[i] = true;
            s_pwrOffStartMs[i] = od_hal_uptime_ms();
            continue;
        }
        if (!s_pwrOffDone[i] && od_hal_uptime_ms() - s_pwrOffStartMs[i] >= btn->power_off_hold_ms) {
            s_pwrOffDone[i] = true;
            passiveBuzzerPowerOffAlert();
            powerLatchTriggerOff();
        }
    }
}

// BinaryInputs.input_type wire values (see structs.h for the full contract).
// 2 is reserved for switches (host-side feature); the ADC ladder uses 3.
#define BINARY_INPUT_TYPE_ADC_LADDER 3

// --- ADC resistor-ladder buttons (e.g. XTEINK X4) -------------------------
// Several buttons share one ADC pin via a resistor ladder, distinguished by
// voltage. They have no edge interrupt, so they are polled. Reported through
// the same MSD button byte as digital buttons for a uniform host contract.
//
// The single SoC-specific call (input range) is shimmed in adcLadderConfigurePin()
// below. Everything else is the HAL's ADC, GPIO and clock.
#define MAX_ADC_LADDERS     4
#define MAX_LADDER_BUTTONS  4    // reserved[] holds at most N+1 = 5 LE uint16 thresholds
#define ADC_LADDER_POLL_MS  5
#define ADC_LADDER_DEBOUNCE 3    // consecutive equal samples required to accept a change

// Thresholds for N+1 buttons must fit in BinaryInputs.reserved[]; fail the build if not.
static_assert(2 + 2 * (MAX_LADDER_BUTTONS + 1) <= sizeof(BinaryInputs::reserved),
              "ADC ladder thresholds would overflow BinaryInputs.reserved[]");

struct AdcLadder {
    uint8_t  pin;
    uint8_t  num_buttons;
    uint8_t  id_base;
    uint8_t  byte_index;
    uint16_t thresholds[MAX_LADDER_BUTTONS + 1];  // descending; [0] = idle ceiling
    int8_t   current_button;     // -1 = none pressed
    int8_t   candidate_button;   // debounce: last raw classification
    uint8_t  candidate_count;    // consecutive samples equal to candidate
    uint8_t  press_count;        // 0-15, increments per press (5 s reset window)
    uint8_t  last_button_id;     // id of most recent press (for clean release reporting)
    uint32_t last_press_time;
};
static AdcLadder adcLadders[MAX_ADC_LADDERS];
static uint8_t   adcLadderCount = 0;

// The one SoC-specific step: put the pad in its widest input range and fix the
// reading scale, so a single set of config thresholds means the same thing on both.
//
// ESP32: 11 dB attenuation widens the usable span to roughly 0..2.5 V; analogRead is
// already 12-bit there by default.
// nRF52840: the SAADC needs no per-pad attenuation call, but the Adafruit core
// defaults analogRead to 10-bit, which would silently quarter every reading against
// thresholds written for a 12-bit part. Match the scale explicitly.
//
// UNVALIDATED ON nRF HARDWARE -- no nRF board with a ladder exists yet. The reference
// voltages still differ, so thresholds remain a per-board calibration carried in
// BinaryInputs.reserved[]; this only makes the SCALE comparable.
static void adcLadderConfigurePin(uint8_t pin) {
    // 12 dB, which is what Arduino's ADC_11db mapped to: IDF 5.x deprecated ADC_ATTEN_DB_11 in
    // favour of DB_12 -- the same setting under a name that matches the silicon.
    od_hal_adc_set_atten(pin, OD_ADC_ATTEN_12DB);
}

// Returns button index 0..num_buttons-1, or -1 when nothing is pressed.
static int classifyAdcLadder(int adc, const AdcLadder* l) {
    if (adc > (int)l->thresholds[0]) return -1;            // above idle ceiling
    for (uint8_t i = 0; i < l->num_buttons; i++) {
        if (adc > (int)l->thresholds[i + 1]) return (int)i; // thr[i+1] < adc <= thr[i]
    }
    return (int)l->num_buttons - 1;                        // catch-all bottom bucket
}

static void registerAdcLadder(const struct BinaryInputs* input) {
    if (adcLadderCount >= MAX_ADC_LADDERS) return;
    uint8_t n = input->reserved[0];                        // num_buttons
    if (n == 0 || n > MAX_LADDER_BUTTONS) {
        od_log_warn("ADC ladder: count %u out of range 1..%u on pin %u, skipping",
                    n, MAX_LADDER_BUTTONS, input->input_pin_1);
        return;
    }
    if (input->button_data_byte_index > 10) {              // index into the 11-byte MSD block
        od_log_warn("ADC ladder: byte_index %u out of range 0..10 on pin %u, skipping",
                    input->button_data_byte_index, input->input_pin_1);
        return;
    }
    if ((int)input->reserved[1] + n > 8) {                 // 3-bit id field: id_base..id_base+n-1 must be <= 7
        od_log_warn("ADC ladder: id_base %u + count %u exceeds 3-bit id space on pin %u, skipping",
                    input->reserved[1], n, input->input_pin_1);
        return;
    }
    AdcLadder* l = &adcLadders[adcLadderCount];
    l->pin = input->input_pin_1;                            // ADC GPIO
    l->num_buttons = n;
    l->id_base = input->reserved[1];
    l->byte_index = input->button_data_byte_index;
    for (uint8_t k = 0; k <= n; k++) {
        l->thresholds[k] = (uint16_t)input->reserved[2 + 2 * k] |
                           ((uint16_t)input->reserved[3 + 2 * k] << 8);
    }
    for (uint8_t k = 0; k < n; k++) {                      // contract: thresholds strictly descending
        if (l->thresholds[k] <= l->thresholds[k + 1]) {    // reject malformed config from any host
            od_log_warn("ADC ladder: thresholds not strictly descending on pin %u, skipping", input->input_pin_1);
            return;
        }
    }
    l->current_button = -1;
    l->candidate_button = -1;
    l->candidate_count = 0;
    l->press_count = 0;
    l->last_button_id = (uint8_t)(l->id_base & 0x07);
    l->last_press_time = 0;
    od_hal_gpio_config_input(l->pin, /*pull_up=*/false, /*pull_down=*/false);
    // A throwaway read, kept: it forces the channel to be configured before the attenuation is
    // applied below, which is the order this was found working in.
    (void)od_hal_adc_read(l->pin);
    adcLadderConfigurePin(l->pin);
    if (!od_hal_adc_pin_readable(l->pin)) {
        // One line at registration. Every later read returns 0, and the classifier puts 0 in
        // its catch-all bottom bucket -- so the failure presents as the last button
        // permanently pressed, not as nothing happening. See od_hal_adc.h.
        od_log_warn("ADC ladder: pin %u is not an ADC1 input -- readings will be 0", l->pin);
    }
    adcLadderCount++;
    od_log_info("ADC ladder: pin %u n %u idBase %u byteIdx %u", l->pin, n, l->id_base, l->byte_index);
}

static void pollAdcButtons() {
    if (adcLadderCount == 0) return;
    static uint32_t lastPoll = 0;
    uint32_t now = od_hal_uptime_ms();
    if (now - lastPoll < ADC_LADDER_POLL_MS) return;
    lastPoll = now;
    for (uint8_t i = 0; i < adcLadderCount; i++) {
        AdcLadder* l = &adcLadders[i];
        int adc = od_hal_adc_read(l->pin);
        int btn = classifyAdcLadder(adc, l);
        if (btn == l->candidate_button) {
            if (l->candidate_count < 255) l->candidate_count++;
        } else {
            l->candidate_button = (int8_t)btn;
            l->candidate_count = 1;
        }
        if (l->candidate_count < ADC_LADDER_DEBOUNCE) continue;  // not yet stable
        if (btn == l->current_button) continue;                 // no change
        uint8_t state;
        if (btn >= 0) {
            if (l->last_press_time == 0 || now - l->last_press_time > 5000) l->press_count = 0;
            l->press_count = (uint8_t)((l->press_count + 1) & 0x0F);
            l->last_press_time = now;
            l->last_button_id = (uint8_t)((l->id_base + btn) & 0x07);
            state = 1;
        } else {
            state = 0;  // released; last_button_id identifies which button
        }
        l->current_button = (int8_t)btn;
        uint8_t data = (uint8_t)((l->last_button_id & 0x07) |
                                 ((l->press_count & 0x0F) << 3) |
                                 ((state & 0x01) << 7));
        if (l->byte_index < 11) dynamicreturndata[l->byte_index] = data;
        updatemsdata();
        od_log_debug("ADC btn pin %u adc=%d idx=%d id=%u cnt=%u state=%u",
                    l->pin, adc, btn, l->last_button_id, l->press_count, state);
    }
}

// The BLE connect/disconnect application hooks that used to live here are gone
// as of Phase 3. Both targets now service connect and disconnect from loop():
// serviceBleEvents() does the connect-side work (rebootFlag, MSD refresh, link
// tuning) and calls requestTransferSessionCleanup(), and
// serviceBleDisconnectCleanup() owns the session teardown -- which is where the
// mid-refresh and LAN-ownership guards live. nRF used to run that teardown
// inline on the SoftDevice callback task with neither guard.

// Tear NimBLE down before esp_restart(): esp_restart() resets the CPU but NOT the
// BT controller hardware, so on the next (software-reset) boot BLEDevice::init()
// tries to enable an already-enabled controller and aborts -> PANIC boot loop that
// only a physical power cycle clears. Mirrors the deep-sleep teardown in
// enterDeepSleep() (main.cpp), which is why sleep->wake re-inits cleanly.
static void esp32_ble_deinit_before_restart() {
#ifdef OPENDISPLAY_HAS_WIFI
    // F5 (extends PR #114): esp_restart() resets the CPU but not the WiFi radio or
    // open sockets/TLS context. Tear the LAN listener + mbedTLS state down first so
    // the next boot re-inits WiFi cleanly (mirrors the BLE deinit below).
    opendisplay_lan_teardown();
#endif
    ble.stopAdvertising();
    od_hal_delay_ms(200);
    // F7: THIS is the path where a failed teardown actually costs something. esp_restart()
    // resets the CPU but NOT the controller, so if the stack is still up here, the next boot
    // re-enters od_ble_init() against a live controller, nimble_port_init() fails, and BLE is
    // dead for that entire boot with only a log line to explain it. The restart proceeds --
    // refusing to reboot on this would be worse -- but the log now names the cause in advance
    // instead of leaving the next boot's failure unexplained.
    if (ble.end()) {              // clearAll: disables + releases the BT controller
        od_log_info("BLE deinitialized before restart");
    } else {
        od_log_error("BLE teardown FAILED before restart -- the controller is still "
                     "enabled; expect nimble_port_init() to fail on the next boot");
    }
    od_hal_delay_ms(100);
}

void reboot(){
    // Banner logged by the dispatcher (commandName() in communication.cpp).
    od_hal_delay_ms(100);
    esp32_ble_deinit_before_restart();
    esp_restart();
}
/* ESP32 adapter for the shared LED runner.
 *
 * The pattern machine, the software PWM and the group/loop accounting are shared/core/od_led.c.
 * This target has no timer for it: the loop task calls processLedFlash() and the machine's
 * returned delay becomes a deadline compared against od_hal_uptime_ms().
 */

static bool s_led_running = false;
static uint32_t s_led_due_ms = 0;

static struct LedConfig* led_instance(uint8_t instance) {
    if (instance >= globalConfig.led_count) {
        return nullptr;
    }
    return &globalConfig.leds[instance];
}

extern "C" void od_led_app_write(uint8_t pin_cfg, bool level_high) {
    if (pin_cfg == 0xFF) {
        return;
    }
    od_hal_gpio_write(pin_cfg, level_high);
}

extern "C" uint8_t od_led_app_mode(uint8_t instance) {
    const struct LedConfig* led = led_instance(instance);
    return (led == nullptr) ? 0u : (uint8_t)(led->reserved[0] & 0x0F);
}

extern "C" void od_led_app_finished(uint8_t instance) {
    struct LedConfig* led = led_instance(instance);
    if (led != nullptr) {
        led->reserved[0] = 0x00;
    }
}

static void led_pump(void) {
    const uint32_t now = od_hal_uptime_ms();
    const uint32_t delay_ms = od_led_service(now);

    if (delay_ms == OD_LED_IDLE) {
        s_led_running = false;
        activeLedInstance = 0xFF;
        return;
    }
    s_led_due_ms = now + delay_ms;
}

void processLedFlash() {
    if (!s_led_running) {
        return;
    }
    /* Wrap-safe: the machine returns the remaining delay on an early call, but skipping the call
     * entirely is cheaper on a loop task that runs every few milliseconds. */
    if ((int32_t)(od_hal_uptime_ms() - s_led_due_ms) < 0) {
        return;
    }
    led_pump();
}

od_cmd_result_t handleLedActivate(const od_cmd_ctx_t *ctx, uint8_t* data, uint16_t len) {
    if (len < 1) {
        uint8_t errorResponse[] = {RESP_NACK, RESP_LED_ACTIVATE_ACK, 0x01, 0x00};
        (void)od_cmd_reply_plain(ctx, errorResponse, sizeof(errorResponse));
        return OD_CMD_NACK;
    }
    uint8_t ledInstance = data[0];
    struct LedConfig* led = led_instance(ledInstance);
    if (led == nullptr) {
        uint8_t errorResponse[] = {RESP_NACK, RESP_LED_ACTIVATE_ACK, 0x02, 0x00};
        (void)od_cmd_reply_plain(ctx, errorResponse, sizeof(errorResponse));
        return OD_CMD_NACK;
    }
    if (len >= 1 + OD_LED_PATTERN_LEN) {
        memcpy(led->reserved, data + 1, OD_LED_PATTERN_LEN);
    }

    struct od_led_pins pins;
    pins.r = led->led_1_r;
    pins.g = led->led_2_g;
    pins.b = led->led_3_b;
    pins.flags = led->led_flags;

    if (od_led_activate(ledInstance, &pins, led->reserved, od_hal_uptime_ms()) != 0) {
        /* Mode is not "run": the deployed contract answers success with the LEDs off. */
        (void)od_led_stop(0, false);
        s_led_running = false;
        activeLedInstance = 0xFF;
    } else {
        s_led_running = true;
        activeLedInstance = ledInstance;
        led_pump();
    }
    uint8_t successResponse[] = {RESP_ACK, RESP_LED_ACTIVATE_ACK, 0x00, 0x00};
    (void)od_cmd_reply(ctx, successResponse, sizeof(successResponse));
    return OD_CMD_OK;
}

void ledStopForSleep(void) {
    // Sleep API, not teardown -- see buzzerStopForSleep(). The observable result is the same as
    // the client having sent LED_STOP.
    (void)od_led_stop(0, false);
    s_led_running = false;
    activeLedInstance = 0xFF;
}

od_cmd_result_t handleLedStop(const od_cmd_ctx_t *ctx, uint8_t* data, uint16_t len) {
    const bool instance_given = (len >= 1);
    const uint8_t instance = instance_given ? data[0] : 0u;

    if (od_led_stop(instance, instance_given) != 0) {
        uint8_t errorResponse[] = {RESP_NACK, RESP_LED_STOP_ACK, 0x02, 0x00};
        (void)od_cmd_reply_plain(ctx, errorResponse, sizeof(errorResponse));
        return OD_CMD_NACK;
    }
    s_led_running = false;
    activeLedInstance = 0xFF;
    uint8_t successResponse[] = {RESP_ACK, RESP_LED_STOP_ACK, 0x00, 0x00};
    (void)od_cmd_reply(ctx, successResponse, sizeof(successResponse));
    return OD_CMD_OK;
}


void processButtonEvents() {
    powerButtonPoll();
    pollConfiguredPowerOffButtons();   // no-op unless the board declares a latch
    pollAdcButtons();                  // no-op off ESP32 (see the ADC ladder guard)
    if (buttonEventPending) {
        od_hal_gpio_irq_lock();
        buttonEventPending = false;
        uint8_t changedButtonIndex = lastChangedButtonIndex;
        lastChangedButtonIndex = 0xFF;
        od_hal_gpio_irq_unlock();
        od_log_debug("Button event pending: %u", changedButtonIndex);
        if (changedButtonIndex < MAX_BUTTONS && buttonStates[changedButtonIndex].initialized) {
            ButtonState* btn = &buttonStates[changedButtonIndex];
            bool pinState = (od_hal_gpio_read(btn->pin) != 0);
            bool logicalPressed = btn->inverted ? !pinState : pinState;
            od_log_debug("Pin state: %d, Logical pressed: %d, inverted: %d", pinState, logicalPressed, btn->inverted);
            uint8_t logicalState = logicalPressed ? 1 : 0;
            btn->current_state = logicalState;
            od_log_debug("Button: %u, Press count: %u, Current state: %u", btn->button_id, btn->press_count, btn->current_state);
            uint8_t buttonData = (btn->button_id & 0x07) |
                                 ((btn->press_count & 0x0F) << 3) |
                                 ((btn->current_state & 0x01) << 7);
            if (btn->byte_index < 11) {
                dynamicreturndata[btn->byte_index] = buttonData;
            }
        }
        // ORDER IS LOAD-BEARING on a target with advertising-interval states: the publish
        // chooses the interval there, so a boost requested after it lands too late to affect the
        // packet it exists for -- the press goes out slow and only the release goes out boosted,
        // so a host sees "not pressed" reliably and "pressed" almost never. THAT IS AN nRF
        // FAILURE, not this target's: nothing here selects among interval states, and
        // od_adv_app_boost() is a no-op. The order costs nothing and is kept so the sequence
        // reads correctly wherever this code is.
        od_adv_app_boost();
        updatemsdata();
    }
}

/* One-shot diagnostic blink on the selected instance -- boot patterns, not a LED_ACTIVATE run.
 * The ramp is the shared one; only instance selection is ESP32's. */
void flashLed(uint8_t color, uint8_t brightness) {
    if (activeLedInstance == 0xFF) {
        for (uint8_t i = 0; i < globalConfig.led_count; i++) {
            if (globalConfig.leds[i].led_type == 1) {
                activeLedInstance = i;
                break;
            }
        }
        if (activeLedInstance == 0xFF) return;
    }
    const struct LedConfig* led = led_instance(activeLedInstance);
    if (led == nullptr) return;

    struct od_led_pins pins;
    pins.r = led->led_1_r;
    pins.g = led->led_2_g;
    pins.b = led->led_3_b;
    pins.flags = led->led_flags;
    od_led_flash_once(&pins, color, brightness);
}

void IRAM_ATTR handleButtonISR(uint8_t buttonIndex) {
    if (buttonIndex >= MAX_BUTTONS || !buttonStates[buttonIndex].initialized) return;
    ButtonState* btn = &buttonStates[buttonIndex];
    bool pinState = (od_hal_gpio_read(btn->pin) != 0);
    bool pressed = btn->inverted ? !pinState : pinState;
    uint8_t newState = pressed ? 1 : 0;
    if (newState != btn->current_state) {
        btn->current_state = newState;
        lastChangedButtonIndex = buttonIndex;
        if (pressed) btn->press_count = (btn->press_count + 1) & 0x0F;
        buttonEventPending = true;
    }
}

void IRAM_ATTR buttonISR(void* arg) {
    uint8_t buttonIndex = (uint8_t)(uintptr_t)arg;
    handleButtonISR(buttonIndex);
}

void initButtons() {
    od_log_info("=== Initializing Buttons ===");
    buttonStateCount = 0;
    for (uint8_t i = 0; i < MAX_BUTTONS; i++) {
        buttonStates[i].initialized = false;
        buttonStates[i].button_id = 0;
        buttonStates[i].press_count = 0;
        buttonStates[i].current_state = 0;
        buttonStates[i].byte_index = 0xFF;
        buttonStates[i].pin = 0xFF;
        buttonStates[i].instance_index = 0xFF;
        buttonStates[i].power_off = false;
        buttonStates[i].power_off_hold_ms = 0;
    }
    adcLadderCount = 0;
    if (globalConfig.binary_input_count == 0) return;
    for (uint8_t instanceIdx = 0; instanceIdx < globalConfig.binary_input_count; instanceIdx++) {
        struct BinaryInputs* input = &globalConfig.binary_inputs[instanceIdx];
        // The `continue` is the load-bearing half: a ladder input that falls through
        // to the digital path below gets a CHANGE interrupt attached to the ladder
        // pin, which is worse than the feature simply not working.
        if (input->input_type == BINARY_INPUT_TYPE_ADC_LADDER) {
            registerAdcLadder(input);
            continue;
        }
        if (input->input_type != OD_INPUT_TYPE_BUTTON) continue;
        if (input->button_data_byte_index > 10) continue;
        uint16_t instanceHoldMs = (input->power_off_hold_sec == 0) ? 3000u : (uint16_t)input->power_off_hold_sec * 1000u;
        uint8_t* instancePins[8] = {
            &input->input_pin_1,&input->input_pin_2,&input->input_pin_3,&input->input_pin_4,
            &input->input_pin_5,&input->input_pin_6,&input->input_pin_7,&input->input_pin_8
        };
        for (uint8_t pinIdx = 0; pinIdx < 8; pinIdx++) {
            if (input->pins_used != 0 && (input->pins_used & (1 << pinIdx)) == 0) {
                continue;
            }
            uint8_t pin = *instancePins[pinIdx];
            if (pin == 0xFF) continue;
            if (touch_input_gpio_is_touch_int(pin)) {
                od_log_debug("Button: skip pin %u (reserved for GT911 INT)", pin);
                continue;
            }
            if (buttonStateCount >= MAX_BUTTONS) break;
            ButtonState* btn = &buttonStates[buttonStateCount];
            btn->button_id = (input->instance_number * 8) + pinIdx;
            if (btn->button_id > 7) btn->button_id = btn->button_id % 8;
            btn->byte_index = input->button_data_byte_index;
            btn->pin = pin;
            btn->instance_index = instanceIdx;
            btn->press_count = 0;
            btn->pin_offset = pinIdx;
            btn->inverted = (input->invert & (1 << pinIdx)) != 0;
            btn->power_off = (input->power_off_flags & (1 << pinIdx)) != 0;
            btn->power_off_hold_ms = instanceHoldMs;
            bool hasPullup = (input->pullups & (1 << pinIdx)) != 0;
            bool hasPulldown = (input->pulldowns & (1 << pinIdx)) != 0;
            // One call where Arduino needed two: pinMode(INPUT) followed by a conditional
            // re-pinMode with the pull. od_hal_gpio_config_input() takes both pulls, and
            // asking for neither is the plain-INPUT case, so the intermediate configuration
            // that existed only because Arduino had no combined form is gone. The pad ends in
            // the same state.
            od_hal_gpio_config_input(pin, hasPullup, hasPulldown);
            od_hal_delay_ms(10);
            bool initialPinState = (od_hal_gpio_read(pin) != 0);
            bool initialPressed = btn->inverted ? !initialPinState : initialPinState;
            btn->current_state = initialPressed ? 1 : 0;
            od_hal_gpio_config_irq_arg(pin, OD_GPIO_EDGE_BOTH, buttonISR,
                                       (void*)(uintptr_t)buttonStateCount);
            btn->initialized = true;
            buttonStateCount++;
        }
    }
    if (buttonStateCount > 0) {
        for (uint8_t i = 0; i < buttonStateCount; i++) {
            if (buttonStates[i].initialized) {
                od_hal_gpio_irq_disable(buttonStates[i].pin);
            }
        }
        od_hal_delay_ms(50);
        buttonEventPending = false;
        lastChangedButtonIndex = 0xFF;
        for (uint8_t i = 0; i < buttonStateCount; i++) {
            if (!buttonStates[i].initialized) continue;
            ButtonState* btn = &buttonStates[i];
            bool pinState = (od_hal_gpio_read(btn->pin) != 0);
            bool initialPressed = btn->inverted ? !pinState : pinState;
            btn->current_state = initialPressed ? 1 : 0;
            btn->press_count = 0;
        }
        for (uint8_t i = 0; i < buttonStateCount; i++) {
            if (buttonStates[i].initialized) {
                od_hal_gpio_irq_enable(buttonStates[i].pin);
            }
        }
    }
}

void enterDFUMode() {
    // Banner logged by the dispatcher (commandName() in communication.cpp).


    od_log_info("ENTER_DFU: unlocking SMP OTA");
    od_smp_unlock();
}

od_cmd_result_t handleDeepSleepCommand(const od_cmd_ctx_t *ctx, const uint8_t* payload, uint16_t payloadLen) {
    // Banner logged by the dispatcher (commandName() in communication.cpp).
    // Optional 2-byte big-endian seconds payload overrides the configured
    // deep-sleep duration for exactly one cycle. 0x0000 = explicit no-override.
    uint16_t overrideSeconds = 0;
    if (payloadLen >= 2) {
        overrideSeconds = ((uint16_t)payload[0] << 8) | payload[1];
        // Bytes beyond 2 ignored for forward compatibility.
    } else if (payloadLen == 1) {
        od_log_warn("Malformed 0x%04X payload length 1 -- ignoring", CMD_DEEP_SLEEP);
    }
    // Enforce a 60 s floor on host overrides: a very short wake timer risks a rapid
    // sleep/wake churn that never stays awake long enough to service a client. This
    // applies to the OVERRIDE only; overrideSeconds == 0 means "no override" and defers
    // to the configured deep_sleep_time_seconds, which is not subject to this floor.
    constexpr uint16_t MIN_DEEP_SLEEP_OVERRIDE_SECONDS = 60;
    if (overrideSeconds != 0 && overrideSeconds < MIN_DEEP_SLEEP_OVERRIDE_SECONDS) {
        od_log_warn("Override %us below %us floor - clamping", overrideSeconds, MIN_DEEP_SLEEP_OVERRIDE_SECONDS);
        overrideSeconds = MIN_DEEP_SLEEP_OVERRIDE_SECONDS;
    }
    if (globalConfig.power_option.power_mode != 1) {
        od_log_warn("Device not battery powered - 0x%04X rejected", CMD_DEEP_SLEEP);
        uint8_t errorResponse[] = {RESP_NACK, RESP_DEEP_SLEEP, OD_ERR_DEEP_SLEEP_NOT_BATTERY, 0x00};
        (void)od_cmd_reply_plain(ctx, errorResponse, sizeof(errorResponse));
        return OD_CMD_NACK;
    }
    if (globalConfig.power_option.deep_sleep_time_seconds == 0) {
        od_log_warn("Deep sleep disabled in config - 0x%04X rejected", CMD_DEEP_SLEEP);
        uint8_t errorResponse[] = {RESP_NACK, RESP_DEEP_SLEEP, OD_ERR_DEEP_SLEEP_DISABLED, 0x00};
        (void)od_cmd_reply_plain(ctx, errorResponse, sizeof(errorResponse));
        return OD_CMD_NACK;
    }
    /* No reply on success, deliberately: the device enters deep sleep and the link drops. The
     * reservation goes unused and the caller releases it. */
    enterDeepSleep(true, overrideSeconds);
    return OD_CMD_OK;
}

od_cmd_result_t handlePowerOffCommand(const od_cmd_ctx_t *ctx, const uint8_t* payload, uint16_t payloadLen) {
    // Banner logged by the dispatcher (commandName() in communication.cpp).
    // CMD_POWER_OFF request is bare [0x00][0x52]; any trailing payload is RESERVED
    // and ignored (unlike CMD_DEEP_SLEEP 0x0053, this has no duration payload).
    (void)payload;
    (void)payloadLen;
    // powerLatchDffConfigured() returns false on a board with no D-FF latch, which
    // falls through to the NACK below. Callers need no guard of their own.
    if (powerLatchDffConfigured()) {
        // Fire-and-forget hard rail-cut: queue the ACK, then release the D-FF latch.
        // On latch HW the rail usually drops before the ACK is actually transmitted.
        /* THE FLUSH IS NOT OPTIONAL, and it is the only reply site where that is true. Every other
         * queued response is drained by the loop; after powerLatchPowerOff() there is no next
         * pass. Without this the ack is queued and then the rail is cut under it -- which on LAN
         * is a regression from delivered to never sent, because the shipped sender wrote LAN
         * replies straight to the socket rather than through a queue.
         *
         * Bounded, and the 100 ms is now the deadline rather than a bare sleep: on latch hardware
         * the rail usually drops before a BLE notification is really on air anyway, so this buys
         * the ack a chance, it does not promise delivery. */
        uint8_t ok[] = {RESP_ACK, RESP_POWER_OFF, 0x00, 0x00};
        (void)od_cmd_reply(ctx, ok, sizeof(ok));
        {
            const uint32_t deadline = od_hal_uptime_ms() + 100u;
            while (od_txq_flush(od_hal_uptime_ms(), deadline) == OD_TXQ_BUSY) {
                od_hal_delay_ms(5);
            }
        }
        powerLatchPowerOff();
        return OD_CMD_OK;
    }
    // ANCHOR(power-off-no-latch-fallback): FUTURE WORK for a later agent/implementer.
    // On non-latch BATTERY targets, implement "enter deep sleep with NO wake timer"
    // (sleep indefinitely; wake only on button/reset) as the closest equivalent to a
    // hard power-off, instead of the unsupported NACK below. Caveats before doing so:
    //   1. This DEVIATES from the current protocol, which mandates
    //      OD_ERR_POWER_OFF_UNSUPPORTED (0x00) for non-latch targets — update the
    //      0x0052 @response contract in opendisplay-protocol/src/opendisplay_protocol.h
    //      FIRST, then propagate the header and implement here.
    //   2. enterDeepSleep(true, 0) currently means "use the configured duration", NOT
    //      "no timer" — a genuinely timer-less deep-sleep entry path must be added.
    //   3. Mains-powered targets (power_option.power_mode != 1) must STILL NACK — never
    //      sleep a device that cannot self-repower.
    // Until that lands: capability-gated NACK. Scope: OD_ERR_POWER_OFF_* only — do NOT
    // conflate with 0x53 deep sleep (a device that refuses 0x52 may still accept 0x53).
    od_log_warn("No power latch on this target - 0x%04X rejected", CMD_POWER_OFF);
    uint8_t errorResponse[] = {RESP_NACK, RESP_POWER_OFF, OD_ERR_POWER_OFF_UNSUPPORTED, 0x00};
    (void)od_cmd_reply_plain(ctx, errorResponse, sizeof(errorResponse));
    return OD_CMD_NACK;
}
