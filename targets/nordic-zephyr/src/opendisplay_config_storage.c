#include "opendisplay_config_storage.h"

#include <stddef.h>

#include "od_cfg_policy.h"
#include "od_config_store.h"

#include <zephyr/kernel.h>

/* od_config_store_hold_writes() runs on the display work queue during the boot render and on
 * main otherwise; saves run on main. A k_mutex, not a spinlock: the save holds it across the NVS
 * write. */
K_MUTEX_DEFINE(s_config_store_lock);

void od_config_store_app_lock(void)
{
	(void)k_mutex_lock(&s_config_store_lock, K_FOREVER);
}

void od_config_store_app_unlock(void)
{
	(void)k_mutex_unlock(&s_config_store_lock);
}

/* Framing, CRC and bounds are shared/core's; the medium is src/od_hal_nvs.c. What is left here
 * is the four names this target's callers use and the workspace the record is assembled in. */

bool initConfigStorage(void)
{
	return od_config_store_init() == OD_CONFIG_STORE_OK;
}

/* Static, not on the stack: the record is larger than all of CONFIG_MAIN_STACK_SIZE (4096).
 * Config writes and the SEC2 policy's own rewrites are serialised on the main thread, so one is
 * enough for both (od_claim_app_workspace). */
static uint8_t s_workspace[OD_CONFIG_STORE_MAX_RECORD];

uint8_t *opendisplay_config_workspace(uint32_t *cap)
{
	*cap = (uint32_t)sizeof(s_workspace);
	return s_workspace;
}

bool saveConfig(uint8_t *config_data, uint32_t len)
{
	if (config_data == NULL) {
		return false;
	}
	/* Every store meets the SEC2 rules (secrets need a key, the 0x27 key field). */
	return od_cfg_policy_save(s_workspace, sizeof(s_workspace), config_data, len)
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
	return od_cfg_policy_clear_store() == OD_CONFIG_STORE_OK;   /* SECURITY_V2 10.6 */
}
