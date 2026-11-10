/* opendisplay_pipe.c -- BLE transport and the main-thread pump. NOT a command subsystem.
 *
 * What is left here is what only this file can own: the GATT write callback, the connection
 * generation that is this target's frame identity, the deferred-close flag, and the bounded pump
 * that drives shared dispatch. The commands moved out to od_cmd_{device,config,direct,nfc}.c and
 * the shared transfer machine; the session logging seam is od_session_app.c.
 */

#include "od_spi_raw_app.h"
#include "opendisplay_pipe.h"

#include "od_cmd_config.h"
#include "od_config_read.h"
#include "od_core.h"
#include "od_dispatch.h"
#include "od_log.h"
#include "od_rxq.h"
#include "od_session.h"
#include "od_session_app.h"
#include "od_span.h"
#include "opendisplay_protocol.h"

#include <zephyr/kernel.h>

/* Bumped on every close. A frame carries the generation that produced it, so both queues can
 * discard work belonging to a connection that has gone. */
static atomic_t s_conn_gen;
/* Raised by the BT RX thread on disconnect; the cleanup it triggers runs on main, because tearing
 * down the display and the session from a stack callback is what starved ATT in the first place. */
static atomic_t s_close_pending;

uint32_t od_pipe_conn_gen(void)
{
  return (uint32_t)atomic_get(&s_conn_gen);
}

/*
 * GATT writes arrive on the BT RX thread. Commands (EPD init/refresh waits,
 * CCM crypto) must not run there: they starve ATT and break service discovery
 * on reconnect. Writes are queued here and drained on the main thread.
 */

/* BT RX thread: copy into the queue only, no command processing here. */
void opendisplay_pipe_on_write(const uint8_t *data, uint16_t len, bool write_cmd)
{
  /* write_cmd is not carried into the ring: on_pipe_write() has always (void)'d it, so a per-frame
   * field for it would be storage for a value nothing reads. Write-command vs write-request is a
   * transport distinction the protocol does not act on. */
  (void)write_cmd;
  /* The connection generation IS the frame's identity, and it travels with the frame rather than
   * being re-read at dispatch: a frame queued by a closed connection must not run against whoever
   * inherited the link. Same role as ESP32's packed owner word. */
  (void)od_rxq_push(data, len, (uint32_t)atomic_get(&s_conn_gen));
}

void opendisplay_pipe_on_notify_changed(bool enabled)
{
  od_log_info("pipe notifications %s", enabled ? "on" : "off");
}

/* BT RX thread: mark only; cleanup (EPD abort etc.) runs on the main thread. */
void opendisplay_pipe_on_connection_closed(void)
{
  atomic_inc(&s_conn_gen);
  atomic_set(&s_close_pending, 1);
}

/* Main thread: run deferred cleanup and process queued writes. */
static bool rx_tag_is_live(uint32_t tag, void *context)
{
  (void)context;
  return tag == (uint32_t)atomic_get(&s_conn_gen);
}

/* IMPLEMENTED FOR shared/core/od_cmd.h. Applies od_frame_policy() with this target's scoping.
 *
 * Nordic has NO exclusive-link idle clock and NO auth-abuse drop -- those are ESP32's
 * CONNECTION_POLICY mechanisms and there is nothing here to stamp or to count. What IS applicable
 * is the session's own activity clock: od_session_open() touches it on the encrypted path, but an
 * accepted PLAINTEXT command (security off, or FIRMWARE_VERSION) never reaches od_session_open, so
 * without this the clock would stop for exactly the traffic keeping the link busy. */
void od_core_frame_done(const od_reply_t *rp, od_frame_outcome_t outcome)
{
  const od_frame_policy_t p = od_frame_policy(outcome);

  if (rp == NULL || !p.stamp_activity) {
    return;
  }
  od_session_touch(od_session_app_state(), od_session_app_now_ms());
}

