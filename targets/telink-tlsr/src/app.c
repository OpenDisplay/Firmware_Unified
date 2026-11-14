/* BLE peripheral bring-up and link events. Initialisation order and the FIFO definitions follow
 * Telink tc_ble_single_sdk vendor/ble_sample and vendor/ble_feature_test/feature_DLE_slave
 * (Apache-2.0); the order inside user_init_normal() is the SDK's, not a choice. */
#include "tl_common.h"
#include "drivers.h"
#include "stack/ble/ble.h"
#include "vendor/common/ble_flash.h"
#include "vendor/common/app_buffer.h"

#include "app.h"
#include "app_att.h"
#include "tlsr_port.h"

#define RX_FIFO_NUM        8
#define TX_FIFO_NUM        8

/* The link layer references these by name; their size carries the 251-octet DLE setting. */
u8        blt_rxfifo_b[RX_FIFO_SIZE * RX_FIFO_NUM] = {0};
my_fifo_t blt_rxfifo = { RX_FIFO_SIZE, RX_FIFO_NUM, 0, 0, blt_rxfifo_b };
u8        blt_txfifo_b[TX_FIFO_SIZE * TX_FIFO_NUM] = {0};
my_fifo_t blt_txfifo = { TX_FIFO_SIZE, TX_FIFO_NUM, 0, 0, blt_txfifo_b };

#define ADV_INTERVAL_MIN   800     /* 500 ms, 0.625 ms units */
#define ADV_INTERVAL_MAX   816     /* 510 ms */
#define RF_POWER           RF_POWER_P3dBm

static u8  s_mac[6];
static u8  s_adv[31];
static u8  s_adv_len;
static u8  s_scan_rsp[31];
static u8  s_scan_rsp_len;
static u8  s_connected;
static u32 s_connect_tick;
static u8  s_dle_requested;

/* Flags, then the manufacturer-specific block: company id + 14 bytes come pre-built by
 * od_advert_build(), so the AD element is length 17 = type byte + 16. */
void tlsr_port_set_adv_msd(const uint8_t msd[16])
{
    s_adv[0] = 2;  s_adv[1] = DT_FLAGS;  s_adv[2] = 0x06;
    s_adv[3] = 17; s_adv[4] = DT_MANUFACTURER_SPECIFIC_DATA;
    memcpy(&s_adv[5], msd, 16);
    s_adv_len = 21;
    bls_ll_setAdvData(s_adv, s_adv_len);
}

static void build_identity(void)
{
    static const char hexd[] = "0123456789ABCDEF";
    u8 name[8] = {'O', 'D'};
    u8 i;

    /* "OD" + the low three MAC bytes, most significant first -- the fleet convention (BG22 and
     * Nordic use "OD" + 6 hex), and the same suffix ATC_BLE_OEPL advertised on this tag. */
    for (i = 0; i < 3; ++i) {
        name[2 + i * 2] = hexd[s_mac[2 - i] >> 4];
        name[3 + i * 2] = hexd[s_mac[2 - i] & 0xF];
    }
    app_att_set_device_name(name, sizeof(name));

    s_scan_rsp[0] = 1 + sizeof(name); s_scan_rsp[1] = DT_COMPLETE_LOCAL_NAME;
    memcpy(&s_scan_rsp[2], name, sizeof(name));
    s_scan_rsp[10] = 3; s_scan_rsp[11] = DT_COMPLETE_LIST_16BIT_SERVICE_UUID;
    s_scan_rsp[12] = 0x46; s_scan_rsp[13] = 0x24;
    s_scan_rsp_len = 14;
}

/* The stack reboots into the new image right after a successful OTA. That reboot is deliberate:
 * clear the breadcrumb first, or the next boot reports it as a hang (and skips the boot screen). */
static void ota_result(int result)
{
    if (result == OTA_SUCCESS) {
        tlsr_port_crumb(0);
    }
}

static void task_connect(u8 e, u8 *p, int n)
{
    (void)e; (void)p; (void)n;
    tlsr_port_crumb(0x20);
    s_connected = 1;
    s_connect_tick = clock_time() | 1;
    s_dle_requested = 0;
    app_att_reset_subscriptions();
    /* 7.5-15 ms, no latency, 4 s supervision: a transfer is short and wants the fast interval. */
    bls_l2cap_requestConnParamUpdate(CONN_INTERVAL_7P5MS, CONN_INTERVAL_15MS, 0, CONN_TIMEOUT_4S);
    od_tlsr_on_connect();
}

static void task_terminate(u8 e, u8 *p, int n)
{
    (void)e; (void)p; (void)n;
    tlsr_port_crumb(0x21);
    tlsr_port_ota_arm(false);
    s_connected = 0;
    s_connect_tick = 0;
    app_att_reset_subscriptions();
    od_tlsr_on_disconnect();
}

