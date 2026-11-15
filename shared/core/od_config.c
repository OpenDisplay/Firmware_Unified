/* od_config.c -- see od_config.h. Plain C99, no HAL and no allocation. */

#include "od_config.h"

#include "od_log.h"

#include <stddef.h>
#include <string.h>

/* One repeatable type's storage, so the cap-and-append rule is written ONCE. The ESP32 spelled
 * it out eight times and each copy was an independent chance to compare against the wrong
 * bound or to increment the wrong counter. */
struct od_config_slot {
    uint8_t *base;        /* first element */
    uint8_t *count;       /* the instance counter beside it */
    uint16_t elem_size;
    uint8_t  max;
};

/* Both stores ask od_span_has() rather than comparing a length: it is the same question, but it
 * cannot be asked of the wrong buffer, and it folds the "is there a pointer at all" check into
 * the bound so a null body is short rather than a memcpy from NULL. */
static enum od_config_apply store_repeatable(const struct od_config_slot *slot, od_span_t body)
{
    if (!od_span_has(body, slot->elem_size)) {
        return OD_CONFIG_APPLY_SHORT_BODY;
    }
    if (*slot->count >= slot->max) {
        /* SKIP, NOT OVERWRITE: the instances already stored are the ones the host sent first,
         * and all three targets agreed on keeping them. */
        return OD_CONFIG_APPLY_FULL;
    }
    memcpy(slot->base + ((size_t)(*slot->count) * slot->elem_size), body.p, slot->elem_size);
    (*slot->count)++;
    return OD_CONFIG_APPLY_STORED;
}

static enum od_config_apply store_single(void *dst, uint16_t dst_size,
                                         od_span_t body, bool *loaded)
{
    if (!od_span_has(body, dst_size)) {
        return OD_CONFIG_APPLY_SHORT_BODY;
    }
    memcpy(dst, body.p, dst_size);
    if (loaded != NULL) {
        *loaded = true;
    }
    return OD_CONFIG_APPLY_STORED;
}

void od_config_reset(struct od_config *cfg)
{
    if (cfg == NULL) {
        return;
    }
    memset(cfg, 0, sizeof(*cfg));
}

const struct FindMyConfig *od_config_findmy(const struct od_config *cfg)
{
#if OD_CONFIG_WITH_FINDMY
    /* Guarded HERE, in the one translation unit that defines the struct, so the answer cannot
     * depend on whether a caller happened to see the macro. */
    if (cfg != NULL && cfg->findmy_loaded) {
        return &cfg->findmy;
    }
#else
    (void)cfg;
#endif
    return NULL;
}

bool od_config_security_key_set(const struct SecurityConfig *sec)
{
    unsigned i;

    if (sec == NULL) {
        return false;
    }
    for (i = 0; i < sizeof(sec->encryption_key); ++i) {
        if (sec->encryption_key[i] != 0u) {
            return true;
        }
    }
    return false;
}

#if OD_CONFIG_WITH_DATA_EXTENDED
/* Every string is a fixed 32-byte field that a host may fill completely, leaving no terminator.
 * Firmware terminates all nine at parse time so no consumer has to remember; doing it here is
 * what keeps a printf on a name from running into the next field. */
static void data_extended_terminate(struct DataExtended *de)
{
    de->manufacturer_name[31] = '\0';
    de->model_name[31] = '\0';
    de->serial_number[31] = '\0';
    de->friendly_name[31] = '\0';
    de->device_location[31] = '\0';
    de->device_id[31] = '\0';
    de->custom_string_1[31] = '\0';
    de->custom_string_2[31] = '\0';
    de->custom_string_3[31] = '\0';
}
#endif

