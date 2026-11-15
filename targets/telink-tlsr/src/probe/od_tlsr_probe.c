/* The bring-up probe: the smallest image that proves this chip runs our BLE stack and can be
 * updated back to anything else. It is the first non-legacy image a tag receives.
 *
 * It runs the SDK link layer, GAP/GATT and the updater (od_tlsr_ota.c, od_tlsr_install.c,
 * od_tlsr_flash.c, the same files as the full image), and nothing of the OpenDisplay protocol
 * stack: no config, no SEC2, no shared core. It never writes the config or secret banks. A
 * fault in the shared stack therefore cannot stop it from accepting the way back.
 *
 * Advertises "ODPROBE" + the low three MAC bytes, with the updater service UUID in the scan
 * response. */
#include "od_tlsr_sdk.h"

#include "od_tlsr_flash.h"
#include "od_tlsr_layout.h"
#include "od_tlsr_mac.h"
#include "od_tlsr_ota.h"

#include "opendisplay_structs.h"

#include <string.h>

#define PROBE_MTU 247u

enum {
    H_GAP_PS = 1, H_GAP_NAME_CD, H_GAP_NAME, H_GAP_APPEAR_CD, H_GAP_APPEAR,
    H_GATT_PS, H_GATT_SC_CD, H_GATT_SC, H_GATT_SC_CCC,
    H_OTA_PS, H_OTA_CD, H_OTA_VAL, H_OTA_CCC,
    H_END
};

static const u16 uuid_primary = GATT_UUID_PRIMARY_SERVICE;
static const u16 uuid_char = GATT_UUID_CHARACTER;
static const u16 uuid_ccc = GATT_UUID_CLIENT_CHAR_CFG;
static const u16 uuid_gap = SERVICE_UUID_GENERIC_ACCESS;
static const u16 uuid_gatt = SERVICE_UUID_GENERIC_ATTRIBUTE;
static const u16 uuid_name = GATT_UUID_DEVICE_NAME;
static const u16 uuid_appear = GATT_UUID_APPEARANCE;
static const u16 uuid_sc = GATT_UUID_SERVICE_CHANGE;
static const u16 appearance = GAP_APPEARE_UNKNOWN;

/* 6f64f000-1e1e-4c4b-8b5a-7454534c5200 / ...f001..., little-endian on the wire */
static const u8 uuid_ota_svc[16] = { 0x00, 0x52, 0x4c, 0x53, 0x54, 0x74, 0x5a, 0x8b,
                                     0x4b, 0x4c, 0x1e, 0x1e, 0x00, 0xf0, 0x64, 0x6f };
static const u8 uuid_ota_chr[16] = { 0x00, 0x52, 0x4c, 0x53, 0x54, 0x74, 0x5a, 0x8b,
                                     0x4b, 0x4c, 0x1e, 0x1e, 0x01, 0xf0, 0x64, 0x6f };

static const u8 cd_name[5] = { CHAR_PROP_READ, U16_LO(H_GAP_NAME), U16_HI(H_GAP_NAME),
                               U16_LO(GATT_UUID_DEVICE_NAME), U16_HI(GATT_UUID_DEVICE_NAME) };
static const u8 cd_appear[5] = { CHAR_PROP_READ, U16_LO(H_GAP_APPEAR), U16_HI(H_GAP_APPEAR),
                                 U16_LO(GATT_UUID_APPEARANCE), U16_HI(GATT_UUID_APPEARANCE) };
static const u8 cd_sc[5] = { CHAR_PROP_INDICATE, U16_LO(H_GATT_SC), U16_HI(H_GATT_SC),
                             U16_LO(GATT_UUID_SERVICE_CHANGE), U16_HI(GATT_UUID_SERVICE_CHANGE) };
static const u8 cd_ota[19] = { CHAR_PROP_WRITE | CHAR_PROP_NOTIFY, U16_LO(H_OTA_VAL), U16_HI(H_OTA_VAL),
                               0x00, 0x52, 0x4c, 0x53, 0x54, 0x74, 0x5a, 0x8b,
                               0x4b, 0x4c, 0x1e, 0x1e, 0x01, 0xf0, 0x64, 0x6f };

