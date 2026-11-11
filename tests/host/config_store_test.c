/* od_config_store -- framing, CRC and bounds for the stored config record, over a fake medium.
 *
 * The fake is a MEDIUM, not an oracle: it holds bytes and reports failures on demand, and it
 * knows nothing about magic, versions or checksums. Every expectation about the record shape
 * below is written out as literal bytes, so a bug that changed both the writer and the reader
 * in the same direction would still fail here.
 *
 * Compiled twice, against OD_CONFIG_MAX_SIZE 4096 and 2048 (shared/profiles.cmake), because the
 * cap is the one thing that differs per target and it decides what is refused.
 */

#include "od_config_store.h"
#include "od_hal_nvs.h"
#include "od_log.h"

#include "od_check.h"

#include <stdarg.h>
#include <stdio.h>
#include <string.h>

/* ------------------------------------------------------------------ the fake medium ------ */

#define MEDIUM_CAP (OD_CONFIG_STORE_MAX_RECORD + 64u)

static uint8_t  g_medium[MEDIUM_CAP];
static uint32_t g_medium_len;
static bool     g_present;

/* Failure injection. Each is consumed by the next matching call. */
static int  g_fail_init;
static int  g_fail_size;
static int  g_fail_read;
/* Let this many reads through, then fail the next one. -1 disarmed, so 0 means "fail the
 * first read" and 1 means "fail the second" -- which is the payload read. */
static int  g_fail_read_after;
static int  g_fail_write;
static int  g_fail_erase;
/* Set when a failing write is meant to have changed the medium anyway -- the real behaviour
 * of ESP-IDF's nvs_set_blob(), which writes the new entry before erasing the old. */
static bool g_write_lands_despite_failure;

static unsigned g_reads;

#define LOG_CAP 32u
#define LOG_TEXT_CAP 128u

struct captured_log {
    int level;
    char text[LOG_TEXT_CAP];
};

static struct captured_log g_logs[LOG_CAP];
static unsigned g_log_count;

void _od_log(int level, const char *fmt, ...)
{
    va_list ap;

    if (g_log_count >= LOG_CAP) {
        return;
    }
    g_logs[g_log_count].level = level;
    va_start(ap, fmt);
    (void)vsnprintf(g_logs[g_log_count].text, sizeof(g_logs[g_log_count].text), fmt, ap);
    va_end(ap);
    ++g_log_count;
}

static void logs_reset(void)
{
    memset(g_logs, 0, sizeof(g_logs));
    g_log_count = 0u;
}

#if OD_LOG_EFFECTIVE_LEVEL >= OD_LOG_INFO
static bool logged_exact(int level, const char *text)
{
    unsigned i;

    for (i = 0u; i < g_log_count; ++i) {
        if (g_logs[i].level == level && strcmp(g_logs[i].text, text) == 0) {
            return true;
        }
    }
    return false;
}
#endif

static void medium_reset(void)
{
    memset(g_medium, 0, sizeof(g_medium));
    g_medium_len = 0;
    g_present = false;
    g_fail_init = g_fail_size = g_fail_read = g_fail_write = g_fail_erase = 0;
    g_fail_read_after = -1;
    g_write_lands_despite_failure = false;
    g_reads = 0;
}

int od_hal_nvs_init(void)
{
    if (g_fail_init) { g_fail_init = 0; return OD_HAL_NVS_EIO; }
    return OD_HAL_NVS_OK;
}

int od_hal_nvs_size(uint32_t *len_out)
{
    if (len_out == NULL) { return OD_HAL_NVS_EIO; }
    *len_out = 0;
    if (g_fail_size) { g_fail_size = 0; return OD_HAL_NVS_EIO; }
    if (!g_present) { return OD_HAL_NVS_ENOENT; }
    *len_out = g_medium_len;
    return OD_HAL_NVS_OK;
}

