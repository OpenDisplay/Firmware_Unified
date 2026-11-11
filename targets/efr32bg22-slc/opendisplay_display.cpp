#include "opendisplay_display.h"
#include "od_diag.h"
#include "od_fmt.h"
#include "od_color.h"
#include "opendisplay_ble.h"
#include "opendisplay_config_parser.h"
#include "opendisplay_constants.h"
#include "opendisplay_runtime.h"
#include "od_epd_shim.h"
#include "od_epd.h"
#include "od_cmd_flash.h"
#include "od_imgstore.h"
#include "od_boot_payload.h"
#include "od_claim_mode.h"
#include "od_sec2_app.h"
#include "od_claim_app.h"
#include "od_caps.h"
#include "od_txq.h"
#include "od_xfer.h"
#include "od_xfer_app.h"
#include "od_link_policy.h"
#include "opendisplay_pipe.h"
#include "opendisplay_protocol.h"
#include "em_cmu.h"
#include "em_gpio.h"
#include "em_system.h"
#include "qrcode/qrcode.h"
#include "od_hal_time.h"
#include "sl_sleeptimer.h"
#include <stdio.h>
#include <string.h>

#define OPENDISPLAY_DECOMPRESSION_CHUNK_SIZE 256u

static bool s_boot_applied;
static uint8_t s_decompression_chunk[OPENDISPLAY_DECOMPRESSION_CHUNK_SIZE];

enum XferAppMode {
  XFER_APP_IDLE = 0,
  XFER_APP_FULL,
  /* Same byte stream as FULL, but the panel is already up at a named mode and the stream
   * carries several frames. Kept distinct so the pass advance cannot fire on a 0x0070. */
  XFER_APP_DRIVE,
};

struct XferAppHardwareState {
  XferAppMode mode;
  od_color_geometry_t geometry;
  uint32_t plane_size;
  uint8_t current_plane;
  bool panel_up;
  /* Multi-pass drive state, all zero on the ordinary 0x0070 path. `frame_bytes` is 0 for a
   * single-pass transfer, which is what makes the pass-advance check below inert there. */
  uint32_t frame_bytes;
  uint8_t drive_passes;
  uint8_t drive_pass;
  uint8_t drive_mode;
  /* Set when CMD_FLASH_PRELOAD armed this upload: bytes go to a flash slot and the panel is
   * never touched, so there is no refresh and no 20-second wait. */
  bool to_flash;
};

static XferAppHardwareState s_xfer_app;

/* -1 = nothing queued. Set by a command, consumed by the loop. */
static int16_t s_pending_slot = -1;

/* The rail has to be up before the controller is addressed. Without it a full frame is clocked
 * into an unpowered part, every write reports success, and the only symptom is the busy-wait
 * timeout. Charged once per switch-on, so an always-on rail never pays it. */
#define OD_PANEL_RAIL_SETTLE_MS 800u

static const struct DisplayConfig *display_cfg(void)
{
  const struct GlobalConfig *cfg = opendisplay_get_global_config();
  if (cfg == nullptr || cfg->display_count == 0u) {
    return nullptr;
  }
  return &cfg->displays[0];
}

static bool decode_pin(uint8_t v, GPIO_Port_TypeDef *port_out, uint8_t *pin_out)
{
  unsigned pr;
  unsigned pn;
  if (v == GPIO_PIN_UNUSED) {
    return false;
  }
  pr = (unsigned)(v >> 4) & 0x0Fu;
  pn = (unsigned)(v & 0x0Fu);
  if (pr > (unsigned)GPIO_PORT_MAX || pn > 15u) {
    return false;
  }
  *port_out = (GPIO_Port_TypeDef)(gpioPortA + pr);
  *pin_out = (uint8_t)pn;
  return true;
}

static void display_park_signal_pin(uint8_t pin_cfg)
{
  GPIO_Port_TypeDef port;
  uint8_t pin;

  if (!decode_pin(pin_cfg, &port, &pin)) {
    return;
  }
  GPIO_PinModeSet(port, pin, gpioModeDisabled, 0);
}

void opendisplay_display_park_pins(void)
{
  const struct DisplayConfig *d = display_cfg();

  if (d == nullptr) {
    return;
  }
  CMU_ClockEnable(cmuClock_GPIO, true);
  display_park_signal_pin(d->cs_pin);
  display_park_signal_pin(d->data_pin);
  display_park_signal_pin(d->clk_pin);
  display_park_signal_pin(d->dc_pin);
  display_park_signal_pin(d->reset_pin);
  display_park_signal_pin(d->busy_pin);
}

static bool s_rail_on;

static void display_power_set(bool on)
{
  const struct GlobalConfig *cfg = opendisplay_get_global_config();
  GPIO_Port_TypeDef port;
  uint8_t pin;
  if (cfg == nullptr) {
    return;
  }
  CMU_ClockEnable(cmuClock_GPIO, true);
  if (!on) {
    opendisplay_display_park_pins();
  }
  /* 0xFF is the contract's documented "no rail to switch". Substituting a pin drives whatever
   * that pad is wired to on a board that never asked for a rail. */
  if (cfg->system_config.pwr_pin == GPIO_PIN_UNUSED) {
    return;
  }
  if (!decode_pin(cfg->system_config.pwr_pin, &port, &pin)) {
    return;
  }
  GPIO_PinModeSet(port, pin, gpioModePushPull, on ? 1u : 0u);
  if (on && !s_rail_on) {
    sl_sleeptimer_delay_millisecond(OD_PANEL_RAIL_SETTLE_MS);
  }
  s_rail_on = on;
}

void opendisplay_display_power_off(void)
{
  display_power_set(false);
}

typedef struct {
  char c;
  uint8_t col[5];
} Glyph5x7;

