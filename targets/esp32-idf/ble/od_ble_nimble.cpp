/* NimBLE C-API implementation of od_ble. See od_ble.h for the contract, the layering split
 * against src/ble_transport_esp32.cpp, and the GATT layout -- which is a wire contract and
 * must not drift.
 *
 * There is no application state in this file. It knows about connections, not about sessions,
 * owners or transfers; everything it learns it reports through the od_ble_evt_* hooks and
 * then forgets. The one piece of bookkeeping it does keep, s_conn_count, exists only because
 * NimBLE's C API has no equivalent of NimBLEServer::getConnectedCount().
 */

#include "../src/od_smp.h"
#include "od_ble.h"

#include <string.h>

/* OD_BLE_MAX_FRAME -- the complete ATT MTU; writes carry three fewer value bytes. */
#include "opendisplay_protocol.h"

/* The shared advertising HAL this file implements at the bottom, and OD_ADV_MSD_LEN. Both are
 * plain-C shared headers; nothing from shared/ is called from here yet. */
#include "od_hal_adv.h"
/* od_hal_uptime_ms(), for the advertising stall report below. */
#include "od_hal_time.h"
#include "od_adv_control.h"
#include "od_adv_app.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "nvs_flash.h"

#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "store/config/ble_store_config.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "od_ble";

/* od_ble.h spells the sentinel without including a NimBLE header. Keep them the same value
 * rather than trusting a comment to stay true. */
static_assert(OD_BLE_CONN_NONE == BLE_HS_CONN_HANDLE_NONE,
              "OD_BLE_CONN_NONE must equal BLE_HS_CONN_HANDLE_NONE");

/* 00002446-0000-1000-8000-00805F9B34FB, little-endian as NimBLE wants it. Service and
 * characteristic deliberately share this UUID -- see od_ble.h.
 *
 * Spelled ONCE, and checked against the canonical big-endian form at compile time.
 *
 * This was wrong in the first port: byte 3 read 0x9F instead of 0x5F, so the firmware
 * registered 00002446-0000-1000-8000-0080_9F_9B34FB. Nothing could detect it. The service
 * registered, reported a valid handle, advertised, and accepted connections -- every log line
 * looked healthy -- and clients simply got NotFoundError on getPrimaryService() because that
 * service genuinely was not present. A hand-transcribed byte-reversed constant has no
 * redundancy, so the assert below supplies it: the LE array reversed must equal the Bluetooth
 * Base UUID with 0x2446 in the 16-bit slot. */
/* The Bluetooth Base UUID 00000000-0000-1000-8000-00805F9B34FB, little-endian, with the
 * 16-bit slot left to the caller. This is a fixed, well-known constant -- it is the same in
 * every Bluetooth product ever shipped -- so it is written once and never edited.
 *
 * Splicing rather than transcribing is the point. The ONLY project-specific number below is
 * 0x2446, which is the number a reader can check against the wire contract; the byte-reversal
 * is done by the compiler, not by me. The reference firmware gets the same property for free
 * by passing a string to NimBLE-Arduino's parser (and NimBLE's own C API offers
 * ble_uuid_from_str() for that, at runtime cost). */
#define OD_BT_BASE_UUID_LE_TAIL  0xFB, 0x34, 0x9B, 0x5F, 0x80, 0x00, 0x00, 0x80, \
                                 0x00, 0x10, 0x00, 0x00
#define OD_UUID128_FROM_16(u16)  OD_BT_BASE_UUID_LE_TAIL,                 \
                                 (uint8_t)((u16) & 0xFFu),                \
                                 (uint8_t)(((u16) >> 8) & 0xFFu),         \
                                 0x00, 0x00

/* The OpenDisplay GATT service/characteristic, 0x2446. NOT currently a named constant in
 * shared/protocol/opendisplay_protocol.h -- the header carries OD_LAN_TCP_PORT 2446u, which is
 * the same number for an unrelated reason (the TCP port), not this UUID. The BLE service UUID
 * is wire contract and belongs in the canonical header; adding it is blocked on the header
 * freeze, so it is flagged here rather than duplicated silently. */
#define OD_BLE_SERVICE_UUID16  0x2446u

#define OD_UUID_LE_BYTES  OD_UUID128_FROM_16(OD_BLE_SERVICE_UUID16)

static constexpr uint8_t od_uuid_le[16] = { OD_UUID_LE_BYTES };

/* 00002446-0000-1000-8000-00805F9B34FB, MSB first -- read straight off the wire contract. */
static constexpr uint8_t od_uuid_be[16] = {
    0x00, 0x00, 0x24, 0x46,  0x00, 0x00,  0x10, 0x00,
    0x80, 0x00,  0x00, 0x80, 0x5F, 0x9B, 0x34, 0xFB
};

static constexpr bool od_uuid_le_is_be_reversed()
{
    for (int i = 0; i < 16; i++) {
        if (od_uuid_le[i] != od_uuid_be[15 - i]) {
            return false;
        }
    }
    return true;
}
static_assert(od_uuid_le_is_be_reversed(),
              "the little-endian GATT UUID does not reverse to "
              "00002446-0000-1000-8000-00805F9B34FB -- clients will not find the service");

static const ble_uuid128_t od_svc_uuid = BLE_UUID128_INIT(OD_UUID_LE_BYTES);
static const ble_uuid128_t od_chr_uuid = BLE_UUID128_INIT(OD_UUID_LE_BYTES);

static uint16_t s_chr_val_handle = 0;
static uint16_t s_preferred_mtu  = 0;
static bool     s_inited         = false;

/* ------------------------------------------------------- the identity handoff (F4, part 1) ---
 *
 * s_own_addr_type and s_addr_resolved USED TO BE two plain file statics: written by
 * od_on_sync() on the NimBLE HOST task, read by od_ble_is_ready(), od_ble_get_identity_addr()
 * and the init spin on the LOOP task. That is a cross-context handoff of a flag and the
 * payload it guards, with no ordering between them -- correctness review F4.
 *
 * WHAT WAS ACTUALLY AT RISK, stated precisely rather than inflated. On the S3 baselines both
 * tasks are pinned to core 0 (CONFIG_BT_NIMBLE_PINNED_TO_CORE=0,
 * CONFIG_ESP_MAIN_TASK_AFFINITY=0x0), and a context switch on one core is a full barrier -- so
 * the reader could not in practice observe the flag without the payload. The defect is that
 * NOTHING SAYS SO: it is undefined behaviour that happens to work, it depends on an affinity
 * setting recorded nowhere near this code, and it breaks silently the day either task moves.
 *
 * The fix publishes both as ONE record behind a seqlock. The sequence is odd while a write is
 * in progress and even-and-non-zero once a coherent snapshot exists, so a reader either gets a
 * whole snapshot or knows to retry. Writer is single (the host task), which is what makes the
 * cheap seqlock sound here.
 *
 * The counter doubles as the STACK GENERATION: it advances on every sync, so a later step can
 * discard facts stamped with a generation the stack has since left behind. */
struct od_ble_identity {
    uint8_t addr_type;      /* BLE_OWN_ADDR_*, as ble_gap_adv_start() consumes it */
};
static struct od_ble_identity s_identity;
static volatile uint32_t      s_identity_seq = 0;

/* HOST TASK ONLY. */
static void od_ble_publish_identity(uint8_t addr_type)
{
    const uint32_t seq = __atomic_load_n(&s_identity_seq, __ATOMIC_RELAXED);
    __atomic_store_n(&s_identity_seq, seq + 1u, __ATOMIC_RELEASE);   /* odd: writing */
    s_identity.addr_type = addr_type;
    __atomic_store_n(&s_identity_seq, seq + 2u, __ATOMIC_RELEASE);   /* even: published */
}

