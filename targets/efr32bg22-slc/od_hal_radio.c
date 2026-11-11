/* BG22 notification adapter for the shared bounded response queue. */

#include "od_hal_radio.h"
#include "od_diag.h"

#include "od_txq.h"
#include "opendisplay_pipe.h"

#include <stdio.h>

od_radio_result_t od_hal_radio_send(od_origin_t origin, uint32_t tag,
                                    const uint8_t *frame, uint16_t len)
{
  sl_status_t sc;

  /* A malformed call is the caller's bug and concerns this frame only. GONE means the tag is
   * dead, and od_txq answers it by dropping every frame queued for that tag -- so folding the two
   * together discards unrelated replies. This target has no LAN transport, so a LAN origin is a
   * routing bug rather than a closed link. */
  if (frame == NULL || len == 0u || origin != OD_ORIGIN_BLE) {
    return OD_RADIO_ERROR;
  }
  if (!od_hal_radio_tag_is_live(origin, tag)) {
    return OD_RADIO_GONE;
  }
  /* Subscription state can change only by delivering a BGAPI event. Retrying while the queue's
   * event gate is closed would deadlock that event behind this entry. */
  if (!opendisplay_pipe_notify_enabled()) {
    return OD_RADIO_ERROR;
  }
  /* THE STACK OWNS THE LENGTH BOUND, and the application deliberately keeps no mirror of it.
   * sl_bt_gatt_server_send_notification returns SL_STATUS_COMMAND_TOO_LONG above ATT_MTU - 3 and
   * never truncates, so it is authoritative for the connection it is sending on. An application
   * copy could only be stale: the sole update path is sl_bt_evt_gatt_mtu_exchanged, an event of
   * the gatt CLIENT class, and this device is only a server -- so a missed event would refuse
   * frames the link can carry. COMMAND_TOO_LONG lands on the permanent arm below, which is the
   * right verdict: no retry can shorten the frame. */
  sc = sl_bt_gatt_server_send_notification(opendisplay_pipe_connection(),
                                            opendisplay_pipe_characteristic(), len, frame);
  if (sc == SL_STATUS_OK) {
    return OD_RADIO_SENT;
  }
  if (sc == SL_STATUS_NO_MORE_RESOURCE) {
    return OD_RADIO_RETRY;
  }
  OD_DIAG("[OD] pipe notify permanent failure sc=0x%04lX len=%u\r\n",
         (unsigned long)sc, (unsigned)len);
  return OD_RADIO_ERROR;
}

bool od_hal_radio_tag_is_live(od_origin_t origin, uint32_t tag)
{
  return origin == OD_ORIGIN_BLE && opendisplay_pipe_connection() != 0xFFu &&
         tag == opendisplay_pipe_connection_tag();
}

void od_txq_app_dropped(const od_reply_t *rp, uint16_t len, od_radio_result_t why)
{
  OD_DIAG("[OD] TX dropped len=%u tag=%lu reason=%d\r\n", (unsigned)len,
         (unsigned long)(rp != NULL ? rp->tag : 0u), (int)why);
}