static const Glyph5x7 s_font5x7[] = {
  { ' ', { 0, 0, 0, 0, 0 } },       { '.', { 0, 0, 0x40, 0, 0 } }, { ':', { 0, 0x24, 0, 0, 0 } },
  { '0', { 0x3E, 0x51, 0x49, 0x45, 0x3E } },
  { '1', { 0x00, 0x42, 0x7F, 0x40, 0x00 } }, { '2', { 0x62, 0x51, 0x49, 0x49, 0x46 } },
  { '3', { 0x22, 0x49, 0x49, 0x49, 0x36 } }, { '4', { 0x18, 0x14, 0x12, 0x7F, 0x10 } },
  { '5', { 0x2F, 0x49, 0x49, 0x49, 0x31 } }, { '6', { 0x3E, 0x49, 0x49, 0x49, 0x32 } },
  { '7', { 0x01, 0x71, 0x09, 0x05, 0x03 } }, { '8', { 0x36, 0x49, 0x49, 0x49, 0x36 } },
  { '9', { 0x26, 0x49, 0x49, 0x49, 0x3E } }, { 'A', { 0x7E, 0x11, 0x11, 0x11, 0x7E } },
  { 'B', { 0x7F, 0x49, 0x49, 0x49, 0x36 } }, { 'C', { 0x3E, 0x41, 0x41, 0x41, 0x22 } },
  { 'D', { 0x7F, 0x41, 0x41, 0x22, 0x1C } }, { 'E', { 0x7F, 0x49, 0x49, 0x49, 0x41 } },
  { 'F', { 0x7F, 0x09, 0x09, 0x09, 0x01 } }, { 'G', { 0x3E, 0x41, 0x49, 0x49, 0x7A } },
  { 'I', { 0x00, 0x41, 0x7F, 0x41, 0x00 } }, { 'L', { 0x7F, 0x40, 0x40, 0x40, 0x40 } },
  { 'N', { 0x7F, 0x02, 0x0C, 0x10, 0x7F } }, { 'O', { 0x3E, 0x41, 0x41, 0x41, 0x3E } },
  { 'P', { 0x7F, 0x09, 0x09, 0x09, 0x06 } }, { 'R', { 0x7F, 0x09, 0x19, 0x29, 0x46 } },
  { 'S', { 0x26, 0x49, 0x49, 0x49, 0x32 } }, { 'W', { 0x3F, 0x40, 0x38, 0x40, 0x3F } },
  { 'Y', { 0x07, 0x08, 0x70, 0x08, 0x07 } },
  /* the setup screen family's words (SECURITY_V2 7.8), upper case only */
  { 'H', { 0x7F, 0x08, 0x08, 0x08, 0x7F } }, { 'K', { 0x7F, 0x08, 0x14, 0x22, 0x41 } },
  { 'M', { 0x7F, 0x02, 0x04, 0x02, 0x7F } }, { 'T', { 0x01, 0x01, 0x7F, 0x01, 0x01 } },
  { 'U', { 0x3F, 0x40, 0x40, 0x40, 0x3F } }, { ',', { 0x00, 0x50, 0x30, 0x00, 0x00 } },
  { '?', { 0x02, 0x01, 0x51, 0x09, 0x06 } },
};

static const uint8_t *glyph5x7(char c)
{
  unsigned i;
  if (c >= 'a' && c <= 'z') {
    c = (char)(c - 'a' + 'A');   /* the font is upper case: "Open" draws as "OPEN" */
  }
  for (i = 0; i < (sizeof(s_font5x7) / sizeof(s_font5x7[0])); i++) {
    if (s_font5x7[i].c == c) {
      return s_font5x7[i].col;
    }
  }
  return s_font5x7[0].col;
}

static void set_pixel_row(uint8_t *row, int x, int bits_per_pixel)
{
  if (bits_per_pixel == 4) {
    row[x >> 1] &= (uint8_t)~(0xF0u >> ((unsigned)(x & 1) * 4u));
  } else if (bits_per_pixel == 2) {
    row[x >> 2] &= (uint8_t)~(0xC0u >> ((unsigned)(x & 3) * 2u));
  } else {
    row[x >> 3] &= (uint8_t)~(0x80u >> ((unsigned)x & 7u));
  }
}

static void make_gray4_plane_row(uint8_t *plane_row, const uint8_t *row_2bpp,
                                 int plane_pitch, uint16_t width, int bit_select)
{
  memset(plane_row, 0x00, (size_t)plane_pitch);
  for (uint16_t x = 0; x < width; x++) {
    uint8_t code = (uint8_t)((~(row_2bpp[x >> 2] >> (6 - ((x & 3u) << 1)))) & 0x03u);
    if (((code >> bit_select) & 0x01u) != 0u) {
      plane_row[x >> 3] |= (uint8_t)(0x80u >> (x & 7u));
    }
  }
}

static void draw_text_row(uint8_t *row, int y, int x0, int y0, const char *s, int scale, int bits_per_pixel,
                          int max_x)
{
  int cursor = x0;
  const char *p;
  if (s == nullptr || scale <= 0) {
    return;
  }
  for (p = s; *p != '\0'; p++) {
    const uint8_t *g = glyph5x7(*p);
    int col, gy, sx;
    for (col = 0; col < 5; col++) {
      for (gy = 0; gy < 7; gy++) {
        int py, px;
        if (((g[col] >> gy) & 1u) == 0u) {
          continue;
        }
        py = y0 + gy * scale;
        if (y < py || y >= py + scale) {
          continue;
        }
        px = cursor + col * scale;
        for (sx = 0; sx < scale; sx++) {
          int rx = px + sx;
          if (rx >= 0 && (max_x < 0 || rx < max_x)) {
            set_pixel_row(row, rx, bits_per_pixel);
          }
        }
      }
    }
    cursor += 6 * scale;
  }
}

static uint16_t text_width_px(const char *s, int scale)
{
  if (s == nullptr || scale <= 0) {
    return 0u;
  }
  return (uint16_t)(strlen(s) * (size_t)(6 * scale));
}

/* Boot rendering is intentionally target-local. It is not direct-stream authority and it
 * retains the BG22 renderer's fixed 256-byte row workspace and silent failure policy. */
static int boot_bits_per_pixel(uint8_t color_scheme)
{
  switch (color_scheme) {
    case OD_COLOR_SCHEME_BWGBRY:
    case OD_COLOR_SCHEME_BWGBRY_SPLIT:
    case OD_COLOR_SCHEME_GRAY16:
      return 4;
    case OD_COLOR_SCHEME_BWRY:
    case OD_COLOR_SCHEME_GRAY4:
      return 2;
    case OD_COLOR_SCHEME_MONO:
    case OD_COLOR_SCHEME_BWR:
    case OD_COLOR_SCHEME_BWY:
    case OD_COLOR_SCHEME_SEVEN_COLOR:
      return 1;
    default:
      return 0;
  }
}

static int boot_line_step(int scale)
{
  return scale * 10;
}

/* One screen: up to 6 text lines (the `big` one at a larger scale) and an optional QR. Shared by
 * the normal boot screen and the setup family (SECURITY_V2 7.8), whose content comes from
 * od_setup_content_build(); only the layout is target-local. */
#define BOOT_LINES_MAX 6u
#define BOOT_NO_BIG    0xFFu

struct boot_layout {
  int scale[BOOT_LINES_MAX];
  int x[BOOT_LINES_MAX];
  int y[BOOT_LINES_MAX];
  bool show[BOOT_LINES_MAX];
  int module_px, qr_px, qr_x, qr_y;   /* module_px 0: no QR */
};

