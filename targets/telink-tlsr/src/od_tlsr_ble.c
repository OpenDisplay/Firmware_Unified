#include "od_tlsr_sdk.h"

#include "od_tlsr_ble.h"

#include "od_tlsr_ota.h"
#include "od_tlsr_target.h"

#include "od_adv_app.h"
#include "od_advert.h"
#include "od_cmd.h"
#include "od_config_read.h"
#include "od_core.h"
#include "od_dispatch.h"
#include "od_hal_radio.h"
#include "od_hal_time.h"
#include "od_sec2.h"
#include "od_span.h"
#include "od_txq.h"

#include "opendisplay_structs.h"

#include <string.h>

#define OD_BLE_MTU            247u
#define OD_AUTH_ABUSE_LIMIT   3u
#define OD_EVENT_HOLD_MAX_MS  2000u
#define OD_CONFIG_READ_MAX_MS 1500u
#define OD_MSD_REFRESH_MS     10000u

/* ---- GATT table ---- */

enum {
    H_GAP_PS = 1, H_GAP_NAME_CD, H_GAP_NAME, H_GAP_APPEAR_CD, H_GAP_APPEAR, H_GAP_CONN_CD, H_GAP_CONN,
    H_GATT_PS, H_GATT_SC_CD, H_GATT_SC, H_GATT_SC_CCC,
    H_OD_PS, H_OD_CD, H_OD_VAL, H_OD_CCC,
    H_OTA_PS, H_OTA_CD, H_OTA_VAL, H_OTA_CCC,
    H_END
};

typedef struct {
    u16 intervalMin, intervalMax, latency, timeout;
} conn_params_t;

static const u16 uuid_primary = GATT_UUID_PRIMARY_SERVICE;
static const u16 uuid_char = GATT_UUID_CHARACTER;
static const u16 uuid_ccc = GATT_UUID_CLIENT_CHAR_CFG;
static const u16 uuid_gap = SERVICE_UUID_GENERIC_ACCESS;
static const u16 uuid_gatt = SERVICE_UUID_GENERIC_ATTRIBUTE;
static const u16 uuid_name = GATT_UUID_DEVICE_NAME;
static const u16 uuid_appear = GATT_UUID_APPEARANCE;
static const u16 uuid_conn = GATT_UUID_PERI_CONN_PARAM;
static const u16 uuid_sc = GATT_UUID_SERVICE_CHANGE;
static const u16 uuid_od = 0x2446;
static const u16 appearance = GAP_APPEARE_UNKNOWN;
static const conn_params_t conn_params = { 8, 24, 0, 600 };

/* Updater service and characteristic, 128-bit, little-endian on the wire:
 * service 6f64f000-1e1e-4c4b-8b5a-7454534c5200, characteristic ...f001-... */
static const u8 uuid_ota_svc[16] = { 0x00, 0x52, 0x4c, 0x53, 0x54, 0x74, 0x5a, 0x8b,
                                     0x4b, 0x4c, 0x1e, 0x1e, 0x00, 0xf0, 0x64, 0x6f };
static const u8 uuid_ota_chr[16] = { 0x00, 0x52, 0x4c, 0x53, 0x54, 0x74, 0x5a, 0x8b,
                                     0x4b, 0x4c, 0x1e, 0x1e, 0x01, 0xf0, 0x64, 0x6f };

static const u8 cd_name[5] = { CHAR_PROP_READ, U16_LO(H_GAP_NAME), U16_HI(H_GAP_NAME),
                               U16_LO(GATT_UUID_DEVICE_NAME), U16_HI(GATT_UUID_DEVICE_NAME) };
static const u8 cd_appear[5] = { CHAR_PROP_READ, U16_LO(H_GAP_APPEAR), U16_HI(H_GAP_APPEAR),
                                 U16_LO(GATT_UUID_APPEARANCE), U16_HI(GATT_UUID_APPEARANCE) };
