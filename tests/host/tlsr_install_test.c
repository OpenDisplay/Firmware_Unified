/* tlsr_install_test.c -- the Telink install routine (targets/telink-tlsr/src/od_tlsr_install.c)
 * over a simulated NOR flash.
 *
 * The property: whatever happens -- a power cut at any erase or program, or page programs that
 * do not take -- the flash is left with a boot flag only on a complete image. Either the image
 * at 0 carries the flag and is the new image byte for byte, or it does not and the staged copy
 * still carries its own flag and is intact (the ROM's multi-boot fallback, which this test
 * cannot prove the ROM takes). A clean run ends with only the new image bootable. */
#include "od_tlsr_layout.h"
#include "od_tlsr_ota.h"

#include "fake_tlsr_flash.h"
#include "tlsr_install_host.h"

#include <setjmp.h>
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

/* ---- the simulated mspi ---- */

static jmp_buf g_jmp;
static long g_budget;          /* erase/program operations allowed before the cut; <0 none */
static unsigned long g_writes; /* erase + program operations so far */
static unsigned long g_bad_page = 0xFFFFFFFFul; /* programs of this page leave a wrong byte */
static unsigned g_bad_left;    /* ... this many times */

void install_host_reset(void)
{
    longjmp(g_jmp, 2);
}

void flash_mspi_read_ram(unsigned char cmd, unsigned long addr, unsigned char addr_en,
                         unsigned char dummy_cnt, unsigned char *data, unsigned long data_len)
{
    (void)addr_en;
    (void)dummy_cnt;
    CHECK(cmd == FLASH_READ_CMD && addr + data_len <= FAKE_FLASH_SIZE);
    memcpy(data, &fake_flash[addr], data_len);
}

void flash_mspi_write_ram(unsigned char cmd, unsigned long addr, unsigned char addr_en,
                          unsigned char *data, unsigned long data_len)
{
    unsigned long i, n;
    int cut;

    (void)addr_en;
    g_writes++;
    cut = g_budget == 0;
    if (g_budget > 0) {
        g_budget--;
    }
    if (cmd == FLASH_SECT_ERASE_CMD) {
        CHECK((addr & (OD_TLSR_SECTOR - 1u)) == 0u);
        memset(&fake_flash[addr], 0xFF, cut ? OD_TLSR_SECTOR / 2u : OD_TLSR_SECTOR);
    } else {
        CHECK(cmd == FLASH_WRITE_CMD && data_len <= OD_TLSR_PAGE);
        CHECK((addr & ~(OD_TLSR_PAGE - 1ul)) == ((addr + data_len - 1u) & ~(OD_TLSR_PAGE - 1ul)));
        n = cut ? data_len / 2u : data_len;
        for (i = 0; i < n; ++i) {
            fake_flash[addr + i] &= data[i];
        }
        if (!cut && g_bad_left > 0u && (addr & ~(OD_TLSR_PAGE - 1ul)) == g_bad_page) {
            g_bad_left--;
            fake_flash[addr] ^= 0x01u; /* a bit that did not take, or was disturbed */
        }
    }
    if (cut) {
        longjmp(g_jmp, 1);
    }
}

/* ---- images ---- */

#define LEN (3u * OD_TLSR_SECTOR + 1712u) /* 3.4 sectors: a partial last sector */
static uint8_t OLD[LEN + 4096u], NEW[LEN];

static void make(uint8_t *img, uint32_t len, unsigned seed)
{
    uint32_t i, x = 0x9E3779B9u * (seed + 1u);

    for (i = 0; i < len; ++i) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        img[i] = (uint8_t)(x >> 24);
    }
    img[8] = OD_TLSR_HDR_FLAG;
    img[9] = 'N';
    img[10] = 'L';
    img[11] = 'T';
}

/* The running OLD image at 0 (longer than NEW), NEW staged and verified with its flag. */
static void setup(void)
{
    fake_flash_reset();
    memcpy(&fake_flash[OD_TLSR_IMAGE_BASE], OLD, sizeof OLD);
    memcpy(&fake_flash[OD_TLSR_STAGE_BASE], NEW, LEN);
    g_writes = 0u;
    g_budget = -1;
    g_bad_left = 0u;
    g_bad_page = 0xFFFFFFFFul;
}

enum boots { BOOTS_NEW, BOOTS_STAGED, BOOTS_NOTHING_SAFE };

