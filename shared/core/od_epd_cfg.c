/* See od_epd_cfg.h. The packet offsets below are asserted against the wire structs. */
#include "od_epd_cfg.h"

#include "od_cfg_index.h"
#include "od_epd.h"
#include "od_log.h"
#include "opendisplay_structs.h"

#include <stddef.h>
#include <string.h>

#define MODE_OFF_NAME      2u
#define MODE_OFF_SCHEME   14u
#define MODE_OFF_PASSES   15u
#define MODE_OFF_TMIN     16u
#define MODE_OFF_SLOT     19u
#define MODE_OFF_VSN1     27u
#define MODE_OFF_SEQLEN   35u
#define MODE_OFF_SEQ      37u

#define LUT_OFF_CRC        2u
#define LUT_OFF_DATA       4u

#define PANEL_OFF_FLAGS      1u
#define PANEL_OFF_IDLE       2u
#define PANEL_OFF_TIMEOUT    3u
#define PANEL_OFF_FIXED      5u
#define PANEL_OFF_CODEMAP    7u
#define PANEL_OFF_SLEEPLEN  12u
#define PANEL_OFF_SLEEPSEQ  13u

_Static_assert(sizeof(struct DisplayModeHead) == MODE_OFF_SEQ, "0xAE head size");
_Static_assert(offsetof(struct DisplayModeHead, seq_len) == MODE_OFF_SEQLEN, "0xAE seq_len");
_Static_assert(offsetof(struct DisplayModeHead, color_scheme) == MODE_OFF_SCHEME, "0xAE scheme");
_Static_assert(sizeof(struct DisplayLutHead) == LUT_OFF_DATA, "0xAF head size");
_Static_assert(offsetof(struct DisplayLutHead, crc16) == LUT_OFF_CRC, "0xAF crc");
_Static_assert(sizeof(struct DisplayPanelHead) == PANEL_OFF_SLEEPSEQ, "0xB1 head size");

#define RAW_BUSY_POLL_MS 2u

static od_epd_t s_epd;
static od_panel_desc_t s_desc;
static const od_epd_cfg_bus_t *s_bus;
static bool s_loaded;
static bool s_in_pass;
static int s_last_rc;
static uint8_t s_mode_entry[OD_CFG_INDEX_MAX];
static uint8_t s_desc_diag;

/* Load-failure stage, for od_epd_desc_diag(). Same packing as BG22: stage in bits 7..5, indexed
 * packet count in bits 4..0; stage 0 is success. */
enum {
    LOAD_OK = 0,
    LOAD_NO_INDEX = 1,
    LOAD_NO_PANEL = 2,
    LOAD_PANEL_READ = 3,
    LOAD_INIT = 4,
};

static bool load_result(uint8_t stage)
{
    const int n = od_cfg_index_count();

    s_desc_diag = (uint8_t)(((unsigned)stage << 5) | ((unsigned)(n < 0 ? 0 : n) & 0x1Fu));
    return stage == LOAD_OK;
}

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static uint16_t min16(uint32_t a, uint32_t b)
{
    return (uint16_t)(a < b ? a : b);
}

static bool bus_faulted(void)
{
    return s_bus != NULL && s_bus->faulted != NULL && s_bus->faulted();
}

/* --- od_epd io ------------------------------------------------------------------------------ */

static void io_cmd(uint8_t c)
{
    s_bus->cmd(c);
}

static void io_data(const uint8_t *d, size_t n)
{
    if (d != NULL && n != 0u) {
        s_bus->data(d, n);
    }
}

static void io_reset(uint32_t hold_ms)
{
    s_bus->reset(hold_ms == 0u ? 1u : hold_ms);
}

/* No BUSY pin reads as idle, so od_epd falls back to the descriptor's fixed delay. */
static bool io_busy_level(void)
{
    const int level = s_bus->busy_read();

    if (level < 0) {
        return s_desc.busy_idle_level != 0u;
    }
    return level != 0;
}

static void io_delay_ms(uint32_t ms)
{
    s_bus->delay_ms(ms);
}

