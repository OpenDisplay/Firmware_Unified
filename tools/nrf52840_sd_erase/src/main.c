/* One-shot migration: erase the s140 SoftDevice region on an nRF52840 that runs the
 * Adafruit bootloader, reclaiming 0x1000..0x27000 (152 kB) for the application.
 *
 * WHY THIS IS SAFE TO INTERRUPT. Only 0x1000..0x27000 is touched. The MBR (0x0..0x1000),
 * UICR (NRFFW[0] -> 0xF4000) and the Adafruit bootloader (0xF4000..0x100000) are never
 * written, so losing power mid-erase cannot remove the recovery path: a double-tap still
 * reaches serial DFU at 2886:0064, and s140 can be reflashed there to revert.
 *
 * WHAT IT PROVES. The Adafruit bootloader is believed to fall back to starting the
 * application at MBR_SIZE (0x1000) instead of SD_SIZE_GET(MBR_SIZE) (0x27000) when the
 * SoftDevice magic at 0x3004 is absent. This image resets WITHOUT requesting DFU, which
 * makes the outcome self-discriminating:
 *
 *   lands in serial DFU (2886:0064)  -> the bootloader now looks at 0x1000, found erased
 *                                       flash, and stayed in DFU. The fallback is real.
 *   comes back as this app           -> the bootloader still jumps to 0x27000. The
 *                                       fallback does not exist in this build; revert by
 *                                       reflashing the SoftDevice.
 *
 * Requesting DFU with GPREGRET1 = 0x4E would force the first outcome either way and prove
 * nothing, which is why it is deliberately not done.
 *
 * Registers are driven directly rather than through the nrfx HAL: those helpers are
 * declare-only in this build and do not link.
 */
#include <zephyr/kernel.h>
#include <zephyr/sys/printk.h>
#include <soc.h>

#define SD_MAGIC_ADDR 0x00003004u
#define SD_MAGIC      0x51B1E5DBu
#define ERASE_START   0x00001000u
#define ERASE_END     0x00027000u

#define WDT_RELOAD    0x6E524635u

static uint32_t rd(uint32_t a) { return *(volatile uint32_t *)a; }

/* The production firmware arms the watchdog at 300 s and it cannot be stopped once
 * started, so it is still running here. 38 page erases at ~85 ms sit well inside that,
 * but feeding is free and removes the dependency on that arithmetic staying true. */
static void feed_wdt(void)
{
	if (NRF_WDT->RUNSTATUS == 0u) {
		return;
	}
	for (int i = 0; i < 8; i++) {
		if (NRF_WDT->RREN & (1u << i)) {
			NRF_WDT->RR[i] = WDT_RELOAD;
		}
	}
}

static void erase_page(uint32_t addr)
{
	NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Een;
	while (NRF_NVMC->READY == 0u) {
	}
	NRF_NVMC->ERASEPAGE = addr;
	while (NRF_NVMC->READY == 0u) {
	}
	NRF_NVMC->CONFIG = NVMC_CONFIG_WEN_Ren;
	while (NRF_NVMC->READY == 0u) {
	}
}

int main(void)
{
	uint32_t before, after;
	unsigned int key;

	/* Long enough for USB CDC to enumerate and a host to attach before anything is
	 * erased -- the log is the only record of what this did. */
	k_sleep(K_SECONDS(12));

	before = rd(SD_MAGIC_ADDR);
	printk("\n=== SoftDevice erase migration ===\n");
	printk("0x3004 before: %08x (%s)\n", before,
	       before == SD_MAGIC ? "SoftDevice PRESENT" : "already absent");

	if (before != SD_MAGIC) {
		printk("nothing to do; resetting\n");
	} else {
		printk("erasing %08x..%08x (%u pages)\n", ERASE_START, ERASE_END,
		       (ERASE_END - ERASE_START) / 4096u);
		key = irq_lock();
		for (uint32_t a = ERASE_START; a < ERASE_END; a += 0x1000u) {
			feed_wdt();
			erase_page(a);
		}
		irq_unlock(key);

		after = rd(SD_MAGIC_ADDR);
		printk("0x3004 after:  %08x (%s)\n", after,
		       after == SD_MAGIC ? "STILL PRESENT - erase FAILED" : "erased");
	}

	printk("resetting WITHOUT a DFU request.\n");
	printk("  -> serial DFU (2886:0064) means the bootloader now starts at 0x1000\n");
	printk("  -> this app again means it still starts at 0x27000\n");
	k_sleep(K_SECONDS(2));
	NVIC_SystemReset();
	return 0;
}
