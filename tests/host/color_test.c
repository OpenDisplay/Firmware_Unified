#include "od_color.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

static int checks;
static int failures;

#define CHECK(expr)                                                                            \
  do {                                                                                         \
    checks++;                                                                                  \
    if (!(expr)) {                                                                             \
      fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #expr);                                 \
      failures++;                                                                              \
    }                                                                                          \
  } while (0)

/* Pin the placeholder identity independently of its unsupported operational behavior. A future
 * canonical enum reservation must retain 9, but does not by itself define a wire encoding. */
_Static_assert(OD_COLOR_SCHEME_GRAY8 == 9u, "GRAY8 reserves placeholder value 9");

static uint32_t expected_row(uint32_t width, uint8_t bpp)
{
  return (uint32_t)(((uint64_t)width * bpp + 7u) / 8u);
}

static void check_geometry(uint8_t scheme, uint32_t width, uint32_t height,
                           od_color_layout_t layout, uint8_t bpp, uint8_t parts,
                           od_color_plane_t plane, bool partial, uint32_t part0_width,
                           uint32_t part1_width)
{
  od_color_geometry_t g;
  uint32_t row0 = expected_row(part0_width, bpp);
  uint32_t row1 = parts == 2u ? expected_row(part1_width, bpp) : 0u;

  memset(&g, 0xA5, sizeof(g));
  CHECK(od_color_direct_geometry(scheme, width, height, &g) == OD_COLOR_OK);
  CHECK(g.layout == layout);
  CHECK(g.bits_per_pixel == bpp);
  CHECK(g.part_count == parts);
  CHECK(g.initial_plane == plane);
  CHECK(g.partial_supported == partial);
  CHECK(g.part_width[0] == part0_width);
  CHECK(g.part_width[1] == part1_width);
  CHECK(g.row_bytes[0] == row0);
  CHECK(g.row_bytes[1] == row1);
  CHECK(g.part_bytes[0] == row0 * height);
  CHECK(g.part_bytes[1] == row1 * height);
  CHECK(g.total_bytes == (row0 + row1) * height);
}

static void check_unsupported(uint8_t scheme, uint32_t width, uint32_t height)
{
  od_color_geometry_t g;
  od_color_geometry_t zero;
  memset(&g, 0xA5, sizeof(g));
  memset(&zero, 0, sizeof(zero));
  CHECK(od_color_direct_geometry(scheme, width, height, &g) == OD_COLOR_UNSUPPORTED);
  CHECK(memcmp(&g, &zero, sizeof(g)) == 0);
}

static void classification_matrix(void)
{
  static const uint32_t widths[] = {1u, 2u, 3u, 4u, 7u, 8u, 9u, 121u, 122u, 127u, 128u};
  static const uint32_t heights[] = {1u, 2u, 250u};
  size_t wi;
  size_t hi;

  for (wi = 0; wi < sizeof(widths) / sizeof(widths[0]); wi++) {
    for (hi = 0; hi < sizeof(heights) / sizeof(heights[0]); hi++) {
      uint32_t w = widths[wi];
      uint32_t h = heights[hi];
      check_geometry(OD_COLOR_SCHEME_MONO, w, h, OD_COLOR_LAYOUT_PACKED_ROWS, 1u, 1u,
                     OD_COLOR_PLANE_0, true, w, 0u);
      check_geometry(OD_COLOR_SCHEME_BWR, w, h, OD_COLOR_LAYOUT_CONTROLLER_PLANES, 1u,
                     2u, OD_COLOR_PLANE_0, false, w, w);
      check_geometry(OD_COLOR_SCHEME_BWY, w, h, OD_COLOR_LAYOUT_CONTROLLER_PLANES, 1u,
                     2u, OD_COLOR_PLANE_0, false, w, w);
      check_geometry(OD_COLOR_SCHEME_GRAY4, w, h, OD_COLOR_LAYOUT_CONTROLLER_PLANES, 1u,
                     2u, OD_COLOR_PLANE_0, false, w, w);
      check_geometry(OD_COLOR_SCHEME_BWRY, w, h, OD_COLOR_LAYOUT_PACKED_ROWS, 2u, 1u,
                     OD_COLOR_PLANE_1, false, w, 0u);
      /* BWRY16 is ONE pass of BWRY packing. The pass count lives on the display_mode, so the
       * geometry of the two schemes must be identical -- asserted field-by-field below as well,
       * because a host dithering for 10 and a device packing for 3 have to agree byte for byte. */
      check_geometry(OD_COLOR_SCHEME_BWRY16, w, h, OD_COLOR_LAYOUT_PACKED_ROWS, 2u, 1u,
                     OD_COLOR_PLANE_1, false, w, 0u);
      check_geometry(OD_COLOR_SCHEME_BWGBRY, w, h, OD_COLOR_LAYOUT_PACKED_ROWS, 4u, 1u,
                     OD_COLOR_PLANE_1, false, w, 0u);
      check_geometry(OD_COLOR_SCHEME_GRAY16, w, h, OD_COLOR_LAYOUT_PACKED_ROWS, 4u, 1u,
                     OD_COLOR_PLANE_0, false, w, 0u);
      check_geometry(OD_COLOR_SCHEME_SEVEN_COLOR, w, h, OD_COLOR_LAYOUT_PACKED_ROWS, 4u,
                     1u, OD_COLOR_PLANE_1, false, w, 0u);
      check_unsupported(OD_COLOR_SCHEME_BWGBRY_SPLIT, w, h);
      check_unsupported(OD_COLOR_SCHEME_GRAY8, w, h);
      {
        /* The equality itself, not two copies of the same expectation: this fails if either
         * arm is ever edited without the other. */
        od_color_geometry_t bwry, bwry16;
        CHECK(od_color_direct_geometry(OD_COLOR_SCHEME_BWRY, w, h, &bwry) == OD_COLOR_OK);
        CHECK(od_color_direct_geometry(OD_COLOR_SCHEME_BWRY16, w, h, &bwry16) == OD_COLOR_OK);
        CHECK(memcmp(&bwry, &bwry16, sizeof bwry) == 0);
      }
    }
  }
}

