#include "od_color.h"

#include <limits.h>
#include <string.h>

static bool part_geometry(uint32_t width, uint32_t height, uint8_t bits_per_pixel,
                          uint32_t *row_bytes, uint32_t *part_bytes)
{
  uint64_t row_bits = (uint64_t)width * bits_per_pixel;
  uint64_t row = (row_bits + 7u) / 8u;
  uint64_t part = row * height;

  if (row > UINT32_MAX || part > UINT32_MAX) {
    return false;
  }
  *row_bytes = (uint32_t)row;
  *part_bytes = (uint32_t)part;
  return true;
}

od_color_status_t od_color_direct_geometry(uint8_t color_scheme, uint32_t width, uint32_t height,
                                           od_color_geometry_t *geometry)
{
  od_color_geometry_t out;
  uint64_t total;

  if (geometry == NULL) {
    return OD_COLOR_BAD_GEOMETRY;
  }
  memset(geometry, 0, sizeof(*geometry));
  if (width == 0u || height == 0u) {
    return OD_COLOR_BAD_GEOMETRY;
  }
  memset(&out, 0, sizeof(out));

  switch (color_scheme) {
    case OD_COLOR_SCHEME_MONO:
      out.layout = OD_COLOR_LAYOUT_PACKED_ROWS;
      out.bits_per_pixel = 1u;
      out.part_count = 1u;
      out.initial_plane = OD_COLOR_PLANE_0;
      out.partial_supported = true;
      out.part_width[0] = width;
      break;

    case OD_COLOR_SCHEME_BWR:
    case OD_COLOR_SCHEME_BWY:
    case OD_COLOR_SCHEME_GRAY4:
      out.layout = OD_COLOR_LAYOUT_CONTROLLER_PLANES;
      out.bits_per_pixel = 1u;
      out.part_count = 2u;
      out.initial_plane = OD_COLOR_PLANE_0;
      out.part_width[0] = width;
      out.part_width[1] = width;
      break;

    /* BWRY16 packs IDENTICALLY to BWRY: 2bpp, one plane, ONE frame. The 16 colours come from
     * driving that frame several times with different per-pass pixel codes, and the pass count
     * is a property of the display_mode (0xAE), not of the scheme. So this answers the geometry
     * of a SINGLE pass; multiplying by the pass count belongs to the drive path, which is the
     * only place that knows which mode was named.
     *
     * Sharing the arm rather than copying it is deliberate -- a host dithering for BWRY16 and a
     * device packing for BWRY have to agree byte for byte, so the two must not be able to drift.
     * `partial_supported` stays false for both: a partial update of one pass of a multi-pass
     * waveform is not a defined operation. */
    case OD_COLOR_SCHEME_BWRY:
    case OD_COLOR_SCHEME_BWRY16:
      out.layout = OD_COLOR_LAYOUT_PACKED_ROWS;
      out.bits_per_pixel = 2u;
      out.part_count = 1u;
      out.initial_plane = OD_COLOR_PLANE_1;
      out.part_width[0] = width;
      break;

    case OD_COLOR_SCHEME_BWGBRY:
    case OD_COLOR_SCHEME_SEVEN_COLOR:
      out.layout = OD_COLOR_LAYOUT_PACKED_ROWS;
      out.bits_per_pixel = 4u;
      out.part_count = 1u;
      out.initial_plane = OD_COLOR_PLANE_1;
      out.part_width[0] = width;
      break;

    case OD_COLOR_SCHEME_GRAY16:
      out.layout = OD_COLOR_LAYOUT_PACKED_ROWS;
      out.bits_per_pixel = 4u;
      out.part_count = 1u;
      out.initial_plane = OD_COLOR_PLANE_0;
      out.part_width[0] = width;
      break;

    case OD_COLOR_SCHEME_BWGBRY_SPLIT: /* retired: BWGBRY + frame_split HALVES */
    case OD_COLOR_SCHEME_GRAY8:
    case OD_COLOR_SCHEME_RGB565:
    case OD_COLOR_SCHEME_RGB888:
    case OD_COLOR_SCHEME_RGB16BPC:
    default:
      return OD_COLOR_UNSUPPORTED;
  }

  if (!part_geometry(out.part_width[0], height, out.bits_per_pixel, &out.row_bytes[0],
                     &out.part_bytes[0])) {
    return OD_COLOR_OVERFLOW;
  }
  if (out.part_count == 2u
      && !part_geometry(out.part_width[1], height, out.bits_per_pixel, &out.row_bytes[1],
                        &out.part_bytes[1])) {
    return OD_COLOR_OVERFLOW;
  }
  total = (uint64_t)out.part_bytes[0] + out.part_bytes[1];
  if (total > UINT32_MAX) {
    return OD_COLOR_OVERFLOW;
  }
  out.total_bytes = (uint32_t)total;
  *geometry = out;
  return OD_COLOR_OK;
}