static enum boots what_boots(void)
{
    if (fake_flash[OD_TLSR_HDR_FLAG_OFF] == OD_TLSR_HDR_FLAG) {
        return memcmp(&fake_flash[OD_TLSR_IMAGE_BASE], NEW, LEN) == 0 ? BOOTS_NEW
                                                                      : BOOTS_NOTHING_SAFE;
    }
    if (fake_flash[OD_TLSR_STAGE_BASE + OD_TLSR_HDR_FLAG_OFF] == OD_TLSR_HDR_FLAG &&
        memcmp(&fake_flash[OD_TLSR_STAGE_BASE], NEW, LEN) == 0) {
        return BOOTS_STAGED;
    }
    return BOOTS_NOTHING_SAFE;
}

/* Returns 1 for a cut, 2 for a reset. */
static int run(void)
{
    volatile int r = setjmp(g_jmp);

    if (r == 0) {
        od_tlsr_install(LEN);
        CHECK(!"od_tlsr_install returned");
    }
    return r;
}

static void clean_install(void)
{
    uint32_t i;
    bool stray = false;

    setup();
    CHECK(run() == 2);
    CHECK(what_boots() == BOOTS_NEW);
    CHECK(fake_flash[OD_TLSR_STAGE_BASE + OD_TLSR_HDR_FLAG_OFF] == 0x00u);
    /* the old image's tail past the new length is the old bytes: only whole sectors move */
    for (i = 0; i < FAKE_FLASH_SIZE; ++i) {
        if (i >= OD_TLSR_STAGE_BASE && i < OD_TLSR_STAGE_BASE + LEN) {
            continue;
        }
        if (i < ((LEN + OD_TLSR_SECTOR - 1u) & ~(OD_TLSR_SECTOR - 1u))) {
            continue;
        }
        if (i < sizeof OLD) {
            stray = stray || fake_flash[i] != OLD[i];
        } else {
            stray = stray || fake_flash[i] != 0xFFu;
        }
    }
    CHECK(!stray);
    printf("tlsr_install: clean run %lu erase/program operations\n", g_writes);
}

static void cut_everywhere(void)
{
    unsigned long total, k;
    unsigned new_ok = 0u, staged = 0u, bad = 0u;

    setup();
    (void)run();
    total = g_writes;
    for (k = 0u; k < total; ++k) {
        setup();
        g_budget = (long)k;
        CHECK(run() == 1);
        switch (what_boots()) {
        case BOOTS_NEW: new_ok++; break;
        case BOOTS_STAGED: staged++; break;
        default: bad++; printf("  cut at %lu: nothing safe boots\n", k); break;
        }
    }
    CHECK(bad == 0u);
    CHECK(staged > 0u && new_ok > 0u);
    printf("tlsr_install: power cut at each of %lu operations: %u new, %u staged fallback\n",
           total, new_ok, staged);
}

static void bad_programs(void)
{
    /* a page that takes on the second try: the install completes */
    setup();
    g_bad_page = 2u * OD_TLSR_SECTOR + 3u * OD_TLSR_PAGE;
    g_bad_left = 1u;
    CHECK(run() == 2);
    CHECK(what_boots() == BOOTS_NEW);
    CHECK(fake_flash[OD_TLSR_STAGE_BASE + OD_TLSR_HDR_FLAG_OFF] == 0x00u);

    /* sector 0's first page also retries */
    setup();
    g_bad_page = 0u;
    g_bad_left = 2u;
    CHECK(run() == 2);
    CHECK(what_boots() == BOOTS_NEW);

    /* a page that never takes: stop with sector 0 flagless and the staged copy bootable */
    setup();
    g_bad_page = 1u * OD_TLSR_SECTOR;
    g_bad_left = 1000u;
    CHECK(run() == 2);
    CHECK(fake_flash[OD_TLSR_HDR_FLAG_OFF] != OD_TLSR_HDR_FLAG);
    CHECK(what_boots() == BOOTS_STAGED);

    /* the same in sector 0 */
    setup();
    g_bad_page = 0u;
    g_bad_left = 1000u;
    CHECK(run() == 2);
    CHECK(fake_flash[OD_TLSR_HDR_FLAG_OFF] != OD_TLSR_HDR_FLAG);
    CHECK(what_boots() == BOOTS_STAGED);
}

int main(void)
{
    make(OLD, sizeof OLD, 1);
    make(NEW, LEN, 2);
    clean_install();
    cut_everywhere();
    bad_programs();
    printf("tlsr_install: %u checks, %u failures\n", g_checks, g_failures);
    return g_failures == 0u ? 0 : 1;
}
