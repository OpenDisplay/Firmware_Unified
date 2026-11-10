/* drive_fixture -- the stored-config medium, a config builder and a recording panel bus, for the
 * suites that drive od_cfg_index / od_epd_cfg / od_epd / od_cmd_drive / od_action.
 *
 * Header-only and single-TU, like od_check.h: it DEFINES the od_hal_nvs seam, so exactly one
 * translation unit per executable includes it.
 *
 * The NVS fake holds one record and honours the od_hal_nvs contract precisely (E2BIG past the
 * record, ENOENT when empty), because od_cfg_index reads at offsets and a fake that served
 * out-of-range reads would hide exactly the bounds bugs these suites look for.
 */
#ifndef OD_DRIVE_FIXTURE_H
#define OD_DRIVE_FIXTURE_H

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "od_config_store.h"
#include "od_config_tlv.h"
#include "od_epd_cfg.h"
#include "od_hal_nvs.h"
#include "opendisplay_structs.h"

/* ------------------------------------------------------------------------ known-bug markers -- */

/* KNOWN_BUG(id, cond): `cond` is what the code SHOULD do; today it does not. A failing cond is
 * reported as XFAIL and does not fail the suite. A passing cond is XPASS and DOES fail it: the
 * bug is fixed, so the marker must become a CHECK (and the bug list entry closed). Strict on
 * purpose -- a silent XPASS is how a regression marker stops guarding anything. */
