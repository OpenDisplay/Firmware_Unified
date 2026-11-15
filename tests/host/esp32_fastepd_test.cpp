/* esp32_fastepd_test -- targets/esp32-idf/src/display_fastepd.cpp (parallel panels) on host
 * fakes: the real FastEPD header, fake library bodies (fake_fastepd/) and the fake heap
 * (fake_esp32/), which keeps guard bytes after every block.
 *
 * FastEPD allocates its framebuffer once, at the first initPanel(), for that panel's native
 * size, and cannot re-allocate it. A config write changes the 0xB3 parallel panel or the
 * DisplayConfig size with no reboot, so these cases change them while that buffer exists and
 * check that nothing is written past it, that the wrong panel is never refreshed, and that the
 * driver reports the failure. */
#include "display_fastepd.h"
#include "display_service.h"
#include "fake_esp32.h"
#include "fake_fastepd.h"
#include "od_controller_esp.h"
#include "od_sensor_sht40.h"
#include "structs.h"

#include <stdio.h>
#include <string.h>

struct od_config globalConfig;
static struct od_controller_desc g_desc;

const struct od_controller_desc *od_controller_esp(void) { return &g_desc; }
bool od_sensor_sht40_read_temp_c(const struct od_config *, int *) { return false; }
void initOrRestoreWireForOpenDisplay(void) {}
bool fastepd_driver_used(void) { return true; }

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

static uint8_t g_data[16384];

/* What a CONFIG_WRITE leaves behind: a new 0xB3 parallel panel and DisplayConfig. */
static void config(uint8_t parallel, uint16_t w, uint16_t h, bool gray16)
{
    memset(&g_desc, 0, sizeof g_desc);
    g_desc.cls = OD_CTRL_CLASS_PARALLEL;
    g_desc.parallel_panel = parallel;
    g_desc.default_temp_c = OD_CTRL_TEMP_DEFAULT;
    g_desc.miso_pin = g_desc.en_pin = g_desc.ite_en_pin = OD_CTRL_PIN_NONE;
    g_desc.from_packet = true;
    globalConfig.display_count = 1u;
    globalConfig.displays[0].pixel_width = w;
    globalConfig.displays[0].pixel_height = h;
    globalConfig.displays[0].color_scheme = (uint8_t)(gray16 ? (unsigned)OD_COLOR_SCHEME_GRAY16 : 0u);
}

/* One direct transfer of `bytes` and a full refresh, the way display_service drives it. */
static void upload(size_t bytes)
{
    fastepd_direct_write_reset();
    memset(g_data, 0x5A, sizeof g_data);
    for (size_t done = 0; done < bytes;) {
        size_t n = bytes - done < sizeof g_data ? bytes - done : sizeof g_data;
        fastepd_direct_write_chunk(g_data, (uint32_t)n);
        done += n;
    }
    fastepd_direct_refresh(0);
}

static void sleep_panel(void)
{
    fastepd_direct_sleep();
    fastepd_mark_hw_deinitialized();
}

/* Matching config: the frame lands and refreshes. */
static void test_native_size_works(void)
{
    config(OD_CTRL_PARALLEL_M5PAPERS3, 960u, 540u, false);
    fastepd_epaper_begin();
    CHECK(!fastepd_init_failed());
    CHECK(fake_fastepd_init_panel_calls == 1);      /* the buffer from the cold case is kept */

    int before = fake_fastepd_updates;
    upload(120u * 540u);
    CHECK(fake_fastepd_updates == before + 1);
    CHECK(fake_esp32_guards_intact());

    config(OD_CTRL_PARALLEL_M5PAPERS3, 960u, 540u, true);   /* GRAY16: the whole buffer */
    upload(480u * 540u);
    CHECK(!fastepd_init_failed());
    CHECK(fake_esp32_guards_intact());
    sleep_panel();
}

/* 0xB3 now names a different (larger) parallel panel; the 960x540 buffer stays. */
static void test_panel_changes_with_buffer(void)
{
    config(OD_CTRL_PARALLEL_INKPLATE10, 1200u, 825u, true);   /* 600*825 B > 960*540/2 */
    int before = fake_fastepd_updates;
    fastepd_epaper_begin();
    CHECK(fastepd_init_failed());
    upload(600u * 825u);
    CHECK(fake_esp32_guards_intact());
    CHECK(fake_fastepd_updates == before);          /* the M5PaperS3 bus is never driven */
    CHECK(fake_fastepd_init_panel_calls == 1);      /* and FastEPD is not re-initialised */

    /* Same with the hardware still warm: no begin, straight into a transfer. */
    config(OD_CTRL_PARALLEL_M5PAPERS3, 960u, 540u, false);
    fastepd_epaper_begin();
    CHECK(!fastepd_init_failed());
    config(OD_CTRL_PARALLEL_INKPLATE10, 1200u, 825u, true);
    before = fake_fastepd_updates;
    upload(600u * 825u);
    CHECK(fake_esp32_guards_intact());
    CHECK(fake_fastepd_updates == before);
    CHECK(fastepd_init_failed());
    sleep_panel();
}

/* Same panel id, but the DisplayConfig size is not the panel's. Runs first, so this is the
 * cold begin that allocates the one 960x540 buffer every later case inherits (the FastEPD
 * object is a file-static, as on the device). */
static void test_config_size_differs(void)
{
    config(OD_CTRL_PARALLEL_M5PAPERS3, 1872u, 1404u, false);   /* 234*1404 B > 259200 */
    int before = fake_fastepd_updates;
    fastepd_epaper_begin();
    CHECK(fake_fastepd_init_panel_calls == 1);
    CHECK(fastepd_init_failed());
    upload(234u * 1404u);
    CHECK(fake_esp32_guards_intact());
    CHECK(fake_fastepd_updates == before);

    /* A boot row past the panel's last line. */
    memset(g_data, 0x33, sizeof g_data);
    fastepd_boot_write_row(1000u, g_data, 234u);
    CHECK(fake_esp32_guards_intact());

    /* Back to the native size: usable again, no reboot. */
    config(OD_CTRL_PARALLEL_M5PAPERS3, 960u, 540u, false);
    fastepd_epaper_begin();
    CHECK(!fastepd_init_failed());
    upload(120u * 540u);
    CHECK(fake_fastepd_updates == before + 1);
    CHECK(fake_esp32_guards_intact());
}

int main(void)
{
    fake_esp32_reset();
    test_config_size_differs();
    test_native_size_works();
    test_panel_changes_with_buffer();
    printf("esp32_fastepd: %u checks, %u failures\n", g_checks, g_failures);
    return g_failures == 0u ? 0 : 1;
}