static int line_scale(unsigned i, unsigned big, int s, int big_s)
{
  return i == big ? big_s : s;
}

/* Height (no gap under the last line) and widest width of the shown lines. */
static void boot_block(const char *const *lines, unsigned n, const bool *show, unsigned big, int s,
                       int big_s, int *h_out, int *w_out)
{
  int h = 0, w = 0, last = 0;
  for (unsigned i = 0; i < n; i++) {
    if (!show[i]) {
      continue;
    }
    int si = line_scale(i, big, s, big_s);
    int tw = (int)text_width_px(lines[i], si);
    h += boot_line_step(si);
    last = si;
    if (tw > w) {
      w = tw;
    }
  }
  *h_out = h - 3 * last;
  *w_out = w;
}

/* Place the shown lines centred in [x0, x0 + area_w), from y0 down. */
static void boot_place(struct boot_layout *L, const char *const *lines, unsigned n, const bool *show,
                       unsigned big, int s, int big_s, int x0, int area_w, int y0)
{
  int y = y0;
  for (unsigned i = 0; i < n; i++) {
    if (!show[i]) {
      continue;
    }
    int si = line_scale(i, big, s, big_s);
    int x = x0 + (area_w - (int)text_width_px(lines[i], si)) / 2;
    L->scale[i] = si;
    L->x[i] = x < 0 ? 0 : x;
    L->y[i] = y;
    L->show[i] = true;
    y += boot_line_step(si);
  }
}

/* qr_first: the reduced setup layout -- the QR at its largest module, text only where it still
 * fits. Otherwise the text scale is tried from 2 down with the QR beside or below it. */
static bool boot_layout(uint16_t w, uint16_t h, const char *const *lines, unsigned n, unsigned big,
                        int qr_modules, bool qr_first, struct boot_layout *L)
{
  bool show[BOOT_LINES_MAX];
  int min_dim = (int)(w < h ? w : h);

  memset(L, 0, sizeof *L);
  for (unsigned i = 0; i < BOOT_LINES_MAX; i++) {
    show[i] = i < n;
  }
  for (int s = (w >= 400u && h >= 300u && !qr_first) ? 2 : 1; s >= 1; s--) {
    int pad = 6 * s;
    int big_s = s;
    int block_h, max_w;
    if (big != BOOT_NO_BIG) {
      int by_w = ((int)w - 2 * pad) / (int)(6u * strlen(lines[big]));
      int by_h = (int)h / 30;
      big_s = by_w < by_h ? by_w : by_h;
      if (big_s > 4 * s) {
        big_s = 4 * s;
      }
      if (big_s < s) {
        big_s = s;
      }
    }
    boot_block(lines, n, show, big, s, big_s, &block_h, &max_w);
    if (qr_modules == 0) {
      if (max_w <= (int)w - 2 * pad && block_h <= (int)h - 2 * pad) {
        boot_place(L, lines, n, show, big, s, big_s, 0, (int)w, ((int)h - block_h) / 2);
        return true;
      }
      continue;
    }
    int module_px = (qr_first ? min_dim : min_dim - 2 * pad) / qr_modules;
    if (module_px > 6) {
      module_px = 6;
    }
    for (; module_px >= 1; module_px--) {
      int qr_px = module_px * qr_modules;
      int side = qr_first ? 0 : pad;
      L->module_px = module_px;
      L->qr_px = qr_px;
      if ((int)w - qr_px - side >= max_w + 2 * pad && (int)h >= block_h + 2 * pad &&
          (int)h >= qr_px + 2 * side) {
        /* text left, QR right */
        L->qr_x = (int)w - side - qr_px;
        L->qr_y = ((int)h - qr_px) / 2;
        boot_place(L, lines, n, show, big, s, big_s, 0, L->qr_x, ((int)h - block_h) / 2);
        return true;
      }
      if ((int)h - qr_px - side >= block_h + 2 * pad && (int)w >= max_w + 2 * pad &&
          (int)w >= qr_px + 2 * side) {
        /* text above, QR below */
        L->qr_x = ((int)w - qr_px) / 2;
        L->qr_y = (int)h - side - qr_px;
        boot_place(L, lines, n, show, big, s, big_s, 0, (int)w, (L->qr_y - block_h) / 2);
        return true;
      }
      if (qr_first) {
        break;      /* the module is fixed: the text goes instead */
      }
    }
  }
  if (qr_modules != 0) {
    /* Nothing fits beside it: the QR alone. */
    int module_px = min_dim / qr_modules;
    if (module_px > 6) {
      module_px = 6;
    }
    if (module_px < 1) {
      return false;
    }
    memset(L, 0, sizeof *L);
    L->module_px = module_px;
    L->qr_px = module_px * qr_modules;
    L->qr_x = ((int)w - L->qr_px) / 2;
    L->qr_y = ((int)h - L->qr_px) / 2;
    return true;
  }
  if (big == BOOT_NO_BIG) {
    return false;
  }
  /* The code wins: the digits alone, at the largest scale that fits. */
  int big_s = ((int)w - 12) / (int)(6u * strlen(lines[big]));
  if (big_s < 1) {
    return false;
  }
  if (big_s > (int)h / 10) {
    big_s = (int)h / 10 > 0 ? (int)h / 10 : 1;
  }
  L->scale[big] = big_s;
  L->x[big] = ((int)w - (int)text_width_px(lines[big], big_s)) / 2;
  L->y[big] = ((int)h - 7 * big_s) / 2;
  L->show[big] = true;
  return true;
}

