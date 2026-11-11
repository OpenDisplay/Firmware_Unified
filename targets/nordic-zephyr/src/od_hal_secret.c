/* Zephyr settings implementation of od_hal_secret. Contract: shared/hal/od_hal_secret.h.
 *
 * Three settings keys beside "od/config", in the same NVS partition. SECURITY_V2 H13 asks for a
 * separate area so a config-partition wipe cannot reach the key; that is open (the partition
 * layout is a DTS change on three boards). settings_save_one replaces a value atomically. */

#include "od_hal_secret.h"

#include <zephyr/settings/settings.h>

#include "od_hal_nvs.h"

static const char *const s_keys[3] = { "od/os2k", "od/os2s", "od/os2r" };

bool od_hal_secret_read(enum od_hal_secret_id id, uint8_t *buf, uint16_t len)
{
	uint8_t tmp[32];
	ssize_t got;
	uint16_t i;

	if (buf == NULL || id > OD_HAL_SECRET_MARKER || len > sizeof(tmp) ||
	    od_hal_nvs_init() != OD_HAL_NVS_OK) {
		return false;
	}
	got = settings_load_one(s_keys[id], tmp, sizeof(tmp));
	if (got != (ssize_t)len) {
		return false;
	}
	for (i = 0; i < len; ++i) {
		buf[i] = tmp[i];
	}
	return true;
}

bool od_hal_secret_write(enum od_hal_secret_id id, const uint8_t *buf, uint16_t len)
{
	return buf != NULL && id <= OD_HAL_SECRET_MARKER && od_hal_nvs_init() == OD_HAL_NVS_OK &&
	       settings_save_one(s_keys[id], buf, len) == 0;
}

bool od_hal_secret_erase(enum od_hal_secret_id id)
{
	return id <= OD_HAL_SECRET_MARKER && od_hal_nvs_init() == OD_HAL_NVS_OK &&
	       settings_delete(s_keys[id]) == 0;
}