/* ---- frame layout ------------------------------------------------------------------------ */

#define LAYOUT_SCAN_DEFINED   (OD_FRAME_SCAN_COLUMN_MAJOR | OD_FRAME_SCAN_MIRROR_X | \
                               OD_FRAME_SCAN_MIRROR_Y | OD_FRAME_SCAN_LSB_FIRST)
#define LAYOUT_INVERT_DEFINED (OD_PLANE_INVERT_0 | OD_PLANE_INVERT_1)

bool od_color_layout_is_linear(const struct DisplayConfig *d)
{
  size_t i;

  if (d->frame_split != 0u || d->frame_scan != 0u || d->part_width != 0u ||
      d->part_stride != 0u || d->pixel_pack != 0u || d->row_order != 0u) {
    return false;
  }
  for (i = 0u; i < sizeof d->reserved; i++) {
    if (d->reserved[i] != 0u) {
      return false;
    }
  }
  return true;
}

od_color_status_t od_color_frame_layout(const struct DisplayConfig *d, uint8_t color_scheme,
                                        od_color_frame_layout_t *out)
{
  od_color_geometry_t base;
  od_color_status_t rc;
  size_t i;
  uint32_t pw;
  uint32_t stride;

  if (d == NULL || out == NULL) {
    return OD_COLOR_BAD_GEOMETRY;
  }
  memset(out, 0, sizeof *out);
  rc = od_color_direct_geometry(color_scheme, d->pixel_width, d->pixel_height, &base);
  if (rc != OD_COLOR_OK) {
    return rc;
  }
  for (i = 0u; i < sizeof d->reserved; i++) {
    if (d->reserved[i] != 0u) {
      return OD_COLOR_BAD_LAYOUT;
    }
  }
  if (d->frame_split > OD_FRAME_SPLIT_HALVES || (d->frame_scan & ~LAYOUT_SCAN_DEFINED) != 0u ||
      (d->plane_invert & ~LAYOUT_INVERT_DEFINED) != 0u ||
      d->pixel_pack > OD_PIXEL_PACK_NIBBLE || d->row_order > OD_ROW_ORDER_INTERLEAVE) {
    return OD_COLOR_BAD_LAYOUT;
  }
  out->parts = d->frame_split == OD_FRAME_SPLIT_HALVES ? 2u : 1u;
  out->scan = d->frame_scan;
  out->row_order = d->row_order;
  out->pixel_pack = d->pixel_pack;
  out->plane_invert = d->plane_invert;
  out->bits_per_pixel = base.bits_per_pixel;
  out->cell_bits = d->pixel_pack == OD_PIXEL_PACK_NIBBLE ? 4u : base.bits_per_pixel;
  if (d->pixel_pack == OD_PIXEL_PACK_NIBBLE && base.bits_per_pixel != 2u) {
    return OD_COLOR_BAD_LAYOUT;
  }
  if (d->row_order == OD_ROW_ORDER_INTERLEAVE &&
      (out->cell_bits != 2u || (d->frame_scan & OD_FRAME_SCAN_COLUMN_MAJOR) != 0u ||
       (d->pixel_height & 1u) != 0u)) {
    return OD_COLOR_BAD_LAYOUT;
  }
  if (out->parts == 2u && base.layout != OD_COLOR_LAYOUT_PACKED_ROWS) {
    return OD_COLOR_BAD_LAYOUT;
  }
  if (out->parts == 1u && (d->part_width != 0u || d->part_stride != 0u)) {
    return OD_COLOR_BAD_LAYOUT;
  }
  if ((d->plane_invert & OD_PLANE_INVERT_1) != 0u &&
      base.layout != OD_COLOR_LAYOUT_CONTROLLER_PLANES) {
    return OD_COLOR_BAD_LAYOUT;
  }
  pw = d->part_width != 0u ? d->part_width : (uint32_t)d->pixel_width / out->parts;
  stride = d->part_stride != 0u ? d->part_stride : pw;
  if (pw == 0u || (out->parts == 2u && stride >= d->pixel_width)) {
    return OD_COLOR_BAD_LAYOUT;
  }
  /* A part boundary off a byte would need a bit shift on every row; no panel needs it. */
  if (out->parts == 2u &&
      (((pw * out->cell_bits) & 7u) != 0u || ((stride * base.bits_per_pixel) & 7u) != 0u)) {
    return OD_COLOR_BAD_LAYOUT;
  }
  if ((d->frame_scan & OD_FRAME_SCAN_COLUMN_MAJOR) != 0u && ((pw * out->cell_bits) & 7u) != 0u) {
    return OD_COLOR_BAD_LAYOUT;
  }
  out->part_width = (uint16_t)pw;
  out->part_stride = (uint16_t)stride;
  return OD_COLOR_OK;
}

