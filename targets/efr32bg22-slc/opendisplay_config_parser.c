/* BG22 config adapter: NVM3 owns persistence; shared/core owns parsing and normalization. */

#include "opendisplay_config_parser.h"
#include "od_diag.h"

#include "od_config.h"
#include "od_span.h"
#include "opendisplay_config_storage.h"

#include <stdio.h>

static struct od_config *s_parsed;

const struct SecurityConfig *od_get_parsed_security(void)
{
  return (s_parsed != NULL && s_parsed->security_loaded) ? &s_parsed->security : NULL;
}

static void log_parse(const struct od_config_report *report,
                      enum od_config_tlv_result result)
{
  if (report == NULL) {
    return;
  }
  OD_DIAG("[OD] config parse rc=%d stored=%u full=%u not-built=%u unknown=0x%02X\r\n",
         (int)result, (unsigned)report->stored, (unsigned)report->dropped_full,
         (unsigned)report->dropped_not_built, (unsigned)report->unknown_id);
  if (report->crc_checked && report->crc_stored != report->crc_computed) {
    OD_DIAG("[OD] config CRC advisory mismatch stored=0x%04X computed=0x%04X\r\n",
           (unsigned)report->crc_stored, (unsigned)report->crc_computed);
  }
}

bool parseConfigBytes(uint8_t *config_data, uint32_t config_len,
                      struct GlobalConfig *global_config)
{
  struct od_config_report report;
  enum od_config_tlv_result result;

  if (global_config == NULL || config_data == NULL) {
    return false;
  }
  result = od_config_parse(global_config, od_span_make(config_data, config_len), &report);
  log_parse(&report, result);
  s_parsed = global_config;
  return result == OD_CFG_TLV_OK;
}

bool loadGlobalConfig(struct GlobalConfig *global_config)
{
  uint8_t *data;
  uint32_t len = OD_CONFIG_MAX_SIZE;

  if (global_config == NULL) {
    return false;
  }
  od_config_reset(global_config);
  s_parsed = global_config;
  data = opendisplay_config_assembler()->buffer;
  if (!initConfigStorage() || !loadConfig(data, &len)) {
    return false;
  }
  return parseConfigBytes(data, len, global_config);
}