static void fixed_vectors(void)
{
  od_color_geometry_t g;

  CHECK(od_color_direct_geometry(OD_COLOR_SCHEME_MONO, 122u, 250u, &g) == OD_COLOR_OK);
  CHECK(g.part_bytes[0] == 4000u && g.total_bytes == 4000u);
  CHECK(od_color_direct_geometry(OD_COLOR_SCHEME_GRAY4, 122u, 250u, &g) == OD_COLOR_OK);
  CHECK(g.part_bytes[0] == 4000u && g.part_bytes[1] == 4000u && g.total_bytes == 8000u);
  CHECK(od_color_direct_geometry(OD_COLOR_SCHEME_BWR, 122u, 250u, &g) == OD_COLOR_OK);
  CHECK(g.part_bytes[0] == 4000u && g.part_bytes[1] == 4000u && g.total_bytes == 8000u);
  CHECK(od_color_direct_geometry(OD_COLOR_SCHEME_BWY, 122u, 250u, &g) == OD_COLOR_OK);
  CHECK(g.part_bytes[0] == 4000u && g.part_bytes[1] == 4000u && g.total_bytes == 8000u);
  CHECK(od_color_direct_geometry(OD_COLOR_SCHEME_BWRY, 122u, 250u, &g) == OD_COLOR_OK);
  CHECK(g.part_bytes[0] == 7750u && g.total_bytes == 7750u);
  /* ONE pass, not three: total_bytes is a single frame and the drive path multiplies by the
   * mode's pass count. If this ever reports 23250 the multiplication has leaked in here. */
  CHECK(od_color_direct_geometry(OD_COLOR_SCHEME_BWRY16, 122u, 250u, &g) == OD_COLOR_OK);
  CHECK(g.part_bytes[0] == 7750u && g.total_bytes == 7750u);
  CHECK(od_color_direct_geometry(OD_COLOR_SCHEME_SEVEN_COLOR, 122u, 250u, &g)
        == OD_COLOR_OK);
  CHECK(g.part_bytes[0] == 15250u && g.total_bytes == 15250u);
}

