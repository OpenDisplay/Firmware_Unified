/* od_img_confirm_app.c -- see od_img_confirm_app.h and shared/core/od_img_confirm.h.
 *
 * Evidence on this target:
 *   link opened / closed   BT_CONN_CB connected / disconnected (opendisplay_ble.c)
 *   ATT seen               an ATT MTU exchange (bt_gatt_cb.att_mtu_updated), a CCC write, or a
 *                          write to the OpenDisplay characteristic
 *   served                 a write to the OpenDisplay characteristic accepted by od_gatt_write
 * The callbacks only set flags; tick() feeds od_health / od_img_confirm in order and acts.
 * Without the MCUboot image manager (no second slot) every call is a no-op. */
#include "od_img_confirm_app.h"

#include "od_health.h"
#include "od_img_confirm.h"
#include "od_log.h"

#include <zephyr/kernel.h>
#include <zephyr/bluetooth/gatt.h>
#if defined(CONFIG_MCUBOOT_IMG_MANAGER)
#include <zephyr/dfu/mcuboot.h>
#endif

#if defined(CONFIG_MCUBOOT_IMG_MANAGER)

static struct od_health s_health;
static struct od_img_confirm s_conf;
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
	od_img_confirm_app_att_seen();
}

static struct bt_gatt_cb s_gatt_cb = {
	.att_mtu_updated = mtu_updated,
};

static const char *reason_name(uint8_t r)
{
	switch (r) {
	case OD_IMG_REASON_ALREADY:  return "already confirmed";
	case OD_IMG_REASON_SERVED:   return "GATT write served";
	case OD_IMG_REASON_FALLBACK: return "24 h with no dead link";
	case OD_IMG_REASON_DEAD:     return "GATT not served";
	default:                     return "none";
	}
}

void od_img_confirm_app_boot(void)
{
	const bool confirmed = boot_is_img_confirmed();

	od_health_init(&s_health, 0u);
	od_img_confirm_init(&s_conf, confirmed);
	bt_gatt_cb_register(&s_gatt_cb);
	s_ready = true;
	od_log_info("image: %s", confirmed ? "confirmed"
				  : "UNCONFIRMED -- confirms on the first served GATT write");
}

void od_img_confirm_app_link_opened(void)
{
	s_open_ms = k_uptime_get_32();
	atomic_or(&s_ev, EV_OPEN);
}

void od_img_confirm_app_link_closed(void)
{
	s_close_ms = k_uptime_get_32();
	atomic_or(&s_ev, EV_CLOSE);
}

void od_img_confirm_app_att_seen(void)
{
	atomic_or(&s_ev, EV_ATT);
}

void od_img_confirm_app_served(void)
{
	atomic_or(&s_ev, EV_ATT | EV_SERVED);
}

static void act(enum od_img_confirm_action a)
{
	if (a == OD_IMG_CONFIRM) {
		const int rc = boot_write_img_confirmed();

		od_log_info("image confirmed (%s): rc=%d", reason_name(s_conf.reason), rc);
		if (rc != 0) {
			s_conf.confirmed = false;   /* try again on the next evidence */
		}
	} else if (a == OD_IMG_REVERT) {
		od_log_error("image cannot serve GATT (%u dead links): resetting so MCUboot reverts it",
			     (unsigned)s_health.dead_links);
		k_sleep(K_MSEC(300));
		NVIC_SystemReset();
	}
}

void od_img_confirm_app_tick(void)
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
		act(od_img_confirm_served(&s_conf));
	}
	if ((ev & EV_CLOSE) != 0) {
		const enum od_health_action v = od_health_link_closed(&s_health, s_close_ms);

		if (!s_conf.confirmed && s_health.dead_links != 0u) {
			od_log_warn("image unconfirmed: link with no ATT (%u in a row)",
				    (unsigned)s_health.dead_links);
		}
		act(od_img_confirm_link_closed(&s_conf, v, s_health.dead_links));
	}
	act(od_img_confirm_poll(&s_conf, k_uptime_get_32()));
}

#else /* !CONFIG_MCUBOOT_IMG_MANAGER */

void od_img_confirm_app_boot(void) {}
void od_img_confirm_app_link_opened(void) {}
void od_img_confirm_app_link_closed(void) {}
void od_img_confirm_app_att_seen(void) {}
void od_img_confirm_app_served(void) {}
void od_img_confirm_app_tick(void) {}

#endif