int od_hal_nvs_read(uint32_t offset, void *buf, uint32_t len)
{
    if (buf == NULL) { return OD_HAL_NVS_EIO; }
    if (g_fail_read) { g_fail_read = 0; return OD_HAL_NVS_EIO; }
    if (g_fail_read_after == 0) { g_fail_read_after = -1; return OD_HAL_NVS_EIO; }
    if (g_fail_read_after > 0) { --g_fail_read_after; }
    if (!g_present) { return OD_HAL_NVS_ENOENT; }
    if (offset > g_medium_len || len > g_medium_len - offset) { return OD_HAL_NVS_E2BIG; }
    ++g_reads;
    if (len > 0u) { memcpy(buf, g_medium + offset, len); }
    return OD_HAL_NVS_OK;
}

int od_hal_nvs_write(const void *record, uint32_t len)
{
    if (record == NULL || len == 0u) { return OD_HAL_NVS_EIO; }
    if (len > sizeof(g_medium)) { return OD_HAL_NVS_E2BIG; }
    if (g_fail_write) {
        g_fail_write = 0;
        if (g_write_lands_despite_failure) {
            memcpy(g_medium, record, len);
            g_medium_len = len;
            g_present = true;
        }
        return OD_HAL_NVS_EIO;
    }
    memcpy(g_medium, record, len);
    g_medium_len = len;
    g_present = true;
    return OD_HAL_NVS_OK;
}

int od_hal_nvs_erase(void)
{
    if (g_fail_erase) { g_fail_erase = 0; return OD_HAL_NVS_EIO; }
    g_medium_len = 0;
    g_present = false;
    return OD_HAL_NVS_OK;
}

/* ------------------------------------------------------------------------- helpers ------- */

static uint8_t  g_workspace[OD_CONFIG_STORE_MAX_RECORD];
static uint8_t  g_payload[OD_CONFIG_MAX_SIZE];
static uint8_t  g_out[OD_CONFIG_MAX_SIZE];

static void fill(uint8_t *p, uint32_t n, uint8_t seed)
{
    for (uint32_t i = 0; i < n; ++i) {
        p[i] = (uint8_t)(seed + (i & 0xFFu));
    }
}

static uint32_t medium_u32(uint32_t offset)
{
    return (uint32_t)g_medium[offset]
         | ((uint32_t)g_medium[offset + 1] << 8)
         | ((uint32_t)g_medium[offset + 2] << 16)
         | ((uint32_t)g_medium[offset + 3] << 24);
}

static enum od_config_store_result save(const uint8_t *p, uint32_t len)
{
    return od_config_store_save(g_workspace, sizeof(g_workspace), p, len);
}

static enum od_config_store_result load(uint32_t cap, uint32_t *out_len)
{
    uint32_t n = cap;
    enum od_config_store_result r = od_config_store_load(g_out, &n);
    *out_len = n;
    return r;
}

static void test_store_logging(void)
{
    uint32_t len;

    CASE("shared storage reports lifecycle outcomes");
    medium_reset();
    fill(g_payload, 64u, 0xA0u);
    logs_reset();
    CHECK(save(g_payload, 64u) == OD_CONFIG_STORE_OK);
    CHECK(load(sizeof(g_out), &len) == OD_CONFIG_STORE_OK);
#if OD_LOG_EFFECTIVE_LEVEL >= OD_LOG_INFO
    CHECK(logged_exact(OD_LOG_INFO, "Config stored: 64 bytes"));
    CHECK(logged_exact(OD_LOG_INFO, "Config loaded from storage: 64 bytes"));
#else
    CHECK(g_log_count == 0u);
#endif

    CASE("shared storage reports empty, corrupt, clear, and init failure outcomes");
    medium_reset();
    logs_reset();
    CHECK(load(sizeof(g_out), &len) == OD_CONFIG_STORE_EMPTY);
    CHECK(od_config_store_clear() == OD_CONFIG_STORE_OK);
    g_fail_init = 1;
    CHECK(od_config_store_init() == OD_CONFIG_STORE_IO);
#if OD_LOG_EFFECTIVE_LEVEL >= OD_LOG_INFO
    CHECK(logged_exact(OD_LOG_INFO, "No stored config found"));
    CHECK(logged_exact(OD_LOG_INFO, "Stored config cleared"));
    CHECK(logged_exact(OD_LOG_ERROR, "Config storage initialization failed"));
#else
    CHECK(g_log_count == 0u);
#endif

    medium_reset();
    CHECK(save(g_payload, 64u) == OD_CONFIG_STORE_OK);
    g_medium[16u] ^= 0x01u;
    logs_reset();
    CHECK(load(sizeof(g_out), &len) == OD_CONFIG_STORE_CORRUPT);
#if OD_LOG_EFFECTIVE_LEVEL >= OD_LOG_INFO
    CHECK(logged_exact(OD_LOG_ERROR, "Stored config is corrupt: CRC mismatch"));
#else
    CHECK(g_log_count == 0u);
#endif
}