/* Any task. False when no coherent snapshot exists yet. */
static bool od_ble_read_identity(struct od_ble_identity *out)
{
    for (unsigned attempt = 0; attempt < 4u; ++attempt) {
        const uint32_t s1 = __atomic_load_n(&s_identity_seq, __ATOMIC_ACQUIRE);
        if (s1 == 0u || (s1 & 1u) != 0u) {
            continue;   /* never published, or a write is in flight */
        }
        const struct od_ble_identity snap = s_identity;
        const uint32_t s2 = __atomic_load_n(&s_identity_seq, __ATOMIC_ACQUIRE);
        if (s1 == s2) {
            if (out) {
                *out = snap;
            }
            return true;
        }
    }
    /* A single writer cannot starve a reader indefinitely; four attempts is generous. Failing
     * closed reports "no identity yet", which every caller already handles. */
    return false;
}

static bool od_ble_identity_valid(void)
{
    return od_ble_read_identity(NULL);
}

/* The characteristic's stored value: the last frame written to it. Serves both the write path
 * (as its flatten scratch) and the READ path, which hands it back the way NimBLE-Arduino's
 * NimBLEAttValue did. One buffer, because NimBLE serialises host callbacks so a read can never
 * overlap a write. */
static uint8_t  s_chr_value[OD_BLE_MAX_FRAME];
static uint16_t s_chr_value_len  = 0;

/* The stack's peer count. Maintained here because NimBLE's C API offers no accessor for it;
 * written only from GAP events (host task), read from the loop task, hence atomic. It is a
 * COUNT, never a test for whether one particular link is up -- the transport's instance table
 * answers that per handle. */
static volatile uint8_t s_conn_count = 0;

static char s_name[32] = "OpenDisplay";

/* ---------------------------------------------- advertising ownership (F4, part 2) ---
 *
 * ADVERTISING POLICY NOW LIVES ON THE LOOP TASK, in the shared controller. This file no
 * longer decides when to advertise; it supplies the HAL at the bottom and publishes facts
 * from the stack. s_adv_wanted, s_msd and s_msd_len are GONE -- their jobs are
 * od_adv_control's `desired` and `msd`, which are owned by exactly one context.
 *
 * What that removes: the old od_ble_advertise() was re-entered from BLE_GAP_EVENT_CONNECT
 * (failed attempt), BLE_GAP_EVENT_ADV_COMPLETE and od_on_sync() -- all HOST task -- while the
 * loop task wrote the same flag and MSD buffer through the public API. Two writers, no
 * ordering, and a stack event able to restart advertising after the application had committed
 * to stop for deep sleep. That last one is the reason this is worth the churn.
 *
 * s_adv is touched ONLY from the loop task. Nothing below this line may call into it from a
 * callback -- that would recreate the defect with more code.
 */
static struct od_adv_control s_adv;

/* Stall reporting. Every way advertising can fail to start is a silent transient -- RETRY from a
 * missing identity or BLE_HS_ENOMEM, a start gated off, a connection that never cleared. Report
 * the condition (intent held without advertising) rather than the events, which would spam at
 * loop rate. Loop-task only, like everything else touching s_adv. */
#define OD_ADV_STALL_REPORT_MS 5000u
static uint32_t s_adv_wanted_since_ms = 0;   /* 0 = not currently stalled */
static uint32_t s_adv_stall_logged_ms = 0;

/* The one fact the stack must hand across: advertising is no longer running.
 *
 * Consume-once rather than a level, because "it ended" is an edge and the controller reacts by
 * reconciling. Coalescing is safe: two ends before one drain state the same thing. */
static volatile uint8_t s_adv_ended_pending = 0;

bool od_ble_service_advertising(bool start_allowed);

/* HOST TASK. The only advertising-related thing a callback may now do. */
static void od_ble_note_adv_ended(void)
{
    __atomic_store_n(&s_adv_ended_pending, (uint8_t)1, __ATOMIC_RELEASE);
}

/* ------------------------------------------------------------------ GATT access */

static int od_gatt_access(uint16_t conn_handle, uint16_t attr_handle,
                          struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)attr_handle; (void)arg;

    switch (ctxt->op) {
    case BLE_GATT_ACCESS_OP_WRITE_CHR: {
        /* om_len is the flat length of the first mbuf; a long write can span several, so
         * flatten. The old NimBLE-Arduino path used NimBLEAttValue, which did this for us --
         * and note the reason its comment gave for not converting to String: pipe-write
         * frames begin with 0x00, so any C-string conversion truncates to length 0. Same trap
         * here: this payload is binary, never a string. */
        /* Enforce the ATT VALUE ceiling at the GATT layer, returning ATT 0x0D. The 256-byte
         * OD_BLE_MAX_FRAME includes opcode(1) + handle(2), so it admits 253 value bytes. A storage
         * buffer sized to the whole MTU is not permission to accept a 256-byte value. */
        if (OS_MBUF_PKTLEN(ctxt->om) > (OD_BLE_MAX_FRAME - 3u)) {
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }
        /* File-static rather than a stack array: this runs on the NimBLE host task, whose
         * stack is sized by CONFIG_BT_NIMBLE_TASK_STACK_SIZE and has no room to spare for a
         * 256-byte frame buffer. Safe because NimBLE serialises host callbacks -- there is
         * never a second access in flight -- and because od_ble_evt_write() is contractually
         * required to copy before returning.
         *
         * It now doubles as the characteristic's stored value; see the READ case. */
        uint16_t len = 0;
        int rc = ble_hs_mbuf_to_flat(ctxt->om, s_chr_value, sizeof(s_chr_value), &len);
        if (rc != 0) {
            s_chr_value_len = 0;
            return BLE_ATT_ERR_INSUFFICIENT_RES;
        }
        /* Retain it, exactly as NimBLECharacteristic::writeEvent() does with setValue()
         * (NimBLE-Arduino/src/NimBLECharacteristic.cpp:334). See the READ case for why. */
        s_chr_value_len = len;
        /* The handle travels with the frame. Without it the transport cannot tell an owner's
         * write from a gatecrasher's, and the non-owner filter -- which must run HERE, before
         * the bytes reach the RX ring, because during a ~16 s refresh no loop-side decision
         * runs at all -- would have nothing to decide on. */
        od_ble_evt_write(conn_handle, s_chr_value, len);
        return 0;
    }
    case BLE_GATT_ACCESS_OP_READ_CHR: {
        /* RETURN THE LAST WRITTEN VALUE, restoring NimBLE-Arduino behaviour (2026-08-05).
         *
         * This case used to return an empty value, claiming it "matches what the Arduino
         * characteristic did with no value set". THAT PREMISE WAS WRONG: a value was always
         * set, by every write. NimBLECharacteristic::writeEvent() calls setValue() on each
         * write (NimBLECharacteristic.cpp:334) and the READ path appends the stored value with
         * os_mbuf_append() (NimBLEServer.cpp:743). So under the shipped firmware, reading the
         * characteristic returned the last command written to it; under this port it returned
         * zero bytes.
         *
         * The primary data path is notify-based and unaffected either way -- this matters to
         * diagnostic and third-party clients that read rather than subscribe.
         *
         * CONSEQUENCE WORTH KNOWING: the last frame written is readable by any connected
         * client, with no authentication. That is the shipped behaviour being restored, not a
         * new exposure, and with app-layer encryption enabled the retained bytes are AES-CCM
         * ciphertext. With encryption disabled they are the plaintext command. If that is not
         * wanted, the fix is to stop retaining rather than to keep diverging silently. */
        if (s_chr_value_len == 0) {
            return 0;
        }
        int rc = os_mbuf_append(ctxt->om, s_chr_value, s_chr_value_len);
        return (rc == 0) ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    default:
        return BLE_ATT_ERR_UNLIKELY;
    }
}

static uint16_t s_smp_val_handle;
static uint8_t s_smp_rx[OD_BLE_MAX_FRAME];

