/* Driver knobs beneath the production BG22 command hooks. Wire answers remain in od_cmd_silabs.c. */

#ifndef OD_TEST_FAKE_SILABS_H
#define OD_TEST_FAKE_SILABS_H

#include <stdbool.h>
#include <stdint.h>

struct od_config;

void fake_silabs_reset(void);

extern bool fake_silabs_store_ok;
extern unsigned fake_silabs_store_attempts;
extern unsigned fake_silabs_store_saves;
extern unsigned fake_silabs_store_reloads;
extern unsigned fake_silabs_store_clears;
extern bool fake_silabs_save_saw_queue_empty;
extern bool fake_silabs_reload_saw_queued;
extern bool fake_silabs_reload_saw_authenticated;
extern uint8_t fake_silabs_store_blob[2048];
extern uint32_t fake_silabs_store_len;
/* What opendisplay_get_global_config() returns. Zeroed (not loaded) by fake_silabs_reset(). */
extern struct od_config fake_silabs_global_config;
extern bool fake_silabs_xfer_active;
extern bool fake_silabs_refresh_ok;
extern unsigned fake_silabs_refreshes;
extern unsigned fake_silabs_aborts;
extern unsigned fake_silabs_resets;
extern bool fake_silabs_nfc_read_ok;
extern uint16_t fake_silabs_nfc_read_len;
/* What the handler forwarded to the tag, and whether the tag accepts it. Driver inputs and
 * observations, never expected wire bytes. */
extern bool     fake_silabs_nfc_write_ok;
extern unsigned fake_silabs_nfc_write_calls;
extern uint8_t  fake_silabs_nfc_write_rec_type;
extern uint16_t fake_silabs_nfc_write_len;
/* THE BYTES, not only their count -- see fake_nordic.h for why a length alone is not enough. */
#define FAKE_SILABS_NFC_WRITE_MAX 512u
extern uint8_t  fake_silabs_nfc_write_data[FAKE_SILABS_NFC_WRITE_MAX];

/* BGAPI and clock script used only by the production transport fault suite. */
#define FAKE_SILABS_SENT_MAX 32u
#define FAKE_SILABS_RESOURCE_MAX 16u
/* Every notification attempt, including NO_MORE_RESOURCE. Recording refused attempts is what lets
 * the suite prove a RETRY reuses identical sealed bytes instead of resealing with a new nonce. */
struct fake_silabs_sent_frame {
    uint8_t connection;
    uint16_t characteristic;
    uint16_t len;
    uint8_t data[253];
};
struct fake_silabs_resource_result {
    uint32_t status;
    uint16_t flags;
    uint16_t packets;
    uint32_t bytes;
};
extern uint32_t fake_silabs_now_ms;
extern uint32_t fake_silabs_run_advance_ms;
extern unsigned fake_silabs_run_calls;
extern unsigned fake_silabs_close_calls;
extern uint8_t fake_silabs_closed_connection;
extern uint32_t fake_silabs_notify_status;
/* The connection's real ATT MTU, enforced by the fake exactly as the stack does. The application
 * keeps no copy of this -- proving it does not is the point of several cases. */
extern uint16_t fake_silabs_att_mtu;
extern struct fake_silabs_sent_frame fake_silabs_sent[FAKE_SILABS_SENT_MAX];
extern unsigned fake_silabs_sent_n;
/* Attempts that the stack ACCEPTED. sent_n counts offers, this counts deliveries; a test that
 * only checks the queue drained cannot tell a refusal from a successful send. */
extern unsigned fake_silabs_delivered_n;
extern struct fake_silabs_resource_result
    fake_silabs_resource_script[FAKE_SILABS_RESOURCE_MAX];
extern unsigned fake_silabs_resource_script_n;
extern unsigned fake_silabs_resource_calls;
extern void (*fake_silabs_run_hook)(void);

void fake_silabs_bgapi_reset(void);

#endif /* OD_TEST_FAKE_SILABS_H */