#if OD_TLSR_PMTEST
static u8 s_name[13] = { 'O', 'D', 'P', 'M', 'T', 'S', 'T' };
#else
static u8 s_name[13] = { 'O', 'D', 'P', 'R', 'O', 'B', 'E' };
#endif
static u16 s_sc_val[2];
static u8 s_sc_ccc[2];
static u8 s_ota_val[1];
static u8 s_ota_ccc[2];

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

    { H_OTA_PS - H_GATT_PS, ATT_PERMISSIONS_READ, 2, 2, (u8 *)&uuid_primary, (u8 *)&uuid_gatt, 0, 0 },
    { 0, ATT_PERMISSIONS_READ, 2, sizeof cd_sc, (u8 *)&uuid_char, (u8 *)cd_sc, 0, 0 },
    { 0, ATT_PERMISSIONS_READ, 2, sizeof s_sc_val, (u8 *)&uuid_sc, (u8 *)s_sc_val, 0, 0 },
    { 0, ATT_PERMISSIONS_RDWR, 2, sizeof s_sc_ccc, (u8 *)&uuid_ccc, s_sc_ccc, 0, 0 },

    { H_END - H_OTA_PS, ATT_PERMISSIONS_READ, 2, 16, (u8 *)&uuid_primary, (u8 *)uuid_ota_svc, 0, 0 },
    { 0, ATT_PERMISSIONS_READ, 2, sizeof cd_ota, (u8 *)&uuid_char, (u8 *)cd_ota, 0, 0 },
    { 0, ATT_PERMISSIONS_RDWR, 16, sizeof s_ota_val, (u8 *)uuid_ota_chr, s_ota_val, ota_write_cb, 0 },
    { 0, ATT_PERMISSIONS_RDWR, 2, sizeof s_ota_ccc, (u8 *)&uuid_ccc, s_ota_ccc, 0, 0 },
};
OD_STATIC_ASSERT(sizeof s_att / sizeof s_att[0] == H_END, "one table entry per handle");

/* Written by stack callbacks inside blt_sdk_main_loop(), on the main thread. */
static struct {
    bool     connected;
    bool     closed_pending;
    uint8_t  reply[OD_TLSR_OTA_REPLY_MAX];
    uint8_t  reply_len;
    uint32_t last_tick;
    uint32_t ms;
    uint32_t sub;
} s;

static u8 s_adv[31];
static u8 s_scan_rsp[18];

static int ota_write_cb(void *p)
{
    rf_packet_att_write_t *req = (rf_packet_att_write_t *)p;
    uint8_t n = od_tlsr_ota_write(&req->value, (uint16_t)(req->l2capLen - 3u), s.reply);

    if (n != 0u) {
        s.reply_len = n;
    }
    return 0;
}

static void on_connect(u8 e, u8 *p, int n)
{
    (void)e; (void)p; (void)n;
    s.connected = true;
    s_ota_ccc[0] = s_ota_ccc[1] = 0u;
    bls_l2cap_requestConnParamUpdate(8, 24, 0, 600);
}

static void on_terminate(u8 e, u8 *p, int n)
{
    (void)e; (void)p; (void)n;
    s.connected = false;
    s.closed_pending = true;
}

void od_tlsr_ota_app_notify(const uint8_t *reply, uint8_t len)
{
    if (len <= sizeof s.reply) {
        memcpy(s.reply, reply, len);
        s.reply_len = len;
    }
}

void od_tlsr_ota_app_before_install(void)
{
    wd_clear();
}

/* The probe does not track why it booted. */
void od_tlsr_ota_app_boot_info(uint8_t info[2])
{
    info[0] = 0xFFu;
    info[1] = 0xFFu;
}

/* The probe has no SEC2: its updater is the bring-up way in and stays open. */
bool od_tlsr_ota_app_needs_unlock(void)
{
    return false;
}

/* Milliseconds from the 16 MHz system tick, carried past the tick's 268 s wrap. */
static uint32_t now_ms(void)
{
    uint32_t t = clock_time();

    s.sub += t - s.last_tick;
    s.last_tick = t;
    s.ms += s.sub / (1000u * sys_tick_per_us);
    s.sub %= 1000u * sys_tick_per_us;
    return s.ms;
}