/* mcumgr SMP over BLE. These UUIDs are fixed by the mcumgr transport. */
static const ble_uuid128_t od_smp_svc_uuid = BLE_UUID128_INIT(
    0x84, 0xAA, 0x60, 0x74, 0x52, 0x8A, 0x8B, 0x86, 0xD3, 0x4C, 0xB7, 0x1D, 0x1D, 0xDC, 0x53, 0x8D);
static const ble_uuid128_t od_smp_chr_uuid = BLE_UUID128_INIT(
    0x48, 0x7C, 0x99, 0x74, 0x11, 0x26, 0x9E, 0xAE, 0x01, 0x4E, 0xCE, 0xFB, 0x28, 0x78, 0x2E, 0xDA);

static int od_smp_access(uint16_t conn_handle, uint16_t attr_handle,
                         struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    uint16_t len = 0;

    (void)attr_handle; (void)arg;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        return 0;
    }
    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return BLE_ATT_ERR_UNLIKELY;
    }
    if (ble_hs_mbuf_to_flat(ctxt->om, s_smp_rx, sizeof(s_smp_rx), &len) != 0) {
        return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
    }
    od_smp_rx(conn_handle, s_smp_rx, len);
    return 0;
}

static const struct ble_gatt_chr_def od_smp_chrs[] = {
    {
        .uuid       = &od_smp_chr_uuid.u,
        .access_cb  = od_smp_access,
        .arg        = NULL,
        .descriptors = NULL,
        .flags      = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_NOTIFY,
        .min_key_size = 0,
        .val_handle = &s_smp_val_handle,
    },
    { 0 }
};

static const struct ble_gatt_chr_def od_chrs[] = {
    {
        .uuid       = &od_chr_uuid.u,
        .access_cb  = od_gatt_access,
        .arg        = NULL,
        .descriptors = NULL,
        /* READ | WRITE | WRITE_NO_RSP | NOTIFY -- the exact property set the Arduino code
         * declared. NimBLE creates the 0x2902 CCCD automatically for NOTIFY, which is why
         * there is no explicit descriptor here (the old code relied on the same behaviour). */
        .flags      = BLE_GATT_CHR_F_READ | BLE_GATT_CHR_F_WRITE |
                      BLE_GATT_CHR_F_WRITE_NO_RSP | BLE_GATT_CHR_F_NOTIFY,
        .min_key_size = 0,
        .val_handle = &s_chr_val_handle,
    },
    { 0 }
};

static const struct ble_gatt_svc_def od_svcs[] = {
    {
        .type            = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid            = &od_svc_uuid.u,
        .includes        = NULL,
        .characteristics = od_chrs,
    },
    {
        .type            = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid            = &od_smp_svc_uuid.u,
        .includes        = NULL,
        .characteristics = od_smp_chrs,
    },
    { 0 }
};

/* ------------------------------------------------------------------ GAP events */