static const u8 cd_conn[5] = { CHAR_PROP_READ, U16_LO(H_GAP_CONN), U16_HI(H_GAP_CONN),
                               U16_LO(GATT_UUID_PERI_CONN_PARAM), U16_HI(GATT_UUID_PERI_CONN_PARAM) };
static const u8 cd_sc[5] = { CHAR_PROP_INDICATE, U16_LO(H_GATT_SC), U16_HI(H_GATT_SC),
                             U16_LO(GATT_UUID_SERVICE_CHANGE), U16_HI(GATT_UUID_SERVICE_CHANGE) };
static const u8 cd_od[5] = { CHAR_PROP_READ | CHAR_PROP_WRITE | CHAR_PROP_WRITE_WITHOUT_RSP | CHAR_PROP_NOTIFY,
                             U16_LO(H_OD_VAL), U16_HI(H_OD_VAL), 0x46, 0x24 };
static const u8 cd_ota[19] = { CHAR_PROP_WRITE | CHAR_PROP_NOTIFY, U16_LO(H_OTA_VAL), U16_HI(H_OTA_VAL),
                               0x00, 0x52, 0x4c, 0x53, 0x54, 0x74, 0x5a, 0x8b,
                               0x4b, 0x4c, 0x1e, 0x1e, 0x01, 0xf0, 0x64, 0x6f };

static u8 s_name[OD_ADVERT_NAME_LEN];
static u16 s_sc_val[2];
static u8 s_sc_ccc[2];
static u8 s_od_val[1];
static u8 s_od_ccc[2];
static u8 s_ota_val[1];
static u8 s_ota_ccc[2];

static int od_write_cb(void *p);
static int ota_write_cb(void *p);

/* A primary-service entry's first field is the number of attributes in its group, itself
 * included; each is derived from the handle enum so the table and the count cannot disagree. */
static const attribute_t s_att[] = {
    { H_END - 1, 0, 0, 0, 0, 0, 0, 0 },
    { H_GATT_PS - H_GAP_PS, ATT_PERMISSIONS_READ, 2, 2, (u8 *)&uuid_primary, (u8 *)&uuid_gap, 0, 0 },
    { 0, ATT_PERMISSIONS_READ, 2, sizeof cd_name, (u8 *)&uuid_char, (u8 *)cd_name, 0, 0 },
    { 0, ATT_PERMISSIONS_READ, 2, sizeof s_name, (u8 *)&uuid_name, s_name, 0, 0 },
    { 0, ATT_PERMISSIONS_READ, 2, sizeof cd_appear, (u8 *)&uuid_char, (u8 *)cd_appear, 0, 0 },
    { 0, ATT_PERMISSIONS_READ, 2, sizeof appearance, (u8 *)&uuid_appear, (u8 *)&appearance, 0, 0 },
    { 0, ATT_PERMISSIONS_READ, 2, sizeof cd_conn, (u8 *)&uuid_char, (u8 *)cd_conn, 0, 0 },
    { 0, ATT_PERMISSIONS_READ, 2, sizeof conn_params, (u8 *)&uuid_conn, (u8 *)&conn_params, 0, 0 },

    { H_OD_PS - H_GATT_PS, ATT_PERMISSIONS_READ, 2, 2, (u8 *)&uuid_primary, (u8 *)&uuid_gatt, 0, 0 },
    { 0, ATT_PERMISSIONS_READ, 2, sizeof cd_sc, (u8 *)&uuid_char, (u8 *)cd_sc, 0, 0 },
    { 0, ATT_PERMISSIONS_READ, 2, sizeof s_sc_val, (u8 *)&uuid_sc, (u8 *)s_sc_val, 0, 0 },
    { 0, ATT_PERMISSIONS_RDWR, 2, sizeof s_sc_ccc, (u8 *)&uuid_ccc, s_sc_ccc, 0, 0 },

    { H_OTA_PS - H_OD_PS, ATT_PERMISSIONS_READ, 2, 2, (u8 *)&uuid_primary, (u8 *)&uuid_od, 0, 0 },
    { 0, ATT_PERMISSIONS_READ, 2, sizeof cd_od, (u8 *)&uuid_char, (u8 *)cd_od, 0, 0 },
    { 0, ATT_PERMISSIONS_RDWR, 2, sizeof s_od_val, (u8 *)&uuid_od, s_od_val, od_write_cb, 0 },
    { 0, ATT_PERMISSIONS_RDWR, 2, sizeof s_od_ccc, (u8 *)&uuid_ccc, s_od_ccc, 0, 0 },

    { H_END - H_OTA_PS, ATT_PERMISSIONS_READ, 2, 16, (u8 *)&uuid_primary, (u8 *)uuid_ota_svc, 0, 0 },
    { 0, ATT_PERMISSIONS_READ, 2, sizeof cd_ota, (u8 *)&uuid_char, (u8 *)cd_ota, 0, 0 },
    { 0, ATT_PERMISSIONS_RDWR, 16, sizeof s_ota_val, (u8 *)uuid_ota_chr, s_ota_val, ota_write_cb, 0 },
    { 0, ATT_PERMISSIONS_RDWR, 2, sizeof s_ota_ccc, (u8 *)&uuid_ccc, s_ota_ccc, 0, 0 },
};
OD_STATIC_ASSERT(sizeof s_att / sizeof s_att[0] == H_END, "one table entry per handle");

