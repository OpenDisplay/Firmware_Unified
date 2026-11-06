/* od_epd_shim -- the config-driven panel path, as plain C, for opendisplay_display.cpp.
 *
 * WHY A SHIM AND NOT THE ADAPTER DIRECTLY. od_epd_adapter.h re-declares bb_epaper's constants
 * (BBEP_SUCCESS, PLANE_0, REFRESH_FULL...) as macros, because it was written to REPLACE that
 * library. bb_epaper declares the same names as ENUMERATORS. A macro cannot be #ifndef-guarded
 * against an enum, so including both headers in one translation unit is decided by include
 * order: adapter first turns `enum { BBEP_SUCCESS, ... }` into `enum { 0, ... }` and fails to
 * compile. The values happen to agree today, which makes the hazard worse, not better -- it
 * would compile in one order and break in the other for reasons nothing in the file explains.
 *
 * Keeping both backends therefore means keeping their headers apart. This is the seam.
 *
 * THE TWO MODELS DIFFER, which is the other reason a 1:1 macro swap was never going to work.
 * bb_epaper is window-and-plane: setAddrWindow, startWrite(plane), writeData, refresh. od_epd
 * is pass-based: the drive sequence from config says where the frame goes, so it is
 * selectMode, beginPass, writeData, [nextPlane], finishPass. The mapping lives here.
 */
#ifndef OD_EPD_SHIM_H
#define OD_EPD_SHIM_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Does THIS DEVICE carry drive data for display `instance`?
 *
 * Per-device, not per-build: a unit provisioned before the drive packets existed has to keep
 * working on bb_epaper, which is why both backends stay linked. */
bool od_epd_shim_available(uint8_t display_instance);

/* Power-up through to "ready for pixels": load the panel descriptor from config, apply pins
 * and rotation, select a drive mode and run the sequence up to its image marker. */
bool od_epd_shim_begin(uint8_t display_instance, uint8_t mode_index,
                       uint8_t rotation_quarters,
                       uint8_t dc, uint8_t rst, uint8_t busy, uint8_t cs,
                       uint8_t mosi, uint8_t sck);

bool od_epd_shim_write(const uint8_t *data, uint32_t len);

/* Advance to the sequence's next image marker, for two-plane panels. */
bool od_epd_shim_next_plane(void);

/* Run the sequence tail: power-on, refresh, BUSY wait. */
bool od_epd_shim_finish(void);

void od_epd_shim_sleep(void);

#ifdef __cplusplus
}
#endif
#endif /* OD_EPD_SHIM_H */
