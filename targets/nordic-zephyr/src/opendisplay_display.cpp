#include "opendisplay_display.h"

#include "od_color.h"
#include "od_cmd_reply.h"
#include "od_log.h"
#include "od_xfer_app.h"
#include "opendisplay_ble.h"
#include "opendisplay_config_parser.h"
#include "opendisplay_constants.h"
#include "opendisplay_epd_map.h"
#include "opendisplay_protocol.h"
#include "od_runtime_types.h"
#include "opendisplay_touch.h"
#include "od_board.h"
#include "boot_screen.h"
#include "od_gpio.h"
#include "od_epd_sizes.h"
#include "od_epd_nordic.h"
#include "od_epd_spi.h"
#include "od_hal_time.h"
#include "od_zephyr_compat.h"
#include "od_watchdog_app.h"
#include "bb_epaper.h"
#include "od_bbep_zephyr.h"
#include <stdio.h>
#include <string.h>

extern "C" {
#include "od_zlib_pump.h"
}

void bbepSendCMDSequence(BBEPDISP *pBBEP, const uint8_t *pSeq);

/* Raw BBEPDISP, not the vendored BBEPAPER C++ class -- see panel/od_bbep_zephyr.h. The class is
 * not compiled on this target, exactly as on esp32-idf. */
static BBEPDISP s_epd;
static bool s_boot_applied;
static uint8_t s_decompression_chunk[OPENDISPLAY_DECOMPRESSION_CHUNK_SIZE];
static uint32_t s_displayed_etag;
static bool s_epd_rail_conditioned;

enum XferAppMode {
  XFER_APP_IDLE = 0,
  XFER_APP_FULL,
  XFER_APP_PARTIAL,
  /* The panel is up at a config-declared mode, on od_epd rather than bb_epaper. */
  XFER_APP_DRIVE,
};

struct XferAppHardwareState {
  XferAppMode mode;
  od_color_geometry_t geometry;
  uint16_t x;
  uint16_t y;
  uint16_t width;
  uint16_t height;
  uint32_t plane_size;
  uint8_t current_plane;
  bool panel_up;
  uint32_t frame_bytes;
  uint8_t drive_passes;
  uint8_t drive_pass;
};

static XferAppHardwareState s_xfer_app;

static const struct DisplayConfig *display_cfg(void)
{
  const struct od_config *cfg = opendisplay_get_global_config();
  if (cfg == nullptr || cfg->display_count == 0u) {
    return nullptr;
  }
  return &cfg->displays[0];
}

static bool display_bus_acquire(const struct DisplayConfig *d)
{
  if (d == nullptr || !od_epd_spi_init(d->data_pin, d->clk_pin)) {
    od_log_error("panel: SPI acquire failed");
    return false;
  }
  return true;
}

static void display_park_signal_pin(uint8_t pin_cfg)
{
  od_gpio_park(pin_cfg);
}

void opendisplay_display_park_pins(void)
{
  const struct DisplayConfig *d = display_cfg();

  if (d == nullptr) {
    return;
  }
  display_park_signal_pin(d->cs_pin);
  display_park_signal_pin(d->data_pin);
  display_park_signal_pin(d->clk_pin);
  display_park_signal_pin(d->dc_pin);
  display_park_signal_pin(d->reset_pin);
  display_park_signal_pin(d->busy_pin);
}

/*
 * PANEL RAIL BRING-UP. Ported from the Arduino reference's pwrmgm(true)
 * (Firmware/src/main.cpp:1341-1382), which is the behaviour the deployed configs were
 * validated against.
 *
 * THE RAIL NEEDS ~900 ms BEFORE THE PANEL WILL TALK, and this function used to return the
 * instant the enable pin went high. The whole init then ran against an unpowered controller:
 *
 *     dw init after cfg           0 ms
 *     dw init after setPanelType  2 ms
 *     dw init after initIO        8 ms     <- RST already toggling here
 *     dw init after wake          60 ms    <- BUSY sampled here
 *
 * At 8 ms the reference has not yet finished the first of its two delays. bbepIsBusy() reads
 * an unpowered BUSY line as idle, bbepWakeUp()'s wait returns immediately, pInitFull and all
 * 48000 bytes are clocked into a dead controller, and the only symptom is the 60 s
 * "BUSY NEVER ASSERTED" at refresh time. Nothing upstream of that reports an error.
 *
 * The reference's two delays are separate and both are needed: 800 ms for the rail itself,
 * then the signal lines are pre-driven to their idle levels and 100 ms lets the panel see
 * them settle before RST is toggled.
 */
#define OD_PANEL_RAIL_SETTLE_MS 800u
#define OD_PANEL_PIN_SETTLE_MS  100u

