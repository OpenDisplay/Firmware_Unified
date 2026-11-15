/* tlsr_store_test.c -- the Telink two-bank store (targets/telink-tlsr/src/od_tlsr_store.c)
 * through od_hal_nvs and od_hal_secret, over a simulated NOR flash that loses power part-way
 * through any chosen erase or program.
 *
 * The property under test is od_hal_nvs_write's contract: after a power cut at ANY point, what
 * is readable is the complete previous record or the complete new one, never a mix and never an
 * OLDER record than the previous one. Every operation index of a write, an erase and a purge is
 * cut in turn and the store is re-opened from the medium afterwards. */
#include "../../targets/telink-tlsr/src/od_tlsr_store.c"

#include "fake_tlsr_flash.h"

#include <stdio.h>
#include <stdlib.h>

static unsigned g_checks, g_failures;
#define CHECK(c)                                                                \
    do {                                                                        \
        ++g_checks;                                                             \
        if (!(c)) {                                                             \
            ++g_failures;                                                       \
            printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #c);                  \
        }                                                                       \
    } while (0)

/* A reboot: RAM state is gone, the medium remains. */
static void reboot(void)
{
    s_cfg.valid = false;
    s_sec.valid = false;
}

static void fill(uint8_t *b, uint32_t n, unsigned seed)
{
    uint32_t i, x = 0x9E3779B9u * (seed + 1u);
    for (i = 0; i < n; ++i) {
        x ^= x << 13;
        x ^= x >> 17;
        x ^= x << 5;
        b[i] = (uint8_t)(x >> 24);
    }
}

/* 1 = holds exactly `want`, 0 = holds something else, -1 = nothing stored */
static int nvs_holds(const uint8_t *want, uint32_t n)
{
    static uint8_t got[OD_TLSR_CFG_BANK_SIZE];
    uint32_t len = 0u;
    int rc = od_hal_nvs_size(&len);

    if (rc == OD_HAL_NVS_ENOENT) {
        return -1;
    }
    if (rc != OD_HAL_NVS_OK || len != n || od_hal_nvs_read(0u, got, len) != OD_HAL_NVS_OK) {
        return 0;
    }
    return memcmp(got, want, n) == 0 ? 1 : 0;
}

static uint8_t X[3000], A[4112], B[777], C[64];

static void basics(void)
{
    uint32_t len = 99u;
    uint8_t buf[8];

    fake_flash_reset();
    reboot();
    CHECK(od_hal_nvs_size(&len) == OD_HAL_NVS_ENOENT && len == 0u);
    CHECK(od_hal_nvs_read(0u, buf, 0u) == OD_HAL_NVS_ENOENT);
    CHECK(od_hal_nvs_erase() == OD_HAL_NVS_OK);
    CHECK(od_hal_nvs_write(A, sizeof A) == OD_HAL_NVS_OK);
    CHECK(nvs_holds(A, sizeof A) == 1);
    reboot();
    CHECK(nvs_holds(A, sizeof A) == 1);
    CHECK(od_hal_nvs_read(sizeof A - 4u, buf, 4u) == OD_HAL_NVS_OK && memcmp(buf, &A[sizeof A - 4u], 4) == 0);
    CHECK(od_hal_nvs_read(sizeof A - 4u, buf, 5u) == OD_HAL_NVS_E2BIG);
    CHECK(od_hal_nvs_read(sizeof A + 1u, buf, 0u) == OD_HAL_NVS_E2BIG);
    CHECK(od_hal_nvs_write(B, sizeof B) == OD_HAL_NVS_OK);
    CHECK(nvs_holds(B, sizeof B) == 1);
    CHECK(od_hal_nvs_erase() == OD_HAL_NVS_OK);
    CHECK(nvs_holds(B, sizeof B) == -1);
    reboot();
    CHECK(nvs_holds(B, sizeof B) == -1);
    CHECK(od_hal_nvs_write(C, sizeof C) == OD_HAL_NVS_OK);
    reboot();
    CHECK(nvs_holds(C, sizeof C) == 1);
    /* too large for a bank */
    {
        static uint8_t big[OD_TLSR_CFG_BANK_SIZE];
        CHECK(od_hal_nvs_write(big, sizeof big) == OD_HAL_NVS_EIO);
        CHECK(nvs_holds(C, sizeof C) == 1);
    }
    /* nothing outside the store's banks was written */
    {
        uint32_t i, stray = 0u;
        for (i = 0u; i < FAKE_FLASH_SIZE; ++i) {
            if ((i < OD_TLSR_CFG_BANK_A || i >= OD_TLSR_CFG_BANK_B + OD_TLSR_CFG_BANK_SIZE) &&
                fake_flash[i] != 0xFFu) {
                stray++;
            }
        }
        CHECK(stray == 0u);
    }
}