/* ------------------------------------------------------------------------ the record ----- */

/* The CRC-32 the three targets shipped, checked against values computed by an independent
 * implementation rather than against this module's own output. */
static void test_crc32_reference(void)
{
    CASE("crc32 reference vectors");
    CHECK(od_config_store_crc32((const uint8_t *)"", 0u) == 0x00000000u);
    CHECK(od_config_store_crc32((const uint8_t *)"a", 1u) == 0xE8B7BE43u);
    CHECK(od_config_store_crc32((const uint8_t *)"abc", 3u) == 0x352441C2u);
    CHECK(od_config_store_crc32((const uint8_t *)"123456789", 9u) == 0xCBF43926u);
    CHECK(od_config_store_crc32((const uint8_t *)"The quick brown fox jumps over the lazy dog",
                                43u) == 0x414FA339u);
}

static void test_record_bytes(void)
{
    uint32_t len;

    CASE("the record laid out on the medium");
    medium_reset();
    fill(g_payload, 100u, 0x10u);
    CHECK(save(g_payload, 100u) == OD_CONFIG_STORE_OK);

    CHECK(g_medium_len == 16u + 100u);
    CHECK(medium_u32(0) == 0xDEADBEEFu);
    CHECK(medium_u32(4) == 1u);
    /* The literal, from zlib.crc32 over the same fixture -- not od_config_store_crc32()'s own
     * answer, which would agree with itself however wrong it was. */
    CHECK(medium_u32(8) == 0x725F702Eu);
    CHECK(medium_u32(12) == 100u);
    CHECK(memcmp(g_medium + 16, g_payload, 100u) == 0);

    /* Little-endian, spelled out: a struct write on a big-endian host would pass every check
     * above and still produce a record no deployed device can read. */
    CHECK(g_medium[0] == 0xEFu && g_medium[1] == 0xBEu &&
          g_medium[2] == 0xADu && g_medium[3] == 0xDEu);
    CHECK(g_medium[4] == 0x01u && g_medium[5] == 0x00u &&
          g_medium[6] == 0x00u && g_medium[7] == 0x00u);
    CHECK(g_medium[12] == 100u && g_medium[13] == 0x00u);

    CASE("round trip");
    CHECK(load(sizeof(g_out), &len) == OD_CONFIG_STORE_OK);
    CHECK(len == 100u);
    CHECK(memcmp(g_out, g_payload, 100u) == 0);
}

static void test_empty_payload(void)
{
    uint32_t len;

    CASE("a zero-length payload is a legal record");
    medium_reset();
    CHECK(save(g_payload, 0u) == OD_CONFIG_STORE_OK);
    CHECK(g_medium_len == 16u);
    CHECK(medium_u32(12) == 0u);
    CHECK(medium_u32(8) == od_config_store_crc32(g_payload, 0u));
    CHECK(load(sizeof(g_out), &len) == OD_CONFIG_STORE_OK);
    CHECK(len == 0u);
}