static bool display_power_set(bool on)
{
  const struct od_config *cfg = opendisplay_get_global_config();
  const struct DisplayConfig *d = display_cfg();
  uint8_t p;

  /* Never bring the panel rail up in watchdog safe mode. Powering DOWN is still allowed, so a
   * rail left on by a pre-safe-mode boot can still be shut off. This is the same gate the
   * reference puts at the top of pwrmgm() (esp32-idf main.cpp), and display_power_set is this
   * target's pwrmgm -- one refusal point covers boot refresh and every pushed image alike. */
  if (on && od_watchdog_app_safe_mode()) {
    od_log_warn("panel: power-up refused - watchdog safe mode");
    return false;
  }
  if (!on) {
    od_epd_spi_deinit();
  }
  if (cfg == nullptr) {
    od_log_error("panel: global config unavailable");
    return false;
  }
  p = cfg->system_config.pwr_pin;
  /*
   * 0xFF means the pin is NOT PRESENT: the rail is permanently powered and cannot be turned
   * off. There is then no pin to drive, no rail settle to wait for, and nothing to park --
   * the panel stays live either way, so leaving its signal lines driven at their idle levels
   * is safer than floating them into a powered controller.
   *
   * What must never happen is substituting a fallback pin number; driving a guessed pin is
   * the unsafe case, not this configuration. 0xFF is the contract's documented default
   * (opendisplay_structs.h: "primary power-management pin; 0xFF = not present"), so refusing
   * it outright disabled the display on every board wired without a rail switch.
   * Matches pwrmgm() (esp32-idf main.cpp:1382 on, :1422 off).
   */
  const bool has_pwr_pin = (p != 0xFFu);

  if (!on) {
    od_watchdog_app_phase(OD_WDT_PHASE_FORCE_OFF);
    if (has_pwr_pin) {
      opendisplay_display_park_pins();
      od_gpio_configure_output(p, false);
    } else if (d != nullptr) {
      /* nrfx release disconnects SCK/MOSI. A permanently powered controller must
       * instead see the same driven-low idle bus that this path historically left. */
      od_gpio_configure_output(d->clk_pin, false);
      od_gpio_configure_output(d->data_pin, false);
    }
    od_watchdog_app_phase(OD_WDT_PHASE_IDLE_OFF);
    return true;
  }

  /* Rail bring-up, breadcrumbed per sub-step. A 2026-08-03 freeze on the reference reset ~120 s
   * into bring-up without ever reaching the init sequence, and a single ACQUIRE_COLD stamp could
   * not say which step it died in. There is no PWRMGM_PMIC stamp here because no PMIC sits on
   * this path -- the nPM1300 is read as a sensor, not brought up to power the panel. */
  od_watchdog_app_phase(OD_WDT_PHASE_PWRMGM_RAIL);

  /* Boost-select conditioning is a panel-interface setting, not a rail switch: the panel
   * needs it whether or not this board can cut power. */
  od_board_prepare_epd_rail();

  if (!has_pwr_pin) {
    od_log_warn("panel: pwr_pin not present; rail is permanently powered");
  } else {
    od_gpio_configure_output(p, true);

    /* The deployed nRF52840 board requires the donor firmware's one-time cold
     * rail conditioning: BS low, on 50 ms, off 50 ms, BS low, on. nRF54 board
     * implementations return false and retain their existing single power-on. */
    if (!s_epd_rail_conditioned && od_board_epd_requires_cold_cycle()) {
      od_msleep(50);
      od_gpio_configure_output(p, false);
      od_msleep(50);
      od_board_prepare_epd_rail();
      od_gpio_configure_output(p, true);
    }
    s_epd_rail_conditioned = true;
    od_log_debug("panel: rail on (pwr_pin=%u), settling %u ms", (unsigned)p,
                 (unsigned)OD_PANEL_RAIL_SETTLE_MS);
    od_msleep(OD_PANEL_RAIL_SETTLE_MS);
  }

  /* Pre-drive the signal lines to their idle levels, exactly as pwrmgm() does, so the panel
   * comes out of its own power-on reset seeing a quiet bus rather than floating inputs. */
  od_watchdog_app_phase(OD_WDT_PHASE_PWRMGM_PINS);
  if (d != nullptr) {
    od_gpio_configure_output(d->reset_pin, true);   /* RST idle HIGH */
    od_gpio_configure_output(d->cs_pin, true);      /* CS  idle HIGH */
    od_gpio_configure_output(d->dc_pin, false);
    od_gpio_configure_output(d->clk_pin, false);
    od_gpio_configure_output(d->data_pin, false);
    od_gpio_configure_input(d->busy_pin, false, false);
    od_msleep(OD_PANEL_PIN_SETTLE_MS);
  }
  return true;
}

