#include "opendisplay_config_storage.h"

#include <stddef.h>
#include <stdio.h>

#include "nvm3_default_config.h"
#include "od_config_store.h"

/* The record must fit ONE NVM3 object. Nothing checks this at runtime -- od_hal_nvs_write()
 * forwards the length straight to nvm3_writeData -- so without this a raised cap would compile
 * and then fail in the field on any config near the advertised limit. */
_Static_assert(OD_CONFIG_STORE_MAX_RECORD <= NVM3_DEFAULT_MAX_OBJECT_SIZE,
               "config record exceeds NVM3_DEFAULT_MAX_OBJECT_SIZE");

/* ONE OBJECT IS BOTH the chunked-write reassembly state and the record workspace. The
 * assembler's four state words occupy exactly the 16 bytes the record header needs, and both
 * put their byte array at offset 16, so a save writes the header over the live state words and
 * resets the assembler immediately afterwards. That is what keeps a 32 KB part from needing a
 * second 2 KB staging object. od_config_store.h asserts the offset; the size is asserted here.
 *
 * The ordering this forces is the whole correctness argument, and it is not visible from any
 * offsetof: the declared length must be captured by the CALLER before the header write, every
 * refusal that could still leave a transfer in flight must happen BEFORE it, and nothing may
 * read the assembler afterwards. tests/host/silabs_storage_test.c exists for that ordering. */
typedef union {
  struct od_config_asm assembler;
  uint8_t              record[OD_CONFIG_STORE_MAX_RECORD];
} od_config_work_t;

_Static_assert(sizeof(struct od_config_asm) == OD_CONFIG_STORE_MAX_RECORD,
               "the assembler and the record must be the same object");

static od_config_work_t s_cfg;

struct od_config_asm *opendisplay_config_assembler(void)
{
  return &s_cfg.assembler;
}

bool initConfigStorage(void)
{
  return od_config_store_init() == OD_CONFIG_STORE_OK;
}

bool saveConfig(uint8_t *config_data, uint32_t len)
{
  enum od_config_store_result rc;

  /* Ahead of the header write, deliberately: everything that can refuse without consuming the
   * assembler's state words is asked first, so a refusal leaves an in-flight transfer intact. */
  if (config_data == NULL || len > OD_CONFIG_MAX_SIZE) {
    return false;
  }
  if (od_config_store_init() != OD_CONFIG_STORE_OK) {
    return false;
  }

  rc = od_config_store_save(s_cfg.record, sizeof(s_cfg.record), config_data, len);
  if (rc != OD_CONFIG_STORE_OK) {
    printf("[OD] config save failed rc=%d len=%u\r\n", (int)rc, (unsigned)len);
  }
  /* After the write, always: the state words are already gone, so leaving `active` set would
   * resume a transfer against a record header. */
  od_config_asm_reset(&s_cfg.assembler);
  return rc == OD_CONFIG_STORE_OK;
}

bool loadConfig(uint8_t *config_data, uint32_t *len)
{
  return od_config_store_load(config_data, len) == OD_CONFIG_STORE_OK;
}

bool clearStoredConfig(void)
{
  return od_config_store_clear() == OD_CONFIG_STORE_OK;
}
