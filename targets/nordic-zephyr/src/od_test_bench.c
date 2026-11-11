/* od_test_bench.c -- TEST-ONLY bench aids for the nRF52840 (XIAO / EN04) USB console.
 *
 * Compiled only when targets/nordic-zephyr/zephyr/CMakeLists.txt sees an explicit bench
 * acknowledgement in the environment (the SEC2 trace switch and/or
 * OD_TEST_REVERT_S=<seconds>), and never in a versioned or CI build: the configure step refuses
 * both. A release build does not contain this file.
 *
 * Two jobs, both for a bench where nobody can touch the board:
 *
 * 1. A NO-TOUCH WAY BACK for a test image (OD_TEST_REVERT_S > 0). The release image confirms
 *    itself in main() right after opendisplay_ble_init(), so an image whose GATT server never
 *    answers is confirmed and can only be recovered with a double-tap. A bench image built
 *    with OD_TEST_REVERT_S does NOT confirm itself. While it is unconfirmed:
 *      - a k_timer resets the SoC after OD_TEST_REVERT_S seconds (MCUboot then reverts);
 *      - from OD_TEST_REVERT_S + 30 s the main-loop watchdog feed is withheld, so the hardware
 *        watchdog (300 s) resets it even if the timer path were broken;
 *      - a fatal error resets it (CONFIG_RESET_ON_FATAL_ERROR), which MCUboot also reverts.
 *    Confirming (SMP image confirm over BLE, or `od confirm` on the console) cancels both.
 *
 * 2. A line-based console on the USB CDC port, which the release image never reads:
 *      od stat     uptime, image confirmed?, heap + thread stack headroom
 *      od confirm  boot_write_img_confirmed()
 *      od blind    ignore GATT evidence in od_img_confirm_app (dead-GATT simulation)
 *      od reboot   software reset (SREQ)
 *      od dfu      GPREGRET1 = 0x4E + reset: Adafruit SERIAL DFU (BLE-independent recovery;
 *                  the Adafruit bootloader then writes at 0x1000, i.e. over MCUboot -- recover
 *                  with the usb-2-install package)
 *      od panic    k_panic() -> fatal error -> reset
 *      od hang     lock the scheduler and spin: the watchdog fires (300 s)
 */
#include "od_test_bench.h"

#include "od_log.h"
#include "od_sec2_trace.h"

#include <string.h>

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/drivers/retained_mem.h>
#include <zephyr/drivers/uart.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/sys_heap.h>
#if defined(CONFIG_THREAD_ANALYZER)
#include <zephyr/debug/thread_analyzer.h>
#endif
#if defined(CONFIG_MCUBOOT_IMG_MANAGER)
#include <zephyr/dfu/mcuboot.h>
#endif

#ifndef OD_TEST_REVERT_S
#define OD_TEST_REVERT_S 0u
#endif

static volatile bool s_hold_feed;
static int64_t s_feed_deadline_ms;
static bool s_revert_armed;

static void revert_timer_fn(struct k_timer *t)
{
	ARG_UNUSED(t);
	/* ISR context: no logging that could block. The reset is the whole point. */
	NVIC_SystemReset();
}
K_TIMER_DEFINE(s_revert_timer, revert_timer_fn, NULL);

static bool image_confirmed(void)
{
#if defined(CONFIG_MCUBOOT_IMG_MANAGER)
	return boot_is_img_confirmed();
#else
	return true;
#endif
}

static void do_confirm(const char *by)
{
#if defined(CONFIG_MCUBOOT_IMG_MANAGER)
	int rc = boot_write_img_confirmed();

	od_log_warn("[BENCH] confirm (%s): rc=%d confirmed=%u", by, rc, (unsigned)image_confirmed());
#else
	od_log_warn("[BENCH] confirm (%s): no image manager", by);
#endif
}

void od_test_bench_image_boot(void)
{
#if defined(CONFIG_MCUBOOT_IMG_MANAGER)
	if (OD_TEST_REVERT_S == 0u) {
		return;   /* od_img_confirm_app decides alone */
	}
	if (image_confirmed()) {
		od_log_warn("[BENCH] image already confirmed; no revert timer");
		return;
	}
	s_feed_deadline_ms = k_uptime_get() + (int64_t)(OD_TEST_REVERT_S + 30u) * 1000;
	k_timer_start(&s_revert_timer, K_SECONDS(OD_TEST_REVERT_S), K_NO_WAIT);
	s_revert_armed = true;
	od_log_warn("[BENCH] UNCONFIRMED test image: reset in %u s unless confirmed "
		    "(watchdog backstop from %u s)", (unsigned)OD_TEST_REVERT_S,
		    (unsigned)(OD_TEST_REVERT_S + 30u));
#endif
}