static int od_gap_event(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        /* OD-INSTRUMENTATION 2026-08-05 -- TEMPORARY, remove once the wake-connect defect is
         * understood. Deliberately ESP_LOGW and not od_log: od_log drops records emitted from
         * non-loop tasks under load (od_log.cpp's s_dropped path), and this callback runs on
         * the NimBLE host task during a busy wake -- so an od_log line here could go missing
         * and take the evidence with it. ESP_LOG writes straight to the port.
         *
         * WHAT THIS ANSWERS. On the first connection after a deep-sleep wake, the app's
         * connect hook demonstrably does not run: od_ble_evt_connect() allocates an epoch as
         * its first statement, for admitted and refused instances alike, yet the NEXT
         * connection logged e=1 -- the first epoch of the boot. The same connection produced
         * MTU and DISCONNECT events through THIS switch, so the callback was live. Those two
         * facts do not sit together, and this line separates them: if it prints, the event
         * arrived and the CONNECT case ran; if it does not, the event never reached us. */
        ESP_LOGW(TAG, "[instr] GAP CONNECT h=%u status=%d",
                 (unsigned)event->connect.conn_handle, event->connect.status);
        if (event->connect.status == 0) {
            /* A connection consumes the advertisement. Publishing the fact keeps the
             * controller's view truthful; without it, it would still self-correct on the next
             * pass (its stop returns ALREADY, which is success) but would issue a pointless
             * stack call to get there. */
            od_ble_note_adv_ended();
            if (s_conn_count < 0xFF) {
                __atomic_fetch_add(&s_conn_count, (uint8_t)1, __ATOMIC_RELEASE);
            }
            if (s_preferred_mtu) {
                ble_att_set_preferred_mtu(s_preferred_mtu);
            }
            od_ble_evt_connect(event->connect.conn_handle);
        } else {
            /* A NON-ZERO STATUS DOES NOT MEAN "NO LINK". This branch used to assume it did --
             * skip the hook, resume advertising -- and that assumption cost every first
             * connection after a wake.
             *
             * OBSERVED ON HARDWARE 2026-08-05: NimBLE delivers CONNECT with status 26,
             * BLE_HS_EENCRYPT_KEY_SZ ("invalid encryption key size"), for a link that IS
             * established. The proof is in the same log: the very next call to
             * ble_gap_adv_start() returns BLE_HS_ENOMEM because the single connection slot is
             * IN USE (CONFIG_BT_NIMBLE_MAX_CONNECTIONS=1), and the link then completes an MTU
             * exchange, carries a GATT write and produces a real DISCONNECT event. The
             * application, never having been told, dropped that write as non-owner -- so the
             * client saw a GATT server it could not use and gave up after ~8 s.
             *
             * ASK THE STACK INSTEAD OF INFERRING. ble_gap_conn_find() is authoritative: if a
             * descriptor exists, the link is real and must be adopted exactly as a status-0
             * connect would be. Only when it does not exist is this genuinely a failed attempt
             * with nothing to own, which is the case the advertise-resume was written for. */
            struct ble_gap_conn_desc desc;
            const bool link_exists =
                (ble_gap_conn_find(event->connect.conn_handle, &desc) == 0);
            ESP_LOGW(TAG, "[instr] GAP CONNECT status=%d link_exists=%d",
                     event->connect.status, (int)link_exists);
            if (link_exists) {
                od_ble_note_adv_ended();   /* the link consumed it -- see the status-0 case */
                if (s_conn_count < 0xFF) {
                    __atomic_fetch_add(&s_conn_count, (uint8_t)1, __ATOMIC_RELEASE);
                }
                if (s_preferred_mtu) {
                    ble_att_set_preferred_mtu(s_preferred_mtu);
                }
                od_ble_evt_connect(event->connect.conn_handle);
            } else {
                /* Genuinely no link. Resuming advertising is stack housekeeping, not policy --
                 * without it the device goes quiet after a failed connect and only a reboot
                 * brings it back. */
                od_ble_note_adv_ended();
            }
        }
        return 0;

    case BLE_GAP_EVENT_DISCONNECT: {
        od_smp_link_lost();
        /* OD-INSTRUMENTATION: pairs with the CONNECT line above so handles can be matched.
         * A DISCONNECT for a handle that never produced a CONNECT line is the signature of
         * the defect. */
        ESP_LOGW(TAG, "[instr] GAP DISCONNECT h=%u reason=0x%04X",
                 (unsigned)event->disconnect.conn.conn_handle,
                 (unsigned)event->disconnect.reason);
        const uint8_t n = __atomic_load_n(&s_conn_count, __ATOMIC_ACQUIRE);
        if (n > 0) {
            __atomic_store_n(&s_conn_count, (uint8_t)(n - 1), __ATOMIC_RELEASE);
        }
        /* Full width, unmodified. NimBLE's reason spans two ranges and truncation aliases
         * them -- see od_ble_evt_disconnect()'s declaration. */
        od_ble_evt_disconnect(event->disconnect.conn.conn_handle,
                              (uint16_t)event->disconnect.reason);
        /* Advertising is NOT restarted here. BleTransport::restartsAdvertisingOnDisconnect()
         * reports false for this target precisely so the application can hold the restart off
         * while an EPD refresh is mid-flight; re-arming it from the host task would take that
         * decision away and do it on the wrong task. */
        return 0;
    }

    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_chr_val_handle) {
            od_ble_evt_subscribe(event->subscribe.conn_handle,
                                 event->subscribe.cur_notify != 0);
        }
        return 0;

    case BLE_GAP_EVENT_MTU:
        od_ble_evt_link_negotiated(event->mtu.conn_handle, "MTU exchange");
        return 0;

    case BLE_GAP_EVENT_PHY_UPDATE_COMPLETE:
        /* Fires whether or not the request was granted, and it is the only point at which the
         * granted PHY is knowable -- od_ble_request_fast_link() returns long before the
         * controller has finished. The hook re-reads both directions rather than trusting the
         * event's fields, so one log line describes the whole link. */
        od_ble_evt_link_negotiated(event->phy_updated.conn_handle, "PHY update");
        return 0;

    /* ------------------------------------------------- the rest of the wrapper's census
     *
     * NimBLE-Arduino's NimBLEServer::handleGapEvent covers 14 GAP events; phase B's C-API
     * rewrite covered 6, and the gap produced a live regression (see REPEAT_PAIRING below).
     * These five close the census. Each is the EQUIVALENT of what the wrapper did, translated
     * into this target's idiom -- the wrapper dispatched to NimBLEServerCallbacks virtuals
     * that this firmware has no counterpart for, so "equivalent" means the same information
     * reaches the same place, not the same shape.
     *
     * All are host-task context and therefore flag-or-log only, exactly like the six above. */

    case BLE_GAP_EVENT_CONN_UPDATE:
        /* Wrapper: looked the connection up and called onConnParamsUpdate(peerInfo).
         *
         * The connection interval changed -- the central re-negotiated it, typically dropping
         * from a fast transfer interval back to a slow idle one. This target already has the
         * right reporter for that: the same one MTU and PHY changes use, so all three
         * renegotiations produce one comparable line rather than three formats. That answers
         * "why did throughput fall off mid-transfer", which was previously unanswerable. */
        od_ble_evt_link_negotiated(event->conn_update.conn_handle, "connection update");
        return 0;

    case BLE_GAP_EVENT_NOTIFY_TX:
        /* Wrapper: found the characteristic and called onStatus(chr, status), after letting an
         * unacknowledged INDICATION pass silently (status 0 on an indication means "sent, ack
         * still outstanding" -- the real result arrives in a second event).
         *
         * This target has ONE characteristic, so the lookup collapses. What matters is the
         * status: a non-zero value means the notification did NOT go out, and this firmware
         * runs its own BLE TX queue that has been unable to see that. Only failures are
         * logged -- a line per successful notification would be one per frame of every
         * transfer, which is exactly the kind of logging that hides the failures. */
        if (event->notify_tx.indication && event->notify_tx.status == 0) {
            return 0;   /* indication sent, ack outstanding -- not a result yet */
        }
        if (event->notify_tx.status != 0) {
            ESP_LOGW(TAG, "notify TX failed: status=%d h=%u attr=%u",
                     event->notify_tx.status,
                     (unsigned)event->notify_tx.conn_handle,
                     (unsigned)event->notify_tx.attr_handle);
        }
        return 0;

    case BLE_GAP_EVENT_IDENTITY_RESOLVED: {
        /* Wrapper: resolved the peer and called onIdentity(peerInfo).
         *
         * The peer connected with a resolvable private address and the stack has now matched
         * it to a bonded identity. Diagnostics only here -- this firmware identifies clients
         * by its own link-owner epoch, not by BLE address -- but it is worth seeing, because
         * it is the event that says "this peer IS bonded to us", which is the state the
         * REPEAT_PAIRING path exists to repair. */
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(event->identity_resolved.conn_handle, &desc) == 0) {
            ESP_LOGI(TAG, "identity resolved h=%u type=%u",
                     (unsigned)event->identity_resolved.conn_handle,
                     (unsigned)desc.peer_id_addr.type);
        }
        return 0;
    }

    case BLE_GAP_EVENT_NOTIFY_RX:
        /* Wrapper: forwarded to NimBLEClient -- it supports the CENTRAL role and this event is
         * a notification arriving FROM a peer.
         *
         * This build is peripheral-only and never subscribes to anything, so it cannot occur.
         * Handled explicitly rather than left to fall through: an unhandled case returns 0 and
         * is indistinguishable from success, which is precisely how eight missing events went
         * unnoticed until one of them broke connections. If this ever fires, the assumption
         * behind it has changed and the log will say so. */
        ESP_LOGW(TAG, "NOTIFY_RX on a peripheral-only build (h=%u) -- unexpected",
                 (unsigned)event->notify_rx.conn_handle);
        return 0;

    case BLE_GAP_EVENT_SCAN_REQ_RCVD:
        /* Wrapper: routed to NimBLEAdvertising/NimBLEExtAdvertising, which for legacy
         * advertising does no work beyond its own bookkeeping.
         *
         * DELIBERATELY SILENT. A scan request arrives from every scanning phone in radio
         * range, several times a second; logging each one would bury the events that matter --
         * the opposite of what this census is for. The case exists so the event is accounted
         * for rather than falling through as an unknown. */
        return 0;

    /* ---------------------------------------------------------------- security
     *
     * THESE THREE WERE LOST IN THE PORT, and their absence is a REGRESSION against the
     * shipped Arduino firmware. NimBLE-Arduino's NimBLEServer::handleGapEvent handles 14 GAP
     * events; phase B's C-API rewrite covered 6. REPEAT_PAIRING is the one that reached the
     * user: with a stale bond on the client, the wrapper deleted it and re-paired, while this
     * port silently kept the mismatched keys and failed the same way on every attempt.
     * Restored 2026-08-05 after the connect-stutter investigation. */
    case BLE_GAP_EVENT_REPEAT_PAIRING: {
        /* The peer is bonded to us but the keys no longer agree -- typically because the
         * client kept a bond across a reflash, or the key sizes differ. Deleting our side and
         * retrying is what the Arduino wrapper did and what every NimBLE peripheral example
         * does; without it the condition is PERMANENT until a human forgets the device on the
         * client, because nothing on this side can clear it. */
        struct ble_gap_conn_desc desc;
        if (ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc) == 0) {
            ble_store_util_delete_peer(&desc.peer_id_addr);
            ESP_LOGW(TAG, "repeat pairing: stale bond deleted, retrying");
        }
        return BLE_GAP_REPEAT_PAIRING_RETRY;
    }

    case BLE_GAP_EVENT_ENC_CHANGE:
        /* Observation only -- the app layer does its own AES-CCM and does not depend on link
         * encryption (no characteristic carries an _ENC flag and CONFIG_BT_NIMBLE_SM_LVL=0).
         * Logged because without it an encryption failure is INVISIBLE, which is precisely
         * how the connect stutter stayed unexplained across three captures. */
        ESP_LOGW(TAG, "encryption change: status=%d h=%u",
                 event->enc_change.status, (unsigned)event->enc_change.conn_handle);
        return 0;

    case BLE_GAP_EVENT_PASSKEY_ACTION:
        /* Should not fire: no IO capability is declared, so pairing is "just works". Logged
         * rather than ignored so that if it ever DOES fire, it appears instead of the pairing
         * silently stalling -- the same failure mode REPEAT_PAIRING had. */
        ESP_LOGW(TAG, "passkey action requested (action=%u) -- unhandled, pairing will stall",
                 (unsigned)event->passkey.params.action);
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        od_ble_note_adv_ended();
        return 0;

    default:
        return 0;
    }
}

/* ------------------------------------------------------------------ advertising */

