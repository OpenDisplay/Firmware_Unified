/* Log lines, severities and the [WDT] prefix are the canonical ones from
 * Firmware/src/watchdog_nrf.cpp. That file is the field-proven original and what anyone
 * triaging a tag expects to read; keeping the wording identical is what lets a log from this
 * firmware be compared against one from the deployed build.
 *
 * The reason NAMES differ, and only there: the canonical file prints nRF RESETREAS bit names
 * (RESETPIN|DOG|SREQ...), while shared/hal/od_hal_wdt.h defines one portable cause set that
 * every chip maps onto. Printing the portable names is the point of the promotion -- the same
 * vocabulary now appears in an ESP32 log and an nRF log.
 */

#include "od_watchdog_app.h"
#include "od_test_bench.h"

#include "od_log.h"
#include "od_watchdog.h"

#include <stdio.h>
#include <string.h>
#include <zephyr/kernel.h>

static struct od_watchdog s_wdt;

/* SERIALISES THE THREE THREADS THAT REACH od_watchdog.
 *
 * The canonical implementation needs no lock: Firmware is one loop task and every breadcrumb and
 * feed is on it. Here main() feeds while the display work queue stamps breadcrumbs, and both are
 * a read-modify-write of the same retained byte through od_watchdog's write cache. Without this
 * a breadcrumb interleaved with the ten-minute strike clear can write back a stale strike count
 * -- which is the one field whose loss disables boot-loop containment.
 *
 * A spinlock and not a mutex: every critical section is a couple of register accesses, and a
 * feed site must never be able to block. */
static struct k_spinlock s_lock;

/* --------------------------------------------------------------------------- boot report --- */

/* Decodes the OD_HAL_WDT_RESET_* bitmask. A bitmask, so print every set bit rather than the
 * first match: several causes latch at once, and a decoder that stops at the first one hides a
 * watchdog reset behind a coincident pin reset. */
static void log_reset_reason(uint32_t r)
{
	static const struct {
		uint32_t msk;
		const char *name;
	} kinds[] = {
		{ OD_HAL_WDT_RESET_POWER_ON, "POWERON"  },
		{ OD_HAL_WDT_RESET_WATCHDOG, "DOG"      },
		{ OD_HAL_WDT_RESET_PIN,      "RESETPIN" },
		{ OD_HAL_WDT_RESET_SOFTWARE, "SREQ"     },
		{ OD_HAL_WDT_RESET_LOCKUP,   "LOCKUP"   },
		{ OD_HAL_WDT_RESET_BROWNOUT, "BROWNOUT" },
		{ OD_HAL_WDT_RESET_OTHER,    "OTHER"    },
	};
	char buf[80];
	size_t n = 0;
	uint32_t seen = 0;

	if (r == OD_HAL_WDT_RESET_UNKNOWN) {
		/* Not the same as a cold start: the target could not tell. The HAL maps a cold
		 * nRF boot -- which latches nothing -- to POWERON explicitly. */
		od_log_info("[WDT] reset reason: UNKNOWN (0x00)");
		return;
	}

	buf[0] = '\0';
	for (unsigned i = 0; i < ARRAY_SIZE(kinds); i++) {
		int w;

		if ((r & kinds[i].msk) == 0u) {
			continue;
		}
		seen |= kinds[i].msk;
		w = snprintf(buf + n, sizeof(buf) - n, "%s%s", n ? "|" : "", kinds[i].name);
		/* snprintf returns what it WOULD have written; clamp so a truncating write
		 * cannot push n past the buffer. */
		if (w <= 0) {
			break;
		}
		n += (size_t)w;
		if (n >= sizeof(buf) - 1u) {
			n = sizeof(buf) - 1u;
			break;
		}
	}
	if ((r & ~seen) != 0u) {
		(void)snprintf(buf + n, sizeof(buf) - n, "%sUNMAPPED", n ? "|" : "");
	}
	od_log_info("[WDT] reset reason: %s (0x%02X)", buf, (unsigned)r);
}

