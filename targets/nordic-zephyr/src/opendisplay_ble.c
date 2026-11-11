#include "od_sec2.h"
#include "opendisplay_ble.h"
#include "opendisplay_idle_wake.h"
#include "od_log.h"
#include "od_caps.h"
#if OD_CAP_IMGSTORE
#include "od_cmd_flash.h"
#endif
#include "opendisplay_config_parser.h"
#include "opendisplay_config_storage.h"
#include "opendisplay_cs.h"
#include "opendisplay_constants.h"
#include "opendisplay_display.h"
#include "opendisplay_button.h"
#include "opendisplay_led.h"
#include "opendisplay_touch.h"
#include "opendisplay_buzzer.h"
#include "opendisplay_pipe.h"
#include "od_claim_mode.h"
#include "od_sec2.h"
#include "factory_config.h"
#include "opendisplay_battery.h"
#include "od_sensor_sht40.h"
#include "od_sensor_bq27220.h"
#include "opendisplay_sensor_npm1300.h"
#include "opendisplay_nfc.h"
#include "od_board.h"
#include "od_advert.h"
#include "od_adv_app.h"

#include <stdio.h>
#include <string.h>

#include "od_rxq.h"
#include "od_hal_adv.h"
#include "od_adv_control.h"
#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/gap.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/hci.h>
#include <zephyr/bluetooth/hci_vs.h>
#include <zephyr/bluetooth/uuid.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/drivers/retained_mem.h>
#include <zephyr/drivers/sensor.h>
#include <zephyr/kernel.h>   /* also CMSIS, for NVIC_SystemReset() */
#include <zephyr/settings/settings.h>
#if defined(CONFIG_MCUMGR_TRANSPORT_BT_DYNAMIC_SVC_REGISTRATION)
#include <zephyr/mgmt/mcumgr/transport/smp_bt.h>
#endif
#include <errno.h>

#define MSD_PAYLOAD_LEN        OD_ADVERT_MSD_LEN
#define OD_NAME_PREFIX         "OD"
/* Set by zephyr/CMakeLists.txt, which explains why this is not a Kconfig. Mirrored here so the
 * file still compiles if it is ever built outside that CMake. */
#ifndef OD_TX_POWER_DBM
#define OD_TX_POWER_DBM 8
#endif
#ifndef OD_FW_VERSION
#include "od_fw_version.h"
#define OD_FW_VERSION OD_FW_VERSION_DEFAULT
#endif

static const char *fw_build_version_string(void)
{
	const char *v = OD_FW_VERSION;

	if (v == NULL || v[0] == '\0') {
		return NULL;
	}
	/* CMake may pass a quoted string that was stringified again. */
	if (v[0] == '"') {
		v++;
	}
	if (v[0] == '\0') {
		return NULL;
	}
	return v;
}

#ifndef OD_APP_VERSION
#define OD_APP_VERSION         0x0300u
#endif

static uint8_t fw_major_from_build_version(void)
{
	const char *v = fw_build_version_string();

	if (v == NULL) {
		return 0;
	}

	while (*v == ' ' || *v == 'v' || *v == 'V') {
		v++;
	}
	if (*v < '0' || *v > '9') {
		return 0;
	}
	unsigned maj = 0U;

	while (*v >= '0' && *v <= '9') {
		maj = maj * 10U + (unsigned)(*v - '0');
		v++;
	}
	if (maj > 255U) {
		maj = 255U;
	}
	return (uint8_t)maj;
}

static uint8_t fw_minor_from_build_version(void)
{
	const char *v = fw_build_version_string();

	if (v == NULL) {
		return 0;
	}

	while (*v == ' ' || *v == 'v' || *v == 'V') {
		v++;
	}
	while (*v >= '0' && *v <= '9') {
		v++;
	}
	if (*v != '.') {
		return 0;
	}
	v++;
	if (*v < '0' || *v > '9') {
		return 0;
	}
	unsigned min = 0U;

	while (*v >= '0' && *v <= '9') {
		min = min * 10U + (unsigned)(*v - '0');
		v++;
	}
	if (min > 255U) {
		min = 255U;
	}
	return (uint8_t)min;
}

static uint8_t fw_patch_from_build_version(void)
{
	const char *v = fw_build_version_string();

	if (v == NULL) {
		return 0;
	}

	while (*v == ' ' || *v == 'v' || *v == 'V') {
		v++;
	}
	while (*v >= '0' && *v <= '9') {
		v++;
	}
	if (*v != '.') {
		return 0;
	}
	v++;
	while (*v >= '0' && *v <= '9') {
		v++;
	}
	if (*v != '.') {
		return 0;
	}
	v++;
	if (*v < '0' || *v > '9') {
		return 0;
	}
	unsigned patch = 0U;

	while (*v >= '0' && *v <= '9') {
		patch = patch * 10U + (unsigned)(*v - '0');
		v++;
	}
	if (patch > 255U) {
		patch = 255U;
	}
	return (uint8_t)patch;
}

/* Steady advertising: fixed ~1000 ms (SoftDevice often sticks to interval_min
 * when given a window — the old 160–1000 ms range showed as continuous 160 ms).
 * Matches Firmware_NRF APP_ADV_INTERVAL. Boost still 20–30 ms for 3 s after
 * button/touch for faster rediscovery. */
#define OD_ADV_INTERVAL_MIN       BT_GAP_ADV_SLOW_INT_MIN /* 1600 = 1000 ms */
#define OD_ADV_INTERVAL_MAX       BT_GAP_ADV_SLOW_INT_MIN /* 1600 = 1000 ms */
#define OD_ADV_BOOST_INTERVAL_MIN 32u   /* 20 ms */
#define OD_ADV_BOOST_INTERVAL_MAX 48u   /* 30 ms */
#define OD_ADV_BOOST_MS           3000u

static struct od_config s_od_global_config;
static uint8_t msd_payload[MSD_PAYLOAD_LEN];
static uint8_t dynamic_return[OD_ADVERT_DYNAMIC_LEN];
static char s_dev_name[16];
static struct bt_conn *s_conn;
static bool s_notify_enabled;
static uint32_t s_adv_boost_start_ms;
static bool s_adv_boost_on;
static uint8_t s_msd_loop_counter;
static uint8_t s_reboot_flag = 1; /* set after boot, cleared on first BLE connect */
/* MSD status bit 2 (OD_MSD_STATUS_CONNECTION_REQUESTED). Nothing writes this; it is read once,
 * when the advertisement is built, so the bit always goes out as 0. Dormant the same way on the
 * other two targets -- ESP32's connectionRequested and BG22's equivalent are also read and never
 * written. Kept as the field's placeholder rather than hardcoding false at the read. There is no
 * setter: wire one up on every target together, not here alone. */