static bool render_screen(const struct DisplayConfig *dc, const char *const *lines, unsigned n,
                          unsigned big, const char *url, bool qr_first)
{
  static uint8_t qr_buf[256];
  static uint8_t s_boot_row[256];
  struct boot_layout L;
  QRCode qr;
  uint8_t qr_size = 0u;
  int qr_modules = 0;
  const uint16_t w = dc->pixel_width;
  const uint16_t h = dc->pixel_height;
  int y;

  if (url != nullptr) {
    uint16_t qr_buf_size = qrcode_getBufferSize(6u);
    if (qr_buf_size == 0u || qr_buf_size > sizeof(qr_buf) ||
        qrcode_initText(&qr, qr_buf, 6u, ECC_MEDIUM, url) != 0) {
      return false;
    }
    qr_size = qr.size;
    qr_modules = (int)qr_size + 8;
  }
  if (!boot_layout(w, h, lines, n, big, qr_modules, qr_first, &L)) {
    return false;
  }

  /* Two image planes, from the colour scheme rather than a vendored panel flag: a
   * spot-colour panel takes a second plane even when the boot screen leaves it blank. */
  bool is3clr = od_boot_has_blank_second_plane(dc->color_scheme);
  bool is_gray4 = dc->color_scheme == OD_COLOR_SCHEME_GRAY4;
  int bits_per_pixel = boot_bits_per_pixel(dc->color_scheme);
  if (bits_per_pixel == 0) {
    return false;
  }
  int pitch = ((int)w * bits_per_pixel + 7) / 8;
  int plane_pitch = ((int)w + 7) / 8;
  size_t workspace = (size_t)pitch;
  uint8_t white_byte = 0xFFu;

  if (is_gray4) {
    workspace += (size_t)plane_pitch;
  }
  if (workspace > sizeof(s_boot_row)) {
    return false;
  }
  if (dc->color_scheme == OD_COLOR_SCHEME_BWRY) {
    white_byte = 0x55u;
  } else if (dc->color_scheme == OD_COLOR_SCHEME_BWGBRY) {
    white_byte = 0x11u;
  }

  const int render_passes = is_gray4 ? 2 : 1;
  for (int pass = 0; pass < render_passes; pass++) {
    /* Pass 0's image marker was already reached by od_epd_shim_begin(). There is no
     * address window to set: the drive sequence from config says where the frame goes. */
    if (pass > 0 && !od_epd_shim_next_plane()) {
      return false;
    }

    for (y = 0; y < (int)h; y++) {
      memset(s_boot_row, white_byte, (size_t)pitch);

      for (unsigned i = 0; i < n; i++) {
        if (L.show[i]) {
          draw_text_row(s_boot_row, y, L.x[i], L.y[i], lines[i], L.scale[i], bits_per_pixel, (int)w);
        }
      }

      if (L.module_px > 0 && y >= L.qr_y && y < L.qr_y + L.qr_px) {
        int my = (y - L.qr_y) / L.module_px;
        int qy_qr = my - 4;
        for (int mx = 0; mx < qr_modules; mx++) {
          int qx_qr = mx - 4;
          if (qx_qr < 0 || qy_qr < 0 || qx_qr >= (int)qr_size || qy_qr >= (int)qr_size ||
              !qrcode_getModule(&qr, (uint8_t)qx_qr, (uint8_t)qy_qr)) {
            continue;
          }
          for (int sx = 0; sx < L.module_px; sx++) {
            int px = L.qr_x + mx * L.module_px + sx;
            if (px >= 0 && px < (int)w) {
              set_pixel_row(s_boot_row, px, bits_per_pixel);
            }
          }
        }
      }

      if (is_gray4) {
        uint8_t *plane_row = s_boot_row + pitch;
        make_gray4_plane_row(plane_row, s_boot_row, plane_pitch, w, pass);
        if (!od_epd_shim_write(plane_row, (uint32_t)plane_pitch)) {
          return false;
        }
      } else if (!od_epd_shim_write(s_boot_row, (uint32_t)pitch)) {
        return false;
      }
    }
  }
  if (is3clr) {
    /* The second plane of a 3-colour panel, left blank: these screens are black on white
     * and have nothing to say in the spot colour. It still has to be SENT -- a panel whose
     * sequence declares two planes and receives one refreshes from whatever the controller
     * held. */
    if (!od_epd_shim_next_plane()) {
      return false;
    }
    for (y = 0; y < (int)h; y++) {
      memset(s_boot_row, 0x00u, (size_t)pitch);
      if (!od_epd_shim_write(s_boot_row, (uint32_t)pitch)) {
        return false;
      }
    }
  }
  return true;
}

static void fw_version(uint8_t *major, uint8_t *minor, uint8_t *patch)
{
  uint16_t ver = opendisplay_ble_get_app_version();
  *major = (uint8_t)((ver >> 8) & 0xFFu);
  *minor = (uint8_t)(ver & 0xFFu);
  *patch = (uint8_t)opendisplay_ble_get_app_version_patch();
}

static bool render_boot_screen(const struct GlobalConfig *cfg)
{
  const struct DisplayConfig *dc;
  uint32_t last3;
  char url[OD_BOOT_URL_SIZE];
  char name_line[16];
  char fw_line[20];
  char k1[17], k2[17];
  char key_hex[OD_BOOT_KEY_HEX_SIZE];
  uint8_t key[OD_BOOT_KEY_SIZE];
  enum od_boot_key_state key_state;
  bool url_ok;
  uint8_t major, minor, patch;

  if (cfg == nullptr || cfg->display_count == 0u) {
    return false;
  }
  dc = &cfg->displays[0];
  last3 = (uint32_t)(SYSTEM_GetUnique() & 0xFFFFFFu);
  /* SECURITY_V2 10.7: the same policy the other targets get through od_boot_screen_render();
   * key[] is K_dev only when SHOWN (claimed + SHOW_KEY_ON_SCREEN) and zeros otherwise, so the
   * two hex lines and the QR payload always agree. Wiped before anything can return. */
  key_state = od_boot_screen_key(key);
  url_ok = od_boot_url_build(dc->legacy_tag_type, last3, key_state == OD_BOOT_KEY_SHOWN ? key : nullptr,
                             cfg->manufacturer_data.manufacturer_id, url, sizeof(url));
  od_boot_format_key_display(key_state, key, key_hex);
  memset(key, 0, sizeof key);
  memcpy(k1, key_hex, 16u);
  k1[16] = '\0';
  memcpy(k2, key_hex + 16, 16u);
  k2[16] = '\0';
  memset(key_hex, 0, sizeof key_hex);
  if (!url_ok) {
    return false;
  }
  name_line[0] = 'O';
  name_line[1] = 'D';
  *od_fmt_hex(&name_line[2], last3, 6u) = '\0';
  {
    /* "FW:S M.m.p": 5 + 3 * 3 + 2 dots + NUL = 17 <= sizeof(fw_line). */
    char *p = fw_line;
    fw_version(&major, &minor, &patch);
    memcpy(p, "FW:S ", 5u);
    p = od_fmt_u8(p + 5, major);
    *p++ = '.';
    p = od_fmt_u8(p, minor);
    *p++ = '.';
    p = od_fmt_u8(p, patch);
    *p = '\0';
  }
  {
    const char *lines[5] = { "OPENDISPLAY.ORG", name_line, fw_line, k1, k2 };
    const bool ok = render_screen(dc, lines, 5u, BOOT_NO_BIG, url, false);
    memset(k1, 0, sizeof k1);
    memset(k2, 0, sizeof k2);
    memset(url, 0, sizeof url);
    return ok;
  }
}

