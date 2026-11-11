/* od_cmd_device.c -- the device and lifecycle commands: version, MSD, reboot, DFU, power-off,
 * deep sleep, LED and buzzer.
 *
 * Handlers only. The opcode map is shared/core/od_dispatch.c and the seam is od_cmd_app.h; nothing
 * here is stateful, so there is no reset for disconnect cleanup to call.
 */

#include "od_claim_mode.h"
#include "od_cmd_app.h"

#include "od_cmd_reply.h"
#include "od_log.h"
#include "opendisplay_ble.h"
#include "opendisplay_buzzer.h"
#include "opendisplay_led.h"
#include "opendisplay_protocol.h"

#include <zephyr/kernel.h>   /* CMSIS, for NVIC_SystemReset() */

#include <string.h>

#ifndef SHA
#define SHA ""
#endif
#define OD_STRINGIFY(x) #x
#define OD_XSTRINGIFY(x) OD_STRINGIFY(x)
#define SHA_STRING OD_XSTRINGIFY(SHA)

#define FIRMWARE_SHA_HEX_BYTES 40
static const char kFirmwareShaPlaceholder[FIRMWARE_SHA_HEX_BYTES + 1] =
    "0000000000000000000000000000000000000000";

/* SHA may be -DSHA=abc or CMake SHA=\"abc\"; XSTRINGIFY covers both and may
 * leave surrounding quotes. Empty / missing SHA → 40 zero hex chars (Firmware). */
static const char *fw_sha_string(void)
{
  static char sha_buf[FIRMWARE_SHA_HEX_BYTES + 1];
  const char *sha = SHA_STRING;
  size_t len;

  if (sha[0] == '"') {
    sha++;
  }
  len = strlen(sha);
  if (len > 0u && sha[len - 1u] == '"') {
    len--;
  }
  if (len == 0u) {
    return kFirmwareShaPlaceholder;
  }
  if (len > FIRMWARE_SHA_HEX_BYTES) {
    len = FIRMWARE_SHA_HEX_BYTES;
  }
  memcpy(sha_buf, sha, len);
  sha_buf[len] = '\0';
  return sha_buf;
}

od_cmd_result_t od_cmd_app_firmware_version(const od_cmd_ctx_t *ctx, od_span_t body)
{
  /* [ACK][0x43][major][minor][shaLen][sha…][patch] — patch trails so old
   * hosts that stop after SHA keep working. */
  uint8_t rsp[2 + 1 + 1 + 1 + 40 + 1];
  uint16_t ver = opendisplay_ble_get_app_version();
  uint8_t major = (uint8_t)((ver >> 8) & 0xFFu);
  uint8_t minor = (uint8_t)(ver & 0xFFu);
  uint8_t patch = opendisplay_ble_get_app_version_patch();
  const char *sha = fw_sha_string();
  uint8_t sha_len = (uint8_t)strlen(sha);
  uint16_t o = 0;

  (void)body;
  if (sha_len > 40u) {
    sha_len = 40u;
  }
  rsp[o++] = 0x00u;
  rsp[o++] = RESP_FIRMWARE_VERSION;
  rsp[o++] = major;
  rsp[o++] = minor;
  rsp[o++] = sha_len;
  memcpy(&rsp[o], sha, sha_len);
  o += sha_len;
  rsp[o++] = patch;
  /* PLAIN. A client must be able to identify a device before it can authenticate, and one whose
   * key the host has lost must stay identifiable -- which is also why od_dispatch exempts this
   * opcode from the session gate. */
  (void)od_cmd_reply_plain(ctx, rsp, o);
  return OD_CMD_OK;
}

od_cmd_result_t od_cmd_app_read_msd(const od_cmd_ctx_t *ctx, od_span_t body)
{
  uint8_t rsp[2 + 16];

  (void)body;
  rsp[0] = 0x00u;
  rsp[1] = RESP_MSD_READ;
  opendisplay_ble_copy_msd_bytes(&rsp[2]);
  (void)od_cmd_reply(ctx, rsp, sizeof(rsp));
  return OD_CMD_OK;
}

od_cmd_result_t od_cmd_app_reboot(const od_cmd_ctx_t *ctx, od_span_t body)
{
  (void)ctx;
  (void)body;
  od_log_info("reboot");
  (void)od_claim_mode_before_reboot();   /* OS2C, or END(CLAIMED); disarms the count */
  for (volatile uint32_t i = 0; i < 800000u; i++) {
  }
  NVIC_SystemReset();
  return OD_CMD_OK;                   /* not reached: the reset does not return */
}

od_cmd_result_t od_cmd_app_enter_dfu(const od_cmd_ctx_t *ctx, od_span_t body)
{
  uint8_t ok[] = { 0x00u, RESP_ENTER_DFU };

  (void)body;
  (void)od_cmd_reply(ctx, ok, sizeof(ok));
  opendisplay_ble_schedule_dfu();
  return OD_CMD_OK;
}

