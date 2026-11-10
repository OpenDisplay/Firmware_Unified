/* The wire half of od_epd_cfg on Nordic: D/C, CS (and cs_pin_2), RST, BUSY, sequence-driven
 * pins and the panel SPI bus. Every display path (od_display, CMD_SPI_RAW) drives the panel
 * through it.
 */
#ifndef OD_EPD_NORDIC_H
#define OD_EPD_NORDIC_H

#include "od_epd_cfg.h"

#ifdef __cplusplus
extern "C" {
#endif

struct DisplayConfig;

const od_epd_cfg_bus_t *od_epd_nordic_bus(void);
/* Claims D/C, both CS lines, RST and BUSY, pulling BUSY toward the loaded panel's idle level.
 * The SPI bus and the rail are the caller's. */
void od_epd_nordic_claim_pins(const struct DisplayConfig *d);

#ifdef __cplusplus
}
#endif

#endif /* OD_EPD_NORDIC_H */
