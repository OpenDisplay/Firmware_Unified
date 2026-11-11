/* The BG22 config overlay: one 2,064-byte object serving as both od_config_asm reassembly state
 * and the NVM3 record, against a fake NVM3.
 *
 * WHY THIS EXISTS SEPARATELY from the two static assertions in opendisplay_config_storage.c.
 * Those compare LAYOUT -- buffer offsets and total size -- and layout was never the risk. The risk
 * is SEQUENCING: saveConfig() writes a 16-byte header over the four live assembler state words
 * (active, total_size, received, chunks), so the declared length has to have been captured before
 * that write, the reset has to come after it, and a caller must not read s->received afterwards.
 * No amount of offsetof proves any of that. Every case below is written so that getting the order
 * wrong changes an observable, and the ones that pin an ordering say which reordering they catch.
 */

#include "opendisplay_config_storage.h"

#include "nvm3_default.h"
#include "od_config_asm.h"
#include "od_config_store.h"
#include "od_span.h"

#include <stdio.h>
#include <string.h>

static unsigned g_checks;
static unsigned g_failures;
static const char *g_case = "(none)";

#define CHECK(cond)                                                            \
    do {                                                                       \
        ++g_checks;                                                            \
        if (!(cond)) {                                                         \
            ++g_failures;                                                      \
            printf("FAIL %s:%d [%s] %s\n", __FILE__, __LINE__, g_case, #cond); \
        }                                                                      \
    } while (0)
#define CASE(name) (g_case = (name))

#define HDR_LEN     16u
#define REC_MAGIC   0xDEADBEEFu
#define REC_VERSION 1u

/* The record header as the device persists it, transcribed independently of the storage file so a
 * layout change there has to be restated here rather than silently agreeing with itself. */
static uint32_t le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}
static uint32_t wrote_magic(void)    { return le32(&nvm3_fake_last_write[0]); }
static uint32_t wrote_version(void)  { return le32(&nvm3_fake_last_write[4]); }
static uint32_t wrote_crc(void)      { return le32(&nvm3_fake_last_write[8]); }
static uint32_t wrote_data_len(void) { return le32(&nvm3_fake_last_write[12]); }

/* A reboot: static RAM is zero, flash is not. Zeroing through the assembler pointer clears the
 * whole overlay, both members, because the two are the same object. */
static void simulate_reboot(void)
{
    memset(opendisplay_config_assembler(), 0, sizeof(struct od_config_asm));
}

static void setup(void)
{
    nvm3_fake_reset();
    od_config_asm_reset(opendisplay_config_assembler());
}

/* Drive a real chunked CONFIG_WRITE into the shared assembler, exactly as od_cmd_app_config_write
 * does, and return the assembled length. `seed` varies the payload so a stale buffer cannot pass. */
static uint32_t assemble(uint16_t total, uint8_t seed)
{
    struct od_config_asm *s = opendisplay_config_assembler();
    static uint8_t frame[CONFIG_CHUNK_SIZE_WITH_PREFIX];
    uint32_t written = 0u;
    enum od_config_asm_result rc;

    frame[0] = (uint8_t)(total & 0xffu);
    frame[1] = (uint8_t)(total >> 8);
    for (uint32_t i = 0u; i < CONFIG_CHUNK_SIZE; ++i) {
        frame[2u + i] = (uint8_t)(seed + i);
    }
    rc = od_config_asm_start(s, od_span_make(frame, sizeof frame));
    CHECK(rc == OD_CONFIG_ASM_ACCEPTED);
    written = CONFIG_CHUNK_SIZE;

    while (rc == OD_CONFIG_ASM_ACCEPTED) {
        uint32_t n = total - written;
        if (n > CONFIG_CHUNK_SIZE) n = CONFIG_CHUNK_SIZE;
        for (uint32_t i = 0u; i < n; ++i) {
            frame[i] = (uint8_t)(seed + written + i);
        }
        rc = od_config_asm_chunk(s, od_span_make(frame, n));
        written += n;
    }
    CHECK(rc == OD_CONFIG_ASM_COMPLETE);
    CHECK(s->received == total);
    return written;
}

