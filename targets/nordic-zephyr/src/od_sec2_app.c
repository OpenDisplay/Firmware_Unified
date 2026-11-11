/* Nordic half of shared/core/od_sec2_app.h: clock, uid, panel facts, the parsed 0x27. */

#include "od_sec2_app.h"
#include "od_sec2_wire.h"

#include "opendisplay_ble.h"
#include "opendisplay_config_parser.h"

#include <string.h>
#include <zephyr/drivers/hwinfo.h>
#include <zephyr/kernel.h>

uint32_t od_sec2_app_now_ms(void)
{
	return k_uptime_get_32();
}

/* SECURITY_V2 4.3: the first 8 bytes hwinfo writes, in buffer order. Zeroed first so a driver
 * that writes fewer leaves the same answer on every boot rather than stack residue. The real
 * per-SoC bytes are still to be recorded from a board (H11). */
void od_sec2_app_uid(uint8_t uid[8])
{
	uint8_t id[8] = {0};

	(void)hwinfo_get_device_id(id, sizeof(id));
	memcpy(uid, id, sizeof(id));
}

uint8_t od_sec2_app_dflags(void)
{
	const struct SecurityConfig *sec = od_get_parsed_security();
	const struct od_config *cfg = opendisplay_get_global_config();
	uint8_t f = 0u;

	if (cfg != NULL && cfg->loaded && cfg->display_count > 0u) {
		f |= OD_SEC2_DFLAG_HAS_PANEL;
	}
	if (sec != NULL && (sec->flags & OD_SECURITY_FLAG_RESET_PIN_ENABLED) != 0u) {
		f |= OD_SEC2_DFLAG_HAS_RESET_PIN;
	}
	return f;
}

const struct SecurityConfig *od_sec2_app_security(void)
{
	return od_get_parsed_security();
}