/* --- CMD_SPI_RAW session ---------------------------------------------------------------------
 *
 * The rail and transfer ownership live here, as they do for every other path; the adapter-level
 * operations go through od_epd_shim_raw_*, which is the only translation unit allowed to see the
 * adapter (od_epd_shim.h explains why).
 */
static bool s_spi_raw_open;

extern "C" bool opendisplay_display_spi_raw_is_open(void)
{
  return s_spi_raw_open;
}

extern "C" int opendisplay_display_spi_raw_open(bool run_panel_init)
{
  const struct DisplayConfig *d = display_cfg();

  if (d == nullptr) {
    return -OD_ERR_SPI_RAW_NO_CONFIG;
  }
  /* NO PANEL TABLE: without drive packets there is no init sequence, no pin sense and no
   * colour map, so there is nothing to bring up. Same test the boot screen uses. */
  if (!od_epd_shim_available(d)) {
    return -OD_ERR_SPI_RAW_NO_CONFIG;
  }
  /* A raw session and a direct write share one panel object, so raw TAKES OWNERSHIP rather
   * than interleaving with a transfer in flight. Re-opening an existing session is also an
   * abort: it is the documented way to get back to a known state. */
  opendisplay_display_abort();
  s_spi_raw_open = false;

  display_power_set(true);
  if (!od_epd_shim_raw_begin(d->instance_number, d->rotation,
                             d->dc_pin, d->reset_pin, d->busy_pin, d->cs_pin,
                             d->data_pin, d->clk_pin, run_panel_init)) {
    display_power_set(false);
    return -OD_ERR_SPI_RAW_PANEL;
  }
  s_spi_raw_open = true;
  return 0;
}

extern "C" int opendisplay_display_spi_raw_cmd(const uint8_t *data, uint16_t len)
{
  if (!s_spi_raw_open) {
    return -OD_ERR_SPI_RAW_NOT_OPEN;
  }
  if (data == nullptr || len == 0u) {
    return -OD_ERR_SPI_RAW_BAD_ARGS;
  }
  od_epd_shim_raw_cmd(data[0], len > 1u ? &data[1] : nullptr, (uint32_t)(len - 1u));
  return 0;
}

extern "C" int opendisplay_display_spi_raw_data(const uint8_t *data, uint16_t len)
{
  if (!s_spi_raw_open) {
    return -OD_ERR_SPI_RAW_NOT_OPEN;
  }
  if (data == nullptr || len == 0u) {
    return -OD_ERR_SPI_RAW_BAD_ARGS;
  }
  od_epd_shim_raw_data(data, (uint32_t)len);
  return 0;
}

extern "C" int opendisplay_display_spi_raw_busy(uint32_t timeout_ms, uint16_t *elapsed_ms_out,
                                                bool *still_busy_out)
{
  if (!s_spi_raw_open) {
    return -OD_ERR_SPI_RAW_NOT_OPEN;
  }
  /* Clamped to the driver's own refresh ceiling. A two-byte wire field cannot exceed 65535
   * anyway, so this only bites if the field widens. */
  if (timeout_ms > 90000u) {
    timeout_ms = 90000u;
  }
  od_epd_shim_raw_busy(timeout_ms, elapsed_ms_out, still_busy_out);
  return 0;
}

extern "C" int opendisplay_display_spi_raw_reset(uint8_t hold_ms)
{
  if (!s_spi_raw_open) {
    return -OD_ERR_SPI_RAW_NOT_OPEN;
  }
  /* Clamped per the protocol: 0 would not be a pulse, and a long hold from a one-byte field
   * is more likely a mistake than a request. */
  if (hold_ms < 1u) {
    hold_ms = 1u;
  } else if (hold_ms > 50u) {
    hold_ms = 50u;
  }
  return od_epd_shim_raw_reset(hold_ms) ? 0 : -OD_ERR_SPI_RAW_PANEL;
}

extern "C" void opendisplay_display_spi_raw_close(void)
{
  if (!s_spi_raw_open) {
    return;
  }
  s_spi_raw_open = false;
  od_epd_shim_sleep();
  display_power_set(false);
}

extern "C" void opendisplay_display_abort(void)
{
  /* A raw session is torn down HERE because abort is also the transport's fail-safe path
   * (opendisplay_pipe_reset_transport), which is what gives the protocol's "a BLE disconnect
   * force-closes the session" without a second hook in the connection-closed handler.
   *
   * Leaving the flag set would be worse than leaking the rail: the rail goes down below either
   * way, so a surviving session would accept the next CMD and clock bytes at an unpowered
   * panel. No recursion -- close() does not call abort(). */
  if (s_spi_raw_open) {
    opendisplay_display_spi_raw_close();
  }
  if (s_xfer_app.to_flash) {
    od_imgstore_abort();
  }
  od_cmd_flash_clear_arm();
  if (s_xfer_app.panel_up) {
    od_epd_shim_sleep();
  }
  display_power_set(false);
  memset(&s_xfer_app, 0, sizeof(s_xfer_app));
}

