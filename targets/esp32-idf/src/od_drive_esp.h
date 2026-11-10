/* Transfer hooks for a CMD_DRIVE_WRITE_START stream on ESP32. display_service.cpp's od_xfer_app
 * implementation hands a drive stream here instead of to bb_epaper. */
#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool od_drive_esp_active(void);
bool od_drive_esp_write(uint32_t stream_offset, const uint8_t *data, size_t len);
/* Finishes the last pass, sleeps the panel and drops the rail. */
bool od_drive_esp_refresh(void);
/* Ends a drive stream, if any, and powers the panel down. */
void od_drive_esp_clear(void);
