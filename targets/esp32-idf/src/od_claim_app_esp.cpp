/* ESP32 half of shared/core/od_claim_app.h (except od_claim_app_draw, boot_screen side) and
 * shared/hal/od_hal_reset.h. */

#include "od_claim_app.h"
#include "od_hal_reset.h"

#include <string.h>

#include "esp_attr.h"
#include "esp_rom_sys.h"
#include "esp_system.h"
#include "nvs.h"
#include "nvs_flash.h"

#include "config_parser.h"
#include "od_hal_gpio.h"
#include "od_hal_nvs.h"
extern "C" {
#include "od_hal_nvs_esp.h"   /* a C header without its own extern "C" guard */
}
#include "od_log.h"
#include "od_sec2_trace.h"
#include "od_watchdog_app.h"
#include "wifi_service.h"

void reboot();

/* ------------------------------------------------------------------- reset cause --- */

static bool s_rtc_wake;

/* main.cpp, before securityBoot(): this power-on was the PCF8563 alarm re-applying the rail
 * (a power-off sleep), not a person. */
void od_esp_reset_note_rtc_wake(void)
{
    s_rtc_wake = true;
}

/* SECURITY_V2 7.2 table. esp_reset_reason() is a stored value, not an accumulating register,
 * so there is nothing to clear. ESP_RST_POWERON also covers the EN / CHIP_PU button. */
extern "C" uint32_t od_hal_reset_cause(void)
{
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:   return s_rtc_wake ? OD_HAL_RESET_SLEEP_WAKE : OD_HAL_RESET_POWER_ON;
    case ESP_RST_EXT:       return OD_HAL_RESET_PIN;
    case ESP_RST_SW:        return OD_HAL_RESET_SOFTWARE;
    case ESP_RST_BROWNOUT:  return OD_HAL_RESET_BROWNOUT;
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:       return OD_HAL_RESET_WATCHDOG;
    case ESP_RST_PANIC:     return OD_HAL_RESET_LOCKUP;
    case ESP_RST_DEEPSLEEP: return OD_HAL_RESET_SLEEP_WAKE;
    case ESP_RST_JTAG:      return OD_HAL_RESET_DEBUG;
    default:                return OD_HAL_RESET_OTHER;
    }
}

/* OS2C. RTC_NOINIT: survives a software reset (the bootloader reloads only the initialised RTC
 * segment), garbage after power loss -- the record's magic and CRC reject that. */
static RTC_NOINIT_ATTR uint8_t s_retained[OD_HAL_RETAINED_LEN];

extern "C" bool od_hal_retained_read(uint8_t buf[OD_HAL_RETAINED_LEN])
{
    memcpy(buf, s_retained, OD_HAL_RETAINED_LEN);
    return true;
}

extern "C" bool od_hal_retained_write(const uint8_t buf[OD_HAL_RETAINED_LEN])
{
    memcpy(s_retained, buf, OD_HAL_RETAINED_LEN);
    return true;
}

extern "C" void od_hal_retained_clear(void)
{
    memset(s_retained, 0, OD_HAL_RETAINED_LEN);
}

/* reboot() tears NimBLE (and WiFi) down first; esp_restart() alone leaves the radio half-up. */
extern "C" void od_hal_reset_reboot(void)
{
    reboot();
}

/* --------------------------------------------------------------------- signalling --- */

/* The panel is the primary signal; LED/buzzer patterns are not implemented on this target yet
 * (od_claim_app.h allows a no-op). */
extern "C" void od_claim_app_signal(enum od_claim_signal sig, uint8_t arg)
{
    OD_SEC2_TRACE("signal %u arg %u (LED/buzzer pattern; no-op on this target)", (unsigned)sig,
                  (unsigned)arg);
    (void)sig;
    (void)arg;
}

/* ---------------------------------------------------------------------- reset pin --- */

/* 0x27 reset_pin is a GPIO number, as the binary inputs use (od_hal_gpio.h). */
extern "C" bool od_claim_app_pin_input(uint8_t pin, bool pull_up, bool pull_down)
{
    if (!od_hal_gpio_pin_valid(pin)) {
        return false;
    }
    od_hal_gpio_config_input(pin, pull_up, pull_down);
    return true;
}

extern "C" int od_claim_app_pin_read(uint8_t pin)
{
    const int v = od_hal_gpio_read(pin);

    return v < 0 ? -1 : (v != 0 ? 1 : 0);
}

extern "C" void od_claim_app_wait_ms(uint32_t ms)
{
    while (ms > 0u) {
        const uint32_t step = ms < 10u ? ms : 10u;

        esp_rom_delay_us(step * 1000u);
        ms -= step;
        od_watchdog_app_service();
    }
}

/* ------------------------------------------------------------------ factory reset --- */

/* No image store on this target. */
extern "C" bool od_claim_app_erase_images(void)
{
    return true;
}

/* FACTORY_RESET.md R5b: bonds and extras. NimBLE here persists no bonds, but NVS is
 * log-structured: nvs_erase_key() and nvs_erase_all() only mark entries erased, so every older
 * config record (WiFi password, FindMy keys), every older OS2K and ESP-IDF's nvs.net80211 copy
 * stays readable in the partition until its page is recycled. A factory reset must leave
 * nothing of the owner's behind (SECURITY_V2 10.4), so R5b erases the whole partition.
 *
 * It runs at boot, before the radios, so no NVS handle is open. It also takes the reset marker,
 * the config and OS2S with it: R5c then finds no config, R6 writes a fresh OS2S and R7 finds no
 * marker. A power cut after this erase and before R6 boots a blank partition, which is a
 * virgin device: FRESH claim mode, exactly where R6-R8 lead. */
extern "C" bool od_claim_app_erase_bonds(void)
{
    esp_err_t err;

    (void)nvs_flash_deinit();
    err = nvs_flash_erase();
    od_hal_nvs_esp_cache_drop();
    if (err != ESP_OK) {
        od_log_error("factory reset: NVS partition erase failed: %s", esp_err_to_name(err));
    }
    /* Re-init either way: a failed erase still leaves a partition the later stages must use. */
    err = nvs_flash_init();
    if (err != ESP_OK) {
        od_log_error("factory reset: NVS re-init failed: %s", esp_err_to_name(err));
        return false;
    }
    return true;
}

/* SECURITY_V2 10.4 scrub step 2 (A16): ESP-IDF's own copy of the WiFi credentials. */
extern "C" bool od_claim_app_scrub_extras(void)
{
#ifdef OPENDISPLAY_HAS_WIFI
    nvs_handle_t h;
    esp_err_t err = nvs_open("nvs.net80211", NVS_READWRITE, &h);

    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return true;
    }
    if (err != ESP_OK) {
        od_log_error("scrub: nvs.net80211 open failed: %d", (int)err);
        return false;
    }
    err = nvs_erase_all(h);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err == ESP_OK;
#else
    return true;
#endif
}

/* ----------------------------------------------------------------- config workspace --- */

extern "C" uint8_t *od_claim_app_workspace(uint32_t *cap)
{
    return getConfigWorkspace(cap);
}

/* The chunked write assembles into g_configAsm, never into this workspace. */
extern "C" bool od_claim_app_workspace_busy(void)
{
    return false;
}

extern "C" void od_claim_app_config_changed(void)
{
    configChangedBySecurityPolicy();
}