static int purge_for_test(void)
{
    return od_tlsr_store_purge_all();
}

/* Store X then A (so the superseded bank holds X), then cut operation n of `op`. */
enum op { OP_WRITE, OP_ERASE, OP_PURGE };

static jmp_buf g_cut;

static long sweep(enum op op)
{
    long n, total = -1;

    for (n = 0; n < 1000; ++n) {
        volatile bool completed = false;
        int got;

        fake_flash_reset();
        reboot();
        CHECK(od_hal_nvs_write(X, sizeof X) == OD_HAL_NVS_OK);
        CHECK(od_hal_nvs_write(A, sizeof A) == OD_HAL_NVS_OK);
        reboot();
        if (setjmp(g_cut) == 0) {
            fake_flash_cut_after(n, &g_cut);
            if (op == OP_WRITE) {
                (void)od_hal_nvs_write(B, sizeof B);
            } else if (op == OP_ERASE) {
                (void)od_hal_nvs_erase();
            } else {
                (void)purge_for_test();
            }
            completed = true;
        }
        fake_flash_no_cut();
        reboot();
        got = nvs_holds(A, sizeof A);
        if (op == OP_WRITE) {
            int gb = nvs_holds(B, sizeof B);
            CHECK(got == 1 || gb == 1);
            if (completed) {
                CHECK(gb == 1);
            }
        } else if (op == OP_ERASE) {
            CHECK(got == 1 || got == -1);
            CHECK(nvs_holds(X, sizeof X) != 1);
            if (completed) {
                CHECK(got == -1);
            }
        } else {
            CHECK(got == 1);
        }
        /* the store is usable afterwards */
        CHECK(od_hal_nvs_write(C, sizeof C) == OD_HAL_NVS_OK);
        reboot();
        CHECK(nvs_holds(C, sizeof C) == 1);
        if (completed) {
            total = n;
            break;
        }
    }
    CHECK(total > 0);
    return total;
}

/* After a purge no superseded bytes survive anywhere in the store's banks. */
static void purge_removes_superseded(void)
{
    uint32_t i;
    bool found = false;

    fake_flash_reset();
    reboot();
    CHECK(od_hal_nvs_write(X, sizeof X) == OD_HAL_NVS_OK);
    CHECK(od_hal_nvs_write(A, sizeof A) == OD_HAL_NVS_OK);
    for (i = OD_TLSR_CFG_BANK_A; i + 64u <= OD_TLSR_CFG_BANK_B + OD_TLSR_CFG_BANK_SIZE; ++i) {
        if (memcmp(&fake_flash[i], &X[100], 64u) == 0) {
            found = true;
        }
    }
    CHECK(found); /* the precondition: an ordinary write leaves X readable */
    CHECK(od_tlsr_store_purge_all() == 0);
    found = false;
    for (i = OD_TLSR_CFG_BANK_A; i + 64u <= OD_TLSR_CFG_BANK_B + OD_TLSR_CFG_BANK_SIZE; ++i) {
        if (memcmp(&fake_flash[i], &X[100], 64u) == 0) {
            found = true;
        }
    }
    CHECK(!found);
    reboot();
    CHECK(nvs_holds(A, sizeof A) == 1);
}

