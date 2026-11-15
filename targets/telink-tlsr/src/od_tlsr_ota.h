/* Firmware update for TLSR825x tags: stage, verify, install.
 *
 * PROVISIONAL. It takes any sealed Telink image (an OpenDisplay build or a legacy firmware
 * release); images are not signed, and the od_health trust rule is not applied.
 *
 * SEC2 gate (SECURITY_V2 6.1, the predicate the ESP32 and Nordic DFU transports use): while
 * od_sec2_dfu_needs_unlock() -- CLAIMED, LOCKED or claim mode -- every op except STATUS is
 * refused with E_LOCKED until od_tlsr_ota_unlock(), which an authenticated CMD_ENTER_DFU calls.
 * The unlock lasts until the link closes, and BEGIN and INSTALL also need the session that sent
 * ENTER_DFU to be live; once it is not, the gate is shut until the next ENTER_DFU. DATA and
 * FINISH do not ask, so an upload may outlast the session. LOCKED never opens a session, so it
 * stays shut; a claimed tag whose key is lost is opened by the factory-reset power-cycle
 * pattern.
 *
 * Wire (one GATT characteristic, write with response; replies are notifications):
 *   0x01 BEGIN  u32 len           erase staging; notify 81 00 <len> when ready
 *   0x02 DATA   u32 off, bytes    sequential; an error latches and is reported by STATUS/FINISH
 *   0x03 FINISH                   verify; notify 83 00 <len> when the image checks out
 *   0x04 INSTALL                  notify 84 00, then copy staging over the running image and reboot
 *   0x05 STATUS                   notify 85 <state> <u32 next> <u32 len> <u8 err>
 *                                        <u8 boot cause> <u8 unplanned resets>
 *   0x06 ABORT                    back to idle
 * Integers little-endian. Reply: op|0x80, status (0 ok, else enum od_tlsr_ota_err), u32 arg.
 *
 * BEGIN, every erase step, DATA, the flag write and INSTALL first check the supply
 * (od_tlsr_flash_supply_ok()). A refused INSTALL leaves the image staged and verified.
 *
 * The staged image's boot-flag byte (offset 8) is held back and written only once the whole
 * image has verified, so the ROM never sees a partially staged image as bootable. */
#ifndef OD_TLSR_OTA_H
#define OD_TLSR_OTA_H

#include <stdbool.h>
#include <stdint.h>

enum od_tlsr_ota_state {
    OD_TLSR_OTA_IDLE = 0,
    OD_TLSR_OTA_ERASING,
    OD_TLSR_OTA_RECEIVING,
    OD_TLSR_OTA_VERIFYING,
    OD_TLSR_OTA_VERIFIED,
    OD_TLSR_OTA_INSTALLING,
    OD_TLSR_OTA_FAILED
};

enum od_tlsr_ota_err {
    OD_TLSR_OTA_OK = 0,
    OD_TLSR_OTA_E_STATE = 1,
    OD_TLSR_OTA_E_LENGTH = 2,
    OD_TLSR_OTA_E_OFFSET = 3,
    OD_TLSR_OTA_E_FLASH = 4,
    OD_TLSR_OTA_E_HEADER = 5,
    OD_TLSR_OTA_E_CRC = 6,
    OD_TLSR_OTA_E_OPCODE = 7,
    OD_TLSR_OTA_E_LOCKED = 8,
    OD_TLSR_OTA_E_SUPPLY = 9   /* supply below OD_TLSR_FLASH_MIN_MV: nothing written */
};

#define OD_TLSR_OTA_REPLY_MAX 13u

void od_tlsr_ota_reset(void);

/* One characteristic write. Returns the reply length (0: nothing to notify now). */
uint8_t od_tlsr_ota_write(const uint8_t *data, uint16_t len, uint8_t reply[OD_TLSR_OTA_REPLY_MAX]);

/* Main loop: one bounded step of erase or verify, or the install once it is due. */
void od_tlsr_ota_service(uint32_t now_ms);

/* CMD_ENTER_DFU succeeded on this link (od_tlsr_cmd.c): open the gate (see the header). */
void od_tlsr_ota_unlock(void);

/* The link closed. The gate shuts; a verified image stays staged; anything in progress is
 * dropped. */
void od_tlsr_ota_link_closed(void);

enum od_tlsr_ota_state od_tlsr_ota_state(void);

/* Target hooks. */
/* od_sec2_dfu_needs_unlock(). */
bool od_tlsr_ota_app_needs_unlock(void);
/* The session that sent the unlocking ENTER_DFU is still live. Asked at BEGIN and INSTALL while
 * the gate is needed and open; false shuts it, so a later session on the same link has to send
 * ENTER_DFU again. */
bool od_tlsr_ota_app_unlock_live(void);
void od_tlsr_ota_app_notify(const uint8_t *reply, uint8_t len);
/* Called once, just before od_tlsr_install(): feed the watchdog, mark the reboot as planned. */
void od_tlsr_ota_app_before_install(void);
/* Why this boot happened, for STATUS: info[0] an OD_TLSR_BOOT_* value, info[1] the unplanned
 * resets since power was applied (0xFF, 0xFF where the image does not track it). */
#define OD_TLSR_BOOT_POWER_ON  0u /* power applied, a brownout included */
#define OD_TLSR_BOOT_PLANNED   1u /* od_hal_reset_reboot() or the updater's install */
#define OD_TLSR_BOOT_UNPLANNED 2u /* watchdog, reset pin */
void od_tlsr_ota_app_boot_info(uint8_t info[2]);
/* Copy the staged image of `len` bytes over the running one and reboot. Runs from RAM; does
 * not return (od_tlsr_install.c). */
void od_tlsr_install(uint32_t len);

#endif /* OD_TLSR_OTA_H */
