#ifndef OPENDISPLAY_DISPLAY_H
#define OPENDISPLAY_DISPLAY_H

#include <stdbool.h>
#include <stdint.h>

#include "od_color.h"

#ifdef __cplusplus
extern "C" {
#endif

void opendisplay_display_abort(void);
void opendisplay_display_boot_apply(void);
/* Ask for the boot screen from the superloop once the response queue has drained; used after a
 * config write so its ACK is on the wire before a 20-35 s refresh. No-op once it has been drawn. */
void opendisplay_display_boot_request(void);
void opendisplay_display_park_pins(void);
void opendisplay_display_power_off(void);

/* Runs a queued display-from-slot refresh. Called from the superloop, because the
 * refresh is seconds long and must not sit inside a command handler. */
void opendisplay_display_service(void);

/* --- config-driven drive path (CMD_DRIVE_WRITE_START 0x0085) ---------------------------------
 *
 * Accessors rather than exported statics: the rail and the stored DisplayConfig are owned here,
 * and od_display_drive.cpp needs both to bring a named mode up. */
const struct DisplayConfig *opendisplay_display_config(void);
void opendisplay_display_power(bool on);

/* Register a multi-pass drive transfer whose panel is ALREADY up at the chosen mode.
 *
 * `geometry` describes ONE pass; the stream carries `passes` of them back to back, and a frame
 * boundary is a pass boundary. Validates a COMPRESSED declaration against passes*frame, which
 * is the only size the device will accept. Returns false on a size mismatch. */
bool opendisplay_display_drive_adopt(const od_color_geometry_t *geometry, uint8_t passes,
                                     uint8_t mode, bool compressed,
                                     const uint8_t *payload, uint16_t payload_len);

/* The byte counts the shared transfer layer needs, valid after a successful adopt. */
bool opendisplay_display_drive_stream_info(od_color_geometry_t *geometry_out,
                                           uint32_t *expected_out);

/* --- CMD_SPI_RAW (0x0084) session -------------------------------------------------------------
 *
 * A raw session PERSISTS across commands, which is the whole point: a handler that
 * re-initialised per call would discard whatever the previous call staged, making register
 * debugging impossible.
 *
 * Each returns 0, or -OD_ERR_SPI_RAW_* so the caller can hand the code straight to a NACK.
 * Everything except open() refuses with NOT_OPEN outside a session. */
int  opendisplay_display_spi_raw_open(bool run_panel_init);
int  opendisplay_display_spi_raw_cmd(const uint8_t *data, uint16_t len);
int  opendisplay_display_spi_raw_data(const uint8_t *data, uint16_t len);
int  opendisplay_display_spi_raw_busy(uint32_t timeout_ms, uint16_t *elapsed_ms_out,
                                      bool *still_busy_out);
int  opendisplay_display_spi_raw_reset(uint8_t hold_ms);
/* Sleep the panel and drop the rail. Idempotent, and safe to call from the disconnect path. */
void opendisplay_display_spi_raw_close(void);
bool opendisplay_display_spi_raw_is_open(void);

#ifdef __cplusplus
}
#endif

#endif
