/* od_epd on Nordic: pins, SPI and config-backed reads for the config-driven panel driver.
 *
 * Used only by CMD_DRIVE_WRITE_START. Ordinary uploads and the boot screen stay on bb_epaper, so
 * a device without drive packets behaves exactly as before.
 */
#ifndef OD_EPD_NORDIC_H
#define OD_EPD_NORDIC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct DisplayConfig;

typedef struct {
    uint8_t passes;
    uint8_t color_scheme;   /* 0xFF = the panel's own */
    int8_t  temp_min_c;
    int8_t  temp_max_c;
    uint8_t temp_policy;
} od_epd_nordic_mode_t;

/* Indexes the stored config and loads the panel descriptor, so call it once per drive session:
 * index entries are offsets and a config write invalidates them. False when the config carries
 * no drive packets for this display. */
bool    od_epd_nordic_load(const struct DisplayConfig *d);
/* Claims D/C, CS, RST and BUSY. The SPI bus and the rail are the caller's. */
void    od_epd_nordic_claim_pins(const struct DisplayConfig *d);
bool    od_epd_nordic_select(uint8_t mode, od_epd_nordic_mode_t *out);
uint8_t od_epd_nordic_mode_count(void);
bool    od_epd_nordic_begin_pass(uint8_t pass);
bool    od_epd_nordic_write(const uint8_t *data, size_t len);
bool    od_epd_nordic_next_plane(void);
bool    od_epd_nordic_finish(void);
void    od_epd_nordic_sleep(void);
/* The od_epd return code of the last failed begin_pass. */
int     od_epd_nordic_last_rc(void);

#ifdef __cplusplus
}
#endif

#endif /* OD_EPD_NORDIC_H */