static bool payload_matches(const uint8_t *p, uint32_t len, uint8_t seed)
{
    for (uint32_t i = 0u; i < len; ++i) {
        if (p[i] != (uint8_t)(seed + i)) return false;
    }
    return true;
}

/* ------------------------------------------------------------------ the overlay write itself */

static void test_header_overwrites_live_assembler_state(void)
{
    struct od_config_asm *s = opendisplay_config_assembler();
    uint32_t len;

    CASE("the persisted image is header-then-payload, built from state the header then destroys");
    setup();
    len = assemble(1000u, 0x11u);
    /* The declared length is read from the assembler by the CALLER, before saveConfig overwrites
     * the word holding it. Capturing it here is what od_cmd_app_config_write does. */
    CHECK(saveConfig(s->buffer, len));
    CHECK(nvm3_fake_writes == 1u);
    CHECK(nvm3_fake_last_write_len == HDR_LEN + 1000u);
    CHECK(wrote_magic() == REC_MAGIC);
    CHECK(wrote_version() == REC_VERSION);
    CHECK(wrote_data_len() == 1000u);
    CHECK(payload_matches(&nvm3_fake_last_write[HDR_LEN], 1000u, 0x11u));

    CASE("reset follows the write, so the header reaches flash before the state words are zeroed");
    /* Catches reordering od_config_asm_reset() ahead of nvm3_writeData(): the reset zeroes the
     * first 16 bytes, which ARE the header, so magic would persist as 0 and every later load
     * would fail. A live assembler after the write is the opposite defect. */
    CHECK(wrote_magic() != 0u);
    CHECK(!s->active);
    CHECK(s->received == 0u);

    CASE("the CRC covers the payload as stored, not the header");
    {
        uint32_t expect = od_config_store_crc32(&nvm3_fake_last_write[HDR_LEN], 1000u);
        CHECK(wrote_crc() == expect);
    }
}

static void test_aliased_and_external_saves_agree(void)
{
    struct od_config_asm *s = opendisplay_config_assembler();
    static uint8_t external[1000];
    static uint8_t via_overlay[HDR_LEN + 1000u];
    uint32_t len;

    CASE("saving from the overlay and from an unrelated buffer persist identical images");
    /* saveConfig skips its memmove when the source already IS the record's data field. That branch
     * is the aliasing the overlay creates; both arms must produce the same record.
     *
     * The overlay is POISONED with different bytes before the external arm. Seeding both arms
     * alike would let a missing memmove pass: the stale overlay contents would coincidentally be
     * the expected payload, and the two images would agree while the copy never happened. */
    setup();
    len = assemble(1000u, 0x40u);
    CHECK(saveConfig(s->buffer, len));
    memcpy(via_overlay, nvm3_fake_last_write, sizeof via_overlay);

    setup();
    memset(s->buffer, 0xeeu, 1000u);
    for (uint32_t i = 0u; i < sizeof external; ++i) external[i] = (uint8_t)(0x40u + i);
    CHECK(saveConfig(external, sizeof external));
    CHECK(nvm3_fake_last_write_len == sizeof via_overlay);
    CHECK(memcmp(nvm3_fake_last_write, via_overlay, sizeof via_overlay) == 0);
    /* Stated independently of the other arm, so this case cannot pass by both being poison. */
    CHECK(payload_matches(&nvm3_fake_last_write[HDR_LEN], sizeof external, 0x40u));
}

/* ------------------------------------------------------------------------ read back on reboot */

