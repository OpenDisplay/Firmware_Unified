#include "od_boot_app.h"
#include "od_boot_screen.h"
#include "od_color.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static uint8_t g_segments;
static bool g_direct_2bpp;
static uint8_t g_scheme;
static int g_fail_begin_frame;
static int g_fail_begin_plane;
static int g_fail_write_at;
static int g_fail_end_plane;
static int g_fail_end_frame;
static unsigned g_begin_frames;
static unsigned g_begin_planes;
static unsigned g_writes;
static unsigned g_end_planes;
static unsigned g_end_frames;
static uint8_t g_last_segment;
static uint16_t g_last_y;
static int g_last_plane;
static uint32_t g_hash;
static int g_current_plane;
static uint8_t g_bwr_primary_black;
static uint8_t g_bwr_primary_color;
static uint8_t g_bwr_secondary_color;
static uint8_t g_gray4_plane[2][4];
static uint8_t g_gray4_direct[4];

static void reset_fake(void)
{
  g_segments = 1;
  g_direct_2bpp = false;
  g_scheme = OD_COLOR_SCHEME_MONO;
  g_fail_begin_frame = g_fail_begin_plane = g_fail_end_plane = g_fail_end_frame = 0;
  g_fail_write_at = -1;
  g_begin_frames = g_begin_planes = g_writes = g_end_planes = g_end_frames = 0;
  g_last_segment = 0;
  g_last_y = 0;
  g_last_plane = -1;
  g_hash = 2166136261u;
  g_current_plane = -1;
  g_bwr_primary_black = 1u;
  g_bwr_primary_color = 0u;
  g_bwr_secondary_color = 0u;
  memset(g_gray4_plane, 0xff, sizeof(g_gray4_plane));
  memset(g_gray4_direct, 0xff, sizeof(g_gray4_direct));
}

static void hash_byte(uint8_t value)
{
  g_hash ^= value;
  g_hash *= 16777619u;
}

int od_boot_app_begin_frame(uint16_t width, uint16_t height, uint8_t segments)
{
  g_begin_frames++;
  hash_byte((uint8_t)width); hash_byte((uint8_t)(width >> 8));
  hash_byte((uint8_t)height); hash_byte((uint8_t)(height >> 8));
  hash_byte(segments);
  return g_fail_begin_frame;
}

int od_boot_app_begin_plane(int plane)
{
  g_begin_planes++;
  g_current_plane = plane;
  hash_byte((uint8_t)(0xA0 | plane));
  return g_fail_begin_plane;
}

int od_boot_app_write_row(uint16_t y, uint8_t segment, const uint8_t *row, uint16_t len)
{
  if (g_writes != 0u && g_current_plane == g_last_plane) {
    assert(segment > g_last_segment || (segment == g_last_segment && y >= g_last_y));
  }
  g_last_segment = segment;
  g_last_y = y;
  g_last_plane = g_current_plane;
  hash_byte(segment); hash_byte((uint8_t)y); hash_byte((uint8_t)(y >> 8));
  hash_byte((uint8_t)len); hash_byte((uint8_t)(len >> 8));
  for (uint16_t i = 0; i < len; i++) hash_byte(row[i]);
  if (y == 280u && len > 41u) {
    if (g_current_plane == OD_BOOT_PLANE_PRIMARY) {
      g_bwr_primary_black = (uint8_t)((row[6] >> 5) & 1u);   /* x=50, black swatch */
      g_bwr_primary_color = (uint8_t)((row[41] >> 5) & 1u);  /* x=330, color swatch */
    } else if (g_current_plane == OD_BOOT_PLANE_SECOND) {
      g_bwr_secondary_color = (uint8_t)((row[41] >> 5) & 1u);
    }
    if (g_scheme == OD_COLOR_SCHEME_GRAY4) {
      static const uint16_t sample_x[4] = {50u, 150u, 250u, 350u};
      for (unsigned i = 0; i < 4u; i++) {
        uint16_t x = sample_x[i];
        if (g_direct_2bpp) {
          g_gray4_direct[i] = (uint8_t)((row[x >> 2] >> (6u - ((x & 3u) << 1u))) & 0x03u);
        } else if (g_current_plane == OD_BOOT_PLANE_PRIMARY ||
                   g_current_plane == OD_BOOT_PLANE_SECOND) {
          g_gray4_plane[g_current_plane][i] =
              (uint8_t)((row[x >> 3] >> (7u - (x & 7u))) & 0x01u);
        }
      }
    }
  }
  if ((int)g_writes == g_fail_write_at) {
    g_writes++;
    return -1;
  }
  g_writes++;
  return 0;
}

