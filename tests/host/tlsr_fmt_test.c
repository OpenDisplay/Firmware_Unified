/* tlsr_fmt_test.c -- targets/telink-tlsr/src/od_tlsr_fmt.c against the host libc.
 *
 * The Telink SDK has no snprintf, so the target carries its own, and it formats doubles with
 * integer arithmetic only (tc32's double helpers are unusable). CMake compiles that file with
 * snprintf/vsnprintf renamed to od_snprintf/od_vsnprintf so both can live in one binary; every
 * case here must match libc byte for byte, rounding (half to even) included. */

#include "od_check.h"

#include <stdarg.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int od_snprintf(char *out, size_t cap, const char *fmt, ...);

static void check_same_f(const char *fmt, double v)
{
    char want[64], got[64];
    snprintf(want, sizeof want, fmt, v);
    od_snprintf(got, sizeof got, fmt, v);
    if (strcmp(want, got) != 0) {
        printf("  %s %.17g: libc '%s' target '%s'\n", fmt, v, want, got);
    }
    CHECK(strcmp(want, got) == 0);
}

int main(void)
{
    static const char *const ffmt[] = { "%.0f", "%.1f", "%.2f", "%.3f", "%.2fV", "%.1fC" };
    char want[64], got[64];
    unsigned i;

    CASE("floats, including float -> double promotions and exact binary ties");
    srand(1);
    for (i = 0; i < 400000u; i++) {
        double v = (rand() / (double)RAND_MAX - 0.5) * (i % 3u == 0u ? 2.0 : i % 3u == 1u ? 200.0 : 2e6);
        if (i % 7u == 0u) v = (double)(float)v;
        check_same_f(ffmt[i % 6u], v);
    }
    check_same_f("%.2fV", 3.0);
    check_same_f("%.1fC", -40.0);
    check_same_f("%.2f", 0.005);

    CASE("the boot screen's integer and string formats");
    snprintf(want, sizeof want, "%06lX", 0x80A992ul); od_snprintf(got, sizeof got, "%06lX", 0x80A992ul);
    CHECK(strcmp(want, got) == 0);
    snprintf(want, sizeof want, "%06lX", 0xAul); od_snprintf(got, sizeof got, "%06lX", 0xAul);
    CHECK(strcmp(want, got) == 0);
    snprintf(want, sizeof want, "%ux%u", 152u, 296u); od_snprintf(got, sizeof got, "%ux%u", 152u, 296u);
    CHECK(strcmp(want, got) == 0);
    snprintf(want, sizeof want, "FW:   OD ver %u.%u.%u", 0u, 1u, 0u);
    od_snprintf(got, sizeof got, "FW:   OD ver %u.%u.%u", 0u, 1u, 0u);
    CHECK(strcmp(want, got) == 0);
    snprintf(want, sizeof want, "%s %s|%s%s", "Key", "abc", "x", "y");
    od_snprintf(got, sizeof got, "%s %s|%s%s", "Key", "abc", "x", "y");
    CHECK(strcmp(want, got) == 0);

    CASE("truncation reports the untruncated length and always terminates");
    CHECK(od_snprintf(got, 5, "%s", "truncate-me") == 11);
    CHECK(strcmp(got, "trun") == 0);
    CHECK(od_snprintf(NULL, 0, "%u", 12345u) == 5);

    CASE("out of range asks print ? rather than garbage");
    od_snprintf(got, sizeof got, "%.4f", 1.0);
    CHECK(strcmp(got, "?") == 0);
    od_snprintf(got, sizeof got, "%.1f", 1e12);
    CHECK(strcmp(got, "?") == 0);

    return OD_CHECK_REPORT_NONEMPTY("tlsr_fmt", 400000u);
}