#ifdef OD_CHECK_H
static unsigned fx_xfail_count;
#define KNOWN_BUG(id, cond)                                                          \
    do {                                                                             \
        ++od_check_count;                                                            \
        if (cond) {                                                                  \
            ++od_check_failures;                                                     \
            printf("XPASS %s:%d [%s] %s -- bug appears FIXED: turn KNOWN_BUG into "  \
                   "CHECK\n", __FILE__, __LINE__, (id), #cond);                     \
        } else {                                                                     \
            ++fx_xfail_count;                                                        \
            printf("XFAIL %s [%s]\n", (id), od_check_case);                          \
        }                                                                            \
    } while (0)
#endif

/* ------------------------------------------------------------------------------ NVS medium -- */

#define FX_NVS_CAP (OD_CONFIG_STORE_HEADER_SIZE + 8192u)

static uint8_t  fx_nvs[FX_NVS_CAP];
static uint32_t fx_nvs_len;
static bool     fx_nvs_present;
static unsigned fx_nvs_reads;
static int      fx_nvs_fail_after = -1;   /* fail the Nth read from now (0 = next), -1 never */
static bool     fx_nvs_write_fail;        /* next write returns EIO, medium untouched */
static unsigned fx_nvs_writes;

int od_hal_nvs_init(void) { return OD_HAL_NVS_OK; }

int od_hal_nvs_size(uint32_t *len_out)
{
    if (!fx_nvs_present) {
        *len_out = 0u;
        return OD_HAL_NVS_ENOENT;
    }
    *len_out = fx_nvs_len;
    return OD_HAL_NVS_OK;
}

int od_hal_nvs_read(uint32_t offset, void *buf, uint32_t len)
{
    ++fx_nvs_reads;
    if (fx_nvs_fail_after == 0) {
        return OD_HAL_NVS_EIO;
    }
    if (fx_nvs_fail_after > 0) {
        --fx_nvs_fail_after;
    }
    if (!fx_nvs_present) {
        return OD_HAL_NVS_ENOENT;
    }
    if (offset > fx_nvs_len || len > fx_nvs_len - offset) {
        return OD_HAL_NVS_E2BIG;
    }
    memcpy(buf, fx_nvs + offset, len);
    return OD_HAL_NVS_OK;
}

int od_hal_nvs_write(const void *record, uint32_t len)
{
    ++fx_nvs_writes;
    if (fx_nvs_write_fail) {
        fx_nvs_write_fail = false;
        return OD_HAL_NVS_EIO;
    }
    if (len > FX_NVS_CAP) {
        return OD_HAL_NVS_EIO;
    }
    memcpy(fx_nvs, record, len);
    fx_nvs_len = len;
    fx_nvs_present = true;
    return OD_HAL_NVS_OK;
}

int od_hal_nvs_erase(void)
{
    fx_nvs_present = false;
    fx_nvs_len = 0u;
    return OD_HAL_NVS_OK;
}

static inline void fx_put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

/* Frame a payload as the record od_config_store writes: magic, version 1, CRC-32, length. */
static inline void fx_store_payload(const uint8_t *payload, uint32_t n)
{
    if (OD_CONFIG_STORE_HEADER_SIZE + n > FX_NVS_CAP) {
        printf("fixture: payload %u too large\n", (unsigned)n);
        abort();
    }
    fx_put32(&fx_nvs[0], OD_CONFIG_STORE_MAGIC);
    fx_put32(&fx_nvs[4], OD_CONFIG_STORE_VERSION);
    fx_put32(&fx_nvs[8], od_config_store_crc32(payload, n));
    fx_put32(&fx_nvs[12], n);
    memmove(&fx_nvs[OD_CONFIG_STORE_HEADER_SIZE], payload, n);
    fx_nvs_len = OD_CONFIG_STORE_HEADER_SIZE + n;
    fx_nvs_present = true;
    fx_nvs_fail_after = -1;
}

/* --------------------------------------------------------------------------- config builder -- */

typedef struct {
    uint8_t  b[8192];
    uint32_t n;
    uint8_t  counter;
} fx_cfg_t;

static inline void fx_cfg_bytes(fx_cfg_t *c, const void *p, uint32_t n)
{
    if (c->n + n > sizeof c->b) {
        printf("fixture: config builder overflow\n");
        abort();
    }
    memcpy(c->b + c->n, p, n);
    c->n += n;
}

static inline void fx_cfg_u8(fx_cfg_t *c, uint8_t v) { fx_cfg_bytes(c, &v, 1u); }

/* [reserved:2][version]. The reserved pair is the toolbox's length field; see the CRC test. */
static inline void fx_cfg_begin(fx_cfg_t *c, uint8_t version)
{
    c->n = 0u;
    c->counter = 0u;
    fx_cfg_u8(c, 0u);
    fx_cfg_u8(c, 0u);
    fx_cfg_u8(c, version);
}

/* Legacy range: [number][id][body] with the size implied by the id. */
static inline void fx_cfg_legacy(fx_cfg_t *c, uint8_t id, const void *body, uint32_t n)
{
    fx_cfg_u8(c, c->counter++);
    fx_cfg_u8(c, id);
    fx_cfg_bytes(c, body, n);
}

/* Length-prefixed range: [number][id][len:2 LE][body]. */
static inline void fx_cfg_ext_len(fx_cfg_t *c, uint8_t id, const void *body, uint32_t n,
                           uint16_t declared)
{
    fx_cfg_u8(c, c->counter++);
    fx_cfg_u8(c, id);
    fx_cfg_u8(c, (uint8_t)declared);
    fx_cfg_u8(c, (uint8_t)(declared >> 8));
    if (n > 0u) {
        fx_cfg_bytes(c, body, n);
    }
}

static inline void fx_cfg_ext(fx_cfg_t *c, uint8_t id, const void *body, uint32_t n)
{
    fx_cfg_ext_len(c, id, body, n, (uint16_t)n);
}

/* Trailing CRC-16 over everything before it, as the toolbox computes it. */
static inline void fx_cfg_finish(fx_cfg_t *c)
{
    const uint16_t crc = od_config_tlv_crc16(od_span_make(c->b, c->n));

    fx_cfg_u8(c, (uint8_t)crc);
    fx_cfg_u8(c, (uint8_t)(crc >> 8));
}

static inline void fx_cfg_store(fx_cfg_t *c) { fx_store_payload(c->b, c->n); }

/* A minimal SPI display: instance `inst`, BWRY, pins all wired. */
static inline struct DisplayConfig fx_display(uint8_t inst, uint16_t w, uint16_t h, uint8_t scheme)
{
    struct DisplayConfig d;

    memset(&d, 0, sizeof d);
    d.instance_number = inst;
    d.pixel_width = w;
    d.pixel_height = h;
    d.color_scheme = scheme;
    d.reset_pin = 1u;
    d.busy_pin = 2u;
    d.dc_pin = 3u;
    d.cs_pin = 4u;
    d.data_pin = 5u;
    d.clk_pin = 6u;
    d.cs_pin_2 = 0xFFu;
    return d;
}

/* The three legacy packets every config carries, then one display. */
static inline void fx_cfg_legacy_head(fx_cfg_t *c, const struct DisplayConfig *d)
{
    struct SystemConfig sys;
    struct ManufacturerData man;
    struct PowerOption pwr;

    memset(&sys, 0, sizeof sys);
    memset(&man, 0, sizeof man);
    memset(&pwr, 0, sizeof pwr);
    fx_cfg_legacy(c, 0x01u, &sys, sizeof sys);
    fx_cfg_legacy(c, 0x02u, &man, sizeof man);
    fx_cfg_legacy(c, 0x04u, &pwr, sizeof pwr);
    if (d != NULL) {
        fx_cfg_legacy(c, 0x20u, d, sizeof *d);
    }
}

/* ---------------------------------------------------------------------- real panel packets -- */

typedef struct {
    uint8_t        id;
    const uint8_t *body;
    uint16_t       n;
} fx_panel_pkt_t;

typedef struct {
    const char           *name;
    uint16_t              width, height;
    uint8_t               scheme;
    const fx_panel_pkt_t *pkts;
    unsigned              count;
} fx_panel_t;

#include "../vectors/drive_panels.inc"

/* Append a panel's drive packets, optionally re-targeted at another display instance. Every
 * drive packet carries its instance in body[0]. */
static inline void fx_cfg_panel(fx_cfg_t *c, const fx_panel_t *p, uint8_t instance)
{
    uint8_t buf[1024];

    for (unsigned i = 0; i < p->count; i++) {
        if (p->pkts[i].n > sizeof buf) {
            abort();
        }
        memcpy(buf, p->pkts[i].body, p->pkts[i].n);
        buf[0] = instance;
        fx_cfg_ext(c, p->pkts[i].id, buf, p->pkts[i].n);
    }
}

/* Build and store a complete, valid config for one real panel on display 0. */
static inline struct DisplayConfig fx_store_panel(const fx_panel_t *p)
{
    fx_cfg_t c;
    struct DisplayConfig d = fx_display(0u, p->width, p->height, p->scheme);

    fx_cfg_begin(&c, 1u);
    fx_cfg_legacy_head(&c, &d);
    fx_cfg_panel(&c, p, 0u);
    fx_cfg_finish(&c);
    fx_cfg_store(&c);
    return d;
}

/* ------------------------------------------------------------------------ recording panel bus -- */

/* Every bus call as one text line, so a golden file diff reads like a logic-analyser capture:
 *   R <ms>       reset pulse
 *   C xx         command byte
 *   D xx xx ..   data bytes (<= 64 B), or "D[n] crc32=XXXXXXXX" for an image-sized block
 *   W <ms>       delay
 */
#define FX_TRACE_CAP (256u * 1024u)

static char     fx_trace[FX_TRACE_CAP];
static size_t   fx_trace_n;
static bool     fx_trace_overflow;
static uint32_t fx_clock_ms;
static int      fx_busy_level = -1;      /* -1 = no BUSY pin */
static unsigned fx_busy_reads;
static uint32_t fx_busy_release_after;   /* reads that report busy before it releases, 0 = never busy */
static unsigned fx_cmd_count;
static unsigned fx_data_bytes;
static unsigned fx_reset_count;
static bool     fx_bus_fault;

static inline void fx_trace_line(const char *s)
{
    const size_t n = strlen(s);

    if (fx_trace_n + n + 2u >= FX_TRACE_CAP) {
        fx_trace_overflow = true;
        return;
    }
    memcpy(fx_trace + fx_trace_n, s, n);
    fx_trace_n += n;
    fx_trace[fx_trace_n++] = '\n';
    fx_trace[fx_trace_n] = '\0';
}

static inline void fx_bus_cmd(uint8_t c)
{
    char line[16];

    ++fx_cmd_count;
    snprintf(line, sizeof line, "C %02x", (unsigned)c);
    fx_trace_line(line);
}

static inline void fx_bus_data(const uint8_t *d, size_t n)
{
    char line[8 + 64u * 3u];

    fx_data_bytes += (unsigned)n;
    if (n > 64u) {
        snprintf(line, sizeof line, "D[%u] crc32=%08x", (unsigned)n,
                 (unsigned)od_config_store_crc32(d, (uint32_t)n));
    } else {
        size_t o = (size_t)snprintf(line, sizeof line, "D");
        for (size_t i = 0; i < n; i++) {
            o += (size_t)snprintf(line + o, sizeof line - o, " %02x", (unsigned)d[i]);
        }
    }
    fx_trace_line(line);
}

static inline void fx_bus_reset(uint32_t ms)
{
    char line[24];

    ++fx_reset_count;
    snprintf(line, sizeof line, "R %u", (unsigned)ms);
    fx_trace_line(line);
}

static inline int fx_bus_busy_read(void)
{
    ++fx_busy_reads;
    if (fx_busy_level < 0) {
        return -1;
    }
    if (fx_busy_release_after > 0u) {
        --fx_busy_release_after;
        return fx_busy_level ? 0 : 1;      /* the non-idle level */
    }
    return fx_busy_level;
}

static inline void fx_bus_delay(uint32_t ms)
{
    char line[24];

    fx_clock_ms += ms;
    snprintf(line, sizeof line, "W %u", (unsigned)ms);
    fx_trace_line(line);
}

static inline uint32_t fx_bus_now(void) { return fx_clock_ms; }
static inline bool fx_bus_faulted(void) { return fx_bus_fault; }

static const od_epd_cfg_bus_t FX_BUS = {
    .cmd = fx_bus_cmd,
    .data = fx_bus_data,
    .reset = fx_bus_reset,
    .busy_read = fx_bus_busy_read,
    .delay_ms = fx_bus_delay,
    .now_ms = fx_bus_now,
    .faulted = fx_bus_faulted,
};

static inline void fx_bus_clear(void)
{
    fx_trace_n = 0u;
    fx_trace[0] = '\0';
    fx_trace_overflow = false;
    fx_busy_reads = 0u;
    fx_busy_release_after = 0u;
    fx_cmd_count = 0u;
    fx_data_bytes = 0u;
    fx_reset_count = 0u;
    fx_bus_fault = false;
}

#endif /* OD_DRIVE_FIXTURE_H */
