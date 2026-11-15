/* tlsr_ota_test.c -- the Telink updater (targets/telink-tlsr/src/od_tlsr_ota.c) over the
 * simulated flash: a sealed image goes through BEGIN / DATA / FINISH / INSTALL, and every way
 * of getting it wrong leaves the staged boot flag unset.
 *
 * The flag byte at staging + 8 is what the ROM looks for. The property is that it reads 0x4B
 * only after the whole image has been received and its CRC checked, and never otherwise. */
#include "od_config_store.h"
#include "od_tlsr_layout.h"
#include "od_tlsr_ota.h"

#include "fake_tlsr_flash.h"

#include <stdio.h>
#include <string.h>

static unsigned g_checks, g_failures;
#define CHECK(c)                                                                \
    do {                                                                        \
        ++g_checks;                                                             \
        if (!(c)) {                                                             \
            ++g_failures;                                                       \
            printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c);                  \
        }                                                                       \
    } while (0)

static uint8_t g_note[OD_TLSR_OTA_REPLY_MAX];
static uint8_t g_note_len;
static unsigned g_notes, g_feeds, g_installs;
static uint32_t g_install_len;

void od_tlsr_ota_app_notify(const uint8_t *reply, uint8_t len)
{
    memcpy(g_note, reply, len);
    g_note_len = len;
    g_notes++;
}

void od_tlsr_ota_app_before_install(void)
{
    g_feeds++;
}

static bool g_needs_unlock;

bool od_tlsr_ota_app_needs_unlock(void)
{
    return g_needs_unlock;
}

void od_tlsr_ota_app_boot_info(uint8_t info[2])
{
    info[0] = 2u;
    info[1] = 3u;
}