static void test_payload_already_in_place(void)
{
    uint32_t len;

    /* BG22's shape: the payload is already at offset 16 because the workspace overlays the
     * config assembler. The save must not copy it over itself, and the header write must not
     * disturb it. */
    CASE("payload aliasing the workspace slot");
    medium_reset();
    fill(od_config_store_payload(g_workspace), 300u, 0x77u);
    CHECK(save(od_config_store_payload(g_workspace), 300u) == OD_CONFIG_STORE_OK);
    CHECK(g_medium_len == 16u + 300u);
    CHECK(g_medium[16] == 0x77u);
    CHECK(g_medium[16 + 299u] == (uint8_t)(0x77u + (299u & 0xFFu)));
    CHECK(medium_u32(8) == 0x967B1377u);   /* independent: zlib.crc32 over the same 300 bytes */
    CHECK(load(sizeof(g_out), &len) == OD_CONFIG_STORE_OK);
    CHECK(len == 300u);
    CHECK(g_out[0] == 0x77u);
}

static void test_overlapping_payload(void)
{
    uint32_t len;

    /* memmove, not memcpy: a payload that overlaps the destination slot without being it must
     * still land intact. */
    CASE("payload overlapping the workspace slot but not aligned to it");
    medium_reset();
    fill(g_workspace + 24u, 200u, 0x40u);
    CHECK(od_config_store_save(g_workspace, sizeof(g_workspace), g_workspace + 24u, 200u)
          == OD_CONFIG_STORE_OK);
    CHECK(g_medium_len == 16u + 200u);
    for (uint32_t i = 0; i < 200u; ++i) {
        CHECK(g_medium[16u + i] == (uint8_t)(0x40u + (i & 0xFFu)));
    }
    CHECK(load(sizeof(g_out), &len) == OD_CONFIG_STORE_OK);
    CHECK(len == 200u);

    CASE("payload inside the header region");
    medium_reset();
    fill(g_workspace + 4u, 64u, 0x50u);
    CHECK(od_config_store_save(g_workspace, sizeof(g_workspace), g_workspace + 4u, 64u)
          == OD_CONFIG_STORE_OK);
    for (uint32_t i = 0; i < 64u; ++i) {
        CHECK(g_medium[16u + i] == (uint8_t)(0x50u + (i & 0xFFu)));
    }
}

static void test_caps(void)
{
    uint32_t len;

    CASE("the payload cap is refused, never truncated");
    medium_reset();
    fill(g_payload, OD_CONFIG_MAX_SIZE, 0x01u);
    CHECK(save(g_payload, OD_CONFIG_MAX_SIZE) == OD_CONFIG_STORE_OK);
    CHECK(g_medium_len == 16u + OD_CONFIG_MAX_SIZE);

    medium_reset();
    CHECK(save(g_payload, OD_CONFIG_MAX_SIZE + 1u) == OD_CONFIG_STORE_TOO_BIG);
    CHECK(!g_present);   /* refused means nothing was stored */

    CASE("a workspace too small for the span is refused");
    medium_reset();
    CHECK(od_config_store_save(g_workspace, 16u + 50u, g_payload, 51u)
          == OD_CONFIG_STORE_TOO_BIG);
    CHECK(!g_present);
    CHECK(od_config_store_save(g_workspace, 16u + 50u, g_payload, 50u)
          == OD_CONFIG_STORE_OK);

    CASE("a record larger than the caller's buffer is refused, nothing copied");
    medium_reset();
    fill(g_payload, 400u, 0x20u);
    CHECK(save(g_payload, 400u) == OD_CONFIG_STORE_OK);
    memset(g_out, 0xCC, sizeof(g_out));
    CHECK(load(399u, &len) == OD_CONFIG_STORE_TOO_BIG);
    CHECK(len == 0u);
    CHECK(g_out[0] == 0xCCu);
    CHECK(load(400u, &len) == OD_CONFIG_STORE_OK);
    CHECK(len == 400u);
}

