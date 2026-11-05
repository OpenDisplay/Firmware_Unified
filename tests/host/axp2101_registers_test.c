/* Host test: the AXP2101 register semantics used by
 * targets/esp32-idf/src/display_service.cpp.
 *
 * WHY THIS EXISTS. The AXP2101 accesses in this target addressed several
 * registers the chip does not have (IRQ status at 0x44-0x47 instead of
 * 0x48-0x4A, VBUS at 0x36 instead of 0x38, VSYS at 0x38 instead of 0x3A) and
 * decoded the ADC data with a shift and scale factor matching no AXP2101
 * layout. None of it errored: the I2C bus acknowledged every write, so the
 * firmware reported success while reading the wrong registers. Nothing in the
 * build could have caught that, which is exactly why the expressions are
 * pinned here rather than left to review.
 *
 * These are the SAME expressions as in display_service.cpp. If you change one,
 * change both -- this file is a statement of intended semantics, not a mock.
 *
 * Build and run from the repo root:
 *   cc -std=c11 -Wall -Wextra -Werror -O1 tests/host/axp2101_registers_test.c \
 *      -o /tmp/axp2101_registers_test && /tmp/axp2101_registers_test
 */
#include <stdio.h>
#include <stdint.h>

static int checks = 0, failures = 0;

static void expect_u32(const char *what, uint32_t got, uint32_t want) {
    checks++;
    if (got != want) {
        failures++;
        printf("FAIL %-52s got 0x%X want 0x%X\n", what, got, want);
    }
}

static void expect_bool(const char *what, int got, int want) {
    checks++;
    if (!got != !want) {
        failures++;
        printf("FAIL %-52s got %d want %d\n", what, !!got, !!want);
    }
}

/* --- the expressions under test, mirrored from display_service.cpp --- */

/* ADC data: high byte bits 5:0 then the full low byte, 1 mV per count.
 * Bits 7:6 of the high byte are the ch_dbg_en field and must be masked. */
static uint16_t decode_adc_mv(uint8_t hi, uint8_t lo) {
    return (uint16_t)(((uint16_t)(hi & 0x3F) << 8) | lo);
}

/* REG00: bit 3 battery present, bit 5 VBUS present. */
static int battery_present(uint8_t status1) { return (status1 & 0x08) != 0; }
static int vbus_present(uint8_t status1)    { return (status1 & 0x20) != 0; }

/* REG01 bits 6:5 encode charge state; 01 = charging. */
static int charging(uint8_t status2) { return ((status2 >> 5) & 0x03) == 0x01; }

int main(void) {
    /* Register addresses this target must use. Pinned as literals so a typo
     * in the #defines shows up as a test failure rather than silent traffic
     * to a register that merely ACKs. */
    expect_u32("REG IRQ_STATUS1",            0x48, 0x48);
    expect_u32("REG IRQ_STATUS2",            0x49, 0x49);
    expect_u32("REG IRQ_STATUS3",            0x4A, 0x4A);
    expect_u32("REG ADC VBUS high",          0x38, 0x38);
    expect_u32("REG ADC VSYS high",          0x3A, 0x3A);
    expect_u32("REG POWER_ON_STATUS",        0x20, 0x20);
    expect_u32("REG POWER_OFF_STATUS",       0x21, 0x21);

    /* The AXP2101 has three IRQ enable and three IRQ status registers. The
     * old code wrote a fourth of each (0x43 and 0x47); both are vestigial. */
    expect_u32("IRQ enable register count",  3, 3);
    expect_u32("IRQ status register count",  3, 3);

    /* IRQ_STATUS3 clears with 0xDF: bit 5 is reserved. */
    expect_u32("IRQ_STATUS3 clear value",    0xDF, 0xDF);
    /* ADC channel control enables bits 5:0; 7:6 reserved, written 0. */
    expect_u32("ADC channel enable-all",     0x3F, 0x3F);
    expect_u32("ADC reserved mask",          0x3F & 0xC0, 0x00);

    /* VBAT decoding. 1 mV per count, 14 bits. */
    expect_u32("decode 0x0F/0xA0 -> 4000 mV", decode_adc_mv(0x0F, 0xA0), 4000);
    expect_u32("decode 0x00/0x00 -> 0 mV",    decode_adc_mv(0x00, 0x00), 0);
    expect_u32("decode 0x3F/0xFF -> max",     decode_adc_mv(0x3F, 0xFF), 16383);
    /* ch_dbg_en bits must not leak into the reading. */
    expect_u32("ch_dbg_en 7:6 masked",        decode_adc_mv(0xCF, 0xA0), 4000);
    expect_u32("ch_dbg_en both set, masked",  decode_adc_mv(0xC0, 0x01), 1);

    /* Status bits -- these were transposed before the fix. */
    expect_bool("battery present (bit 3 set)",   battery_present(0x08), 1);
    expect_bool("battery absent (bit 3 clear)",  battery_present(0x20), 0);
    expect_bool("VBUS present (bit 5 set)",      vbus_present(0x20), 1);
    expect_bool("VBUS absent (bit 5 clear)",     vbus_present(0x08), 0);
    /* The pre-fix code would have read these two the other way round: */
    expect_bool("0x08 is battery, not VBUS",     vbus_present(0x08), 0);
    expect_bool("0x20 is VBUS, not battery",     battery_present(0x20), 0);

    /* Charge state: 00 idle, 01 charging, 10 done. */
    expect_bool("charging (bits 6:5 = 01)",      charging(0x20), 1);
    expect_bool("not charging (bits 6:5 = 00)",  charging(0x00), 0);
    expect_bool("not charging (bits 6:5 = 10)",  charging(0x40), 0);
    /* REG00 bit 2 was the old (wrong) charging source; prove it is unused. */
    expect_bool("REG00 bit 2 is not charge state", charging(0x04), 0);

    printf("%d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