/* Pack the ADV and scan-response records from one MSD snapshot.
 *
 * EXTRACTED so there is exactly ONE implementation of the on-air layout. It is called both by
 * od_ble_advertise() below (the current path) and by od_hal_adv_program() at the end of this
 * file (the shared controller's path). Two packers would be free to drift, and the thing most
 * likely to drift is the byte layout every host discovers this device by.
 *
 * Returns false only when the record cannot be set at all. */
static bool od_ble_pack_adv_records(const uint8_t *msd, uint8_t msd_len)
{
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof fields);

    /* ADVERTISEMENT = flags + NAME + MSD. This is the Arduino payload byte-for-byte:
     * updatemsdata() built setName + setFlags(0x06) + setManufacturerData and pushed the whole
     * record with setAdvertisementData(). It fits 31 bytes EXACTLY --
     *   flags 3 + name (2 + 8, "OD" + 6 hex chars) + MSD (2 + 16) = 31
     * -- which is why the name length is not free to grow. A longer device name overflows and
     * ble_gap_adv_set_fields() will reject the record; the fallback below is what catches that.
     *
     * An earlier version of this port put the name in the SCAN RESPONSE instead. That is not
     * what the fleet advertises: a passive scanner (and any host that filters on name without
     * issuing a scan request) sees an unnamed device. */
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    fields.name = (uint8_t *)s_name;
    fields.name_len = (uint8_t)strlen(s_name);
    fields.name_is_complete = 1;
    if (msd_len) {
        fields.mfg_data = (uint8_t *)msd;
        fields.mfg_data_len = msd_len;
    }

    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        /* Only reachable with a device name longer than the stock 8 characters. Shorten the
         * name rather than drop the MSD: the MSD is what host discovery keys on
         * (manufacturer id 9286) and it carries the live sensor payload. */
        ESP_LOGW(TAG, "adv record too large (rc=%d, name=%u B); advertising without the name",
                 rc, (unsigned)fields.name_len);
        fields.name = NULL;
        fields.name_len = 0;
        fields.name_is_complete = 0;
        rc = ble_gap_adv_set_fields(&fields);
        if (rc != 0) {
            ESP_LOGE(TAG, "ble_gap_adv_set_fields failed: %d", rc);
            return false;
        }
    }

    /* SCAN RESPONSE = the 128-bit service UUID (18 B). The Arduino build had no scan response
     * at all and its advertisement had no room for the UUID either, so 0x2446 was not
     * discoverable by scanning on any shipped unit -- a client had to connect to find it.
     * Putting it here costs nothing on air (scan responses are only sent when requested) and
     * makes "filter by service UUID" work in a scanner. It is additive: no byte of the
     * advertisement above changes. */
    struct ble_hs_adv_fields rsp;
    memset(&rsp, 0, sizeof rsp);
    rsp.uuids128 = (ble_uuid128_t *)&od_svc_uuid;
    rsp.num_uuids128 = 1;
    rsp.uuids128_is_complete = 1;
    ble_gap_adv_rsp_set_fields(&rsp);
    return true;
}

/* Begin advertising with whatever records were last packed. Extracted alongside the packer,
 * and for the same reason: od_hal_adv_start() must issue the identical call. */
static int od_ble_start_now(void)
{
    struct ble_gap_adv_params adv;
    memset(&adv, 0, sizeof adv);
    adv.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv.disc_mode = BLE_GAP_DISC_MODE_GEN;

    struct od_ble_identity id;
    if (!od_ble_read_identity(&id)) {
        /* Unreachable from the callers below, which all gate on the identity first. Kept as a
         * hard floor rather than an assert: advertising from an unresolved address is a silent
         * on-air defect, not a crash. */
        return BLE_HS_EAGAIN;
    }
    int rc = ble_gap_adv_start(id.addr_type, NULL, BLE_HS_FOREVER, &adv, od_gap_event, NULL);
    /* OD-INSTRUMENTATION: stamp EVERY start, with a sequence number, so the log shows whether
     * od_gap_event was the live callback before the connection in question formed -- the
     * "event arrived before we were listening" hypothesis. rc=6 is BLE_HS_ENOMEM and is
     * EXPECTED while a client holds the single connection slot
     * (CONFIG_BT_NIMBLE_MAX_CONNECTIONS=1): connectable undirected advertising needs a free
     * connection object. Logged, not silenced, because which CALLER produced it is the open
     * question. */
    {
        static uint32_t s_adv_seq = 0;
        ESP_LOGW(TAG, "[instr] adv_start #%u rc=%d%s", (unsigned)++s_adv_seq, rc,
                 rc == BLE_HS_ENOMEM ? " (ENOMEM: connection slot in use -- expected)" : "");
    }
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "ble_gap_adv_start failed: %d", rc);
    }
    return rc;
}


/* The shared boost seam (shared/core/od_adv_app.h).
 *
 * A NO-OP, AND THAT IS THE CORRECT ANSWER HERE, not an unimplemented one. od_ble_start_now()
 * leaves ble_gap_adv_params::itvl_min/itvl_max at zero, so NimBLE substitutes its own defaults
 * for connectable undirected advertising -- BLE_GAP_ADV_FAST_INTERVAL1_MIN/MAX, which are 30 ms
 * and 60 ms because CONFIG_BT_NIMBLE_HIGH_DUTY_ADV_ITVL is unset in every board baseline. This
 * THE COMPARISON THAT MATTERS IS AGAINST THE OTHER TARGETS' *IDLE* RATE, not their boosted one.
 * nordic-zephyr idles at 1000 ms and drops to 20-30 ms for three seconds after an event; the
 * Bluefruit nRF build in ../Firmware idles at 160-1000 ms. This target's permanent 30-60 ms is
 * within a factor of two of their BOOSTED rate and 17-33x their idle rate, so a ~230 ms button
 * press or touch contact already yields several advertisements here.
 *
 * The boost exists to rescue a target that idles slow. There is no slow state here to rescue. If
 * this target ever grows an advertising-interval policy, this is where the boost attaches.
 */
extern "C" void od_adv_app_boost(void)
{
}

/* ------------------------------------------------------------------ host lifecycle */

static void od_on_sync(void)
{
    ble_hs_util_ensure_addr(0);
    uint8_t own_addr_type = 0;
    if (ble_hs_id_infer_auto(0, &own_addr_type) != 0) {
        ESP_LOGE(TAG, "no usable BLE address");
        return;
    }
    /* Published to the LAN mDNS TXT record via od_ble_get_identity_addr(); a host correlating
     * BLE with LAN matches on it, so log which kind it is. */
    /* Publish the whole record, THEN it becomes visible -- see the seqlock above. */
    od_ble_publish_identity(own_addr_type);
    ESP_LOGI(TAG, "BLE identity address type: %s",
             (own_addr_type == BLE_OWN_ADDR_PUBLIC) ? "public" : "static-random");
    /* NO od_ble_advertise() here any more: the loop notices the identity and starts. */
}

static void od_on_reset(int reason)
{
    ESP_LOGW(TAG, "NimBLE host reset, reason %d", reason);
}

static void od_host_task(void *param)
{
    (void)param;
    nimble_port_run();            /* returns only on nimble_port_stop() */
    nimble_port_freertos_deinit();
}

/* ------------------------------------------------------------------ public API */

