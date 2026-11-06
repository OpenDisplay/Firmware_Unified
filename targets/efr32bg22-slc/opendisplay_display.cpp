#include "opendisplay_display.h"
#include "od_color.h"
#include "opendisplay_ble.h"
#include "opendisplay_config_parser.h"
#include "opendisplay_constants.h"
#include "opendisplay_runtime.h"
#include "od_epd_shim.h"
#include "od_boot_payload.h"
#include "od_caps.h"
#include "od_xfer_app.h"
#include "opendisplay_pipe.h"
#include "em_cmu.h"
#include "em_gpio.h"
#include "em_system.h"
#include "qrcode/qrcode.h"
#include "sl_sleeptimer.h"
#include <stdio.h>
#include <string.h>

#define OPENDISPLAY_DECOMPRESSION_CHUNK_SIZE 256u

static bool s_boot_applied;
static uint8_t s_decompression_chunk[OPENDISPLAY_DECOMPRESSION_CHUNK_SIZE];

enum XferAppMode {
  XFER_APP_IDLE = 0,
  XFER_APP_FULL,
};

struct XferAppHardwareState {
  XferAppMode mode;
  od_color_geometry_t geometry;
  uint32_t plane_size;
  uint8_t current_plane;
  bool panel_up;
};

static XferAppHardwareState s_xfer_app;

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
};

