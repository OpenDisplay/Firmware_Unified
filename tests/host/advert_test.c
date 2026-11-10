/* advert_test.c -- host tests for the shared 16-byte MSD encoder.
 *
 * This is a PROMOTION, not a new feature: three targets already broadcast these bytes, so the
 * test that matters is not "does the encoder look right" but "does it agree with what ships".
 * The core of this file is therefore DIFFERENTIAL -- the shipped algorithms are transcribed
 * independently below and swept against the shared encoder across the whole input space that
 * reaches the wire. A vector table written from the implementation under test would only prove
 * the code does what it does; reproducing the shipped algorithm from the other side catches a
 * promotion that quietly changed a byte.
 *
 * THE FIRMWARE REPO IS THE AUTHORITY. Where the source repos disagree, `Firmware` (imported as
 * targets/esp32-idf) is correct and `Firmware_NRF54` (imported as targets/nordic-zephyr) is the
 * re-derived copy. So shipped_msd_firmware() is the reference this promotion must match, and
 * shipped_msd_nrf54() is here to record whether the copy agreed. On this subsystem it does, at
 * every input swept -- which is a finding, not an assumption: it means the promotion did not
 * have to choose between them, and if a later edit makes them diverge the third assertion in
 * the sweep fails loudly rather than the two silently drifting.
 *
 * The sweep deliberately excludes temperatures outside int16_t range. That is not a gap being
 * papered over: the shipped copies cast before clamping, which is undefined there, so a
 * differential comparison against them would be comparing against UB. Those inputs are
 * covered separately by direct assertions on the defined behaviour this module adds.
 */
#include "od_advert.h"

#include <stdio.h>
#include <string.h>

static unsigned g_checks;
static unsigned g_failures;
static const char *g_case = "(none)";

