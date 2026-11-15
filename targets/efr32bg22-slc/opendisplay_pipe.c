/* BG22 transport binding for shared dispatch/session/egress. */

#include "opendisplay_pipe.h"
#include "od_diag.h"

#include "od_cmd.h"
#include "od_config_read.h"
#include "od_core.h"
#include "od_dispatch.h"
#include "od_sec2.h"
#include "od_span.h"
#include "od_txq.h"
#include "od_xfer.h"
#include "opendisplay_display.h"

#include "sl_component_catalog.h"
#include "od_hal_time.h"

#include <stdio.h>

#ifdef SL_CATALOG_KERNEL_PRESENT
#error "C13 BGAPI event retention requires the no-kernel sl_bt_step() path"
#endif

#define OD_EVENT_HOLD_MAX_MS  2000u
#define OD_CONFIG_READ_MAX_MS 1500u
#define OD_AUTH_ABUSE_LIMIT  3u

#if OD_CONFIG_READ_MAX_MS >= OD_EVENT_HOLD_MAX_MS
#error "CONFIG_READ must expire before the outer command-transport hold"
#endif

static uint16_t s_attr;
static uint8_t s_connection = 0xFFu;
static uint32_t s_connection_epoch;
static bool s_notify;
static bool s_tx_report_ok = true;
static bool s_tx_report_warned;
static uint32_t s_hold_started_ms;
static uint32_t s_read_started_ms;
static bool s_hold_active;
static bool s_read_active;
static uint8_t s_auth_abuse;

void od_cmd_silabs_reset(void);

static uint32_t now_ms(void)
{
  return od_hal_uptime_ms();
}

uint8_t opendisplay_pipe_connection(void) { return s_connection; }
uint16_t opendisplay_pipe_characteristic(void) { return s_attr; }
uint32_t opendisplay_pipe_connection_tag(void) { return s_connection_epoch; }
bool opendisplay_pipe_notify_enabled(void) { return s_notify; }

void opendisplay_pipe_set_tx_report_available(bool available)
{
  s_tx_report_ok = available;
  s_tx_report_warned = false;
}

static bool wait_tx_idle(uint32_t tag, uint32_t deadline_ms)
{
  if (!s_tx_report_ok) {
    /* Fail closed rather than fall back to stack acceptance. "Accepted by the stack" is not
     * "on air", and refreshing on it is exactly the D5 defect this barrier exists to close. */
    if (!s_tx_report_warned) {
      s_tx_report_warned = true;
      OD_DIAG("[OD] TX completion reporting unavailable; direct-write END refuses\r\n");
    }
    return false;
  }
  for (;;) {
    uint16_t flags = 0u;
    uint16_t packets = 0u;
    uint32_t bytes = 0u;
    sl_status_t sc;

    if (s_connection == 0xFFu || tag != s_connection_epoch) {
      return false;
    }
    sc = sl_bt_resource_get_connection_tx_status(s_connection, &flags, &packets, &bytes);
    if (sc != SL_STATUS_OK ||
        (flags & (SL_BT_RESOURCE_CONNECTION_TX_FLAGS_ERROR_PACKET_OVERFLOW |
                  SL_BT_RESOURCE_CONNECTION_TX_FLAGS_ERROR_CORRUPT)) != 0u) {
      OD_DIAG("[OD] TX completion report failed sc=0x%04lX flags=0x%04X packets=%u bytes=%lu\r\n",
             (unsigned long)sc, (unsigned)flags, (unsigned)packets, (unsigned long)bytes);
      return false;
    }
    if (packets == 0u) {
      return true;
    }
    if ((int32_t)(now_ms() - deadline_ms) >= 0) {
      OD_DIAG("[OD] TX completion timeout packets=%u bytes=%lu\r\n",
             (unsigned)packets, (unsigned long)bytes);
      return false;
    }
    sl_bt_run();
  }
}

void opendisplay_pipe_close_tag(uint32_t tag)
{
  if (s_connection != 0xFFu && tag == s_connection_epoch) {
    (void)sl_bt_connection_close(s_connection);
  }
}

bool opendisplay_pipe_flush_before_refresh(uint32_t tag, uint32_t deadline_ms)
{
  for (;;) {
    od_txq_status_t rc = od_txq_flush(now_ms(), deadline_ms);
    if (rc == OD_TXQ_OK) {
      break;
    }
    if (rc == OD_TXQ_TIMEOUT) {
      return false;
    }
    sl_bt_run();
  }
  return wait_tx_idle(tag, deadline_ms);
}

static void reset_transport_state(void)
{
  od_core_reset();
  od_cmd_silabs_reset();
  s_auth_abuse = 0u;
  s_hold_started_ms = 0u;
  s_read_started_ms = 0u;
  s_hold_active = false;
  s_read_active = false;
}

void opendisplay_pipe_abort_xfer_barrier(uint32_t tag)
{
  reset_transport_state();
  opendisplay_pipe_close_tag(tag);
}

void opendisplay_pipe_reset_transport(void)
{
  if (!od_xfer_frames_may_arrive()) {
    /* A transport reset is also the target's fail-safe panel power-off path. */
    opendisplay_display_abort();
  }
  reset_transport_state();
}

void opendisplay_pipe_set_characteristic(uint16_t pipe_value_handle)
{
  s_attr = pipe_value_handle;
  s_notify = false;
  opendisplay_pipe_reset_transport();
}

void opendisplay_pipe_on_connection_closed(void)
{
  s_notify = false;
  s_connection = 0xFFu;
  opendisplay_pipe_reset_transport();
}

