/* BG22 half of shared/core/od_sec2_app.h: clock, uid, panel facts, the parsed 0x27. */

#include "od_advert.h"
#include "od_sec2_app.h"
#include "od_sec2_wire.h"

#include "em_system.h"
#include "od_hal_time.h"
#include "od_xfer.h"
#include "opendisplay_ble.h"
#include "opendisplay_config_parser.h"

uint32_t od_sec2_app_now_ms(void)
{
  return od_hal_uptime_ms();
}

/* SECURITY_V2 4.3: SYSTEM_GetUnique() as 8 bytes big-endian. */
void od_sec2_app_uid(uint8_t uid[8])
{
  const uint64_t u = SYSTEM_GetUnique();
  unsigned i;

  for (i = 0; i < 8u; ++i) {
    uid[i] = (uint8_t)(u >> (56u - 8u * i));
  }
}

uint8_t od_sec2_app_dflags(void)
{
  const struct SecurityConfig *sec = od_get_parsed_security();
  const struct GlobalConfig *cfg = opendisplay_get_global_config();
  uint8_t f = 0u;

  if (od_ident_display_drivable(cfg)) {
    f |= OD_SEC2_DFLAG_HAS_PANEL;
  }
  if (sec != NULL && (sec->flags & OD_SECURITY_FLAG_RESET_PIN_ENABLED) != 0u) {
    f |= OD_SEC2_DFLAG_HAS_RESET_PIN;
  }
  return f;
}

const struct SecurityConfig *od_sec2_app_security(void)
{
  return od_get_parsed_security();
}

/* A display pass runs or is queued. BG22's refresh blocks the loop, so a handshake frame is only
 * seen between passes; this covers the queued transfer that is about to start one. */
bool od_sec2_app_display_busy(void)
{
  return od_xfer_active();
}
