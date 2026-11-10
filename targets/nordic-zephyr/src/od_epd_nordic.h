/* The wire half of od_epd_cfg on Nordic: D/C, CS, RST, BUSY and the panel SPI bus.
 *
 * Used by CMD_DRIVE_WRITE_START and CMD_SPI_RAW. Ordinary uploads and the boot screen stay on
 * bb_epaper, so a device without drive packets behaves exactly as before.
 */
#ifndef OD_EPD_NORDIC_H
#define OD_EPD_NORDIC_H

#include "od_epd_cfg.h"

#ifdef __cplusplus
extern "C" {
#endif

struct DisplayConfig;

const od_epd_cfg_bus_t *od_epd_nordic_bus(void);
/* Claims D/C, CS, RST and BUSY, pulling BUSY toward the loaded panel's idle level. The SPI bus
 * and the rail are the caller's. */
void od_epd_nordic_claim_pins(const struct DisplayConfig *d);

#ifdef __cplusplus
}
#endif

#endif /* OD_EPD_NORDIC_H */