static uint8_t s_connection_requested;
static bool s_config_changed;        /* od_claim_app_config_changed(): reload from the loop */
static bool s_unpair_pending;        /* factory reset R5b, before bt_enable() */
static bool s_boot_screen_deferred;  /* od_claim_mode_defer_boot_screen() held it back */
static struct bt_le_adv_param s_adv_param = BT_LE_ADV_PARAM_INIT(
	BT_LE_ADV_OPT_CONN,
	OD_ADV_INTERVAL_MIN, OD_ADV_INTERVAL_MAX, NULL);

static struct k_work_delayable s_dfu_work;
static struct k_work s_boot_display_work;
static struct k_work_q s_display_work_q;
static K_THREAD_STACK_DEFINE(s_display_wq_stack, 8192);
static bool s_display_wq_started;
/* Gives once the boot-display work item returns, success or failure. opendisplay_ble_init()
 * blocks on this before od_adv_request_start() -- see its call site for why. */
static struct k_sem s_boot_display_done;
/* When encryption is on, SMP stays hidden until CMD_ENTER_DFU unlocks it
 * for this boot (Adafruit bledfu.begin() gating). */
static bool s_ota_unlocked;

/* The facts a callback may publish about advertising/MSD: edges, drained on the loop thread. */
static volatile uint8_t s_adv_ended_pending = 0;
static volatile uint8_t s_msd_publish_pending = 0;
/* SDC applies TX power to the advertising role and can change it while advertising is running.
 * Persistence across stop/start is an assumption for the hardware RSSI gate to verify. Apply only
 * where the configured or controller-held value can have changed: boot, disconnect, and config
 * reload. Set from the BT RX thread and consumed on the loop thread. */
static volatile uint8_t s_adv_txp_pending = 1u;

/* ---- advertising ownership (F4): loop-thread-only state, touched from exactly one context
 * (opendisplay_ble_process(), called only from main.c's superloop). Nothing below this point
 * may be touched from BT_CONN_CB_DEFINE callbacks (BT RX thread) or from the NFC driver's
 * callback (HAL_NFC/ISR context) -- see od_adv_app_boost() for why that
 * constraint is load-bearing here and not just a style preference. */
static struct od_adv_control s_adv;
static bool s_adv_interval_is_boost;   /* which interval was last SUCCESSFULLY applied */
static bool s_adv_restart_pending;     /* see request_adv_restart() */

/* Separate from msd_payload[]: this is the ONLY buffer ad[] points at, and it is written
 * exclusively by od_hal_adv_program(), which the shared controller calls only from the loop
 * thread and only while advertising is inactive. msd_payload[] remains the free-running staging
 * buffer update_msd_payload() writes into from any context (unchanged, pre-existing behavior) --
 * decoupling the two means the live-on-air bytes can never be torn by a concurrent write. */
static uint8_t s_programmed_msd[MSD_PAYLOAD_LEN];

BUILD_ASSERT(MSD_PAYLOAD_LEN == OD_ADV_MSD_LEN,
	     "opendisplay_ble.c and od_adv_control.h disagree on the MSD length");

static void apply_tx_power(uint8_t handle_type, uint16_t handle);
static void od_smp_sync(void);
static void request_fast_link(struct bt_conn *conn);
static void compute_device_name(void);
static bool apply_adv_interval(void);
static uint8_t connected_count(void);
static bool ble_service_advertising(bool start_allowed);
static void request_adv_restart(void);

/* Mirror Adafruit ble_nrf_request_fast_link(): ask for 2M + max DLE; central may decline. */
static void request_fast_link(struct bt_conn *conn)
{
	int err;

	if (conn == NULL) {
		return;
	}

#if defined(CONFIG_BT_CTLR_PHY_2M)
	err = bt_conn_le_phy_update(conn, BT_CONN_LE_PHY_PARAM_2M);
	if (err != 0) {
		od_log_info("PHY 2M request failed: %d", err);
	}
#endif

	err = bt_conn_le_data_len_update(conn, BT_LE_DATA_LEN_PARAM_MAX);
	if (err != 0) {
		od_log_info("DLE max request failed: %d", err);
	}

	err = bt_conn_le_param_update(conn, BT_LE_CONN_PARAM(6, 12, 0, 400));
	if (err != 0) {
		od_log_info("conn param update failed: %d", err);
	}
}

static void od_smp_sync(void)
{
#if defined(CONFIG_MCUMGR_TRANSPORT_BT_DYNAMIC_SVC_REGISTRATION)
	/* smp_bt_setup() already registers the SVC at boot when dynamic reg is on. */
	static bool s_smp_visible = true;
	/* SECURITY_V2 6.1 / 7.4: hidden on a claimed or LOCKED device until ENTER_DFU, and always
	 * in claim mode. From the security state, never from the 0x27 bytes at rest (10.2); the
	 * same predicate as the ESP32 SMP lock (od_sec2_dfu_needs_unlock). */
	const bool want = s_ota_unlocked || !od_sec2_dfu_needs_unlock();
	int err;

	if (want == s_smp_visible) {
		return;
	}
	if (want) {
		err = smp_bt_register();
		if (err != 0 && err != -EALREADY) {
			od_log_info("SMP register failed: %d", err);
			return;
		}
		s_smp_visible = true;
		od_log_info("SMP DFU service %s",
		       s_ota_unlocked ? "unlocked" : "available (encryption off)");
	} else {
		err = smp_bt_unregister();
		if (err != 0 && err != -ENOENT) {
			od_log_info("SMP unregister failed: %d", err);
			return;
		}
		s_smp_visible = false;
		od_log_info("SMP DFU service hidden (use CMD_ENTER_DFU)");
	}
#else
	ARG_UNUSED(s_ota_unlocked);
#endif
}

/* Reboot into the Adafruit bootloader's DFU mode.
 *
 * ONLY FOR THE COMPOSITION THAT HAS NO MCUboot. Without this, CMD_ENTER_DFU on nRF52840 is a
 * lie: it ACKs, unlocks an SMP service that was compiled out, and nothing can update the
 * device. Measured on an EN04 -- once this firmware is flashed there is NO software path back
 * to the bootloader at all. Zephyr's CDC does not implement the Arduino 1200-baud touch (a BSP
 * convention, verified absent here), and CMD_REBOOT only resets into the same valid
 * application, so a tag in a case was unrecoverable without opening it.
 *
 * GPREGRET1 is the handshake register the Adafruit bootloader reads on reset. od_hal_wdt
 * deliberately keeps its breadcrumb in GPREGRET2 so a strike write cannot cancel a pending DFU
 * request -- see the note there; this is the other half of that arrangement.
 *
 * 0x4E is SERIAL-only DFU, chosen over 0xA8 (BLE OTA) deliberately. This bootloader exposes CDC
 * and no mass storage, so serial DFU is its only USB path, and a unit that enters BLE OTA and
 * is never updated waits in DFU until someone physically resets it. Serial recovery needs a
 * cable but cannot strand a device that is merely out of radio range.
 */
