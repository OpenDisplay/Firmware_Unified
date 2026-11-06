/* od_flash_probe -- is there an SPI NOR on the configured pins, and what is it?
 *
 * DIAGNOSTIC, not the storage driver. It answers one question -- does the chip respond with a
 * plausible JEDEC id -- before anything is built on the assumption that it does. The real
 * driver belongs behind od_hal_spi once this says yes.
 *
 * BIT-BANGED ON PURPOSE. The panel already owns a USART on this board, and a probe that had to
 * negotiate peripheral ownership could fail for reasons that have nothing to do with whether a
 * flash chip is present -- which is the one thing it exists to find out. Speed is irrelevant:
 * this reads six bytes, once, at boot.
 *
 * MISO IS reserved[0] OF FlashConfig, not a named field. Canonical stops at `mode`; the
 * toolbox writes miso/wp/hold into the first three reserved bytes (wire offsets 12/13/14).
 * Reading it by index states what it actually is rather than implying a field exists.
 *
 * THE WAKE SEQUENCE IS NOT OPTIONAL. OpenDisplay has only ever sent 0xB9 (deep power-down) at
 * sleep and never 0xAB to release it, so a chip that has been slept once ignores every command
 * until released -- every read returns 0xFF and the part looks absent. A CS pulse first covers
 * MX25 parts that wake on CS rather than on 0xAB.
 */
#include "od_flash_probe.h"

#include "em_cmu.h"
#include "em_gpio.h"
#include "opendisplay_runtime.h"
#include "sl_udelay.h"

#define PIN_UNUSED 0xFFu

static od_flash_probe_result_t s_result;

static bool decode(uint8_t v, GPIO_Port_TypeDef *port, uint8_t *pin)
{
    if (v == PIN_UNUSED) {
        return false;
    }
    const unsigned pr = (unsigned)(v >> 4) & 0x0Fu;
    const unsigned pn = (unsigned)(v & 0x0Fu);
    if (pr > (unsigned)GPIO_PORT_MAX || pn > 15u) {
        return false;
    }
    *port = (GPIO_Port_TypeDef)(gpioPortA + pr);
    *pin = (uint8_t)pn;
    return true;
}

struct bus {
    GPIO_Port_TypeDef mosi_p, miso_p, sck_p, cs_p;
    uint8_t mosi, miso, sck, cs;
};

/* SPI mode 0: data is set while SCK is low and sampled on the rising edge. */
static uint8_t xfer_byte(const struct bus *b, uint8_t out)
{
    uint8_t in = 0u;

    for (int i = 7; i >= 0; i--) {
        if ((out >> i) & 1u) {
            GPIO_PinOutSet(b->mosi_p, b->mosi);
        } else {
            GPIO_PinOutClear(b->mosi_p, b->mosi);
        }
        sl_udelay_wait(1u);
        GPIO_PinOutSet(b->sck_p, b->sck);
        in = (uint8_t)((in << 1) | (GPIO_PinInGet(b->miso_p, b->miso) ? 1u : 0u));
        sl_udelay_wait(1u);
        GPIO_PinOutClear(b->sck_p, b->sck);
    }
    return in;
}

static void cs_low(const struct bus *b)  { GPIO_PinOutClear(b->cs_p, b->cs); }
static void cs_high(const struct bus *b) { GPIO_PinOutSet(b->cs_p, b->cs); }

const od_flash_probe_result_t *od_flash_probe_run(const struct od_config *cfg)
{
    struct bus b;
    const struct FlashConfig *fc = NULL;

    s_result = (od_flash_probe_result_t){ 0 };
    s_result.status = OD_FLASH_PROBE_NO_CONFIG;

    if (cfg == NULL) {
        return &s_result;
    }
    for (uint8_t i = 0; i < cfg->flash_config_count; i++) {
        if ((cfg->flash_configs[i].flags & OD_FLASH_FLAG_ENABLED) != 0u) {
            fc = &cfg->flash_configs[i];
            break;
        }
    }
    if (fc == NULL) {
        return &s_result;
    }

    s_result.status = OD_FLASH_PROBE_BAD_PINS;
    if (!decode(fc->mosi_pin, &b.mosi_p, &b.mosi) ||
        !decode(fc->sck_pin, &b.sck_p, &b.sck) ||
        !decode(fc->cs_pin, &b.cs_p, &b.cs)) {
        return &s_result;
    }
    /* No MISO means no read path: the JEDEC id, the status register and every verification
     * this feature would rest on are all reads. Say so distinctly rather than reporting a
     * chip that failed to answer. */
    if (!decode(fc->reserved[0], &b.miso_p, &b.miso)) {
        s_result.status = OD_FLASH_PROBE_NO_MISO;
        return &s_result;
    }

    CMU_ClockEnable(cmuClock_GPIO, true);
    GPIO_PinModeSet(b.cs_p, b.cs, gpioModePushPull, 1);
    GPIO_PinModeSet(b.sck_p, b.sck, gpioModePushPull, 0);
    GPIO_PinModeSet(b.mosi_p, b.mosi, gpioModePushPull, 0);
    /* INPUT ONLY. If this pin is not actually wired to the chip it floats and reads garbage;
     * it is never driven, so a wrong guess here cannot contend with whatever owns that pad. */
    GPIO_PinModeSet(b.miso_p, b.miso, gpioModeInput, 0);

    /* Wake: CS pulse for MX25-style parts, then the standard 0xAB release. */
    cs_low(&b);
    sl_udelay_wait(20u);
    cs_high(&b);
    sl_udelay_wait(35u);
    cs_low(&b);
    (void)xfer_byte(&b, 0xABu);
    cs_high(&b);
    sl_udelay_wait(50u);

    cs_low(&b);
    (void)xfer_byte(&b, 0x9Fu);
    s_result.manufacturer = xfer_byte(&b, 0x00u);
    s_result.mem_type     = xfer_byte(&b, 0x00u);
    s_result.capacity     = xfer_byte(&b, 0x00u);
    cs_high(&b);

    cs_low(&b);
    (void)xfer_byte(&b, 0x05u);
    s_result.status_reg = xfer_byte(&b, 0x00u);
    cs_high(&b);

    /* 0x00 and 0xFF are what a floating or dead bus reads as, in either direction. Neither is
     * a valid JEDEC manufacturer, so treating them as "no answer" keeps a disconnected pin
     * from being reported as a chip. */
    if (s_result.manufacturer == 0x00u || s_result.manufacturer == 0xFFu) {
        s_result.status = OD_FLASH_PROBE_NO_ANSWER;
    } else {
        s_result.status = OD_FLASH_PROBE_OK;
    }
    return &s_result;
}

const od_flash_probe_result_t *od_flash_probe_last(void)
{
    return &s_result;
}
