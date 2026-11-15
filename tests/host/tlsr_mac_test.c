/* tlsr_mac_test.c -- the Telink tag address (targets/telink-tlsr/src/od_tlsr_mac.h): the flash
 * UID gives the legacy firmware's address; a missing, unstable or blank UID falls back to the MAC
 * record, then to a random address in Telink's vendor block, made once and kept through the
 * target's hooks. The SDK's MAC record is never written. */
#include <stdint.h>

void flash_read_uid(unsigned char idcmd, unsigned char *buf);
void generateRandomNum(int len, unsigned char *data);

#include "od_tlsr_mac.h"

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

static unsigned char g_uid[2][16];
static unsigned g_uid_reads, g_uid_bad_cmd, g_random_calls;

void flash_read_uid(unsigned char idcmd, unsigned char *buf)
{
    if (idcmd != 0x4Bu) {
        ++g_uid_bad_cmd;
    }
    memcpy(buf, g_uid[g_uid_reads > 0u ? 1 : 0], 16u);
    ++g_uid_reads;
}

void generateRandomNum(int len, unsigned char *data)
{
    int i;

    ++g_random_calls;
    for (i = 0; i < len; ++i) {
        data[i] = (unsigned char)(0x10 + i);
    }
}

/* The target's keep-the-random-address hooks, over one slot. */
static unsigned char g_kept[6];
static int g_kept_valid, g_saves;

bool od_tlsr_mac_app_load(unsigned char mac[6])
{
    if (g_kept_valid) {
        memcpy(mac, g_kept, 6u);
    }
    return g_kept_valid != 0;
}

void od_tlsr_mac_app_save(const unsigned char mac[6])
{
    memcpy(g_kept, mac, 6u);
    g_kept_valid = 1;
    ++g_saves;
}

static void set_uid(const unsigned char uid[16])
{
    memcpy(g_uid[0], uid, 16u);
    memcpy(g_uid[1], uid, 16u);
    g_uid_reads = 0u;
    g_random_calls = 0u;
}

/* The address OD05B911 advertises, FA:FB:3E:05:B9:11, least significant byte first. */
static const unsigned char k_od05b911[6] = { 0x11, 0xB9, 0x05, 0x3E, 0xFB, 0xFA };

static void uid_for(const unsigned char mac[6], unsigned char uid[16])
{
    unsigned i;

    for (i = 0u; i < 16u; ++i) {
        uid[i] = (unsigned char)(0x5Au + 7u * i);
    }
    for (i = 0u; i < 6u; ++i) {
        uid[i + 8u] = (unsigned char)(uid[i] ^ mac[i]);
    }
}

static void test_uid_gives_legacy_address(void)
{
    unsigned char uid[16], mac[6];
    uint32_t ops;

    fake_flash_reset();
    memset(&fake_flash[OD_TLSR_MAC_ADDR], 0x42, 6u); /* a record is present but loses */
    uid_for(k_od05b911, uid);
    set_uid(uid);
    ops = (uint32_t)fake_flash_ops;
    od_tlsr_mac_read(mac);
    CHECK(memcmp(mac, k_od05b911, 6u) == 0);
    CHECK(g_uid_reads == 2u && g_uid_bad_cmd == 0u && g_random_calls == 0u);
    CHECK(fake_flash_ops == ops);
}

static void test_bad_uid_falls_back_to_record(void)
{
    static const unsigned char record[6] = { 1, 2, 3, 4, 5, 6 };
    unsigned char uid[16], mac[6];
    unsigned i;

    fake_flash_reset();
    memcpy(&fake_flash[OD_TLSR_MAC_ADDR], record, 6u);

    for (i = 0u; i < 16u; ++i) {
        uid[i] = (i & 1u) ? 0x01u : 0x51u; /* the SDK's "no UID" answer */
    }
    set_uid(uid);
    od_tlsr_mac_read(mac);
    CHECK(memcmp(mac, record, 6u) == 0);

    memset(uid, 0xFF, sizeof uid);
    set_uid(uid);
    od_tlsr_mac_read(mac);
    CHECK(memcmp(mac, record, 6u) == 0);

    memset(uid, 0x00, sizeof uid);
    set_uid(uid);
    od_tlsr_mac_read(mac);
    CHECK(memcmp(mac, record, 6u) == 0);

    uid_for(k_od05b911, uid);
    set_uid(uid);
    g_uid[1][3] ^= 0x01u; /* two reads disagree */
    od_tlsr_mac_read(mac);
    CHECK(memcmp(mac, record, 6u) == 0);
    CHECK(g_random_calls == 0u);
}

static void test_nothing_gives_random_vendor_address(void)
{
    static const unsigned char want[6] = { 0x10, 0x11, 0x12, 0x38, 0xC1, 0xA4 };
    unsigned char uid[16], mac[6];
    uint32_t ops;

    fake_flash_reset();
    memset(uid, 0xFF, sizeof uid);
    set_uid(uid);
    ops = (uint32_t)fake_flash_ops;
    od_tlsr_mac_read(mac);
    CHECK(memcmp(mac, want, 6u) == 0);
    CHECK(g_random_calls == 1u);
    CHECK(fake_flash_ops == ops);
    for (unsigned i = 0u; i < 8u; ++i) {
        CHECK(fake_flash[OD_TLSR_MAC_ADDR + i] == 0xFFu);
    }
    CHECK(g_saves == 1 && memcmp(g_kept, want, 6u) == 0);
}

/* The next boot gets the kept address, not a new one: it is also the SEC2 device id. */
static void test_random_address_is_kept(void)
{
    static const unsigned char kept[6] = { 0x77, 0x66, 0x55, 0x38, 0xC1, 0xA4 };
    unsigned char uid[16], mac[6];

    fake_flash_reset();
    memset(uid, 0xFF, sizeof uid);
    set_uid(uid);
    memcpy(g_kept, kept, 6u);
    g_kept_valid = 1;
    g_saves = 0;
    od_tlsr_mac_read(mac);
    CHECK(memcmp(mac, kept, 6u) == 0);
    CHECK(g_random_calls == 0u && g_saves == 0);

    /* a tag with a UID never consults or fills the slot */
    g_kept_valid = 0;
    uid_for(k_od05b911, uid);
    set_uid(uid);
    od_tlsr_mac_read(mac);
    CHECK(memcmp(mac, k_od05b911, 6u) == 0);
    CHECK(g_kept_valid == 0 && g_saves == 0);
}

int main(void)
{
    test_uid_gives_legacy_address();
    test_bad_uid_falls_back_to_record();
    test_nothing_gives_random_vendor_address();
    test_random_address_is_kept();
    printf("tlsr_mac: %u checks, %u failures\n", g_checks, g_failures);
    return g_failures == 0u ? 0 : 1;
}