void od_test_bench_tick(void)
{
	static int64_t next_ms;
	const int64_t now = k_uptime_get();

	if (!s_revert_armed || now < next_ms) {
		return;
	}
	next_ms = now + 2000;
	if (image_confirmed()) {
		k_timer_stop(&s_revert_timer);
		s_revert_armed = false;
		s_hold_feed = false;
		od_log_warn("[BENCH] image confirmed (by SMP or console): revert timer cancelled");
		return;
	}
	if (now >= s_feed_deadline_ms && !s_hold_feed) {
		s_hold_feed = true;
		od_log_warn("[BENCH] still unconfirmed past the timer: withholding the watchdog feed");
	}
}

bool od_test_bench_hold_feed(void)
{
	return s_hold_feed;
}

#if defined(CONFIG_THREAD_ANALYZER)
static void stack_cb(struct thread_analyzer_info *info)
{
	od_log_warn("[BENCH] stack %-20s used %4u / %4u B (headroom %u)", info->name,
		    (unsigned)info->stack_used, (unsigned)info->stack_size,
		    (unsigned)(info->stack_size - info->stack_used));
}
#endif

static void print_stats(void)
{
	extern struct k_heap _system_heap;
	struct sys_memory_stats hs;

	od_log_warn("[BENCH] uptime %u ms, image confirmed=%u, revert armed=%u",
		    (unsigned)k_uptime_get_32(), (unsigned)image_confirmed(), (unsigned)s_revert_armed);
#if defined(CONFIG_SYS_HEAP_RUNTIME_STATS)
	if (sys_heap_runtime_stats_get(&_system_heap.heap, &hs) == 0) {
		od_log_warn("[BENCH] system heap: free %u allocated %u max_allocated %u (pool %u)",
			    (unsigned)hs.free_bytes, (unsigned)hs.allocated_bytes,
			    (unsigned)hs.max_allocated_bytes, (unsigned)CONFIG_HEAP_MEM_POOL_SIZE);
	}
#else
	ARG_UNUSED(hs);
#endif
#if defined(CONFIG_THREAD_ANALYZER)
	thread_analyzer_run(stack_cb, 0);
#endif
}

static void enter_adafruit_serial_dfu(void)
{
	const struct device *gp1 = DEVICE_DT_GET_OR_NULL(DT_NODELABEL(gpregret1));
	const uint8_t magic = 0x4Eu;

	if (gp1 == NULL || !device_is_ready(gp1) || retained_mem_write(gp1, 0, &magic, 1) != 0) {
		od_log_error("[BENCH] gpregret1 unavailable; not resetting");
		return;
	}
	od_log_warn("[BENCH] GPREGRET1=0x4E: resetting into the Adafruit serial bootloader");
	k_sleep(K_MSEC(300));
	NVIC_SystemReset();
}

static void run_line(const char *l)
{
	if (strncmp(l, "od ", 3) != 0) {
		return;
	}
	l += 3;
	if (strcmp(l, "stat") == 0) {
		print_stats();
	} else if (strcmp(l, "confirm") == 0) {
		do_confirm("console");
	} else if (strcmp(l, "blind") == 0) {
		extern void od_img_confirm_app_bench_blind(void);
		od_img_confirm_app_bench_blind();
	} else if (strcmp(l, "reboot") == 0) {
		od_log_warn("[BENCH] software reset");
		k_sleep(K_MSEC(300));
		NVIC_SystemReset();
	} else if (strcmp(l, "dfu") == 0) {
		enter_adafruit_serial_dfu();
	} else if (strcmp(l, "panic") == 0) {
		od_log_warn("[BENCH] k_panic()");
		k_sleep(K_MSEC(300));
		k_panic();
	} else if (strcmp(l, "hang") == 0) {
		od_log_warn("[BENCH] scheduler locked, spinning: the watchdog will fire");
		k_sleep(K_MSEC(300));
		k_sched_lock();
		for (;;) {
		}
	} else {
		od_log_warn("[BENCH] unknown: %s", l);
	}
}

static void console_thread(void *a, void *b, void *c)
{
	const struct device *con = DEVICE_DT_GET_OR_NULL(DT_CHOSEN(zephyr_console));
	char line[32];
	size_t n = 0;

	ARG_UNUSED(a);
	ARG_UNUSED(b);
	ARG_UNUSED(c);
	if (con == NULL) {
		return;
	}
	/* The CDC ACM class only queues OUT transfers once RX is enabled through the interrupt
	 * API (usbd_cdc_acm.c: rx_fifo_work is submitted by irq_rx_enable or a consumed poll_in).
	 * Without this the host's first write blocks forever. No callback is installed: the bytes
	 * are read with poll_in below. */
	while (!device_is_ready(con)) {
		k_msleep(100);
	}
	uart_irq_rx_enable(con);
	for (;;) {
		unsigned char ch;

		if (!device_is_ready(con) || uart_poll_in(con, &ch) != 0) {
			k_msleep(20);
			continue;
		}
		if (ch == '\r' || ch == '\n') {
			line[n] = '\0';
			if (n > 0u) {
				run_line(line);
			}
			n = 0;
		} else if (n + 1u < sizeof line) {
			line[n++] = (char)ch;
		}
	}
}
K_THREAD_DEFINE(s_bench_console, 1536, console_thread, NULL, NULL, NULL, 14, 0, 2000);