/* NO POWER LATCH ON THIS TARGET, and it says so rather than staying silent. Before C11 the opcode
 * fell through to the unknown arm, so a host could not distinguish "this device has no rail cut"
 * from "this firmware is older than the command" -- and 0x0052 is exactly the opcode where that
 * ambiguity is expensive, because the client's alternative is to keep retrying. The canonical
 * header defines OD_ERR_POWER_OFF_UNSUPPORTED for this answer.
 *
 * NACK, not UNKNOWN: the frame WAS recognised. It still must not stamp activity, and it does not
 * -- od_frame_policy gives HANDLER_NACK no stamp either. */
od_cmd_result_t od_cmd_app_power_off(const od_cmd_ctx_t *ctx, od_span_t body)
{
  uint8_t err[] = { 0xFFu, RESP_POWER_OFF, OD_ERR_POWER_OFF_UNSUPPORTED, 0x00u };

  (void)body;
  (void)od_cmd_reply_plain(ctx, err, sizeof(err));
  return OD_CMD_NACK;
}

od_cmd_result_t od_cmd_app_deep_sleep(const od_cmd_ctx_t *ctx, od_span_t body)
{
  /* RECOGNISED AND SILENT, matching Firmware's non-ESP32 arm
   * (../Firmware/src/device_control.cpp:931-933, inside handleDeepSleepCommand at :895), which
   * stays silent deliberately and says so. The command is acted on but NO response is sent.
   *
   * SILENCE IS NOT A DISCLAIMER, and the note below that clients will not treat deep sleep as
   * supported is wrong about the canonical host: py-opendisplay reads a disconnect or timeout as
   * a successful sleep. It is only saved today by still sending 0x0052. See docs/FOLLOWUPS.md
   * item 27 before moving that host to 0x0053 -- the answer is the unsupported NACK, not silence.
   *
   * OPCODE CHANGED 0x0052 -> 0x0053 when this target adopted the canonical protocol header. The
   * subset header it used to carry still had the value from before the split that made 0x0052
   * CMD_POWER_OFF -- a hard rail-cut -- and left deep sleep on 0x0053, which is what Firmware and
   * Firmware_Silabs already answer.
   *
   * py-opendisplay still sends 0x0052 (protocol/commands.py DEEP_SLEEP), so deep sleep from that
   * client stops working against this target until the host library is updated. That is the known
   * cost of the alignment, and 0x0052 now answers the unsupported NACK above rather than falling
   * silent -- which tells such a host something, where silence told it nothing. It MUST be fixed
   * before anything implements a real CMD_POWER_OFF here: at that point an un-updated host asking
   * for deep sleep would cut the rail. */
  (void)ctx;
  (void)body;
  opendisplay_ble_schedule_deep_sleep();
  return OD_CMD_OK;
}

od_cmd_result_t od_cmd_app_led_activate(const od_cmd_ctx_t *ctx, od_span_t body)
{
  uint8_t ok[] = { 0x00u, RESP_LED_ACTIVATE_ACK, 0x00u, 0x00u };
  uint8_t e1[] = { 0xFFu, RESP_LED_ACTIVATE_ACK, 0x01u, 0x00u };
  uint8_t e2[] = { 0xFFu, RESP_LED_ACTIVATE_ACK, 0x02u, 0x00u };
  const uint8_t *payload = body.p;
  const uint16_t payload_len = (uint16_t)body.n;

  if (payload_len < 1u) {
    (void)od_cmd_reply_plain(ctx, e1, sizeof(e1));
    return OD_CMD_NACK;
  }
  if (opendisplay_led_activate(payload[0], payload + 1u,
                               (uint16_t)(payload_len - 1u)) != 0) {
    (void)od_cmd_reply_plain(ctx, e2, sizeof(e2));
    return OD_CMD_NACK;
  }
  (void)od_cmd_reply(ctx, ok, sizeof(ok));
  return OD_CMD_OK;
}

od_cmd_result_t od_cmd_app_led_stop(const od_cmd_ctx_t *ctx, od_span_t body)
{
  uint8_t ok[] = { 0x00u, RESP_LED_STOP_ACK, 0x00u, 0x00u };
  uint8_t e2[] = { 0xFFu, RESP_LED_STOP_ACK, 0x02u, 0x00u };
  const uint8_t *payload = body.p;
  const uint16_t payload_len = (uint16_t)body.n;
  int rc;

  if (payload_len >= 1u) {
    rc = opendisplay_led_stop(payload[0], true);
  } else {
    rc = opendisplay_led_stop(0, false);
  }
  if (rc != 0) {
    (void)od_cmd_reply_plain(ctx, e2, sizeof(e2));
    return OD_CMD_NACK;
  }
  (void)od_cmd_reply(ctx, ok, sizeof(ok));
  return OD_CMD_OK;
}

od_cmd_result_t od_cmd_app_buzzer(const od_cmd_ctx_t *ctx, od_span_t body)
{
  int rc = opendisplay_buzzer_activate(body.p, (uint16_t)body.n);

  if (rc == 0) {
    uint8_t ok[] = { 0x00u, RESP_BUZZER_ACK, 0x00u, 0x00u };
    (void)od_cmd_reply(ctx, ok, sizeof(ok));
    return OD_CMD_OK;
  }
  {
    uint8_t err[] = { 0xFFu, RESP_BUZZER_ACK, (uint8_t)rc, 0x00u };
    (void)od_cmd_reply_plain(ctx, err, sizeof(err));
  }
  return OD_CMD_NACK;
}
