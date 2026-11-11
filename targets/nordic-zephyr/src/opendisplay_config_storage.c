#include "opendisplay_config_storage.h"

#include <stddef.h>

#include "od_config_store.h"

/* Framing, CRC and bounds are shared/core's; the medium is src/od_hal_nvs.c. What is left here
 * is the four names this target's callers use and the workspace the record is assembled in. */

bool initConfigStorage(void)
{
	return od_config_store_init() == OD_CONFIG_STORE_OK;
}

bool saveConfig(uint8_t *config_data, uint32_t len)
{
	/* Static, not on the stack: the record is larger than all of CONFIG_MAIN_STACK_SIZE
	 * (4096). Config writes are serialised on the main thread, so one is enough. */
	static uint8_t workspace[OD_CONFIG_STORE_MAX_RECORD];

	if (config_data == NULL) {
		return false;
	}
	return od_config_store_save(workspace, sizeof(workspace), config_data, len)
	       == OD_CONFIG_STORE_OK;
}

bool loadConfig(uint8_t *config_data, uint32_t *len)
{
	return od_config_store_load(config_data, len) == OD_CONFIG_STORE_OK;
}

bool clearStoredConfig(void)
{
	/* Reports what the medium actually did. This used to discard settings_delete()'s result
	 * and return true regardless, which told a caller the config was gone while it was still
	 * on the device -- see DIVERGENCE_MATRIX 17. */
	return od_config_store_clear() == OD_CONFIG_STORE_OK;
}
