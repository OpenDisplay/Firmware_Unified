/* od_epd_lut -- waveform slot helpers shared by every config-driven panel adapter.
 *
 * Three jobs, all on the 0xAF display_lut slot and the inline tables of CMD_DRIVE_WRITE_START:
 *
 *   - the slot byte: bit 7 = the slot body is raw deflate (10-bit window), bits 0..3 = the slot
 *     id the 0xAE lut_slot[] names. Firmware without this code sees an id it never references,
 *     so a compressed slot degrades to "absent" there.
 *   - the device-side table lint per lut_family (0xB1 tail): the cheap checks that keep a bad
 *     table off the glass (S6 hang patterns, JD-BWRY row repeats past a block's end, table size). The full
 *     charge gate runs on the host and in presets CI; this is the backstop.
 *   - a small RAM cache holding inflated slots and inline tables, because a deflate stream
 *     cannot be read at random offsets and a pass reads its waveform in slices.
 *
 * The byte layouts are provisional (not in opendisplay_structs.h yet); they match the
 * py-opendisplay presets test implementation. docs/REFRESH_MODES.md lists them.
 */
#ifndef OD_EPD_LUT_H
#define OD_EPD_LUT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 0xAF body byte 1. */
#define OD_LUT_SLOT_COMPRESSED   0x80u
#define OD_LUT_SLOT_ID_MASK      0x0Fu
#define OD_LUT_SLOT_MAX          16u

/* Largest inflated (and stored) table. Also the deflate window: 10 bits. */
#define OD_LUT_MAX_BYTES         1024u

/* 0xB1 tail lut_family. 0 = undeclared: no lint, no never-send list (every config written
 * before the tail existed). */
#define OD_LUT_FAMILY_NONE       0u
#define OD_LUT_FAMILY_S6_800     1u   /* Spectra 6 register LUT at 0x20, 800 B */
#define OD_LUT_FAMILY_JD_BWRY    2u   /* JD7966x-class BWRY register LUT, 535 B */
#define OD_LUT_FAMILY_X4_536     3u   /* the "x4" JD variant (4.2in / 1.54in): 535 B + 1 pad */
#define OD_LUT_FAMILY_COUNT      4u

/* 0xB1 tail (after sleep_seq), v1. All optional past the 6-byte core. */
#define OD_PANEL_EXT_VERSION     0u
#define OD_PANEL_EXT_KEY         1u   /* drive_preset_key u16 (host only) */
#define OD_PANEL_EXT_REV         3u   /* drive_rev u16 (host only) */
#define OD_PANEL_EXT_FLAGS       5u   /* drive_flags (host only) */
#define OD_PANEL_EXT_BUDGET      6u   /* budget_limit (host only) */
#define OD_PANEL_EXT_FAMILY      7u   /* lut_family */
#define OD_PANEL_EXT_TIMEOUT_S   8u   /* busy_timeout_s, wins over busy_timeout_ms when != 0 */
#define OD_PANEL_EXT_V1_LEN      9u

/* CMD_DRIVE_WRITE_START flag: the two reserved bytes are inline_len (u16 LE) and the stream
 * starts with inline_len bytes of tables ([slot][len:2][crc16:2][table], repeated) before the
 * frames. They shadow the stored 0xAF slots for this transfer only; nothing is written to flash.
 * Provisional: not in opendisplay_protocol.h yet. */
#ifndef DRIVE_WRITE_FLAG_INLINE_LUT
#define DRIVE_WRITE_FLAG_INLINE_LUT 0x04u
#endif

/* Inline table header in a CMD_DRIVE_WRITE_START INLINE_LUT prefix:
 * [slot_id:1][len:2 LE][crc16:2 LE] then `len` raw table bytes. */
#define OD_LUT_INLINE_HDR        5u

/* Device-side table lint. `t` is the whole (inflated) table. */
bool od_lut_lint(uint8_t family, const uint8_t *t, uint16_t len);

/* The same lint over a table streamed in order in chunks (stored uncompressed slots are never
 * buffered). Feed every byte once, in order, then call _done. */
typedef struct {
    uint8_t  family;
    uint16_t off;
    bool     bad;
    uint8_t  prev;           /* the byte before `off` */
    uint8_t  zero_run;       /* S6 */
    bool     hdr_nonzero;    /* S6 */
    uint8_t  m;              /* JD: the current row's repeat */
    uint8_t  row[10];        /* JD: the current row's bytes 0..9 */
    bool     blk_done;       /* JD: the current block has ended */
} od_lut_lint_t;

void od_lut_lint_begin(od_lut_lint_t *l, uint8_t family);
void od_lut_lint_feed(od_lut_lint_t *l, const uint8_t *d, uint16_t n);
bool od_lut_lint_done(const od_lut_lint_t *l);

/* Commands a sequence of this family may never send (OTP/MTP programming and the vendor
 * commands the datasheets warn about). */
bool od_lut_never_send(uint8_t family, uint8_t cmd);

/* CRC-16/CCITT-FALSE, chained: pass 0xFFFF to start. */
uint16_t od_lut_crc16(uint16_t crc, const uint8_t *d, size_t n);

/* Reads `len` bytes at `off` of a compressed stream; returns the bytes read. */
typedef uint16_t (*od_lut_reader_t)(void *ctx, uint16_t off, uint8_t *dst, uint16_t len);

/* One-shot raw deflate (RFC 1951, no zlib header) of `in_len` bytes into out[0..cap). False on
 * a malformed stream, a distance past the start of the output, or output over `cap`. */
bool od_lut_inflate(od_lut_reader_t rd, void *ctx, uint16_t in_len, uint8_t *out, uint16_t cap,
                    uint16_t *out_len);

/* --- cache --------------------------------------------------------------------------------- */

/* Entries held in RAM (OD_LUT_MAX_BYTES each). 0 compiles the cache and the inflater out: a
 * compressed slot then reads as absent and INLINE_LUT is refused. */
#ifndef OD_EPD_LUT_CACHE
#define OD_EPD_LUT_CACHE 2u
#endif

#if OD_EPD_LUT_CACHE > 0
typedef struct {
    bool     used;
    bool     is_inline;    /* from a CMD_DRIVE_WRITE_START prefix; shadows a stored slot */
    uint8_t  instance;
    uint8_t  slot;         /* slot id, 0..15 */
    uint16_t len;
    uint32_t gen;          /* config generation the stored slot was inflated from */
    uint8_t  data[OD_LUT_MAX_BYTES];
} od_lut_cache_entry_t;

od_lut_cache_entry_t *od_lut_cache_find(uint8_t instance, uint8_t slot, bool want_inline,
                                        uint32_t gen);
/* A free entry, else the oldest stored one; never evicts an inline entry. NULL when every
 * entry holds an inline table. */
od_lut_cache_entry_t *od_lut_cache_claim(void);
void od_lut_cache_drop_inline(void);
void od_lut_cache_clear(void);
#endif

#ifdef __cplusplus
}
#endif

#endif /* OD_EPD_LUT_H */