#if !defined(CONFIG_BOOTLOADER_MCUBOOT)
#define OD_ADAFRUIT_DFU_SERIAL_MAGIC 0x4Eu

static void enter_adafruit_dfu(void)
{
	const struct device *gpregret1 = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(gpregret1));
	const uint8_t magic = OD_ADAFRUIT_DFU_SERIAL_MAGIC;

	if (gpregret1 == NULL || !device_is_ready(gpregret1)) {
		od_log_error("ENTER_DFU: gpregret1 unavailable; cannot reach the bootloader");
		return;
	}
	if (retained_mem_write(gpregret1, 0, &magic, sizeof magic) != 0) {
		od_log_error("ENTER_DFU: gpregret1 write failed; not resetting");
		return;
	}
	od_log_info("ENTER_DFU: rebooting into the Adafruit serial bootloader");
	/* Let the log drain and the ACK reach the host before the reset takes the link down. */
	k_sleep(K_MSEC(250));
	/* NVIC_SystemReset, not sys_reboot: od_cmd_app_reboot already resets this way, and
	 * sys_reboot would need CONFIG_REBOOT which this build does not set. */
	NVIC_SystemReset();
}
#endif

static void dfu_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	s_ota_unlocked = true;
	od_smp_sync();
	if (s_conn != NULL) {
		(void)bt_conn_disconnect(s_conn, BT_HCI_ERR_REMOTE_USER_TERM_CONN);
	}
#if !defined(CONFIG_BOOTLOADER_MCUBOOT)
	/* After the disconnect, so the host sees the link drop rather than a silent stall. */
	(void)od_claim_mode_before_reboot();
	enter_adafruit_dfu();
#endif
}

static void boot_display_work_handler(struct k_work *work)
{
	ARG_UNUSED(work);
	if (!opendisplay_display_boot_apply()) {
		od_log_error("boot display failed after bounded retry");
	}
	/* Given on both the success and failure arms above: the waiter in opendisplay_ble_init()
	 * cares that the panel is no longer being touched, not that the render succeeded. */
	k_sem_give(&s_boot_display_done);
}

/* Deliberately shorter than opendisplay_display_boot_apply()'s worst legitimate case (two
 * attempts of ~900 ms rail settle + a 60 s wait_for_refresh() each, ~125 s): BLE reachability
 * within 30 s wins over waiting out a slow-but-healthy render. A render still in flight past
 * this point keeps running on s_display_work_q after advertising opens -- the race the wait
 * exists to close (see the call site) is narrowed, not eliminated, for that tail case. */
#define OD_BOOT_DISPLAY_WAIT_MS 30000u

static void schedule_boot_display_apply(void)
{
	if (!s_display_wq_started) {
		k_sem_init(&s_boot_display_done, 0, 1);
		k_work_queue_init(&s_display_work_q);
		k_work_queue_start(&s_display_work_q, s_display_wq_stack,
				   K_THREAD_STACK_SIZEOF(s_display_wq_stack), 14, NULL);
		s_display_wq_started = true;
	}
	(void)k_work_submit_to_queue(&s_display_work_q, &s_boot_display_work);
}

uint32_t opendisplay_ble_chip_id_last24(void)
{
	uint8_t id[8];
	uint64_t uid = 0;

	(void)hwinfo_get_device_id(id, sizeof(id));
	for (unsigned i = 0; i < sizeof(id); i++) {
		uid = (uid << 8) | id[i];
	}
	/*
	 * THIS IS THE DISPLAY/ADVERTISING IDENTITY ONLY. The session-auth device_id is a
	 * DIFFERENT FICR word by contract -- getAuthDeviceIdBytes() uses DEVICEID[0] while
	 * getChipIdHex() uses DEVICEID[1] -- so opendisplay_pipe.c derives its own and must
	 * not be routed through here. Aligning them would change every derived session key.
	 *
	 * WHICH FICR WORD THIS TAKES IS A COMPATIBILITY CONTRACT, NOT A DETAIL.
	 *
	 * Zephyr's nRF hwinfo driver returns be32(DEVICEID[1]) || be32(DEVICEID[0])
	 * (zephyr/drivers/hwinfo/hwinfo_nrf.c) -- the two words are SWAPPED relative to the
	 * register order. Accumulating big-endian therefore yields
	 *     uid = (DEVICEID[1] << 32) | DEVICEID[0]
	 * so a plain `uid & 0xFFFFFF` is the low 3 bytes of DEVICEID[0].
	 *
	 * The Arduino nRF52 firmware names the device from DEVICEID[**1**] & 0xFFFFFF
	 * (Firmware/src/encryption.cpp getChipIdHex). So on the nRF52840 the migrated
	 * firmware advertised a DIFFERENT OD<id> than the same physical board did under the
	 * old firmware -- reported from hardware as "OD address is incorrect/different".
	 *
	 * Fixed only for the nRF52840, deliberately. The nRF54 boards have never had an
	 * Arduino firmware to agree with, and their current names are already deployed;
	 * "correcting" them here would rename every field unit for no benefit.
	 */
#if defined(OD_BOARD_XIAO_NRF52840)
	uid >>= 32; /* DEVICEID[1] -- match the Arduino nRF52 firmware. */
#endif
	return (uint32_t)(uid & 0xFFFFFFu);
}

/* Every consumer of the advertised identity goes through opendisplay_ble_chip_id_last24():
 * the boot screen used to re-derive it and omitted the board conditional above, so an
 * nRF52840 printed one OD<id> on screen and advertised another. */
static void chip_id_hex6(char out[7])
{
	snprintf(out, 7, "%06lX", (unsigned long)opendisplay_ble_chip_id_last24());
}

static float s_chip_temperature = -999.0f;

/*
 * With CONFIG_TEMP_NRF5_MPSL (SoftDevice builds) sample_fetch uses
 * mpsl_temperature_get() and is safe after bt_enable(). Legacy Zephyr temp
 * drivers block on DATARDY IRQ; keep this call after the stack is up.
 */
static void read_chip_temperature(void)
{
#if DT_HAS_COMPAT_STATUS_OKAY(nordic_nrf_temp)
	const struct device *temp_dev = DEVICE_DT_GET(DT_NODELABEL(temp));
	struct sensor_value val;

	if (device_is_ready(temp_dev) && sensor_sample_fetch(temp_dev) == 0 &&
	    sensor_channel_get(temp_dev, SENSOR_CHAN_DIE_TEMP, &val) == 0) {
		s_chip_temperature = (float)sensor_value_to_double(&val);
	}
#endif
}