#if OD_TLSR_PMTEST
/* Power-floor test (-DOD_TLSR_PMTEST=ON; never a release image). Every boot runs:
 *   A  PMTEST_A_MS of connectable advertising at 100 ms with suspend between events: the
 *      firmware's idle state, and the window in which this image accepts the way back;
 *   B  radio off, SUSPEND_MODE on a timer for PMTEST_B_MS: the suspend floor alone;
 *   C  radio off, DEEPSLEEP_MODE_RET_SRAM_LOW32K on a timer, PMTEST_C_WAKES wakes: the
 *      retention floor;
 *   D  DEEPSLEEP_MODE (no retention) for PMTEST_D_MS, whose wake is a cold boot into A.
 * A fault in B..D therefore ends in a reset and lands in A again. Phase A waits while a link
 * is up and for PMTEST_LINGER_MS after one closes, so an updater session is never cut. The
 * panel rail pin is held off with a pull-down (analog, so it holds through deep sleep). */
#define PMTEST_A_MS       30000u
#define PMTEST_LINGER_MS  15000u
#define PMTEST_B_MS       15000u
#define PMTEST_C_WAKES    4u
#define PMTEST_STEP_TICKS (4u * 1000u * 1000u * 16u)   /* 4 s of the 16 MHz system timer */
#define PMTEST_D_TICKS    (10u * 1000u * 1000u * 16u)
#ifndef PMTEST_RAIL_PIN
#define PMTEST_RAIL_PIN   GPIO_PB7
#endif

_attribute_data_retention_ static uint8_t s_ret_wakes;
static uint32_t s_closed_at;

static void on_suspend_exit(u8 e, u8 *p, int n)
{
    (void)e; (void)p; (void)n;
    rf_set_power_level_index(RF_POWER_P3dBm);
}

static void pmtest_rail_off(void)
{
    gpio_set_func(PMTEST_RAIL_PIN, AS_GPIO);
    gpio_setup_up_down_resistor(PMTEST_RAIL_PIN, PM_PIN_PULLDOWN_100K);
    gpio_write(PMTEST_RAIL_PIN, 0);
    gpio_set_output_en(PMTEST_RAIL_PIN, 1);
    gpio_set_input_en(PMTEST_RAIL_PIN, 0);
}

static void pmtest_deep_ret(void)
{
    cpu_sleep_wakeup(DEEPSLEEP_MODE_RET_SRAM_LOW32K, PM_WAKEUP_TIMER,
                     clock_time() + PMTEST_STEP_TICKS);
}

/* Phases B, C and D; never returns. */
static void pmtest_sleep_phases(void)
{
    uint32_t slept = 0u;

    bls_ll_setAdvEnable(BLC_ADV_DISABLE);
    irq_disable();
    wd_stop();
    while (slept < PMTEST_B_MS) {
        cpu_sleep_wakeup(SUSPEND_MODE, PM_WAKEUP_TIMER, clock_time() + PMTEST_STEP_TICKS / 4u);
        slept += 1000u;
    }
    s_ret_wakes = PMTEST_C_WAKES;
    pmtest_deep_ret();
    for (;;) {
    }
}

/* A wake from phase C re-enters main() with retention RAM intact. */
static void pmtest_retention_wake(void)
{
    if (s_ret_wakes > 1u) {
        s_ret_wakes--;
        pmtest_deep_ret();
    }
    s_ret_wakes = 0u;
    cpu_sleep_wakeup(DEEPSLEEP_MODE, PM_WAKEUP_TIMER, clock_time() + PMTEST_D_TICKS);
    for (;;) {
    }
}
#endif

