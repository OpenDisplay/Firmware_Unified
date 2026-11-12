/* Nordic display glue over the shared config-driven path (shared/core/od_display.h).
 *
 * What stays here is what is genuinely this target's: the panel rail and its settle times, the
 * SPI bus acquire, the watchdog breadcrumbs, the image-store diverts, CMD_SPI_RAW, and the
 * boot-time policy (safe mode, CLEAR_ON_BOOT, two attempts). Every panel command comes from the
 * drive packets; a config without them draws nothing and says "needs migration".
 */
#include "od_sec2_trace.h"
#include "opendisplay_display.h"

#include "od_board.h"
#include "od_boot_app.h"
#include "od_boot_screen.h"
#include "od_cmd_reply.h"
#include "od_display.h"
#include "od_claim_app.h"
#include "od_display_app.h"
#include "od_drive_app.h"
#include "od_epd_cfg.h"
#include "od_epd_nordic.h"
#include "od_epd_sizes.h"
#include "od_epd_spi.h"
#include "od_gpio.h"
#include "od_hal_time.h"
#include "od_ident_app.h"
#include "od_log.h"
#include "od_claim_mode.h"
#include "od_sec2_app.h"
#include "od_spi_raw_app.h"
#include "od_txq.h"
#include "od_watchdog_app.h"
#include "od_xfer.h"
#include "od_xfer_app.h"
#include "od_zephyr_compat.h"
#include "opendisplay_battery.h"
#include "opendisplay_ble.h"
#include "opendisplay_config_parser.h"
#include "opendisplay_protocol.h"
#include "opendisplay_touch.h"
#if OD_CAP_IMGSTORE
#include "od_cmd_flash.h"
#include "od_imgstore.h"
#endif

#include <string.h>

/* 0x0072 refresh byte. */
#define REFRESH_FULL 0u

static bool s_boot_applied;
/* Set by a config write while the boot screen has never been shown; serviced on the loop. */
static bool s_boot_pending;
static uint8_t s_decompression_chunk[OPENDISPLAY_DECOMPRESSION_CHUNK_SIZE];
#if OD_CAP_PARTIAL
static uint32_t s_displayed_etag;
#endif
static bool s_epd_rail_conditioned;
static bool s_spi_raw_open;
#if OD_CAP_IMGSTORE
/* The current upload goes to a flash slot; the panel is never touched. */
static bool s_to_flash;
#endif

/* Boot screen buffers: one native row (680 B covers 1360 px at 4 bpp, or a GRAY4 row plus its
 * de-interleaved plane row) and the QR module bitmap. */
static uint8_t s_boot_row[680];
static uint8_t s_boot_qr[256];

static const struct DisplayConfig *display_cfg(void)
{
  const struct od_config *cfg = opendisplay_get_global_config();

  if (cfg == NULL || cfg->display_count == 0u) {
    return NULL;
  }
  return &cfg->displays[0];
}

static bool display_bus_acquire(const struct DisplayConfig *d)
{
  if (d == NULL || !od_epd_spi_init(d->data_pin, d->clk_pin)) {
    od_log_error("panel: SPI acquire failed");
    return false;
  }
  return true;
}

void opendisplay_display_park_pins(void)
{
  const struct DisplayConfig *d = display_cfg();

  if (d == NULL) {
    return;
  }
  od_gpio_park(d->cs_pin);
  od_gpio_park(d->cs_pin_2);
  od_gpio_park(d->data_pin);
  od_gpio_park(d->clk_pin);
  od_gpio_park(d->dc_pin);
  od_gpio_park(d->reset_pin);
  od_gpio_park(d->busy_pin);
}

/*
 * PANEL RAIL BRING-UP, ported from the Arduino reference's pwrmgm(true)
 * (Firmware/src/main.cpp:1341-1382), the behaviour the deployed configs were validated against.
 *
 * The rail needs ~900 ms before the panel will talk. Run the init against an unpowered
 * controller and an unpowered BUSY line reads idle, every wait returns at once, the whole frame
 * is clocked into a dead controller, and nothing reports an error until the refresh times out.
 * The two delays are separate and both are needed: 800 ms for the rail itself, then the signal
 * lines are pre-driven to their idle levels and 100 ms lets the panel see them settle before RST
 * is toggled.
 */
#define OD_PANEL_RAIL_SETTLE_MS 800u
#define OD_PANEL_PIN_SETTLE_MS  100u

