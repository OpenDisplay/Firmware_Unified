#include "boot_screen.h"

#include "communication.h"
#include "display_fastepd.h"
#include "display_service.h"
#include "encryption.h"
/* securityConfig is a reference to globalConfig.security, not an object of its own. Declaring it
 * here as `extern struct SecurityConfig` would compile and link, and `&securityConfig` would then
 * be the address of the reference's own pointer word rather than of the config. */
#include "encryption_state.h"
#include "od_boot_app.h"
#include "od_boot_screen.h"
#include "od_claim_app.h"
#include "od_sec2_trace.h"
#include "od_watchdog_app.h"
#include "touch_input.h"
#include "od_color.h"
#include "od_display.h"
#include "od_display_esp.h"
#include "structs.h"

#include <stdlib.h>

extern struct od_config globalConfig;
extern uint8_t staticRowBuffer[BOOT_ROW_BUFFER_SIZE];

static uint8_t s_boot_qr[256];
static const struct od_boot_bufs s_bufs = {
    staticRowBuffer, BOOT_ROW_BUFFER_SIZE, s_boot_qr, sizeof(s_boot_qr)};

/* Two sinks for one renderer: od_display's stream for config-driven panels, FastEPD's
 * framebuffer for the panels on a built-in driver (built with OD_DISPLAY_SHARED_BOOT_SINK=0). */
#if defined(OPENDISPLAY_FASTEPD)
#define NATIVE() displayUsesNativeDriver()
#else
#define NATIVE() false
#endif

int od_boot_app_begin_frame(uint16_t width, uint16_t height, uint8_t segments)
{
  if (NATIVE()) return segments == 1u ? 0 : -1;
  return od_display_boot_sink_begin_frame(width, height, segments);
}

int od_boot_app_begin_plane(int plane)
{
  if (NATIVE()) return 0;
  return od_display_boot_sink_begin_plane(plane);
}

int od_boot_app_write_row(uint16_t y, uint8_t segment, const uint8_t *row, uint16_t len)
{
#if defined(OPENDISPLAY_FASTEPD)
  if (NATIVE()) {
    if (segment != 0u) return -1;
    fastepd_boot_write_row(y, row, len);
    return 0;
  }
#endif
  return od_display_boot_sink_write_row(y, segment, row, len);
}

int od_boot_app_end_plane(int plane)
{
  if (NATIVE()) return 0;
  return od_display_boot_sink_end_plane(plane);
}

int od_boot_app_end_frame(void)
{
#if defined(OPENDISPLAY_FASTEPD)
  if (NATIVE()) {
    fastepd_boot_skip_planes();
    return 0;
  }
#endif
  return od_display_boot_sink_end_frame();
}

int od_boot_app_bits_per_pixel(uint8_t color_scheme)
{
  if (NATIVE()) return displayBootBitsPerPixel(color_scheme);
  return od_display_boot_sink_bits_per_pixel(color_scheme);
}

int od_boot_app_default_plane(uint8_t color_scheme)
{
  if (NATIVE()) {
    return (color_scheme == OD_COLOR_SCHEME_MONO || color_scheme == OD_COLOR_SCHEME_GRAY16 ||
            color_scheme == OD_COLOR_SCHEME_BWR || color_scheme == OD_COLOR_SCHEME_BWY)
        ? OD_BOOT_PLANE_PRIMARY : OD_BOOT_PLANE_SECOND;
  }
  return od_display_boot_sink_default_plane(color_scheme);
}

bool od_boot_app_direct_2bpp(void)
{
  if (NATIVE()) return true;
  return od_display_boot_sink_direct_2bpp();
}

uint8_t od_boot_app_segments(void)
{
  if (NATIVE()) return 1u;
  return od_display_boot_sink_segments();
}

uint32_t od_boot_app_device_id24(void)
{
  char id[7];
  getChipIdHex(id, sizeof(id));
  return (uint32_t)strtoul(id, NULL, 16) & 0xFFFFFFu;
}

void od_boot_app_firmware_version(uint8_t *major, uint8_t *minor, uint8_t *patch)
{
  *major = getFirmwareMajor();
  *minor = getFirmwareMinor();
  *patch = getFirmwarePatch();
}

float od_boot_app_battery_volts(void) { return readBatteryVoltage(); }
float od_boot_app_chip_temp_c(void) { return readChipTemperature(); }

const struct od_boot_bufs *bootScreenBufs(void)
{
  return &s_bufs;
}

bool writeBootScreenWithQr()
{
  return od_boot_screen_render(&globalConfig, &securityConfig, &s_bufs);
}

/* SECURITY_V2 7.8 / FACTORY_RESET.md 4 (od_claim_app.h): a setup-family screen through the same
 * two sinks as the boot screen. Blocks for the refresh; the caller has flushed its replies. */
static const char *setup_kind_name(enum od_setup_screen kind)
{
  switch (kind) {
  case OD_SETUP_SCREEN_SETUP: return "SETUP";
  case OD_SETUP_SCREEN_CODE:  return "CODE";
  case OD_SETUP_SCREEN_RESET: return "RESET";
  default:                    return "NORMAL";
  }
}

static bool claim_app_draw(enum od_setup_screen kind, uint32_t code);

extern "C" bool od_claim_app_draw(enum od_setup_screen kind, uint32_t code)
{
  const bool ok = claim_app_draw(kind, code);

  OD_SEC2_TRACE("od_claim_app_draw(%s) -> %s (display_count=%u needs_migration=%u)",
                setup_kind_name(kind), ok ? "drawn" : "NOT drawn",
                (unsigned)globalConfig.display_count, (unsigned)od_display_needs_migration());
  (void)setup_kind_name;
  return ok;
}

static bool claim_app_draw(enum od_setup_screen kind, uint32_t code)
{
  bool ok = false;

  if (kind == OD_SETUP_SCREEN_NORMAL || globalConfig.display_count == 0u) {
    return false;
  }
  od_watchdog_app_phase(OD_WDT_PHASE_BOOT_REFRESH);
#if defined(OPENDISPLAY_FASTEPD)
  if (NATIVE()) {
    if (displayBootBitsPerPixel(globalConfig.displays[0].color_scheme) != 0) {
      displayRailAcquire();
      fastepd_epaper_begin();
      if (fastepd_init_failed()) {
        fastepd_mark_hw_deinitialized();
      } else if (od_boot_screen_render_setup(&globalConfig, kind, code, &s_bufs)) {
        touchSuspendForEpdRefresh();
        fastepd_full_update();
        ok = fastepd_wait_refresh(60);
        touchResumeAfterEpdRefresh();
      }
      epdSessionForceOff();
    }
    od_watchdog_app_phase(OD_WDT_PHASE_IDLE);
    return ok;
  }
#endif
  if (!od_display_needs_migration()) {
    touchSuspendForEpdRefresh();
    ok = od_display_setup_screen(&globalConfig, kind, code, &s_bufs) == OD_DISPLAY_OK;
    epdSessionForceOff();   /* ends with the rail off, like the boot screen */
    touchResumeAfterEpdRefresh();
  }
  od_watchdog_app_phase(OD_WDT_PHASE_IDLE);
  return ok;
}