void od_tlsr_install(uint32_t len)
{
    g_installs++;
    g_install_len = len;
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/* A sealed image as tools/tlsr_image.py makes one: KNLT at 8, length at 0x18, body padded to
 * 16, trailing CRC-32 without its final inversion. */
static uint8_t IMG[0x21000];

static uint32_t reseal(uint32_t raw)
{
    uint32_t len;

    IMG[8] = 'K';
    IMG[9] = 'N';
    IMG[10] = 'L';
    IMG[11] = 'T';
    while ((raw & 15u) != 0u) {
        IMG[raw++] = 0xFFu;
    }
    len = raw + 4u;
    wr32(&IMG[OD_TLSR_HDR_SIZE_OFF], len);
    wr32(&IMG[raw], od_config_store_crc32(IMG, raw) ^ 0xFFFFFFFFu);
    return len;
}

static uint32_t make_image(uint32_t raw)
{
    uint32_t i;

    for (i = 0; i < raw; ++i) {
        IMG[i] = (uint8_t)(i * 13u + 5u);
    }
    return reseal(raw);
}

static void reset_all(void)
{
    fake_flash_reset();
    od_tlsr_ota_reset();
    g_notes = g_feeds = g_installs = 0u;
    g_note_len = 0u;
}

static uint8_t cmd(const uint8_t *d, uint16_t n, uint8_t *out)
{
    return od_tlsr_ota_write(d, n, out);
}

static void pump(unsigned rounds)
{
    while (rounds-- > 0u) {
        od_tlsr_ota_service(0u);
    }
}

static uint8_t staged_flag(void)
{
    return fake_flash[OD_TLSR_STAGE_BASE + OD_TLSR_HDR_FLAG_OFF];
}

static bool begin(uint32_t len)
{
    uint8_t d[5] = {0x01}, out[OD_TLSR_OTA_REPLY_MAX];
    unsigned before = g_notes;

    wr32(&d[1], len);
    if (cmd(d, 5u, out) != 0u) {
        return false;
    }
    pump(len / OD_TLSR_SECTOR + 2u);
    return g_notes == before + 1u && g_note[0] == 0x81u && g_note[1] == 0u && rd32(&g_note[2]) == len &&
           od_tlsr_ota_state() == OD_TLSR_OTA_RECEIVING;
}

static void data(uint32_t off, const uint8_t *p, uint32_t n)
{
    uint8_t d[5 + 256], out[OD_TLSR_OTA_REPLY_MAX];
    d[0] = 0x02u;
    wr32(&d[1], off);
    memcpy(&d[5], p, n);
    CHECK(cmd(d, (uint16_t)(5u + n), out) == 0u);
}

static void send_all(uint32_t len, uint32_t chunk)
{
    uint32_t off;
    for (off = 0; off < len; off += chunk) {
        data(off, &IMG[off], len - off < chunk ? len - off : chunk);
    }
}

/* FINISH, then run the verify pass to completion. Returns the final 0x83 status. */
static int finish(void)
{
    uint8_t f = 0x03u, out[OD_TLSR_OTA_REPLY_MAX];
    unsigned before = g_notes;
    uint8_t n = cmd(&f, 1u, out);

    if (n != 0u) {
        return out[0] == 0x83u ? out[1] : -1;
    }
    pump(64u);
    if (g_notes != before + 1u || g_note[0] != 0x83u) {
        return -1;
    }
    return g_note[1];
}

static void happy_path(void)
{
    uint32_t len = make_image(9000u);
    uint8_t out[OD_TLSR_OTA_REPLY_MAX], op;
    uint32_t i;

    reset_all();
    CHECK(begin(len));
    send_all(len, 240u);
    CHECK(staged_flag() == 0xFFu); /* held back */
    CHECK(finish() == OD_TLSR_OTA_OK);
    CHECK(rd32(&g_note[2]) == len);
    CHECK(od_tlsr_ota_state() == OD_TLSR_OTA_VERIFIED);
    CHECK(memcmp(&fake_flash[OD_TLSR_STAGE_BASE], IMG, len) == 0);
    /* nothing outside staging was touched */
    for (i = 0; i < FAKE_FLASH_SIZE; ++i) {
        if ((i < OD_TLSR_STAGE_BASE || i >= OD_TLSR_STAGE_BASE + len) && fake_flash[i] != 0xFFu) {
            CHECK(!"stray write");
            break;
        }
    }
    op = 0x05u;
    CHECK(cmd(&op, 1u, out) == 13u && out[0] == 0x85u && out[1] == OD_TLSR_OTA_VERIFIED &&
          rd32(&out[2]) == len && rd32(&out[6]) == len && out[10] == 0u);
    CHECK(out[11] == 2u && out[12] == 3u); /* the target's boot info, after the old fields */
    /* a link drop keeps a verified image */
    od_tlsr_ota_link_closed();
    CHECK(od_tlsr_ota_state() == OD_TLSR_OTA_VERIFIED);
    op = 0x04u;
    CHECK(cmd(&op, 1u, out) == 6u && out[0] == 0x84u && out[1] == 0u && rd32(&out[2]) == len);
    op = 0x06u;
    CHECK(cmd(&op, 1u, out) == 6u && out[1] == OD_TLSR_OTA_E_STATE); /* no abort once installing */
    od_tlsr_ota_service(1000u); /* arms the delay */
    od_tlsr_ota_service(2000u);
    CHECK(g_installs == 0u);
    od_tlsr_ota_service(2500u);
    CHECK(g_installs == 1u && g_install_len == len && g_feeds >= 1u);
}

static void failures(void)
{
    uint32_t len = make_image(5000u);
    uint8_t out[OD_TLSR_OTA_REPLY_MAX], op, d[9];

    /* corrupted byte: CRC fails, flag stays unset, INSTALL refused */
    reset_all();
    CHECK(begin(len));
    IMG[3000] ^= 1u;
    send_all(len, 200u);
    IMG[3000] ^= 1u;
    CHECK(finish() == OD_TLSR_OTA_E_CRC);
    CHECK(staged_flag() == 0xFFu);
    op = 0x04u;
    CHECK(cmd(&op, 1u, out) == 6u && out[1] == OD_TLSR_OTA_E_STATE);
    CHECK(g_installs == 0u);

    /* corrupted flag byte itself: the CRC covers the held-back value */
    reset_all();
    CHECK(begin(len));
    IMG[8] = 0x4Au;
    send_all(len, 200u);
    IMG[8] = 'K';
    CHECK(finish() == OD_TLSR_OTA_E_HEADER);
    CHECK(staged_flag() == 0xFFu);

    /* wrong length field in the header */
    reset_all();
    CHECK(begin(len - 16u));
    send_all(len - 16u, 200u);
    CHECK(finish() == OD_TLSR_OTA_E_HEADER);
    CHECK(staged_flag() == 0xFFu);

    /* a gap latches OFFSET; FINISH reports it */
    reset_all();
    CHECK(begin(len));
    data(0u, IMG, 200u);
    data(400u, &IMG[400], 200u);
    send_all(len, 200u); /* even a correct restart cannot clear the latch */
    CHECK(od_tlsr_ota_state() == OD_TLSR_OTA_FAILED);
    CHECK(finish() == OD_TLSR_OTA_E_OFFSET);
    CHECK(staged_flag() == 0xFFu);
    op = 0x05u;
    CHECK(cmd(&op, 1u, out) == 13u && out[1] == OD_TLSR_OTA_FAILED && out[10] == OD_TLSR_OTA_E_OFFSET);

    /* short image: FINISH before the end */
    reset_all();
    CHECK(begin(len));
    send_all(len - 100u, 200u);
    CHECK(finish() == OD_TLSR_OTA_E_OFFSET);

    /* overrun past the declared length */
    reset_all();
    CHECK(begin(len));
    send_all(len - 4u, 200u);
    data(len - 4u, &IMG[len - 4u], 8u);
    CHECK(od_tlsr_ota_state() == OD_TLSR_OTA_FAILED);

    /* DATA before BEGIN */
    reset_all();
    data(0u, IMG, 16u);
    CHECK(od_tlsr_ota_state() == OD_TLSR_OTA_FAILED);
    CHECK(staged_flag() == 0xFFu);

    /* lengths BEGIN refuses */
    reset_all();
    d[0] = 0x01u;
    wr32(&d[1], OD_TLSR_IMAGE_MIN - 1u);
    CHECK(cmd(d, 5u, out) == 6u && out[0] == 0x81u && out[1] == OD_TLSR_OTA_E_LENGTH);
    wr32(&d[1], OD_TLSR_IMAGE_MAX + 1u);
    CHECK(cmd(d, 5u, out) == 6u && out[1] == OD_TLSR_OTA_E_LENGTH);
    CHECK(cmd(d, 4u, out) == 6u && out[1] == OD_TLSR_OTA_E_LENGTH);
    CHECK(od_tlsr_ota_state() == OD_TLSR_OTA_IDLE);

    /* the largest image fits staging exactly; BEGIN erases only staging */
    reset_all();
    memset(fake_flash, 0x00, FAKE_FLASH_SIZE);
    wr32(&d[1], OD_TLSR_IMAGE_MAX);
    CHECK(cmd(d, 5u, out) == 0u);
    pump(OD_TLSR_IMAGE_MAX / OD_TLSR_SECTOR + 2u);
    CHECK(od_tlsr_ota_state() == OD_TLSR_OTA_RECEIVING);
    CHECK(fake_flash[OD_TLSR_STAGE_BASE - 1u] == 0x00u);
    CHECK(fake_flash[OD_TLSR_STAGE_BASE] == 0xFFu);
    CHECK(fake_flash[OD_TLSR_STAGE_BASE + OD_TLSR_IMAGE_MAX - 1u] == 0xFFu);
    CHECK(fake_flash[OD_TLSR_STAGE_BASE + OD_TLSR_IMAGE_MAX] == 0x00u);

    /* a link drop mid-transfer drops it */
    reset_all();
    CHECK(begin(len));
    send_all(1000u, 200u);
    od_tlsr_ota_link_closed();
    CHECK(od_tlsr_ota_state() == OD_TLSR_OTA_IDLE);

    /* ABORT, unknown opcode, empty write */
    reset_all();
    CHECK(begin(len));
    op = 0x06u;
    CHECK(cmd(&op, 1u, out) == 6u && out[0] == 0x86u && out[1] == 0u);
    CHECK(od_tlsr_ota_state() == OD_TLSR_OTA_IDLE);
    op = 0x7Eu;
    CHECK(cmd(&op, 1u, out) == 6u && out[0] == 0xFEu && out[1] == OD_TLSR_OTA_E_OPCODE);
    CHECK(od_tlsr_ota_write(NULL, 1u, out) == 0u);
    CHECK(od_tlsr_ota_write(&op, 0u, out) == 0u);
}

/* The SEC2 gate: everything but STATUS refused until unlock; the unlock ends with the link. */
static void gate(void)
{
    uint8_t out[OD_TLSR_OTA_REPLY_MAX];
    uint8_t op;
    uint8_t b[5] = { 0x01u };
    uint32_t len;

    reset_all();
    g_needs_unlock = true;
    len = make_image(4000u);
    wr32(&b[1], len);
    CHECK(cmd(b, sizeof b, out) == 6u && out[0] == 0x81u && out[1] == OD_TLSR_OTA_E_LOCKED);
    CHECK(od_tlsr_ota_state() == OD_TLSR_OTA_IDLE);
    for (op = 0x02u; op <= 0x06u; ++op) {
        if (op == 0x05u) {
            CHECK(cmd(&op, 1u, out) == 13u && out[0] == 0x85u);
        } else {
            CHECK(cmd(&op, 1u, out) == 6u && out[1] == OD_TLSR_OTA_E_LOCKED);
        }
    }
    od_tlsr_ota_unlock();
    CHECK(begin(len));
    od_tlsr_ota_link_closed();
    CHECK(cmd(b, sizeof b, out) == 6u && out[1] == OD_TLSR_OTA_E_LOCKED);
    g_needs_unlock = false;
    CHECK(begin(len));
    reset_all();
}

/* H2: an image with the boot flag at the 0x20000 multi-boot slot never verifies. */
static void multiboot_slot(void)
{
    uint32_t len;

    reset_all();
    g_needs_unlock = false;
    len = make_image(0x20100u);
    CHECK(IMG[OD_TLSR_MULTI_BOOT_A + 8u] != OD_TLSR_HDR_FLAG);
    CHECK(begin(len));
    send_all(len, 240u);
    CHECK(finish() == OD_TLSR_OTA_OK);

    reset_all();
    for (len = 0u; len < 0x20100u; ++len) {
        IMG[len] = (uint8_t)(len * 13u + 5u);
    }
    IMG[OD_TLSR_MULTI_BOOT_A + 8u] = OD_TLSR_HDR_FLAG;
    len = reseal(0x20100u);
    CHECK(begin(len));
    send_all(len, 240u);
    CHECK(finish() == OD_TLSR_OTA_E_HEADER);
    CHECK(staged_flag() == 0xFFu);
    reset_all();
}

/* M1: a low supply refuses every flash step; a verified image stays staged and verified. */
static void supply(void)
{
    uint8_t out[OD_TLSR_OTA_REPLY_MAX];
    uint8_t b[5] = { 0x01u };
    uint8_t op;
    uint32_t len = make_image(6000u), i;
    bool untouched = true;

    reset_all();
    g_needs_unlock = false;
    wr32(&b[1], len);
    fake_flash_supply_low = true;
    CHECK(cmd(b, sizeof b, out) == 6u && out[0] == 0x81u && out[1] == OD_TLSR_OTA_E_SUPPLY);
    pump(4u);
    CHECK(od_tlsr_ota_state() == OD_TLSR_OTA_IDLE && fake_flash_ops == 0u);

    /* the supply drops during the erase */
    fake_flash_supply_low = false;
    CHECK(cmd(b, sizeof b, out) == 0u);
    pump(1u);
    fake_flash_supply_low = true;
    pump(1u);
    CHECK(od_tlsr_ota_state() == OD_TLSR_OTA_FAILED && g_note[0] == 0x81u &&
          g_note[1] == OD_TLSR_OTA_E_SUPPLY);

    /* during DATA: nothing programmed, the error latches */
    reset_all();
    CHECK(begin(len));
    fake_flash_supply_low = true;
    data(0u, IMG, 200u);
    CHECK(od_tlsr_ota_state() == OD_TLSR_OTA_FAILED);
    for (i = 0; i < 200u; ++i) {
        untouched = untouched && fake_flash[OD_TLSR_STAGE_BASE + i] == 0xFFu;
    }
    CHECK(untouched);
    CHECK(finish() == OD_TLSR_OTA_E_SUPPLY);

    /* at INSTALL, and again when the install falls due */
    reset_all();
    CHECK(begin(len));
    send_all(len, 240u);
    CHECK(finish() == OD_TLSR_OTA_OK);
    fake_flash_supply_low = true;
    op = 0x04u;
    CHECK(cmd(&op, 1u, out) == 6u && out[0] == 0x84u && out[1] == OD_TLSR_OTA_E_SUPPLY);
    CHECK(od_tlsr_ota_state() == OD_TLSR_OTA_VERIFIED);
    fake_flash_supply_low = false;
    CHECK(cmd(&op, 1u, out) == 6u && out[1] == OD_TLSR_OTA_OK);
    od_tlsr_ota_service(0u);           /* arms the delay */
    fake_flash_supply_low = true;
    od_tlsr_ota_service(5000u);
    CHECK(g_installs == 0u && od_tlsr_ota_state() == OD_TLSR_OTA_VERIFIED);
    CHECK(g_note[0] == 0x84u && g_note[1] == OD_TLSR_OTA_E_SUPPLY);
    CHECK(staged_flag() == 0x4Bu);
    fake_flash_supply_low = false;
    CHECK(cmd(&op, 1u, out) == 6u && out[1] == OD_TLSR_OTA_OK);
    od_tlsr_ota_service(0u);
    od_tlsr_ota_service(5000u);
    CHECK(g_installs == 1u && g_install_len == len);
    reset_all();
}

int main(void)
{
    happy_path();
    failures();
    gate();
    supply();
    multiboot_slot();
    printf("tlsr_ota: %u checks, %u failures\n", g_checks, g_failures);
    return g_failures == 0u ? 0 : 1;
}