int od_boot_app_end_plane(int plane)
{
  g_end_planes++;
  hash_byte((uint8_t)(0xB0 | plane));
  return g_fail_end_plane;
}

int od_boot_app_end_frame(void)
{
  g_end_frames++;
  hash_byte(0xCF);
  return g_fail_end_frame;
}

int od_boot_app_bits_per_pixel(uint8_t scheme)
{
  g_scheme = scheme;
  if (scheme == OD_COLOR_SCHEME_BWRY || scheme == OD_COLOR_SCHEME_GRAY4) return 2;
  /* BWGBRY_SPLIT is 4 bpp too -- ../Firmware/src/display_service.cpp:1755-1756. Omitting it here
   * silently rendered the split case at 1 bpp, which is the same omission the renderer's own
   * scheme table had. A fake that is wrong the same way as the code proves nothing. */
  if (scheme == OD_COLOR_SCHEME_BWGBRY || scheme == OD_COLOR_SCHEME_BWGBRY_SPLIT ||
      scheme == OD_COLOR_SCHEME_GRAY16) return 4;
  return 1;
}

int od_boot_app_default_plane(uint8_t scheme)
{
  return scheme == OD_COLOR_SCHEME_MONO || scheme == OD_COLOR_SCHEME_GRAY16
      ? OD_BOOT_PLANE_PRIMARY : OD_BOOT_PLANE_SECOND;
}

bool od_boot_app_direct_2bpp(void) { return g_direct_2bpp; }
uint8_t od_boot_app_segments(void) { return g_segments; }
uint32_t od_boot_app_device_id24(void) { return 0xABCDEFu; }
void od_boot_app_firmware_version(uint8_t *a, uint8_t *b, uint8_t *c)
{
  *a = 1; *b = 2; *c = 3;
}
float od_boot_app_battery_volts(void) { return 3.14f; }
float od_boot_app_chip_temp_c(void) { return 22.5f; }

static void make_case(struct od_config *cfg, struct SecurityConfig *sec,
                      uint16_t width, uint16_t height, uint8_t scheme)
{
  memset(cfg, 0, sizeof(*cfg));
  memset(sec, 0, sizeof(*sec));
  cfg->display_count = 1;
  cfg->displays[0].pixel_width = width;
  cfg->displays[0].pixel_height = height;
  cfg->displays[0].color_scheme = scheme;
  cfg->displays[0].legacy_tag_type = 0x1234;
  cfg->manufacturer_data.manufacturer_id = 0x5678;
  for (unsigned i = 0; i < sizeof(sec->encryption_key); i++) sec->encryption_key[i] = (uint8_t)i;
  /* A key the device actually demands, and asked to be shown. Both are needed for the renderer
   * to print it: a key that is not in force is rendered as absent whatever the flag says. */
  sec->encryption_enabled = 1u;
  sec->flags = OD_SECURITY_FLAG_SHOW_KEY_ON_SCREEN;
}

static bool render(struct od_config *cfg, struct SecurityConfig *sec,
                   uint8_t *row, size_t row_len, uint8_t *qr, size_t qr_len)
{
  const struct od_boot_bufs bufs = {row, row_len, qr, qr_len};
  return od_boot_screen_render(cfg, sec, &bufs);
}

