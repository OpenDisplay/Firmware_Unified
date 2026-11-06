/* od_hal_wdt for Zephyr on nRF. Contract and division of labour: shared/hal/od_hal_wdt.h.
 *
 * Three peripherals, one per function, all resolved from devicetree so the same file serves
 * nRF52840 (WDT0, GPREGRET2 in POWER) and nRF54L15 / nRF54LM20A (WDT31, GPREGRET2 in RESETINFO)
 * without a per-SoC arm. The boards differ only in whether the nodes ship enabled, which is a
 * devicetree overlay question and not a code one.
 */

#include "od_hal_wdt.h"
#include "od_log.h"

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/drivers/retained_mem.h>
#include <zephyr/drivers/watchdog.h>

#include <hal/nrf_wdt.h>

#define OD_WDT_NODE      DT_ALIAS(watchdog0)
#define OD_RETAINED_NODE DT_NODELABEL(gpregret2)

#define OD_WDT_HAVE_WDT      DT_NODE_HAS_STATUS(OD_WDT_NODE, okay)
#define OD_WDT_HAVE_RETAINED DT_NODE_HAS_STATUS(OD_RETAINED_NODE, okay)

/* --------------------------------------------------------------------------- reset cause --- */

uint32_t od_hal_wdt_reset_reason(void)
{
	static uint32_t cached;
	static bool     have_cached;

	uint32_t cause = 0;
	uint32_t out   = 0;

	if (have_cached) {
		return cached;
	}

	if (hwinfo_get_reset_cause(&cause) != 0) {
		/* No hwinfo backend, or the SoC could not answer. Distinct from "nothing latched". */
		cached      = OD_HAL_WDT_RESET_UNKNOWN;
		have_cached = true;
		return cached;
	}

	/* CLEAR IT. nRF latches causes cumulatively and nothing else in this firmware clears them,
	 * so leaving the register alone makes one watchdog reset visible on every subsequent boot --
	 * which would drive the strike counter to safe mode on a device that recovered. */
	(void)hwinfo_clear_reset_cause();

	if ((cause & RESET_POR) != 0u) {
		out |= OD_HAL_WDT_RESET_POWER_ON;
	}
	if ((cause & RESET_WATCHDOG) != 0u) {
		out |= OD_HAL_WDT_RESET_WATCHDOG;
	}
	if ((cause & RESET_PIN) != 0u) {
		out |= OD_HAL_WDT_RESET_PIN;
	}
	if ((cause & RESET_SOFTWARE) != 0u) {
		out |= OD_HAL_WDT_RESET_SOFTWARE;
	}
	if ((cause & RESET_CPU_LOCKUP) != 0u) {
		out |= OD_HAL_WDT_RESET_LOCKUP;
	}
	if ((cause & RESET_BROWNOUT) != 0u) {
		out |= OD_HAL_WDT_RESET_BROWNOUT;
	}
	/* DEBUG, SECURITY, LOW_POWER_WAKE and the rest: real causes this policy does not model.
	 * They must still report as OTHER rather than fall through to zero, or a debugger-triggered
	 * reset would be indistinguishable from a cold start. */
	if (cause != 0u && out == 0u) {
		out = OD_HAL_WDT_RESET_OTHER;
	}
	/* A cold start latches nothing on nRF. Mapping zero to POWER_ON here is what keeps
	 * OD_HAL_WDT_RESET_UNKNOWN meaning "could not tell" and nothing else. */
	if (cause == 0u) {
		out = OD_HAL_WDT_RESET_POWER_ON;
	}

	cached      = out;
	have_cached = true;
	return cached;
}

/* ----------------------------------------------------------------------- retained storage --- */

/* GPREGRET2, through the retained_mem API rather than the register.
 *
 * GPREGRET2 AND NOT GPREGRET: in Zephyr's devicetree the two nodes are labelled gpregret1 and
 * gpregret2, and gpregret1 is retained-register 0 -- the one MCUboot and the DFU handshake use.
 * Taking it would make a breadcrumb write cancel a pending DFU request.
 *
 * A hardware register and not a .noinit RAM byte: MCUboot runs before the application and its
 * stack and bss are free to sit anywhere in RAM, so a retained RAM byte would be clobbered on
 * exactly the reset paths that pass through the bootloader. The whole value of the strike
 * counter is that it survives those.
 */
static const struct device *retained_device(void)
{
#if OD_WDT_HAVE_RETAINED
	const struct device *dev = DEVICE_DT_GET(OD_RETAINED_NODE);

	return device_is_ready(dev) ? dev : NULL;
#else
	return NULL;
#endif
}

bool od_hal_wdt_retained_read(uint8_t *out)
{
	const struct device *dev = retained_device();

	if (out == NULL || dev == NULL) {
		return false;
	}
	return retained_mem_read(dev, 0, out, 1) == 0;
}

bool od_hal_wdt_retained_write(uint8_t value)
{
	const struct device *dev = retained_device();

	if (dev == NULL) {
		return false;
	}
	return retained_mem_write(dev, 0, &value, 1) == 0;
}

/* -------------------------------------------------------------------------- the watchdog --- */

#if OD_WDT_HAVE_WDT

#define OD_WDT_REG ((NRF_WDT_Type *)DT_REG_ADDR(OD_WDT_NODE))

/* Set when wdt_setup() armed the peripheral this boot; feed() then goes through the driver. */
static const struct device *s_dev;
static int                  s_channel;