static const uint8_t *glyph5x7(char c)
{
  unsigned i;
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

static int boot_block_h(int scale)
{
  return 4 * boot_line_step(scale) + 7 * scale;
}

static uint16_t boot_max_text_width(const char *const *lines, unsigned n, int scale)
{
  uint16_t max_w = 0u;
  unsigned i;
  for (i = 0u; i < n; i++) {
    uint16_t tw = text_width_px(lines[i], scale);
    if (tw > max_w) {
      max_w = tw;
    }
  }
  return max_w;
}

static bool boot_layout_fit(uint16_t w, uint16_t h, int scale, int pad, int qr_modules, int *module_px_out,
                            int *qr_px_out, bool *qr_right_out, int *qr_x_out, int *qr_y_out, int *avail_w_out,
                            int *text_y_out, uint16_t max_text_w)
{
  int block_h = boot_block_h(scale);
  int text_gap = pad;
  int side_gap = pad;
  int module_px;
  int qr_px;
  int min_dim = (int)((w < h) ? w : h);
  int module_ideal = (min_dim - pad * 2) / (int)qr_modules;
  if (module_ideal < 1) {
    module_ideal = 1;
  }
  if (module_ideal > 6) {
    module_ideal = 6;
  }

  for (module_px = module_ideal; module_px >= 1; module_px--) {
    qr_px = module_px * (int)qr_modules;
    if (qr_px > (int)w - pad * 2) {
      continue;
    }
    if ((int)w >= pad * 2 + (int)max_text_w + side_gap + qr_px
        && (int)h >= pad * 2 + (block_h > qr_px ? block_h : qr_px)) {
      int avail_w = (int)w - pad * 2 - qr_px - side_gap;
      if ((int)max_text_w <= avail_w) {
        int qr_x = (int)w - pad - qr_px;
        int qr_y = pad;
        int text_y = pad;
        if (block_h < qr_px) {
          text_y = pad + (qr_px - block_h) / 2;
        }
        *module_px_out = module_px;
        *qr_px_out = qr_px;
        *qr_right_out = true;
        *qr_x_out = qr_x;
        *qr_y_out = qr_y;
        *avail_w_out = avail_w;
        *text_y_out = text_y;
        return true;
      }
    }
    if ((int)w >= pad * 2 + (int)max_text_w && (int)h >= pad * 2 + block_h + text_gap + qr_px) {
      *module_px_out = module_px;
      *qr_px_out = qr_px;
      *qr_right_out = false;
      *qr_x_out = ((int)w - qr_px) / 2;
      *qr_y_out = (int)h - pad - qr_px;
      *avail_w_out = (int)w - pad * 2;
      *text_y_out = pad;
      return true;
    }
  }
  return false;
}


static bool render_boot_screen(const struct GlobalConfig *cfg)
{
  static const uint8_t zero_key[OD_BOOT_KEY_SIZE] = { 0 };
  const struct SecurityConfig *sec = od_get_parsed_security();
  const struct DisplayConfig *dc;
  uint16_t w, h;
  uint32_t last3;
  const uint8_t *key;
  enum od_boot_key_state key_state;
  bool show_key;
  char url[OD_BOOT_URL_SIZE];
  QRCode qr;
  static uint8_t qr_buf[256];
  uint16_t qr_buf_size;
  uint8_t qr_size;
  uint16_t qr_modules;
  int scale_text;
  int pad;
  int module_px;
  int qr_px;
  bool qr_right;
  int qr_x, qr_y, avail_w, text_y;
  char name_line[16];
  char fw_line[20];
  const char *domain_line = "OPENDISPLAY.ORG";
  char key_hex[33];
  char k1[17], k2[17];
  uint16_t dW, nW, fwW, k1W, k2W;

  if (cfg == nullptr || cfg->display_count == 0u) {
    return false;
  }
  dc = &cfg->displays[0];
  /* From config, the same source od_xfer_app_panel_info() uses for the upload geometry.
   * bb_epaper reported its own post-rotation width/height here; taking both from one place
   * is what keeps the boot screen and an uploaded image from disagreeing about the frame. */
  w = dc->pixel_width;
  h = dc->pixel_height;

  last3 = (uint32_t)(SYSTEM_GetUnique() & 0xFFFFFFu);
  /* The same policy the other two targets get through od_boot_screen_render(); this panel only
   * differs in how it lays the answer out. */
  key_state = od_boot_key_state(sec);
  key = key_state == OD_BOOT_KEY_NOT_SET ? zero_key : sec->encryption_key;
  show_key = key_state == OD_BOOT_KEY_SHOWN;
  if (!od_boot_url_build(dc->legacy_tag_type, last3, key, show_key,
                         cfg->manufacturer_data.manufacturer_id, url, sizeof(url))) {
    return false;
  }

  qr_buf_size = qrcode_getBufferSize(6u);
  if (qr_buf_size == 0u || qr_buf_size > sizeof(qr_buf)) {
    return false;
  }
  if (qrcode_initText(&qr, qr_buf, 6u, ECC_MEDIUM, url) != 0) {
    return false;
  }
  qr_size = qr.size;
  qr_modules = (uint16_t)(qr_size + 8u);

  (void)snprintf(name_line, sizeof(name_line), "OD%06lX", (unsigned long)last3);
  {
    uint16_t ver = opendisplay_ble_get_app_version();
    (void)snprintf(fw_line, sizeof(fw_line), "FW:S %u.%u.%u",
                   (unsigned)((ver >> 8) & 0xFFu), (unsigned)(ver & 0xFFu),
                   (unsigned)opendisplay_ble_get_app_version_patch());
  }
  od_boot_format_key_display(key, show_key, key_hex);
  memcpy(k1, key_hex, 16);
  k1[16] = '\0';
  memcpy(k2, key_hex + 16, 16);
  k2[16] = '\0';

  {
    static const char *boot_lines[] = {
      domain_line, name_line, fw_line, k1, k2,
    };
    uint16_t max_text_w;
    bool layout_ok = false;
    int try_scale;

    for (try_scale = (w >= 400u && h >= 300u) ? 2 : 1; try_scale >= 1 && !layout_ok; try_scale--) {
      scale_text = try_scale;
      pad = 6 * scale_text;
      max_text_w = boot_max_text_width(boot_lines, 5u, scale_text);
      layout_ok = boot_layout_fit(w, h, scale_text, pad, (int)qr_modules, &module_px, &qr_px, &qr_right, &qr_x,
                                  &qr_y, &avail_w, &text_y, max_text_w);
    }
    if (!layout_ok) {
      scale_text = 1;
      pad = 6;
      max_text_w = boot_max_text_width(boot_lines, 5u, scale_text);
      module_px = 1;
      qr_px = module_px * (int)qr_modules;
      qr_right = false;
      qr_x = ((int)w - qr_px) / 2;
      qr_y = (int)h - pad - qr_px;
      if (qr_y < pad) {
        qr_y = pad;
      }
      avail_w = (int)w - pad * 2;
      text_y = pad;
    }
  }

  {
    static uint8_t s_boot_row[256];
    /* Two image planes, from the colour scheme rather than a vendored panel flag: a
     * spot-colour panel takes a second plane even when the boot screen leaves it blank. */
    bool is3clr = dc->color_scheme == OD_COLOR_SCHEME_BWR
               || dc->color_scheme == OD_COLOR_SCHEME_BWY
               || dc->color_scheme == OD_COLOR_SCHEME_BWRY;
    bool is_gray4 = dc->color_scheme == OD_COLOR_SCHEME_GRAY4;
    int bits_per_pixel = boot_bits_per_pixel(dc->color_scheme);
    if (bits_per_pixel == 0) {
      return false;
    }
    int pitch = ((int)w * bits_per_pixel + 7) / 8;
    int plane_pitch = ((int)w + 7) / 8;
    size_t workspace = (size_t)pitch;
    uint8_t white_byte = 0xFFu;
    int domX, nameX, fwX, k1X, k2X, y;
    int text_max_x;
    int text_origin_x;

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

    dW  = text_width_px(domain_line, scale_text);
    nW  = text_width_px(name_line, scale_text);
    fwW = text_width_px(fw_line, scale_text);
    k1W = text_width_px(k1, scale_text);
    k2W = text_width_px(k2, scale_text);
    text_origin_x = qr_right ? pad : ((int)w - (int)avail_w) / 2;
    if (text_origin_x < pad) {
      text_origin_x = pad;
    }
    text_max_x = qr_right ? (qr_x - pad) : (int)w;
    domX  = text_origin_x + ((avail_w - (int)dW)  / 2);
    nameX = text_origin_x + ((avail_w - (int)nW)  / 2);
    fwX   = text_origin_x + ((avail_w - (int)fwW) / 2);
    k1X   = text_origin_x + ((avail_w - (int)k1W) / 2);
    k2X   = text_origin_x + ((avail_w - (int)k2W) / 2);
    if (domX < pad) {
      domX = pad;
    }
    if (nameX < pad) {
      nameX = pad;
    }
    if (fwX < pad) {
      fwX = pad;
    }
    if (k1X < pad) {
      k1X = pad;
    }
    if (k2X < pad) {
      k2X = pad;
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

        draw_text_row(s_boot_row, y, domX,  text_y,                      domain_line, scale_text, bits_per_pixel,
                      text_max_x);
        draw_text_row(s_boot_row, y, nameX, text_y + boot_line_step(scale_text), name_line, scale_text, bits_per_pixel,
                      text_max_x);
        draw_text_row(s_boot_row, y, fwX,   text_y + boot_line_step(scale_text) * 2, fw_line, scale_text, bits_per_pixel,
                      text_max_x);
        draw_text_row(s_boot_row, y, k1X,   text_y + boot_line_step(scale_text) * 3, k1, scale_text, bits_per_pixel,
                      text_max_x);
        draw_text_row(s_boot_row, y, k2X,   text_y + boot_line_step(scale_text) * 4, k2, scale_text, bits_per_pixel,
                      text_max_x);

        if (y >= qr_y && y < qr_y + qr_px) {
          int local_y = y - qr_y;
          int my = local_y / module_px;
          if (my < (int)qr_modules) {
            int qy_qr = my - 4;
            int mx;
            for (mx = 0; mx < (int)qr_modules; mx++) {
              int qx_qr = mx - 4;
              bool on = false;
              if (qx_qr >= 0 && qy_qr >= 0 && qx_qr < (int)qr_size && qy_qr < (int)qr_size) {
                on = qrcode_getModule(&qr, (uint8_t)qx_qr, (uint8_t)qy_qr);
              }
              if (!on) {
                continue;
              }
              {
                int px0 = qr_x + mx * module_px;
                int sx;
                for (sx = 0; sx < module_px; sx++) {
                  int px = px0 + sx;
                  if (px >= 0 && px < (int)w) {
                    set_pixel_row(s_boot_row, px, bits_per_pixel);
                  }
                }
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
      /* The second plane of a 3-colour panel, left blank: the boot screen is black on white
       * and has nothing to say in the spot colour. It still has to be SENT -- a panel whose
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
  }
  return true;
}

extern "C" void opendisplay_display_abort(void)
{
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
    if (!od_epd_shim_write(data.p + off, chunk)) {
      return false;
    }
    off += chunk;
  }
  return true;
}

extern "C" void opendisplay_display_boot_apply(void)
{
  const struct DisplayConfig *d = display_cfg();
  int panel;
  if (s_boot_applied || d == nullptr) {
    return;
  }
  s_boot_applied = true;

  /* NO PANEL TABLE, so a device with no drive packets has no boot screen -- and that is the
   * honest answer rather than a defect. There is nothing to fall back to: the init sequence,
   * waveform and colour map all arrive in the config now. */
  if (!od_epd_shim_available(d->instance_number)) {
    return;
  }
  display_power_set(true);
  /* Mode 0 by convention is the plain full refresh. */
  if (!od_epd_shim_begin(d->instance_number, 0u, d->rotation,
                         d->dc_pin, d->reset_pin, d->busy_pin, d->cs_pin,
                         d->data_pin, d->clk_pin)) {
    display_power_set(false);
    return;
  }
  if (!render_boot_screen(opendisplay_get_global_config())) {
    od_epd_shim_sleep();
    display_power_set(false);
    return;
  }
  /* finishPass runs the sequence tail: power-on, refresh, BUSY wait. The wait is inside it,
   * so boot cannot proceed while the panel is still refreshing. */
  (void)od_epd_shim_finish();
  od_epd_shim_sleep();
  display_power_set(false);
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
  if (od_color_direct_geometry(d->color_scheme, d->pixel_width, d->pixel_height,
                               &out->geometry) != OD_COLOR_OK) {
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
  if (!od_epd_shim_available(d->instance_number)) {
    return false;
  }

  memset(&s_xfer_app, 0, sizeof(s_xfer_app));
  display_power_set(true);
  /* Mode 0 by convention is the plain full refresh. */
  if (!od_epd_shim_begin(d->instance_number, 0u, d->rotation,
                         d->dc_pin, d->reset_pin, d->busy_pin, d->cs_pin,
                         d->data_pin, d->clk_pin)) {
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

extern "C" uint32_t od_xfer_app_write(uint32_t stream_offset, od_span_t data)
{
  if (!od_span_valid(data) || data.n == 0u || data.n > UINT32_MAX
      || s_xfer_app.mode != XFER_APP_FULL) {
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

  if (completed == nullptr || s_xfer_app.mode != XFER_APP_FULL
      || !s_xfer_app.panel_up) {
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
  return sl_sleeptimer_tick_to_ms(sl_sleeptimer_get_tick_count());
}
