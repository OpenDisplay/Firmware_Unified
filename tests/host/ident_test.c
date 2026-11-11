/* ident_test: CMD_READ_IDENT (0x0046) on the default (ESP32 / Nordic) profile -- the 3.0 tail
 * [flags][max_config:2][max_seq_steps:2][escapes], the per-family fw_target, NEEDS_MIGRATION
 * (no drive packets, or a mode 0 breaking the mode-0 rule) and BUILTIN_DRIVER for a display a
 * native controller driver owns. The BG22 profile's values are asserted in silabs_fault_test. */
#include "od_check.h"
#include "drive_fixture.h"

#include "od_cmd_app.h"
#include "od_cmd_test_ctx.h"
#include "od_config.h"
#include "od_ident_app.h"
#include "od_reply.h"
#include "opendisplay_protocol.h"

static uint8_t g_rsp[64];
static uint16_t g_rsp_n;
static struct od_config g_cfg;
static bool g_native;

od_txq_status_t od_reply_plain(od_tx_reservation_t *r, const od_reply_t *rp,
                               const uint8_t *frame, uint16_t len)
{
    (void)r;
    (void)rp;
    g_rsp_n = len < sizeof g_rsp ? len : (uint16_t)sizeof g_rsp;
    memcpy(g_rsp, frame, g_rsp_n);
    return OD_TXQ_OK;
}

const struct od_config *od_ident_app_config(void) { return &g_cfg; }
bool od_ident_app_native_panel(const struct DisplayConfig *d) { (void)d; return g_native; }

static void ident(void)
{
    od_tx_reservation_t r;
    od_cmd_ctx_t ctx = od_test_cmd_ctx((od_reply_t){ OD_ORIGIN_BLE, 1u }, &r, 2u, false);

    g_rsp_n = 0u;
    CHECK(od_cmd_app_read_ident(&ctx, od_span_make(NULL, 0u)) == OD_CMD_OK);
}

/* One display: an 0xB1 head and one mode 0 in `scheme`. */
static void store(uint8_t mode_scheme)
{
    static const uint8_t panel[13] = { 0, 0x01, 0x01, 0xE8, 0x03, 0, 0, 0, 1, 3, 2, 1, 0 };
    static const uint8_t seq[] = { 0x01, 0x10, 0x00, 0x05, 0x00 };   /* cmd 0x10, IMAGE */
    uint8_t mode[37 + sizeof seq];
    fx_cfg_t c;

    memset(mode, 0, sizeof mode);
    strncpy((char *)&mode[2], "full", 12u);
    mode[14] = mode_scheme;
    mode[15] = 1u;
    memset(&mode[19], 0xFF, 8u);
    mode[35] = (uint8_t)sizeof seq;
    memcpy(&mode[37], seq, sizeof seq);
    fx_cfg_begin(&c, 1u);
    fx_cfg_ext(&c, OD_PKT_DISPLAY_PANEL, panel, sizeof panel);
    fx_cfg_ext(&c, OD_PKT_DISPLAY_MODE, mode, sizeof mode);
    fx_cfg_finish(&c);
    fx_cfg_store(&c);
}

int main(void)
{
    (void)fx_xfail_count;
    CASE("no config: header, zero records, then the tail");
    memset(&g_cfg, 0, sizeof g_cfg);
    ident();
    CHECK(g_rsp_n == 11u);
    CHECK(g_rsp[0] == RESP_ACK && g_rsp[1] == RESP_READ_IDENT);
    CHECK(g_rsp[3] == OD_IDENT_TARGET_NRF52840);
    CHECK(g_rsp[4] == 0u);
    CHECK(g_rsp[5] == OD_IDENT_FLAG_CONFIG_DRIVEN);
    /* 4096 B stored-config cap, but chunked CONFIG_WRITE carries at most 20 x 200. */
    CHECK(g_rsp[6] == (4000u & 0xFFu) && g_rsp[7] == (4000u >> 8));
    CHECK(g_rsp[8] == 128u && g_rsp[9] == 0u);
    CHECK(g_rsp[10] == (OD_IDENT_ESC_WINDOW | OD_IDENT_ESC_FILL));

    CASE("a display with a conforming mode 0: no migration needed");
    g_cfg.loaded = true;
    g_cfg.display_count = 1u;
    g_cfg.displays[0] = fx_display(0u, 296u, 128u, 0u);
    g_cfg.displays[0].panel_ic_type = 0x0002u;
    store(0xFFu);
    ident();
    CHECK(g_rsp_n == 18u);
    CHECK(g_rsp[4] == 1u && g_rsp[5] == 0x02u && g_rsp[6] == 0x00u);
    CHECK(g_rsp[12] == OD_IDENT_FLAG_CONFIG_DRIVEN);

    CASE("mode 0 in another scheme: NEEDS_MIGRATION");
    store(3u);
    ident();
    CHECK(g_rsp[12] == (OD_IDENT_FLAG_CONFIG_DRIVEN | OD_IDENT_FLAG_NEEDS_MIGRATION));

    CASE("a display on a built-in controller driver: BUILTIN_DRIVER, never NEEDS_MIGRATION");
    g_native = true;
    ident();
    CHECK(g_rsp[12] == (OD_IDENT_FLAG_CONFIG_DRIVEN | OD_IDENT_FLAG_BUILTIN_DRIVER));
    return OD_CHECK_REPORT("ident");
}