/* Set when the peripheral was ALREADY running before this boot configured anything. The Zephyr
 * driver cannot be told about it -- wdt_install_timeout() only refuses a setup this boot has
 * already done -- so the feed goes straight to the reload registers. */
static bool s_inherited;

/* Reload an inherited watchdog by writing every ENABLED request register.
 *
 * RREN is an AND, not an OR: the counter reloads only once every enabled RR has been written,
 * so feeding RR0 alone would never reload a watchdog armed with a different subset. What that
 * subset is cannot be known -- a previous build chose it -- so read it back and satisfy it. */
static void feed_inherited(void)
{
	for (uint32_t i = (uint32_t)NRF_WDT_RR0; i <= (uint32_t)NRF_WDT_RR7; i++) {
		nrf_wdt_rr_register_t rr = (nrf_wdt_rr_register_t)i;

		if (nrf_wdt_reload_request_enable_check(OD_WDT_REG, rr)) {
			nrf_wdt_reload_request_set(OD_WDT_REG, rr);
		}
	}
}

enum od_hal_wdt_arm_result od_hal_wdt_arm(uint32_t timeout_s)
{
	const struct device *dev = DEVICE_DT_GET(OD_WDT_NODE);
	struct wdt_timeout_cfg cfg;
	int ch;

	if (!device_is_ready(dev)) {
		return OD_HAL_WDT_ARM_UNSUPPORTED;
	}

	/* PROBE BEFORE CONFIGURING, and probe the peripheral rather than the driver. On nRF52840 the
	 * watchdog has no stop task and keeps running across a soft reset, so a build reached by DFU
	 * from one that armed it inherits a live watchdog. Not detecting that is a brick: nothing
	 * feeds it, and it resets forever until someone pulls the battery. Checked ahead of the
	 * timeout_s == 0 return for the same reason -- disabled must not mean unfed. */
	if (nrf_wdt_started_check(OD_WDT_REG)) {
		/* Dumped here because this is the only layer that can see the peripheral, and
		 * because what an inherited watchdog's period actually is decides whether the
		 * feed cadence above is fast enough for it. CRV is in 32768 Hz ticks. */
		od_log_warn("[WDT] ALREADY RUNNING at boot (not started by this call). "
			    "CRV=%lu (%lus) RREN=0x%lX CONFIG=0x%lX - cannot be reconfigured; "
			    "feeding it as-is.",
			    (unsigned long)nrf_wdt_reload_value_get(OD_WDT_REG),
			    (unsigned long)((nrf_wdt_reload_value_get(OD_WDT_REG) + 1UL) / 32768UL),
			    (unsigned long)OD_WDT_REG->RREN,
			    (unsigned long)OD_WDT_REG->CONFIG);
		s_inherited = true;
		feed_inherited();
		return OD_HAL_WDT_ARM_INHERITED;
	}

	if (timeout_s == 0u) {
		return OD_HAL_WDT_ARM_DISABLED;
	}

	cfg.window.min = 0u;
	cfg.window.max = timeout_s * 1000u; /* bounded to 3600 s by od_watchdog.h */
	cfg.callback   = NULL;
	cfg.flags      = WDT_FLAG_RESET_SOC;

	ch = wdt_install_timeout(dev, &cfg);
	if (ch < 0) {
		return OD_HAL_WDT_ARM_ERROR;
	}

	/* WDT_OPT_PAUSE_HALTED_BY_DBG and NOT WDT_OPT_PAUSE_IN_SLEEP. Halting on a debug probe is
	 * routine here and must not reset the board mid-inspection. Sleep is the opposite case: the
	 * main loop spends nearly all of its time in k_msleep(), so pausing there would suspend the
	 * watchdog for the whole idle period and leave it watching almost nothing.
	 *
	 * THIS REPRODUCES THE CANONICAL REGISTER STATE through the Zephyr driver rather than by
	 * writing CRV/RREN/CONFIG directly the way Firmware/src/watchdog_nrf.cpp does. The two
	 * agree bit for bit: wdt_nrfx sets RUN_SLEEP unless WDT_OPT_PAUSE_IN_SLEEP is passed and
	 * RUN_HALT unless WDT_OPT_PAUSE_HALTED_BY_DBG is, which is exactly
	 * NRF_WDT_BEHAVIOUR_RUN_SLEEP; wdt_install_timeout hands out channels from RR0 up, so the
	 * single timeout installed above is RR0, the one reload register the canonical arm enables.
	 * Going through the driver is what lets the nRF54L boards -- whose WDT has a stop task the
	 * nRF52840's lacks -- share this file instead of forking the register sequence per SoC. */
	if (wdt_setup(dev, WDT_OPT_PAUSE_HALTED_BY_DBG) < 0) {
		return OD_HAL_WDT_ARM_ERROR;
	}

	s_dev     = dev;
	s_channel = ch;
	return OD_HAL_WDT_ARM_OK;
}

void od_hal_wdt_feed(void)
{
	if (s_dev != NULL) {
		(void)wdt_feed(s_dev, s_channel);
		return;
	}
	if (s_inherited) {
		feed_inherited();
	}
}

#else /* !OD_WDT_HAVE_WDT */

enum od_hal_wdt_arm_result od_hal_wdt_arm(uint32_t timeout_s)
{
	(void)timeout_s;
	return OD_HAL_WDT_ARM_UNSUPPORTED;
}

void od_hal_wdt_feed(void)
{
}

#endif /* OD_WDT_HAVE_WDT */