bool od_ble_init(const char *device_name)
{
    if (s_inited) {
        return true;
    }
    if (device_name && *device_name) {
        strncpy(s_name, device_name, sizeof(s_name) - 1);
        s_name[sizeof(s_name) - 1] = '\0';
    }

    if (nimble_port_init() != ESP_OK) {
        ESP_LOGE(TAG, "nimble_port_init failed");
        return false;
    }

    ble_hs_cfg.sync_cb  = od_on_sync;
    ble_hs_cfg.reset_cb = od_on_reset;

    /* SECURITY MANAGER DEFAULTS -- RESTORED FROM NimBLE-Arduino 2026-08-05.
     *
     * THIS IS THE ROOT CAUSE OF THE CONNECT STUTTER, and it is an omission rather than a
     * mistake: NimBLEDevice::init() sets all six of these explicitly
     * (NimBLE-Arduino/src/NimBLEDevice.cpp:991-997) and phase B's C-API rewrite set NONE of
     * them, silently inheriting ESP-IDF's defaults -- which ENABLE bonding and Secure
     * Connections. So the shipped Arduino firmware never bonded, and this port started
     * advertising bonding capability the moment it was flashed.
     *
     * The failure that follows is exact: a client accepts the offer and bonds; nothing here
     * calls ble_store_config_init() and NVS persistence is off in every baseline, so the bond
     * dies with the boot; the client keeps its half; the next connection presents keys this
     * device no longer has and NimBLE reports the connect with BLE_HS_EENCRYPT_KEY_SZ (26).
     * That is why it reproduced on the first connection after every deep-sleep wake.
     *
     * Nothing wants bonding here. No characteristic carries an _ENC or _AUTHEN flag
     * (od_chrs above), CONFIG_BT_NIMBLE_SM_LVL is 0, and confidentiality and authentication
     * are the application's job -- the 0x0050 challenge/response and per-frame AES-CCM, which
     * work identically over an unencrypted link. Link-layer pairing buys this device nothing
     * and costs it a connection failure class.
     *
     * Values are the wrapper's, field for field, deliberately: this is a restoration of
     * shipped behaviour, not a new security policy. Changing any of them is a wire-visible
     * decision about how the device pairs and belongs in DIVERGENCE_MATRIX.md. */
    ble_hs_cfg.sm_io_cap         = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_bonding        = 0;   /* do not offer to store keys */
    ble_hs_cfg.sm_mitm           = 0;   /* no man-in-the-middle protection demanded */
    ble_hs_cfg.sm_sc             = 0;   /* no Secure Connections */
    ble_hs_cfg.sm_our_key_dist   = BLE_SM_PAIR_KEY_DIST_ENC;
    ble_hs_cfg.sm_their_key_dist = BLE_SM_PAIR_KEY_DIST_ENC;

    ble_svc_gap_init();
    ble_svc_gatt_init();

    int rc = ble_gatts_count_cfg(od_svcs);
    if (rc == 0) {
        rc = ble_gatts_add_svcs(od_svcs);
    }
    if (rc != 0) {
        /* UNWIND WHAT WAS ACQUIRED. nimble_port_init() succeeded above, so returning here
         * without undoing it left the host partially initialised with nothing recording that
         * fact: s_inited stays false, so a retry calls nimble_port_init() again on top of a
         * live init and fails there instead -- one layer away from the cause, which is the
         * same class of mislocated failure F7 is about. */
        ESP_LOGE(TAG, "GATT registration failed: %d -- unwinding nimble_port_init()", rc);
        nimble_port_deinit();
        return false;
    }

    ble_svc_gap_device_name_set(s_name);
    if (s_preferred_mtu) {
        ble_att_set_preferred_mtu(s_preferred_mtu);
    }

    /* DELIBERATELY NO od_adv_control_init(&s_adv) HERE. s_adv is a static, so it already
     * holds exactly that function's postcondition -- all zero, "nothing wanted, nothing
     * running" -- on the first init. On a RE-init it must not be re-run: od_ble_deinit() calls
     * od_adv_stack_reset(), which preserves application intent precisely so a later init
     * resumes advertising without the caller asking again. Re-initialising here would discard
     * the intent the deinit just took care to keep. */
    nimble_port_freertos_init(od_host_task);
    s_inited = true;

    /* WAIT FOR HOST SYNC BEFORE RETURNING -- restored from NimBLEDevice::init(), which loops
     * on m_synced (NimBLE-Arduino/src/NimBLEDevice.cpp:1008) before it returns.
     *
     * Without this, init() returns the instant the host TASK is created, while the controller
     * has not yet synced and no identity address exists. Every caller reads that as "BLE is
     * up". Advertising happens to be safe -- the loop's pump defers until the identity exists
     * -- but od_ble_get_identity_addr() is NOT: it would hand back a
     * zeroed address, and that address is published in the mDNS `mac` TXT record, which is how
     * a host correlates the BLE and LAN identities of the same device. Publishing 00:00:.. is
     * worse than publishing late.
     *
     * BOUNDED, unlike the wrapper's unbounded spin: a controller that never syncs is a dead
     * radio, and hanging setup() forever is a worse failure than continuing without BLE. On a
     * healthy part this returns in a few milliseconds. */
    {
        const uint32_t kSyncTimeoutMs = 2000;
        uint32_t waited = 0;
        while (!od_ble_identity_valid() && waited < kSyncTimeoutMs) {
            vTaskDelay(pdMS_TO_TICKS(5));
            waited += 5;
        }
        if (!od_ble_identity_valid()) {
            ESP_LOGE(TAG, "host did not sync within %u ms -- continuing, but the BLE identity "
                          "address is not available yet", (unsigned)kSyncTimeoutMs);
        }
    }

    ESP_LOGI(TAG, "NimBLE up; GATT service registered, val_handle=%u",
             (unsigned)s_chr_val_handle);
    return true;
}

bool od_ble_is_ready(void)
{
    /* Both halves: the stack is initialised AND the host has synced far enough to have an
     * identity address. s_inited alone was true before sync, so this used to answer "ready"
     * for a stack that could not yet name itself. */
    return s_inited && od_ble_identity_valid();
}

bool od_ble_notify_handle(uint16_t conn_handle, const uint8_t *data, uint16_t len)
{
    if (!s_inited || !data || len == 0 || conn_handle == OD_BLE_CONN_NONE) {
        return false;
    }
    if (s_chr_val_handle == 0) {
        return false;
    }
    struct os_mbuf *om = ble_hs_mbuf_from_flat(data, len);
    if (!om) {
        return false;   /* mbuf exhaustion -- backpressure; caller retries from its queue */
    }
    /* Copies the payload into the mbuf above before this returns, so a concurrent client
     * WRITE_NR on this shared RX/TX characteristic cannot corrupt the outgoing frame.
     * ble_gatts_notify_custom() takes ownership of the mbuf on every path, success or not --
     * do not free it here. It fails when conn_handle is not connected or has not subscribed,
     * which is what makes a stale handle a false return rather than a misdelivery. */
    return ble_gatts_notify_custom(conn_handle, s_chr_val_handle, om) == 0;
}

extern "C" void od_ble_smp_fast_link(uint16_t conn_handle)
{
    const struct ble_gap_upd_params params = {
        .itvl_min = 6,               /* 7.5 ms */
        .itvl_max = 12,              /* 15 ms */
        .latency = 0,
        .supervision_timeout = 400,  /* 4 s */
        .min_ce_len = 0,
        .max_ce_len = 0,
    };

    (void)ble_gap_update_params(conn_handle, &params);
}