/* ---- link state ----
 * Written by stack callbacks, which run inside blt_sdk_main_loop() on the main thread, never in
 * an interrupt; the loop reads them after that call returns. */

/* OD_RXQ_SLOTS is the profile's depth; od_pipe.h asserts it holds a PIPE window plus END. */
#define RXQ_SLOTS OD_RXQ_SLOTS

static struct {
    bool     connected;
    bool     closed_pending;  /* a link ended; the loop owes it a teardown */
    uint32_t epoch;           /* od_reply_t tag of the current link; 0 = none */
    uint8_t  abuse;
    uint8_t  rx_head, rx_count;
    uint16_t rx_len[RXQ_SLOTS];
    uint8_t  rx[RXQ_SLOTS][OD_BLE_MTU];
    uint32_t rx_dropped;
    uint8_t  ota_reply[OD_TLSR_OTA_REPLY_MAX];
    uint8_t  ota_reply_len;
    bool     hold_active;
    uint32_t hold_started;
    bool     read_active;
    uint32_t read_started;
} s;

static u8 s_adv[OD_ADVERT_LEGACY_PDU_MAX];
static u8 s_msd[OD_ADVERT_MSD_LEN];
static uint8_t s_msd_counter;
static uint32_t s_msd_at;

static const u8 s_scan_rsp[] = { 3, DT_COMPLETE_LIST_16BIT_SERVICE_UUID, 0x46, 0x24 };

/* ---- power management ----
 * The link layer sleeps between advertising and connection events whenever the mask allows it;
 * a wake comes with the next radio event. Sleep is allowed only while the app has nothing to do,
 * so queued work never waits a whole interval: a frame queued by the write callback turns it
 * off at once, and the loop turns it back on once everything has drained.
 *
 * Between advertising events the sleep may be deep retention (4 uA floor against suspend's
 * 45 uA on OD060F40): the first 32 KB of SRAM, which tools/retention_link.py makes hold every static,
 * survives and main() runs again (od_tlsr_ble_retention_wake()). Never in a connection, so a
 * transfer never depends on that path. And only once RETN_AFTER_MS have passed since a boot with
 * no unplanned reset since power was applied: a fault in the wake path ends in a watchdog reset,
 * which turns retention off until the next power cycle and leaves the tag in suspend-only
 * operation, still answering the updater. */

#define RETN_AFTER_MS 60000u

static uint16_t s_pm_mask = 0xFFFFu;
static volatile bool s_boost_req;   /* od_adv_app_boost(): open the fast window */

static bool retention_ok(void)
{
    return od_tlsr_hal_unplanned() == 0u && od_hal_uptime_ms() >= RETN_AFTER_MS;
}