static void test_read_back_across_reboot(void)
{
    struct od_config_asm *s = opendisplay_config_assembler();
    uint32_t len;
    uint32_t out_len;

    CASE("a config written through the overlay survives a reboot and reads back byte-identically");
    setup();
    len = assemble(2048u, 0x7bu);
    CHECK(len == 2048u);
    CHECK(saveConfig(s->buffer, len));

    simulate_reboot();
    CHECK(!s->active);
    CHECK(initConfigStorage());
    out_len = OD_CONFIG_MAX_SIZE;
    /* The CONFIG_READ path loads straight back into the overlay's buffer. */
    CHECK(loadConfig(s->buffer, &out_len));
    CHECK(out_len == 2048u);
    CHECK(payload_matches(s->buffer, 2048u, 0x7bu));

    CASE("the smallest chunked config round-trips too, so the cap is not the only size proven");
    setup();
    len = assemble((uint16_t)(CONFIG_CHUNK_SIZE + 1u), 0x03u);
    CHECK(saveConfig(s->buffer, len));
    simulate_reboot();
    out_len = OD_CONFIG_MAX_SIZE;
    CHECK(loadConfig(s->buffer, &out_len));
    CHECK(out_len == CONFIG_CHUNK_SIZE + 1u);
    CHECK(payload_matches(s->buffer, out_len, 0x03u));
}

static void test_load_rejects_damaged_records(void)
{
    struct od_config_asm *s = opendisplay_config_assembler();
    uint32_t out_len;

    CASE("a flipped payload byte fails the CRC rather than loading");
    setup();
    (void)assemble(600u, 0x21u);
    CHECK(saveConfig(s->buffer, 600u));
    nvm3_fake_object[HDR_LEN + 100u] ^= 0xffu;
    simulate_reboot();
    out_len = OD_CONFIG_MAX_SIZE;
    CHECK(!loadConfig(s->buffer, &out_len));

    CASE("a wrong magic is refused before any length is trusted");
    setup();
    (void)assemble(600u, 0x21u);
    CHECK(saveConfig(s->buffer, 600u));
    nvm3_fake_object[0] ^= 0xffu;
    simulate_reboot();
    out_len = OD_CONFIG_MAX_SIZE;
    CHECK(!loadConfig(s->buffer, &out_len));

    CASE("a data_len longer than the stored object is refused");
    setup();
    (void)assemble(600u, 0x21u);
    CHECK(saveConfig(s->buffer, 600u));
    nvm3_fake_object[12] = 0xffu;   /* data_len -> 0x000002ff, past the 600-byte body */
    nvm3_fake_object[13] = 0x02u;
    simulate_reboot();
    out_len = OD_CONFIG_MAX_SIZE;
    CHECK(!loadConfig(s->buffer, &out_len));

    CASE("a caller buffer smaller than the stored config is refused, never truncated into");
    setup();
    (void)assemble(600u, 0x21u);
    CHECK(saveConfig(s->buffer, 600u));
    simulate_reboot();
    out_len = 599u;
    CHECK(!loadConfig(s->buffer, &out_len));

    CASE("an absent record is not a load");
    setup();
    out_len = OD_CONFIG_MAX_SIZE;
    CHECK(!loadConfig(s->buffer, &out_len));
}

/* ------------------------------------------------------------------------- failure behaviour */

/* A damaged stored record never blocks its replacement: saveConfig reads nothing back, so a
 * client can always repair a device by writing a whole config over it. */
static void test_damaged_record_is_overwritten(void)
{
    static const unsigned pokes[] = { 0u, 8u, 12u, HDR_LEN + 100u };
    struct od_config_asm *s = opendisplay_config_assembler();
    uint32_t out_len;

    CASE("a new config write replaces a damaged stored record");
    for (unsigned i = 0u; i < sizeof pokes / sizeof pokes[0]; ++i) {
        setup();
        (void)assemble(600u, 0x21u);
        CHECK(saveConfig(s->buffer, 600u));
        nvm3_fake_object[pokes[i]] ^= 0xffu;
        simulate_reboot();
        out_len = OD_CONFIG_MAX_SIZE;
        CHECK(!loadConfig(s->buffer, &out_len));
        (void)assemble(400u, 0x43u);
        CHECK(saveConfig(s->buffer, 400u));
        simulate_reboot();
        out_len = OD_CONFIG_MAX_SIZE;
        CHECK(loadConfig(s->buffer, &out_len));
        CHECK(out_len == 400u);
        CHECK(payload_matches(s->buffer, 400u, 0x43u));
    }
}