static bool xfer_app_write_full(uint32_t stream_offset, od_span_t data)
{
  uint32_t off = 0u;

  if (stream_offset > s_xfer_app.geometry.total_bytes
      || data.n > s_xfer_app.geometry.total_bytes - stream_offset) {
    return false;
  }

  if (s_xfer_app.to_flash) {
    /* Sequential by construction: od_imgstore streams and keeps no buffer, so it cannot
     * honour an out-of-order chunk. The transfer layer delivers in order; refusing a gap is
     * better than programming one image's bytes at another's offset. */
    return od_imgstore_write(data.p, (uint32_t)data.n);
  }

  while (off < data.n) {
    const uint32_t logical = stream_offset + off;
    uint32_t chunk = (uint32_t)data.n - off;

    if (s_xfer_app.geometry.layout == OD_COLOR_LAYOUT_CONTROLLER_PLANES) {
      const uint8_t plane = logical < s_xfer_app.plane_size ? 0u : 1u;
      const uint32_t plane_end = plane == 0u ? s_xfer_app.plane_size
                                             : s_xfer_app.geometry.total_bytes;
      if (s_xfer_app.current_plane != plane) {
        /* The drive sequence decides where the second plane goes, so this is "advance to the
         * next image marker" rather than "address plane 1". A sequence declaring only one
         * refuses, instead of silently writing both planes into the same place. */
        if ((logical != 0u && logical != s_xfer_app.plane_size)
            || !od_epd_shim_next_plane()) {
          return false;
        }
        s_xfer_app.current_plane = plane;
      }
      if (chunk > plane_end - logical) {
        chunk = plane_end - logical;
      }
    }
    /* A chunk must not straddle a pass boundary: the bytes after it belong to a frame that is
     * not on the panel yet. Clamp first, advance below. */
    if (s_xfer_app.mode == XFER_APP_DRIVE && s_xfer_app.frame_bytes > 0u) {
      const uint32_t boundary = (uint32_t)(s_xfer_app.drive_pass + 1u) * s_xfer_app.frame_bytes;
      if (logical < boundary && chunk > boundary - logical) {
        chunk = boundary - logical;
      }
    }
    if (!od_epd_shim_write(data.p + off, chunk)) {
      return false;
    }
    off += chunk;

    /* FRAME BOUNDARY IS PASS BOUNDARY. One pass is a whole refresh, not a plane: finish runs
     * the panel's own waveform and BLOCKS for it -- 20.6 s for the 2.9in's first 16-colour pass
     * -- then the next pass re-runs the mode's init with its own LUT. Blocking here is also the
     * flow control: the host cannot usefully send pass N+1 until pass N is off the glass, so a
     * late ACK on whichever chunk lands on the boundary is the signal. */
    if (s_xfer_app.mode == XFER_APP_DRIVE && s_xfer_app.frame_bytes > 0u
        && s_xfer_app.drive_pass + 1u < s_xfer_app.drive_passes
        && stream_offset + off >= (uint32_t)(s_xfer_app.drive_pass + 1u)
                                  * s_xfer_app.frame_bytes) {
      if (!od_epd_shim_finish()) {
        OD_DIAG("[OD] drive pass %u refresh failed\r\n", (unsigned)s_xfer_app.drive_pass);
        return false;
      }
      s_xfer_app.drive_pass++;
      s_xfer_app.current_plane = 0u;
      /* The rail stays up across passes. The 16-colour behaviour was characterised with power
       * held for all three, and a drop between them would re-run the panel's power-on reset. */
      if (!od_epd_shim_begin_pass(s_xfer_app.drive_pass)) {
        return false;
      }
      OD_DIAG("[OD] drive pass %u/%u\r\n", (unsigned)s_xfer_app.drive_pass + 1u,
             (unsigned)s_xfer_app.drive_passes);
    }
  }
  return true;
}

const struct DisplayConfig *opendisplay_display_config(void)
{
  return display_cfg();
}

void opendisplay_display_power(bool on)
{
  display_power_set(on);
}

bool opendisplay_display_drive_adopt(const od_color_geometry_t *geometry, uint8_t passes,
                                     uint8_t mode, bool compressed,
                                     const uint8_t *payload, uint16_t payload_len)
{
  uint64_t total;

  if (geometry == nullptr || geometry->total_bytes == 0u || passes == 0u) {
    return false;
  }
  total = (uint64_t)geometry->total_bytes * passes;
  if (total > UINT32_MAX) {
    return false;
  }
  /* A COMPRESSED stream declares its uncompressed length, and passes*frame is the ONLY value
   * this device will accept -- a host that got the pass count wrong is told so at START rather
   * than after it has streamed a frame and a half. */
  if (compressed) {
    uint32_t declared;
    if (payload == nullptr || payload_len < 4u) {
      return false;
    }
    declared = (uint32_t)payload[0] | ((uint32_t)payload[1] << 8)
             | ((uint32_t)payload[2] << 16) | ((uint32_t)payload[3] << 24);
    if (declared != (uint32_t)total) {
      OD_DIAG("[OD] drive declared %lu != passes*frame %lu\r\n",
             (unsigned long)declared, (unsigned long)total);
      return false;
    }
  }

  memset(&s_xfer_app, 0, sizeof(s_xfer_app));
  s_xfer_app.mode = XFER_APP_DRIVE;
  s_xfer_app.geometry = *geometry;
  s_xfer_app.geometry.total_bytes = (uint32_t)total;
  s_xfer_app.frame_bytes = geometry->total_bytes;
  s_xfer_app.drive_passes = passes;
  s_xfer_app.drive_pass = 0u;
  s_xfer_app.drive_mode = mode;
  s_xfer_app.panel_up = true;
  s_xfer_app.plane_size = geometry->layout == OD_COLOR_LAYOUT_CONTROLLER_PLANES
                        ? geometry->part_bytes[0] : 0u;
  return true;
}

bool opendisplay_display_drive_stream_info(od_color_geometry_t *geometry_out,
                                           uint32_t *expected_out)
{
  if (s_xfer_app.mode != XFER_APP_DRIVE) {
    return false;
  }
  if (geometry_out != nullptr) {
    *geometry_out = s_xfer_app.geometry;
  }
  if (expected_out != nullptr) {
    *expected_out = s_xfer_app.geometry.total_bytes;
  }
  return true;
}

static bool render_setup_screen(const struct GlobalConfig *cfg, enum od_setup_screen kind,
                                uint32_t code)
{
  static struct od_setup_content c;   /* static: ~150 B off a 2.7 kB stack */
  const struct DisplayConfig *dc;
  uint8_t major, minor, patch;
  bool full;

  if (cfg == nullptr || cfg->display_count == 0u) {
    return false;
  }
  dc = &cfg->displays[0];
  full = dc->pixel_width >= 400u && dc->pixel_height >= 300u;
  fw_version(&major, &minor, &patch);
  if (!od_setup_content_build(kind, full, code, (uint32_t)(SYSTEM_GetUnique() & 0xFFFFFFu),
                              OD_IDENT_TARGET_SILABS, major, minor, patch,
                              cfg->manufacturer_data.simple_config_driver_index,
                              cfg->manufacturer_data.simple_config_display_index,
                              cfg->manufacturer_data.simple_config_power_index, &c)) {
    return false;
  }
  return render_screen(dc, c.line, c.n, c.big == OD_SETUP_NO_BIG ? BOOT_NO_BIG : c.big,
                       c.qr ? c.url : nullptr, !full);
}

/* One mode-0 pass: NORMAL is the boot screen, anything else a setup-family screen. Blocks for
 * the refresh. false = nothing was refreshed (no drive packets, mode 0 unavailable, no fit). */