static void update_msd_payload(void)
{
	struct od_advert_inputs adv;
	uint16_t battery_voltage_10mv;

	/* Mirror the reference updatemsdata() ordering: refresh the sensor
	 * dynamic slots, then the battery source (BQ27220-preferred, else SAADC),
	 * before packing the frame. All three are TTL-cached (30 s). */
	od_sensor_sht40_poll(opendisplay_get_global_config(), k_uptime_get_32());
	od_sensor_bq27220_poll(opendisplay_get_global_config(), k_uptime_get_32());
	opendisplay_sensor_npm1300_poll();
	battery_voltage_10mv = opendisplay_battery_get_10mv();
	/* Reference updatemsdata() reads the die temperature per publish; every caller
	 * of this function runs after bt_enable(). */
	read_chip_temperature();

	/* Encoding -- company id, the (t + 40) * 2 temperature step, the 10-bit battery
	 * split across battery_voltage_low and status bit0, the status bit positions --
	 * belongs to shared/core/od_advert.c. What stays here is acquisition: the polls
	 * above, and a battery module that already reports the wire's 10 mV units. */
	memset(&adv, 0, sizeof(adv));
	adv.dynamic = dynamic_return;
	adv.chip_temperature_c = s_chip_temperature;
	adv.battery_10mv = battery_voltage_10mv;
	adv.reboot_flag = (s_reboot_flag != 0u);
	adv.connection_requested = (s_connection_requested != 0u);
	adv.loop_counter = s_msd_loop_counter;
	/* Status bits 2 (CLAIM_MODE) and 3 (SESSION_REQUIRED): SECURITY_V2 5.3, shared. */
	{
		struct od_advert_device_state st;

		od_advert_device_state_now(&st);
		od_advert_apply_device_state(&adv, &st);
	}
	od_advert_build(&adv, msd_payload);

	s_msd_loop_counter = od_advert_advance_counter(s_msd_loop_counter);
}

static void log_msd(int level, const char *tag)
{
#if defined(OD_LOW_POWER_QUIET)
	ARG_UNUSED(level);
	ARG_UNUSED(tag);
#else
	/* Gate BEFORE formatting, using the constant-fold `if` idiom od_log.h documents rather
	 * than `#if`: the body still compiles at every level, but a build below the caller's
	 * level does no snprintf work. The od_log_*() macros gate inside themselves, so they
	 * cannot elide the argument formatting this helper does -- and this runs on every MSD
	 * publish and every 0x0044 read. */
	if (OD_LOG_LEVEL >= level) {
		char label[32];
		char line[96];

		snprintf(label, sizeof(label), "[OD] msd %s: ", tag);
		od_log_hex_line(line, sizeof(line), label, msd_payload, MSD_PAYLOAD_LEN);
		_od_log(level, "%s", line);
	}
#endif
}

static ssize_t od_gatt_write(struct bt_conn *conn, const struct bt_gatt_attr *attr,
			       const void *buf, uint16_t len, uint16_t offset, uint8_t flags)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(attr);
	ARG_UNUSED(offset);
	if (len == 0) {
		return 0;
	}
	/* ADMISSION AT THE ATT LAYER, matching ESP32. Over-length values are refused with ATT 0x0D
	 * (Invalid Attribute Value Length) rather than accepted and dropped at the queue: a host can
	 * discover the first and cannot discover the second. ATT MTU 256 leaves 253 value bytes after
	 * opcode(1) + handle(2), keeping the 245..253 band
	 * reachable, where the DISPATCHER answers {0xFF,cmd,0xFE} -- so a client learns whether its
	 * frame was too big for the transport or too big for the protocol. */
	if (len > OD_RXQ_VALUE_MAX_BLE) {
		return BT_GATT_ERR(BT_ATT_ERR_INVALID_ATTRIBUTE_LEN);
	}
	opendisplay_pipe_on_write(buf, len, (flags & BT_GATT_WRITE_FLAG_CMD) != 0);
	return len;
}

static void od_ccc_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value)
{
	ARG_UNUSED(attr);
	s_notify_enabled = (value == BT_GATT_CCC_NOTIFY);
	opendisplay_pipe_on_notify_changed(s_notify_enabled);
}

BT_GATT_SERVICE_DEFINE(
	od_svc, BT_GATT_PRIMARY_SERVICE(BT_UUID_DECLARE_16(0x2446)),
	BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_16(0x2446),
			       BT_GATT_CHRC_WRITE | BT_GATT_CHRC_WRITE_WITHOUT_RESP |
				       BT_GATT_CHRC_NOTIFY,
			       BT_GATT_PERM_WRITE, NULL, od_gatt_write, NULL),
	BT_GATT_CCC(od_ccc_cfg_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE), );

/* SECURITY_V2 5.3.3: the name is in the advertising PDU, as on every other target, so passive
 * scanners (and HA's name check) see it: flags 3 + MSD 18 + name 10 = 31 bytes, exactly full.
 * The scan response keeps the 16-bit service UUID and, with Channel Sounding, the Ranging UUID. */
static struct bt_data ad[] = {
	BT_DATA_BYTES(BT_DATA_FLAGS, (BT_LE_AD_GENERAL | BT_LE_AD_NO_BREDR)),
	BT_DATA(BT_DATA_MANUFACTURER_DATA, s_programmed_msd, MSD_PAYLOAD_LEN),
	BT_DATA(BT_DATA_NAME_COMPLETE, s_dev_name, OD_ADVERT_NAME_LEN),
};
BUILD_ASSERT(sizeof(OD_NAME_PREFIX) - 1u + 6u == OD_ADVERT_NAME_LEN,
	     "the advertised name is OD + 6 hex digits");
BUILD_ASSERT(OD_ADVERT_ADV_PDU_LEN <= 31u, "flags + MSD + name exceed a legacy PDU");

static const struct bt_data sd_od_uuid = BT_DATA_BYTES(BT_DATA_UUID16_ALL, 0x46, 0x24);
static struct bt_data sd_cs_uuid;
static struct bt_data sd_buf[2];

static unsigned sd_prepare(void)
{
	unsigned count = 1u;
	unsigned cs_extra = 0u;

	sd_buf[0] = sd_od_uuid;
	opendisplay_cs_fill_scan_response(&s_od_global_config, &sd_cs_uuid, 1u, &cs_extra);
	if (cs_extra != 0u) {
		sd_buf[1] = sd_cs_uuid;
		count = 2u;
	}
	return count;
}

void opendisplay_ble_update_msd(bool refresh_advertising)
{
	update_msd_payload();   /* unchanged: already called from any context in the old code too */
	if (!refresh_advertising) {
		return;
	}
	/* ISR-safe: only sets a flag. od_adv_set_payload() itself -- and its own memcmp-based
	 * dedup against the currently-programmed payload -- runs on the next
	 * ble_service_advertising() pass, never here. Reachable from opendisplay_nfc.c's
	 * nfc_callback(), which nfc_t2t_lib.h documents as running in HAL_NFC callback context. */
	__atomic_store_n(&s_msd_publish_pending, (uint8_t)1, __ATOMIC_RELEASE);
}

