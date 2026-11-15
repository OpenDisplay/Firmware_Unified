/* OpenDisplay's configuration of the Telink B85 BLE SDK.
 *
 * The SDK picks its application config by project macro: vendor/common/user_config.h includes
 * "vendor/ble_sample/app_config.h" when __PROJECT_8258_BLE_SAMPLE__ is set, through the include
 * path. This overlay directory sits ahead of the SDK on that path, so this file is the one the
 * SDK's own sources compile against; the SDK tree itself is never edited. */
#pragma once

/* Deep-retention sleep is used by the OpenDisplay image (src/od_tlsr_ble.c). The second switch
 * arms the link script's "retained RAM < 32 KB" assert; the SDK requires the first with it. */
#define BLE_APP_PM_ENABLE                1
#define PM_DEEPSLEEP_RETENTION_ENABLE    1
#define TEST_CONN_CURRENT_ENABLE         0

/* No link-layer pairing: OpenDisplay authenticates at the application layer (SEC2). Pairing
 * would also write bonding records to flash at 0x74000, a sector these tags' previous
 * firmware may own. */
#define BLE_APP_SECURITY_ENABLE          0

/* Telink's own OTA service stays out: it is unauthenticated and writes the other boot bank
 * with its own flag protocol. src/od_tlsr_ota.c is the update path. */
#define BLE_OTA_SERVER_ENABLE            0

/* Compiled for flash_protection_init() and flash_unlock() only: od_tlsr_flash.c unlocks before
 * its first write, because a previous firmware may have left block protection set. The SDK's
 * lock policy (app_flash_protection_operation) is never registered, so nothing is locked. */
#define APP_FLASH_PROTECTION_ENABLE      1
#define APP_BATT_CHECK_ENABLE            0

#define DEBUG_GPIO_ENABLE                0
#define UART_PRINT_DEBUG_ENABLE          0
#define APP_LOG_EN                       0
#define APP_SMP_LOG_EN                   0
#define APP_KEY_LOG_EN                   0
#define APP_CONTR_EVENT_LOG_EN           0
#define APP_HOST_EVENT_LOG_EN            0
#define APP_OTA_LOG_EN                   0
#define APP_FLASH_INIT_LOG_EN            0
#define APP_FLASH_PROT_LOG_EN            0
#define APP_BATT_CHECK_LOG_EN            0

/* The SDK insists on a board header; the EVK one only supplies pin names the app never
 * touches (no keyboard, LED or button UI is enabled below). Tag pins come from the device
 * config, not from a board file. */
#define BOARD_SELECT                     BOARD_825X_EVK_C1T139A30
#define UI_KEYBOARD_ENABLE               0
#define UI_LED_ENABLE                    0
#define UI_BUTTON_ENABLE                 0

#define USED_DEEP_ANA_REG                DEEP_ANA_REG0
#define LOW_BATT_FLG                     BIT(0)
#define CONN_DEEP_FLG                    BIT(4)

/* 24 MHz, as the tags' previous firmware runs them. */
#define CLOCK_SYS_CLOCK_HZ               24000000

/* Fed from the main loop; the install routine stops it before it erases. A hang therefore
 * reboots instead of leaving a tag that neither advertises nor answers. */
#define MODULE_WATCHDOG_ENABLE           1
#define WATCHDOG_INIT_TIMEOUT            4000 /* ms */

#include "vendor/common/default_config.h"