static void rejected_and_invalid(void)
{
  static const uint8_t unsupported[] = {
    OD_COLOR_SCHEME_BWGBRY_SPLIT,   /* retired: BWGBRY + DisplayConfig.frame_split */
    OD_COLOR_SCHEME_GRAY8,
    OD_COLOR_SCHEME_RGB565,
    OD_COLOR_SCHEME_RGB888,
    OD_COLOR_SCHEME_RGB16BPC,
    /* 10 (BWRY16) used to sit here as "not implemented". It is implemented now and packs as
     * BWRY, asserted in direct_geometry(); leaving it here would assert the opposite. */
    0xFFu,
  };
  od_color_geometry_t g;
  od_color_geometry_t zero;
  size_t i;

  memset(&zero, 0, sizeof(zero));
  for (i = 0; i < sizeof(unsupported) / sizeof(unsupported[0]); i++) {
    memset(&g, 0xA5, sizeof(g));
    CHECK(od_color_direct_geometry(unsupported[i], 122u, 250u, &g) == OD_COLOR_UNSUPPORTED);
    CHECK(memcmp(&g, &zero, sizeof(g)) == 0);
  }
  memset(&g, 0xA5, sizeof(g));
  CHECK(od_color_direct_geometry(OD_COLOR_SCHEME_MONO, 0u, 1u, &g) == OD_COLOR_BAD_GEOMETRY);
  CHECK(memcmp(&g, &zero, sizeof(g)) == 0);
  memset(&g, 0xA5, sizeof(g));
  CHECK(od_color_direct_geometry(OD_COLOR_SCHEME_MONO, 1u, 0u, &g) == OD_COLOR_BAD_GEOMETRY);
  CHECK(memcmp(&g, &zero, sizeof(g)) == 0);
  CHECK(od_color_direct_geometry(OD_COLOR_SCHEME_MONO, 1u, 1u, NULL)
        == OD_COLOR_BAD_GEOMETRY);

  memset(&g, 0xA5, sizeof(g));
  CHECK(od_color_direct_geometry(OD_COLOR_SCHEME_BWGBRY, UINT32_MAX, 3u, &g)
        == OD_COLOR_OVERFLOW);
  CHECK(memcmp(&g, &zero, sizeof(g)) == 0);
}

/* ---- frame layout (DisplayConfig 0x20 structs 2.5) ------------------------------------------- */

#include "../vectors/frame_layout.inc"

static struct DisplayConfig layout_display(uint16_t w, uint16_t h, uint8_t scheme)
{
  struct DisplayConfig d;

  memset(&d, 0, sizeof d);
  d.pixel_width = w;
  d.pixel_height = h;
  d.color_scheme = scheme;
  return d;
}

static uint8_t fl_code(uint32_t x, uint32_t y, uint32_t plane, uint8_t bits)
{
  return (uint8_t)(((x * 7u + y * 13u + plane * 5u) ^ (x >> 2)) & ((1u << bits) - 1u));
}

typedef struct {
  uint8_t buf[2048];
  uint32_t n;
  bool overflow;
} fl_sink_t;

static int fl_write(void *ctx, uint16_t y, uint8_t part, const uint8_t *p, uint16_t n)
{
  fl_sink_t *s = (fl_sink_t *)ctx;

  (void)y;
  (void)part;
  if (s->n + n > sizeof s->buf) {
    s->overflow = true;
    return -1;
  }
  memcpy(s->buf + s->n, p, n);
  s->n += n;
  return 0;
}

/* The walker against py-opendisplay's packer on the same raster: the boot screen and the host
 * must put every byte in the same place, for every layout field. */
static void frame_walk_matches_host_packer(void)
{
  size_t i;

  for (i = 0; i < sizeof k_fl_cases / sizeof k_fl_cases[0]; i++) {
    const fl_case_t *c = &k_fl_cases[i];
    struct DisplayConfig d = layout_display(c->width, c->height, c->scheme);
    od_color_frame_layout_t L;
    od_color_geometry_t g;
    od_frame_walk_t wk;
    static fl_sink_t sink;
    uint8_t scratch[256];
    uint8_t row[64];
    uint16_t y, x0, x1;
    uint32_t x;
    bool ok = true;

    d.frame_split = c->frame_split;
    d.frame_scan = c->frame_scan;
    d.plane_invert = c->plane_invert;
    d.pixel_pack = c->pixel_pack;
    d.row_order = c->row_order;
    d.part_width = c->part_width;
    d.part_stride = c->part_stride;
    memset(&sink, 0, sizeof sink);
    CHECK(od_color_frame_layout(&d, c->scheme, &L) == OD_COLOR_OK);
    CHECK(od_color_display_geometry(&d, c->scheme, &g) == OD_COLOR_OK);
    CHECK(g.total_bytes == c->n * (g.layout == OD_COLOR_LAYOUT_CONTROLLER_PLANES ? 2u : 1u));
    CHECK(od_frame_walk_init(&wk, &L, c->width, c->height, c->white,
                             ((c->plane_invert >> c->plane) & 1u) != 0u, scratch, sizeof scratch));
    while (od_frame_walk_next(&wk, &y, &x0, &x1)) {
      memset(row, 0, sizeof row);
      for (x = 0; x < c->width; x++) {   /* the whole row: x0/x1 only bound what must be drawn */
        const uint8_t v = fl_code(x, y, c->plane, c->bits);
        const uint32_t bit = x * c->bits;

        row[bit / 8u] |= (uint8_t)(v << (8u - c->bits - (bit % 8u)));
      }
      ok = ok && x0 <= x1 && x1 <= c->width;
      ok = ok && od_frame_walk_emit(&wk, row, fl_write, &sink);
    }
    ok = ok && od_frame_walk_finish(&wk, fl_write, &sink);
    if (!ok || sink.overflow || sink.n != c->n || memcmp(sink.buf, c->bytes, c->n) != 0) {
      fprintf(stderr, "FAIL frame walk: %s (%u of %u bytes)\n", c->label, (unsigned)sink.n,
              (unsigned)c->n);
      failures++;
    }
    checks++;
  }
}