static void test_empty_device(void)
{
    uint32_t len;

    CASE("an unprovisioned device is EMPTY, not a failure");
    medium_reset();
    CHECK(load(sizeof(g_out), &len) == OD_CONFIG_STORE_EMPTY);
    CHECK(len == 0u);

    CASE("clearing an empty device succeeds");
    CHECK(od_config_store_clear() == OD_CONFIG_STORE_OK);

    CASE("clear removes the record");
    fill(g_payload, 64u, 0x05u);
    CHECK(save(g_payload, 64u) == OD_CONFIG_STORE_OK);
    CHECK(od_config_store_clear() == OD_CONFIG_STORE_OK);
    CHECK(load(sizeof(g_out), &len) == OD_CONFIG_STORE_EMPTY);
}

static void test_corruption(void)
{
    uint32_t len;

    CASE("a flipped payload byte fails CRC");
    medium_reset();
    fill(g_payload, 256u, 0x31u);
    CHECK(save(g_payload, 256u) == OD_CONFIG_STORE_OK);
    g_medium[16 + 128u] ^= 0x01u;
    CHECK(load(sizeof(g_out), &len) == OD_CONFIG_STORE_CORRUPT);
    CHECK(len == 0u);

    CASE("a wrong magic is rejected before anything else");
    medium_reset();
    CHECK(save(g_payload, 256u) == OD_CONFIG_STORE_OK);
    g_medium[0] ^= 0xFFu;
    CHECK(load(sizeof(g_out), &len) == OD_CONFIG_STORE_CORRUPT);

    CASE("a record shorter than the header is rejected");
    medium_reset();
    CHECK(save(g_payload, 256u) == OD_CONFIG_STORE_OK);
    g_medium_len = 15u;
    CHECK(load(sizeof(g_out), &len) == OD_CONFIG_STORE_CORRUPT);

    CASE("a header declaring more payload than the medium holds is rejected");
    medium_reset();
    CHECK(save(g_payload, 256u) == OD_CONFIG_STORE_OK);
    g_medium[12] = 0xFFu;   /* data_len 255 -> 511, but only 256 bytes follow */
    g_medium[13] = 0x01u;
    CHECK(load(sizeof(g_out), &len) == OD_CONFIG_STORE_CORRUPT);

    CASE("a header declaring more than this build's cap is refused");
    medium_reset();
    CHECK(save(g_payload, 256u) == OD_CONFIG_STORE_OK);
    g_medium[12] = 0x00u;
    g_medium[13] = 0x00u;
    g_medium[14] = 0x01u;   /* 65536 */
    CHECK(load(sizeof(g_out), &len) == OD_CONFIG_STORE_TOO_BIG);
}

/* A config write never reads the stored record, so whatever state it is in -- bad CRC, bad
 * magic, truncated, over-long header -- a new write replaces it. That is the repair path for a
 * device whose record is damaged. */
static void test_damaged_record_is_overwritten(void)
{
    uint32_t len;

    for (unsigned damage = 0u; damage < 4u; ++damage) {
        CASE("a new config write replaces a damaged stored record");
        medium_reset();
        fill(g_payload, 256u, 0x31u);
        CHECK(save(g_payload, 256u) == OD_CONFIG_STORE_OK);
        switch (damage) {
        case 0u: g_medium[16 + 128u] ^= 0x01u; break;   /* CRC */
        case 1u: g_medium[0] ^= 0xFFu; break;            /* magic */
        case 2u: g_medium_len = 15u; break;              /* shorter than the header */
        default: g_medium[13] = 0x01u; g_medium[12] = 0xFFu; break;   /* data_len overrun */
        }
        CHECK(load(sizeof(g_out), &len) != OD_CONFIG_STORE_OK);
        fill(g_payload, 100u, 0x77u);
        g_reads = 0;
        CHECK(save(g_payload, 100u) == OD_CONFIG_STORE_OK);
        CHECK(g_reads == 0u);
        CHECK(load(sizeof(g_out), &len) == OD_CONFIG_STORE_OK);
        CHECK(len == 100u);
        CHECK(memcmp(g_out, g_payload, 100u) == 0);
    }
}

