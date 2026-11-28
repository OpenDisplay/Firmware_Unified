#ifndef OD_COLOR_H
#define OD_COLOR_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "opendisplay_structs.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Firmware-local compatibility placeholder. The canonical protocol repository does not yet
 * carry value 9, and this implementation deliberately does not change that sibling repository.
 * The preprocessor cannot detect C enum members, so a future canonical reservation requires
 * removing this fallback after the vendored header is synced; the numeric identity stays 9. */
#ifndef OD_COLOR_SCHEME_GRAY8
#define OD_COLOR_SCHEME_GRAY8 ((uint8_t)9u)
#endif

typedef enum {
  OD_COLOR_OK = 0,
  OD_COLOR_UNSUPPORTED,
  OD_COLOR_BAD_GEOMETRY,
  OD_COLOR_OVERFLOW,
  OD_COLOR_BAD_LAYOUT,
} od_color_status_t;

typedef enum {
  OD_COLOR_LAYOUT_PACKED_ROWS = 0,
  OD_COLOR_LAYOUT_CONTROLLER_PLANES,
  OD_COLOR_LAYOUT_SPLIT_HALVES,
} od_color_layout_t;

typedef enum {
  OD_COLOR_PLANE_0 = 0,
  OD_COLOR_PLANE_1 = 1,
  OD_COLOR_PLANE_NONE = 0xff,
} od_color_plane_t;

typedef struct {
  od_color_layout_t layout;
  uint8_t bits_per_pixel;
  uint8_t part_count;
  od_color_plane_t initial_plane;
  bool partial_supported;
  uint32_t part_width[2];
  uint32_t row_bytes[2];
  uint32_t part_bytes[2];
  uint32_t total_bytes;
} od_color_geometry_t;

/* Describe an implemented direct-write byte stream. Reserved GRAY8 value 9 is a valid
 * classification identity but intentionally returns OD_COLOR_UNSUPPORTED: it has no wire
 * packing, FastEPD adapter, boot fallback, or target admission in this implementation. */
od_color_status_t od_color_direct_geometry(uint8_t color_scheme, uint32_t width, uint32_t height,
                                           od_color_geometry_t *geometry);

/* A display's frame layout (DisplayConfig 0x20 offsets 33-45, structs 2.5), validated and
 * resolved: part_width / part_stride are never 0, cell_bits is the wire bits per pixel. */
typedef struct {
  uint8_t parts;
  uint8_t scan;
  uint8_t row_order;
  uint8_t pixel_pack;
  uint8_t plane_invert;
  uint8_t bits_per_pixel;
  uint8_t cell_bits;
  uint16_t part_width;
  uint16_t part_stride;
} od_color_frame_layout_t;

/* True when the frame is sent in linear row-major order: every layout byte except
 * plane_invert is 0. Partial updates need this; an inverted plane keeps its order. */
bool od_color_layout_is_linear(const struct DisplayConfig *d);

/* Validate d's frame layout for frames packed as color_scheme (the panel's, or a mode's). Any
 * undefined value or bit, a non-zero reserved byte, a part boundary off a byte, a split of a
 * two-plane scheme, NIBBLE or INTERLEAVE on a scheme that is not 2 bpp, INTERLEAVE with
 * COLUMN_MAJOR or an odd height: OD_COLOR_BAD_LAYOUT. Scheme 8 is OD_COLOR_UNSUPPORTED. */
od_color_status_t od_color_frame_layout(const struct DisplayConfig *d, uint8_t color_scheme,
                                        od_color_frame_layout_t *out);

/* The wire geometry of one frame of d in color_scheme, with d's frame layout applied. Use this
 * for any frame of a configured display; od_color_direct_geometry() is for bare rectangles. */
od_color_status_t od_color_display_geometry(const struct DisplayConfig *d, uint8_t color_scheme,
                                            od_color_geometry_t *geometry);

/* Walks one plane of a frame in wire order for a renderer that draws a row at a time
 * (od_boot_screen). next() names the row and native columns [x0, x1) to draw; emit() takes that
 * row, packed native at bits_per_pixel MSB-first, and writes the wire bytes. Each pixel is
 * drawn once per part that covers it, so no frame buffer is needed for any layout. */
typedef int (*od_frame_walk_write_fn)(void *ctx, uint16_t y, uint8_t part, const uint8_t *p,
                                      uint16_t n);

typedef struct {
  od_color_frame_layout_t L;
  uint16_t w;
  uint16_t h;
  uint16_t pitch;
  uint8_t white;
  uint8_t invert;
  bool linear;
  bool direct;
  uint8_t *out;
  uint16_t out_cap;
  uint16_t out_len;
  uint8_t *hold;
  uint8_t part;
  uint16_t group;
  uint16_t groups;
  uint16_t k;
  uint16_t y;
  bool done;
} od_frame_walk_t;

/* scratch holds the output row (and, for INTERLEAVE, the held first row of each pair). A layout
 * that only cuts byte-aligned parts and reorders rows (split, FOLD, MIRROR_Y) writes straight
 * from the drawn row and needs none, like a linear one. invert is this plane's bit of plane_invert. false = scratch too
 * small or bad arguments. */
bool od_frame_walk_init(od_frame_walk_t *wk, const od_color_frame_layout_t *layout, uint16_t w,
                        uint16_t h, uint8_t white_code, bool invert, uint8_t *scratch,
                        size_t scratch_len);
bool od_frame_walk_next(od_frame_walk_t *wk, uint16_t *y, uint16_t *x0, uint16_t *x1);
bool od_frame_walk_emit(od_frame_walk_t *wk, const uint8_t *row, od_frame_walk_write_fn write,
                        void *ctx);
/* Writes what COLUMN_MAJOR still buffers. Call once after next() returns false. */
bool od_frame_walk_finish(od_frame_walk_t *wk, od_frame_walk_write_fn write, void *ctx);

#ifdef __cplusplus
}
#endif

#endif
