/* SDK-side implementation of tlsr_port.h: timer, flash, AES engine, noise, GPIO, breadcrumbs. */
#include "tl_common.h"
#include "drivers.h"
#include "stack/ble/ble.h"

#include "tlsr_port.h"

uint32_t tlsr_port_ticks(void)
{
    return clock_time();
}

void tlsr_port_delay_us(uint32_t us)
{
    sleep_us(us);
}

void tlsr_port_flash_read(uint32_t addr, uint32_t len, uint8_t *buf)
{
    flash_read_page(addr, len, buf);
}

void tlsr_port_flash_program(uint32_t addr, uint32_t len, const uint8_t *buf)
{
    flash_write_page(addr, len, (u8 *)buf);
}

void tlsr_port_flash_erase_4k(uint32_t addr)
{
    flash_erase_sector(addr);
}

void tlsr_port_aes_block(const uint8_t key[16], const uint8_t in[16], uint8_t out[16])
{
    u8 r = irq_disable();
    aes_encrypt((u8 *)key, (u8 *)in, out);
    irq_restore(r);
}

uint32_t tlsr_port_noise32(void)
{
    return rand();
}

void tlsr_port_reboot(void)
{
    analog_write(0x3b, 0u);            /* a requested reboot is not a hang: clear the breadcrumb */
    start_reboot();
}

static GPIO_PinTypeDef pin_of(uint8_t pin)
{
    return (GPIO_PinTypeDef)(((u32)(pin >> 3) << 8) | BIT(pin & 7u));
}

static bool pin_valid(uint8_t pin)
{
    return pin != TLSR_PORT_PIN_NONE && (pin >> 3) <= 4u;
}

void tlsr_port_gpio_output(uint8_t pin, bool level)
{
    if (!pin_valid(pin)) return;
    gpio_set_func(pin_of(pin), AS_GPIO);
    gpio_write(pin_of(pin), level);
    gpio_set_input_en(pin_of(pin), 0);
    gpio_set_output_en(pin_of(pin), 1);
}

void tlsr_port_gpio_input(uint8_t pin, uint8_t pull)
{
    if (!pin_valid(pin)) return;
    gpio_set_func(pin_of(pin), AS_GPIO);
    gpio_set_output_en(pin_of(pin), 0);
    gpio_set_input_en(pin_of(pin), 1);
    gpio_setup_up_down_resistor(pin_of(pin), pull == TLSR_PORT_PULL_UP   ? PM_PIN_PULLUP_10K
                                           : pull == TLSR_PORT_PULL_DOWN ? PM_PIN_PULLDOWN_100K
                                                                         : PM_PIN_UP_DOWN_FLOAT);
}

void tlsr_port_gpio_write(uint8_t pin, bool level)
{
    if (!pin_valid(pin)) return;
    gpio_write(pin_of(pin), level);
}

bool tlsr_port_gpio_read(uint8_t pin)
{
    return pin_valid(pin) && gpio_read(pin_of(pin)) != 0;
}

void tlsr_port_gpio_release(uint8_t pin)
{
    if (!pin_valid(pin)) return;
    gpio_set_output_en(pin_of(pin), 0);
    gpio_set_input_en(pin_of(pin), 0);
    gpio_setup_up_down_resistor(pin_of(pin), PM_PIN_UP_DOWN_FLOAT);
}

_attribute_ram_code_ void tlsr_port_spi_out(uint8_t mosi, uint8_t sclk, const uint8_t *buf, uint32_t n)
{
    GPIO_PinTypeDef pm = pin_of(mosi), pc = pin_of(sclk);
    volatile u8 *om = &reg_gpio_out(pm);
    volatile u8 *oc = &reg_gpio_out(pc);
    u8 bm = (u8)(pm & 0xff), bc = (u8)(pc & 0xff);
    u8 i;

    if (!pin_valid(mosi) || !pin_valid(sclk)) return;
    if (om == oc) {
        while (n-- != 0u) {
            u8 b = *buf++;
            /* Re-read once per byte, so a pin on the same port changed meanwhile is kept. */
            u8 base = (u8)(*om & (u8)~(bm | bc));
            for (i = 0; i < 8u; i++) {
                u8 v = (b & 0x80u) ? (u8)(base | bm) : base;
                *om = v;                   /* clock low, data out */
                *om = (u8)(v | bc);        /* rising edge: sampled */
                b = (u8)(b << 1);
            }
            *om = base;
        }
        return;
    }
    while (n-- != 0u) {
        u8 b = *buf++;
        for (i = 0; i < 8u; i++) {
            if (b & 0x80u) *om |= bm; else *om &= (u8)~bm;
            *oc |= bc;
            *oc &= (u8)~bc;
            b = (u8)(b << 1);
        }
    }
}

void tlsr_port_led_park(uint8_t pin)
{
    if (pin != TLSR_PORT_PIN_SWS) {
        tlsr_port_gpio_release(pin);
        return;
    }
    gpio_set_output_en(GPIO_PA7, 0);
    gpio_write(GPIO_PA7, 0);
    gpio_set_func(GPIO_PA7, AS_SWIRE);
    gpio_set_input_en(GPIO_PA7, 1);
    gpio_setup_up_down_resistor(GPIO_PA7, PM_PIN_PULLUP_1M);
}

void tlsr_port_service_stack(void)
{
    wd_clear();                        /* long waits (panel refresh) must not trip the watchdog */
    blt_sdk_main_loop();
}

/* DEEP_ANA_REG1/2 (0x3b/0x3c): reset only by a power cycle, so they survive the watchdog. */
#define CRUMB_STEP_REG   0x3b
#define CRUMB_RESET_REG  0x3c

void tlsr_port_crumb(uint8_t step)
{
    analog_write(CRUMB_STEP_REG, step);
}

uint8_t tlsr_port_crumb_boot(uint8_t *resets)
{
    uint8_t last = analog_read(CRUMB_STEP_REG);
    uint8_t count = analog_read(CRUMB_RESET_REG);

    if (last != 0u && count != 0xFFu) {
        count++;
        analog_write(CRUMB_RESET_REG, count);
    }
    analog_write(CRUMB_STEP_REG, 0u);
    *resets = count;
    return last;
}

uint16_t tlsr_port_battery_mv(uint8_t pin)
{
    unsigned int mv;
    uint8_t port = pin >> 3, bit = pin & 7u;

    if (!pin_valid(pin) || !((port == 1u) || (port == 2u && (bit == 4u || bit == 5u)))) {
        return 0u;
    }
    adc_init();
    adc_vbat_init(pin_of(pin));
    adc_power_on_sar_adc(1);
    mv = adc_sample_and_get_result();
    adc_power_on_sar_adc(0);
    tlsr_port_gpio_release(pin);
    return (uint16_t)(mv > 0xFFFFu ? 0xFFFFu : mv);
}
