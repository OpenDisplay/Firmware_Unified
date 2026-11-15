/* libFuzzer target: a stored config record, end to end through everything that reads it on
 * demand -- od_cfg_index, od_epd_cfg + od_epd (load, every mode, one pass) and od_action, then a
 * config write over it. The config is written by any client and, with encryption off,
 * before authentication; every one of these runs on it at boot or at the next START.
 *
 * Input: [ctl:1][temp:1][payload...]. ctl bits pick the display geometry/scheme and whether the
 * overwrite runs; temp is the reading the drive temp policy sees (0x7F = none).
 *
 * od_epd streams each sequence from the stored record, so every input is also driven with the
 * medium misbehaving under the walk: a read error from the k-th NVS read on (every k the clean
 * run reached), and at the image pause a config write attempt, a clear(), and the record swapped
 * underneath without the store knowing (truncated -- reads come back short -- or the same length
 * with different bytes).
 *
 * Oracles beyond ASan/UBSan (abort() = finding):
 *   - od_cfg_index entries equal the packets od_config_tlv_walk delivers (first 32);
 *   - a config write while a pass is paused is refused with OD_CONFIG_STORE_BUSY, and once the
 *     pass is over (finished, failed or slept) a write is never refused;
 *   - whatever is stored (corrupt, truncated, unwalkable), a new config write succeeds and
 *     loads back byte for byte: a bad stored record never blocks its own replacement.
 * Known divergences (KNOWN_BUG ids in the tests/host suites) are not asserted here.
 */
#include "od_check.h"   /* only for KNOWN_BUG's guard in the fixture; no CHECKs here */
#include "drive_fixture.h"

#include "od_action.h"
#include "od_action_app.h"
#include "od_cfg_index.h"
#include "od_config_store.h"
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

enum { AT_PAUSE_NONE, AT_PAUSE_SAVE, AT_PAUSE_CLEAR, AT_PAUSE_TRUNCATE, AT_PAUSE_SWAP,
       AT_PAUSE_COUNT };

static const uint8_t *s_payload;
static uint32_t s_payload_n;
static uint8_t s_ws[OD_CONFIG_STORE_HEADER_SIZE + OD_CONFIG_MAX_SIZE];

/* What happens to the medium while the pass is paused on its first image marker. */
static void at_pause(unsigned what)
{
    static uint8_t alt[OD_CONFIG_MAX_SIZE];

    switch (what) {
    case AT_PAUSE_SAVE:
        if (od_config_store_save(s_ws, sizeof s_ws, s_payload, s_payload_n)
            != OD_CONFIG_STORE_BUSY) {
            abort();
        }
        break;
    case AT_PAUSE_CLEAR:
        (void)od_config_store_clear();
        break;
    case AT_PAUSE_TRUNCATE:
        fx_store_payload(s_payload, s_payload_n / 2u);
        break;
    case AT_PAUSE_SWAP:
        for (uint32_t i = 0; i < s_payload_n; i++) {
            alt[i] = (uint8_t)(s_payload[i] ^ (uint8_t)(0x5Au + i));
        }
        fx_store_payload(alt, s_payload_n);
        break;
    default:
        break;
    }
}

static void drive(uint8_t ctl, int8_t temp, unsigned pause_what)
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
        at_pause(pause_what);
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

/* After any drive the pass is over, so the store takes a write; it also puts the input's record
 * back for the next run. */
static void restore(void)
{
    fx_nvs_fail_after = -1;
    if (od_config_store_save(s_ws, sizeof s_ws, s_payload, s_payload_n) != OD_CONFIG_STORE_OK) {
        abort();
    }
}

static void faults(uint8_t ctl, int8_t temp)
{
    unsigned reads;

    for (unsigned what = AT_PAUSE_SAVE; what < AT_PAUSE_COUNT; what++) {
        drive(ctl, temp, what);
        restore();
    }
    reads = fx_nvs_reads;
    drive(ctl, temp, AT_PAUSE_NONE);
    reads = fx_nvs_reads - reads;
    for (unsigned k = 0; k < reads && k < 256u; k++) {
        fx_nvs_fail_after = (int)k;
        drive(ctl, temp, AT_PAUSE_NONE);
        restore();
    }
}

static void overwrite(uint8_t poke)
{
    static uint8_t ws[OD_CONFIG_STORE_HEADER_SIZE + OD_CONFIG_MAX_SIZE];
    static uint8_t out[OD_CONFIG_MAX_SIZE];
    uint32_t n = sizeof out;
    fx_cfg_t c;
    struct DisplayConfig d = fx_display(0u, 400u, 300u, 0u);

    /* Damage the stored record anywhere, header included, before writing over it. */
    if (fx_nvs_len > 0u) {
        fx_nvs[poke % fx_nvs_len] ^= 0xA5u;
    }
    fx_cfg_begin(&c, 1u);
    fx_cfg_legacy_head(&c, &d);
    fx_cfg_finish(&c);
    if (od_config_store_save(ws, sizeof ws, c.b, c.n) != OD_CONFIG_STORE_OK
        || od_config_store_load(out, &n) != OD_CONFIG_STORE_OK
        || n != c.n || memcmp(out, c.b, c.n) != 0) {
        abort();
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
    s_payload = data + 2u;
    s_payload_n = (uint32_t)(size - 2u);
    drive(ctl, temp, AT_PAUSE_NONE);
    actions(temp);
    if (s_payload_n > 0u) {
        faults(ctl, temp);
    }
    if (ctl & 0x80u) {
        overwrite((uint8_t)temp);
    }
    return 0;
}