static void ble_init(void)
{
    static const char hex[] = "0123456789ABCDEF";
    u8 mac[6];
    unsigned k, n = 0u;

    od_tlsr_mac_read(mac);
    for (k = 0u; k < 3u; ++k) {
        s_name[7u + 2u * k] = (u8)hex[mac[2u - k] >> 4];
        s_name[8u + 2u * k] = (u8)hex[mac[2u - k] & 0x0Fu];
    }
    s_adv[n++] = 2;
    s_adv[n++] = DT_FLAGS;
    s_adv[n++] = 0x06;
    s_adv[n++] = (u8)(1u + sizeof s_name);
    s_adv[n++] = DT_COMPLETE_LOCAL_NAME;
    memcpy(&s_adv[n], s_name, sizeof s_name);
    n += sizeof s_name;
    s_scan_rsp[0] = 17;
    s_scan_rsp[1] = DT_COMPLETE_LIST_128BIT_SERVICE_UUID;
    memcpy(&s_scan_rsp[2], uuid_ota_svc, 16u);

    blc_ll_initBasicMCU();
    blc_ll_initStandby_module(mac);
    blc_ll_initAdvertising_module(mac);
    blc_ll_initConnection_module();
    blc_ll_initSlaveRole_module();
    blc_gap_peripheral_init();
    blc_l2cap_register_handler(blc_l2cap_packet_receive);
    bls_att_setAttributeTable((u8 *)s_att);
    blc_att_setRxMtuSize(PROBE_MTU);
    blc_smp_setSecurityLevel(No_Security);

    bls_ll_setAdvParam(ADV_INTERVAL_100MS, ADV_INTERVAL_100MS + 16, ADV_TYPE_CONNECTABLE_UNDIRECTED,
                       OWN_ADDRESS_PUBLIC, 0, NULL, BLT_ENABLE_ADV_ALL, ADV_FP_NONE);
    bls_ll_setAdvData(s_adv, (u8)n);
    bls_ll_setScanRspData(s_scan_rsp, sizeof s_scan_rsp);
    bls_ll_setAdvEnable(BLC_ADV_ENABLE);
    rf_set_power_level_index(RF_POWER_P3dBm);

    bls_app_registerEventCallback(BLT_EV_FLAG_CONNECT, &on_connect);
    bls_app_registerEventCallback(BLT_EV_FLAG_TERMINATE, &on_terminate);
#if OD_TLSR_PMTEST
    bls_app_registerEventCallback(BLT_EV_FLAG_SUSPEND_EXIT, &on_suspend_exit);
    blc_ll_initPowerManagement_module();
    bls_pm_setSuspendMask(SUSPEND_ADV | SUSPEND_CONN);
#else
    bls_pm_setSuspendMask(SUSPEND_DISABLE);
#endif
    blc_app_checkControllerHostInitialization();
}

static void flush_reply(void)
{
    if (s.reply_len == 0u) {
        return;
    }
    if (!s.connected || (s_ota_ccc[0] & 0x01u) == 0u) {
        s.reply_len = 0u;
        return;
    }
    if (blc_gatt_pushHandleValueNotify(BLS_CONN_HANDLE, H_OTA_VAL, s.reply, s.reply_len) ==
        BLE_SUCCESS) {
        s.reply_len = 0u;
    }
}

_attribute_ram_code_ void irq_handler(void)
{
    irq_blt_sdk_handler();
}

_attribute_ram_code_ int main(void)
{
    blc_pm_select_internal_32k_crystal();
    cpu_wakeup_init();
#if OD_TLSR_PMTEST
    pmtest_rail_off();
    if (pm_is_MCU_deepRetentionWakeup()) {
        pmtest_retention_wake();
    }
#endif
    rf_drv_ble_init();
    gpio_init(1);
    clock_init(SYS_CLK_TYPE);
    wd_set_interval_ms(WATCHDOG_INIT_TIMEOUT, CLOCK_SYS_CLOCK_1MS);
    wd_start();

    random_generator_init();
    blc_readFlashSize_autoConfigCustomFlashSector();
    blc_app_loadCustomizedParameters_normal();
    flash_protection_init();
    od_tlsr_ota_reset();
    s.last_tick = clock_time();
    ble_init();
    irq_enable();

    for (;;) {
        wd_clear();
        blt_sdk_main_loop();
        if (s.closed_pending) {
            s.closed_pending = false;
            s.reply_len = 0u;
            od_tlsr_ota_link_closed();
        }
        od_tlsr_ota_service(now_ms());
        flush_reply();
#if OD_TLSR_PMTEST
        if (s.connected) {
            s_closed_at = now_ms();
        } else if (now_ms() >= PMTEST_A_MS && now_ms() - s_closed_at >= PMTEST_LINGER_MS &&
                   s.reply_len == 0u) {
            pmtest_sleep_phases();
        }
#endif
    }
}