static void connected(struct bt_conn *conn, uint8_t err)
{
	/* A connection attempt -- successful or not -- means Zephyr's single legacy-advertiser
	 * instance was engaged by the CONNECT_IND. A non-zero err here is documented for the
	 * peripheral-role directed-high-duty-cycle-advertising-timeout case; publishing "ended"
	 * either way is the conservative, correct choice, mirroring ESP32's
	 * od_ble_note_adv_ended(). */
	__atomic_store_n(&s_adv_ended_pending, (uint8_t)1, __ATOMIC_RELEASE);

	if (err != 0) {
		od_log_info("connect failed: %u", (unsigned)err);
		od_adv_app_boost();
		opendisplay_idle_wake();
		return;
	}
	s_conn = bt_conn_ref(conn);
	s_reboot_flag = 0;
	request_fast_link(conn);
	if (opendisplay_cs_config_enabled(&s_od_global_config)) {
		opendisplay_cs_on_connected(conn);
	}
#if defined(CONFIG_BT_HCI_VS)
	{
		uint16_t conn_handle = 0;

		if (bt_hci_get_conn_handle(conn, &conn_handle) == 0) {
			apply_tx_power(BT_HCI_VS_LL_HANDLE_TYPE_CONN, conn_handle);
		}
	}
#endif
	opendisplay_idle_wake();
}

static void disconnected(struct bt_conn *conn, uint8_t reason)
{
	ARG_UNUSED(conn);
	od_log_info("disconnected reason=%u", (unsigned)reason);
	opendisplay_cs_on_disconnected(conn);
	opendisplay_pipe_on_connection_closed();
	if (s_conn != NULL) {
		bt_conn_unref(s_conn);
		s_conn = NULL;
	}
	/* THE FIX: no restart call here. od_adv_control's `desired` intent was never withdrawn,
	 * and ble_service_advertising() re-reads connected_count() fresh on the very next
	 * opendisplay_ble_process() pass. Replaces the blind 150ms k_work_schedule() that was
	 * the reported bug -- see docs on od_adv_control.h / shared/hal/od_hal_adv.h for the
	 * reconcile-every-pass design this now follows. */
	__atomic_store_n(&s_adv_txp_pending, (uint8_t)1, __ATOMIC_RELEASE);
	od_adv_app_boost();
	opendisplay_idle_wake();
}

/* .recycled is gone -- it was structurally dead code (scheduling an already-pending work item
 * with K_NO_WAIT is a silent Zephyr no-op). Its job is now just connected_count()'s level read
 * every pass; safe because a transient -ENOMEM from bt_le_adv_start() is mapped to RETRY, not
 * ERROR (see od_hal_adv_start() below), so the loop keeps retrying on its own. */
BT_CONN_CB_DEFINE(conn_callbacks) = {
	.connected = connected,
	.disconnected = disconnected,
};

const struct od_config *opendisplay_get_global_config(void)
{
	return &s_od_global_config;
}

uint16_t opendisplay_ble_get_app_version(void)
{
	if (fw_build_version_string() != NULL) {
		return ((uint16_t)fw_major_from_build_version() << 8) |
		       fw_minor_from_build_version();
	}
	return OD_APP_VERSION;
}

uint8_t opendisplay_ble_get_app_version_patch(void)
{
	if (fw_build_version_string() != NULL) {
		return fw_patch_from_build_version();
	}
	return 0;
}

float opendisplay_ble_get_chip_temperature(void)
{
	return s_chip_temperature;
}

void opendisplay_ble_copy_msd_bytes(uint8_t out[16])
{
	/* DEBUG, not INFO: a polling host emits one of these per read. Matches the ESP32,
	 * which logs the read at debug and the publish at info. */
	log_msd(OD_LOG_DEBUG, "read 0x0044");
	memcpy(out, msd_payload, 16);
}

bool opendisplay_ble_pipe_notify(const uint8_t *data, uint16_t len)
{
	if (!s_notify_enabled || s_conn == NULL || len == 0) {
		return false;
	}
	return bt_gatt_notify(s_conn, &od_svc.attrs[2], data, len) == 0;
}

bool opendisplay_ble_pipe_notify_enabled(void)
{
	return s_notify_enabled;
}

void opendisplay_ble_set_dynamic_byte(uint8_t index, uint8_t value)
{
	if (index < sizeof(dynamic_return)) {
		dynamic_return[index] = value;
	}
}

/*
 * Apply the configured TX power (power_option.tx_power, dBm as a signed int8).
 * The reference nRF52840 build calls Bluefruit.setTxPower(power_option.tx_power)
 * once at init (ble_init.cpp:90). On Zephyr with the SoftDevice Controller there
 * is no stable bt_le_* runtime API for this, so use the standard HCI vendor-
 * specific Write_Tx_Power_Level command (as in Zephyr's hci_pwr_ctrl sample):
 * the controller clamps the requested value to its supported set and returns the
 * value it actually selected, which we log. handle_type selects advertising vs a
 * specific connection.
 */
static void apply_tx_power(uint8_t handle_type, uint16_t handle)
{
#if defined(CONFIG_BT_HCI_VS)
	/* Config wins when it states a power; 0 means "unstated" and takes the build default.
	 *
	 * The wire field is one UNSIGNED byte read as signed dBm, so every level the radio supports
	 * stays expressible except exactly 0 dBm -- -8 dBm is 0xF8, not 0. What 0 actually is, in
	 * practice, is the zero-filled value carried by a device whose config omits the field or that
	 * has no stored config at all, and 0 dBm is never what a battery tag on a weak link wants.
	 * Treating it as "unstated" is therefore a reinterpretation of exactly one value, and the
	 * only one where the wire cannot distinguish a request from a default. */
	int8_t configured = (int8_t)s_od_global_config.power_option.tx_power;
	int8_t requested = (configured != 0) ? configured : (int8_t)OD_TX_POWER_DBM;
	struct bt_hci_cp_vs_write_tx_power_level *cp;
	struct bt_hci_rp_vs_write_tx_power_level *rp;
	struct net_buf *buf;
	struct net_buf *rsp = NULL;
	int err;

	buf = bt_hci_cmd_alloc(K_FOREVER);
	if (buf == NULL) {
		od_log_info("tx_power: no HCI cmd buffer");
		return;
	}
	cp = net_buf_add(buf, sizeof(*cp));
	cp->handle = sys_cpu_to_le16(handle);
	cp->handle_type = handle_type;
	cp->tx_power_level = requested;

	err = bt_hci_cmd_send_sync(BT_HCI_OP_VS_WRITE_TX_POWER_LEVEL, buf, &rsp);
	if (err != 0) {
		od_log_info("tx_power set failed (type=%u req=%d dBm): %d",
		       (unsigned)handle_type, (int)requested, err);
		return;
	}
	rp = (struct bt_hci_rp_vs_write_tx_power_level *)rsp->data;
	od_log_info("tx_power type=%u requested=%d selected=%d dBm",
	       (unsigned)handle_type, (int)requested, (int8_t)rp->selected_tx_power);
	net_buf_unref(rsp);
#else
	ARG_UNUSED(handle_type);
	ARG_UNUSED(handle);
	od_log_info("tx_power: CONFIG_BT_HCI_VS disabled; not applied");
#endif
}

