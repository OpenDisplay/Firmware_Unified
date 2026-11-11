/* Nordic half of shared/core/od_claim_app.h, except od_claim_app_draw (opendisplay_display.c).
 * od_hal_reset.h lives in od_hal_wdt.c, which owns the one read of the reset-cause register. */

#include "od_claim_app.h"

#include "od_cmd_flash.h"
#include "od_gpio.h"
#include "od_imgstore.h"
#include "od_watchdog_app.h"
#include "opendisplay_ble.h"
#include "opendisplay_config_storage.h"
#include "od_caps.h"

#include <zephyr/kernel.h>

/* The panel is the primary signal; LED/buzzer patterns are not implemented on this target yet
 * (od_claim_app.h allows a no-op). */
void od_claim_app_signal(enum od_claim_signal sig, uint8_t arg)
{
	(void)sig;
	(void)arg;
}

/* 0x27 reset_pin uses the binary-input encoding of every other Nordic pin (od_gpio.h). */
bool od_claim_app_pin_input(uint8_t pin, bool pull_up, bool pull_down)
{
	uint8_t port;
	uint8_t p;

	if (pin == 0xFFu || !od_pin_decode(pin, &port, &p) || !od_gpio_port_ready(port)) {
		return false;
	}
	od_gpio_configure_input(pin, pull_up, pull_down);
	return true;
}

int od_claim_app_pin_read(uint8_t pin)
{
	const int v = od_gpio_read(pin);

	return v < 0 ? -1 : (v != 0 ? 1 : 0);
}

void od_claim_app_wait_ms(uint32_t ms)
{
	while (ms > 0u) {
		const uint32_t step = ms < 10u ? ms : 10u;

		k_busy_wait(step * 1000u);
		ms -= step;
		od_watchdog_app_service();
	}
}

bool od_claim_app_erase_images(void)
{
#if OD_CAP_IMGSTORE
	bool ok;

	if (!od_imgstore_status()->mounted) {
		od_flash_store_mount();
	}
	if (!od_imgstore_status()->mounted) {
		return true;
	}
	od_flash_store_wake();
	ok = od_imgstore_erase_all();
	od_flash_store_sleep();
	return ok;
#else
	return true;
#endif
}

/* Runs before bt_enable() (od_security_boot is ahead of the radio), so the unpair is deferred to
 * just after settings_load(): opendisplay_ble_unpair_all_pending(). */
bool od_claim_app_erase_bonds(void)
{
	opendisplay_ble_request_unpair_all();
	return true;
}

bool od_claim_app_scrub_extras(void)
{
	return true;
}

uint8_t *od_claim_app_workspace(uint32_t *cap)
{
	return opendisplay_config_workspace(cap);
}

/* The chunked write assembles into od_cmd_config.c's own buffer, never into this workspace. */
bool od_claim_app_workspace_busy(void)
{
	return false;
}

void od_claim_app_config_changed(void)
{
	opendisplay_ble_config_changed();
}
