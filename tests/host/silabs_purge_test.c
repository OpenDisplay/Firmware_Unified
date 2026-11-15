/* targets/efr32bg22-slc/od_nvm3_purge.c over a ring-model NVM3.
 *
 * The model is the BG22 layout as the SDK sees it: three pages, and nvm3_repackNeeded() true
 * on every call with a ~2 KB config live (soft threshold 22,632 B > 24,516 B data area minus the
 * live set), while nvm3_repack() returns SL_STATUS_OK and erases the bottom page each time.
 * Appends repack on their own when the page fills (repackUntilGood), which is what turns the
 * ring. A purge that loops on repackNeeded()/repack() therefore never returns on hardware; here
 * the repack fake gives up after a bound so that defect shows as a failed check, not a hang.
 *
 * Above 5,028 B live (24,516 B data area minus thrHard(3136) = 19,488 B) repackUntilGood() never
 * reaches its threshold and runs all 2 x page-count steps, so one append erases several pages.
 * s_erase_per_write models that: with a multiple of the page count, fifoFirstIdx comes back to
 * where it was and only the bottom page's erase count shows that the ring turned. */

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
static unsigned s_erase_per_write;    /* 0 = only when the page fills (live data < 5,028 B) */
static bool s_ring_stuck;             /* appends succeed but no page is ever erased */
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
    s_erase_per_write = 0u;
    s_ring_stuck = false;
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
    if (s_ring_stuck) {
        return SL_STATUS_OK;
    }
    if (s_erase_per_write != 0u) {    /* repackUntilGood's full 2 x page-count steps */
        for (unsigned e = 0u; e < s_erase_per_write; ++e) {
            erase_bottom();
        }
        return SL_STATUS_OK;
    }
    s_page_fill += APPEND_BYTES;
    if (s_page_fill >= PAGE_DATA) {   /* repackUntilGood ahead of the append */
        erase_bottom();
        s_page_fill -= PAGE_DATA;
    }
    return SL_STATUS_OK;
}

/* The SDK reads the bottom page's header; a page's count rises by one per erase. */
sl_status_t nvm3_getEraseCount(nvm3_Handle_t *h, uint32_t *eraseCnt)
{
    if (h != &s_h || !h->hasBeenOpened) {
        return SL_STATUS_NOT_INITIALIZED;
    }
    *eraseCnt = 100u + s_erases[h->fifoFirstIdx];
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
    check(s_writes <= (5u * PAGES * 8192u) / (4u * 64u) + 32u, "appends stay within the bound");
    check(s_deletes == 1u, "the throw-away object is deleted");
}

/* A whole number of turns per append leaves fifoFirstIdx where it was. Counting only the index
 * delta never sees progress, runs every append of the bound at several erases each, and then
 * reports not done, so PURGE stays set and the next boot does it all again. */
static void check_turn_per_append(unsigned erase_per_write, const char *what)
{
    reset();
    s_erase_per_write = erase_per_write;
    check(od_claim_app_storage_purge(), what);
    check(s_writes <= PAGES, "the turn is seen within one append per page");
    for (unsigned p = 0u; p < PAGES; ++p) {
        check(s_erases[p] >= 1u && s_erases[p] <= s_writes * erase_per_write / PAGES + 1u,
              "every page erased, and no more than those appends did");
    }
}

static void test_whole_turn_per_append_is_seen(void)
{
    check_turn_per_append(PAGES, "one turn per append (index unchanged) completes");
    check_turn_per_append(2u * PAGES, "two turns per append (repackUntilGood's 6 steps) complete");
    check_turn_per_append(PAGES + 1u, "a turn plus a page per append completes");
}

/* A ring that never turns although every append succeeds: bounded work, then reported done so
 * the caller clears PURGE instead of repeating the same churn on every boot. */
static void test_no_turn_gives_up_bounded(void)
{
    reset();
    s_ring_stuck = true;
    check(od_claim_app_storage_purge(), "no turn within the bound is a give-up, reported done");
    check(s_writes <= (5u * PAGES * 8192u) / (4u * 64u) + 32u, "the give-up is bounded");
    check(s_writes > 100u, "the give-up is not premature");
    check(s_deletes == 1u, "the throw-away object is deleted on give-up");
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
    test_whole_turn_per_append_is_seen();
    test_no_turn_gives_up_bounded();
    test_write_failure_reports_not_done();
    test_unmountable_store_reports_not_done();

    printf("silabs purge: %u checks, %u failures\n", s_checks, s_failures);
    return s_failures == 0u ? 0 : 1;
}
