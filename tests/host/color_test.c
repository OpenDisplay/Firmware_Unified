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
      check_geometry(OD_COLOR_SCHEME_BWGBRY_SPLIT, w, h, OD_COLOR_LAYOUT_SPLIT_HALVES, 4u,
                     2u, OD_COLOR_PLANE_NONE, false, w / 2u, w - w / 2u);
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
  CHECK(od_color_direct_geometry(OD_COLOR_SCHEME_BWGBRY_SPLIT, 122u, 250u, &g)
        == OD_COLOR_OK);
  CHECK(g.part_width[0] == 61u && g.part_width[1] == 61u);
  CHECK(g.part_bytes[0] == 7750u && g.part_bytes[1] == 7750u && g.total_bytes == 15500u);
}

static void rejected_and_invalid(void)
{
  static const uint8_t unsupported[] = {
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

int main(void)
{
  classification_matrix();
  fixed_vectors();
  rejected_and_invalid();
  printf("color: %d checks, %d failures\n", checks, failures);
  return failures == 0 ? 0 : 1;
}