extern "C" int od_ble_smp_notify(uint16_t conn_handle, const uint8_t *data, size_t len)
{
    const uint16_t mtu = ble_att_mtu(conn_handle);
    const size_t chunk = mtu > 3u ? (size_t)(mtu - 3u) : 0u;
    const size_t n = len < chunk ? len : chunk;

    for (int tries = 0; n > 0u && tries < 50; tries++) {
        struct os_mbuf *om = ble_hs_mbuf_from_flat(data, (uint16_t)n);

        if (om != NULL && ble_gatts_notify_custom(conn_handle, s_smp_val_handle, om) == 0) {
            return (int)n;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return -1;
}

uint8_t od_ble_connected_count(void)
{
    return __atomic_load_n(&s_conn_count, __ATOMIC_ACQUIRE);
}

bool od_ble_disconnect(uint16_t conn_handle)
{
    if (!s_inited || conn_handle == OD_BLE_CONN_NONE) {
        return false;
    }
    const int rc = ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    /* "Already gone" is success, matching NimBLE-Arduino's NimBLEServer::disconnect(): a
     * client that left between the caller's decision and this call is a benign race, not a
     * failure to ask. Anything else is a genuine failure and the caller logs it. */
    return rc == 0 || rc == BLE_HS_ENOTCONN || rc == BLE_HS_EALREADY;
}

uint16_t od_ble_conn_interval_units(uint16_t conn_handle)
{
    struct ble_gap_conn_desc desc;
    if (!s_inited || conn_handle == OD_BLE_CONN_NONE) {
        return 0;
    }
    if (ble_gap_conn_find(conn_handle, &desc) != 0) {
        return 0;
    }
    return desc.conn_itvl;   /* 1.25 ms units */
}

void od_ble_request_fast_link(uint16_t conn_handle)
{
    if (!s_inited || conn_handle == OD_BLE_CONN_NONE) {
        return;
    }
    /* 2 Mbps both directions. The third argument is the CODED-PHY option and applies only
     * when the coded mask is set, so 0. The peer may decline and stay at 1M -- not an error,
     * and the grant (or refusal) surfaces via BLE_GAP_EVENT_PHY_UPDATE_COMPLETE. */
    int rc = ble_gap_set_prefered_le_phy(conn_handle,
                                         BLE_GAP_LE_PHY_2M_MASK, BLE_GAP_LE_PHY_2M_MASK, 0);
    if (rc != 0) {
        ESP_LOGW(TAG, "2M PHY request rejected (rc=%d, staying at 1M)", rc);
    }
    /* 251-octet Link-Layer PDUs (max DLE). Unlike NimBLE-Arduino's setDataLen(handle, octets),
     * the C API takes the on-air time too and does not derive it: 251 payload octets plus the
     * 14 bytes of LL overhead at 8 us/byte on the 1M PHY is 2120 us. Passing the range
     * maximum instead would be rejected by controllers that validate the pair. */
    const uint16_t tx_time = (uint16_t)((251 + 14) * 8);   /* 2120 us, within 0x0148..0x4290 */
    rc = ble_gap_set_data_len(conn_handle, 251, tx_time);
    if (rc != 0) {
        ESP_LOGW(TAG, "DLE 251 request rejected (rc=%d)", rc);
    }
    ESP_LOGD(TAG, "Requested fast link: 2M PHY + 251-octet DLE");
}

void od_ble_link_params(uint16_t conn_handle, uint8_t *tx_phy_out, uint8_t *rx_phy_out,
                        uint16_t *att_mtu_out, uint16_t *interval_units_out)
{
    uint8_t tx_phy = 0;
    uint8_t rx_phy = 0;
    if (s_inited && conn_handle != OD_BLE_CONN_NONE) {
        if (ble_gap_read_le_phy(conn_handle, &tx_phy, &rx_phy) != 0) {
            tx_phy = 0;
            rx_phy = 0;
        }
    }
    if (tx_phy_out) *tx_phy_out = tx_phy;
    if (rx_phy_out) *rx_phy_out = rx_phy;
    if (att_mtu_out) {
        *att_mtu_out = (s_inited && conn_handle != OD_BLE_CONN_NONE)
                           ? ble_att_mtu(conn_handle) : 0;
    }
    if (interval_units_out) *interval_units_out = od_ble_conn_interval_units(conn_handle);
}

void od_ble_set_manufacturer_data(const uint8_t *msd, uint8_t len)
{
    /* EXACTLY 16 now, where this used to take anything up to 32. The controller carries the
     * canonical MsdAdvertisement body as one fixed record, which is what lets the payload and
     * its revision publish together; a variable length would reintroduce the array/length pair
     * whose torn update is half of F4. Every caller in the tree already passes 16
     * (display_service.cpp updatemsdata()). A wrong length is refused loudly rather than
     * silently truncated -- a short MSD is a discovery failure that looks like a dead device. */
    if (!msd || len != (uint8_t)OD_ADV_MSD_LEN) {
        ESP_LOGE(TAG, "MSD must be exactly %u bytes (got %u) -- advertisement not updated",
                 (unsigned)OD_ADV_MSD_LEN, (unsigned)len);
        return;
    }
    od_adv_set_payload(&s_adv, msd);
}

void od_ble_restart_advertising(void)
{
    /* INTENT ONLY. No stack call happens here any more: od_ble_service_advertising() below
     * does the work on the loop task. That is what makes a request landing before host sync
     * simply wait rather than fail, and it is why this no longer needs to be told whether the
     * stack is up. */
    od_adv_request_start(&s_adv);
}

void od_ble_stop_advertising(void)
{
    od_adv_request_stop(&s_adv);
    /* Drive the stop to completion here rather than leaving it to the next loop pass. The
     * deep-sleep and reboot paths call this and then proceed immediately, so a deferred stop
     * would be a stop that had not happened yet. Bounded: RETRY (a busy controller) must not
     * turn a teardown into a hang, so give up after a few passes and let the caller continue
     * with the radio still up rather than block the loop.
     *
     * This is the seed of the teardown barrier; the full version with a timeout and truthful
     * failure reporting is plan step 7, alongside F7. */
    for (unsigned pass = 0; pass < 8u && !od_adv_is_quiescent(&s_adv); ++pass) {
        (void)od_ble_service_advertising(true);
    }
    if (!od_adv_is_quiescent(&s_adv)) {
        ESP_LOGW(TAG, "advertising did not reach quiescence in 8 passes");
    }
}

bool od_ble_service_advertising(bool start_allowed)
{
    /* THE LOOP-SIDE PUMP -- the whole point of the ownership change. Facts in, one
     * reconciliation step out, all on this task.
     *
     * Called every pass, not only when something looks pending: the controller is the thing
     * that knows whether anything is needed, and a caller second-guessing it is how the old
     * two-owner arrangement started. */
    if (!s_inited) {
        return false;
    }

    /* Facts. Each is a LEVEL read from an authoritative source, except the ended edge. */
    if (od_ble_identity_valid()) {
        od_adv_stack_ready(&s_adv);
    }
    od_adv_set_connection_count(&s_adv, od_ble_connected_count());
    if (__atomic_exchange_n(&s_adv_ended_pending, (uint8_t)0, __ATOMIC_ACQUIRE)) {
        od_adv_observe_ended(&s_adv);
    }

    const enum od_adv_process_result r = od_adv_process(&s_adv, start_allowed);

    /* Advertising wanted but not running, once every 5 s. The fields are the ones
     * od_adv_process() branches on, so the line names the gate that is stuck. */
    if (s_adv.desired && !s_adv.active) {
        const uint32_t now = od_hal_uptime_ms();
        if (s_adv_wanted_since_ms == 0u) {
            s_adv_wanted_since_ms = (now != 0u) ? now : 1u;
        } else if ((now - s_adv_wanted_since_ms) >= OD_ADV_STALL_REPORT_MS &&
                   (now - s_adv_stall_logged_ms) >= OD_ADV_STALL_REPORT_MS) {
            s_adv_stall_logged_ms = now;
            ESP_LOGW(TAG,
                     "advertising WANTED but not running for %u ms: stack_ready=%d conns=%u "
                     "payload_valid=%d payload_dirty=%d faulted=%d start_allowed=%d step=%d",
                     (unsigned)(now - s_adv_wanted_since_ms), (int)s_adv.stack_ready,
                     (unsigned)s_adv.connection_count, (int)s_adv.payload_valid,
                     (int)s_adv.payload_dirty, (int)s_adv.faulted, (int)start_allowed, (int)r);
        }
    } else {
        s_adv_wanted_since_ms = 0u;
        s_adv_stall_logged_ms = 0u;
    }

    return r == OD_ADV_ACTED;
}

bool od_ble_advertising_active(void)
{
    /* The controller's belief, reconciled against the stack every pass. Loop-task only. */
    return s_adv.active;
}

void od_ble_set_preferred_mtu(uint16_t mtu)
{
    s_preferred_mtu = mtu;
    if (s_inited && mtu) {
        ble_att_set_preferred_mtu(mtu);
    }
}

bool od_ble_get_identity_addr(uint8_t addr_out[6], uint8_t *addr_type_out)
{
    struct od_ble_identity id;
    if (!addr_out || !s_inited || !od_ble_read_identity(&id)) {
        return false;
    }
    /* s_own_addr_type is BLE_OWN_ADDR_* as ble_gap_adv_start() consumes it; ble_hs_id_copy_addr
     * wants a BLE_ADDR_* identity type. The two low values coincide (PUBLIC=0, RANDOM=1) and
     * only the RPA variants differ, which this build never selects. */
    uint8_t id_type = (id.addr_type == BLE_OWN_ADDR_PUBLIC) ? BLE_ADDR_PUBLIC
                                                            : BLE_ADDR_RANDOM;
    if (ble_hs_id_copy_addr(id_type, addr_out, NULL) != 0) {
        return false;
    }
    if (addr_type_out) {
        *addr_type_out = id_type;
    }
    return true;
}

bool od_ble_deinit(void)
{
    if (!s_inited) {
        return true;   /* already down: the caller's desired end state */
    }
    /* THE TEARDOWN BARRIER. Withdraw advertising intent and pump the controller to quiescence
     * BEFORE stopping the host, so nothing can be advertising when the controller is released.
     * od_ble_stop_advertising() does both and is bounded, so a stuck stop degrades to a logged
     * warning rather than a hang -- teardown must always make progress.
     *
     * This replaces a bare ble_gap_adv_stop(). The difference is not the stack call, it is
     * that INTENT is withdrawn first: a stop without that could be undone by the controller's
     * next reconciliation, which is exactly the "stop does not mean stop" defect F4 names. */
    od_ble_stop_advertising();
    /* nimble_port_stop() unblocks nimble_port_run() in the host task; nimble_port_deinit()
     * then tears the host down AND disables/releases the controller, which is the half that
     * od_ble_stop_advertising() never did and that esp_restart() does not do for us. */
    if (nimble_port_stop() != 0) {
        /* THE STOP FAILED, SO THE STACK IS STILL UP. Clearing the flags here -- which this
         * function used to do unconditionally -- would leave the firmware believing BLE was
         * torn down while the host task and the controller remained allocated. The next
         * od_ble_init() would then take its `if (s_inited) return true` early exit... except
         * s_inited would be false, so it would try a fresh nimble_port_init() on top of a
         * running stack and fail there instead, one layer away from the cause.
         *
         * NimBLEDevice::deinit() clears m_initialized only inside the successful branch and
         * reports failure (NimBLE-Arduino/src/NimBLEDevice.cpp:1025); this is that behaviour.
         * State is left intact so it still describes reality. */
        ESP_LOGE(TAG, "nimble_port_stop() failed -- BLE left UP, state unchanged");
        return false;
    }
    nimble_port_deinit();
    /* Drop the retained characteristic value. The wrapper got this for free -- deinit
     * destroyed the NimBLECharacteristic and a later init built a fresh one with an empty
     * NimBLEAttValue -- whereas this buffer is file-static and would otherwise let a client
     * read the previous session's last frame back after a teardown and restart. */
    s_chr_value_len  = 0;
    s_chr_val_handle = 0;
    /* Invalidate the identity so nothing reads a departed stack's address: leave the counter
     * ODD, which od_ble_read_identity() treats as "no coherent snapshot".
     *
     * DELIBERATELY NOT A RESET TO ZERO. The counter is also the stack generation, and a later
     * step discards facts stamped with a generation the stack has left behind -- which only
     * works if it never repeats a value. Zeroing here would restart generations at 2 after
     * every deinit/init cycle and make a stale fact from the previous stack indistinguishable
     * from a current one. `| 1` invalidates while staying monotonic. */
    {
        const uint32_t seq = __atomic_load_n(&s_identity_seq, __ATOMIC_RELAXED);
        __atomic_store_n(&s_identity_seq, seq | 1u, __ATOMIC_RELEASE);
    }
    s_inited         = false;
    /* The stack is gone: invalidate applied state but KEEP application intent, so a later
     * init resumes advertising without the caller re-requesting it. */
    od_adv_stack_reset(&s_adv);
    __atomic_store_n(&s_adv_ended_pending, (uint8_t)0, __ATOMIC_RELEASE);
    __atomic_store_n(&s_conn_count, (uint8_t)0, __ATOMIC_RELEASE);
    ESP_LOGI(TAG, "NimBLE host stopped and controller released");
    return true;
}

/* ------------------------------------------------- shared advertising HAL (od_hal_adv.h) ---
 *
 * The target side of the portable advertising controller
 * (shared/core/od_adv_control.c, docs/F4_PORTABLE_BLE_LIFECYCLE_PLAN.md).
 *
 * WHY THESE LIVE HERE AND NOT IN hal/od_hal_adv.c, where every other HAL lives. They need
 * this file's statics -- s_name, the identity snapshot, od_svc_uuid and od_gap_event --
 * only way to reach those from hal/ would be accessors that duplicate the state the F4 work
 * exists to consolidate. A second home for advertising state is the defect, not the fix.
 *
 * extern "C" because shared/ is plain C and binds its HAL at link time; this translation unit
 * is C++.
 *
 * NOT YET CALLED BY ANYTHING. od_ble_advertise() and the NimBLE callbacks still own
 * advertising exactly as before, so on-air behaviour is unchanged by this commit. Wiring the
 * controller in -- the event bridge, then removing advertising from the callbacks -- is plan
 * steps 4-7 and must not land before the Milestone 0 byte fixture exists to prove those bytes
 * did not move.
 */
extern "C" {

enum od_hal_adv_result od_hal_adv_program(const uint8_t msd[16])
{
    if (!s_inited) {
        return OD_HAL_ADV_ERROR;
    }
    /* Before host sync there is no identity address, so a programmed record could not be
     * advertised anyway. RETRY, not ERROR: this is ordinary startup timing and the controller
     * must simply try again next pass. */
    if (!od_ble_identity_valid()) {
        return OD_HAL_ADV_RETRY;
    }
    return od_ble_pack_adv_records(msd, (uint8_t)OD_ADV_MSD_LEN) ? OD_HAL_ADV_OK
                                                                 : OD_HAL_ADV_ERROR;
}

enum od_hal_adv_result od_hal_adv_start(void)
{
    if (!s_inited) {
        return OD_HAL_ADV_ERROR;
    }
    if (!od_ble_identity_valid()) {
        return OD_HAL_ADV_RETRY;
    }
    const int rc = od_ble_start_now();
    if (rc == 0) {
        return OD_HAL_ADV_OK;
    }
    if (rc == BLE_HS_EALREADY) {
        return OD_HAL_ADV_ALREADY;
    }
    /* BLE_HS_ENOMEM here is NOT a fault. With CONFIG_BT_NIMBLE_MAX_CONNECTIONS=1, connectable
     * undirected advertising needs a free connection object, so a client holding the single
     * slot makes this the EXPECTED result -- see the instrumentation note in od_ble_start_now().
     * Reporting it as RETRY keeps the controller's state truthful and lets the disconnect
     * resolve it, where ERROR would latch a fault on entirely normal operation. */
    if (rc == BLE_HS_ENOMEM) {
        return OD_HAL_ADV_RETRY;
    }
    return OD_HAL_ADV_ERROR;
}

enum od_hal_adv_result od_hal_adv_stop(void)
{
    if (!s_inited) {
        return OD_HAL_ADV_ERROR;
    }
    const int rc = ble_gap_adv_stop();
    if (rc == 0) {
        return OD_HAL_ADV_OK;
    }
    /* Already stopped is the outcome the caller wanted. */
    if (rc == BLE_HS_EALREADY) {
        return OD_HAL_ADV_NOT_ACTIVE;
    }
    return OD_HAL_ADV_ERROR;
}

} /* extern "C" */