/* The main-thread pump. The ORDER is the design, and each step is here because leaving it out is
 * silent rather than loud:
 *
 *   1. deferred disconnect cleanup      -- before anything reads session or transfer state
 *   2. TX progress                      -- free capacity BEFORE dispatch needs to reserve it
 *   3. config-read producer             -- one chunk per available slot; without it a read stops
 *                                          after chunk 0 and stays active forever, deferring every
 *                                          later config write behind it
 *   4. stale discard + identity recheck -- a frame must not run on an identity that has gone
 *   5. shared dispatch                  -- validate, gate, decrypt, handler
 *   6. outcome policy
 *   7. RX consumption, EXCEPT DEFERRED  -- which stays at the head, byte-identical
 *   8. TX progress again                -- so a reply queued by this pass leaves in this pass
 *
 * 2 before 5 is the one that is not obvious: dispatch reserves capacity before it decrypts, so
 * draining first is what stops a frame being decrypted -- advancing the replay window -- and only
 * then discovering there is no room to answer it. A frame deferred after decrypt is a REPLAY when
 * it is re-offered, and the window refuses it the second time. */
void opendisplay_pipe_process(void)
{
  uint8_t drained;

  if (atomic_cas(&s_close_pending, 1, 0)) {
    od_core_reset();
    od_cmd_config_reset();
#if OD_CAP_SPI_RAW
    /* A raw session is not a transfer, so od_core_reset() does not end it. The protocol says a
     * disconnect force-closes it. */
    opendisplay_display_spi_raw_close();
#endif
  }

  /* BOUNDED, and the bound is not defensive tidiness. A central issuing write-without-response can
   * refill the SPSC ring as fast as this drains it, and an unbounded loop then never returns to the
   * LED, buzzer, input and watchdog work the main thread also owns -- a peer could hold the thread
   * indefinitely with valid traffic. One ring's worth per pass is enough for a full PIPE window
   * plus its END, which is the deepest legitimate burst; anything beyond that is the next pass's. */
  for (drained = 0u; drained < OD_RXQ_SLOTS; ++drained) {
    od_rxq_item_t *item;
    od_reply_t rp;
    od_frame_outcome_t outcome;

    (void)od_txq_process();
    (void)od_config_read_pump();

    (void)od_rxq_discard_stale(rx_tag_is_live, NULL);
    item = od_rxq_peek();
    if (item == NULL) {
      break;
    }
    /* Close can advance the generation after the shared helper accepted this head. Recheck at the
     * handler boundary; the next iteration consumes it under the new generation. */
    if (!rx_tag_is_live(item->tag, NULL)) {
      /* Consumed here rather than left for the next iteration's discard: `continue` alone would
       * re-peek the same head, and if the generation moved again between the two calls the loop
       * would spin without progress. The frame is dead either way. */
      od_rxq_consume();
      continue;
    }

    /* The tag is the frame's OWN generation, from its slot rather than a re-read of s_conn_gen: a
     * frame must be answered on the identity that sent it, not on whatever the link has become. */
    rp.origin = OD_ORIGIN_BLE;
    rp.tag = item->tag;
    outcome = od_dispatch_frame(&rp, od_span_make(item->data, item->len));
    od_core_frame_done(&rp, outcome);
    /* The unknown-opcode line, kept where both the bytes and the verdict are visible. The opcode
     * map is shared now and shared/ cannot log; this is the one place that has the frame and the
     * dispatcher's conclusion at the same time. */
    if (outcome == OD_FRAME_UNKNOWN_OPCODE && item->len >= 2u) {
      od_log_info("unknown cmd 0x%04X",
                  (unsigned)(((uint16_t)item->data[0] << 8) | item->data[1]));
    }

    if (!od_frame_policy(outcome).consume_rx) {
      /* DEFERRED: not consumed, re-offered unchanged. Stop the drain too -- the head has not moved,
       * so another pass this tick would reach the same answer. What cleared it is TX capacity or a
       * finishing config read, and both advance at the top of the next pass. */
      break;
    }
    od_rxq_consume();
  }

  (void)od_txq_process();
}