static const od_cfg_entry_t *mode_entry(uint8_t mode)
{
    return mode < s_desc.mode_count ? od_cfg_index_entry((int)s_mode_entry[mode]) : NULL;
}

static uint16_t io_seq_read(uint8_t mode, uint16_t off, uint8_t *dst, uint16_t len)
{
    const od_cfg_entry_t *e = mode_entry(mode);

    return e != NULL ? (uint16_t)od_cfg_read(e, (uint16_t)(MODE_OFF_SEQ + off), dst, len) : 0u;
}

static bool io_mode_read(uint8_t index, od_epd_mode_t *out)
{
    const od_cfg_entry_t *e = mode_entry(index);
    uint8_t h[MODE_OFF_SEQ];

    if (out == NULL || e == NULL || e->len < MODE_OFF_SEQ ||
        od_cfg_read(e, 0u, h, sizeof(h)) != sizeof(h)) {
        return false;
    }
    memset(out, 0, sizeof(*out));
    memcpy(out->name, &h[MODE_OFF_NAME], sizeof(out->name));
    out->name[sizeof(out->name) - 1u] = '\0';
    out->color_scheme = h[MODE_OFF_SCHEME];
    out->passes = h[MODE_OFF_PASSES];
    memcpy(out->lut_slot, &h[MODE_OFF_SLOT], OD_EPD_MAX_PASSES);
    memcpy(out->vsn1, &h[MODE_OFF_VSN1], OD_EPD_MAX_PASSES);
    out->temp_min_c = (int8_t)h[MODE_OFF_TMIN];
    out->temp_max_c = (int8_t)h[MODE_OFF_TMIN + 1u];
    out->temp_policy = h[MODE_OFF_TMIN + 2u];
    out->seq_len = rd16(&h[MODE_OFF_SEQLEN]);
    if (out->passes == 0u || out->passes > OD_EPD_MAX_PASSES ||
        out->seq_len == 0u || out->seq_len > OD_EPD_SEQ_MAX ||
        (uint32_t)MODE_OFF_SEQ + out->seq_len > e->len) {
        od_log_warn("drive: mode %u rejected", (unsigned)index);
        return false;
    }
    return true;
}

/* Matched on body[1]: the packet-header byte is a don't-care the host writers disagree about. */
static const od_cfg_entry_t *find_lut(uint8_t slot)
{
    for (int i = 0; i < od_cfg_index_count(); i++) {
        const od_cfg_entry_t *e = od_cfg_index_entry(i);

        if (e != NULL && e->type == OD_PKT_DISPLAY_LUT && e->sub == slot) {
            return e;
        }
    }
    return NULL;
}

static uint16_t io_lut_read(uint8_t slot, uint16_t off, uint8_t *dst, uint16_t len)
{
    const od_cfg_entry_t *e = find_lut(slot);

    return e != NULL ? (uint16_t)od_cfg_read(e, (uint16_t)(LUT_OFF_DATA + off), dst, len) : 0u;
}

/* A bad CRC reports the slot as absent: a truncated waveform still has a plausible analog header,
 * so it would drive sane voltages with nonsense frames and render the wrong image. */
static bool io_lut_info(uint8_t slot, uint16_t *len_out)
{
    const od_cfg_entry_t *e = find_lut(slot);
    uint8_t hdr[LUT_OFF_DATA];
    uint8_t buf[64];
    uint16_t len;
    uint16_t want;
    uint16_t crc = 0xFFFFu;
    uint16_t off = 0u;

    if (e == NULL || e->len <= LUT_OFF_DATA ||
        od_cfg_read(e, 0u, hdr, sizeof(hdr)) != sizeof(hdr)) {
        return false;
    }
    len = (uint16_t)(e->len - LUT_OFF_DATA);
    want = rd16(&hdr[LUT_OFF_CRC]);
    if (len > OD_EPD_LUT_BYTES) {
        return false;
    }
    while (off < len) {
        const uint16_t chunk = min16((uint32_t)(len - off), sizeof(buf));

        if (od_cfg_read(e, (uint16_t)(LUT_OFF_DATA + off), buf, chunk) != chunk) {
            return false;
        }
        for (uint16_t i = 0u; i < chunk; i++) {
            crc ^= (uint16_t)((uint16_t)buf[i] << 8);
            for (int b = 0; b < 8; b++) {
                crc = (crc & 0x8000u) ? (uint16_t)((crc << 1) ^ 0x1021u) : (uint16_t)(crc << 1);
            }
        }
        off = (uint16_t)(off + chunk);
    }
    if (crc != want) {
        od_log_warn("drive: lut slot %u CRC mismatch", (unsigned)slot);
        return false;
    }
    if (len_out != NULL) {
        *len_out = len;
    }
    return true;
}