void od_watchdog_app_boot(void)
{
	struct od_watchdog_boot_report report;

	od_watchdog_boot_init(&s_wdt, k_uptime_get_32(), &report);

	log_reset_reason(report.reset_reason);

	if (!report.retained_readable) {
		od_log_warn("[WDT] GPREGRET2 unreadable - breadcrumb and strike count unavailable");
	} else if (report.breadcrumb_valid) {
		od_log_info("[WDT] breadcrumb from previous run: phase=%s (%u) strikes=%u",
			    od_watchdog_phase_name(report.prev_phase),
			    (unsigned)report.prev_phase, (unsigned)report.strikes);
	} else {
		od_log_info("[WDT] no retained breadcrumb (cold start or first boot)");
		if (!report.retained_writable) {
			od_log_warn("[WDT] GPREGRET2 unwritable - breadcrumbs disabled");
		}
	}

	if (report.was_watchdog) {
		od_log_warn("[WDT] previous boot ended in a watchdog reset (strike %u/%u)",
			    (unsigned)report.strikes, (unsigned)OD_WDT_SAFE_MODE_STRIKES);
	}

	if (report.safe_mode) {
		od_log_error("[WDT] SAFE MODE: %u consecutive watchdog resets - skipping ALL panel "
			     "work this boot so the device stays reachable over BLE/DFU. "
			     "Clears after %lu s of healthy uptime.",
			     (unsigned)report.strikes,
			     (unsigned long)(OD_WDT_HEALTHY_MS / 1000UL));
	}
}

void od_watchdog_app_arm(void)
{
	enum od_hal_wdt_arm_result rc = od_watchdog_arm(&s_wdt);

	switch (rc) {
	case OD_HAL_WDT_ARM_OK:
		od_log_info("[WDT] armed: %us", (unsigned)OD_WDT_TIMEOUT_S);
		break;
	case OD_HAL_WDT_ARM_INHERITED:
		/* The register dump that goes with this is in od_hal_wdt.c, which is the only
		 * layer that can see the peripheral. */
		od_log_warn("[WDT] inherited a running watchdog; feeding it as-is");
		break;
	case OD_HAL_WDT_ARM_DISABLED:
		od_log_warn("[WDT] disabled at build time (OD_WDT_TIMEOUT_S=0)");
		break;
	case OD_HAL_WDT_ARM_UNSUPPORTED:
		od_log_warn("[WDT] no hardware watchdog on this board");
		break;
	default:
		od_log_error("[WDT] arm FAILED - this boot has no watchdog");
		break;
	}
}

void od_watchdog_app_service(void)
{
	bool cleared = false;

	/* Bench test image past its revert deadline: let the hardware watchdog reset it (MCUboot
	 * then reverts). A release build compiles this to nothing (src/od_test_bench.h). */
	if (od_test_bench_hold_feed()) {
		return;
	}
	K_SPINLOCK(&s_lock) {
		bool pending_before = s_wdt.strikes_to_clear;

		od_watchdog_feed(&s_wdt, k_uptime_get_32());
		cleared = pending_before && !s_wdt.strikes_to_clear;
	}

	/* Logged outside the lock, and only on the transition: the uptime rule runs on every
	 * feed, so an unconditional line here would be one per loop pass. */
	if (cleared) {
		od_log_info("[WDT] %lu s of healthy uptime - strike counter cleared",
			    (unsigned long)(OD_WDT_HEALTHY_MS / 1000UL));
	}
}

bool od_watchdog_app_safe_mode(void)
{
	/* Unlocked, and safe: this is decided once in od_watchdog_app_boot(), before any work
	 * queue starts, and never written again. */
	return od_watchdog_in_safe_mode(&s_wdt);
}

void od_watchdog_app_phase(uint8_t phase)
{
	static bool fail_logged;
	bool failed = false;

	K_SPINLOCK(&s_lock) {
		bool failed_before = s_wdt.write_failed;

		od_watchdog_breadcrumb(&s_wdt, phase);
		failed = !failed_before && s_wdt.write_failed;
	}

	/* Latched: breadcrumb sites sit on per-frame paths, so this must warn once and then stay
	 * quiet. A silently dead breadcrumb is worse than none -- the boot log still prints a
	 * phase, and invites a confident wrong conclusion about where the firmware wedged. */
	if (failed && !fail_logged) {
		fail_logged = true;
		od_log_warn("[WDT] GPREGRET2 write failed - breadcrumb may be stale");
	}
}