int main(void)
{
  struct od_config cfg;
  struct SecurityConfig sec;
  uint8_t row[600];
  /* The E1004 glass is natively 1200x1600 (bb_ep.inl:4157) and reaches 1600x1200 by rotation,
   * so pixel_width is 1200 and pitch is computed from the NATIVE width: 1200 at 4 bpp = 600 B.
   * Kept separate from row[] so the other cases keep the exact workspace they were pinned at. */
  uint8_t row_wide[600];
  uint8_t qr[256];

  reset_fake();
  make_case(&cfg, &sec, 296, 128, OD_COLOR_SCHEME_MONO);
  assert(render(&cfg, &sec, row, sizeof(row), qr, sizeof(qr)));
  assert(g_begin_frames == 1 && g_begin_planes == 1 && g_writes == 128);
  assert(g_end_planes == 1 && g_end_frames == 1);
  assert(g_hash == 0xBA916E4Cu);

  /* A stored key with encryption_enabled == 0 is not in force, so the key lines and the QR
   * payload render it as absent -- byte for byte what a device carrying no key at all renders.
   * Asserted as an identity rather than a second pinned hash so the two cannot drift apart. */
  reset_fake();
  make_case(&cfg, &sec, 296, 128, OD_COLOR_SCHEME_MONO);
  sec.encryption_enabled = 0u;
  assert(render(&cfg, &sec, row, sizeof(row), qr, sizeof(qr)));
  const uint32_t absent_key_hash = g_hash;
  assert(absent_key_hash != 0xBA916E4Cu);

  reset_fake();
  make_case(&cfg, &sec, 296, 128, OD_COLOR_SCHEME_MONO);
  memset(sec.encryption_key, 0, sizeof(sec.encryption_key));
  assert(render(&cfg, &sec, row, sizeof(row), qr, sizeof(qr)));
  assert(g_hash == absent_key_hash);

  reset_fake();
  make_case(&cfg, &sec, 296, 128, OD_COLOR_SCHEME_GRAY8);
  assert(!render(&cfg, &sec, row, sizeof(row), qr, sizeof(qr)));
  assert(g_begin_frames == 0 && g_begin_planes == 0 && g_writes == 0);

  reset_fake();
  make_case(&cfg, &sec, 2048, 8, OD_COLOR_SCHEME_MONO);
  assert(render(&cfg, &sec, row, 256, qr, sizeof(qr)));
  assert(g_begin_frames == 1);

  reset_fake();
  make_case(&cfg, &sec, 2056, 8, OD_COLOR_SCHEME_MONO);
  assert(!render(&cfg, &sec, row, 256, qr, sizeof(qr)));
  assert(g_begin_frames == 0 && g_writes == 0 && g_end_frames == 0);

  reset_fake();
  make_case(&cfg, &sec, 960, 8, OD_COLOR_SCHEME_GRAY4);
  assert(!render(&cfg, &sec, row, 256, qr, sizeof(qr)));
  assert(g_begin_frames == 0);

  reset_fake();
  make_case(&cfg, &sec, 960, 8, OD_COLOR_SCHEME_BWGBRY);
  assert(!render(&cfg, &sec, row, 256, qr, sizeof(qr)));
  assert(g_begin_frames == 0);

  reset_fake();
  make_case(&cfg, &sec, 304, 16, OD_COLOR_SCHEME_MONO);
  assert(!render(&cfg, &sec, row, sizeof(row), qr, 1));
  assert(g_begin_frames == 0);

  reset_fake();
  g_fail_write_at = 3;
  make_case(&cfg, &sec, 296, 16, OD_COLOR_SCHEME_MONO);
  assert(!render(&cfg, &sec, row, sizeof(row), qr, sizeof(qr)));
  assert(g_writes == 4 && g_end_planes == 1 && g_end_frames == 1);

  reset_fake();
  g_fail_begin_frame = -1;
  assert(!render(&cfg, &sec, row, sizeof(row), qr, sizeof(qr)));
  assert(g_begin_frames == 1 && g_begin_planes == 0 && g_end_frames == 0);

  reset_fake();
  g_fail_begin_plane = -1;
  assert(!render(&cfg, &sec, row, sizeof(row), qr, sizeof(qr)));
  assert(g_begin_planes == 1 && g_writes == 0 && g_end_planes == 0 && g_end_frames == 1);

  reset_fake();
  g_fail_end_plane = -1;
  assert(!render(&cfg, &sec, row, sizeof(row), qr, sizeof(qr)));
  assert(g_begin_planes == 1 && g_writes == 16 && g_end_planes == 1 && g_end_frames == 1);

  reset_fake();
  g_fail_end_frame = -1;
  assert(!render(&cfg, &sec, row, sizeof(row), qr, sizeof(qr)));
  assert(g_end_frames == 1);

  reset_fake();
  g_segments = 2;
  make_case(&cfg, &sec, 304, 16, OD_COLOR_SCHEME_MONO);
  assert(render(&cfg, &sec, row, sizeof(row), qr, sizeof(qr)));
  assert(g_begin_planes == 2 && g_writes == 32 && g_end_planes == 2 && g_end_frames == 1);

  reset_fake();
  make_case(&cfg, &sec, 400, 300, OD_COLOR_SCHEME_BWR);
  assert(render(&cfg, &sec, row, sizeof(row), qr, sizeof(qr)));
  assert(g_begin_planes == 2 && g_writes == 600);
  assert(g_bwr_primary_black == 0u);
  assert(g_bwr_primary_color == 1u);
  assert(g_bwr_secondary_color == 1u);
  /* Zone layout composition, not just its write count: this is the only case that renders the
   * QR right-placement B3 corrects and the swatch band B2 changes. Without it both can be
   * reverted silently -- the 296x128 case is below useZoneLayout and never reaches either. */
  assert(g_hash == 0xDDA0775Fu);

  /* EP368 0x0048 uses the V2 LUT. The packed swatches are converted back to the
   * controller's stored codes 3,2,1,0 across the two 1bpp planes. */
  reset_fake();
  make_case(&cfg, &sec, 400, 300, OD_COLOR_SCHEME_GRAY4);
  cfg.displays[0].panel_ic_type = 0x0048u;
  assert(render(&cfg, &sec, row, sizeof(row), qr, sizeof(qr)));
  assert(g_begin_planes == 2 && g_writes == 600);
  assert(g_gray4_plane[OD_BOOT_PLANE_PRIMARY][0] == 1u);
  assert(g_gray4_plane[OD_BOOT_PLANE_PRIMARY][1] == 0u);
  assert(g_gray4_plane[OD_BOOT_PLANE_PRIMARY][2] == 1u);
  assert(g_gray4_plane[OD_BOOT_PLANE_PRIMARY][3] == 0u);
  assert(g_gray4_plane[OD_BOOT_PLANE_SECOND][0] == 1u);
  assert(g_gray4_plane[OD_BOOT_PLANE_SECOND][1] == 1u);
  assert(g_gray4_plane[OD_BOOT_PLANE_SECOND][2] == 0u);
  assert(g_gray4_plane[OD_BOOT_PLANE_SECOND][3] == 0u);

  /* A direct packed-2bpp target consumes logical gray levels, not the bb_epaper
   * controller LUT codes. A base-LUT panel distinguishes those two mappings. */
  reset_fake();
  g_direct_2bpp = true;
  make_case(&cfg, &sec, 400, 300, OD_COLOR_SCHEME_GRAY4);
  cfg.displays[0].panel_ic_type = 0x0043u;
  assert(render(&cfg, &sec, row, sizeof(row), qr, sizeof(qr)));
  assert(g_begin_planes == 1 && g_writes == 300);
  assert(g_gray4_direct[0] == 0u && g_gray4_direct[1] == 1u &&
         g_gray4_direct[2] == 2u && g_gray4_direct[3] == 3u);

  /* 800x480, the fleet's most common panel and the smallest geometry whose logo selector can
   * reach S2: it needs w_log >= 514 and headerH >= 96, and 400x300 gives maxLogoW 120 against
   * S2's 154 width, so S1 is taken there as the unconditional fallback whatever the height test
   * says. Logo size selection is unpinned without this case. */
  reset_fake();
  make_case(&cfg, &sec, 800, 480, OD_COLOR_SCHEME_BWR);
  assert(render(&cfg, &sec, row, sizeof(row), qr, sizeof(qr)));
  assert(g_begin_planes == 2 && g_writes == 960);
  assert(g_hash == 0x3347F980u);

  /* BWGBRY_SPLIT is scheme 8 and was the one scheme with no case. It needs its own: the local
   * scheme enum this renderer replaced had no entry for it, so it fell past the end of
   * kSchemeWhiteValue to the 0xFF fallback instead of 0x11, and past the swatch switch to
   * default. Both are packed-4bpp values a count-only assertion cannot see, and its "6c split"
   * label was missing too. 1600x1200 is the real dual-controller geometry (the E1004's 13.3"
   * Spectra6 glass); it also renders the footer scheme text, which 400x300 does not, so the
   * label is unpinned at the smaller size. segments = 2 because a split-buffer scheme is
   * dual-controller by definition. */
  reset_fake();
  g_segments = 2;
  make_case(&cfg, &sec, 1200, 1600, OD_COLOR_SCHEME_BWGBRY_SPLIT);
  cfg.displays[0].rotation = 1;   /* landscape: w_log 1600, h_log 1200 */
  assert(render(&cfg, &sec, row_wide, sizeof(row_wide), qr, sizeof(qr)));
  assert(g_begin_planes == 2 && g_writes == 3200 && g_end_frames == 1);
  /* Moved when large panels got a QR floor (od_boot_screen.c): the QR here was 3-pixel modules
   * beside scale-8 text, and is now larger with smaller text. */
  assert(g_hash == 0x2670EA32u);
  /* 960x672 (the Telink 9.7" dual-controller BWR): the geometry that exposed the missing QR
   * floor -- scale-8 text left the QR 1-pixel modules. */
  reset_fake();
  make_case(&cfg, &sec, 960, 672, OD_COLOR_SCHEME_BWR);
  assert(render(&cfg, &sec, row, sizeof(row), qr, sizeof(qr)));
  assert(g_begin_planes == 2 && g_writes == 1344);
  assert(g_hash == 0xF80EDB48u);
  /* One byte short of the full pitch is refused, not silently half-rendered. */
  reset_fake();
  g_segments = 2;
  make_case(&cfg, &sec, 1200, 1600, OD_COLOR_SCHEME_BWGBRY_SPLIT);
  cfg.displays[0].rotation = 1;
  assert(!render(&cfg, &sec, row_wide, 599, qr, sizeof(qr)));
  assert(g_begin_frames == 0 && g_writes == 0);

  puts("boot_screen_test: ok");
  return 0;
}
