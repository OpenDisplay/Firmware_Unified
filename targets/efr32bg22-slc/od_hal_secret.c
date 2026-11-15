/* NVM3 implementation of od_hal_secret: three small objects beside the config object.
 * Contract: shared/hal/od_hal_secret.h. nvm3_writeData replaces an object atomically. */

#include "od_hal_secret.h"

#include <stddef.h>

#include "nvm3_default.h"
#include "sl_status.h"

/* Next to OD_NVM3_CONFIG_KEY 0x0F4401 (od_hal_nvs.c). */
static nvm3_ObjectKey_t key_of(enum od_hal_secret_id id)
{
  return (nvm3_ObjectKey_t)(0x0F4410u + (uint32_t)id);
}

static bool ready(void)
{
  return nvm3_defaultHandle->hasBeenOpened || nvm3_initDefault() == SL_STATUS_OK;
}

bool od_hal_secret_read(enum od_hal_secret_id id, uint8_t *buf, uint16_t len)
{
  uint32_t type = 0;
  size_t n = 0;

  if (buf == NULL || id > OD_HAL_SECRET_MARKER || !ready() ||
      nvm3_getObjectInfo(nvm3_defaultHandle, key_of(id), &type, &n) != SL_STATUS_OK ||
      type != NVM3_OBJECTTYPE_DATA || n != len) {
    return false;
  }
  return nvm3_readData(nvm3_defaultHandle, key_of(id), buf, len) == SL_STATUS_OK;
}

bool od_hal_secret_write(enum od_hal_secret_id id, const uint8_t *buf, uint16_t len)
{
  return buf != NULL && id <= OD_HAL_SECRET_MARKER && ready() &&
         nvm3_writeData(nvm3_defaultHandle, key_of(id), buf, len) == SL_STATUS_OK;
}

bool od_hal_secret_erase(enum od_hal_secret_id id)
{
  sl_status_t sc;

  if (id > OD_HAL_SECRET_MARKER || !ready()) {
    return false;
  }
  sc = nvm3_deleteObject(nvm3_defaultHandle, key_of(id));
  return sc == SL_STATUS_OK || sc == ECODE_NVM3_ERR_KEY_NOT_FOUND;
}