enum od_config_apply od_config_apply_packet(struct od_config *cfg, uint8_t packet_id,
                                            od_span_t body)
{
    struct od_config_slot slot;

    if (cfg == NULL || !od_span_valid(body)) {
        return OD_CONFIG_APPLY_SHORT_BODY;
    }

    switch (packet_id) {
    case 0x01:
        return store_single(&cfg->system_config, (uint16_t)sizeof cfg->system_config,
                            body, NULL);
    case 0x02:
        return store_single(&cfg->manufacturer_data, (uint16_t)sizeof cfg->manufacturer_data,
                            body, NULL);
    case 0x04:
        return store_single(&cfg->power_option, (uint16_t)sizeof cfg->power_option,
                            body, NULL);

    case 0x20:
        slot.base = (uint8_t *)cfg->displays;
        slot.count = &cfg->display_count;
        slot.elem_size = (uint16_t)sizeof cfg->displays[0];
        slot.max = (uint8_t)OD_CONFIG_MAX_DISPLAYS;
        return store_repeatable(&slot, body);
    case 0x21:
        slot.base = (uint8_t *)cfg->leds;
        slot.count = &cfg->led_count;
        slot.elem_size = (uint16_t)sizeof cfg->leds[0];
        slot.max = (uint8_t)OD_CONFIG_MAX_LEDS;
        return store_repeatable(&slot, body);
    case 0x23:
        slot.base = (uint8_t *)cfg->sensors;
        slot.count = &cfg->sensor_count;
        slot.elem_size = (uint16_t)sizeof cfg->sensors[0];
        slot.max = (uint8_t)OD_CONFIG_MAX_SENSORS;
        return store_repeatable(&slot, body);
    case 0x24:
        slot.base = (uint8_t *)cfg->data_buses;
        slot.count = &cfg->data_bus_count;
        slot.elem_size = (uint16_t)sizeof cfg->data_buses[0];
        slot.max = (uint8_t)OD_CONFIG_MAX_DATA_BUSES;
        return store_repeatable(&slot, body);
    case 0x25:
        slot.base = (uint8_t *)cfg->binary_inputs;
        slot.count = &cfg->binary_input_count;
        slot.elem_size = (uint16_t)sizeof cfg->binary_inputs[0];
        slot.max = (uint8_t)OD_CONFIG_MAX_BINARY_INPUTS;
        return store_repeatable(&slot, body);
    case 0x2B:
        slot.base = (uint8_t *)cfg->flash_configs;
        slot.count = &cfg->flash_config_count;
        slot.elem_size = (uint16_t)sizeof cfg->flash_configs[0];
        slot.max = (uint8_t)OD_CONFIG_MAX_FLASH;
        return store_repeatable(&slot, body);

    case 0x28:
#if OD_CONFIG_WITH_TOUCH
        slot.base = (uint8_t *)cfg->touch_controllers;
        slot.count = &cfg->touch_controller_count;
        slot.elem_size = (uint16_t)sizeof cfg->touch_controllers[0];
        slot.max = (uint8_t)OD_CONFIG_MAX_TOUCH;
        return store_repeatable(&slot, body);
#else
        return OD_CONFIG_APPLY_NOT_BUILT;
#endif

    case 0x29:
#if OD_CONFIG_WITH_BUZZER
        slot.base = (uint8_t *)cfg->passive_buzzers;
        slot.count = &cfg->passive_buzzer_count;
        slot.elem_size = (uint16_t)sizeof cfg->passive_buzzers[0];
        slot.max = (uint8_t)OD_CONFIG_MAX_BUZZERS;
        return store_repeatable(&slot, body);
#else
        return OD_CONFIG_APPLY_NOT_BUILT;
#endif

    case 0x2A:
#if OD_CONFIG_WITH_NFC
        slot.base = (uint8_t *)cfg->nfc_configs;
        slot.count = &cfg->nfc_config_count;
        slot.elem_size = (uint16_t)sizeof cfg->nfc_configs[0];
        slot.max = (uint8_t)OD_CONFIG_MAX_NFC;
        return store_repeatable(&slot, body);
#else
        return OD_CONFIG_APPLY_NOT_BUILT;
#endif

    case 0x2C:
#if OD_CONFIG_WITH_DATA_EXTENDED
        {
            enum od_config_apply r = store_single(&cfg->data_extended,
                                                  (uint16_t)sizeof cfg->data_extended,
                                                  body, &cfg->data_extended_loaded);
            if (r == OD_CONFIG_APPLY_STORED) {
                data_extended_terminate(&cfg->data_extended);
            }
            return r;
        }
#else
        return OD_CONFIG_APPLY_NOT_BUILT;
#endif

    case 0x26:
#if OD_CONFIG_WITH_WIFI
        return store_single(&cfg->wifi_config, (uint16_t)sizeof cfg->wifi_config,
                            body, &cfg->wifi_config_loaded);
#else
        return OD_CONFIG_APPLY_NOT_BUILT;
#endif

    case 0x2D:
#if OD_CONFIG_WITH_FINDMY
        return store_single(&cfg->findmy, (uint16_t)sizeof cfg->findmy,
                            body, &cfg->findmy_loaded);
#else
        return OD_CONFIG_APPLY_NOT_BUILT;
#endif

    case 0x27:
        {
            enum od_config_apply r = store_single(&cfg->security, (uint16_t)sizeof cfg->security,
                                                  body, &cfg->security_loaded);
            if (r != OD_CONFIG_APPLY_STORED) {
                return r;
            }
            /* THE ZERO-KEY RULE, from the Firmware repo. A config that asks for encryption but
             * carries an all-zero key does not get it: the alternative is a device demanding
             * authentication against a key any client can guess, which reads as protected and
             * is not. Applied at store time so no consumer can reach the un-normalised value --
             * the divergence on the other two targets is exactly that their gate reads a field
             * nothing had normalised. */
            if (!od_config_security_key_set(&cfg->security)) {
                cfg->security.encryption_enabled = 0u;
            }
            return OD_CONFIG_APPLY_STORED;
        }

    default:
        return OD_CONFIG_APPLY_UNKNOWN_ID;
    }
}