static bool wait_for_refresh(uint32_t timeout_ms)
{
  uint32_t elapsed = 0;
  bool saw_busy = false;

  od_watchdog_app_phase(OD_WDT_PHASE_REFRESH_WAIT);

  /*
   * READ THE EXIT CONDITION CAREFULLY: this returns true only after it has seen BUSY go
   * ASSERTED and then RELEASED. If BUSY never asserts at all -- panel not actually driven,
   * or the BUSY line mis-decoded, or the wrong pull for the chip type -- the loop runs the
   * FULL timeout in silence and then returns false. At the 60 s the caller passes, that is
   * a minute of apparent hang with no output, during which the host gives up and its next
   * attempt is refused with "Pipe write already in progress".
   *
   * The od_log_debug() lines exist so that case is distinguishable from a panel that is simply
   * slow: "busy asserted" appearing at all is the difference between "the panel is talking
   * to us" and "we are bit-banging into the void".
   */
  while (elapsed < timeout_ms) {
    bool busy = bbepIsBusy(&s_epd);
    if (busy) {
      if (!saw_busy) {
        od_log_debug("refresh: busy asserted after %u ms", (unsigned)elapsed);
      }
      saw_busy = true;
    } else if (saw_busy) {
      od_log_debug("refresh: busy released after %u ms", (unsigned)elapsed);
      return true;
    }
    od_msleep(50);
    elapsed += 50;
  }
  if (!saw_busy) {
    od_log_debug("refresh: BUSY NEVER ASSERTED in %u ms -- panel is not responding; "
                 "check the BUSY pin decode and the panel power rail",
                 (unsigned)timeout_ms);
  } else {
    od_log_debug("refresh: busy asserted but still held at %u ms timeout",
           (unsigned)timeout_ms);
  }
  return saw_busy && !bbepIsBusy(&s_epd);
}

static bool panel_skips_bbep_set_addr_window(const BBEPDISP *pBBEP)
{
  return pBBEP->type == EP397_800x480 || pBBEP->type == EP397_800x480_4GRAY || pBBEP->type == EP426_800x480
         || pBBEP->type == EP426_800x480_4GRAY;
}

static bool panel_uses_pixel_ram_x(const BBEPDISP *pBBEP)
{
  return pBBEP->type == EP397_800x480 || pBBEP->type == EP397_800x480_4GRAY || pBBEP->type == EP426_800x480
         || pBBEP->type == EP426_800x480_4GRAY;
}

static bool panel_uses_ep397_y_decrement(const BBEPDISP *pBBEP)
{
  return pBBEP->type == EP397_800x480 || pBBEP->type == EP397_800x480_4GRAY;
}

static bool panel_uses_ep426_x_decrement(const BBEPDISP *pBBEP)
{
  return pBBEP->type == EP426_800x480 || pBBEP->type == EP426_800x480_4GRAY;
}

static bool panel_skips_reinit_on_partial_refresh(const BBEPDISP *pBBEP)
{
  return panel_uses_ep397_y_decrement(pBBEP) || panel_uses_ep426_x_decrement(pBBEP);
}

static void partial_set_ep397_ram_y(BBEPDISP *pBBEP, int ty, int cy)
{
  uint8_t uc[4];
  int yLast = ty + cy - 1;
  int ramYStart = (pBBEP->native_height - 1) - ty;
  int ramYEnd = (pBBEP->native_height - 1) - yLast;

  bbepWriteCmd(&s_epd, SSD1608_SET_RAMYPOS);
  uc[0] = (uint8_t)(ramYStart & 0xff);
  uc[1] = (uint8_t)(ramYStart >> 8);
  uc[2] = (uint8_t)(ramYEnd & 0xff);
  uc[3] = (uint8_t)(ramYEnd >> 8);
  bbepWriteData(&s_epd, uc, 4);

  bbepWriteCmd(&s_epd, SSD1608_SET_RAMYCOUNT);
  uc[0] = (uint8_t)(ramYStart & 0xff);
  uc[1] = (uint8_t)(ramYStart >> 8);
  bbepWriteData(&s_epd, uc, 2);
}

static void partial_set_ep426_ram_y(BBEPDISP *pBBEP, int ty, int cy)
{
  uint8_t uc[4];
  int yLast = ty + cy - 1;

  bbepWriteCmd(&s_epd, SSD1608_SET_RAMYPOS);
  uc[0] = (uint8_t)ty;
  uc[1] = (uint8_t)(ty >> 8);
  uc[2] = (uint8_t)yLast;
  uc[3] = (uint8_t)(yLast >> 8);
  bbepWriteData(&s_epd, uc, 4);

  bbepWriteCmd(&s_epd, SSD1608_SET_RAMYCOUNT);
  uc[0] = (uint8_t)ty;
  uc[1] = (uint8_t)(ty >> 8);
  bbepWriteData(&s_epd, uc, 2);
  (void)pBBEP;
}