static void pm_set(bool allow)
{
    const uint16_t m = !allow ? SUSPEND_DISABLE
                     : retention_ok() ? (SUSPEND_ADV | DEEPSLEEP_RETENTION_ADV | SUSPEND_CONN)
                                      : (SUSPEND_ADV | SUSPEND_CONN);

    if (m != s_pm_mask) {
        s_pm_mask = m;
        bls_pm_setSuspendMask(m);
    }
}

/* RF power settings do not survive a suspend. */
static void on_suspend_exit(u8 e, u8 *p, int n)
{
    (void)e;
    (void)p;
    (void)n;
    rf_set_power_level_index(RF_POWER_P3dBm);
}

/* ---- stack callbacks ---- */

static int od_write_cb(void *p)
{
    rf_packet_att_write_t *req = (rf_packet_att_write_t *)p;
    uint16_t len = (uint16_t)(req->l2capLen - 3u);
    uint8_t slot;

    if (!s.connected || len < 2u || len > OD_BLE_MTU) {
        return 0;
    }
    if (s.rx_count == RXQ_SLOTS) {
        s.rx_dropped++;
        return 0;
    }
    slot = (uint8_t)((s.rx_head + s.rx_count) % RXQ_SLOTS);
    memcpy(s.rx[slot], &req->value, len);
    s.rx_len[slot] = len;
    s.rx_count++;
    pm_set(false);
    return 0;
}

static int ota_write_cb(void *p)
{
    rf_packet_att_write_t *req = (rf_packet_att_write_t *)p;
    uint16_t len = (uint16_t)(req->l2capLen - 3u);
    uint8_t n;

    /* The updater programs flash synchronously, here, so its writes are acknowledged at the ATT
     * layer only once they are in flash. */
    n = od_tlsr_ota_write(&req->value, len, s.ota_reply);
    if (n != 0u) {
        s.ota_reply_len = n;
        pm_set(false);
    }
    return 0;
}

/* What a closed link leaves behind: the rx ring, core and command state, the updater's link
 * state. Runs from the loop after a terminate, or from on_connect when the terminate and the
 * next connect arrived in one blt_sdk_main_loop() pass -- before the new link's first write is
 * queued, so its frames are never the ones cleared. */
static void teardown(void)
{
    od_core_reset();
    od_tlsr_cmd_reset();
    od_tlsr_ota_link_closed();
    s.rx_head = 0u;
    s.rx_count = 0u;
    s.abuse = 0u;
    s.hold_active = false;
    s.read_active = false;
    s.ota_reply_len = 0u;
}

static void on_connect(u8 e, u8 *p, int n)
{
    (void)e; (void)p; (void)n;
    if (s.closed_pending) {
        s.closed_pending = false;
        teardown();
    }
    s.connected = true;
    if (++s.epoch == 0u) {
        s.epoch = 1u;
    }
    s.abuse = 0u;
    s_od_ccc[0] = s_od_ccc[1] = 0u;
    s_ota_ccc[0] = s_ota_ccc[1] = 0u;
    bls_l2cap_requestConnParamUpdate(conn_params.intervalMin, conn_params.intervalMax,
                                     conn_params.latency, conn_params.timeout);
}

static void on_terminate(u8 e, u8 *p, int n)
{
    (void)e; (void)p; (void)n;
    s.connected = false;
    s.closed_pending = true;
    s_boost_req = true;
}

/* ---- od_hal_radio ---- */

bool od_hal_radio_tag_is_live(od_origin_t origin, uint32_t tag)
{
    return origin == OD_ORIGIN_BLE && s.connected && tag == s.epoch;
}