static void secrets(void)
{
    uint8_t key[28], st[16], mk[12], got[28];
    long n;

    fill(key, sizeof key, 1);
    fill(st, sizeof st, 2);
    fill(mk, sizeof mk, 3);
    fake_flash_reset();
    reboot();
    CHECK(!od_hal_secret_read(OD_HAL_SECRET_KEY, got, sizeof key));
    CHECK(od_hal_secret_erase(OD_HAL_SECRET_MARKER));
    CHECK(od_hal_secret_write(OD_HAL_SECRET_KEY, key, sizeof key));
    CHECK(od_hal_secret_write(OD_HAL_SECRET_STATE, st, sizeof st));
    reboot();
    CHECK(od_hal_secret_read(OD_HAL_SECRET_KEY, got, sizeof key) && memcmp(got, key, sizeof key) == 0);
    CHECK(!od_hal_secret_read(OD_HAL_SECRET_KEY, got, 27u)); /* exact length only */
    CHECK(od_hal_secret_read(OD_HAL_SECRET_STATE, got, sizeof st) && memcmp(got, st, sizeof st) == 0);
    CHECK(!od_hal_secret_read(OD_HAL_SECRET_MARKER, got, sizeof mk));
    CHECK(!od_hal_secret_write((enum od_hal_secret_id)3, key, 4u));
    CHECK(!od_hal_secret_write(OD_HAL_SECRET_KEY, key, 33u));

    /* a cut while writing MARKER never disturbs KEY or STATE */
    for (n = 0; n < 32; ++n) {
        volatile bool completed = false;
        reboot();
        if (setjmp(g_cut) == 0) {
            fake_flash_cut_after(n, &g_cut);
            (void)od_hal_secret_write(OD_HAL_SECRET_MARKER, mk, sizeof mk);
            completed = true;
        }
        fake_flash_no_cut();
        reboot();
        CHECK(od_hal_secret_read(OD_HAL_SECRET_KEY, got, sizeof key) && memcmp(got, key, sizeof key) == 0);
        CHECK(od_hal_secret_read(OD_HAL_SECRET_STATE, got, sizeof st) && memcmp(got, st, sizeof st) == 0);
        if (completed) {
            CHECK(od_hal_secret_read(OD_HAL_SECRET_MARKER, got, sizeof mk) && memcmp(got, mk, sizeof mk) == 0);
            break;
        }
        (void)od_hal_secret_erase(OD_HAL_SECRET_MARKER);
    }
    CHECK(od_hal_secret_erase(OD_HAL_SECRET_KEY));
    reboot();
    CHECK(!od_hal_secret_read(OD_HAL_SECRET_KEY, got, sizeof key));
    CHECK(od_hal_secret_read(OD_HAL_SECRET_STATE, got, sizeof st));
}

/* M1: below the supply floor nothing is erased or programmed; the stored record stays. */
static void low_supply(void)
{
    uint8_t key[32];
    unsigned long ops;

    fake_flash_reset();
    reboot();
    CHECK(od_hal_nvs_write(B, sizeof B) == OD_HAL_NVS_OK);
    fill(key, sizeof key, 3);
    CHECK(od_hal_secret_write(OD_HAL_SECRET_KEY, key, sizeof key));
    fake_flash_supply_low = true;
    ops = fake_flash_ops;
    CHECK(od_hal_nvs_write(C, sizeof C) == OD_HAL_NVS_EIO);
    CHECK(od_hal_nvs_erase() == OD_HAL_NVS_EIO);
    CHECK(!od_hal_secret_erase(OD_HAL_SECRET_KEY));
    CHECK(od_tlsr_store_purge_all() != 0);
    CHECK(fake_flash_ops == ops);
    fake_flash_supply_low = false;
    reboot();
    CHECK(nvs_holds(B, sizeof B) == 1);
    CHECK(od_hal_secret_read(OD_HAL_SECRET_KEY, key, sizeof key));
}

int main(void)
{
    long w, e, p;

    fill(X, sizeof X, 9);
    fill(A, sizeof A, 10);
    fill(B, sizeof B, 11);
    fill(C, sizeof C, 12);
    basics();
    w = sweep(OP_WRITE);
    e = sweep(OP_ERASE);
    p = sweep(OP_PURGE);
    purge_removes_superseded();
    secrets();
    low_supply();
    printf("tlsr_store: power cut at each of %ld write, %ld erase, %ld purge operations\n", w + 1,
           e + 1, p + 1);
    printf("tlsr_store: %u checks, %u failures\n", g_checks, g_failures);
    return g_failures == 0u ? 0 : 1;
}
