/* Minimal em_gpio surface for host tests that compile production BG22 sources.
 * Pin writes are recorded, not performed; see fake_silabs_gpio_writes. */
#ifndef OD_TEST_FAKE_SILABS_EM_GPIO_H
#define OD_TEST_FAKE_SILABS_EM_GPIO_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    gpioPortA = 0,
    gpioPortB = 1,
    gpioPortC = 2,
    gpioPortD = 3
} GPIO_Port_TypeDef;

#define GPIO_PORT_MAX 3

typedef enum {
    gpioModeDisabled = 0,
    gpioModePushPull = 1,
    gpioModeInputPull = 2,
    gpioModeWiredAndFilter = 3
} GPIO_Mode_TypeDef;

void GPIO_PinModeSet(GPIO_Port_TypeDef port, uint8_t pin, GPIO_Mode_TypeDef mode, unsigned out);
void GPIO_PinOutSet(GPIO_Port_TypeDef port, uint8_t pin);
void GPIO_PinOutClear(GPIO_Port_TypeDef port, uint8_t pin);
unsigned GPIO_PinInGet(GPIO_Port_TypeDef port, uint8_t pin);

/* ---- edge trace, for the BG22 I2C transport suite ----
 *
 * The transport is bit-banged GPIO, so the ONLY way to check it is to watch the edges. Anything
 * above this -- the NFC adapter's read/write helpers -- says nothing about START placement,
 * per-byte ACK sampling or the NACK on a final read byte.
 */
enum gpio_edge {
    GPIO_EDGE_SCL_HIGH = 1,
    GPIO_EDGE_SCL_LOW,
    GPIO_EDGE_SDA_HIGH,
    GPIO_EDGE_SDA_LOW,
    GPIO_EDGE_SDA_INPUT,     /* released to read: ACK sampling or an incoming bit */
    GPIO_EDGE_SDA_OUTPUT,
};

#define GPIO_TRACE_MAX 4096u
extern enum gpio_edge gpio_trace[GPIO_TRACE_MAX];
extern unsigned       gpio_trace_len;

/* Which pins count as SCL and SDA for the trace. */
void fake_gpio_reset(GPIO_Port_TypeDef scl_port, uint8_t scl_pin,
                     GPIO_Port_TypeDef sda_port, uint8_t sda_pin);

/* What SDA reads back while released. Bits are consumed most-significant first, and a 0 at an
 * ACK slot is an ACK. */
void fake_gpio_set_sda_reads(const uint8_t *bits, unsigned count);

/* Total microseconds passed to sl_udelay_wait(), so the suite can prove the pacing was not
 * silently dropped. */
extern unsigned long fake_udelay_total_us;

#ifdef __cplusplus
}
#endif

#endif /* OD_TEST_FAKE_SILABS_EM_GPIO_H */