od_color_status_t od_color_display_geometry(const struct DisplayConfig *d, uint8_t color_scheme,
                                            od_color_geometry_t *geometry)
{
  od_color_frame_layout_t L;
  od_color_geometry_t g;
  od_color_status_t rc;
  uint64_t total = 0u;
  uint8_t n;
  uint8_t planes;

  if (geometry == NULL) {
    return OD_COLOR_BAD_GEOMETRY;
  }
  memset(geometry, 0, sizeof *geometry);
  if (d == NULL) {
    return OD_COLOR_BAD_GEOMETRY;
  }
  rc = od_color_direct_geometry(color_scheme, d->pixel_width, d->pixel_height, &g);
  if (rc == OD_COLOR_OK) {
    rc = od_color_frame_layout(d, color_scheme, &L);
  }
  if (rc != OD_COLOR_OK) {
    return rc;
  }
  if (od_color_layout_is_linear(d)) {
    *geometry = g;   /* plane_invert alone changes no size and no order */
    return OD_COLOR_OK;
  }
  planes = L.parts == 2u ? 2u : g.part_count;
  if (L.parts == 2u) {
    g.layout = OD_COLOR_LAYOUT_SPLIT_HALVES;
    g.part_count = 2u;
    g.initial_plane = OD_COLOR_PLANE_NONE;
  }
  for (n = 0u; n < planes; n++) {
    const uint32_t width = L.part_width;

    if (!part_geometry(width, d->pixel_height, L.cell_bits, &g.row_bytes[n], &g.part_bytes[n])) {
      return OD_COLOR_OVERFLOW;
    }
    g.part_width[n] = width;
    total += g.part_bytes[n];
  }
  if (total > UINT32_MAX) {
    return OD_COLOR_OVERFLOW;
  }
  g.total_bytes = (uint32_t)total;
  g.partial_supported = false;
  *geometry = g;
  return OD_COLOR_OK;
}

/* ---- frame walk -------------------------------------------------------------------------- */

static uint8_t get_code(const uint8_t *row, uint32_t x, uint8_t bpp)
{
  const uint32_t bit = x * bpp;
  const uint8_t shift = (uint8_t)(8u - bpp - (bit & 7u));

  return (uint8_t)((row[bit >> 3] >> shift) & ((1u << bpp) - 1u));
}

static void put_cell(uint8_t *out, uint32_t c, uint8_t cell, uint8_t code, bool lsb_first)
{
  const uint32_t bit = c * cell;
  const uint8_t slot = (uint8_t)((bit & 7u) / cell);
  const uint8_t per = (uint8_t)(8u / cell);
  const uint8_t pos = lsb_first ? slot : (uint8_t)(per - 1u - slot);

  out[bit >> 3] |= (uint8_t)(code << (pos * cell));
}