od_radio_result_t od_hal_radio_send(od_origin_t origin, uint32_t tag, const uint8_t *frame,
                                    uint16_t len)
{
    ble_sts_t st;

    if (frame == NULL || len == 0u || origin != OD_ORIGIN_BLE) {
        return OD_RADIO_ERROR;
    }
    if (!od_hal_radio_tag_is_live(origin, tag)) {
        return OD_RADIO_GONE;
    }
    if ((s_od_ccc[0] & 0x01u) == 0u) {
        return OD_RADIO_ERROR;
    }
    st = blc_gatt_pushHandleValueNotify(BLS_CONN_HANDLE, H_OD_VAL, (u8 *)frame, len);
    if (st == BLE_SUCCESS) {
        return OD_RADIO_SENT;
    }
    if (st == LL_ERR_TX_FIFO_NOT_ENOUGH || st == GATT_ERR_DATA_PENDING_DUE_TO_SERVICE_DISCOVERY_BUSY) {
        return OD_RADIO_RETRY;
    }
    if (st == LL_ERR_CONNECTION_NOT_ESTABLISH) {
        return OD_RADIO_GONE;
    }
    return OD_RADIO_ERROR;
}

void od_txq_app_dropped(const od_reply_t *rp, uint16_t len, od_radio_result_t why)
{
    (void)rp; (void)len; (void)why;
}

void od_tlsr_ble_close(uint32_t tag)
{
    if (s.connected && tag == s.epoch) {
        bls_ll_terminateConnection(HCI_ERR_REMOTE_USER_TERM_CONN);
    }
}

/* ---- od_core_frame_done: od_frame_policy with BLE scoping (od_cmd.h) ---- */

void od_core_frame_done(const od_reply_t *rp, od_frame_outcome_t outcome)
{
    od_frame_policy_t policy = od_frame_policy(outcome);

    if (rp == NULL || rp->origin != OD_ORIGIN_BLE || !s.connected || rp->tag != s.epoch) {
        return;
    }
    if (policy.reset_abuse) {
        s.abuse = 0u;
    }
    if (policy.increment_abuse && s.abuse < 0xFFu && ++s.abuse >= OD_AUTH_ABUSE_LIMIT) {
        bls_ll_terminateConnection(HCI_ERR_REMOTE_USER_TERM_CONN);
    }
}

/* ---- advertising ---- */

void od_tlsr_ble_copy_msd(uint8_t out[16])
{
    memcpy(out, s_msd, sizeof s_msd);
}

void od_tlsr_ble_refresh_adv(void)
{
    struct od_advert_inputs in;
    struct od_advert_device_state st;
    unsigned n = 0u;

    memset(&in, 0, sizeof in);
    in.dynamic = NULL;
    in.chip_temperature_c = od_tlsr_chip_temperature_c();
    in.battery_10mv = od_advert_battery_10mv_from_mv(od_tlsr_battery_mv());
    in.reboot_flag = od_tlsr_reboot_flag();
    in.loop_counter = s_msd_counter;
    od_advert_device_state_now(&st);
    od_advert_apply_device_state(&in, &st);
    od_advert_build(&in, s_msd);
    s_msd_counter = od_advert_advance_counter(s_msd_counter);

    s_adv[n++] = 2;
    s_adv[n++] = DT_FLAGS;
    s_adv[n++] = 0x06; /* LE general discoverable, BR/EDR not supported */
    s_adv[n++] = (u8)(1u + OD_ADVERT_NAME_LEN);
    s_adv[n++] = DT_COMPLETE_LOCAL_NAME;
    memcpy(&s_adv[n], s_name, OD_ADVERT_NAME_LEN);
    n += OD_ADVERT_NAME_LEN;
    s_adv[n++] = (u8)(1u + OD_ADVERT_MSD_LEN);
    s_adv[n++] = DT_MANUFACTURER_SPECIFIC_DATA;
    memcpy(&s_adv[n], s_msd, OD_ADVERT_MSD_LEN);
    n += OD_ADVERT_MSD_LEN;
    bls_ll_setAdvData(s_adv, (u8)n);
}

/* Advertising cadence, as on Nordic and BG22: 1000 ms while idle, 20-30 ms for 3 s after
 * something a host should see (boot, a disconnect, an od_adv_app_boost() caller). One
 * advertising event measures about 17 uC at 3.0 V, so at 1000 ms it adds ~17 uA to the ~45 uA
 * suspend floor; at 100 ms it was ~170 uA. */
