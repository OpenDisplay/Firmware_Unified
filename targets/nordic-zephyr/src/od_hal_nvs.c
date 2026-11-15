/* Zephyr settings implementation of od_hal_nvs. See shared/hal/od_hal_nvs.h for the contract.
 *
 * WHY THERE IS A CACHE. The settings NVS backend registers no csi_load_one, so
 * settings_load_one() falls back to a full csi_load scan of the partition, and the only read
 * primitive under it -- nvs_read() -- starts at byte 0. There is no offset read at any layer.
 * So the record is read once into s_cache and every offset is served from there. That also
 * keeps od_hal_nvs_size() from costing a second scan: settings_get_val_len() exists, but on
 * this backend it walks the partition again to learn a number the fill already knows.
 */

#include "od_hal_nvs.h"

#include <errno.h>
#include <string.h>
#include <zephyr/settings/settings.h>

#include "od_config_asm.h"   /* OD_CONFIG_MAX_SIZE */
#include "od_log.h"

#define OD_SETTINGS_KEY "od/config"

/* 16-byte record header plus the payload cap. The framing is shared/core's; this is only how
 * much of it can arrive. */
#define OD_HAL_NVS_MAX_RECORD (16u + OD_CONFIG_MAX_SIZE)

/* Static, not on a stack: CONFIG_MAIN_STACK_SIZE is 4096 and this is larger than all of it. */
static uint8_t  s_cache[OD_HAL_NVS_MAX_RECORD];
static uint32_t s_cache_len;
static bool     s_cache_valid;
static bool     s_ready;

int od_hal_nvs_init(void)
{
	if (s_ready) {
		return OD_HAL_NVS_OK;
	}
	if (settings_subsys_init() != 0) {
		return OD_HAL_NVS_EIO;
	}
	s_ready = true;
	return OD_HAL_NVS_OK;
}

static int cache_fill(void)
{
	ssize_t got;

	if (s_cache_valid) {
		return OD_HAL_NVS_OK;
	}
	if (od_hal_nvs_init() != OD_HAL_NVS_OK) {
		return OD_HAL_NVS_EIO;
	}

	got = settings_load_one(OD_SETTINGS_KEY, s_cache, sizeof(s_cache));
	if (got < 0) {
		return OD_HAL_NVS_EIO;
	}
	if (got == 0) {
		return OD_HAL_NVS_ENOENT;
	}
	/* settings_load_one() reports the FULL stored length even when it truncated into a
	 * smaller buffer, so an over-size record is only detectable here, by comparing its
	 * answer against the buffer we gave it. Trusting the return value would hand the caller
	 * a length for bytes it never received. */
	if ((size_t)got > sizeof(s_cache)) {
		return OD_HAL_NVS_E2BIG;
	}

	s_cache_len = (uint32_t)got;
	s_cache_valid = true;
	return OD_HAL_NVS_OK;
}

int od_hal_nvs_size(uint32_t *len_out)
{
	int rc;

	if (len_out == NULL) {
		return OD_HAL_NVS_EIO;
	}
	*len_out = 0;

	rc = cache_fill();
	if (rc != OD_HAL_NVS_OK) {
		return rc;
	}
	*len_out = s_cache_len;
	return OD_HAL_NVS_OK;
}

int od_hal_nvs_read(uint32_t offset, void *buf, uint32_t len)
{
	int rc;

	if (buf == NULL) {
		return OD_HAL_NVS_EIO;
	}

	/* A zero-length read is still a question about the record, so presence and bounds are
	 * answered before the span is dismissed as empty. */
	rc = cache_fill();
	if (rc != OD_HAL_NVS_OK) {
		return rc;
	}
	/* Subtraction, so a large offset cannot wrap the sum past the record. */
	if (offset > s_cache_len || len > s_cache_len - offset) {
		return OD_HAL_NVS_E2BIG;
	}
	if (len == 0u) {
		return OD_HAL_NVS_OK;
	}

	memcpy(buf, s_cache + offset, len);
	return OD_HAL_NVS_OK;
}

/* NVS stores the new value before retiring the old one, so both must fit at once. The settings
 * area can be two 4 kB sectors, where a record over about half a sector cannot replace itself, so
 * the old record is retired first and restored if the new one still does not fit. This gives up
 * power-fail atomicity for records this large only: a reset between the delete and the save
 * leaves no config. */
static int replace_without_overlap(const void *record, uint32_t len)
{
	const int fill = cache_fill();
	int rc;

	if (fill != OD_HAL_NVS_OK && fill != OD_HAL_NVS_ENOENT) {
		return -EIO;
	}
	rc = settings_delete(OD_SETTINGS_KEY);
	if (rc != 0) {
		return rc;
	}
	rc = settings_save_one(OD_SETTINGS_KEY, record, len);
	if (rc != 0 && fill == OD_HAL_NVS_OK &&
	    settings_save_one(OD_SETTINGS_KEY, s_cache, s_cache_len) != 0) {
		od_log_error("Previous config could not be restored");
	}
	return rc;
}

int od_hal_nvs_write(const void *record, uint32_t len)
{
	int rc;

	if (record == NULL || len == 0u) {
		return OD_HAL_NVS_EIO;
	}
	if (len > sizeof(s_cache)) {
		return OD_HAL_NVS_E2BIG;
	}
	if (od_hal_nvs_init() != OD_HAL_NVS_OK) {
		return OD_HAL_NVS_EIO;
	}

	rc = settings_save_one(OD_SETTINGS_KEY, record, len);
	if (rc == -ENOSPC) {
		od_log_info("Config replace: retiring the old record first");
		rc = replace_without_overlap(record, len);
	}
	if (rc != 0) {
		od_log_error("settings save failed: %d", rc);
		/* Commit the cache only after the medium accepts the write, and do not assume a
		 * rejected write changed nothing: settings_nvs_save() writes the value entry
		 * before the name entry, so a failure can leave either record in place. The next
		 * read re-fills from whatever is actually stored. */
		s_cache_valid = false;
		return OD_HAL_NVS_EIO;
	}

	memcpy(s_cache, record, len);
	s_cache_len = len;
	s_cache_valid = true;
	return OD_HAL_NVS_OK;
}

int od_hal_nvs_erase(void)
{
	if (od_hal_nvs_init() != OD_HAL_NVS_OK) {
		return OD_HAL_NVS_EIO;
	}

	if (settings_delete(OD_SETTINGS_KEY) != 0) {
		/* The record may or may not still be there -- settings_nvs_save() deletes the
		 * name entry before the value entry, so a failure on the second leaves the
		 * setting unreachable even though it reported an error. Invalidate rather than
		 * clear: the next read re-fills from the medium, so a config that survived is
		 * still served and one that did not is not. Clearing to empty would be the
		 * failure the contract names -- a device that forgets a config the medium still
		 * holds, and remembers it again on the next boot. */
		s_cache_valid = false;
		return OD_HAL_NVS_EIO;
	}

	s_cache_len = 0u;
	s_cache_valid = false;
	return OD_HAL_NVS_OK;
}
