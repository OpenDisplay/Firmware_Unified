/* BG22 ownership seam for shared/core/od_session. */

#include "od_session_app.h"

#include "em_system.h"
#include "opendisplay_config_parser.h"
#include "od_hal_time.h"

#include <stdio.h>

static struct od_session s_session;
static uint32_t s_open_log_ms;

struct od_session *od_session_app_state(void)
{
  return &s_session;
}

const struct SecurityConfig *od_session_app_security(void)
{
  return od_get_parsed_security();
}

uint32_t od_session_app_now_ms(void)
{
  return od_hal_uptime_ms();
}

void od_session_app_device_id(uint8_t out[OD_SESSION_DEVICE_ID_LEN])
{
  uint64_t uid = SYSTEM_GetUnique();

  out[0] = (uint8_t)(uid >> 24);
  out[1] = (uint8_t)(uid >> 16);
  out[2] = (uint8_t)(uid >> 8);
  out[3] = (uint8_t)uid;
}

void od_session_app_report(enum od_session_app_op op, int result, uint16_t cmd,
                           const struct od_session_report *report)
{
  uint32_t now;

  if (op == OD_SESSION_APP_AUTH) {
    if (result == OD_SESSION_AUTH_ESTABLISHED) {
      printf("[OD] auth session established\r\n");
    } else if (result != OD_SESSION_AUTH_CHALLENGE) {
      printf("[OD] auth refused rc=%d status=0x%02X crypto=%d\r\n", result,
             (unsigned)(report != NULL ? report->status_byte : 0u),
             (int)(report != NULL ? report->crypto_status : OD_HAL_CRYPTO_OK));
    }
    return;
  }
  if (op != OD_SESSION_APP_OPEN || result == OD_SESSION_OPEN_OK) {
    return;
  }
  now = od_session_app_now_ms();
  if (s_open_log_ms == 0u || (uint32_t)(now - s_open_log_ms) >= 5000u) {
    s_open_log_ms = now;
    printf("[OD] decrypt failed cmd=0x%04X rc=%d nonce=%u\r\n", (unsigned)cmd, result,
           (unsigned)(report != NULL ? report->nonce_reason : 0u));
  }
}
