/* ESP-IDF NVS implementation of od_hal_secret. Contract: shared/hal/od_hal_secret.h.
 *
 * Namespace "od_sec", separate from the config namespace so a config erase cannot reach it.
 * nvs_set_blob + nvs_commit replaces an entry atomically. */

#include "od_hal_secret.h"
#include "od_hal_nvs.h"

#include "nvs.h"

static const char *const s_keys[3] = { "os2k", "os2s", "os2r" };

static bool open_ns(nvs_open_mode_t mode, nvs_handle_t *h)
{
    return od_hal_nvs_init() == OD_HAL_NVS_OK && nvs_open("od_sec", mode, h) == ESP_OK;
}

bool od_hal_secret_read(enum od_hal_secret_id id, uint8_t *buf, uint16_t len)
{
    nvs_handle_t h;
    size_t n = len;
    bool ok;

    if (buf == NULL || id > OD_HAL_SECRET_MARKER || !open_ns(NVS_READONLY, &h)) {
        return false;
    }
    ok = nvs_get_blob(h, s_keys[id], buf, &n) == ESP_OK && n == len;
    nvs_close(h);
    return ok;
}

bool od_hal_secret_write(enum od_hal_secret_id id, const uint8_t *buf, uint16_t len)
{
    nvs_handle_t h;
    bool ok;

    if (buf == NULL || id > OD_HAL_SECRET_MARKER || !open_ns(NVS_READWRITE, &h)) {
        return false;
    }
    ok = nvs_set_blob(h, s_keys[id], buf, len) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}

bool od_hal_secret_erase(enum od_hal_secret_id id)
{
    nvs_handle_t h;
    esp_err_t err;
    bool ok;

    if (id > OD_HAL_SECRET_MARKER || !open_ns(NVS_READWRITE, &h)) {
        return false;
    }
    err = nvs_erase_key(h, s_keys[id]);
    ok = (err == ESP_OK || err == ESP_ERR_NVS_NOT_FOUND) && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
}
