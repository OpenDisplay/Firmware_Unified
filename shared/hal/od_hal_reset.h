/* od_hal_reset.h -- why this boot happened, 16 bytes that survive a planned reboot, and the
 * planned reboot itself.
 *
 * The boot-history mechanism (od_claim_mode.h: the claim gesture and the factory-reset pattern)
 * counts only boots a person caused by switching power or pressing the chip's reset pin
 * (SECURITY_V2 section 7.2). Everything else a remote peer or the firmware itself can provoke --
 * a software reset, a watchdog, a sleep wake -- must be reported as such, or the gesture becomes
 * remotely triggerable.
 *
 * CAUSE IS A BITMASK, read once per boot. Accumulating registers (nRF RESETREAS) MUST be cleared
 * by the first call, so a pin reset from days ago is not reported again. Later calls in the same
 * boot return the cached value. A target that also feeds od_hal_wdt_reset_reason() from the same
 * register reads it ONCE and serves both from the cached word.
 *
 * RETAINED: 16 bytes that survive a software reset and are lost (or garbage) after power loss.
 * Used only for the OS2C claim-mode record, which carries its own magic and CRC, so a target need
 * not zero or validate it. ESP32 RTC_NOINIT_ATTR, Zephyr __noinit, BG22 BURAM.
 *
 * CONTEXT: main loop / boot only.
 */
#ifndef OD_HAL_RESET_H
#define OD_HAL_RESET_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OD_HAL_RESET_POWER_ON             0x0001u /* cold start, supply applied */
#define OD_HAL_RESET_PIN                  0x0002u /* the chip's reset pin (nRESET / EN / CHIP_PU) */
#define OD_HAL_RESET_BROWNOUT             0x0004u /* supply dip, reported as such */
#define OD_HAL_RESET_POWER_ON_OR_BROWNOUT 0x0008u /* hardware that cannot tell the two apart (nRF52) */
#define OD_HAL_RESET_SOFTWARE             0x0010u /* esp_restart, sys_reboot, NVIC_SystemReset */
#define OD_HAL_RESET_WATCHDOG             0x0020u
#define OD_HAL_RESET_LOCKUP               0x0040u /* CPU lockup, panic */
#define OD_HAL_RESET_SLEEP_WAKE           0x0080u /* deep sleep / System OFF / EM4 wake, timer or pin */
#define OD_HAL_RESET_DEBUG                0x0100u /* debugger, JTAG/SWD */
#define OD_HAL_RESET_OTHER                0x0200u /* anything else, including "could not tell" */

/* The cause of THIS boot. Never 0: a target that cannot tell returns OD_HAL_RESET_OTHER. */
uint32_t od_hal_reset_cause(void);

#define OD_HAL_RETAINED_LEN 16u

/* false = this target has no retained RAM (the record is then never found, which only means a
 * claim-mode session does not continue across a planned reboot). */
bool od_hal_retained_read(uint8_t buf[OD_HAL_RETAINED_LEN]);
bool od_hal_retained_write(const uint8_t buf[OD_HAL_RETAINED_LEN]);
void od_hal_retained_clear(void);

/* Planned software reset. Does not return. */
void od_hal_reset_reboot(void);

#ifdef __cplusplus
}
#endif

#endif /* OD_HAL_RESET_H */
