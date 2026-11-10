/* od_hal_nvs with nothing stored: for executables that index the config (od_cfg_index) but
 * whose cases never store a record. An empty medium is ENOENT, not an error. */
#include "od_hal_nvs.h"

int od_hal_nvs_init(void) { return OD_HAL_NVS_OK; }
int od_hal_nvs_size(uint32_t *len_out) { (void)len_out; return OD_HAL_NVS_ENOENT; }
int od_hal_nvs_read(uint32_t offset, void *buf, uint32_t len)
{
    (void)offset;
    (void)buf;
    (void)len;
    return OD_HAL_NVS_ENOENT;
}
int od_hal_nvs_write(const void *record, uint32_t len)
{
    (void)record;
    (void)len;
    return OD_HAL_NVS_EIO;
}
int od_hal_nvs_erase(void) { return OD_HAL_NVS_OK; }