static void test_oversized_physical_record(void)
{
    uint32_t len;

    /* The medium can hold more than a legal record: BG22's NVM3 objects go to 2112 bytes
     * against a 2064-byte record cap, and a device provisioned by a larger-cap build is how
     * one arrives. Refused on the stored span, before the header inside it is trusted. */
    CASE("a stored span longer than any legal record is refused");
    medium_reset();
    fill(g_payload, 64u, 0x11u);
    CHECK(save(g_payload, 64u) == OD_CONFIG_STORE_OK);
    /* A perfectly well-formed header and CRC, inside an over-long object. */
    g_medium_len = OD_CONFIG_STORE_MAX_RECORD + 1u;
    g_reads = 0;
    CHECK(load(sizeof(g_out), &len) == OD_CONFIG_STORE_TOO_BIG);
    CHECK(len == 0u);
    CHECK(g_reads == 0u);   /* refused before the header was read at all */

    CASE("exactly the maximum record still loads");
    medium_reset();
    fill(g_payload, OD_CONFIG_MAX_SIZE, 0x12u);
    CHECK(save(g_payload, OD_CONFIG_MAX_SIZE) == OD_CONFIG_STORE_OK);
    CHECK(g_medium_len == OD_CONFIG_STORE_MAX_RECORD);
    CHECK(load(sizeof(g_out), &len) == OD_CONFIG_STORE_OK);
    CHECK(len == OD_CONFIG_MAX_SIZE);
}

static void test_truncated_and_oversize_together(void)
{
    uint32_t len;

    /* Both faults at once. The order is ESP32's -- the authority per CLAUDE.md -- so the cap
     * is answered before the truncation. Pinned because the enum makes the classification
     * observable even though every caller collapses it to a refusal. */
    CASE("a record both truncated and larger than the caller's buffer reports the cap");
    medium_reset();
    fill(g_payload, 400u, 0x21u);
    CHECK(save(g_payload, 400u) == OD_CONFIG_STORE_OK);
    g_medium_len = 16u + 100u;             /* the medium holds far less than declared */
    CHECK(load(399u, &len) == OD_CONFIG_STORE_TOO_BIG);

    CASE("truncated alone is CORRUPT");
    CHECK(load(sizeof(g_out), &len) == OD_CONFIG_STORE_CORRUPT);
    CHECK(len == 0u);
}

static void test_version_is_carried_not_checked(void)
{
    uint32_t len;

    /* The behaviour that must NOT change: every target writes 1 and none reads it back, so a
     * record carrying anything else still loads. Enforcing it here would stop a device booting
     * on a config it has been using. */
    CASE("an unrecognised version still loads");
    medium_reset();
    fill(g_payload, 128u, 0x60u);
    CHECK(save(g_payload, 128u) == OD_CONFIG_STORE_OK);
    g_medium[4] = 2u;
    CHECK(load(sizeof(g_out), &len) == OD_CONFIG_STORE_OK);
    CHECK(len == 128u);
    CHECK(memcmp(g_out, g_payload, 128u) == 0);

    CASE("and so does 0xFFFFFFFF");
    g_medium[4] = 0xFFu; g_medium[5] = 0xFFu; g_medium[6] = 0xFFu; g_medium[7] = 0xFFu;
    CHECK(load(sizeof(g_out), &len) == OD_CONFIG_STORE_OK);
    CHECK(len == 128u);
}