static void test_write_failure_preserves_prior_record(void)
{
    struct od_config_asm *s = opendisplay_config_assembler();
    uint32_t out_len;

    CASE("a failed write leaves the previously stored config readable");
    setup();
    (void)assemble(400u, 0x90u);
    CHECK(saveConfig(s->buffer, 400u));

    od_config_asm_reset(s);
    (void)assemble(800u, 0xc0u);
    nvm3_fake_write_status = SL_STATUS_FAIL;
    CHECK(!saveConfig(s->buffer, 800u));

    nvm3_fake_write_status = SL_STATUS_OK;
    simulate_reboot();
    out_len = OD_CONFIG_MAX_SIZE;
    CHECK(loadConfig(s->buffer, &out_len));
    CHECK(out_len == 400u);
    CHECK(payload_matches(s->buffer, 400u, 0x90u));

    CASE("a failed write still resets the assembler, whose state words the header already ate");
    /* Not tidiness: the four state words were overwritten before nvm3_writeData was called, so
     * leaving `active` set would resume a transfer against a header. */
    setup();
    (void)assemble(400u, 0x90u);
    nvm3_fake_write_status = SL_STATUS_FAIL;
    CHECK(!saveConfig(s->buffer, 400u));
    CHECK(!s->active);
    CHECK(s->received == 0u);

    CASE("an unmounted NVM3 refuses save, load and clear before it can eat assembler state");
    /* Deliberately mid-transfer: 600 declared, one 200-byte chunk in. saveConfig's guards run
     * ahead of the header write, so a refusal must leave the four state words -- and therefore
     * the in-flight transfer -- intact. */
    setup();
    {
        static uint8_t frame[CONFIG_CHUNK_SIZE_WITH_PREFIX];
        frame[0] = 0x58u; frame[1] = 0x02u;   /* declare 600 */
        memset(&frame[2], 0x90u, CONFIG_CHUNK_SIZE);
        CHECK(od_config_asm_start(s, od_span_make(frame, sizeof frame)) ==
              OD_CONFIG_ASM_ACCEPTED);
    }
    CHECK(s->active);
    nvm3_fake_set_mounted(false);
    CHECK(!initConfigStorage());
    CHECK(!saveConfig(s->buffer, 600u));
    CHECK(nvm3_fake_writes == 0u);
    CHECK(s->active);
    CHECK(s->received == CONFIG_CHUNK_SIZE);
    out_len = OD_CONFIG_MAX_SIZE;
    CHECK(!loadConfig(s->buffer, &out_len));
    CHECK(!clearStoredConfig());
    nvm3_fake_set_mounted(true);
}

static void test_on_demand_open(void)
{
    struct od_config_asm *s = opendisplay_config_assembler();
    uint32_t out_len;

    /* nvm3_defaultHandle is statically bound, so a pointer test says nothing about whether the
     * instance was ever opened -- hasBeenOpened does. Every entry point must therefore open on
     * demand rather than assume some earlier call did. The fake now refuses operations on a
     * closed handle, so an adapter that stopped doing this fails here. */
    CASE("a closed but openable instance is opened on demand by save");
    setup();
    (void)assemble(400u, 0x90u);
    nvm3_fake_set_opened(false);
    CHECK(saveConfig(s->buffer, 400u));
    CHECK(nvm3_fake_writes == 1u);

    CASE("and by load");
    nvm3_fake_set_opened(false);
    od_config_asm_reset(s);
    out_len = OD_CONFIG_MAX_SIZE;
    CHECK(loadConfig(s->buffer, &out_len));
    CHECK(out_len == 400u);
    CHECK(payload_matches(s->buffer, 400u, 0x90u));

    CASE("and by clear");
    nvm3_fake_set_opened(false);
    CHECK(clearStoredConfig());
    CHECK(nvm3_fake_deletes == 1u);

    CASE("and by init itself");
    nvm3_fake_set_opened(false);
    CHECK(initConfigStorage());
}

