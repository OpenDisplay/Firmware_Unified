/* Telink SDK configuration for the OpenDisplay TLSR825x target. Force-included ahead of every
 * SDK-side translation unit, so vendor/common/default_config.h only fills what is not set here. */
#pragma once

/* Suspend between radio events, never deep-retention sleep: shared/core keeps its state in
 * ordinary .bss, which deep retention would discard. */
#define BLE_APP_PM_ENABLE                1
#define PM_DEEPSLEEP_RETENTION_ENABLE    0

/* OpenDisplay authenticates at the application layer (od_session); no BLE pairing. */
#define BLE_APP_SECURITY_ENABLE          0
#define BLE_OTA_SERVER_ENABLE            1   /* gated: see app_att.c */
#define APP_FLASH_PROTECTION_ENABLE      0
#define APP_BATT_CHECK_ENABLE            0

#define UART_PRINT_DEBUG_ENABLE          0
#define DEBUG_GPIO_ENABLE                0
#define UI_KEYBOARD_ENABLE               0
#define UI_LED_ENABLE                    0
#define UI_BUTTON_ENABLE                 0

/* 247-byte ATT MTU and 251-byte link-layer PDUs: a full OD pipe frame per notification. */
#define MTU_SIZE_SETTING                 247
#define ACL_CONN_MAX_RX_OCTETS           251
#define ACL_CONN_MAX_TX_OCTETS           251

#define CLOCK_SYS_CLOCK_HZ               48000000
#define MODULE_WATCHDOG_ENABLE           1
#define WATCHDOG_INIT_TIMEOUT            4000    /* ms; main loop and long waits clear it */

#include "vendor/common/default_config.h"