void tlsr_port_stay_awake(uint8_t holder, bool on)
{
    static uint8_t s_holders;

    s_holders = on ? (uint8_t)(s_holders | holder) : (uint8_t)(s_holders & ~holder);
    bls_pm_setSuspendMask(s_holders != 0u ? SUSPEND_DISABLE : (SUSPEND_ADV | SUSPEND_CONN));
}

bool tlsr_port_connected(void)
{
    return s_connected != 0;
}

void tlsr_port_disconnect(void)
{
    if (s_connected) {
        bls_ll_terminateConnection(HCI_ERR_REMOTE_USER_TERM_CONN);
    }
}

int tlsr_port_notify(const uint8_t *frame, uint16_t len)
{
    ble_sts_t st;

    if (!s_connected || !app_att_notify_enabled()) {
        return TLSR_PORT_NOTIFY_FAILED;
    }
    tlsr_port_crumb(8);
    st = blc_gatt_pushHandleValueNotify(BLS_CONN_HANDLE, OD_DP_H, (u8 *)frame, len);
    tlsr_port_crumb(9);
    if (st == BLE_SUCCESS) {
        return TLSR_PORT_NOTIFY_SENT;
    }
    if (st == LL_ERR_TX_FIFO_NOT_ENOUGH || st == GATT_ERR_DATA_PENDING_DUE_TO_SERVICE_DISCOVERY_BUSY) {
        return TLSR_PORT_NOTIFY_BUSY;
    }
    return TLSR_PORT_NOTIFY_FAILED;
}

void tlsr_port_mac(uint8_t out[6])
{
    memcpy(out, s_mac, 6);
}

_attribute_no_inline_ void user_init_normal(void)
{
    u8 mac_random_static[6];

    random_generator_init();
    blc_readFlashSize_autoConfigCustomFlashSector();
    blc_app_loadCustomizedParameters_normal();
    blc_initMacAddress(flash_sector_mac_address, s_mac, mac_random_static);

    blc_ll_initBasicMCU();
    blc_ll_initStandby_module(s_mac);
    blc_ll_initAdvertising_module(s_mac);
    blc_ll_initConnection_module();
    blc_ll_initSlaveRole_module();

    blc_gap_peripheral_init();
    blc_l2cap_register_handler(blc_l2cap_packet_receive);
    my_att_init();
    blc_att_setRxMtuSize(MTU_SIZE_SETTING);
#if (MTU_SIZE_SETTING > ATT_MTU_MAX_SDK_DFT_BUF)    /* the stack's own buffer covers <= 250 */
    blc_l2cap_initMtuBuffer(app_l2cap_rx_fifo, ACL_L2CAP_BUFF_SIZE, app_l2cap_tx_fifo, ACL_L2CAP_BUFF_SIZE);
#endif
    blc_smp_setSecurityLevel(No_Security);

    blc_ota_initOtaServer_module();
    blc_ota_setOtaProcessTimeout(300);
    blc_ota_registerOtaResultIndicationCb(ota_result);

    build_identity();
    od_tlsr_init();                     /* loads config and publishes the first MSD via the port */

    bls_ll_setAdvParam(ADV_INTERVAL_MIN, ADV_INTERVAL_MAX, ADV_TYPE_CONNECTABLE_UNDIRECTED,
                       OWN_ADDRESS_PUBLIC, 0, NULL, BLT_ENABLE_ADV_ALL, ADV_FP_NONE);
    bls_ll_setScanRspData(s_scan_rsp, s_scan_rsp_len);
    bls_ll_setAdvEnable(BLC_ADV_ENABLE);
    rf_set_power_level_index(RF_POWER);

    bls_app_registerEventCallback(BLT_EV_FLAG_CONNECT, &task_connect);
    bls_app_registerEventCallback(BLT_EV_FLAG_TERMINATE, &task_terminate);

    blc_ll_initPowerManagement_module();
    bls_pm_setSuspendMask(SUSPEND_ADV | SUSPEND_CONN);

    blc_app_checkControllerHostInitialization();
}

_attribute_ram_code_ void user_init_deepRetn(void)
{
    /* Deep retention is disabled (app_config.h); a retention wake cannot happen. */
}

_attribute_no_inline_ void main_loop(void)
{
    tlsr_port_crumb(0x10);
    blt_sdk_main_loop();
    tlsr_port_crumb(0x11);

    /* Centrals usually start the data-length update; ask once if this one has not after 1 s. */
    if (s_connect_tick && !s_dle_requested && clock_time_exceed(s_connect_tick, 1000000)) {
        s_dle_requested = 1;
        blc_ll_exchangeDataLength(LL_LENGTH_REQ, ACL_CONN_MAX_TX_OCTETS);
    }
    od_tlsr_poll();
    tlsr_port_crumb(0);
}