/* The vendor stack retains the head event when this returns false. Dispatch is therefore called
 * only when it can reserve and when no config producer owns the shared blob. */
bool sl_bt_can_process_event(uint32_t len)
{
  bool held;
  (void)len;
  held = od_config_read_active() || od_sec2_tx_active() || od_txq_depth() != 0u;
  if (held && !s_hold_active) {
    s_hold_started_ms = now_ms();
    s_hold_active = true;
  } else if (!held) {
    s_hold_active = false;
  }
  return !held;
}

void od_core_frame_done(const od_reply_t *rp, od_frame_outcome_t outcome)
{
  od_frame_policy_t policy = od_frame_policy(outcome);

  if (rp == NULL || rp->origin != OD_ORIGIN_BLE ||
      rp->tag != s_connection_epoch || s_connection == 0xFFu) {
    return;
  }
  if (!policy.consume_rx) {
    /* can_process_event() should make this unreachable; swallowing it would lose a command. */
    OD_DIAG("[OD] invariant: shared dispatch deferred an admitted BGAPI frame\r\n");
    return;
  }
  if (policy.reset_abuse) {
    s_auth_abuse = 0u;
  }
  if (policy.increment_abuse && s_auth_abuse < 0xFFu) {
    s_auth_abuse++;
    if (s_auth_abuse >= OD_AUTH_ABUSE_LIMIT) {
      OD_DIAG("[OD] auth-abuse limit: closing connection epoch=%lu\r\n",
             (unsigned long)s_connection_epoch);
      (void)sl_bt_connection_close(s_connection);
    }
  }
}

static void dispatch_write(const uint8_t *data, uint16_t len)
{
  od_reply_t rp;
  od_frame_outcome_t outcome;

  if (data == NULL || len < 2u || s_connection == 0xFFu) {
    return;
  }
  rp.origin = OD_ORIGIN_BLE;
  rp.tag = s_connection_epoch;
  outcome = od_dispatch_frame(&rp, od_span_make(data, len));
  od_core_frame_done(&rp, outcome);
  (void)od_txq_process();
}

void opendisplay_pipe_handle_gatt_event(sl_bt_msg_t *evt)
{
  switch (SL_BT_MSG_ID(evt->header)) {
  case sl_bt_evt_connection_opened_id:
    s_connection = evt->data.evt_connection_opened.connection;
    s_connection_epoch++;
    if (s_connection_epoch == 0u) s_connection_epoch = 1u;
    s_notify = false;
    s_auth_abuse = 0u;
    break;
  case sl_bt_evt_gatt_mtu_exchanged_id:
    /* Diagnostic only. The stack enforces the bound on every send; recording the negotiated value
     * is what lets a field log distinguish "peer negotiated small" from "device refused". */
    if (evt->data.evt_gatt_mtu_exchanged.connection == s_connection) {
      OD_DIAG("[OD] ATT MTU negotiated=%u (value max=%u)\r\n",
             (unsigned)evt->data.evt_gatt_mtu_exchanged.mtu,
             (unsigned)(evt->data.evt_gatt_mtu_exchanged.mtu - 3u));
    }
    break;
  case sl_bt_evt_connection_closed_id:
    opendisplay_pipe_on_connection_closed();
    break;
  case sl_bt_evt_gatt_server_attribute_value_id: {
    sl_bt_evt_gatt_server_attribute_value_t *e = &evt->data.evt_gatt_server_attribute_value;
    if (e->attribute == s_attr && e->offset == 0u &&
        (e->att_opcode == (uint8_t)sl_bt_gatt_write_request ||
         e->att_opcode == (uint8_t)sl_bt_gatt_write_command)) {
      dispatch_write(e->value.data, e->value.len);
    }
    break;
  }
  case sl_bt_evt_gatt_server_characteristic_status_id: {
    sl_bt_evt_gatt_server_characteristic_status_t *e =
        &evt->data.evt_gatt_server_characteristic_status;
    if (e->characteristic == s_attr &&
        e->status_flags == (uint8_t)sl_bt_gatt_server_client_config) {
      s_notify = (e->client_config_flags & (uint16_t)sl_bt_gatt_server_notification) != 0u;
      OD_DIAG("[OD] pipe notifications %s\r\n", s_notify ? "on" : "off");
    }
    break;
  }
  default:
    break;
  }
}

void opendisplay_pipe_process(void)
{
  const uint32_t now = now_ms();

  (void)od_txq_process();
  (void)od_config_read_pump();
  od_sec2_pump();
  (void)od_txq_process();
  if (od_config_read_active()) {
    if (!s_read_active) {
      s_read_active = true;
      s_read_started_ms = now;
    } else if ((uint32_t)(now - s_read_started_ms) >= OD_CONFIG_READ_MAX_MS) {
      OD_DIAG("[OD] config-read producer deadline expired epoch=%lu\r\n",
             (unsigned long)s_connection_epoch);
      opendisplay_pipe_reset_transport();
      return;
    }
  } else {
    s_read_active = false;
  }
  if (!od_config_read_active() && od_txq_depth() == 0u) {
    s_hold_active = false;
    return;
  }
  if (!s_hold_active) {
    s_hold_started_ms = now;
    s_hold_active = true;
  } else if ((uint32_t)(now - s_hold_started_ms) >= OD_EVENT_HOLD_MAX_MS) {
    OD_DIAG("[OD] command transport hold expired epoch=%lu depth=%u; resetting\r\n",
           (unsigned long)s_connection_epoch, (unsigned)od_txq_depth());
    opendisplay_pipe_reset_transport();
  }
}