static void partial_set_pixel_ram_x(BBEPDISP *pBBEP, int x, int cx)
{
  uint8_t uc[4];
  int px0 = x;
  int px1 = x + cx - 1;
  if (panel_uses_ep426_x_decrement(pBBEP)) {
    px0 = (pBBEP->native_width - 1) - x;
    px1 = (pBBEP->native_width - 1) - (x + cx - 1);
  }

  bbepWriteCmd(&s_epd, SSD1608_SET_RAMXPOS);
  uc[0] = (uint8_t)(px0 & 0xff);
  uc[1] = (uint8_t)((px0 >> 8) & 0xff);
  uc[2] = (uint8_t)(px1 & 0xff);
  uc[3] = (uint8_t)(px1 >> 8);
  bbepWriteData(&s_epd, uc, 4);

  bbepWriteCmd(&s_epd, SSD1608_SET_RAMXCOUNT);
  uc[0] = (uint8_t)(px0 & 0xff);
  uc[1] = (uint8_t)(px0 >> 8);
  bbepWriteData(&s_epd, uc, 2);
}

static void partial_set_addr_window(BBEPDISP *pBBEP, int x, int y, int cx, int cy)
{
  if (!panel_skips_bbep_set_addr_window(pBBEP)) {
    bbepSetAddrWindow(&s_epd, x, y, cx, cy);
    return;
  }

  uint8_t uc[4];
  int ty = y;
  cx = (cx + 7) & 0xfff8;

  if (panel_uses_pixel_ram_x(pBBEP)) {
    partial_set_pixel_ram_x(pBBEP, x, cx);
  } else {
    int tx = x / 8;
    bbepWriteCmd(&s_epd, SSD1608_SET_RAMXPOS);
    uc[0] = (uint8_t)tx;
    uc[1] = (uint8_t)(tx + ((cx - 1) >> 3));
    bbepWriteData(&s_epd, uc, 2);
    bbepCMD2(&s_epd, SSD1608_SET_RAMXCOUNT, (uint8_t)tx);
  }

  if (panel_uses_ep426_x_decrement(pBBEP)) {
    partial_set_ep426_ram_y(pBBEP, ty, cy);
  } else if (panel_uses_ep397_y_decrement(pBBEP)) {
    partial_set_ep397_ram_y(pBBEP, ty, cy);
  } else {
    bbepWriteCmd(&s_epd, SSD1608_SET_RAMYPOS);
    uc[0] = (uint8_t)ty;
    uc[1] = (uint8_t)(ty >> 8);
    uc[2] = (uint8_t)(ty + cy - 1);
    uc[3] = (uint8_t)((ty + cy - 1) >> 8);
    bbepWriteData(&s_epd, uc, 4);
    uc[0] = (uint8_t)ty;
    uc[1] = (uint8_t)(ty >> 8);
    bbepWriteCmd(&s_epd, SSD1608_SET_RAMYCOUNT);
    bbepWriteData(&s_epd, uc, 2);
  }
  bbepWaitBusy(&s_epd);
}

static bool partial_trigger_refresh(int refresh_mode)
{
  if (refresh_mode < 0 || refresh_mode > 3) {
    refresh_mode = REFRESH_PARTIAL;
  }
  if (panel_skips_reinit_on_partial_refresh(&s_epd)) {
    if (panel_uses_ep397_y_decrement(&s_epd)) {
      static const uint8_t u8cmdz3[4] = { 0xf7, 0xd7, 0xff, 0 };
      bbepCMD2(&s_epd, SSD1608_DISP_CTRL2, u8cmdz3[refresh_mode]);
    } else {
      static const uint8_t u8cmd[4] = { 0xf7, 0xc7, 0xff, 0xc0 };
      bbepCMD2(&s_epd, SSD1608_DISP_CTRL2, u8cmd[refresh_mode]);
    }
    bbepWriteCmd(&s_epd, SSD1608_MASTER_ACTIVATE);
    return wait_for_refresh(60000u);
  }
  (void)bbepRefresh(&s_epd, refresh_mode);
  return wait_for_refresh(60000u);
}

static bool partial_prepare_panel_ram_hardware(void)
{
  const struct DisplayConfig *d = display_cfg();
  int panel;

  if (d == nullptr) {
    return false;
  }
  panel = opendisplay_map_epd(d->panel_ic_type);
  if (panel == EP_PANEL_UNDEFINED) {
    return false;
  }

  if (!display_power_set(true)) {
    return false;
  }
  memset(&s_epd, 0, sizeof(s_epd));
  if (bbepSetPanelType(&s_epd, panel) != BBEP_SUCCESS) {
    display_power_set(false);
    return false;
  }
  if ((s_epd.iFlags & BBEP_SPLIT_BUFFER) != 0u) {
    od_log_error("split-panel transport is not hardware-qualified on this target");
    display_power_set(false);
    return false;
  }
  bbepSetRotation(&s_epd, (int)d->rotation * 90);
  if (!display_bus_acquire(d)) {
    display_power_set(false);
    return false;
  }
  bbepInitIO(&s_epd, d->dc_pin, d->reset_pin, d->busy_pin, d->cs_pin, d->data_pin, d->clk_pin,
             od_epd_spi_hz());
  od_bbep_wake(&s_epd);
  {
    const uint8_t *init_seq = s_epd.pInitPart ? s_epd.pInitPart : s_epd.pInitFull;
    bbepSendCMDSequence(&s_epd, init_seq);
  }
  bbepFill(&s_epd, BBEP_WHITE, PLANE_1);
  bbepFill(&s_epd, BBEP_WHITE, PLANE_0);
  if (od_epd_spi_faulted()) {
    display_power_set(false);
    return false;
  }
  return true;
}

