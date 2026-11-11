#include "od_boot_app.h"
#include "od_boot_screen.h"
#include "od_color.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
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

/* The primary plane of a 1-segment frame, kept so a test can look at the picture itself. */
#define CAP_MAX (800u * 480u / 8u)
static uint8_t g_cap[CAP_MAX];
static uint16_t g_cap_pitch;
static uint16_t g_cap_rows;

/* Golden hashes. OD_PIN=1 prints the actual values instead of asserting them (re-pinning after
 * a deliberate layout change). */
#define PIN(v) do { \
    if (getenv("OD_PIN") != NULL) printf("boot_screen_test.c:%d hash 0x%08X\n", __LINE__, (unsigned)g_hash); \
    else assert(g_hash == (v)); \
  } while (0)

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
  memset(g_cap, 0xff, sizeof(g_cap));
  g_cap_pitch = 0;
  g_cap_rows = 0;
}

/* 1bpp mono capture: bit 0 = black. */
static bool cap_black(unsigned x, unsigned y)
{
  return ((g_cap[(size_t)y * g_cap_pitch + x / 8u] >> (7u - (x % 8u))) & 1u) == 0u;
}

static unsigned cap_black_count(void)
{
  unsigned n = 0;
  for (unsigned y = 0; y < g_cap_rows; y++)
    for (unsigned x = 0; x < (unsigned)g_cap_pitch * 8u; x++) n += cap_black(x, y) ? 1u : 0u;
  return n;
}

/* Finder patterns at module size m: a 7x7-module block (black ring, white ring, black 3x3) with a
 * white one-module separator, matched pixel for pixel. A QR has exactly three. */
static bool finder_at(unsigned x0, unsigned y0, unsigned m)
{
  const unsigned W = (unsigned)g_cap_pitch * 8u;
  if (x0 < m || y0 < m || x0 + 8u * m > W || y0 + 8u * m > g_cap_rows) return false;
  for (unsigned my = 0; my < 9u; my++) {
    for (unsigned mx = 0; mx < 9u; mx++) {
      /* module coords -1..7 relative to the pattern */
      const int ix = (int)mx - 1, iy = (int)my - 1;
      bool want;
      if (ix < 0 || iy < 0 || ix > 6 || iy > 6) want = false;
      else if (ix == 0 || iy == 0 || ix == 6 || iy == 6) want = true;
      else if (ix == 1 || iy == 1 || ix == 5 || iy == 5) want = false;
      else want = true;
      for (unsigned py = 0; py < m; py++)
        for (unsigned px = 0; px < m; px++)
          if (cap_black(x0 + (unsigned)(ix * (int)m) + px, y0 + (unsigned)(iy * (int)m) + py) != want)
            return false;
    }
  }
  return true;
}