static bool boot_pass(const struct DisplayConfig *d, enum od_setup_screen kind, uint32_t code)
{
  bool ok;

  /* NO PANEL TABLE, so a device with no drive packets has no boot screen. That is the normal
   * state straight after an OTA, so a failed attempt must NOT latch: the config-reload path
   * calls this again so that writing drive packets brings the boot screen up without a power
   * cycle. Say so, since an undriven panel looks exactly like one driven white. */
  if (!od_epd_shim_available(d)) {
    OD_DIAG("[OD] boot screen: no drive packets; will retry after the next config write\r\n");
    return false;
  }
  display_power_set(true);
  /* Mode 0 by convention is the plain full refresh. */
  if (!od_epd_shim_begin(d->instance_number, 0u, d->rotation,
                         d->dc_pin, d->reset_pin, d->busy_pin, d->cs_pin,
                         d->data_pin, d->clk_pin)) {
    OD_DIAG("[OD] boot screen: mode 0 unavailable; will retry after the next config write\r\n");
    display_power_set(false);
    return false;
  }
  ok = kind == OD_SETUP_SCREEN_NORMAL ? render_boot_screen(opendisplay_get_global_config())
                                      : render_setup_screen(opendisplay_get_global_config(), kind, code);
  if (!ok) {
    OD_DIAG("[OD] boot screen did not fit; skipping refresh\r\n");
    od_epd_shim_sleep();
    display_power_set(false);
    return false;
  }
  /* finishPass runs the sequence tail: power-on, refresh, BUSY wait. The wait is inside it,
   * so boot cannot proceed while the panel is still refreshing. */
  (void)od_epd_shim_finish();
  od_epd_shim_sleep();
  display_power_set(false);
  return true;
}

extern "C" void opendisplay_display_boot_apply(void)
{
  const struct DisplayConfig *d = display_cfg();
  if (s_boot_applied || d == nullptr) {
    return;
  }
  /* Latch only once the panel has really been driven, so a later config write cannot redraw
   * the boot screen over a user's image. */
  if (boot_pass(d, OD_SETUP_SCREEN_NORMAL, 0u)) {
    s_boot_applied = true;
  }
}

/* A config write is ACKed before its redraw: the ACK is only QUEUED when the handler returns,
 * and a 20-35 s refresh run inline would hold it past the host's 5 s bound, so a successful
 * write would report as a timeout. The request is serviced from the loop, and only with no link
 * open and advertising paused (od_link_policy.h): the refresh does not run the Bluetooth host. */
static struct od_panel_defer s_boot_defer;

/* SECURITY_V2 7.8 / FACTORY_RESET.md 4 (od_claim_app.h). A drawn setup screen also latches the
 * boot screen, so a config write in claim mode does not paint the normal screen over it.
 *
 * The draw blocks the loop like the boot screen does. With no link open (SETUP, RESET),
 * connectable advertising is held off for its duration so no peer opens a link into the stalled
 * host (od_link_policy.h). The CODE screen is drawn for the claimant on its open link, which
 * waits for it by design (SECURITY_V2 7.8); a pause already owned by the boot-screen deferral is
 * left to it. */
extern "C" bool od_claim_app_draw(enum od_setup_screen kind, uint32_t code)
{
  const struct DisplayConfig *d = display_cfg();
  bool paused = false;
  bool ok;

  if (d == nullptr || kind == OD_SETUP_SCREEN_NORMAL) {
    return false;
  }
  if (opendisplay_pipe_connection() == 0xFFu && !od_panel_defer_adv_paused(&s_boot_defer)) {
    opendisplay_ble_set_connectable(false);
    paused = true;
  }
  ok = boot_pass(d, kind, code);
  if (paused) {
    opendisplay_ble_set_connectable(true);
  }
  if (!ok) {
    return false;
  }
  s_boot_applied = true;
  return true;
}

extern "C" void opendisplay_display_boot_request(void)
{
  if (!s_boot_applied) {
    od_panel_defer_request(&s_boot_defer);
  }
}

/* SECURITY_V2 10.7 (od_sec2_app.h): the key lines went stale, so the NORMAL boot screen is drawn
 * again over whatever the panel shows. Like every BG22 boot pass it waits for the link to close
 * (s_boot_defer): the refresh blocks the Bluetooth host. */
extern "C" void od_sec2_app_key_screen_changed(void)
{
  if (od_claim_mode_active()) {
    return;
  }
  s_boot_applied = false;
  od_panel_defer_request(&s_boot_defer);
}

extern "C" void od_xfer_app_prepare_start(void)
{
  if (s_xfer_app.mode != XFER_APP_IDLE) {
    opendisplay_display_abort();
  }
}

extern "C" bool od_xfer_app_panel_info(od_xfer_panel_info_t *out)
{
  const struct DisplayConfig *d = display_cfg();

  if (out == nullptr || d == nullptr) {
    return false;
  }
  memset(out, 0, sizeof(*out));
  /* No drive packets: every upload start NACKs needs-migration before the panel is touched. */
  out->needs_migration = !od_epd_shim_available(d);
  if (od_color_direct_geometry(d->color_scheme, d->pixel_width, d->pixel_height,
                               &out->geometry) != OD_COLOR_OK && !out->needs_migration) {
    return false;
  }
  out->width = d->pixel_width;
  out->height = d->pixel_height;
  out->partial_enabled = false;
  return true;
}

extern "C" bool od_xfer_app_begin_full(const od_color_geometry_t *geometry)
{
  const struct DisplayConfig *d = display_cfg();

  if (d == nullptr || geometry == nullptr || geometry->total_bytes == 0u
      || geometry->layout == OD_COLOR_LAYOUT_SPLIT_HALVES) {
    return false;
  }
  /* A device with no drive packets cannot be driven at all now -- there is no built-in panel
   * table left to fall back to. Refusing here is what makes that visible as a failed upload
   * rather than as a panel that stays blank after a successful one. */
  if (!od_epd_shim_available(d)) {
    return false;
  }

  memset(&s_xfer_app, 0, sizeof(s_xfer_app));

  /* PRELOAD: store instead of display. Checked before the panel is powered, so an upload
   * bound for flash never spins the rail up or costs a refresh. */
  uint8_t slot = 0u;
  if (od_cmd_flash_take_arm(&slot)) {
    if (!od_imgstore_begin(slot, d->pixel_width, d->pixel_height, d->color_scheme,
                           1u, geometry->total_bytes)) {
      return false;
    }
    s_xfer_app.mode = XFER_APP_FULL;
    s_xfer_app.geometry = *geometry;
    s_xfer_app.to_flash = true;
    return true;
  }

  display_power_set(true);
  /* Mode 0 by convention is the plain full refresh. */
  if (!od_epd_shim_begin(d->instance_number, 0u, d->rotation,
                         d->dc_pin, d->reset_pin, d->busy_pin, d->cs_pin,
                         d->data_pin, d->clk_pin)) {
    if (od_epd_shim_last_rc() == OD_EPD_RC_BUSY) {
      od_xfer_app_note_start_error(OD_ERR_PANEL_BUSY);
    }
    display_power_set(false);
    return false;
  }
  s_xfer_app.mode = XFER_APP_FULL;
  s_xfer_app.geometry = *geometry;
  s_xfer_app.plane_size = geometry->layout == OD_COLOR_LAYOUT_CONTROLLER_PLANES
      ? geometry->part_bytes[0] : 0u;
  s_xfer_app.current_plane = geometry->initial_plane == OD_COLOR_PLANE_0 ? 0u : 1u;
  s_xfer_app.panel_up = true;
  return true;
}