/* --------------------------------------------------------------------------- the orchestration --- */

struct parse_ctx {
    struct od_config *cfg;
    struct od_config_report *report;
};

static void log_parse_result(const struct od_config *cfg, size_t blob_len,
                             enum od_config_tlv_result walk,
                             const struct od_config_report *report)
{
    unsigned buzzers = 0u;
    unsigned nfc = 0u;

    if (report->unknown_id != 0u) {
        od_log_warn("Unknown config packet 0x%02X; remainder skipped",
                    (unsigned)report->unknown_id);
    }
    if (walk == OD_CFG_TLV_TOO_SHORT) {
        od_log_error("Config too short: %u bytes", (unsigned)blob_len);
        return;
    }
    if (walk == OD_CFG_TLV_TRUNCATED) {
        od_log_error("Config truncated: a packet exceeds the blob");
        return;
    }
    if (walk != OD_CFG_TLV_OK) {
        od_log_error("Config rejected while applying a packet");
        return;
    }
    if (report->dropped_full != 0u) {
        od_log_warn("%u config packet(s) dropped at an instance cap",
                    (unsigned)report->dropped_full);
    }
    if (report->dropped_not_built != 0u) {
        od_log_info("%u config packet(s) ignored for unavailable subsystems",
                    (unsigned)report->dropped_not_built);
    }
    if (report->crc_checked && report->crc_stored != report->crc_computed) {
        od_log_warn("Config CRC mismatch: stored 0x%04X, computed 0x%04X",
                    (unsigned)report->crc_stored, (unsigned)report->crc_computed);
    }
#if OD_CONFIG_WITH_BUZZER
    buzzers = cfg->passive_buzzer_count;
#endif
#if OD_CONFIG_WITH_NFC
    nfc = cfg->nfc_config_count;
#endif
    od_log_info("Config parsed: version=%u, displays=%u, leds=%u, sensors=%u, "
                "data_buses=%u, binary_inputs=%u, buzzers=%u, nfc=%u, flash=%u",
                (unsigned)cfg->version, (unsigned)cfg->display_count,
                (unsigned)cfg->led_count, (unsigned)cfg->sensor_count,
                (unsigned)cfg->data_bus_count, (unsigned)cfg->binary_input_count,
                buzzers, nfc, (unsigned)cfg->flash_config_count);
}