#define ADV_IDLE_MIN   ADV_INTERVAL_1S
#define ADV_IDLE_MAX   (ADV_INTERVAL_1S + 16)
#define ADV_BOOST_MIN  ADV_INTERVAL_20MS
#define ADV_BOOST_MAX  ADV_INTERVAL_30MS
#define ADV_BOOST_MS   3000u

static bool s_boosting;
static uint32_t s_boost_start;

/* od_adv_app.h: something happened that a host should learn about soon. Publish only; the
 * loop rebuilds the payload and opens the fast window. */
void od_adv_app_boost(void)
{
    s_boost_req = true;
}

static void adv_cadence(uint32_t now)
{
    if (s_boost_req) {
        s_boost_req = false;
        od_tlsr_ble_refresh_adv();
        s_boost_start = now;
        if (!s_boosting) {
            s_boosting = true;
            (void)bls_ll_setAdvInterval(ADV_BOOST_MIN, ADV_BOOST_MAX);
        }
    } else if (s_boosting && (uint32_t)(now - s_boost_start) >= ADV_BOOST_MS) {
        s_boosting = false;
        (void)bls_ll_setAdvInterval(ADV_IDLE_MIN, ADV_IDLE_MAX);
    }
}

/* ---- init and loop ---- */

static void hex6(u8 *out, const uint8_t mac[6])
{
    static const char hex[] = "0123456789ABCDEF";
    unsigned k;
    /* the low three bytes of the public address (stored least significant first), printed
     * most significant first */
    for (k = 0u; k < 3u; ++k) {
        out[2u * k] = (u8)hex[mac[2u - k] >> 4];
        out[2u * k + 1u] = (u8)hex[mac[2u - k] & 0x0Fu];
    }
}

void od_tlsr_ble_init(const uint8_t mac[6])
{
    u8 addr[6];

    memcpy(addr, mac, 6u);
    s_name[0] = 'O';
    s_name[1] = 'D';
    hex6(&s_name[2], mac);

    blc_ll_initBasicMCU();
    blc_ll_initStandby_module(addr);
    blc_ll_initAdvertising_module(addr);
    blc_ll_initConnection_module();
    blc_ll_initSlaveRole_module();
    blc_gap_peripheral_init();
    blc_l2cap_register_handler(blc_l2cap_packet_receive);
    bls_att_setAttributeTable((u8 *)s_att);
    blc_att_setRxMtuSize(OD_BLE_MTU);
    blc_smp_setSecurityLevel(No_Security);

    bls_ll_setAdvParam(ADV_IDLE_MIN, ADV_IDLE_MAX, ADV_TYPE_CONNECTABLE_UNDIRECTED,
                       OWN_ADDRESS_PUBLIC, 0, NULL, BLT_ENABLE_ADV_ALL, ADV_FP_NONE);
    od_tlsr_ble_refresh_adv();
    bls_ll_setScanRspData((u8 *)s_scan_rsp, sizeof s_scan_rsp);
    bls_ll_setAdvEnable(BLC_ADV_ENABLE);
    rf_set_power_level_index(RF_POWER_P3dBm);

    bls_app_registerEventCallback(BLT_EV_FLAG_CONNECT, &on_connect);
    bls_app_registerEventCallback(BLT_EV_FLAG_TERMINATE, &on_terminate);
    bls_app_registerEventCallback(BLT_EV_FLAG_SUSPEND_EXIT, &on_suspend_exit);
    blc_ll_initPowerManagement_module();
    blc_pm_setDeepsleepRetentionType(DEEPSLEEP_MODE_RET_SRAM_LOW32K);
    blc_pm_setDeepsleepRetentionThreshold(95, 95);
    blc_pm_setDeepsleepRetentionEarlyWakeupTiming(270);
    pm_set(false);
    s_boost_req = true;
    blc_app_checkControllerHostInitialization();
}

/* main() after a deep-retention wake, in place of od_tlsr_ble_init(): RAM is intact, the link
 * layer's hardware state is not (the SDK sample's user_init_deepRetn()). */