/* The optional hooks stay NULL, so a sequence using SET_PIN, CS_SELECT or READ_SCRATCH is refused
 * at validation with SEQ_UNSUPPORTED before any panel I/O. */
static const od_epd_io_t s_io = {
    .cmd = io_cmd,
    .data = io_data,
    .reset = io_reset,
    .busy_level = io_busy_level,
    .delay_ms = io_delay_ms,
    .seq_read = io_seq_read,
    .mode_read = io_mode_read,
    .lut_info = io_lut_info,
    .lut_read = io_lut_read,
};

/* --- session -------------------------------------------------------------------------------- */

bool od_epd_cfg_load(const struct DisplayConfig *d, const od_epd_cfg_bus_t *bus)
{
    const od_cfg_entry_t *panel = NULL;
    uint8_t body[PANEL_OFF_SLEEPSEQ + OD_EPD_SLEEP_SEQ_MAX];
    uint16_t got;

    s_loaded = false;
    s_in_pass = false;
    if (d == NULL || bus == NULL || bus->cmd == NULL || bus->data == NULL ||
        bus->reset == NULL || bus->busy_read == NULL || bus->delay_ms == NULL ||
        bus->now_ms == NULL) {
        return false;
    }
    if (od_cfg_index_build() < 0) {
        return load_result(LOAD_NO_INDEX);
    }
    s_bus = bus;
    memset(&s_desc, 0, sizeof(s_desc));
    s_desc.width = d->pixel_width;
    s_desc.height = d->pixel_height;
    s_desc.colour_scheme = d->color_scheme;

    for (int i = 0; i < od_cfg_index_count(); i++) {
        const od_cfg_entry_t *e = od_cfg_index_entry(i);
        uint8_t instance;

        if (e == NULL) {
            continue;
        }
        if (e->type == OD_PKT_DISPLAY_PANEL && e->instance == d->instance_number) {
            panel = e;
        } else if (e->type == OD_PKT_DISPLAY_MODE && s_desc.mode_count < OD_CFG_INDEX_MAX &&
                   od_cfg_read(e, 0u, &instance, 1u) == 1u && instance == d->instance_number) {
            s_mode_entry[s_desc.mode_count++] = (uint8_t)i;
        }
    }
    if (panel == NULL || panel->len < PANEL_OFF_SLEEPSEQ || s_desc.mode_count == 0u) {
        return load_result(LOAD_NO_PANEL);
    }
    memset(body, 0, sizeof(body));
    got = min16(panel->len, sizeof(body));
    if (od_cfg_read(panel, 0u, body, got) != got) {
        return load_result(LOAD_PANEL_READ);
    }
    s_desc.busy_usable = (body[PANEL_OFF_FLAGS] & OD_PANEL_FLAG_BUSY_USABLE) != 0u;
    s_desc.busy_idle_level = body[PANEL_OFF_IDLE];
    s_desc.busy_timeout_ms = rd16(&body[PANEL_OFF_TIMEOUT]);
    s_desc.busy_fixed_delay_ms = rd16(&body[PANEL_OFF_FIXED]);
    memcpy(s_desc.code_map, &body[PANEL_OFF_CODEMAP], sizeof(s_desc.code_map));
    s_desc.sleep_seq_len = body[PANEL_OFF_SLEEPLEN];
    if (s_desc.sleep_seq_len > OD_EPD_SLEEP_SEQ_MAX ||
        (uint32_t)PANEL_OFF_SLEEPSEQ + s_desc.sleep_seq_len > got) {
        s_desc.sleep_seq_len = 0u;
    }
    memcpy(s_desc.sleep_seq, &body[PANEL_OFF_SLEEPSEQ], OD_EPD_SLEEP_SEQ_MAX);

    if (od_epd_init(&s_epd, &s_desc, &s_io) != OD_EPD_RC_OK) {
        return load_result(LOAD_INIT);
    }
    s_loaded = true;
    return load_result(LOAD_OK);
}