static unsigned cap_qr_module(void)
{
  const unsigned W = (unsigned)g_cap_pitch * 8u;
  for (unsigned m = 1; m <= 8u; m++) {
    unsigned found = 0;
    for (unsigned y = 1; y < g_cap_rows; y++)
      for (unsigned x = 1; x < W; x++)
        if (cap_black(x, y) && !cap_black(x - 1u, y) && !cap_black(x, y - 1u) && finder_at(x, y, m))
          found++;
    if (found == 3u) return m;
  }
  return 0;
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
  if (segment == 0u && g_current_plane == OD_BOOT_PLANE_PRIMARY &&
      (size_t)(y + 1u) * len <= sizeof g_cap) {
    memcpy(&g_cap[(size_t)y * len], row, len);
    g_cap_pitch = len;
    if (y + 1u > g_cap_rows) g_cap_rows = (uint16_t)(y + 1u);
  }
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
  PIN(0x3040D050u);

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
  PIN(0x16867395u);

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
  PIN(0x089FE8A5u);

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
  PIN(0xE212C6C2u);
  /* One byte short of the full pitch is refused, not silently half-rendered. */
  reset_fake();
  g_segments = 2;
  make_case(&cfg, &sec, 1200, 1600, OD_COLOR_SCHEME_BWGBRY_SPLIT);
  cfg.displays[0].rotation = 1;
  assert(!render(&cfg, &sec, row_wide, 599, qr, sizeof(qr)));
  assert(g_begin_frames == 0 && g_writes == 0);

  /* ---- the setup screen family (SECURITY_V2 7.8) ---- */
  {
    const struct od_boot_bufs bufs = {row, sizeof(row), qr, sizeof(qr)};
    static const enum od_setup_screen kinds[3] = {
      OD_SETUP_SCREEN_SETUP, OD_SETUP_SCREEN_CODE, OD_SETUP_SCREEN_RESET};
    uint32_t h296[3], h400[3], h800[3];

    /* NORMAL is not a setup screen: refused before any output. */
    reset_fake();
    make_case(&cfg, &sec, 296, 128, OD_COLOR_SCHEME_MONO);
    assert(!od_boot_screen_render_setup(&cfg, OD_SETUP_SCREEN_NORMAL, 0u, &bufs));
    assert(g_begin_frames == 0);

    for (unsigned k = 0; k < 3u; k++) {
      reset_fake();
      make_case(&cfg, &sec, 296, 128, OD_COLOR_SCHEME_MONO);
      assert(od_boot_screen_render_setup(&cfg, kinds[k], 123456u, &bufs));
      assert(g_begin_frames == 1 && g_begin_planes == 1 && g_writes == 128 && g_end_frames == 1);
      assert(cap_black_count() > 0u);
      h296[k] = g_hash;

      reset_fake();
      make_case(&cfg, &sec, 400, 300, OD_COLOR_SCHEME_BWR);
      assert(od_boot_screen_render_setup(&cfg, kinds[k], 123456u, &bufs));
      /* BWR: the black plane, then the spot plane sent blank */
      assert(g_begin_planes == 2 && g_writes == 600);
      assert(g_bwr_secondary_color == 0u);
      h400[k] = g_hash;

      reset_fake();
      make_case(&cfg, &sec, 800, 480, OD_COLOR_SCHEME_MONO);
      assert(od_boot_screen_render_setup(&cfg, kinds[k], 123456u, &bufs));
      assert(g_writes == 480);
      h800[k] = g_hash;
    }
    /* Three different screens at every size. */
    assert(h296[0] != h296[1] && h296[1] != h296[2] && h296[0] != h296[2]);
    assert(h400[0] != h400[1] && h400[1] != h400[2] && h400[0] != h400[2]);
    assert(h800[0] != h800[1] && h800[1] != h800[2] && h800[0] != h800[2]);
    g_hash = h296[0]; PIN(0xE4A6088Bu);
    g_hash = h296[1]; PIN(0x7121A3E9u);
    g_hash = h296[2]; PIN(0x2B5071EAu);
    g_hash = h400[0]; PIN(0xE6728E27u);
    g_hash = h400[1]; PIN(0x32338461u);
    g_hash = h400[2]; PIN(0x195C898Bu);
    g_hash = h800[0]; PIN(0xC06ECB0Bu);
    g_hash = h800[1]; PIN(0x642C4AD3u);
    g_hash = h800[2]; PIN(0xD4FEFD45u);

    /* The code is the variable input of CODE, and only of CODE. */
    reset_fake();
    make_case(&cfg, &sec, 800, 480, OD_COLOR_SCHEME_MONO);
    assert(od_boot_screen_render_setup(&cfg, OD_SETUP_SCREEN_CODE, 654321u, &bufs));
    assert(g_hash != h800[1]);
    reset_fake();
    make_case(&cfg, &sec, 800, 480, OD_COLOR_SCHEME_MONO);
    assert(od_boot_screen_render_setup(&cfg, OD_SETUP_SCREEN_SETUP, 654321u, &bufs));
    assert(g_hash == h800[0]);

    /* No config text reaches these screens (review A1): a 0x2C header and the key change
     * nothing. */
#if OD_CONFIG_WITH_DATA_EXTENDED
    for (unsigned k = 0; k < 3u; k++) {
      reset_fake();
      make_case(&cfg, &sec, 800, 480, OD_COLOR_SCHEME_MONO);
      cfg.data_extended_loaded = true;
      memcpy(cfg.data_extended.manufacturer_name, "EVIL 000 000", 13);
      memcpy(cfg.data_extended.model_name, "TYPE 999 999", 13);
      cfg.security.encryption_enabled = 1u;
      memset(cfg.security.encryption_key, 0x5A, 16);
      assert(od_boot_screen_render_setup(&cfg, kinds[k], 123456u, &bufs));
      assert(g_hash == h800[k]);
    }
#endif

    /* The QR inputs are the simple-config indices: SETUP / RESET change with them, CODE not. */
    reset_fake();
    make_case(&cfg, &sec, 800, 480, OD_COLOR_SCHEME_MONO);
    cfg.manufacturer_data.simple_config_driver_index = 16u;
    assert(od_boot_screen_render_setup(&cfg, OD_SETUP_SCREEN_SETUP, 0u, &bufs));
    assert(g_hash != h800[0]);
    reset_fake();
    make_case(&cfg, &sec, 800, 480, OD_COLOR_SCHEME_MONO);
    cfg.manufacturer_data.simple_config_driver_index = 16u;
    assert(od_boot_screen_render_setup(&cfg, OD_SETUP_SCREEN_CODE, 123456u, &bufs));
    assert(g_hash == h800[1]);

    /* QR v6 + quiet zone = 49 modules: 4 px/module on 200x200, 3 on 152x152, 2 on a 128-px-wide
     * 2.9in (SECURITY_V2 7.8), measured on the picture through the finder patterns. */
    {
      static const struct { uint16_t w, h; uint8_t rot; unsigned module; } q[] = {
        {200, 200, 0, 4}, {152, 152, 0, 3}, {128, 296, 0, 2}, {128, 296, 1, 2},
      };
      for (unsigned i = 0; i < sizeof q / sizeof q[0]; i++) {
        for (unsigned k = 0; k < 3u; k += 2u) {
          reset_fake();
          make_case(&cfg, &sec, q[i].w, q[i].h, OD_COLOR_SCHEME_MONO);
          cfg.displays[0].rotation = q[i].rot;
          assert(od_boot_screen_render_setup(&cfg, kinds[k], 0u, &bufs));
          if (cap_qr_module() != q[i].module) {
            fprintf(stderr, "QR module: %ux%u rot %u kind %u: %u px, want %u\n",
                    q[i].w, q[i].h, q[i].rot, (unsigned)kinds[k], cap_qr_module(), q[i].module);
            assert(0);
          }
        }
      }
    }

    /* CODE on the smallest panels still shows the digits: the code wins. */
    {
      static const uint16_t sz[][2] = {{200, 200}, {152, 152}, {128, 296}, {296, 128}, {96, 48}};
      for (unsigned i = 0; i < sizeof sz / sizeof sz[0]; i++) {
        reset_fake();
        make_case(&cfg, &sec, sz[i][0], sz[i][1], OD_COLOR_SCHEME_MONO);
        assert(od_boot_screen_render_setup(&cfg, OD_SETUP_SCREEN_CODE, 123456u, &bufs));
        assert(cap_black_count() > 0u);
      }
    }

    /* Every packing the normal screen supports renders the family too. */
    {
      static const uint8_t schemes[] = {OD_COLOR_SCHEME_BWY, OD_COLOR_SCHEME_BWRY, OD_COLOR_SCHEME_BWGBRY,
                                        OD_COLOR_SCHEME_GRAY4, OD_COLOR_SCHEME_GRAY16};
      for (unsigned i = 0; i < sizeof schemes; i++) {
        reset_fake();
        make_case(&cfg, &sec, 400, 300, schemes[i]);
        assert(od_boot_screen_render_setup(&cfg, OD_SETUP_SCREEN_SETUP, 0u, &bufs));
        assert(g_end_frames == 1);
      }
      reset_fake();
      make_case(&cfg, &sec, 296, 128, OD_COLOR_SCHEME_GRAY8);
      assert(!od_boot_screen_render_setup(&cfg, OD_SETUP_SCREEN_SETUP, 0u, &bufs));
      reset_fake();
      make_case(&cfg, &sec, 296, 128, OD_COLOR_SCHEME_MONO);
      assert(!od_boot_screen_render_setup(&cfg, OD_SETUP_SCREEN_SETUP, 0u,
                                          &(const struct od_boot_bufs){row, sizeof(row), qr, 1}));
      assert(g_begin_frames == 0);
    }
  }

  puts("boot_screen_test: ok");
  return 0;
}