#define CHECK(cond)                                                             \
    do {                                                                        \
        ++g_checks;                                                             \
        if (!(cond)) {                                                          \
            ++g_failures;                                                       \
            printf("FAIL %s:%d [%s] %s\n", __FILE__, __LINE__, g_case, #cond);  \
        }                                                                       \
    } while (0)

#define CASE(name) (g_case = (name))

/* ----------------------------------------------------- the shipped implementations, retyped --- */

/* THE REFERENCE: the Firmware-repo algorithm, as targets/esp32-idf shipped it before that target
 * called od_advert. It assembles the canonical packed struct and memcpys it, which is
 * why this transcription is host-endian-dependent where the shared encoder is not -- the host
 * is little-endian, so the two agree here, and od_advert.c is the one that stays correct on a
 * big-endian toolchain.
 *
 * Kept in the shipped shape, including the cast-then-clamp order, so the comparison is against
 * what devices broadcast today rather than a tidied version. Callers must keep the temperature
 * inside int16_t range; see the file comment. */
static void shipped_msd_firmware(uint8_t out[16], const uint8_t dynamic[11],
                                 float chip_temperature_c, uint16_t battery_10mv,
                                 int reboot_flag, int connection_requested, uint8_t loop_counter)
{
    int16_t temp_encoded = (int16_t)((chip_temperature_c + 40.0f) * 2.0f);
    struct MsdAdvertisement m;
    uint8_t status_byte;

    if (temp_encoded < 0) {
        temp_encoded = 0;
    } else if (temp_encoded > 255) {
        temp_encoded = 255;
    }
    if (battery_10mv > 511u) {
        battery_10mv = 511u;
    }

    status_byte = (uint8_t)(((battery_10mv >> 8) & 0x01u ? OD_MSD_STATUS_BATTERY_VOLTAGE_BIT8 : 0u) |
                            (reboot_flag ? OD_MSD_STATUS_REBOOT_FLAG : 0u) |
                            (connection_requested ? OD_MSD_STATUS_CONNECTION_REQUESTED : 0u) |
                            (uint8_t)((loop_counter << OD_MSD_STATUS_MAIN_LOOP_COUNTER_SHIFT)
                                      & OD_MSD_STATUS_MAIN_LOOP_COUNTER_MASK));

    memset(&m, 0, sizeof m);
    m.company_id = 0x2446;
    memcpy(m.dynamic, dynamic, sizeof m.dynamic);
    m.chip_temperature = (uint8_t)temp_encoded;
    m.battery_voltage_low = (uint8_t)(battery_10mv & 0xFFu);
    m.status = status_byte;
    memcpy(out, &m, sizeof m);
}

/* THE COPY: the encoder targets/nordic-zephyr and targets/efr32bg22-slc both shipped -- one
 * algorithm, written out twice, placing the bytes by hand rather than through the struct. Swept
 * for agreement with the reference above, not used as the standard.
 *
 * ALL THREE TARGETS NOW CALL od_advert, so these two functions are the only surviving statement
 * of what they used to broadcast. That is the point of keeping them: without them the sweep
 * below would compare the encoder against itself and prove nothing. Do not "update" either one
 * to match od_advert -- if they disagree, the sweep is doing its job. */
static void shipped_msd_nrf54(uint8_t out[16], const uint8_t dynamic[11], float chip_temperature_c,
                              uint16_t battery_10mv, int reboot_flag, int connection_requested,
                              uint8_t loop_counter)
{
    int16_t temp_encoded = (int16_t)((chip_temperature_c + 40.0f) * 2.0f);
    uint8_t temperature_byte;
    uint8_t status_byte;

    if (temp_encoded < 0) {
        temp_encoded = 0;
    } else if (temp_encoded > 255) {
        temp_encoded = 255;
    }
    temperature_byte = (uint8_t)temp_encoded;

    if (battery_10mv > 511u) {
        battery_10mv = 511u;
    }

    status_byte = (uint8_t)(((battery_10mv >> 8) & 0x01u) |
                            ((reboot_flag ? 1u : 0u) << 1) |
                            ((connection_requested ? 1u : 0u) << 2) |
                            ((loop_counter & 0x0Fu) << 4));

    memset(out, 0, 16);
    out[0] = (uint8_t)(0x2446u & 0xFFu);
    out[1] = (uint8_t)((0x2446u >> 8) & 0xFFu);
    memcpy(&out[2], dynamic, 11);
    out[13] = temperature_byte;
    out[14] = (uint8_t)(battery_10mv & 0xFFu);
    out[15] = status_byte;
}

/* ------------------------------------------------------------------------------- the sweep --- */

static void test_agrees_with_shipped(void)
{
    static const uint8_t dynamic[11] = {
        0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0A, 0x0B
    };
    /* -40 C and +87.5 C are the encoding's endpoints; the rest bracket them, cross zero, and
     * land on half-step boundaries where truncation decides the byte. */
    static const float temps[] = {
        -60.0f, -40.5f, -40.0f, -39.75f, -0.25f, 0.0f, 0.25f, 21.0f, 22.3f,
        36.6f, 87.0f, 87.5f, 87.75f, 120.0f
    };
    static const uint16_t batteries[] = { 0u, 1u, 100u, 255u, 256u, 300u, 420u, 510u, 511u,
                                          512u, 1000u, 65535u };
    size_t ti, bi;
    unsigned r, c, k;

    CASE("agrees with the shipped encoder");
    for (ti = 0; ti < sizeof(temps) / sizeof(temps[0]); ++ti) {
        for (bi = 0; bi < sizeof(batteries) / sizeof(batteries[0]); ++bi) {
            for (r = 0; r < 2u; ++r) {
                for (c = 0; c < 2u; ++c) {
                    for (k = 0; k < 16u; ++k) {
                        struct od_advert_inputs in;
                        uint8_t mine[16];
                        uint8_t firmware[16];
                        uint8_t nrf54[16];

                        memset(&in, 0, sizeof in);
                        in.dynamic = dynamic;
                        in.chip_temperature_c = temps[ti];
                        in.battery_10mv = batteries[bi];
                        in.reboot_flag = (r != 0u);
                        in.connection_requested = (c != 0u);
                        in.loop_counter = (uint8_t)k;

                        od_advert_build(&in, mine);
                        shipped_msd_firmware(firmware, dynamic, temps[ti], batteries[bi],
                                             (int)r, (int)c, (uint8_t)k);
                        shipped_msd_nrf54(nrf54, dynamic, temps[ti], batteries[bi],
                                          (int)r, (int)c, (uint8_t)k);

                        /* Against the authority first: this is the assertion that says the
                         * promotion is correct. */
                        CHECK(memcmp(mine, firmware, 16) == 0);
                        /* Then the copy, so a future divergence between the two source repos
                         * surfaces here instead of on a device. */
                        CHECK(memcmp(firmware, nrf54, 16) == 0);
                    }
                }
            }
        }
    }
}

/* ------------------------------------------------------------------------ layout and fields --- */

static void test_layout(void)
{
    static const uint8_t dynamic[11] = {
        0xF0, 0xF1, 0xF2, 0xF3, 0xF4, 0xF5, 0xF6, 0xF7, 0xF8, 0xF9, 0xFA
    };
    struct od_advert_inputs in;
    struct MsdAdvertisement wire;
    uint8_t out[16];

    CASE("company id is little-endian");
    memset(&in, 0, sizeof in);
    in.dynamic = dynamic;
    in.chip_temperature_c = 22.0f;     /* (22 + 40) * 2 == 124 */
    in.battery_10mv = 420u;            /* 4.20 V: low 0xA4, bit8 set */
    in.reboot_flag = true;
    in.loop_counter = 5u;
    od_advert_build(&in, out);

    CHECK(out[0] == 0x46u);
    CHECK(out[1] == 0x24u);

    CASE("fields land where the canonical struct says");
    memcpy(&wire, out, sizeof wire);
    CHECK(wire.company_id == OD_ADVERT_COMPANY_ID);   /* host is LE; the bytes above are the contract */
    CHECK(memcmp(wire.dynamic, dynamic, sizeof wire.dynamic) == 0);
    CHECK(wire.chip_temperature == 124u);
    CHECK(wire.battery_voltage_low == 0xA4u);
    CHECK((wire.status & OD_MSD_STATUS_BATTERY_VOLTAGE_BIT8) != 0u);
    CHECK((wire.status & OD_MSD_STATUS_REBOOT_FLAG) != 0u);
    CHECK((wire.status & OD_MSD_STATUS_CONNECTION_REQUESTED) == 0u);
    CHECK((wire.status & OD_MSD_STATUS_MAIN_LOOP_COUNTER_MASK) == 0x50u);

    CASE("bit 3 follows encryption_enabled and nothing else");
    {
        unsigned k;

        for (k = 0; k < 16u; ++k) {
            in.loop_counter = (uint8_t)k;
            in.reboot_flag = ((k & 1u) != 0u);
            in.connection_requested = ((k & 2u) != 0u);
            od_advert_build(&in, out);
            CHECK((out[15] & OD_MSD_STATUS_ENCRYPTION_ENABLED) == 0u);
        }
        in.encryption_enabled = true;
        od_advert_build(&in, out);
        CHECK((out[15] & OD_MSD_STATUS_ENCRYPTION_ENABLED) != 0u);
        in.encryption_enabled = false;
    }

    CASE("writes exactly 16 bytes");
    {
        uint8_t guarded[24];

        memset(guarded, 0xCC, sizeof guarded);
        od_advert_build(&in, &guarded[4]);
        CHECK(guarded[3] == 0xCCu);
        CHECK(guarded[20] == 0xCCu);
    }

    CASE("a NULL dynamic block broadcasts zeros, not stale bytes");
    in.dynamic = NULL;
    memset(out, 0xFF, sizeof out);
    od_advert_build(&in, out);
    CHECK(out[0] == 0x46u);
    {
        unsigned i;

        for (i = 2u; i < 13u; ++i) {
            CHECK(out[i] == 0x00u);
        }
    }

    CASE("a NULL input zeroes rather than leaving the last advertisement");
    memset(out, 0xFF, sizeof out);
    od_advert_build(NULL, out);
    {
        unsigned i;

        for (i = 0; i < 16u; ++i) {
            CHECK(out[i] == 0x00u);
        }
    }

    CASE("a NULL output is ignored");
    od_advert_build(&in, NULL);   /* must not crash */
    CHECK(1);
}

/* ------------------------------------------------------------------------------- encodings --- */

static void test_temperature(void)
{
    CASE("temperature endpoints");
    CHECK(od_advert_encode_temperature(-40.0f) == 0u);
    CHECK(od_advert_encode_temperature(0.0f) == 80u);
    CHECK(od_advert_encode_temperature(22.0f) == 124u);
    CHECK(od_advert_encode_temperature(87.5f) == 255u);

    CASE("temperature clamps at both ends");
    CHECK(od_advert_encode_temperature(-41.0f) == 0u);
    CHECK(od_advert_encode_temperature(-1000.0f) == 0u);
    CHECK(od_advert_encode_temperature(88.0f) == 255u);
    CHECK(od_advert_encode_temperature(1000.0f) == 255u);

    /* The defined behaviour this module adds: the shipped copies cast these into an int16_t
     * first, which is undefined, so there is nothing to compare against differentially. */
    CASE("out-of-int16 readings are defined, not undefined");
    CHECK(od_advert_encode_temperature(1.0e9f) == 255u);
    CHECK(od_advert_encode_temperature(-1.0e9f) == 0u);

    CASE("NaN reports the bottom of the range");
    {
        volatile float zero = 0.0f;
        float nan_value = zero / zero;

        CHECK(od_advert_encode_temperature(nan_value) == 0u);
    }

    CASE("truncates within a half-degree step, as the shipped copies do");
    CHECK(od_advert_encode_temperature(-39.9f) == 0u);    /* 0.2 steps */
    CHECK(od_advert_encode_temperature(-39.4f) == 1u);    /* 1.2 steps */
}

static void test_battery(void)
{
    CASE("millivolts to the wire's 10 mV units");
    CHECK(od_advert_battery_10mv_from_mv(0u) == 0u);
    CHECK(od_advert_battery_10mv_from_mv(9u) == 0u);
    CHECK(od_advert_battery_10mv_from_mv(4200u) == 420u);
    CHECK(od_advert_battery_10mv_from_mv(5109u) == 510u);
    CHECK(od_advert_battery_10mv_from_mv(5110u) == 511u);

    /* 5.11 V is the top of the field. Above it the value must clamp, never wrap -- a wrapped
     * 512 would broadcast 0x00 with bit8 clear and report a flat battery on an over-voltage. */
    CASE("battery clamps rather than wrapping");
    CHECK(od_advert_battery_10mv_from_mv(5120u) == 511u);
    CHECK(od_advert_battery_10mv_from_mv(65535u) == 511u);

    CASE("the clamp also applies to a caller that already holds 10 mV units");
    {
        struct od_advert_inputs in;
        uint8_t out[16];

        memset(&in, 0, sizeof in);
        in.battery_10mv = 512u;
        od_advert_build(&in, out);
        CHECK(out[14] == 0xFFu);
        CHECK((out[15] & OD_MSD_STATUS_BATTERY_VOLTAGE_BIT8) != 0u);
    }

    CASE("the 9th battery bit is the status bit, not a truncation");
    {
        struct od_advert_inputs in;
        uint8_t out[16];

        memset(&in, 0, sizeof in);
        in.battery_10mv = 255u;
        od_advert_build(&in, out);
        CHECK(out[14] == 0xFFu);
        CHECK((out[15] & OD_MSD_STATUS_BATTERY_VOLTAGE_BIT8) == 0u);

        in.battery_10mv = 256u;
        od_advert_build(&in, out);
        CHECK(out[14] == 0x00u);
        CHECK((out[15] & OD_MSD_STATUS_BATTERY_VOLTAGE_BIT8) != 0u);
    }
}

static void test_counter(void)
{
    CASE("the liveness counter wraps within the nibble");
    CHECK(od_advert_advance_counter(0u) == 1u);
    CHECK(od_advert_advance_counter(14u) == 15u);
    CHECK(od_advert_advance_counter(15u) == 0u);

    /* Callers keep the counter in a uint8_t and some have carried values above 15 through it;
     * the mask is what stops those reaching the flag bits. */
    CASE("a counter above the nibble cannot reach the flag bits");
    {
        struct od_advert_inputs in;
        uint8_t out[16];

        memset(&in, 0, sizeof in);
        in.loop_counter = 0xFFu;
        od_advert_build(&in, out);
        CHECK(out[15] == 0xF0u);
        CHECK(od_advert_advance_counter(0xFFu) == 0u);
    }
}

int main(void)
{
    test_agrees_with_shipped();
    test_layout();
    test_temperature();
    test_battery();
    test_counter();

    printf("advert: %u checks, %u failures\n", g_checks, g_failures);
    return (g_failures == 0u) ? 0 : 1;
}