uint8_t od_epd_desc_diag(void)
{
    return s_desc_diag;
}

bool od_epd_cfg_loaded(void)
{
    return s_loaded;
}

uint8_t od_epd_cfg_busy_idle_level(void)
{
    return s_desc.busy_idle_level;
}

bool od_epd_cfg_select(uint8_t mode, od_epd_cfg_mode_t *out)
{
    if (!s_loaded || od_epd_select_mode(&s_epd, mode) != OD_EPD_RC_OK) {
        return false;
    }
    if (out != NULL) {
        out->passes = s_epd.active.passes;
        out->color_scheme = s_epd.active.color_scheme;
        out->temp_min_c = s_epd.active.temp_min_c;
        out->temp_max_c = s_epd.active.temp_max_c;
        out->temp_policy = s_epd.active.temp_policy;
    }
    return true;
}

uint8_t od_epd_cfg_mode_count(void)
{
    return s_loaded ? s_desc.mode_count : 0u;
}

bool od_epd_cfg_begin_pass(uint8_t pass)
{
    if (!s_loaded) {
        s_last_rc = OD_EPD_RC_ARG;
        return false;
    }
    s_last_rc = od_epd_begin_pass(&s_epd, pass);
    s_in_pass = s_last_rc == OD_EPD_RC_OK && !bus_faulted();
    return s_in_pass;
}

bool od_epd_cfg_write(const uint8_t *data, size_t len)
{
    return s_in_pass && od_epd_write_image_chunk(&s_epd, data, len) == OD_EPD_RC_OK &&
           !bus_faulted();
}

bool od_epd_cfg_next_plane(void)
{
    return s_in_pass && od_epd_next_plane(&s_epd) == OD_EPD_RC_OK;
}

bool od_epd_cfg_finish(void)
{
    if (!s_in_pass) {
        return false;
    }
    s_in_pass = false;
    return od_epd_finish_pass(&s_epd) == OD_EPD_RC_OK && !bus_faulted();
}

void od_epd_cfg_sleep(void)
{
    s_in_pass = false;
    if (s_loaded) {
        (void)od_epd_sleep(&s_epd);
    }
}

int od_epd_cfg_last_rc(void)
{
    return s_last_rc;
}

/* --- multi-pass stream ---------------------------------------------------------------------- */

bool od_epd_cfg_stream_init(od_epd_cfg_stream_t *s, const od_color_geometry_t *frame,
                            uint8_t passes, bool compressed, const uint8_t *tail,
                            uint16_t tail_len)
{
    const uint64_t total = frame != NULL ? (uint64_t)frame->total_bytes * passes : 0u;

    if (s == NULL || total == 0u || total > UINT32_MAX) {
        return false;
    }
    if (compressed) {
        uint32_t declared;

        if (tail == NULL || tail_len < 4u) {
            return false;
        }
        declared = (uint32_t)tail[0] | ((uint32_t)tail[1] << 8)
                 | ((uint32_t)tail[2] << 16) | ((uint32_t)tail[3] << 24);
        if (declared != (uint32_t)total) {
            od_log_error("drive: declared %u != passes*frame %u", (unsigned)declared,
                         (unsigned)total);
            return false;
        }
    }
    memset(s, 0, sizeof(*s));
    s->frame_bytes = frame->total_bytes;
    s->total_bytes = (uint32_t)total;
    s->passes = passes;
    s->plane_size = frame->layout == OD_COLOR_LAYOUT_CONTROLLER_PLANES ? frame->part_bytes[0] : 0u;
    return true;
}

