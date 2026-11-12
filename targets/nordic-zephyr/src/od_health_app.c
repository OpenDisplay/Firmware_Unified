/* od_health_app.c -- see od_health_app.h and shared/core/od_health.h (rollback kind).
 *
 * Evidence on this target:
 *   link opened / closed   BT_CONN_CB connected / disconnected (opendisplay_ble.c)
 *   ATT seen               an ATT MTU exchange (bt_gatt_cb.att_mtu_updated), a CCC write, or a
 *                          write to the OpenDisplay characteristic
 *   served                 a write to the OpenDisplay characteristic accepted by od_gatt_write
 * The callbacks only set flags; tick() feeds od_health in order and acts.
 * Without the MCUboot image manager (no second slot) every call is a no-op. */
#include "od_health_app.h"

#include "od_health.h"
#include "od_log.h"

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/gatt.h>
#if defined(CONFIG_MCUBOOT_IMG_MANAGER)
#include <zephyr/dfu/mcuboot.h>
#endif

#if defined(CONFIG_MCUBOOT_IMG_MANAGER)

static struct od_health s_health;
static bool s_ready;

static atomic_t s_ev;            /* EV_* bits */
static uint32_t s_open_ms;
static uint32_t s_close_ms;
#define EV_OPEN   BIT(0)
#define EV_ATT    BIT(1)
#define EV_SERVED BIT(2)
#define EV_CLOSE  BIT(3)

static void mtu_updated(struct bt_conn *conn, uint16_t tx, uint16_t rx)
{
	ARG_UNUSED(conn);
	ARG_UNUSED(tx);
	ARG_UNUSED(rx);
	od_health_app_att_seen();
}

static struct bt_gatt_cb s_gatt_cb = {
	.att_mtu_updated = mtu_updated,
};

static const char *reason_name(uint8_t r)
{
	switch (r) {
	case OD_HEALTH_CONFIRM_ALREADY:  return "already confirmed";
	case OD_HEALTH_CONFIRM_SERVED:   return "GATT write served";
	case OD_HEALTH_CONFIRM_FALLBACK: return "24 h with no dead link";
	case OD_HEALTH_CONFIRM_EXTERNAL: return "confirmed by SMP";
	default:                     return "none";
	}
}

void od_health_app_boot(void)
{
	const bool confirmed = boot_is_img_confirmed();

	od_health_init_rollback(&s_health, confirmed);
	bt_gatt_cb_register(&s_gatt_cb);
	s_ready = true;
	od_log_info("image: %s", confirmed ? "confirmed"
				  : "UNCONFIRMED -- confirms on the first served GATT write");
}

void od_health_app_link_opened(void)
{
	s_open_ms = k_uptime_get_32();
	atomic_or(&s_ev, EV_OPEN);
}

void od_health_app_link_closed(void)
{
	s_close_ms = k_uptime_get_32();
	atomic_or(&s_ev, EV_CLOSE);
}

#if defined(OD_SEC2_BENCH_BUILD) && OD_SEC2_BENCH_BUILD
/* Bench only ('od blind'): drop the ATT/served evidence, so ordinary links look like the BG22
 * stranding and the REVERT arm can be exercised on hardware. */
static volatile bool s_bench_blind;
void od_health_app_bench_blind(void)
{
	s_bench_blind = true;
	od_log_warn("[BENCH] image-confirm evidence ignored from now on (dead-GATT simulation)");
}
#define BENCH_BLIND() (s_bench_blind)
#else
#define BENCH_BLIND() (false)
#endif

void od_health_app_att_seen(void)
{
	if (!BENCH_BLIND()) {
		atomic_or(&s_ev, EV_ATT);
	}
}

void od_health_app_served(void)
{
	if (!BENCH_BLIND()) {
		atomic_or(&s_ev, EV_ATT | EV_SERVED);
	}
}

static void act(enum od_health_action a)
{
	if (a == OD_HEALTH_CONFIRM) {
		const int rc = boot_write_img_confirmed();

		od_log_info("image confirmed (%s): rc=%d", reason_name(s_health.confirm_reason), rc);
		if (rc != 0) {
			od_health_confirm_failed(&s_health);   /* try again on the next evidence */
		}
	} else if (a == OD_HEALTH_RECOVER) {
		if (boot_is_img_confirmed()) {
			/* Confirmed by another path (an SMP image-confirm): nothing to revert to. */
			od_health_confirmed_externally(&s_health);
			return;
		}
		od_log_error("image cannot serve GATT (%u dead links): resetting so MCUboot reverts it",
			     (unsigned)s_health.dead_links);
		k_sleep(K_MSEC(300));
		NVIC_SystemReset();
	}
}

void od_health_app_tick(void)
{
	atomic_val_t ev;

	if (!s_ready) {
		return;
	}
	ev = atomic_clear(&s_ev);
	if ((ev & EV_OPEN) != 0) {
		od_health_link_opened(&s_health, s_open_ms);
	}
	if ((ev & EV_ATT) != 0) {
		od_health_att_seen(&s_health);
	}
	if ((ev & EV_SERVED) != 0) {
		act(od_health_served(&s_health));
	}
	if ((ev & EV_CLOSE) != 0) {
		const enum od_health_action v = od_health_link_closed(&s_health, s_close_ms);

		if (!s_health.confirmed && s_health.dead_links != 0u) {
			od_log_warn("image unconfirmed: link with no ATT (%u in a row)",
				    (unsigned)s_health.dead_links);
		}
		act(v);
	}
	act(od_health_poll(&s_health, k_uptime_get_32()));
}

#else /* !CONFIG_MCUBOOT_IMG_MANAGER */

void od_health_app_boot(void) {}
void od_health_app_link_opened(void) {}
void od_health_app_link_closed(void) {}
void od_health_app_att_seen(void) {}
void od_health_app_served(void) {}
void od_health_app_tick(void) {}

#endif