extern "C" bool opendisplay_display_boot_apply(void)
{
  const struct DisplayConfig *d = display_cfg();
  int panel;
  if (s_boot_applied) {
    return true;
  }
  /* Three consecutive watchdog resets say the panel path is what wedges, so a fourth attempt is
   * another reset. Reported as applied, not as a failure: the caller's failure arm retries, and
   * retrying is exactly what safe mode exists to stop. The device stays up on BLE and DFU, which
   * is the only way a bad config or a bad image can be replaced. */
  if (od_watchdog_app_safe_mode()) {
    od_log_warn("boot display skipped: watchdog safe mode");
    s_boot_applied = true;
    return true;
  }
  if (d == nullptr) {
    od_log_error("boot display: no display config");
    return false;
  }
  if ((d->transmission_modes & OD_TRANSMISSION_MODE_CLEAR_ON_BOOT) != 0u) {
    od_log_info("boot display intentionally skipped by CLEAR_ON_BOOT");
    s_boot_applied = true;
    return true;
  }
  panel = opendisplay_map_epd(d->panel_ic_type);
  if (panel == EP_PANEL_UNDEFINED) {
    od_log_error("boot display: unsupported panel_ic_type=%u", (unsigned)d->panel_ic_type);
    return false;
  }

  for (unsigned attempt = 1; attempt <= 2; attempt++) {
    bool refresh_ok = false;

    memset(&s_epd, 0, sizeof(s_epd));
    if (!display_power_set(true)) {
      od_log_error("boot display: panel power unavailable (attempt %u)", attempt);
      continue;
    }
    if (bbepSetPanelType(&s_epd, panel) != BBEP_SUCCESS) {
      od_log_error("boot display: setPanelType failed (attempt %u)", attempt);
      display_power_set(false);
      continue;
    }
    if ((s_epd.iFlags & BBEP_SPLIT_BUFFER) != 0u) {
      od_log_error("boot display: split-panel transport is not hardware-qualified");
      display_power_set(false);
      return false;
    }
    bbepSetRotation(&s_epd, (int)d->rotation * 90);
    if (!display_bus_acquire(d)) {
      display_power_set(false);
      continue;
    }
    bbepInitIO(&s_epd, d->dc_pin, d->reset_pin, d->busy_pin, d->cs_pin, d->data_pin,
               d->clk_pin, od_epd_spi_hz());
    od_bbep_wake(&s_epd);
    od_watchdog_app_phase(OD_WDT_PHASE_INIT_SEQ);
    od_bbep_send_panel_init_full(&s_epd);
    if (od_epd_spi_faulted()) {
      od_log_error("boot display: panel SPI failed during initialization");
      display_power_set(false);
      continue;
    }
    if (!writeBootScreenWithQr(s_epd)) {
      od_log_warn("boot display: renderer failed; transmitting white fallback");
      od_watchdog_app_phase(OD_WDT_PHASE_FILL);
      bbepFill(&s_epd, BBEP_WHITE, PLANE_DUPLICATE);
    }
    if (od_epd_spi_faulted()) {
      od_log_error("boot display: panel SPI failed while transmitting framebuffer");
      display_power_set(false);
      continue;
    }
    od_log_info("boot framebuffer transmitted; starting physical refresh (attempt %u)", attempt);
    if (bbepRefresh(&s_epd, REFRESH_FULL) == BBEP_SUCCESS) {
      refresh_ok = wait_for_refresh(60000u);
    } else {
      od_log_error("boot display: refresh command failed (attempt %u)", attempt);
    }
    bbepSleep(&s_epd, DEEP_SLEEP);
    display_power_set(false);

    if (refresh_ok) {
      s_boot_applied = true;
      od_log_info("boot display refresh complete");
      return true;
    }
    od_log_warn("boot display: physical refresh failed (attempt %u of 2)", attempt);
    if (attempt < 2) {
      od_msleep(100);
    }
  }
  return false;
}

static void xfer_app_clear(void)
{
  if (s_xfer_app.panel_up) {
    if (s_xfer_app.mode == XFER_APP_DRIVE) {
      od_epd_nordic_sleep();
    } else {
      bbepSleep(&s_epd, DEEP_SLEEP);
    }
    display_power_set(false);
  }
  od_epd_spi_deinit();
  (void)od_epd_spi_fault_reset();
  memset(&s_xfer_app, 0, sizeof(s_xfer_app));
}