static bool display_power_set(bool on)
{
  const struct od_config *cfg = opendisplay_get_global_config();
  const struct DisplayConfig *d = display_cfg();
  uint8_t p;

  /* Never bring the rail up in watchdog safe mode; powering down is still allowed. This is the
   * one refusal point for the boot screen and every pushed image alike. */
  if (on && od_watchdog_app_safe_mode()) {
    od_log_warn("panel: power-up refused - watchdog safe mode");
    return false;
  }
  if (!on) {
    od_epd_spi_deinit();
  }
  if (cfg == NULL) {
    od_log_error("panel: global config unavailable");
    return false;
  }
  p = cfg->system_config.pwr_pin;
  /* 0xFF is the contract's "not present": the rail is permanently powered. There is no pin to
   * drive and no settle to wait for, and the signal lines stay driven at their idle levels
   * rather than floating into a powered controller. Never substitute a guessed pin. */
  const bool has_pwr_pin = (p != 0xFFu);

  if (!on) {
    od_watchdog_app_phase(OD_WDT_PHASE_FORCE_OFF);
    if (has_pwr_pin) {
      opendisplay_display_park_pins();
      od_gpio_configure_output(p, false);
    } else if (d != NULL) {
      /* nrfx release disconnects SCK/MOSI; a permanently powered controller must see a
       * driven-low idle bus instead. */
      od_gpio_configure_output(d->clk_pin, false);
      od_gpio_configure_output(d->data_pin, false);
    }
    od_watchdog_app_phase(OD_WDT_PHASE_IDLE_OFF);
    return true;
  }

  od_watchdog_app_phase(OD_WDT_PHASE_PWRMGM_RAIL);
  /* Boost-select conditioning is a panel-interface setting, needed with or without a rail
   * switch. */
  od_board_prepare_epd_rail();

  if (!has_pwr_pin) {
    od_log_warn("panel: pwr_pin not present; rail is permanently powered");
  } else {
    od_gpio_configure_output(p, true);
    /* The deployed nRF52840 board needs the donor firmware's one-time cold rail conditioning:
     * BS low, on 50 ms, off 50 ms, BS low, on. nRF54 boards return false. */
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

  od_watchdog_app_phase(OD_WDT_PHASE_PWRMGM_PINS);
  if (d != NULL) {
    od_gpio_configure_output(d->reset_pin, true);
    od_gpio_configure_output(d->cs_pin, true);
    if (d->cs_pin_2 != 0xFFu && d->cs_pin_2 != d->cs_pin) {
      od_gpio_configure_output(d->cs_pin_2, true);
    }
    od_gpio_configure_output(d->dc_pin, false);
    od_gpio_configure_output(d->clk_pin, false);
    od_gpio_configure_output(d->data_pin, false);
    od_gpio_configure_input(d->busy_pin, false, false);
    od_msleep(OD_PANEL_PIN_SETTLE_MS);
  }
  return true;
}

/* --- od_display_app / od_drive_app seams ---------------------------------------------------- */

void od_display_app_phase(od_display_phase_t phase)
{
  switch (phase) {
  case OD_DISPLAY_PHASE_INIT:
    od_watchdog_app_phase(OD_WDT_PHASE_INIT_SEQ);
    break;
  case OD_DISPLAY_PHASE_STREAM:
    od_watchdog_app_phase(OD_WDT_PHASE_STREAM);
    break;
  case OD_DISPLAY_PHASE_REFRESH:
    od_watchdog_app_phase(OD_WDT_PHASE_REFRESH_WAIT);
    break;
  case OD_DISPLAY_PHASE_IDLE:
  default:
    /* display_power_set(false) already stamped FORCE_OFF then IDLE_OFF. */
    break;
  }
}

const struct DisplayConfig *od_drive_app_display(void)
{
  return display_cfg();
}

const od_epd_cfg_bus_t *od_drive_app_bus(void)
{
  return od_epd_nordic_bus();
}

static void xfer_app_clear(void)
{
#if OD_CAP_IMGSTORE
  if (s_to_flash) {
    od_flash_store_abort();
  }
  s_to_flash = false;
  od_cmd_flash_clear_arm();
#endif
  od_display_abort();
  od_epd_spi_deinit();
  (void)od_epd_spi_fault_reset();
}

void od_drive_app_abort(void)
{
  if (s_spi_raw_open) {
    opendisplay_display_spi_raw_close();
  }
  xfer_app_clear();
}

bool od_drive_app_panel_up(const struct DisplayConfig *d)
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

void od_drive_app_panel_down(void)
{
  display_power_set(false);
  od_epd_spi_deinit();
  (void)od_epd_spi_fault_reset();
}

bool od_drive_app_adopt(const od_color_geometry_t *frame, uint8_t passes, bool compressed,
                        od_span_t tail)
{
  return od_display_adopt_drive(frame, passes, compressed, tail);
}

/* The die sensor, an estimate of the glass for policy purposes. */
int8_t od_drive_app_temp_c(void)
{
  const float t = opendisplay_ble_get_chip_temperature();

  if (t < -128.0f || t > 126.0f) {
    return OD_DRIVE_TEMP_UNKNOWN;
  }
  return (int8_t)(t + (t >= 0.0f ? 0.5f : -0.5f));
}

const struct od_config *od_ident_app_config(void)
{
  return opendisplay_get_global_config();
}

/* --- boot screen ------------------------------------------------------------------------------
 * The panel half of od_boot_app.h is od_display.c's; this is the identity half. */

uint32_t od_boot_app_device_id24(void)
{
  return opendisplay_ble_chip_id_last24();
}

void od_boot_app_firmware_version(uint8_t *major, uint8_t *minor, uint8_t *patch)
{
  const uint16_t version = opendisplay_ble_get_app_version();

  *major = (uint8_t)(version >> 8);
  *minor = (uint8_t)version;
  *patch = opendisplay_ble_get_app_version_patch();
}

float od_boot_app_battery_volts(void)
{
  return opendisplay_battery_read_voltage_volts();
}

float od_boot_app_chip_temp_c(void)
{
  return opendisplay_ble_get_chip_temperature();
}

bool opendisplay_display_boot_apply(void)
{
  const struct DisplayConfig *d = display_cfg();
  const struct od_boot_bufs bufs = { s_boot_row, sizeof(s_boot_row),
                                     s_boot_qr, sizeof(s_boot_qr) };

  s_boot_pending = false;
  if (s_boot_applied) {
    return true;
  }
  /* Three consecutive watchdog resets say the panel path is what wedges. Reported as applied so
   * nothing retries: the device stays up on BLE and DFU, the only way to replace a bad config. */
  if (od_watchdog_app_safe_mode()) {
    od_log_warn("boot display skipped: watchdog safe mode");
    s_boot_applied = true;
    return true;
  }
  if (d == NULL) {
    od_log_error("boot display: no display config");
    return false;
  }
  if ((d->transmission_modes & OD_TRANSMISSION_MODE_CLEAR_ON_BOOT) != 0u) {
    od_log_info("boot display intentionally skipped by CLEAR_ON_BOOT");
    s_boot_applied = true;
    return true;
  }
  /* Not latched: the first config write that brings drive packets shows the boot screen. */
  if (od_display_needs_migration()) {
    od_log_warn("boot display skipped: config has no drive packets (needs migration)");
    return true;
  }

  for (unsigned attempt = 1; attempt <= 2; attempt++) {
    OD_SEC2_TRACE("draw NORMAL boot screen (attempt %u) at %u ms", attempt,
                  (unsigned)od_hal_uptime_ms());
    const od_display_rc_t rc = od_display_boot(opendisplay_get_global_config(),
                                               od_get_parsed_security(), &bufs);
    OD_SEC2_TRACE("NORMAL boot screen rc=%d at %u ms", (int)rc, (unsigned)od_hal_uptime_ms());

    od_epd_spi_deinit();
    (void)od_epd_spi_fault_reset();
    if (rc == OD_DISPLAY_OK) {
      s_boot_applied = true;
      od_log_info("boot display refresh complete");
      return true;
    }
    if (rc != OD_DISPLAY_PANEL) {
      /* The config cannot render a boot screen; a second attempt would fail identically. */
      od_log_error("boot display: refused rc=%d", (int)rc);
      return false;
    }
    od_log_warn("boot display: panel failed (attempt %u of 2)", attempt);
    if (attempt < 2) {
      od_msleep(100);
    }
  }
  return false;
}

/* SECURITY_V2 7.8 / FACTORY_RESET.md 4 (od_claim_app.h): a setup-family screen through the
 * mode-0 boot pass. Blocks for the refresh. A drawn screen also latches the boot screen, so a
 * config write in claim mode does not paint the normal screen over it. */
bool od_claim_app_draw(enum od_setup_screen kind, uint32_t code)
{
	const struct od_boot_bufs bufs = { s_boot_row, sizeof(s_boot_row),
					   s_boot_qr, sizeof(s_boot_qr) };
	od_display_rc_t rc;

	if (kind == OD_SETUP_SCREEN_NORMAL || display_cfg() == NULL ||
	    od_watchdog_app_safe_mode() || od_display_needs_migration()) {
		return false;
	}
	OD_SEC2_TRACE("od_claim_app_draw(kind=%d) start at %u ms", (int)kind,
		      (unsigned)od_hal_uptime_ms());
	rc = od_display_setup_screen(opendisplay_get_global_config(), kind, code, &bufs);
	OD_SEC2_TRACE("od_claim_app_draw(kind=%d) rc=%d at %u ms", (int)kind, (int)rc,
		      (unsigned)od_hal_uptime_ms());
	od_epd_spi_deinit();
	(void)od_epd_spi_fault_reset();
	if (rc != OD_DISPLAY_OK) {
		od_log_warn("setup screen %d: rc=%d", (int)kind, (int)rc);
		return false;
	}
	s_boot_applied = true;
	return true;
}

void opendisplay_display_request_boot(void)
{
  if (!s_boot_applied) {
    s_boot_pending = true;
  }
}

/* SECURITY_V2 10.7 (od_sec2_app.h): the key lines went stale, so the NORMAL boot screen is drawn
 * again over whatever the panel shows, from opendisplay_display_service() once the link is idle. */
void od_sec2_app_key_screen_changed(void)
{
  if (od_claim_mode_active()) {
    return;
  }
  s_boot_applied = false;
  s_boot_pending = true;
}

/* --- CMD_SPI_RAW ------------------------------------------------------------------------------
 *
 * The session owns the panel: OPEN aborts any transfer, and the transfer abort path (which a BLE
 * disconnect reaches) closes the session, so bytes never go to an unpowered panel. Needs drive
 * packets: without them there is no BUSY sense and no init sequence. */

bool opendisplay_display_spi_raw_is_open(void)
{
  return s_spi_raw_open;
}

int opendisplay_display_spi_raw_open(bool run_panel_init)
{
  const struct DisplayConfig *d = display_cfg();

  od_drive_app_abort();
  if (d == NULL || !od_epd_cfg_load(d, od_epd_nordic_bus())) {
    return -OD_ERR_SPI_RAW_NO_CONFIG;
  }
  if (!od_drive_app_panel_up(d)) {
    return -OD_ERR_SPI_RAW_PANEL;
  }
  /* run_panel_init stops at the image marker, so the caller still owns everything after the
   * registers. Without it nothing is clocked out and virgin register state is observable. */
  if (run_panel_init && (!od_epd_cfg_select(0u, NULL) || !od_epd_cfg_begin_pass(0u))) {
    od_log_warn("spi_raw: panel init failed rc=%d", od_epd_cfg_last_rc());
    od_drive_app_panel_down();
    return -OD_ERR_SPI_RAW_PANEL;
  }
  if (run_panel_init) {
    od_epd_cfg_raw_detach();
  }
  s_spi_raw_open = true;
  return 0;
}

int opendisplay_display_spi_raw_cmd(const uint8_t *data, uint16_t len)
{
  if (!s_spi_raw_open) {
    return -OD_ERR_SPI_RAW_NOT_OPEN;
  }
  if (data == NULL || len == 0u) {
    return -OD_ERR_SPI_RAW_BAD_ARGS;
  }
  od_epd_cfg_raw_cmd(data, len);
  return 0;
}

int opendisplay_display_spi_raw_data(const uint8_t *data, uint16_t len)
{
  if (!s_spi_raw_open) {
    return -OD_ERR_SPI_RAW_NOT_OPEN;
  }
  if (data == NULL || len == 0u) {
    return -OD_ERR_SPI_RAW_BAD_ARGS;
  }
  od_epd_cfg_raw_data(data, len);
  return 0;
}

int opendisplay_display_spi_raw_busy(uint32_t timeout_ms, uint16_t *elapsed_ms_out,
                                     bool *still_busy_out)
{
  if (!s_spi_raw_open) {
    return -OD_ERR_SPI_RAW_NOT_OPEN;
  }
  if (timeout_ms > 90000u) {
    timeout_ms = 90000u;
  }
  od_watchdog_app_phase(OD_WDT_PHASE_REFRESH_WAIT);
  od_epd_cfg_raw_busy(timeout_ms, elapsed_ms_out, still_busy_out);
  return 0;
}

int opendisplay_display_spi_raw_reset(uint8_t hold_ms)
{
  if (!s_spi_raw_open) {
    return -OD_ERR_SPI_RAW_NOT_OPEN;
  }
  /* Clamped per the protocol: 0 is not a pulse, and a long hold from one byte is a mistake. */
  od_epd_cfg_raw_reset(hold_ms < 1u ? 1u : (hold_ms > 50u ? 50u : hold_ms));
  return 0;
}

/* Idempotent, so a host recovering from a dropped link can always get to a known state. */
void opendisplay_display_spi_raw_close(void)
{
  if (!s_spi_raw_open) {
    return;
  }
  s_spi_raw_open = false;
  od_epd_cfg_sleep();
  od_drive_app_panel_down();
}

/* --- od_xfer_app ------------------------------------------------------------------------------ */

static bool xfer_app_busy(void)
{
#if OD_CAP_IMGSTORE
  if (s_to_flash) {
    return true;
  }
#endif
  return od_display_active();
}

void od_xfer_app_prepare_start(void)
{
  if (xfer_app_busy()) {
    xfer_app_clear();
  }
}

bool od_xfer_app_panel_info(od_xfer_panel_info_t *out)
{
  const struct DisplayConfig *d = display_cfg();
  bool geometry_ok;

  if (out == NULL || d == NULL) {
    return false;
  }
  memset(out, 0, sizeof(*out));
  out->needs_migration = od_display_needs_migration();
  geometry_ok = od_color_direct_geometry(d->color_scheme, d->pixel_width, d->pixel_height,
                                         &out->geometry) == OD_COLOR_OK;
  if (!geometry_ok && !out->needs_migration) {
    return false;
  }
  out->width = d->pixel_width;
  out->height = d->pixel_height;
  /* Partial needs a "partial" mode to drive it, whatever 0x20 claims. */
  out->partial_enabled = !out->needs_migration && od_display_partial_available();
  return true;
}

static bool display_started(od_display_rc_t rc, const char *what)
{
  if (rc != OD_DISPLAY_OK) {
    if (rc == OD_DISPLAY_PANEL_BUSY) {
      od_xfer_app_note_start_error(OD_ERR_PANEL_BUSY);
    }
    od_log_error("display: %s refused rc=%d", what, (int)rc);
    od_epd_spi_deinit();
    (void)od_epd_spi_fault_reset();
    return false;
  }
  return true;
}

bool od_xfer_app_begin_full(const od_color_geometry_t *geometry)
{
  const struct DisplayConfig *d = display_cfg();

  if (d == NULL || geometry == NULL || geometry->total_bytes == 0u) {
    return false;
  }
#if OD_CAP_IMGSTORE
  /* Checked before the rail comes up, so a stored upload costs no refresh. */
  {
    uint8_t slot = 0u;

    if (od_cmd_flash_take_arm(&slot)) {
      if (!od_flash_store_begin(slot, d->pixel_width, d->pixel_height, d->color_scheme,
                                geometry->total_bytes)) {
        return false;
      }
      s_to_flash = true;
      return true;
    }
  }
#endif
  return display_started(od_display_begin_full(geometry), "full upload");
}

#if OD_CAP_PARTIAL
bool od_xfer_app_begin_partial(uint16_t x, uint16_t y, uint16_t width, uint16_t height,
                               uint32_t plane_bytes)
{
  return display_started(od_display_begin_partial(x, y, width, height, plane_bytes),
                         "partial upload");
}
#endif

uint32_t od_xfer_app_write(uint32_t stream_offset, od_span_t data)
{
  bool accepted;

  if (!od_span_valid(data) || data.n == 0u || data.n > UINT32_MAX) {
    return 0u;
  }
#if OD_CAP_IMGSTORE
  if (s_to_flash) {
    return od_imgstore_write(data.p, (uint32_t)data.n) ? (uint32_t)data.n : 0u;
  }
#endif
  od_watchdog_app_phase(OD_WDT_PHASE_STREAM);
  accepted = od_display_write(stream_offset, data.p, data.n);
  return accepted && !od_epd_spi_faulted() ? (uint32_t)data.n : 0u;
}

od_mut_span_t od_xfer_app_inflate_scratch(void)
{
  return od_mut_span_make(s_decompression_chunk, OPENDISPLAY_DECOMPRESSION_CHUNK_SIZE);
}

void od_xfer_app_abort(od_xfer_abort_reason_t reason)
{
  (void)reason;
  xfer_app_clear();
}

od_xfer_barrier_t od_xfer_app_before_refresh(const od_reply_t *owner)
{
  (void)owner;
  od_cmd_flush_before_refresh();
  od_msleep(20);
  return OD_XFER_BARRIER_PROCEED;
}

void od_xfer_app_barrier_abort(const od_reply_t *owner)
{
  (void)owner;
  xfer_app_clear();
}

bool od_xfer_app_refresh(uint8_t mode, bool *completed)
{
  bool ok;

#if OD_CAP_IMGSTORE
  if (completed != NULL && s_to_flash) {
    s_to_flash = false;
    *completed = od_flash_store_finish();
    return true;
  }
#endif
  if (completed == NULL || !od_display_active() || od_epd_spi_faulted()) {
    return false;
  }
  ok = od_display_refresh(mode);
  od_epd_spi_deinit();
  (void)od_epd_spi_fault_reset();
  opendisplay_touch_resume_after_refresh();
  *completed = ok;
  return true;
}

#if OD_CAP_PARTIAL
uint32_t od_xfer_app_displayed_etag(void)
{
  return s_displayed_etag;
}

void od_xfer_app_set_displayed_etag(uint32_t etag)
{
  s_displayed_etag = etag;
}
#endif

uint32_t od_xfer_app_now_ms(void)
{
  return od_hal_uptime_ms();
}

/* --- loop service: stored-image display and the deferred boot screen ------------------------ */

#if OD_CAP_IMGSTORE
/* -1 = nothing queued. Set by a command, consumed by the loop. */
static int16_t s_pending_slot = -1;

bool od_display_from_slot_arm(uint8_t index)
{
  const struct DisplayConfig *d = display_cfg();
  od_imgstore_slot_t slot;
  bool valid;

  if (d == NULL || xfer_app_busy() || od_display_needs_migration()) {
    return false;
  }
  od_flash_store_wake();
  valid = od_imgstore_slot(index, &slot) && slot.state == OD_SLOT_VALID;
  od_flash_store_sleep();
  /* A stored frame only means something on the panel it was rendered for. */
  if (!valid || slot.width != d->pixel_width || slot.height != d->pixel_height
      || slot.colour_scheme != d->color_scheme) {
    return false;
  }
  s_pending_slot = (int16_t)index;
  return true;
}

static void display_pending_slot(void)
{
  const struct DisplayConfig *d = display_cfg();
  od_imgstore_slot_t slot;
  od_color_geometry_t geometry;
  uint8_t index;
  uint32_t off = 0u;
  bool ok;
  bool completed = false;

  index = (uint8_t)s_pending_slot;
  s_pending_slot = -1;
  if (d == NULL || od_color_direct_geometry(d->color_scheme, d->pixel_width, d->pixel_height,
                                            &geometry) != OD_COLOR_OK) {
    return;
  }
  od_flash_store_wake();
  ok = od_imgstore_slot(index, &slot) && slot.state == OD_SLOT_VALID
      && slot.payload_len == geometry.total_bytes;
  if (ok) {
    od_cmd_flash_clear_arm();
    ok = od_xfer_app_begin_full(&geometry);
  }
  while (ok && off < slot.payload_len) {
    uint32_t n = slot.payload_len - off;

    if (n > OPENDISPLAY_DECOMPRESSION_CHUNK_SIZE) {
      n = OPENDISPLAY_DECOMPRESSION_CHUNK_SIZE;
    }
    ok = od_imgstore_read(index, off, s_decompression_chunk, n)
        && od_xfer_app_write(off, od_span_make(s_decompression_chunk, n)) == n;
    off += n;
  }
  od_flash_store_sleep();
  if (ok) {
    ok = od_xfer_app_refresh(REFRESH_FULL, &completed) && completed;
  } else if (xfer_app_busy()) {
    xfer_app_clear();
  }
  if (!ok) {
    od_log_error("flash: display of slot %u failed", (unsigned)index);
  }
}
#endif

void opendisplay_display_service(void)
{
  if (od_txq_depth() != 0u || od_xfer_active() || xfer_app_busy() || s_spi_raw_open) {
    return;
  }
#if OD_CAP_IMGSTORE
  if (s_pending_slot >= 0) {
    display_pending_slot();
    return;
  }
#endif
  if (s_boot_pending) {
    if (!opendisplay_display_boot_apply()) {
      od_log_error("boot display after config write failed");
    }
  }
}
