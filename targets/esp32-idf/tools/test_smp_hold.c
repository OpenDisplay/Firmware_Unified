/* Host test for src/od_smp_hold.h: when an image upload survives a dropped link. */
#include "od_smp_hold.h"

#include <stdio.h>

static int failures;

#define CHECK(c)                                                         \
    do {                                                                 \
        if (!(c)) {                                                      \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #c);          \
            failures++;                                                  \
        }                                                                \
    } while (0)

#define S(x) ((uint64_t)(x) * 1000000ull)

int main(void)
{
    od_smp_hold_t h = { false, 0u };

    /* No drop yet: an active upload is neither waiting nor expired. */
    CHECK(!od_smp_hold_waiting(&h, true, S(1000)));
    CHECK(!od_smp_hold_expired(&h, true, S(1000)));

    /* A drop keeps the upload and holds the device awake until the deadline. */
    od_smp_hold_lost(&h, S(10));
    CHECK(od_smp_hold_waiting(&h, true, S(10)));
    CHECK(od_smp_hold_waiting(&h, true, S(10) + OD_SMP_HOLD_US - 1u));
    CHECK(!od_smp_hold_expired(&h, true, S(10) + OD_SMP_HOLD_US - 1u));

    /* At the deadline it is aborted, not held. */
    CHECK(od_smp_hold_expired(&h, true, S(10) + OD_SMP_HOLD_US));
    CHECK(!od_smp_hold_waiting(&h, true, S(10) + OD_SMP_HOLD_US));

    /* No upload: nothing to hold or abort, whatever the link did. */
    CHECK(!od_smp_hold_waiting(&h, false, S(11)));
    CHECK(!od_smp_hold_expired(&h, false, S(10) + OD_SMP_HOLD_US));

    /* A frame from a new link cancels the hold; a timer firing later aborts nothing. */
    od_smp_hold_lost(&h, S(20));
    od_smp_hold_attach(&h);
    CHECK(!od_smp_hold_waiting(&h, true, S(21)));
    CHECK(!od_smp_hold_expired(&h, true, S(20) + OD_SMP_HOLD_US));

    /* A second drop restarts the hold from that drop, not the first one. */
    od_smp_hold_lost(&h, S(30));
    od_smp_hold_attach(&h);
    od_smp_hold_lost(&h, S(100));
    CHECK(!od_smp_hold_expired(&h, true, S(30) + OD_SMP_HOLD_US));
    CHECK(od_smp_hold_waiting(&h, true, S(30) + OD_SMP_HOLD_US));
    CHECK(od_smp_hold_expired(&h, true, S(100) + OD_SMP_HOLD_US));

    /* The hold is the issue's 120 s unless a build overrides it. */
    _Static_assert(OD_SMP_HOLD_US == 120ull * 1000000ull, "hold is 120 s");

    if (failures != 0) {
        printf("FAIL od_smp_hold: %d failures\n", failures);
        return 1;
    }
    printf("PASS od_smp_hold\n");
    return 0;
}
