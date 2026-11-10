/* libFuzzer target: a stored config record, end to end through everything that reads it on
 * demand -- od_cfg_index, od_epd_cfg + od_epd (load, every mode, one pass), od_action, and
 * od_config_store_save_carry. The config is written by any client and, with encryption off,
 * before authentication; every one of these runs on it at boot or at the next START.
 *
 * Input: [ctl:1][temp:1][payload...]. ctl bits pick the display geometry/scheme and whether the
 * carry runs; temp is the reading the drive temp policy sees (0x7F = none).
 *
 * Oracles beyond ASan/UBSan (abort() = finding):
 *   - od_cfg_index entries equal the packets od_config_tlv_walk delivers (first 32);
 *   - a carry that returns OK leaves a record od_config_store_load accepts.
 * Known divergences (KNOWN_BUG ids in the tests/host suites) are not asserted here.
 */
#include "od_check.h"   /* only for KNOWN_BUG's guard in the fixture; no CHECKs here */
#include "drive_fixture.h"

#include "od_action.h"
#include "od_action_app.h"
#include "od_cfg_index.h"
#include "od_config_carry.h"
#include "od_epd.h"
#include "od_epd_cfg.h"

#include <limits.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

void od_action_app_led(uint8_t colour, uint8_t brightness) { (void)colour; (void)brightness; }
void od_action_app_led_stop(void) {}
void od_action_app_display_slot(uint8_t slot) { (void)slot; }
void od_action_app_buzzer(uint8_t melody) { (void)melody; }
bool od_action_app_has_buzzer(void) { return true; }
static int8_t s_temp;
int8_t od_action_app_temperature_c(void) { return s_temp; }

typedef struct {
    uint8_t  type;
    uint16_t offset, len;
} pkt_t;

typedef struct {
    pkt_t          p[OD_CFG_INDEX_MAX];
    unsigned       n;
    const uint8_t *base;
} walk_t;

static bool walk_cb(void *ctx, uint8_t id, od_span_t body)
{
    walk_t *w = ctx;
    const bool indexed = id >= OD_PKT_DISPLAY_MODE && id <= OD_PKT_ACTION_TABLE;

    if (indexed && body.n > 0u && w->n < OD_CFG_INDEX_MAX) {
        w->p[w->n].type = id;
        w->p[w->n].offset = (uint16_t)(body.p - w->base);
        w->p[w->n].len = (uint16_t)body.n;
        w->n++;
    }
    return true;
}

static void check_index(const uint8_t *payload, size_t n)
{
    walk_t w;
    const int count = od_cfg_index_build();

    memset(&w, 0, sizeof w);
    w.base = payload;
    (void)od_config_tlv_walk(od_span_make(payload, n), walk_cb, &w, NULL, NULL);
    if (count < 0 || (unsigned)count != w.n) {
        abort();
    }
    for (unsigned i = 0; i < w.n; i++) {
        const od_cfg_entry_t *e = od_cfg_index_entry((int)i);
        if (e->type != w.p[i].type || e->offset != w.p[i].offset || e->len != w.p[i].len) {
            abort();
        }
    }
}

static void drive(uint8_t ctl, int8_t temp)
{
    static const uint8_t schemes[] = { 0u, 1u, 3u, 4u, 5u, 8u };
    struct DisplayConfig d = fx_display(0u, (uint16_t)(8u + 8u * (ctl & 3u)),
                                        (uint16_t)(4u + ((ctl >> 2) & 3u)),
                                        schemes[(ctl >> 4) % sizeof schemes]);
    static uint8_t frame[2048];

    fx_bus_clear();
    fx_busy_level = (ctl & 0x40u) ? 1 : -1;
    if (!od_epd_cfg_load(&d, &FX_BUS)) {
        return;
    }
    const uint8_t modes = od_epd_cfg_mode_count();
    for (uint8_t m = 0; m < modes && m < 4u; m++) {
        uint8_t mode = m;
        od_epd_cfg_mode_t hdr;
        od_color_geometry_t geom;
        od_epd_cfg_stream_t st;

        if (od_epd_cfg_select_policy(&mode, temp, &hdr) != 0u) {
            continue;
        }
        if (od_color_direct_geometry(hdr.color_scheme == 0xFFu ? d.color_scheme
                                                                : hdr.color_scheme,
                                     d.pixel_width, d.pixel_height, &geom) != OD_COLOR_OK ||
            geom.total_bytes > sizeof frame) {
            continue;
        }
        if (!od_epd_cfg_begin_pass(0u)) {
            continue;
        }
        if (od_epd_cfg_stream_init(&st, &geom, hdr.passes, false, NULL, 0u)) {
            for (uint8_t p = 0; p < hdr.passes; p++) {
                if (!od_epd_cfg_stream_write(&st, (uint32_t)p * geom.total_bytes, frame,
                                             geom.total_bytes, NULL)) {
                    break;
                }
            }
        }
        (void)od_epd_cfg_finish();
    }
    od_epd_cfg_raw_busy(50u, NULL, NULL);
    od_epd_cfg_sleep();
}

static void actions(int8_t temp)
{
    uint32_t now = 1000u;

    s_temp = temp;
    od_action_reload();
    for (uint8_t t = 1u; t <= 7u; t++) {
        od_action_trigger(t, 0u);
        for (int i = 0; i < 12; i++) {
            const uint32_t next = od_action_service(now);
            now += next == OD_ACTION_IDLE ? 2000u : (next > 3000u ? 3000u : next + 1u);
            s_temp = (int8_t)(s_temp ^ (int8_t)(i & 1));
        }
    }
}

static void carry(void)
{
    static uint8_t ws[OD_CONFIG_STORE_HEADER_SIZE + OD_CONFIG_MAX_SIZE];
    fx_cfg_t c;
    struct DisplayConfig d = fx_display(0u, 400u, 300u, 0u);

    fx_cfg_begin(&c, 1u);
    fx_cfg_legacy_head(&c, &d);
    fx_cfg_finish(&c);
    if (od_config_store_save_carry(ws, sizeof ws, c.b, c.n, NULL) == OD_CONFIG_STORE_OK) {
        static uint8_t out[OD_CONFIG_MAX_SIZE];
        uint32_t n = sizeof out;
        if (od_config_store_load(out, &n) != OD_CONFIG_STORE_OK) {
            abort();
        }
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 2u || size - 2u > OD_CONFIG_MAX_SIZE) {
        return 0;
    }
    const uint8_t ctl = data[0];
    const int8_t temp = (int8_t)data[1];

    fx_store_payload(data + 2u, (uint32_t)(size - 2u));
    if (size > 2u) {
        check_index(data + 2u, size - 2u);
    }
    drive(ctl, temp);
    actions(temp);
    if (ctl & 0x80u) {
        carry();
    }
    return 0;
}