static void test_clear(void)
{
    struct od_config_asm *s = opendisplay_config_assembler();
    uint32_t out_len;

    CASE("clear removes the record, and clearing again still reports the requested postcondition");
    setup();
    (void)assemble(400u, 0x55u);
    CHECK(saveConfig(s->buffer, 400u));
    CHECK(clearStoredConfig());
    CHECK(!nvm3_fake_present);
    simulate_reboot();
    out_len = OD_CONFIG_MAX_SIZE;
    CHECK(!loadConfig(s->buffer, &out_len));
    /* KEY_NOT_FOUND is success here: od_cmd_app_config_clear must not NACK a clear of nothing. */
    CHECK(clearStoredConfig());

    CASE("a genuine delete fault is still a failure");
    setup();
    (void)assemble(400u, 0x55u);
    CHECK(saveConfig(s->buffer, 400u));
    nvm3_fake_delete_status = SL_STATUS_FAIL;
    CHECK(!clearStoredConfig());
    CHECK(nvm3_fake_present);
}

/* ------------------------------------------------------------------------------ the hazard */

static void test_load_into_a_live_assembler_destroys_it(void)
{
    struct od_config_asm *s = opendisplay_config_assembler();
    uint32_t out_len;

    CASE("loading into the overlay while assembly is live corrupts it -- why the guards exist");
    /* This asserts the HAZARD, not a behaviour to keep. Because storage and reassembly share one
     * buffer, a CONFIG_READ overlapping a CONFIG_WRITE would splice two configs into one
     * CRC-valid read-back. Two guards prevent it and neither is visible from this file:
     * od_cmd_app_config_read() refuses while `active`, and od_cmd_mutates_config() defers
     * 0x41/0x42/0x45 while a read producer holds the blob. If this case ever stops corrupting,
     * the overlay changed and those guards need re-deriving rather than trusting. */
    setup();
    (void)assemble(600u, 0x21u);
    CHECK(saveConfig(s->buffer, 600u));

    od_config_asm_reset(s);
    (void)assemble(800u, 0xa0u);
    CHECK(s->active == false);          /* 800 bytes completes assembly */
    od_config_asm_reset(s);

    /* A half-finished transfer, then a read into the same bytes. */
    {
        static uint8_t frame[CONFIG_CHUNK_SIZE_WITH_PREFIX];
        frame[0] = 0x58u; frame[1] = 0x02u;   /* declare 600 */
        for (uint32_t i = 0u; i < CONFIG_CHUNK_SIZE; ++i) frame[2u + i] = 0xa0u;
        CHECK(od_config_asm_start(s, od_span_make(frame, sizeof frame)) ==
              OD_CONFIG_ASM_ACCEPTED);
    }
    CHECK(s->buffer[0] == 0xa0u);
    out_len = OD_CONFIG_MAX_SIZE;
    CHECK(loadConfig(s->buffer, &out_len));
    CHECK(s->buffer[0] == 0x21u);       /* the in-flight chunk is gone */
}

int main(void)
{
    test_header_overwrites_live_assembler_state();
    test_aliased_and_external_saves_agree();
    test_read_back_across_reboot();
    test_load_rejects_damaged_records();
    test_damaged_record_is_overwritten();
    test_write_failure_preserves_prior_record();
    test_on_demand_open();
    test_clear();
    test_load_into_a_live_assembler_destroys_it();
    printf("silabs_storage: %u checks, %u failures\n", g_checks, g_failures);
    return g_failures == 0u ? 0 : 1;
}
