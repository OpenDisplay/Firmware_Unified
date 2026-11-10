/* od_epd_cfg -- the config-driven panel driver bound to the stored config.
 *
 * od_epd holds the panel state machine and reads everything through an od_epd_io_t. This module
 * supplies the config half of that io (mode heads, sequences, waveforms, all read on demand
 * through od_cfg_index) and leaves the wire half -- D/C, CS, RST, BUSY and the SPI bytes -- to
 * the target through od_epd_cfg_bus_t. One session at a time; it is module state because od_epd's
 * io callbacks carry no context pointer.
 *
 * Used by CMD_DRIVE_WRITE_START (0x0085) and CMD_SPI_RAW (0x0084) on targets that keep bb_epaper
 * for ordinary uploads. A device whose config carries no drive packets never reaches this.
 */
#ifndef OD_EPD_CFG_H
#define OD_EPD_CFG_H

#include "od_color.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct DisplayConfig;

typedef struct {
    /* One command byte: D/C low, CS framed around it. */
    void (*cmd)(uint8_t c);
    /* Data bytes: D/C high, CS framed around them. */
    void (*data)(const uint8_t *d, size_t n);
    /* High, low for hold_ms, high, then hold_ms settle. No-op without a reset pin. */
    void (*reset)(uint32_t hold_ms);
    /* BUSY pin level, or -1 when the display has no BUSY pin. */
    int  (*busy_read)(void);
    void (*delay_ms)(uint32_t ms);
    /* Monotonic milliseconds, for the raw BUSY wait's elapsed time. */
    uint32_t (*now_ms)(void);
    /* Optional: true once the bus has failed a transfer. NULL means it never does. */
    bool (*faulted)(void);
} od_epd_cfg_bus_t;

typedef struct {
    uint8_t passes;
    uint8_t color_scheme;   /* 0xFF = the panel's own */
    int8_t  temp_min_c;
    int8_t  temp_max_c;
    uint8_t temp_policy;
} od_epd_cfg_mode_t;

/* Indexes the stored config and loads the panel descriptor for display `d`. Call once per
 * session: index entries are offsets and a config write invalidates them. False when the config
 * carries no drive packets for this display. `bus` must outlive the session. */
bool    od_epd_cfg_load(const struct DisplayConfig *d, const od_epd_cfg_bus_t *bus);
bool    od_epd_cfg_loaded(void);
/* The panel's BUSY idle level, for configuring the pin's pull. Valid after a load. */
uint8_t od_epd_cfg_busy_idle_level(void);

bool    od_epd_cfg_select(uint8_t mode, od_epd_cfg_mode_t *out);
uint8_t od_epd_cfg_mode_count(void);
bool    od_epd_cfg_begin_pass(uint8_t pass);
bool    od_epd_cfg_write(const uint8_t *data, size_t len);
bool    od_epd_cfg_next_plane(void);
bool    od_epd_cfg_finish(void);
void    od_epd_cfg_sleep(void);
/* The od_epd return code of the last failed begin_pass. */
int     od_epd_cfg_last_rc(void);

/* --- multi-pass stream ---------------------------------------------------------------------- */

/* One CMD_DRIVE_WRITE_START transfer: `passes` frames back to back. A frame boundary is a pass
 * boundary, and the refresh at that boundary blocks inside the write, which is also the flow
 * control -- the ACK of the chunk landing on it arrives once the pass is off the glass. */
typedef struct {
    uint32_t frame_bytes;
    uint32_t plane_size;   /* 0 unless the layout is CONTROLLER_PLANES */
    uint32_t total_bytes;
    uint8_t  passes;
    uint8_t  pass;
} od_epd_cfg_stream_t;

/* Validates passes*frame against the START tail (a compressed stream's 4-byte declared length
 * must equal it, so a host with the wrong pass count is told at START) and fills `s`. */
bool od_epd_cfg_stream_init(od_epd_cfg_stream_t *s, const od_color_geometry_t *frame,
                            uint8_t passes, bool compressed, const uint8_t *tail,
                            uint16_t tail_len);

/* `phase` is told true before a mid-stream refresh and false once the next pass is open, so a
 * target can move its watchdog phase. May be NULL. */
bool od_epd_cfg_stream_write(od_epd_cfg_stream_t *s, uint32_t stream_offset,
                             const uint8_t *data, size_t len, void (*phase)(bool refreshing));

/* --- raw session (CMD_SPI_RAW) -------------------------------------------------------------- */

/* Bytes straight to the bus, bypassing the pass state machine. `data[0]` is the command. */
void od_epd_cfg_raw_cmd(const uint8_t *data, uint16_t len);
void od_epd_cfg_raw_data(const uint8_t *data, uint16_t len);
/* Waits for BUSY to release or the timeout. still_busy is re-read after the wait, so a release
 * at the timeout is told apart from a timeout. */
void od_epd_cfg_raw_busy(uint32_t timeout_ms, uint16_t *elapsed_ms_out, bool *still_busy_out);
void od_epd_cfg_raw_reset(uint8_t hold_ms);

#ifdef __cplusplus
}
#endif

#endif /* OD_EPD_CFG_H */
