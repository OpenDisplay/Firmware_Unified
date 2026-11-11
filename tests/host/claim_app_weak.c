/* claim_app_weak.c -- inert, WEAK defaults for the seams claim mode, the factory reset and the
 * config policy need (od_claim_app.h, od_hal_reset.h, od_sec2_app_display_busy), so every host
 * test that links od_shared keeps linking. Host-only: weak symbols are a GCC/Clang feature and
 * never reach a target. A test that exercises one of these defines it itself (strong). */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "od_claim_app.h"
#include "od_config_store.h"
#include "od_hal_reset.h"
#include "od_sec2_app.h"

#define WEAK __attribute__((weak))

static uint8_t s_ws[OD_CONFIG_STORE_MAX_RECORD];

WEAK bool od_claim_app_draw(enum od_setup_screen kind, uint32_t code) { (void)kind; (void)code; return false; }
WEAK void od_claim_app_signal(enum od_claim_signal sig, uint8_t arg) { (void)sig; (void)arg; }
WEAK bool od_claim_app_pin_input(uint8_t pin, bool up, bool down) { (void)pin; (void)up; (void)down; return false; }
WEAK int od_claim_app_pin_read(uint8_t pin) { (void)pin; return -1; }
WEAK void od_claim_app_wait_ms(uint32_t ms) { (void)ms; }
WEAK bool od_claim_app_erase_images(void) { return true; }
WEAK bool od_claim_app_erase_bonds(void) { return true; }
WEAK bool od_claim_app_scrub_extras(void) { return true; }
WEAK uint8_t *od_claim_app_workspace(uint32_t *cap) { *cap = sizeof s_ws; return s_ws; }
WEAK bool od_claim_app_workspace_busy(void) { return false; }
WEAK void od_claim_app_config_changed(void) { }

WEAK uint32_t od_hal_reset_cause(void) { return OD_HAL_RESET_OTHER; }
WEAK bool od_hal_retained_read(uint8_t buf[OD_HAL_RETAINED_LEN]) { (void)buf; return false; }
WEAK bool od_hal_retained_write(const uint8_t buf[OD_HAL_RETAINED_LEN]) { (void)buf; return false; }
WEAK void od_hal_retained_clear(void) { }
WEAK void od_hal_reset_reboot(void) { }

WEAK bool od_sec2_app_display_busy(void) { return false; }
