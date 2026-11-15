/* Target seam for the shared CMD_SPI_RAW handler (shared/core/od_cmd_spi_raw.c).
 *
 * Every function returns 0 or an OD_ERR_SPI_RAW_* code negated. The session persists across
 * commands until CLOSE, a re-OPEN, or the target's transfer abort path (a BLE disconnect)
 * tears it down.
 */
#ifndef OD_SPI_RAW_APP_H
#define OD_SPI_RAW_APP_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

int  opendisplay_display_spi_raw_open(bool run_panel_init);
int  opendisplay_display_spi_raw_cmd(const uint8_t *data, uint16_t len);
int  opendisplay_display_spi_raw_data(const uint8_t *data, uint16_t len);
int  opendisplay_display_spi_raw_busy(uint32_t timeout_ms, uint16_t *elapsed_ms_out,
                                      bool *still_busy_out);
int  opendisplay_display_spi_raw_reset(uint8_t hold_ms);

/* GPIO sub-opcode (provisional, docs/PROTOCOL_CHANGES_3.0.md): set one pin's mode, then read
 * its level. Routed only where OD_CAP_SPI_RAW_GPIO is set; elsewhere it gets UNKNOWN_SUB.
 *   request  [0x00][0x84][0x07][pin:1][mode:1]
 *   response [0x00][0x84][0x07][pin:1][level:1]
 * The pin byte is the target's config pin numbering. A mode change holds until the firmware
 * next drives that pin itself (a panel power-up or power-down, a config reload), a reboot, or
 * a sleep state that does not keep the GPIO registers (Telink's deep retention).
 * Works with or without an open session, so a pin can be held while the panel rail is off.
 * A target refuses pins that would cut its own debug or boot path with BAD_ARGS. */
#ifndef SPI_RAW_SUB_GPIO
#define SPI_RAW_SUB_GPIO 0x07u
#endif

enum od_spi_raw_gpio_mode {
    OD_SPI_RAW_GPIO_READ = 0x00,        /* leave the pin as it is; just read it */
    OD_SPI_RAW_GPIO_IN_FLOAT = 0x01,
    OD_SPI_RAW_GPIO_IN_PULLUP = 0x02,
    OD_SPI_RAW_GPIO_IN_PULLDOWN = 0x03,
    OD_SPI_RAW_GPIO_OUT_LOW = 0x04,
    OD_SPI_RAW_GPIO_OUT_HIGH = 0x05
};

int  opendisplay_display_spi_raw_gpio(uint8_t pin, uint8_t mode, uint8_t *level_out);
void opendisplay_display_spi_raw_close(void);
bool opendisplay_display_spi_raw_is_open(void);

#ifdef __cplusplus
}
#endif

#endif /* OD_SPI_RAW_APP_H */
