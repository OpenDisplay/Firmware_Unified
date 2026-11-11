#include "od_claim_mode.h"
#include "od_board.h"
#include "od_hal_log.h"
#include "od_log.h"
#include "opendisplay_ble.h"
#include "opendisplay_config_parser.h"
#include "opendisplay_display.h"
#include "opendisplay_idle_wake.h"
#include "od_runtime_types.h"
#include "od_watchdog_app.h"
#include "od_test_bench.h"
#include "od_img_confirm_app.h"
#include "od_sec2_trace.h"

#include <stdio.h>
#include <zephyr/kernel.h>
#if defined(CONFIG_BOOTLOADER_MCUBOOT)
#include <zephyr/dfu/mcuboot.h>
#endif

enum idle_wait_result {
	IDLE_WAIT_ELAPSED = 0,
	IDLE_WAIT_INTERRUPTED,
};

static enum idle_wait_result idle_wait_until(int64_t deadline_ms)
{
	const uint32_t chunk_ms = 1000u;
	const uint32_t settle_step_ms = 5u;

	for (;;) {
		int64_t now_ms;
		int64_t remaining_ms;
		uint32_t step_ms;

		/* The idle wait is chunked at one second, so feeding here rather than only in the
		 * caller is what keeps a configured sleep_timeout_ms of minutes from looking like
		 * a hang. Reaching this line proves the thread is running, which is the whole
		 * requirement for a feed site. */
		od_watchdog_app_service();
		opendisplay_ble_process();
		if (opendisplay_ble_is_connected()) {
			return IDLE_WAIT_INTERRUPTED;
		}

		now_ms = k_uptime_get();
		if (now_ms >= deadline_ms) {
			/* A pump pass can exceed a short configured interval. Still block for at
			 * least one tick unless a connection event needs immediate service. */
			return opendisplay_idle_wait(1u) ? IDLE_WAIT_INTERRUPTED : IDLE_WAIT_ELAPSED;
		}
		remaining_ms = deadline_ms - now_ms;
		step_ms = (remaining_ms > (int64_t)chunk_ms) ? chunk_ms : (uint32_t)remaining_ms;
		/* An MSD refresh is stop -> program -> start, one step per pass. Sleeping a full
		 * chunk between steps leaves the advertiser off for the whole refresh interval. */
		if (opendisplay_ble_advertising_settling() && step_ms > settle_step_ms) {
			step_ms = settle_step_ms;
		}
		if (opendisplay_idle_wait(step_ms)) {
			return IDLE_WAIT_INTERRUPTED;
		}
	}
}

static enum idle_wait_result idle_wait_ms(uint32_t delay_ms)
{
	return idle_wait_until(k_uptime_get() + (int64_t)delay_ms);
}

int main(void)
{
	const struct od_config *cfg;
	uint32_t msd_interval_ms = 0u;
	int64_t msd_deadline_ms = 0;
	bool msd_deadline_armed = false;

	od_hal_log_open();
	od_log_init();
	/* A test-trace image says so first, on every boot (tools/check.sh requires this line). */
	OD_SEC2_TRACE_BANNER();
#if defined(OD_DEBUG_BUILD)
	od_log_info("OpenDisplay %s DEBUG starting", od_board_name());
#else
	od_log_info("OpenDisplay %s starting", od_board_name());
#endif
	/* Before anything can touch the panel: this decides whether the previous run ended in a
	 * watchdog reset, and whether three of them in a row mean this boot must skip the panel
	 * entirely. Arming comes after, so a hang inside boot_init cannot be masked by a reset. */
	od_watchdog_app_boot();
	od_watchdog_app_arm();

	od_board_early_init();
	/* MCUboot image confirmation is EVIDENCE-BASED (shared/core/od_img_confirm.h): a swapped-in
	 * image confirms on the first GATT write it serves (or after 24 h with no dead link), and
	 * one whose links carry no ATT resets so MCUboot reverts it. Confirming here, before any
	 * link had carried GATT, let an image with a dead GATT server confirm itself and become
	 * unrecoverable over the air -- the BG22 stranding. Without the image manager (one slot,
	 * nothing to revert to) this is a no-op. */
	od_img_confirm_app_boot();   /* before the radio: no link can predate its callbacks */
	opendisplay_ble_init();
#if defined(OD_TEST_BENCH) && OD_TEST_BENCH
	/* Bench image: the no-touch revert timer on top (src/od_test_bench.c). */
	od_test_bench_image_boot();
#endif

	while (1) {
		uint32_t configured_msd_interval_ms;
		int64_t now_ms;

		od_watchdog_app_service();
		od_test_bench_tick();
		opendisplay_ble_security_tick();
		cfg = opendisplay_get_global_config();
		configured_msd_interval_ms =
			(cfg != NULL && cfg->loaded) ? cfg->power_option.sleep_timeout_ms : 0u;
		if (configured_msd_interval_ms != msd_interval_ms) {
			msd_interval_ms = configured_msd_interval_ms;
			msd_deadline_armed = msd_interval_ms > 0u;
			if (msd_deadline_armed) {
				msd_deadline_ms = k_uptime_get() + (int64_t)msd_interval_ms;
			}
		}

		if (opendisplay_ble_is_connected()) {
			opendisplay_ble_process();
			k_msleep(10);
			continue;
		}

		now_ms = k_uptime_get();
		if (msd_deadline_armed && now_ms >= msd_deadline_ms) {
			opendisplay_ble_update_msd(true);
			/* Schedule from the actual publication time. A connection may have held this
			 * overdue for several intervals; publish once rather than replaying them. */
			msd_deadline_ms = now_ms + (int64_t)msd_interval_ms;
		}

		if (od_claim_mode_active() || now_ms < (int64_t)OD_CM_T_CLEAR_MS + 1000) {
			/* Claim mode's deadline and screens, and the boot-history writes at T_ARM and
			 * T_CLEAR, are driven from this loop: keep the pass rate up while they are due. */
			(void)idle_wait_ms(100u);
		} else if (!opendisplay_ble_advertising_active()) {
			/* Advertising not yet confirmed running -- e.g. a RETRY right after a
			 * disconnect (shared/core/od_adv_control.c). Poll promptly instead of the
			 * full idle chunk below, so a delayed restart attempt doesn't stall
			 * rediscovery for up to ~1s. Resolves in one or two passes in the ordinary
			 * case; not a standing battery cost since it only applies transiently. */
			(void)idle_wait_ms(50u);
		} else if (msd_deadline_armed) {
			/* Matches nRF52840 Firmware: MSD refreshes once per sleep_timeout_ms
			 * idle cycle; without a configured timeout there is no periodic MSD
			 * update (buttons and adv restarts still refresh it). */
			(void)idle_wait_until(msd_deadline_ms);
		} else {
			(void)idle_wait_ms(500u);
		}
	}
	return 0;
}