static void frame_layout_rules(void)
{
  struct DisplayConfig d;
  od_color_frame_layout_t L;
  od_color_geometry_t g;

  /* All zero is the plain frame: the bare-rectangle geometry, field for field. */
  {
    static const uint8_t schemes[] = { 0u, 1u, 2u, 3u, 4u, 5u, 6u, 7u, 10u };
    od_color_geometry_t ref;
    size_t k;

    for (k = 0; k < sizeof schemes; k++) {
      d = layout_display(122u, 250u, schemes[k]);
      CHECK(od_color_layout_is_linear(&d));
      CHECK(od_color_display_geometry(&d, schemes[k], &g) == OD_COLOR_OK);
      CHECK(od_color_direct_geometry(schemes[k], 122u, 250u, &ref) == OD_COLOR_OK);
      CHECK(memcmp(&g, &ref, sizeof g) == 0);
    }
  }

  /* GDEY0579F52: 2 x 26,928 bytes, no partials. */
  d = layout_display(792u, 272u, OD_COLOR_SCHEME_BWRY);
  d.frame_split = OD_FRAME_SPLIT_HALVES;
  d.row_order = OD_ROW_ORDER_FOLD;
  CHECK(od_color_display_geometry(&d, OD_COLOR_SCHEME_BWRY, &g) == OD_COLOR_OK);
  CHECK(g.layout == OD_COLOR_LAYOUT_SPLIT_HALVES && g.part_count == 2u);
  CHECK(g.part_bytes[0] == 26928u && g.part_bytes[1] == 26928u && !g.partial_supported);

  /* CrowPanel 0x0033: 400 / 392, column-major: 2 x 13,600 bytes. */
  d = layout_display(792u, 272u, OD_COLOR_SCHEME_MONO);
  d.frame_split = OD_FRAME_SPLIT_HALVES;
  d.frame_scan = OD_FRAME_SCAN_COLUMN_MAJOR;
  d.part_width = 400u;
  d.part_stride = 392u;
  CHECK(od_color_display_geometry(&d, OD_COLOR_SCHEME_MONO, &g) == OD_COLOR_OK);
  CHECK(g.part_bytes[0] == 13600u && g.part_bytes[1] == 13600u && g.total_bytes == 27200u);

  /* The former scheme 8 panel: BWGBRY + HALVES has the old split geometry. */
  d = layout_display(1200u, 1600u, OD_COLOR_SCHEME_BWGBRY);
  d.frame_split = OD_FRAME_SPLIT_HALVES;
  CHECK(od_color_display_geometry(&d, OD_COLOR_SCHEME_BWGBRY, &g) == OD_COLOR_OK);
  CHECK(g.part_width[0] == 600u && g.part_bytes[0] == 480000u && g.total_bytes == 960000u);
  CHECK(g.initial_plane == OD_COLOR_PLANE_NONE);

  /* plane_invert alone keeps the order, so partials stay where the scheme has them. */
  d = layout_display(16u, 4u, OD_COLOR_SCHEME_MONO);
  d.plane_invert = OD_PLANE_INVERT_0;
  CHECK(od_color_layout_is_linear(&d));
  CHECK(od_color_display_geometry(&d, OD_COLOR_SCHEME_MONO, &g) == OD_COLOR_OK);
  CHECK(g.partial_supported);
  d.frame_scan = OD_FRAME_SCAN_LSB_FIRST;
  CHECK(od_color_display_geometry(&d, OD_COLOR_SCHEME_MONO, &g) == OD_COLOR_OK);
  CHECK(!g.partial_supported);

  /* Refusals: each one would draw a wrong frame on a reader that ignored it. */
  d = layout_display(16u, 4u, OD_COLOR_SCHEME_BWRY);
  d.reserved[3] = 1u;
  CHECK(od_color_display_geometry(&d, OD_COLOR_SCHEME_BWRY, &g) == OD_COLOR_BAD_LAYOUT);
  d = layout_display(16u, 4u, OD_COLOR_SCHEME_BWRY);
  d.frame_split = 2u;
  CHECK(od_color_frame_layout(&d, OD_COLOR_SCHEME_BWRY, &L) == OD_COLOR_BAD_LAYOUT);
  d = layout_display(16u, 4u, OD_COLOR_SCHEME_BWRY);
  d.frame_scan = 0x10u;
  CHECK(od_color_frame_layout(&d, OD_COLOR_SCHEME_BWRY, &L) == OD_COLOR_BAD_LAYOUT);
  d = layout_display(16u, 4u, OD_COLOR_SCHEME_BWRY);
  d.plane_invert = 0x04u;
  CHECK(od_color_frame_layout(&d, OD_COLOR_SCHEME_BWRY, &L) == OD_COLOR_BAD_LAYOUT);
  d = layout_display(16u, 4u, OD_COLOR_SCHEME_BWRY);
  d.plane_invert = OD_PLANE_INVERT_1;        /* one-plane scheme */
  CHECK(od_color_display_geometry(&d, OD_COLOR_SCHEME_BWRY, &g) == OD_COLOR_BAD_LAYOUT);
  d = layout_display(16u, 4u, OD_COLOR_SCHEME_BWRY);
  d.pixel_pack = 2u;
  CHECK(od_color_frame_layout(&d, OD_COLOR_SCHEME_BWRY, &L) == OD_COLOR_BAD_LAYOUT);
  d = layout_display(16u, 4u, OD_COLOR_SCHEME_MONO);
  d.pixel_pack = OD_PIXEL_PACK_NIBBLE;       /* NIBBLE needs 2 bpp */
  CHECK(od_color_frame_layout(&d, OD_COLOR_SCHEME_MONO, &L) == OD_COLOR_BAD_LAYOUT);
  d = layout_display(16u, 4u, OD_COLOR_SCHEME_BWRY);
  d.row_order = 3u;
  CHECK(od_color_frame_layout(&d, OD_COLOR_SCHEME_BWRY, &L) == OD_COLOR_BAD_LAYOUT);
  d = layout_display(16u, 5u, OD_COLOR_SCHEME_BWRY);
  d.row_order = OD_ROW_ORDER_INTERLEAVE;     /* odd height */
  CHECK(od_color_frame_layout(&d, OD_COLOR_SCHEME_BWRY, &L) == OD_COLOR_BAD_LAYOUT);
  d = layout_display(16u, 4u, OD_COLOR_SCHEME_BWR);
  d.frame_split = OD_FRAME_SPLIT_HALVES;     /* split of a two-plane scheme */
  CHECK(od_color_frame_layout(&d, OD_COLOR_SCHEME_BWR, &L) == OD_COLOR_BAD_LAYOUT);
  d = layout_display(24u, 4u, OD_COLOR_SCHEME_MONO);
  d.frame_split = OD_FRAME_SPLIT_HALVES;
  d.part_stride = 12u;                       /* part 1 starts mid-byte */
  CHECK(od_color_frame_layout(&d, OD_COLOR_SCHEME_MONO, &L) == OD_COLOR_BAD_LAYOUT);
  d = layout_display(24u, 4u, OD_COLOR_SCHEME_MONO);
  d.part_width = 8u;                         /* part_width without a split */
  CHECK(od_color_frame_layout(&d, OD_COLOR_SCHEME_MONO, &L) == OD_COLOR_BAD_LAYOUT);
  d = layout_display(16u, 4u, OD_COLOR_SCHEME_BWGBRY_SPLIT);
  CHECK(od_color_display_geometry(&d, OD_COLOR_SCHEME_BWGBRY_SPLIT, &g) == OD_COLOR_UNSUPPORTED);
}

int main(void)
{
  classification_matrix();
  fixed_vectors();
  rejected_and_invalid();
  frame_layout_rules();
  frame_walk_matches_host_packer();
  printf("color: %d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