/* Both full-frame shapes stream through the same write and refresh: an ordinary 0x0070 upload
 * (FULL) and a config-declared multi-pass mode (DRIVE), which differs only in advancing the
 * pass at each frame boundary inside xfer_app_write_full. */
static bool xfer_app_streaming(void)
{
  return s_xfer_app.mode == XFER_APP_FULL || s_xfer_app.mode == XFER_APP_DRIVE;
}

extern "C" uint32_t od_xfer_app_write(uint32_t stream_offset, od_span_t data)
{
  if (!od_span_valid(data) || data.n == 0u || data.n > UINT32_MAX
      || !xfer_app_streaming()) {
    return 0u;
  }
  return xfer_app_write_full(stream_offset, data) ? (uint32_t)data.n : 0u;
}

extern "C" od_mut_span_t od_xfer_app_inflate_scratch(void)
{
  return od_mut_span_make(s_decompression_chunk, OPENDISPLAY_DECOMPRESSION_CHUNK_SIZE);
}

extern "C" void od_xfer_app_abort(od_xfer_abort_reason_t reason)
{
  (void)reason;
  opendisplay_display_abort();
}

extern "C" od_xfer_barrier_t od_xfer_app_before_refresh(const od_reply_t *owner)
{
  const uint32_t deadline = od_xfer_app_now_ms() + 2000u;
  return owner != nullptr
      && opendisplay_pipe_flush_before_refresh(owner->tag, deadline)
      ? OD_XFER_BARRIER_PROCEED : OD_XFER_BARRIER_ABORT;
}

extern "C" void od_xfer_app_barrier_abort(const od_reply_t *owner)
{
  opendisplay_display_abort();
  if (owner != nullptr) {
    opendisplay_pipe_abort_xfer_barrier(owner->tag);
  }
}

extern "C" bool od_xfer_app_refresh(uint8_t mode, bool *completed)
{
  bool ok;

  if (completed == nullptr || !xfer_app_streaming()) {
    return false;
  }
  if (s_xfer_app.to_flash) {
    uint32_t digest = 0u;
    const bool stored = od_imgstore_finish(&digest);
    memset(&s_xfer_app, 0, sizeof(s_xfer_app));
    *completed = stored;
    return true;
  }
  if (!s_xfer_app.panel_up) {
    return false;
  }
  /* `mode` is not consulted: which waveform runs was fixed when the drive mode was selected
   * at begin, and a refresh-mode byte cannot retarget it mid-transfer. finishPass runs the
   * sequence tail -- power-on, refresh opcode, BUSY wait -- so there is no separate refresh
   * call and no wait loop here. */
  (void)mode;
  ok = od_epd_shim_finish();
  od_epd_shim_sleep();
  display_power_set(false);
  memset(&s_xfer_app, 0, sizeof(s_xfer_app));
  *completed = ok;
  return true;
}

extern "C" uint32_t od_xfer_app_now_ms(void)
{
  return od_hal_uptime_ms();
}

/* ------------------------------------------------------------- display from a stored slot --- */

extern "C" bool od_display_from_slot_arm(uint8_t index)
{
  const struct DisplayConfig *d = display_cfg();
  od_imgstore_slot_t slot;

  if (d == nullptr || s_xfer_app.mode != XFER_APP_IDLE) {
    return false;
  }
  if (!od_imgstore_slot(index, &slot) || slot.state != OD_SLOT_VALID) {
    return false;
  }
  /* A stored plane set is only meaningful for the panel it was rendered for. Refusing a
   * mismatch here is the difference between "that slot is for another panel" and a frame that
   * renders as noise and looks like a broken driver. */
  if (slot.width != d->pixel_width || slot.height != d->pixel_height ||
      slot.colour_scheme != d->color_scheme) {
    return false;
  }
  s_pending_slot = (int16_t)index;
  return true;
}

/* Runs from the application loop, not the command handler: the reply has to be queued before
 * the panel takes its 10-20 s, or a successful display reports as a host timeout. */
extern "C" void opendisplay_display_service(void)
{
  switch (od_panel_defer_poll(&s_boot_defer, opendisplay_pipe_connection() != 0xFFu,
                              od_txq_depth() != 0u || od_xfer_active())) {
  case OD_PANEL_DEFER_PAUSE_ADV:
    opendisplay_ble_set_connectable(false);
    return;
  case OD_PANEL_DEFER_RUN:
    opendisplay_display_boot_apply();
    if (od_panel_defer_done(&s_boot_defer)) {
      opendisplay_ble_set_connectable(true);
    }
    break;
  case OD_PANEL_DEFER_IDLE:
  case OD_PANEL_DEFER_WAIT:
    break;
  }
  if (s_pending_slot < 0) {
    return;
  }
  const uint8_t index = (uint8_t)s_pending_slot;
  s_pending_slot = -1;

  const struct DisplayConfig *d = display_cfg();
  od_imgstore_slot_t slot;
  if (d == nullptr || !od_imgstore_slot(index, &slot) || slot.state != OD_SLOT_VALID) {
    return;
  }

  display_power_set(true);
  if (!od_epd_shim_begin(d->instance_number, 0u, d->rotation,
                         d->dc_pin, d->reset_pin, d->busy_pin, d->cs_pin,
                         d->data_pin, d->clk_pin)) {
    display_power_set(false);
    return;
  }
  /* Streamed in the same 256-byte steps the transport uses. The decompression scratch is
   * reused as the read buffer: this path owns the panel, so no transfer can be in flight
   * competing for it, and a second buffer is RAM this part does not have. */
  uint32_t off = 0u;
  bool ok = true;
  while (off < slot.payload_len && ok) {
    uint32_t n = slot.payload_len - off;
    if (n > OPENDISPLAY_DECOMPRESSION_CHUNK_SIZE) {
      n = OPENDISPLAY_DECOMPRESSION_CHUNK_SIZE;
    }
    ok = od_imgstore_read(index, off, s_decompression_chunk, n)
      && od_epd_shim_write(s_decompression_chunk, n);
    off += n;
  }
  if (ok) {
    (void)od_epd_shim_finish();
  }
  od_epd_shim_sleep();
  display_power_set(false);
}