static void test_medium_failures(void)
{
    uint32_t len;

    CASE("a failed write reports IO");
    medium_reset();
    fill(g_payload, 64u, 0x41u);
    g_fail_write = 1;
    CHECK(save(g_payload, 64u) == OD_CONFIG_STORE_IO);
    CHECK(!g_present);

    CASE("a failed write that landed anyway is still reported as IO");
    medium_reset();
    g_fail_write = 1;
    g_write_lands_despite_failure = true;
    CHECK(save(g_payload, 64u) == OD_CONFIG_STORE_IO);
    /* The core does not paper over the medium: what is stored is what the next load sees. */
    CHECK(load(sizeof(g_out), &len) == OD_CONFIG_STORE_OK);
    CHECK(len == 64u);

    CASE("a failed size query reports IO, not EMPTY");
    medium_reset();
    CHECK(save(g_payload, 64u) == OD_CONFIG_STORE_OK);
    g_fail_size = 1;
    CHECK(load(sizeof(g_out), &len) == OD_CONFIG_STORE_IO);
    CHECK(len == 0u);

    CASE("a failed header read reports IO");
    medium_reset();
    CHECK(save(g_payload, 64u) == OD_CONFIG_STORE_OK);
    g_fail_read = 1;
    CHECK(load(sizeof(g_out), &len) == OD_CONFIG_STORE_IO);
    CHECK(len == 0u);

    CASE("a failed payload read reports IO too");
    g_fail_read_after = 1;   /* let the header read through, fail the payload read */
    CHECK(load(sizeof(g_out), &len) == OD_CONFIG_STORE_IO);
    CHECK(len == 0u);

    CASE("a failed erase reports IO");
    g_fail_erase = 1;
    CHECK(od_config_store_clear() == OD_CONFIG_STORE_IO);

    CASE("a failed init reports IO");
    g_fail_init = 1;
    CHECK(od_config_store_init() == OD_CONFIG_STORE_IO);
    CHECK(od_config_store_init() == OD_CONFIG_STORE_OK);
}

static void test_null_arguments(void)
{
    uint32_t len = sizeof(g_out);

    CASE("null arguments are refused, not dereferenced");
    medium_reset();
    CHECK(od_config_store_save(NULL, 64u, g_payload, 1u) == OD_CONFIG_STORE_IO);
    CHECK(od_config_store_save(g_workspace, sizeof(g_workspace), NULL, 1u)
          == OD_CONFIG_STORE_IO);
    len = 1234u;
    CHECK(od_config_store_load(NULL, &len) == OD_CONFIG_STORE_IO);
    CHECK(len == 0u);   /* the contract: *len is 0 on EVERY non-OK return */
    CHECK(od_config_store_load(g_out, NULL) == OD_CONFIG_STORE_IO);
    CHECK(!g_present);
}

static void test_header_read_is_not_the_whole_record(void)
{
    uint32_t len;

    /* The header-first sequence is what lets BG22 read at an offset instead of staging the
     * record. Two reads, never one covering everything. */
    CASE("load reads the header and the payload separately");
    medium_reset();
    fill(g_payload, 512u, 0x90u);
    CHECK(save(g_payload, 512u) == OD_CONFIG_STORE_OK);
    g_reads = 0;
    CHECK(load(sizeof(g_out), &len) == OD_CONFIG_STORE_OK);
    CHECK(g_reads == 2u);

    CASE("an empty payload needs only the header read");
    medium_reset();
    CHECK(save(g_payload, 0u) == OD_CONFIG_STORE_OK);
    g_reads = 0;
    CHECK(load(sizeof(g_out), &len) == OD_CONFIG_STORE_OK);
    CHECK(g_reads == 1u);
}

int main(void)
{
    test_store_logging();
    test_crc32_reference();
    test_record_bytes();
    test_empty_payload();
    test_payload_already_in_place();
    test_overlapping_payload();
    test_caps();
    test_empty_device();
    test_corruption();
    test_damaged_record_is_overwritten();
    test_oversized_physical_record();
    test_truncated_and_oversize_together();
    test_version_is_carried_not_checked();
    test_medium_failures();
    test_null_arguments();
    test_header_read_is_not_the_whole_record();
    return OD_CHECK_REPORT_NONEMPTY("config_store_test", 370u);
}