static bool xfer_app_write_full(uint32_t stream_offset, od_span_t data)
{
  uint32_t consumed = 0u;

  if (stream_offset > s_xfer_app.geometry.total_bytes
      || data.n > s_xfer_app.geometry.total_bytes - stream_offset) {
    return false;
  }
  if (s_xfer_app.geometry.layout != OD_COLOR_LAYOUT_CONTROLLER_PLANES) {
    bbepWriteData(&s_epd, (uint8_t *)(uintptr_t)data.p, (int)data.n);
    return !od_epd_spi_faulted();
  }

  while (consumed < data.n) {
    const uint32_t logical = stream_offset + consumed;
    const uint8_t plane = logical < s_xfer_app.plane_size ? PLANE_0 : PLANE_1;
    const uint32_t plane_end = plane == PLANE_0 ? s_xfer_app.plane_size
                                                 : s_xfer_app.geometry.total_bytes;
    uint32_t chunk;

    if (s_xfer_app.current_plane != plane) {
      if (logical != 0u && logical != s_xfer_app.plane_size) {
        return false;
      }
      bbepStartWrite(&s_epd, plane);
      s_xfer_app.current_plane = plane;
    }
    chunk = plane_end - logical;
    if (chunk > data.n - consumed) {
      chunk = (uint32_t)data.n - consumed;
    }
    bbepWriteData(&s_epd, (uint8_t *)(uintptr_t)(data.p + consumed), (int)chunk);
    if (od_epd_spi_faulted()) {
      return false;
    }
    consumed += chunk;
  }
  return true;
}

static bool xfer_app_write_partial(uint32_t stream_offset, od_span_t data)
{
  const uint32_t total = s_xfer_app.plane_size * 2u;
  uint32_t consumed = 0u;

  if (stream_offset > total || data.n > total - stream_offset) {
    return false;
  }
  while (consumed < data.n) {
    const uint32_t logical = stream_offset + consumed;
    const uint8_t plane = logical < s_xfer_app.plane_size ? PLANE_1 : PLANE_0;
    const uint32_t plane_end = plane == PLANE_1 ? s_xfer_app.plane_size : total;
    uint32_t chunk;

    if (s_xfer_app.current_plane != plane) {
      if (logical != 0u && logical != s_xfer_app.plane_size) {
        return false;
      }
      partial_set_addr_window(&s_epd, s_xfer_app.x, s_xfer_app.y,
                              s_xfer_app.width, s_xfer_app.height);
      bbepStartWrite(&s_epd, plane);
      s_xfer_app.current_plane = plane;
    }
    chunk = plane_end - logical;
    if (chunk > data.n - consumed) {
      chunk = (uint32_t)data.n - consumed;
    }
    bbepWriteData(&s_epd, (uint8_t *)(uintptr_t)(data.p + consumed), (int)chunk);
    if (od_epd_spi_faulted()) {
      return false;
    }
    consumed += chunk;
  }
  return true;
}

/* A frame boundary is a pass boundary. finish runs the panel's refresh and blocks for it, which is
 * also the flow control: the ACK of the chunk landing on the boundary arrives when the pass is off
 * the glass. The rail stays up across passes, as the 16-colour behaviour was characterised. */
static bool xfer_app_write_drive(uint32_t stream_offset, od_span_t data)
{
  size_t off = 0u;

  while (off < data.n) {
    const uint32_t logical = stream_offset + (uint32_t)off;
    const uint32_t frame_end = (uint32_t)(s_xfer_app.drive_pass + 1u) * s_xfer_app.frame_bytes;
    const uint32_t frame_start = frame_end - s_xfer_app.frame_bytes;
    uint32_t chunk = (uint32_t)(data.n - off);

    if (logical >= frame_end || logical < frame_start) {
      return false;
    }
    if (chunk > frame_end - logical) {
      chunk = frame_end - logical;
    }
    if (s_xfer_app.plane_size > 0u) {
      const uint32_t plane_end = frame_start + s_xfer_app.plane_size;

      if (logical == plane_end && !od_epd_nordic_next_plane()) {
        return false;
      }
      if (logical < plane_end && chunk > plane_end - logical) {
        chunk = plane_end - logical;
      }
    }
    if (!od_epd_nordic_write(data.p + off, chunk)) {
      return false;
    }
    off += chunk;

    if (logical + chunk == frame_end && s_xfer_app.drive_pass + 1u < s_xfer_app.drive_passes) {
      od_watchdog_app_phase(OD_WDT_PHASE_REFRESH_WAIT);
      if (!od_epd_nordic_finish()) {
        od_log_error("drive: pass %u refresh failed", (unsigned)s_xfer_app.drive_pass);
        return false;
      }
      s_xfer_app.drive_pass++;
      if (!od_epd_nordic_begin_pass(s_xfer_app.drive_pass)) {
        od_log_error("drive: pass %u begin failed", (unsigned)s_xfer_app.drive_pass);
        return false;
      }
      od_watchdog_app_phase(OD_WDT_PHASE_STREAM);
    }
  }
  return true;
}

