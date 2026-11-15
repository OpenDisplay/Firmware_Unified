/* esp32_it8951_test -- targets/esp32-idf/src/display_it8951.cpp on host fakes (fake_esp32/).
 *
 * The driver keeps a one-row DMA buffer and, in 1-bit mode, a PSRAM framebuffer. A config write
 * reloads the panel geometry with no reboot (od_it8951_set_panel from fastepd_prepare_hardware),
 * so these cases change it between transfers and check that no write lands past an allocation
 * (the fake heap keeps guard bytes after every block). Also: a failed init must not leave the
 * board supply (EN) and controller supply (ITE_EN) switched on. */
#include "display_it8951.h"
#include "fake_esp32.h"

#include <stdio.h>
#include <string.h>

static unsigned g_checks;
static unsigned g_failures;

#define CHECK(cond)                                                              \
    do {                                                                         \
        ++g_checks;                                                              \
        if (!(cond)) {                                                           \
            ++g_failures;                                                        \
            printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond);                \
        }                                                                        \
    } while (0)

enum { MOSI = 9, MISO = 8, SCLK = 7, CS = 10, BUSY = 13, RST = 12, EN = 11, ITE_EN = 21 };

static uint8_t g_data[8192];

static void panel(uint16_t w, uint16_t h, bool gray16)
{
    od_it8951_set_layout(true, 0u);
    od_it8951_set_panel(MOSI, MISO, SCLK, CS, BUSY, RST, EN, ITE_EN, w, h, gray16);
}

/* Init answers DEV_INFO with a plausible panel; afterwards every register reads 0 (LUT idle). */
static void begin(void)
{
    fake_esp32_rx_word = 0x0400u;
    od_it8951_epaper_begin();
    fake_esp32_rx_word = 0u;
}

static void power_cycle(void)
{
    od_it8951_direct_sleep();
    od_it8951_mark_hw_deinitialized();
}

static void test_mono_to_gray16(void)
{
    fake_esp32_reset();
    panel(1872u, 1404u, false);          /* row 234 B */
    begin();
    CHECK(!od_it8951_init_failed());

    panel(1872u, 1404u, true);           /* config write: GRAY16, row 936 B */
    od_it8951_direct_write_reset();
    memset(g_data, 0x5A, sizeof g_data);
    od_it8951_direct_write_chunk(g_data, 936u * 3u);
    CHECK(fake_esp32_guards_intact());
    od_it8951_direct_refresh(0);
    CHECK(fake_esp32_guards_intact());
    power_cycle();
}

static void test_mono_grows(void)
{
    fake_esp32_reset();
    panel(800u, 600u, false);            /* framebuffer 100 x 600 */
    begin();
    CHECK(!od_it8951_init_failed());

    panel(1872u, 1404u, false);          /* config write: a bigger 1-bit panel */
    od_it8951_direct_write_reset();
    od_it8951_direct_write_chunk(g_data, sizeof g_data);
    CHECK(fake_esp32_guards_intact());
    power_cycle();

    /* The same after a cold re-init. */
    panel(800u, 600u, false);
    begin();
    power_cycle();
    panel(1872u, 1404u, false);
    begin();
    od_it8951_direct_write_reset();
    od_it8951_direct_write_chunk(g_data, sizeof g_data);
    CHECK(fake_esp32_guards_intact());
    power_cycle();
}

static void test_boot_row_after_growth(void)
{
    static uint8_t row[936];

    fake_esp32_reset();
    panel(800u, 600u, false);
    begin();
    panel(1872u, 1404u, false);
    begin();
    memset(row, 0x00, sizeof row);
    for (uint16_t y = 0; y < 1404u; y++) {
        od_it8951_boot_write_row(y, row, 234u);
    }
    CHECK(fake_esp32_guards_intact());
    power_cycle();
}

static void test_failed_init_powers_down(void)
{
    fake_esp32_reset();
    panel(1872u, 1404u, false);
    fake_esp32_rx_word = 0u;             /* DEV_INFO reads 0: no controller answering */
    od_it8951_epaper_begin();
    CHECK(od_it8951_init_failed());
    CHECK(fake_esp32_pin_level(EN) == 0);
    CHECK(fake_esp32_pin_level(ITE_EN) == 0);
    CHECK(fake_esp32_pin_level(CS) == 1);

    /* A transfer on the failed controller: the bytes are refused, not swallowed. */
    od_it8951_direct_write_reset();
    memset(g_data, 0x5A, 64u);
    CHECK(!od_it8951_direct_write_chunk(g_data, 64u));
    od_it8951_partial_prepare(0u, 0u, 64u, 8u);
    CHECK(!od_it8951_partial_write_chunk(g_data, 64u));
    od_it8951_mark_hw_deinitialized();
}

/* A working mono frame takes exactly its size; one byte past it is refused. */
static void test_write_reports_overrun(void)
{
    fake_esp32_reset();
    panel(64u, 8u, false);               /* 8 B rows, 64 B frame */
    begin();
    od_it8951_direct_write_reset();
    memset(g_data, 0x5A, 65u);
    CHECK(od_it8951_direct_write_chunk(g_data, 64u));
    CHECK(!od_it8951_direct_write_chunk(g_data, 1u));
    CHECK(fake_esp32_guards_intact());
    power_cycle();
}

int main(void)
{
    test_mono_to_gray16();
    test_mono_grows();
    test_boot_row_after_growth();
    test_failed_init_powers_down();
    test_write_reports_overrun();
    printf("esp32_it8951: %u checks, %u failures\n", g_checks, g_failures);
    return g_failures == 0u ? 0 : 1;
}
