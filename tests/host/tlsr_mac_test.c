/* tlsr_mac_test.c -- the Telink tag address (targets/telink-tlsr/src/od_tlsr_mac.h): the flash
 * UID gives the legacy firmware's address; a missing, unstable or blank UID falls back to the MAC
 * record, then to a random address in Telink's vendor block. Nothing is written to flash. */
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
}

int main(void)
{
    test_uid_gives_legacy_address();
    test_bad_uid_falls_back_to_record();
    test_nothing_gives_random_vendor_address();
    printf("tlsr_mac: %u checks, %u failures\n", g_checks, g_failures);
    return g_failures == 0u ? 0 : 1;
}