/*
 * ELAPSED SINCE START, never a stored deadline. k_uptime_get_32() wraps every 49.7 days, and
 * an absolute `now < deadline` test reads false for a boost that straddles the wrap, ending
 * it the moment it begins. Unsigned subtraction is correct across the wrap. The separate flag
 * carries "boosting at all" so no uptime value has to double as a not-boosting sentinel.
 */
static bool adv_boost_active(uint32_t now)
{
	return s_adv_boost_on && ((now - s_adv_boost_start_ms) < OD_ADV_BOOST_MS);
}

static bool apply_adv_interval(void)
{
	if (adv_boost_active(k_uptime_get_32())) {
		s_adv_param.interval_min = OD_ADV_BOOST_INTERVAL_MIN;
		s_adv_param.interval_max = OD_ADV_BOOST_INTERVAL_MAX;
		return true;
	}
	s_adv_boost_on = false;
	s_adv_param.interval_min = OD_ADV_INTERVAL_MIN;
	s_adv_param.interval_max = OD_ADV_INTERVAL_MAX;
	return false;
}

/* ------------------------------------------------- shared advertising HAL (od_hal_adv.h) ---
 * The target side of shared/core/od_adv_control.c (docs/F4_PORTABLE_BLE_LIFECYCLE_PLAN.md).
 * Lives here, not in a separate hal/od_hal_adv.c, for the same reason
 * targets/esp32-idf/ble/od_ble_nimble.cpp gives: it needs this file's statics (s_adv_param,
 * ad[], sd_buf[], sd_prepare()) and a second home for advertising state would be the defect F4
 * exists to remove, not a cleanup of it.
 */

static void compute_device_name(void)
{
	char hex[7];

	chip_id_hex6(hex);
	snprintf(s_dev_name, sizeof(s_dev_name), "%s%s", OD_NAME_PREFIX, hex);
}

/* Thread-safe LEVEL read of connections, per od_adv_control.h's contract ("from the transport
 * instance table, not counted callback edges") -- NOT s_conn != NULL, which is written by
 * connected()/disconnected() on the BT RX thread with no synchronization against the loop
 * thread reading it. */
static void count_connected_cb(struct bt_conn *conn, void *user_data)
{
	struct bt_conn_info info;
	uint8_t *count = user_data;

	if (bt_conn_get_info(conn, &info) == 0 && info.state == BT_CONN_STATE_CONNECTED) {
		(*count)++;
	}
}

static uint8_t connected_count(void)
{
	uint8_t count = 0;

	bt_conn_foreach(BT_CONN_TYPE_LE, count_connected_cb, &count);
	return count;
}

enum od_hal_adv_result od_hal_adv_program(const uint8_t msd[16])
{
	memcpy(s_programmed_msd, msd, MSD_PAYLOAD_LEN);
	return OD_HAL_ADV_OK;
}

enum od_hal_adv_result od_hal_adv_start(void)
{
	int err;
	unsigned sd_count;
	bool boost;

	compute_device_name();
	sd_count = sd_prepare();
	boost = apply_adv_interval();   /* mutates s_adv_param; commit decision only on success below */

	err = bt_le_adv_start(&s_adv_param, ad, ARRAY_SIZE(ad), sd_buf, sd_count);
	if (err == 0) {
		/* Commit the interval marker ONLY on confirmed success -- an -EALREADY result must
		 * not claim the requested interval is on air when Zephyr never actually re-applied
		 * s_adv_param to an already-running advertiser, or the corrective restart in
		 * opendisplay_ble_advertising_tick() would be silently suppressed. */
		s_adv_interval_is_boost = boost;
		od_log_info("advertising as %s (interval=%u-%u ms)", s_dev_name,
		       (unsigned)BT_GAP_ADV_INTERVAL_TO_MS(s_adv_param.interval_min),
		       (unsigned)BT_GAP_ADV_INTERVAL_TO_MS(s_adv_param.interval_max));
		return OD_HAL_ADV_OK;
	}
	if (err == -EALREADY) {
		return OD_HAL_ADV_ALREADY;
	}
	/* Verified against zephyr/subsys/bluetooth/host/adv.c directly, not assumed by analogy
	 * to NimBLE:
	 *   -ENOMEM       no free bt_conn object (this board has exactly one connection slot)
	 *   -ECONNREFUSED max simultaneous connections already established
	 *   -EAGAIN       BT_DEV_READY not set (unreachable in practice: od_adv_stack_ready() is
	 *                 only asserted after bt_enable() returns, which blocks until ready --
	 *                 mapped anyway rather than asserted against)
	 *   -ENOBUFS      HCI command buffer exhausted before the command was even issued -- no
	 *                 state changed, a textbook RETRY
	 *   -EACCES       HCI "Command Disallowed" -- can occur transiently while the controller
	 *                 is mid-transition around a connection event; treated as retryable
	 *                 rather than a permanent fault */
	if (err == -ENOMEM || err == -ECONNREFUSED || err == -EAGAIN ||
	    err == -ENOBUFS || err == -EACCES) {
		return OD_HAL_ADV_RETRY;
	}
	od_log_info("adv start failed: %d", err);
	return OD_HAL_ADV_ERROR;
}

enum od_hal_adv_result od_hal_adv_stop(void)
{
	int err = bt_le_adv_stop();

	/* Zephyr folds "never started" and "exists but not enabled" into rc==0 -- there is no
	 * -EALREADY-shaped case to map to OD_HAL_ADV_NOT_ACTIVE here, unlike NimBLE. */
	if (err == 0) {
		return OD_HAL_ADV_OK;
	}
	/* -ENOBUFS means advertising was NOT disabled -- RETRY, not a latched fault. */
	if (err == -ENOBUFS) {
		return OD_HAL_ADV_RETRY;
	}
	od_log_info("adv stop failed: %d", err);
	return OD_HAL_ADV_ERROR;
}

/* Loop-thread only, called from opendisplay_ble_process(). Mirrors od_ble_service_advertising()
 * in targets/esp32-idf/ble/od_ble_nimble.cpp: publish facts as level reads (connection count) or
 * drained edges (ended, MSD), then run exactly one reconciliation step. */