static uint16_t walk_row(const od_frame_walk_t *wk, uint16_t k)
{
  uint16_t r = k;

  if (wk->L.row_order == OD_ROW_ORDER_FOLD) {
    r = (k & 1u) == 0u ? (uint16_t)(wk->h - 1u - k / 2u) : (uint16_t)(k / 2u);
  }
  if ((wk->L.scan & OD_FRAME_SCAN_MIRROR_Y) != 0u) {
    r = (uint16_t)(wk->h - 1u - r);
  }
  return r;
}

/* Native column of cell c of the current part; >= w is past the panel edge. */
static uint32_t walk_col(const od_frame_walk_t *wk, uint32_t c)
{
  const uint32_t start = (uint32_t)wk->part * wk->L.part_stride;

  return (wk->L.scan & OD_FRAME_SCAN_MIRROR_X) != 0u ? start + wk->L.part_width - 1u - c
                                                       : start + c;
}

static uint8_t walk_code(const od_frame_walk_t *wk, const uint8_t *row, uint32_t c)
{
  const uint32_t x = walk_col(wk, c);

  return x < wk->w ? get_code(row, x, wk->L.bits_per_pixel) : wk->white;
}

bool od_frame_walk_init(od_frame_walk_t *wk, const od_color_frame_layout_t *layout, uint16_t w,
                        uint16_t h, uint8_t white_code, bool invert, uint8_t *scratch,
                        size_t scratch_len)
{
  size_t need;
  uint32_t row_bytes;

  if (wk == NULL || layout == NULL || w == 0u || h == 0u || layout->bits_per_pixel == 0u) {
    return false;
  }
  memset(wk, 0, sizeof *wk);
  wk->L = *layout;
  wk->w = w;
  wk->h = h;
  wk->pitch = (uint16_t)(((uint32_t)w * layout->bits_per_pixel + 7u) / 8u);
  wk->white = white_code;
  wk->invert = invert ? 0xFFu : 0x00u;
  wk->linear = layout->parts == 1u && layout->scan == 0u && layout->row_order == 0u &&
               layout->pixel_pack == 0u && !invert && layout->part_width == w;
  if (wk->linear) {
    return true;
  }
  wk->direct = (layout->scan & (uint8_t)~OD_FRAME_SCAN_MIRROR_Y) == 0u &&
               layout->row_order != OD_ROW_ORDER_INTERLEAVE && layout->pixel_pack == 0u &&
               !invert &&
               (layout->parts == 1u ? layout->part_width == w
                                    : ((uint32_t)layout->part_width * layout->bits_per_pixel & 7u) == 0u) &&
               (uint32_t)layout->part_stride * (layout->parts - 1u) + layout->part_width <= w;
  if (wk->direct) {
    return true;
  }
  row_bytes = ((uint32_t)layout->part_width * layout->cell_bits + 7u) / 8u;
  if (layout->row_order == OD_ROW_ORDER_INTERLEAVE) {
    row_bytes *= 2u;
  }
  if ((layout->scan & OD_FRAME_SCAN_COLUMN_MAJOR) != 0u) {
    wk->groups = (uint16_t)row_bytes;
    row_bytes = scratch_len < h ? (uint32_t)scratch_len : h;
  }
  need = row_bytes + (layout->row_order == OD_ROW_ORDER_INTERLEAVE ? wk->pitch : 0u);
  if (scratch == NULL || row_bytes == 0u || row_bytes > UINT16_MAX || need > scratch_len) {
    return false;
  }
  wk->out = scratch;
  wk->out_cap = (uint16_t)row_bytes;
  wk->hold = layout->row_order == OD_ROW_ORDER_INTERLEAVE ? scratch + row_bytes : NULL;
  return true;
}