bool od_epd_cfg_stream_write(od_epd_cfg_stream_t *s, uint32_t stream_offset,
                             const uint8_t *data, size_t len, void (*phase)(bool refreshing))
{
    size_t off = 0u;

    if (s == NULL || data == NULL) {
        return false;
    }
    while (off < len) {
        const uint32_t logical = stream_offset + (uint32_t)off;
        const uint32_t frame_end = (uint32_t)(s->pass + 1u) * s->frame_bytes;
        const uint32_t frame_start = frame_end - s->frame_bytes;
        uint32_t chunk = (uint32_t)(len - off);

        if (logical >= frame_end || logical < frame_start) {
            return false;
        }
        if (chunk > frame_end - logical) {
            chunk = frame_end - logical;
        }
        if (s->plane_size > 0u) {
            const uint32_t plane_end = frame_start + s->plane_size;

            if (logical == plane_end && !od_epd_cfg_next_plane()) {
                return false;
            }
            if (logical < plane_end && chunk > plane_end - logical) {
                chunk = plane_end - logical;
            }
        }
        if (!od_epd_cfg_write(data + off, chunk)) {
            return false;
        }
        off += chunk;

        if (logical + chunk == frame_end && s->pass + 1u < s->passes) {
            if (phase != NULL) {
                phase(true);
            }
            if (!od_epd_cfg_finish()) {
                od_log_error("drive: pass %u refresh failed", (unsigned)s->pass);
                return false;
            }
            s->pass++;
            if (!od_epd_cfg_begin_pass(s->pass)) {
                od_log_error("drive: pass %u begin failed", (unsigned)s->pass);
                return false;
            }
            if (phase != NULL) {
                phase(false);
            }
        }
    }
    return true;
}

/* --- raw session ---------------------------------------------------------------------------- */

void od_epd_cfg_raw_cmd(const uint8_t *data, uint16_t len)
{
    if (s_bus == NULL || data == NULL || len == 0u) {
        return;
    }
    s_bus->cmd(data[0]);
    if (len > 1u) {
        s_bus->data(&data[1], (size_t)(len - 1u));
    }
}

void od_epd_cfg_raw_data(const uint8_t *data, uint16_t len)
{
    if (s_bus != NULL && data != NULL && len != 0u) {
        s_bus->data(data, len);
    }
}

/* Raw BUSY ignores the descriptor's busy_usable flag: a host debugging a panel asks what the pin
 * says, and a panel whose BUSY is marked unusable is exactly the one being characterised. */
static bool raw_busy_now(void)
{
    const int level = s_bus->busy_read();

    return level >= 0 && level != (s_desc.busy_idle_level != 0u);
}

void od_epd_cfg_raw_busy(uint32_t timeout_ms, uint16_t *elapsed_ms_out, bool *still_busy_out)
{
    uint32_t t0;
    uint32_t waited = 0u;

    if (s_bus == NULL) {
        if (elapsed_ms_out != NULL) {
            *elapsed_ms_out = 0u;
        }
        if (still_busy_out != NULL) {
            *still_busy_out = false;
        }
        return;
    }
    t0 = s_bus->now_ms();
    while (raw_busy_now()) {
        waited = s_bus->now_ms() - t0;
        if (waited >= timeout_ms) {
            break;
        }
        s_bus->delay_ms(RAW_BUSY_POLL_MS);
    }
    waited = s_bus->now_ms() - t0;
    if (elapsed_ms_out != NULL) {
        *elapsed_ms_out = (uint16_t)(waited > 0xFFFFu ? 0xFFFFu : waited);
    }
    if (still_busy_out != NULL) {
        *still_busy_out = raw_busy_now();
    }
}

void od_epd_cfg_raw_reset(uint8_t hold_ms)
{
    if (s_bus != NULL) {
        s_bus->reset(hold_ms == 0u ? 1u : hold_ms);
    }
}