_attribute_ram_code_ void od_tlsr_ble_retention_wake(void)
{
    blc_app_loadCustomizedParameters_deepRetn();
    blc_ll_initBasicMCU();
    rf_set_power_level_index(RF_POWER_P3dBm);
    blc_ll_recoverDeepRetention();
}

/* Dispatch is admitted only while it can reserve reply capacity and no producer owns the
 * shared buffers -- the condition BG22 expresses through sl_bt_can_process_event(). A frame
 * that is not admitted waits in the ring; DEFERRED leaves it there too. */
static void drain_rx(void)
{
    while (s.rx_count != 0u && s.connected) {
        od_reply_t rp;
        od_frame_outcome_t outcome;

        if (od_config_read_active() || od_sec2_tx_active() || od_txq_depth() != 0u) {
            return;
        }
        rp.origin = OD_ORIGIN_BLE;
        rp.tag = s.epoch;
        outcome = od_dispatch_frame(&rp, od_span_make(s.rx[s.rx_head], s.rx_len[s.rx_head]));
        od_core_frame_done(&rp, outcome);
        if (outcome == OD_FRAME_DEFERRED) {
            return;
        }
        s.rx_head = (uint8_t)((s.rx_head + 1u) % RXQ_SLOTS);
        s.rx_count--;
        (void)od_txq_process();
    }
}

static void watch_holds(uint32_t now)
{
    if (od_config_read_active()) {
        if (!s.read_active) {
            s.read_active = true;
            s.read_started = now;
        } else if ((uint32_t)(now - s.read_started) >= OD_CONFIG_READ_MAX_MS) {
            teardown();
            return;
        }
    } else {
        s.read_active = false;
    }
    if (!od_config_read_active() && od_txq_depth() == 0u) {
        s.hold_active = false;
    } else if (!s.hold_active) {
        s.hold_active = true;
        s.hold_started = now;
    } else if ((uint32_t)(now - s.hold_started) >= OD_EVENT_HOLD_MAX_MS) {
        teardown();
    }
}

void od_tlsr_ota_app_notify(const uint8_t *reply, uint8_t len)
{
    if (len <= sizeof s.ota_reply) {
        memcpy(s.ota_reply, reply, len);
        s.ota_reply_len = len;
    }
}

static void flush_ota_reply(void)
{
    if (s.ota_reply_len == 0u) {
        return;
    }
    if (!s.connected || (s_ota_ccc[0] & 0x01u) == 0u) {
        s.ota_reply_len = 0u;
        return;
    }
    if (blc_gatt_pushHandleValueNotify(BLS_CONN_HANDLE, H_OTA_VAL, s.ota_reply, s.ota_reply_len) ==
        BLE_SUCCESS) {
        s.ota_reply_len = 0u;
    }
}

void od_tlsr_ble_loop(uint32_t now)
{
    if (s.closed_pending) {
        s.closed_pending = false;
        teardown();
    }
    od_tlsr_ota_service(now);
    flush_ota_reply();
    adv_cadence(now);

    (void)od_txq_process();
    (void)od_config_read_pump();
    od_sec2_pump();
    (void)od_txq_process();
    drain_rx();
    if (s.connected) {
        watch_holds(now);
    }

    if (!s.connected && (uint32_t)(now - s_msd_at) >= OD_MSD_REFRESH_MS) {
        s_msd_at = now;
        od_tlsr_ble_refresh_adv();
    }
}

bool od_tlsr_ble_idle(void)
{
    const enum od_tlsr_ota_state ota = od_tlsr_ota_state();

    return s.rx_count == 0u && !s.closed_pending && s.ota_reply_len == 0u &&
           od_txq_depth() == 0u && !od_config_read_active() && !od_sec2_tx_active() &&
           ota != OD_TLSR_OTA_ERASING && ota != OD_TLSR_OTA_VERIFYING &&
           ota != OD_TLSR_OTA_INSTALLING;
}

void od_tlsr_ble_power(bool app_idle)
{
    pm_set(app_idle && od_tlsr_ble_idle());
}
