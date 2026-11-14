/* Reset entry. Mirrors Telink tc_ble_single_sdk vendor/ble_sample/main.c (Apache-2.0) for the
 * 825x core; main() and the IRQ handler must run from RAM. */
#include "tl_common.h"
#include "drivers.h"
#include "stack/ble/ble.h"

#include "app.h"

_attribute_ram_code_ void irq_handler(void)
{
    irq_blt_sdk_handler();
}

_attribute_ram_code_ int main(void)
{
    int deep_ret_wakeup;

    /* OTA banks of 192 KB, new image at 0x40000: bank B ends at 0x6FFFF, clear of the SDK's
     * pairing/MAC/calibration sectors (0x74000-0x77FFF), ATC's tag type (0x79000) and the OD config
     * (0x7A000). The default (124 KB at 0x20000) is too small for this image to grow. Must precede
     * cpu_wakeup_init(). */
    blc_ota_setFirmwareSizeAndBootAddress(192, MULTI_BOOT_ADDR_0x40000);

    blc_pm_select_internal_32k_crystal();
    cpu_wakeup_init();
    deep_ret_wakeup = pm_is_MCU_deepRetentionWakeup();
    rf_drv_ble_init();
    gpio_init(!deep_ret_wakeup);
    clock_init(SYS_CLK_TYPE);

    /* A hang anywhere resets the chip after WATCHDOG_INIT_TIMEOUT instead of leaving it dead
     * until the battery is pulled; tlsr_port_crumb_boot() then reports where it hung. */
    wd_set_interval_ms(WATCHDOG_INIT_TIMEOUT, CLOCK_SYS_CLOCK_1MS);
    wd_start();

    if (deep_ret_wakeup) {
        user_init_deepRetn();
    } else {
        user_init_normal();
    }
    irq_enable();

    while (1) {
        wd_clear();
        main_loop();
    }
    return 0;
}
