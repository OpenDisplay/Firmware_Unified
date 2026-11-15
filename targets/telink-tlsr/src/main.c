/* Boot and main loop, after the SDK's ble_sample main.c / app.c, with two deliberate
 * differences: the address comes from the flash UID as in the legacy firmware and is never written (od_tlsr_mac.h), and there is no SMP. */
#include "od_tlsr_sdk.h"

#include "od_tlsr_ble.h"
#include "od_tlsr_ota.h"
#include "od_tlsr_target.h"

#include "od_cfg_index.h"
#include "od_claim_mode.h"
#include "od_config.h"
#include "od_hal_time.h"
#include "od_sec2_app.h"

_attribute_ram_code_ void irq_handler(void)
{
    irq_blt_sdk_handler();
}

static void od_boot(void)
{
    struct od_security_boot_report rep;
    uint8_t mac[6];
    bool loaded;

    random_generator_init();
    blc_readFlashSize_autoConfigCustomFlashSector();
    blc_app_loadCustomizedParameters_normal();
    flash_protection_init();
    od_tlsr_hal_boot();
    od_tlsr_ota_reset();

    loaded = od_tlsr_config_load();
    od_security_boot(od_sec2_app_security(), loaded, false, &rep);
    (void)rep;
    (void)od_cfg_index_build();
    od_tlsr_display_init();
    od_tlsr_display_request_boot();

    od_tlsr_mac(mac);
    od_tlsr_ble_init(mac);
}

/* Also the entry after a deep-retention wake (od_tlsr_ble.c, power management): every static is
 * as it was before the sleep, so only the hardware is set up again. The analog pull resistors
 * survived the sleep, hence gpio_init(0). */
_attribute_ram_code_ int main(void)
{
    int retention_wake;

    blc_pm_select_internal_32k_crystal();
    cpu_wakeup_init();
    retention_wake = pm_is_MCU_deepRetentionWakeup();
    rf_drv_ble_init();
    gpio_init(!retention_wake);
    clock_init(SYS_CLK_TYPE);
    wd_set_interval_ms(WATCHDOG_INIT_TIMEOUT, CLOCK_SYS_CLOCK_1MS);
    wd_start();

    if (retention_wake) {
#if OD_TLSR_FAULT_TEST
        while (od_tlsr_fault_armed()) {
        }
#endif
        od_tlsr_ble_retention_wake();
        od_tlsr_display_retention_wake();
    } else {
        od_boot();
    }
    irq_enable();

    for (;;) {
        wd_clear();
        blt_sdk_main_loop();
        od_claim_mode_tick();
        od_tlsr_ble_loop(od_hal_uptime_ms());
        od_tlsr_display_service();
        od_tlsr_ble_power(od_tlsr_display_idle());
    }
}