extern "C" const struct DisplayConfig *opendisplay_display_config(void)
{
  return display_cfg();
}

extern "C" void opendisplay_display_abort(void)
{
  xfer_app_clear();
}

extern "C" bool opendisplay_display_drive_panel_up(const struct DisplayConfig *d)
{
  if (!display_power_set(true)) {
    return false;
  }
  if (!display_bus_acquire(d)) {
    display_power_set(false);
    return false;
  }
  od_epd_nordic_claim_pins(d);
  od_watchdog_app_phase(OD_WDT_PHASE_INIT_SEQ);
  return true;
}

extern "C" void opendisplay_display_drive_panel_down(void)
{
  display_power_set(false);
  od_epd_spi_deinit();
  (void)od_epd_spi_fault_reset();
}

extern "C" bool opendisplay_display_drive_adopt(const od_color_geometry_t *geometry, uint8_t passes,
                                                bool compressed, const uint8_t *payload,
                                                uint16_t payload_len)
{
  const uint64_t total = geometry != nullptr
      ? (uint64_t)geometry->total_bytes * passes : 0u;

  if (total == 0u || total > UINT32_MAX) {
    return false;
  }
  /* A compressed stream declares its uncompressed length, and passes*frame is the only value
   * accepted, so a host with the wrong pass count is told at START. */
  if (compressed) {
    uint32_t declared;

    if (payload == nullptr || payload_len < 4u) {
      return false;
    }
    declared = (uint32_t)payload[0] | ((uint32_t)payload[1] << 8)
             | ((uint32_t)payload[2] << 16) | ((uint32_t)payload[3] << 24);
    if (declared != (uint32_t)total) {
      od_log_error("drive: declared %u != passes*frame %u", (unsigned)declared, (unsigned)total);
      return false;
    }
  }
  memset(&s_xfer_app, 0, sizeof(s_xfer_app));
  s_xfer_app.mode = XFER_APP_DRIVE;
  s_xfer_app.geometry = *geometry;
  s_xfer_app.geometry.total_bytes = (uint32_t)total;
  s_xfer_app.frame_bytes = geometry->total_bytes;
  s_xfer_app.drive_passes = passes;
  s_xfer_app.plane_size = geometry->layout == OD_COLOR_LAYOUT_CONTROLLER_PLANES
      ? geometry->part_bytes[0] : 0u;
  s_xfer_app.panel_up = true;
  return true;
}