bool od_frame_walk_next(od_frame_walk_t *wk, uint16_t *y, uint16_t *x0, uint16_t *x1)
{
  uint32_t lo;
  uint32_t hi;
  uint32_t a;
  uint32_t b;

  if (wk == NULL || wk->done) {
    return false;
  }
  if (wk->k == wk->h) {
    wk->k = 0u;
    if ((wk->L.scan & OD_FRAME_SCAN_COLUMN_MAJOR) != 0u && ++wk->group < wk->groups) {
      /* next byte-column of this part */
    } else {
      wk->group = 0u;
      if (wk->linear || ++wk->part >= wk->L.parts) {
        wk->done = true;
        return false;
      }
    }
  }
  wk->y = wk->L.row_order == OD_ROW_ORDER_INTERLEAVE
      ? ((wk->L.scan & OD_FRAME_SCAN_MIRROR_Y) != 0u ? (uint16_t)(wk->h - 1u - wk->k) : wk->k)
      : walk_row(wk, wk->k);
  *y = wk->y;
  if (wk->linear) {
    *x0 = 0u;
    *x1 = wk->w;
    return true;
  }
  if ((wk->L.scan & OD_FRAME_SCAN_COLUMN_MAJOR) != 0u) {
    const uint32_t per = 8u / wk->L.cell_bits;

    a = walk_col(wk, (uint32_t)wk->group * per);
    b = walk_col(wk, (uint32_t)wk->group * per + per - 1u);
  } else {
    a = walk_col(wk, 0u);
    b = walk_col(wk, wk->L.part_width - 1u);
  }
  lo = a < b ? a : b;
  hi = (a < b ? b : a) + 1u;
  *x0 = (uint16_t)(lo < wk->w ? lo : wk->w);
  *x1 = (uint16_t)(hi < wk->w ? hi : wk->w);
  return true;
}

static bool walk_flush(od_frame_walk_t *wk, od_frame_walk_write_fn write, void *ctx)
{
  bool ok = true;

  if (wk->out_len != 0u) {
    ok = write(ctx, wk->y, wk->part, wk->out, wk->out_len) == 0;
    wk->out_len = 0u;
  }
  return ok;
}

bool od_frame_walk_emit(od_frame_walk_t *wk, const uint8_t *row, od_frame_walk_write_fn write,
                        void *ctx)
{
  const bool lsb = (wk->L.scan & OD_FRAME_SCAN_LSB_FIRST) != 0u;
  const uint8_t cell = wk->L.cell_bits;
  uint32_t c;
  uint16_t i;

  if (wk == NULL || row == NULL || write == NULL || wk->done) {
    return false;
  }
  wk->k++;
  if (wk->linear) {
    return write(ctx, wk->y, 0u, row, wk->pitch) == 0;
  }
  if (wk->direct) {
    const uint32_t at = (uint32_t)wk->part * wk->L.part_stride * wk->L.bits_per_pixel / 8u;

    return write(ctx, wk->y, wk->part, row + at,
                 wk->L.parts == 1u ? wk->pitch
                                   : (uint16_t)((uint32_t)wk->L.part_width * wk->L.bits_per_pixel / 8u)) == 0;
  }
  if ((wk->L.scan & OD_FRAME_SCAN_COLUMN_MAJOR) != 0u) {
    const uint32_t per = 8u / cell;
    uint8_t byte = 0u;

    for (c = 0u; c < per; c++) {
      put_cell(&byte, c, cell, walk_code(wk, row, (uint32_t)wk->group * per + c), lsb);
    }
    wk->out[wk->out_len++] = (uint8_t)(byte ^ wk->invert);
    return (wk->out_len < wk->out_cap && wk->k < wk->h) || walk_flush(wk, write, ctx);
  }
  if (wk->L.row_order == OD_ROW_ORDER_INTERLEAVE && (wk->k & 1u) != 0u) {
    memcpy(wk->hold, row, wk->pitch);
    return true;
  }
  memset(wk->out, 0, wk->out_cap);
  for (c = 0u; c < wk->L.part_width; c++) {
    if (wk->L.row_order == OD_ROW_ORDER_INTERLEAVE) {
      put_cell(wk->out, 2u * c, cell, walk_code(wk, row, c), lsb);
      put_cell(wk->out, 2u * c + 1u, cell, walk_code(wk, wk->hold, c), lsb);
    } else {
      put_cell(wk->out, c, cell, walk_code(wk, row, c), lsb);
    }
  }
  /* Pad bits at the end of a row stay 0, as in a linear frame; invert flips them too. */
  for (i = 0u; i < wk->out_cap; i++) {
    wk->out[i] ^= wk->invert;
  }
  wk->out_len = wk->out_cap;
  return walk_flush(wk, write, ctx);
}

bool od_frame_walk_finish(od_frame_walk_t *wk, od_frame_walk_write_fn write, void *ctx)
{
  return wk != NULL && write != NULL && walk_flush(wk, write, ctx);
}
