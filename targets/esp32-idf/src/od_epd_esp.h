/* od_epd_esp -- the ESP32 panel bus under od_epd_cfg: SPI2 for the bytes, GPIO for D/C, CS, RST
 * and BUSY. No panel knowledge: every command comes from the drive packets.
 *
 * One owner of SPI2_HOST for SPI panels. open() (re)initialises the bus every time, because the
 * power-down path parks SCLK/MOSI as plain GPIO and only spi_bus_initialize() routes them back to
 * the peripheral. close() releases it; both are idempotent. */
#ifndef OD_EPD_ESP_H
#define OD_EPD_ESP_H

#include "od_epd_cfg.h"

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

struct DisplayConfig;

const od_epd_cfg_bus_t *od_epd_esp_bus(void);
/* Bus up and pins claimed: D/C, CS, CS2 (when distinct), RST high, BUSY pulled toward
 * od_epd_cfg_busy_idle_level(). False leaves the bus down. */
bool od_epd_esp_open(const struct DisplayConfig *d);
void od_epd_esp_close(void);
bool od_epd_esp_is_open(void);
/* True once a transfer failed since the last open. */
bool od_epd_esp_faulted(void);

#ifdef __cplusplus
}
#endif

#endif /* OD_EPD_ESP_H */