extern "C" void od_xfer_app_prepare_start(void)
{
  if (s_xfer_app.mode != XFER_APP_IDLE) {
    xfer_app_clear();
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
  out->partial_enabled = d->partial_update_support != 0u;
  return true;
}

extern "C" bool od_xfer_app_begin_full(const od_color_geometry_t *geometry)
{
  const struct DisplayConfig *d = display_cfg();
  int panel;

  if (d == nullptr || geometry == nullptr || geometry->total_bytes == 0u
      || geometry->layout == OD_COLOR_LAYOUT_SPLIT_HALVES) {
    return false;
  }
  panel = opendisplay_map_epd(d->panel_ic_type);
  if (panel == EP_PANEL_UNDEFINED) {
    return false;
  }

  if (!display_power_set(true)) {
    return false;
  }
  memset(&s_epd, 0, sizeof(s_epd));
  if (bbepSetPanelType(&s_epd, panel) != BBEP_SUCCESS) {
    display_power_set(false);
    return false;
  }
  if ((s_epd.iFlags & BBEP_SPLIT_BUFFER) != 0u) {
    od_log_error("split-panel transport is not hardware-qualified on this target");
    display_power_set(false);
    return false;
  }
  bbepSetRotation(&s_epd, (int)d->rotation * 90);
  if (!display_bus_acquire(d)) {
    display_power_set(false);
    return false;
  }
  bbepInitIO(&s_epd, d->dc_pin, d->reset_pin, d->busy_pin, d->cs_pin, d->data_pin, d->clk_pin,
             od_epd_spi_hz());
  od_bbep_wake(&s_epd);
  od_watchdog_app_phase(OD_WDT_PHASE_INIT_SEQ);
  od_bbep_send_panel_init_full(&s_epd);
  bbepSetAddrWindow(&s_epd, 0, 0, d->pixel_width, d->pixel_height);

  memset(&s_xfer_app, 0, sizeof(s_xfer_app));
  s_xfer_app.mode = XFER_APP_FULL;
  s_xfer_app.geometry = *geometry;
  s_xfer_app.width = d->pixel_width;
  s_xfer_app.height = d->pixel_height;
  s_xfer_app.plane_size = geometry->layout == OD_COLOR_LAYOUT_CONTROLLER_PLANES
      ? geometry->part_bytes[0] : 0u;
  s_xfer_app.current_plane = geometry->initial_plane == OD_COLOR_PLANE_0 ? PLANE_0 : PLANE_1;
  s_xfer_app.panel_up = true;
  bbepStartWrite(&s_epd, s_xfer_app.current_plane);
  if (od_epd_spi_faulted()) {
    xfer_app_clear();
    return false;
  }
  return true;
}

extern "C" bool od_xfer_app_begin_partial(uint16_t x, uint16_t y, uint16_t width,
                                            uint16_t height, uint32_t plane_bytes)
{
  if (plane_bytes == 0u || plane_bytes > UINT32_MAX / 2u) {
    return false;
  }
  if (!partial_prepare_panel_ram_hardware()) {
    return false;
  }
  memset(&s_xfer_app, 0, sizeof(s_xfer_app));
  s_xfer_app.mode = XFER_APP_PARTIAL;
  s_xfer_app.x = x;
  s_xfer_app.y = y;
  s_xfer_app.width = width;
  s_xfer_app.height = height;
  s_xfer_app.plane_size = plane_bytes;
  s_xfer_app.current_plane = 0xFFu;
  s_xfer_app.panel_up = true;
  return true;
}

extern "C" uint32_t od_xfer_app_write(uint32_t stream_offset, od_span_t data)
{
  bool accepted;

  if (!od_span_valid(data) || data.n == 0u || data.n > UINT32_MAX) {
    return 0u;
  }
  od_watchdog_app_phase(OD_WDT_PHASE_STREAM);
  if (s_xfer_app.mode == XFER_APP_DRIVE) {
    accepted = xfer_app_write_drive(stream_offset, data);
  } else {
    accepted = s_xfer_app.mode == XFER_APP_FULL
        ? xfer_app_write_full(stream_offset, data)
        : s_xfer_app.mode == XFER_APP_PARTIAL
            && xfer_app_write_partial(stream_offset, data);
  }
  return accepted && !od_epd_spi_faulted() ? (uint32_t)data.n : 0u;
}

extern "C" od_mut_span_t od_xfer_app_inflate_scratch(void)
{
  return od_mut_span_make(s_decompression_chunk, OPENDISPLAY_DECOMPRESSION_CHUNK_SIZE);
}

extern "C" void od_xfer_app_abort(od_xfer_abort_reason_t reason)
{
  (void)reason;
  xfer_app_clear();
}

extern "C" od_xfer_barrier_t od_xfer_app_before_refresh(const od_reply_t *owner)
{
  (void)owner;
  od_cmd_flush_before_refresh();
  od_msleep(20);
  return OD_XFER_BARRIER_PROCEED;
}

extern "C" void od_xfer_app_barrier_abort(const od_reply_t *owner)
{
  (void)owner;
  xfer_app_clear();
}

extern "C" bool od_xfer_app_refresh(uint8_t mode, bool *completed)
{
  bool ok;

  if (completed == nullptr || s_xfer_app.mode == XFER_APP_IDLE || !s_xfer_app.panel_up) {
    return false;
  }
  if (od_epd_spi_faulted()) {
    return false;
  }
  if (s_xfer_app.mode == XFER_APP_DRIVE) {
    /* The mode already names the waveform, so the refresh byte selects nothing. */
    (void)mode;
    od_watchdog_app_phase(OD_WDT_PHASE_REFRESH_WAIT);
    ok = od_epd_nordic_finish();
    od_epd_nordic_sleep();
    display_power_set(false);
    memset(&s_xfer_app, 0, sizeof(s_xfer_app));
    opendisplay_touch_resume_after_refresh();
    *completed = ok;
    return true;
  }
  if (s_xfer_app.mode == XFER_APP_PARTIAL) {
    int refresh_mode = REFRESH_PARTIAL;
    if (mode == REFRESH_FULL || mode == REFRESH_FAST) {
      refresh_mode = mode;
    }
    od_msleep(20);
    ok = partial_trigger_refresh(refresh_mode);
  } else {
    const int refresh_mode = mode == REFRESH_FAST ? REFRESH_FAST : REFRESH_FULL;
    (void)bbepRefresh(&s_epd, refresh_mode);
    ok = wait_for_refresh(60000u);
  }
  bbepSleep(&s_epd, DEEP_SLEEP);
  display_power_set(false);
  s_xfer_app.panel_up = false;
  memset(&s_xfer_app, 0, sizeof(s_xfer_app));
  opendisplay_touch_resume_after_refresh();
  *completed = ok;
  return true;
}

extern "C" uint32_t od_xfer_app_displayed_etag(void)
{
  return s_displayed_etag;
}

extern "C" void od_xfer_app_set_displayed_etag(uint32_t etag)
{
  s_displayed_etag = etag;
}

extern "C" uint32_t od_xfer_app_now_ms(void)
{
  return od_hal_uptime_ms();
}