static bool ble_service_advertising(bool start_allowed)
{
	od_adv_set_connection_count(&s_adv, connected_count());
	if (__atomic_exchange_n(&s_adv_ended_pending, (uint8_t)0, __ATOMIC_ACQUIRE)) {
		od_adv_observe_ended(&s_adv);
	}
	if (__atomic_exchange_n(&s_msd_publish_pending, (uint8_t)0, __ATOMIC_ACQUIRE)) {
		log_msd(OD_LOG_INFO, "publish");
		od_adv_set_payload(&s_adv, msd_payload);
	}

	const enum od_adv_process_result r = od_adv_process(&s_adv, start_allowed);
	if (s_adv.active && !s_adv.faulted
	    && __atomic_exchange_n(&s_adv_txp_pending, (uint8_t)0, __ATOMIC_ACQUIRE)) {
		apply_tx_power(BT_HCI_VS_LL_HANDLE_TYPE_ADV, 0);
	}

	if (s_adv_restart_pending) {
		if (s_adv.faulted) {
			/* A latched fault needs a stack reset, not more restarts -- stop asking. */
			s_adv_restart_pending = false;
		} else if (!s_adv.active) {
			od_adv_request_start(&s_adv);
			s_adv_restart_pending = false;
		}
		/* else: still active (stop hasn't completed this pass, e.g. a RETRY) -- leave the
		 * flag set and let the next pass's od_adv_process() call finish the stop, one step
		 * at a time, exactly as RETRY's contract requires. */
	}
	return r == OD_ADV_ACTED;
}

/* Withdraws intent (forcing a stop), and remembers to reassert it once the controller confirms
 * inactive. Used wherever the OLD code forced a stop/start cycle to pick up changed target
 * parameters (interval, config) that od_adv_control itself has no concept of -- it only owns
 * desired/payload/connection-count, never s_adv_param. Loop-thread only. */
static void request_adv_restart(void)
{
	od_adv_request_stop(&s_adv);
	s_adv_restart_pending = true;
}

/* Put a configured external SPI NOR flash into deep power-down after every
 * config load (matches nRF52840 Firmware powerDownExternalFlashFromConfig). */
static void flash_powerdown_from_config(void)
{
	if (!s_od_global_config.loaded) {
		return;
	}
	for (uint8_t i = 0; i < s_od_global_config.flash_config_count; i++) {
		const struct FlashConfig *fc = &s_od_global_config.flash_configs[i];

		if ((fc->flags & OD_FLASH_FLAG_ENABLED) == 0u) {
			continue;
		}
		if (fc->mosi_pin == 0xFFu || fc->sck_pin == 0xFFu || fc->cs_pin == 0xFFu) {
			continue;
		}
		/* MISO / WP / HOLD are reserved[0..2] in the canonical contract, NOT named fields.
		 * The subset header this target used to carry named them miso_pin / wp_pin /
		 * hold_pin; canonical has never carved them out of FlashConfig.reserved, so the
		 * host does not know to set them and they arrive as the must-be-zero the contract
		 * promises unless a config was hand-built against the old nRF header. Reading them
		 * by index keeps the behaviour byte-identical while stating what they actually are.
		 * Promoting them is an upstream change (see protocol_pending.h for the sequence),
		 * not a local rename. */
		od_log_info("flash powerdown MOSI=%u SCK=%u CS=%u MISO=%u WP=%u HOLD=%u",
		       fc->mosi_pin, fc->sck_pin, fc->cs_pin,
		       fc->reserved[0], fc->reserved[1], fc->reserved[2]);
		od_board_flash_powerdown(fc->mosi_pin, fc->sck_pin, fc->cs_pin,
					 fc->reserved[0], fc->reserved[1], fc->reserved[2]);
		break;
	}
}

void opendisplay_ble_reload_config_from_nvm(void)
{
	if (!loadGlobalConfig(&s_od_global_config)) {
		memset(&s_od_global_config, 0, sizeof(s_od_global_config));
	}
	flash_powerdown_from_config();
#if OD_CAP_IMGSTORE
	od_flash_store_mount();
#endif
	od_smp_sync();
	opendisplay_nfc_apply_config(&s_od_global_config);
	/* A live config change is a reboot as far as a host is concerned: the flag tells a scanner
	 * to re-read the device, and the MSD picks up the new encryption state. */
	s_reboot_flag = 1;
	opendisplay_ble_update_msd(true);
	/* Claim mode owns the panel (SECURITY_V2 7.3); a pattern in progress holds the boot screen
	 * back until the loop requests it. */
	if (!od_claim_mode_active() && !s_boot_screen_deferred) {
		opendisplay_display_request_boot();
	}
	/* Apply advertising TX power once the advertiser is active again. */
	__atomic_store_n(&s_adv_txp_pending, (uint8_t)1, __ATOMIC_RELEASE);
	/* Called from od_cmd_config.c's handlers, which run on the loop thread -- safe to request
	 * a restart directly. Only meaningful if something is actually on-air to refresh; if not
	 * yet active the next natural start already picks up the new config. */
	if (s_adv.active) {
		request_adv_restart();
	}
}

/* The shared boost seam (shared/core/od_adv_app.h). ISR-SAFE -- reachable from
 * opendisplay_nfc.c's nfc_callback() (HAL_NFC callback context, confirmed) as well as the main
 * loop (button/touch). MUST NOT touch s_adv or call any bt_le_adv_ or od_adv_ function -- those
 * are loop-thread-only by od_adv_control's single-owner contract. May only set plain flags. */
void od_adv_app_boost(void)
{
	s_adv_boost_start_ms = k_uptime_get_32();
	s_adv_boost_on = true;
}

/* Loop-thread only. Detects a mismatch between the interval currently wanted (boost window
 * open or not) and the interval last SUCCESSFULLY applied, and requests a restart to correct
 * it -- covers both edges (boost engaging, boost expiring) with one comparison. If advertising
 * isn't active yet, nothing to do -- the next natural start already reads
 * apply_adv_interval() fresh. */
void opendisplay_ble_advertising_tick(void)
{
	if (s_adv.active && (adv_boost_active(k_uptime_get_32()) != s_adv_interval_is_boost)) {
		request_adv_restart();
	}
}