static bool on_packet(void *vctx, uint8_t packet_id, od_span_t body)
{
    struct parse_ctx *ctx = (struct parse_ctx *)vctx;
    enum od_config_apply r = od_config_apply_packet(ctx->cfg, packet_id, body);

    if (ctx->report != NULL) {
        switch (r) {
        case OD_CONFIG_APPLY_STORED:     ctx->report->stored++;            break;
        case OD_CONFIG_APPLY_FULL:       ctx->report->dropped_full++;      break;
        case OD_CONFIG_APPLY_NOT_BUILT:  ctx->report->dropped_not_built++; break;
        default:                                                           break;
        }
    }

    /* NEVER ABORTS THE WALK. A cap reached, a subsystem this build lacks, and an id the walk
     * knew but this table does not are all capability differences, not parse errors -- the walk
     * header says so, and aborting would discard packets that follow a full instance array.
     * OD_CONFIG_APPLY_SHORT_BODY cannot arrive from the walk, which guarantees the declared
     * size before calling; if it ever does, the packet is skipped rather than trusted. */
    return true;
}

enum od_config_tlv_result od_config_parse(struct od_config *cfg, od_span_t blob,
                                          struct od_config_report *report)
{
    struct parse_ctx ctx;
    struct od_config_report local_report;
    struct od_config_report *active_report = report != NULL ? report : &local_report;
    enum od_config_tlv_result walk;
    uint8_t version = 0u;
    uint8_t unknown_id = 0u;

    memset(active_report, 0, sizeof(*active_report));
    if (cfg == NULL) {
        od_log_error("Config parse rejected: invalid destination");
        return OD_CFG_TLV_TOO_SHORT;
    }

    od_config_reset(cfg);
    if (!od_span_valid(blob)) {
        od_log_error("Config parse rejected: invalid blob");
        return OD_CFG_TLV_TOO_SHORT;
    }

    od_log_info("Parsing config: %u bytes", (unsigned)blob.n);

    ctx.cfg = cfg;
    ctx.report = active_report;
    walk = od_config_tlv_walk(blob, on_packet, &ctx, &version, &unknown_id);

    cfg->version = version;
    /* Not carried by the current format. Firmware sets it to 0 explicitly rather than leaving
     * it undefined, and a consumer reading a stale minor version is worse than reading 0. */
    cfg->minor_version = 0u;

    active_report->unknown_id = unknown_id;

    if (walk != OD_CFG_TLV_OK) {
        /* loaded stays false, and it is the ONLY thing separating a partial parse from a real
         * config. The walk applies each packet as it reaches it, so a blob that truncates
         * half-way leaves every packet before the truncation stored -- the reset above cleared
         * whatever was there before, it does not un-store what the walk just did. A caller that
         * ignores both this return and cfg->loaded reads those packets as a whole config. */
        log_parse_result(cfg, blob.n, walk, active_report);
        return walk;
    }

    /* Advisory CRC, computed last so it is reported even when nothing was stored. The trailing
     * two bytes are the stored value; everything before them is the covered region -- and the
     * `if` above the take() is what makes the saturating trim the right tool here rather than a
     * split (od_span.h: take/drop are for lengths already known to fit). */
    if (blob.n >= OD_CFG_TLV_CRC_LEN) {
        active_report->crc_checked = true;
        active_report->crc_stored = (uint16_t)((uint16_t)blob.p[blob.n - 2u] |
                                               ((uint16_t)blob.p[blob.n - 1u] << 8));
        active_report->crc_computed =
            od_config_tlv_crc16(od_span_take(blob, blob.n - OD_CFG_TLV_CRC_LEN));
    }

    cfg->loaded = true;
    log_parse_result(cfg, blob.n, walk, active_report);
    od_config_log_dump(cfg);
    return OD_CFG_TLV_OK;
}

const struct DataBus *od_config_data_bus(const struct od_config *cfg, uint8_t instance)
{
    const struct DataBus *found = NULL;

    if (cfg == NULL) {
        return NULL;
    }
    for (uint8_t i = 0; i < cfg->data_bus_count && i < (uint8_t)OD_CONFIG_MAX_DATA_BUSES; ++i) {
        if (cfg->data_buses[i].instance_number != instance) {
            continue;
        }
        if (found != NULL) {
            return NULL;   /* declared twice: ambiguous, so no bus rather than an arbitrary one */
        }
        found = &cfg->data_buses[i];
    }
    return found;
}
