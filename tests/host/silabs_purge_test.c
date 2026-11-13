/* targets/efr32bg22-slc/od_nvm3_purge.c over a ring-model NVM3.
 *
 * The model is the BG22 layout as the SDK sees it: three pages, and nvm3_repackNeeded() true
 * on every call with a ~2 KB config live (soft threshold 22,632 B > 24,516 B data area minus the
 * live set), while nvm3_repack() returns SL_STATUS_OK and erases the bottom page each time.
 * Appends repack on their own when the page fills (repackUntilGood), which is what turns the
 * ring. A purge that loops on repackNeeded()/repack() therefore never returns on hardware; here
 * the repack fake gives up after a bound so that defect shows as a failed check, not a hang. */

#include "od_claim_app.h"

#include "nvm3_default.h"
#include "sl_status.h"

#include <stdio.h>

#define PAGES            3u
#define PAGE_DATA        8172u   /* 8 KB page minus the 20-byte header */
#define APPEND_BYTES     72u     /* 64-byte purge object + large object header */
#define REPACK_RUNAWAY   10000u

static nvm3_Handle_t s_h;
nvm3_Handle_t *nvm3_defaultHandle = &s_h;

static bool s_mountable;
static size_t s_page_fill;
static unsigned s_writes, s_deletes, s_repacks, s_erases[PAGES];
static unsigned s_fail_write_at;      /* 0 = never */
static unsigned s_checks, s_failures;

static void check(int condition, const char *what)
{
    ++s_checks;
    if (!condition) {
        ++s_failures;
        printf("FAIL: %s\n", what);
    }
}

static void erase_bottom(void)
{
    ++s_erases[s_h.fifoFirstIdx];
    s_h.fifoFirstIdx = (s_h.fifoFirstIdx + 1u) % PAGES;
}

static void reset(void)
{
    s_h.nvmSize = PAGES * 8192u;
    s_h.totalNvmPageCnt = PAGES;
    s_h.fifoFirstIdx = 1u;
    s_h.hasBeenOpened = true;
    s_mountable = true;
    s_page_fill = 2100u;   /* the live config */
    s_writes = s_deletes = s_repacks = 0u;
    for (unsigned p = 0u; p < PAGES; ++p) {
        s_erases[p] = 0u;
    }
    s_fail_write_at = 0u;
}

sl_status_t nvm3_initDefault(void)
{
    if (!s_mountable) {
        return SL_STATUS_FAIL;
    }
    s_h.hasBeenOpened = true;
    return SL_STATUS_OK;
}

sl_status_t nvm3_writeData(nvm3_Handle_t *h, nvm3_ObjectKey_t key, const void *value, size_t len)
{
    (void)key;
    (void)value;
    (void)len;
    if (h != &s_h || !h->hasBeenOpened) {
        return SL_STATUS_NOT_INITIALIZED;
    }
    ++s_writes;
    if (s_fail_write_at != 0u && s_writes >= s_fail_write_at) {
        return SL_STATUS_FAIL;
    }
    s_page_fill += APPEND_BYTES;
    if (s_page_fill >= PAGE_DATA) {   /* repackUntilGood ahead of the append */
        erase_bottom();
        s_page_fill -= PAGE_DATA;
    }
    return SL_STATUS_OK;
}

bool nvm3_repackNeeded(nvm3_Handle_t *h)
{
    (void)h;
    return true;
}

sl_status_t nvm3_repack(nvm3_Handle_t *h)
{
    (void)h;
    if (++s_repacks > REPACK_RUNAWAY) {
        return SL_STATUS_FAIL;
    }
    erase_bottom();
    return SL_STATUS_OK;
}

sl_status_t nvm3_deleteObject(nvm3_Handle_t *h, nvm3_ObjectKey_t key)
{
    (void)h;
    (void)key;
    ++s_deletes;
    return SL_STATUS_OK;
}

/* Unused by the purge; the shared header declares them. */
sl_status_t nvm3_readPartialData(nvm3_Handle_t *h, nvm3_ObjectKey_t key, void *value,
                                 size_t offset, size_t len)
{
    (void)h; (void)key; (void)value; (void)offset; (void)len;
    return SL_STATUS_FAIL;
}

sl_status_t nvm3_getObjectInfo(nvm3_Handle_t *h, nvm3_ObjectKey_t key, uint32_t *type, size_t *len)
{
    (void)h; (void)key; (void)type; (void)len;
    return SL_STATUS_FAIL;
}

static void test_purge_ends_with_repack_always_needed(void)
{
    reset();
    check(od_claim_app_storage_purge(), "purge completes on the BG22 layout");
    check(s_repacks == 0u, "purge never calls nvm3_repack (writes repack on their own)");
    for (unsigned p = 0u; p < PAGES; ++p) {
        check(s_erases[p] == 1u, "every page erased exactly once");
    }
    check(s_writes <= (2u * PAGES * 8192u) / 64u + 32u, "appends stay within the bound");
    check(s_deletes == 1u, "the throw-away object is deleted");
}

static void test_write_failure_reports_not_done(void)
{
    reset();
    s_fail_write_at = 5u;
    check(!od_claim_app_storage_purge(), "a failed append is not reported as a purge");
    check(s_writes == 5u, "the purge stops at the failed append");
    check(s_deletes == 1u, "the throw-away object is still deleted");
}

static void test_unmountable_store_reports_not_done(void)
{
    reset();
    s_h.hasBeenOpened = false;
    s_mountable = false;
    check(!od_claim_app_storage_purge(), "an unopenable store is not purged");
    check(s_writes == 0u, "nothing written to an unopenable store");
}

int main(void)
{
    test_purge_ends_with_repack_always_needed();
    test_write_failure_reports_not_done();
    test_unmountable_store_reports_not_done();

    printf("silabs purge: %u checks, %u failures\n", s_checks, s_failures);
    return s_failures == 0u ? 0 : 1;
}
