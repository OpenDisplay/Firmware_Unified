/* od_it8951: the IT8951 controller over SPI, for the ED103TC2 panel on the reTerminal E1003. */
#ifndef DISPLAY_IT8951_H
#define DISPLAY_IT8951_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

void od_it8951_set_panel(int8_t mosi, int8_t miso, int8_t sclk, int8_t cs, int8_t busy, int8_t rst,
                         int8_t en, int8_t ite_en, uint16_t width, uint16_t height, bool gray16);
/* Panel mounting and VCOM. Mirrored (ED103TC2) reverses each row; unmirrored (M5Paper ED047TC1)
 * sends gray rows as-is and bit-reverses 1-bit bytes. vcom_mv 0 keeps the controller's stored
 * VCOM. Defaults: mirrored, 1500 mV. Call before od_it8951_epaper_begin(). */
void od_it8951_set_layout(bool mirrored, uint16_t vcom_mv);
/* The next refresh uses this temperature; invalid selects the default. */
void od_it8951_set_temperature(bool valid, int temp_c);
bool od_it8951_init_failed(void);
void od_it8951_prepare_hardware(void);
void od_it8951_epaper_begin(void);
void od_it8951_full_update(void);
bool od_it8951_wait_refresh(int timeout_sec);
void od_it8951_sleep_after_refresh(void);
void od_it8951_boot_write_row(uint16_t y, const uint8_t *row, unsigned pitch);
void od_it8951_boot_skip_planes(void);
void od_it8951_direct_write_reset(void);
void od_it8951_direct_write_chunk(const uint8_t *data, uint32_t len);
void od_it8951_direct_refresh(int refresh_mode);
void od_it8951_direct_sleep(void);
void od_it8951_mark_hw_deinitialized(void);
void od_it8951_partial_prepare(uint16_t x, uint16_t y, uint16_t w, uint16_t h);
bool od_it8951_partial_write_chunk(const uint8_t *data, uint32_t len);
bool od_it8951_partial_refresh(int refresh_mode);
void od_it8951_release_bus(void);

#ifdef __cplusplus
}
#endif

#endif /* DISPLAY_IT8951_H */