void opendisplay_ble_init(void)
{
	int err;

	(void)initConfigStorage();
#ifdef FACTORY_CLEAR_CONFIG_ON_BOOT
	/* One-shot clear build (scripts/factory_config_gen.py). */
	od_log_info("factory clear build: erasing stored config");
	(void)clearStoredConfig();
#endif
	bool config_loaded = loadGlobalConfig(&s_od_global_config);
	bool factory_provisioned = false;
	if (!config_loaded && tryProvisionFactoryEmbed()) {
		/* No valid stored config, but a factory embed was just provisioned. */
		config_loaded = loadGlobalConfig(&s_od_global_config);
		factory_provisioned = config_loaded;
	}
#if OD_CAP_IMGSTORE
	if (config_loaded) {
		od_flash_store_mount();
	}
#endif
	if (config_loaded) {
		od_log_info("config loaded: displays=%u",
		       (unsigned)s_od_global_config.display_count);
	} else {
		od_log_info("config: defaults");
	}
	/* SECURITY_V2 7.1 / 10.1, FACTORY_RESET.md 3.1: reset pin, boot history, a pending or
	 * triggered factory reset, the boot decision and claim-mode entry, before the radio. */
	{
		struct od_security_boot_report rep;

		od_security_boot(config_loaded ? od_get_parsed_security() : NULL, config_loaded,
				 factory_provisioned, &rep);
		od_log_info("sec2: cause=0x%03x k=%u state=%u claim=%u", (unsigned)rep.cause,
			    (unsigned)rep.short_boots, (unsigned)rep.state, (unsigned)rep.kind);
	}
	if (s_config_changed) {
		s_config_changed = false;
		config_loaded = loadGlobalConfig(&s_od_global_config);
	}
	flash_powerdown_from_config();

	od_sensor_bq27220_init(opendisplay_get_global_config());
	opendisplay_sensor_npm1300_init();
	od_sensor_sht40_init(opendisplay_get_global_config());

	opendisplay_led_init();
	opendisplay_buzzer_init();

	k_work_init_delayable(&s_dfu_work, dfu_work_handler);
	k_work_init(&s_boot_display_work, boot_display_work_handler);
	od_adv_control_init(&s_adv);

	od_log_info("enabling Bluetooth");
	err = bt_enable(NULL);
	if (err != 0) {
		od_log_info("bt_enable failed: %d", err);
		return;
	}
	if (IS_ENABLED(CONFIG_SETTINGS)) {
		(void)settings_load();
	}
	if (s_unpair_pending) {
		/* FACTORY_RESET.md R5b, requested before bt_enable() by od_claim_app_erase_bonds(). */
		s_unpair_pending = false;
		(void)bt_unpair(BT_ID_DEFAULT, BT_ADDR_LE_ANY);
	}
	/* bt_enable(NULL) BLOCKS until ready -- unlike NimBLE's async on_sync, there's no later
	 * "ready" event to poll for, so this is asserted once here. */
	od_adv_stack_ready(&s_adv);

	opendisplay_button_init();
	opendisplay_touch_init();
	/* After SoftDevice + advertising controller init (NFC field MSD uses the pending flag). */
	opendisplay_nfc_apply_config(&s_od_global_config);
	/* Match Adafruit: hide SMP when encryption is on until CMD_ENTER_DFU. */
	od_smp_sync();
	update_msd_payload();

	/* BLOCKS until the boot screen is off the panel, or the bound below expires. Matches
	 * esp32-idf/main.cpp's "SoftDevice must start before display/SPI; advertising starts
	 * after boot screen" -- the BT stack is already up (bt_enable() above), but nothing may
	 * be able to CONNECT yet, because a connected central can push a direct-write immediately,
	 * and the shared transfer adapter touches the same od_display session and the
	 * same panel GPIOs/SPI device the boot-display work queue thread is mid-sequence on, with
	 * no lock between them. A wedge on either side used to surface as a frozen, unpainted
	 * boot screen with no way to tell why. The timeout keeps a genuinely stuck render from
	 * also taking BLE reachability down with it -- staying reachable over BLE/DFU even with a
	 * bad panel is the same trade-off od_watchdog_app_safe_mode() makes elsewhere. */
	/* Claim mode draws its own screen. A boot that may be one step of a power-cycle pattern
	 * defers the refresh until the pattern can no longer continue (od_claim_mode.h). */
	if (od_claim_mode_defer_boot_screen()) {
		s_boot_screen_deferred = !od_claim_mode_active();
	} else if (!od_claim_mode_active()) {
		uint32_t waited = 0u;

		schedule_boot_display_apply();
		/* Waited in slices on this (the loop) thread, which owns od_claim_mode: the
		 * power-cycle count lands at T_ARM / T_CLEAR while the work queue draws, not after
		 * the refresh (SECURITY_V2 7.2). */
		while (k_sem_take(&s_boot_display_done, K_MSEC(100)) != 0) {
			od_claim_mode_tick_count();
			waited += 100u;
			if (waited >= OD_BOOT_DISPLAY_WAIT_MS) {
				od_log_error("boot display: still running after %u ms - advertising anyway",
					     (unsigned)OD_BOOT_DISPLAY_WAIT_MS);
				break;
			}
		}
	}

	od_adv_set_payload(&s_adv, msd_payload);
	od_adv_request_start(&s_adv);
	/* Advertising, device-name computation, and the TX-power apply all happen on the first
	 * opendisplay_ble_process() pass (called from main.c immediately after this function
	 * returns), not synchronously here -- so s_dev_name is not yet valid at this log line. */
	od_log_info("BLE init complete, advertising requested");
}

void opendisplay_ble_config_changed(void)
{
	s_config_changed = true;
}

void opendisplay_ble_request_unpair_all(void)
{
	s_unpair_pending = true;
}

/* Claim mode, the boot-history writes, a deferred boot screen and a policy reload. Called from
 * every main-loop pass, connected or not. */
void opendisplay_ble_security_tick(void)
{
	od_claim_mode_tick();
	if (s_config_changed) {
		s_config_changed = false;
		opendisplay_ble_reload_config_from_nvm();
	}
	if (s_boot_screen_deferred && !od_claim_mode_defer_boot_screen()) {
		s_boot_screen_deferred = false;
		opendisplay_display_request_boot();
	}
}

void opendisplay_ble_process(void)
{
	opendisplay_pipe_process();
	opendisplay_led_process();
	opendisplay_buzzer_process();
	opendisplay_button_process();
	opendisplay_touch_process();
	opendisplay_nfc_process();
	opendisplay_ble_advertising_tick();
	/* No display-refresh gating exists on this target today (unlike ESP32's
	 * !epdRefreshInProgress) -- start_allowed unconditionally true, preserving current
	 * behavior. */
	(void)ble_service_advertising(true);
	opendisplay_display_service();
}

bool opendisplay_ble_advertising_active(void)
{
	return s_adv.active;
}

bool opendisplay_ble_advertising_settling(void)
{
	if (s_adv.faulted || !s_adv.stack_ready) {
		return false;
	}
	if (s_adv_restart_pending || s_adv.payload_dirty) {
		return true;
	}
	return s_adv.desired && !s_adv.active && s_adv.connection_count == 0u;
}

void opendisplay_ble_schedule_dfu(void)
{
	od_log_info("ENTER_DFU: unlocking SMP OTA");
	(void)k_work_cancel_delayable(&s_dfu_work);
	(void)k_work_schedule(&s_dfu_work, K_MSEC(500));
}

void opendisplay_ble_schedule_deep_sleep(void)
{
	if (!od_claim_mode_before_sleep()) {
		return;                      /* claim mode stays awake (SECURITY_V2 7.3) */
	}
	od_log_info("deep sleep: nPM1300 hibernate if available");
	opendisplay_sensor_npm1300_enter_hibernate();
}

bool opendisplay_ble_is_connected(void)
{
	return s_conn != NULL;
}
