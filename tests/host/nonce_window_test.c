/* nonce_window_test.c -- shared/core/od_nonce_window.h, the replay window od_sec2 uses for H->D
 * record counters. It is upstream Firmware's src/nonce_window.h ported unchanged; the brute-force
 * model below shares no code with the bitmap, so the sweep is differential. */

#include "od_nonce_window.h"

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

/* Brute-force oracle: a plain list of accepted counters, consulted linearly. Deliberately the
 * dumbest possible model -- the point is that it shares no code with the bitmap. */
#define MODEL_MAX 4096
static uint64_t g_model[MODEL_MAX];
static unsigned g_model_n;
static uint64_t g_model_last;

static void model_reset(void) { g_model_n = 0; g_model_last = 0; }
static bool model_seen(uint64_t c)
{
    unsigned i;
    for (i = 0; i < g_model_n; ++i) { if (g_model[i] == c) { return true; } }
    return false;
}
static bool model_check(uint64_t c)
{
    if (c > g_model_last) { return true; }                    /* forward: unbounded */
    if ((g_model_last - c) >= OD_NONCE_BACKWARD_BITS) { return false; }  /* too far behind */
    return !model_seen(c);
}
static void model_commit(uint64_t c)
{
    if (!model_seen(c) && g_model_n < MODEL_MAX) { g_model[g_model_n++] = c; }
    if (c > g_model_last) { g_model_last = c; }
}

static void test_replay_bitmap_sweep(void)
{
    uint64_t bm[OD_NONCE_BITMAP_WORDS];
    uint64_t last = 0;
    uint32_t rnd = 12345u;
    unsigned i;

    CASE("bitmap agrees with a brute-force seen-set model over 10000 frames");
    memset(bm, 0, sizeof bm);
    model_reset();
    for (i = 0; i < 10000u; ++i) {
        uint64_t c;
        bool want, got;
        rnd = rnd * 1103515245u + 12345u;
        /* mostly forward, with backfills and far-behind attempts mixed in */
        if ((rnd >> 16) % 4u == 0u && last > 300u) {
            c = last - (uint64_t)((rnd >> 8) % 400u);
        } else {
            c = last + (uint64_t)((rnd >> 8) % 3u);
        }
        want = model_check(c);
        got  = (od_nonce_check(bm, last, c) == NONCE_OK);
        CHECK(want == got);
        if (got) { od_nonce_commit(bm, &last, c); model_commit(c); }
        if (last != g_model_last) { CHECK(last == g_model_last); break; }
    }
}

static void test_replay_exact_counter(void)
{
    uint64_t bm[OD_NONCE_BITMAP_WORDS];
    uint64_t last = 0;

    CASE("a replay at exactly last_seen is refused (the diff==0 hole, closed upstream)");
    memset(bm, 0, sizeof bm);
    CHECK(od_nonce_check(bm, last, 0u) == NONCE_OK);
    od_nonce_commit(bm, &last, 0u);
    CHECK(od_nonce_check(bm, last, 0u) == NONCE_REPLAY);

    CHECK(od_nonce_check(bm, last, 5u) == NONCE_OK);
    od_nonce_commit(bm, &last, 5u);
    CHECK(last == 5u);
    CHECK(od_nonce_check(bm, last, 5u) == NONCE_REPLAY);
}

static void test_replay_counter_zero_backfill(void)
{
    uint64_t bm[OD_NONCE_BITMAP_WORDS];
    uint64_t last = 0;

    CASE("counter 0 arriving after 1 is accepted once (a clear bit, not a sentinel)");
    memset(bm, 0, sizeof bm);
    CHECK(od_nonce_check(bm, last, 1u) == NONCE_OK);
    od_nonce_commit(bm, &last, 1u);
    CHECK(od_nonce_check(bm, last, 0u) == NONCE_OK);     /* never seen */
    od_nonce_commit(bm, &last, 0u);
    CHECK(od_nonce_check(bm, last, 0u) == NONCE_REPLAY); /* now it has been */
    CHECK(last == 1u);                                   /* a backfill never moves last_seen */
}

static void test_replay_window_boundaries(void)
{
    uint64_t bm[OD_NONCE_BITMAP_WORDS];
    uint64_t last = 0;

    CASE("backward width: 255 behind is in, 256 behind is out; forward is unbounded");
    memset(bm, 0, sizeof bm);
    od_nonce_commit(bm, &last, 1000u);
    CHECK(od_nonce_check(bm, last, 1000u - 255u) == NONCE_OK);
    CHECK(od_nonce_check(bm, last, 1000u - 256u) == NONCE_OUT_OF_WINDOW);
    /* No forward cap at all: a huge jump is accepted, which is what stops a session stranding. */
    CHECK(od_nonce_check(bm, last, 1000u + 1000000u) == NONCE_OK);
    CHECK(od_nonce_check(bm, last, UINT64_MAX) == NONCE_OK);
}


int main(void)
{
    test_replay_bitmap_sweep();
    test_replay_exact_counter();
    test_replay_counter_zero_backfill();
    test_replay_window_boundaries();
    printf("nonce_window: %u checks, %u failures\n", g_checks, g_failures);
    return (g_failures == 0u) ? 0 : 1;
}
