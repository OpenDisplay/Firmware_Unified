/* See od_epd_cfg.h. The packet offsets below are asserted against the wire structs. */
#include "od_epd_cfg.h"

#include "od_cfg_index.h"
#include "od_config_store.h"
#include "od_epd.h"
#include "od_epd_lut.h"
#include "od_log.h"
#include "opendisplay_protocol.h"
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

OD_STATIC_ASSERT(sizeof(struct DisplayModeHead) == MODE_OFF_SEQ, "0xAE head size");
OD_STATIC_ASSERT(offsetof(struct DisplayModeHead, seq_len) == MODE_OFF_SEQLEN, "0xAE seq_len");
OD_STATIC_ASSERT(offsetof(struct DisplayModeHead, color_scheme) == MODE_OFF_SCHEME, "0xAE scheme");
OD_STATIC_ASSERT(sizeof(struct DisplayLutHead) == LUT_OFF_DATA, "0xAF head size");
OD_STATIC_ASSERT(offsetof(struct DisplayLutHead, crc16) == LUT_OFF_CRC, "0xAF crc");
OD_STATIC_ASSERT(sizeof(struct DisplayPanelHead) == PANEL_OFF_SLEEPSEQ, "0xB1 head size");
OD_STATIC_ASSERT(OD_EPD_ESC_WINDOW == OD_SEQ_ESC_WINDOW, "WINDOW escape value");
OD_STATIC_ASSERT(OD_EPD_ESC_FILL == OD_SEQ_ESC_FILL, "FILL escape value");

#define RAW_BUSY_POLL_MS 2u

/* DriveSeqPinRef -> DisplayConfig pin byte, filled at load. */
#define PINREF_AUX_COUNT 6u
static uint8_t s_aux_pins[PINREF_AUX_COUNT];
static uint8_t s_named_pins[6];   /* RESET, DC, CS, CS_2, DATA, CLK */

static od_epd_t s_epd;
static od_panel_desc_t s_desc;
static const od_epd_cfg_bus_t *s_bus;
static bool s_loaded;
static bool s_in_pass;
static int s_last_rc;
/* This display's 0xAE entries in packet order. The packet's mode_index (od_cfg_entry_t.sub) is
 * the mode's identity, not its position; the first packet with a given index wins. */
static uint8_t s_mode_entry[OD_CFG_INDEX_MAX];
static uint8_t s_mode_entries;
static uint8_t s_desc_diag;
static uint16_t s_inline_expect;

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
    for (uint8_t i = 0u; i < s_mode_entries; i++) {
        const od_cfg_entry_t *e = od_cfg_index_entry((int)s_mode_entry[i]);

        if (e != NULL && e->sub == mode) {
            return e;
        }
    }
    return NULL;
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
        out->seq_len == 0u || (uint32_t)MODE_OFF_SEQ + out->seq_len > e->len) {
        od_log_warn("drive: mode %u rejected", (unsigned)index);
        return false;
    }
    return true;
}

/* Matched on body[0..1] (instance_number, lut_slot): the packet-header byte is a don't-care the
 * host writers disagree about. Slot numbers are per display, so the instance must match too.
 * The slot byte's bits 0..3 are the id a mode names; bit 7 marks a deflated body. */
static uint8_t s_instance;

static const od_cfg_entry_t *find_lut(uint8_t slot)
{
    if (slot >= OD_LUT_SLOT_MAX) {
        return NULL;
    }
    for (int i = 0; i < od_cfg_index_count(); i++) {
        const od_cfg_entry_t *e = od_cfg_index_entry(i);

        if (e != NULL && e->type == OD_PKT_DISPLAY_LUT && e->instance == s_instance &&
            (e->sub & OD_LUT_SLOT_ID_MASK) == slot && (e->sub & 0x70u) == 0u) {
            return e;
        }
    }
    return NULL;
}

#if OD_EPD_LUT_CACHE > 0
static uint32_t cfg_gen(void)
{
    return od_config_store_generation();
}

/* An inline table from the running CMD_DRIVE_WRITE_START shadows the stored slot; a stored
 * deflated slot is served from its inflated copy. NULL: read the stored bytes directly. */
static const od_lut_cache_entry_t *lut_cached(uint8_t slot)
{
    const od_lut_cache_entry_t *c = od_lut_cache_find(s_instance, slot, true, 0u);

    return c != NULL ? c : od_lut_cache_find(s_instance, slot, false, cfg_gen());
}

static uint16_t cfg_reader(void *ctx, uint16_t off, uint8_t *dst, uint16_t len)
{
    return (uint16_t)od_cfg_read((const od_cfg_entry_t *)ctx, (uint16_t)(LUT_OFF_DATA + off),
                                 dst, len);
}
#endif

static uint16_t io_lut_read(uint8_t slot, uint16_t off, uint8_t *dst, uint16_t len)
{
    const od_cfg_entry_t *e;
#if OD_EPD_LUT_CACHE > 0
    const od_lut_cache_entry_t *c = lut_cached(slot);

    if (c != NULL) {
        if ((uint32_t)off + len > c->len) {
            return 0u;
        }
        memcpy(dst, &c->data[off], len);
        return len;
    }
#endif
    e = find_lut(slot);
    if (e == NULL || (e->sub & OD_LUT_SLOT_COMPRESSED) != 0u) {
        return 0u;
    }
    return (uint16_t)od_cfg_read(e, (uint16_t)(LUT_OFF_DATA + off), dst, len);
}

/* A bad CRC or a failed family lint reports the slot as absent: a truncated waveform still has
 * a plausible analog header, so it would drive sane voltages with nonsense frames and render the
 * wrong image. The CRC covers the STORED bytes (compressed or not) and is checked first. */
static bool io_lut_info(uint8_t slot, uint16_t *len_out)
{
    const od_cfg_entry_t *e;
    uint8_t hdr[LUT_OFF_DATA];
    uint8_t buf[64];
    uint16_t len;
    uint16_t want;
    uint16_t crc = 0xFFFFu;
    uint16_t off = 0u;
    od_lut_lint_t lint;
    bool compressed;

#if OD_EPD_LUT_CACHE > 0
    {
        const od_lut_cache_entry_t *c = lut_cached(slot);

        if (c != NULL) {
            if (len_out != NULL) {
                *len_out = c->len;
            }
            return true;
        }
    }
#endif
    e = find_lut(slot);
    if (e == NULL || e->len <= LUT_OFF_DATA ||
        od_cfg_read(e, 0u, hdr, sizeof(hdr)) != sizeof(hdr)) {
        return false;
    }
    compressed = (e->sub & OD_LUT_SLOT_COMPRESSED) != 0u;
    len = (uint16_t)(e->len - LUT_OFF_DATA);
    want = rd16(&hdr[LUT_OFF_CRC]);
    if (len > OD_EPD_LUT_BYTES) {
        return false;
    }
    od_lut_lint_begin(&lint, s_desc.lut_family);
    while (off < len) {
        const uint16_t chunk = min16((uint32_t)(len - off), sizeof(buf));

        if (od_cfg_read(e, (uint16_t)(LUT_OFF_DATA + off), buf, chunk) != chunk) {
            return false;
        }
        crc = od_lut_crc16(crc, buf, chunk);
        if (!compressed) {
            od_lut_lint_feed(&lint, buf, chunk);
        }
        off = (uint16_t)(off + chunk);
    }
    if (crc != want) {
        od_log_warn("drive: lut slot %u CRC mismatch", (unsigned)slot);
        return false;
    }
    if (compressed) {
#if OD_EPD_LUT_CACHE > 0
        od_lut_cache_entry_t *c = od_lut_cache_claim();
        uint16_t out = 0u;

        if (c == NULL ||
            !od_lut_inflate(cfg_reader, (void *)e, len, c->data, sizeof(c->data), &out) ||
            !od_lut_lint(s_desc.lut_family, c->data, out)) {
            od_log_warn("drive: lut slot %u inflate/lint failed", (unsigned)slot);
            return false;
        }
        c->used = true;
        c->is_inline = false;
        c->instance = s_instance;
        c->slot = slot;
        c->len = out;
        c->gen = cfg_gen();
        len = out;
#else
        return false;   /* no inflater on this target: the slot is absent */
#endif
    } else if (!od_lut_lint_done(&lint)) {
        od_log_warn("drive: lut slot %u failed family %u lint", (unsigned)slot,
                    (unsigned)s_desc.lut_family);
        return false;
    }
    if (len_out != NULL) {
        *len_out = len;
    }
    return true;
}

/* 0xFF when the reference names nothing wired. Aux pins treat 0x00 as unwired too: legacy
 * configs carry 0 in those ex-reserved bytes. */
static uint8_t resolve_pin_ref(uint8_t ref)
{
    uint8_t raw = 0xFFu;

    if (ref < PINREF_AUX_COUNT) {
        raw = s_aux_pins[ref] == 0x00u ? 0xFFu : s_aux_pins[ref];
    } else if (ref >= OD_SEQ_PINREF_RESET && ref <= OD_SEQ_PINREF_CLK) {
        raw = s_named_pins[ref - OD_SEQ_PINREF_RESET];
    }
    return raw;
}

static bool io_set_pin(uint8_t ref, uint8_t level)
{
    const uint8_t pin = resolve_pin_ref(ref);

    if (pin == 0xFFu) {
        od_log_warn("drive: SET_PIN ref 0x%02X unwired", (unsigned)ref);
        return false;
    }
    return s_bus->pin_write(pin, level);
}

static bool io_cs_select(uint8_t which)
{
    return s_bus->cs_select(which);
}

static bool io_read_bytes(uint8_t ref, uint8_t *dst, uint8_t n)
{
    const uint8_t pin = resolve_pin_ref(ref);

    return pin != 0xFFu && s_bus->read_bytes(pin, dst, n);
}

/* Optional hooks are filled at load from what the bus offers; a NULL one makes od_epd refuse
 * the matching escape at validation with SEQ_UNSUPPORTED before any panel I/O. */
static od_epd_io_t s_io = {
    .cmd = io_cmd,
    .data = io_data,
    .reset = io_reset,
    .busy_level = io_busy_level,
    .delay_ms = io_delay_ms,
    .seq_read = io_seq_read,
    .mode_read = io_mode_read,
    .lut_info = io_lut_info,
    .lut_read = io_lut_read,
    .config_gen = od_config_store_generation,
};

/* A pass streams its sequence and waveform from the stored config and pauses at the image
 * marker while the frame arrives, so a config write is refused for exactly that span
 * (od_config_store_hold_writes); every path that ends a pass comes through here. */
static void set_in_pass(bool on)
{
    s_in_pass = on;
    od_config_store_hold_writes(on);
}

/* --- session -------------------------------------------------------------------------------- */

bool od_epd_cfg_load(const struct DisplayConfig *d, const od_epd_cfg_bus_t *bus)
{
    const od_cfg_entry_t *panel = NULL;
    uint8_t body[PANEL_OFF_SLEEPSEQ + OD_EPD_SLEEP_SEQ_MAX];
    uint16_t got;

    s_loaded = false;
    set_in_pass(false);
    if (d == NULL || bus == NULL || bus->cmd == NULL || bus->data == NULL ||
        bus->reset == NULL || bus->busy_read == NULL || bus->delay_ms == NULL ||
        bus->now_ms == NULL) {
        return false;
    }
    if (od_cfg_index_build() < 0) {
        return load_result(LOAD_NO_INDEX);
    }
    s_bus = bus;
    s_instance = d->instance_number;
    s_inline_expect = 0u;
#if OD_EPD_LUT_CACHE > 0
    /* Inline tables live for one drive; inflated slots are re-made from the config just loaded
     * (the generation key alone misses a store that bypasses od_config_store). */
    od_lut_cache_clear();
#endif
    memset(&s_desc, 0, sizeof(s_desc));
    s_mode_entries = 0u;
    s_desc.width = d->pixel_width;
    s_desc.height = d->pixel_height;
    s_desc.colour_scheme = d->color_scheme;

    for (int i = 0; i < od_cfg_index_count(); i++) {
        const od_cfg_entry_t *e = od_cfg_index_entry(i);
        uint8_t instance;

        if (e == NULL) {
            continue;
        }
        /* First match wins for every duplicated drive packet (0xB1, 0xAE per mode_index, 0xAF
         * per slot), as in od_cfg_find, the 0xB2 table and BG22. */
        if (e->type == OD_PKT_DISPLAY_PANEL && e->instance == d->instance_number) {
            if (panel == NULL) {
                panel = e;
            }
        } else if (e->type == OD_PKT_DISPLAY_MODE && s_mode_entries < OD_CFG_INDEX_MAX &&
                   od_cfg_read(e, 0u, &instance, 1u) == 1u && instance == d->instance_number) {
            s_mode_entry[s_mode_entries++] = (uint8_t)i;
            /* od_epd bounds a mode number by mode_count: one past the highest index. Gaps
             * read as absent (io_mode_read fails -> BAD_MODE). */
            if (e->sub >= s_desc.mode_count) {
                s_desc.mode_count = (uint8_t)(e->sub == 0xFFu ? 0xFFu : e->sub + 1u);
            }
        }
    }
    if (panel == NULL || panel->len < PANEL_OFF_SLEEPSEQ || s_mode_entries == 0u) {
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
    {
        /* 0xB1 tail v1 after sleep_seq: only lut_family and busy_timeout_s mean anything here;
         * the rest is host information. Unknown versions are read as v1 (append-only). */
        uint8_t ext[OD_PANEL_EXT_V1_LEN];
        const uint16_t at = (uint16_t)(PANEL_OFF_SLEEPSEQ + body[PANEL_OFF_SLEEPLEN]);
        uint16_t n = 0u;

        memset(ext, 0, sizeof(ext));
        if (body[PANEL_OFF_SLEEPLEN] <= OD_EPD_SLEEP_SEQ_MAX && panel->len > at) {
            n = min16((uint32_t)(panel->len - at), sizeof(ext));
            if (od_cfg_read(panel, at, ext, n) != n) {
                n = 0u;
            }
        }
        if (n > OD_PANEL_EXT_FAMILY && ext[OD_PANEL_EXT_VERSION] >= 1u &&
            ext[OD_PANEL_EXT_FAMILY] < OD_LUT_FAMILY_COUNT) {
            s_desc.lut_family = ext[OD_PANEL_EXT_FAMILY];
        }
        if (n > OD_PANEL_EXT_TIMEOUT_S && ext[OD_PANEL_EXT_VERSION] >= 1u &&
            ext[OD_PANEL_EXT_TIMEOUT_S] != 0u) {
            s_desc.busy_timeout_ms = (uint32_t)ext[OD_PANEL_EXT_TIMEOUT_S] * 1000u;
        }
    }

    s_aux_pins[0] = d->aux_pin_1;
    s_aux_pins[1] = d->aux_pin_2;
    s_aux_pins[2] = d->aux_pin_3;
    s_aux_pins[3] = d->aux_pin_4;
    s_aux_pins[4] = d->aux_pin_5;
    s_aux_pins[5] = d->aux_pin_6;
    s_named_pins[0] = d->reset_pin;
    s_named_pins[1] = d->dc_pin;
    s_named_pins[2] = d->cs_pin;
    s_named_pins[3] = d->cs_pin_2;
    s_named_pins[4] = d->data_pin;
    s_named_pins[5] = d->clk_pin;
    s_io.set_pin = bus->pin_write != NULL ? io_set_pin : NULL;
    /* Offered only where a distinct second CS is wired, so a split-controller sequence on a
     * single-CS board is refused at validation instead of halfway through a pass. */
    s_io.cs_select = (bus->cs_select != NULL && d->cs_pin_2 != 0xFFu && d->cs_pin_2 != d->cs_pin)
                   ? io_cs_select : NULL;
    s_io.read_bytes = bus->read_bytes != NULL ? io_read_bytes : NULL;

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

bool od_epd_cfg_busy_usable(void)
{
    return s_desc.busy_usable;
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

bool od_epd_cfg_code_map(uint8_t out[4])
{
    if (!s_loaded || out == NULL) {
        return false;
    }
    memcpy(out, s_desc.code_map, sizeof(s_desc.code_map));
    return true;
}

uint8_t od_epd_cfg_mode_count(void)
{
    return s_loaded ? s_desc.mode_count : 0u;
}

bool od_epd_cfg_has_drive_data(const struct DisplayConfig *d)
{
    return d != NULL && od_cfg_has_drive_data(d->instance_number, d->color_scheme);
}

static char lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

int od_epd_cfg_find_mode(const char *name)
{
    if (!s_loaded || name == NULL) {
        return -1;
    }
    for (uint8_t i = 0u; i < s_mode_entries; i++) {
        const od_cfg_entry_t *e = od_cfg_index_entry((int)s_mode_entry[i]);
        char n[12];
        size_t k;

        /* A packet shadowed by an earlier one with the same mode_index is not that mode. */
        if (e == NULL || mode_entry(e->sub) != e ||
            od_cfg_read(e, MODE_OFF_NAME, (uint8_t *)n, sizeof n) != sizeof n) {
            continue;
        }
        for (k = 0u; k < sizeof n; k++) {
            if (lower(n[k]) != lower(name[k])) {
                break;
            }
            if (name[k] == '\0') {
                return (int)e->sub;
            }
        }
        if (k == sizeof n && name[k] == '\0') {
            return (int)e->sub;
        }
    }
    return -1;
}

/* min == max == 0 is the zero-filled "no window declared". */
static bool mode_temp_ok(const od_epd_cfg_mode_t *m, int8_t t)
{
    return (m->temp_min_c == 0 && m->temp_max_c == 0) ||
           (t >= m->temp_min_c && t <= m->temp_max_c);
}

uint8_t od_epd_cfg_select_policy(uint8_t *mode, int8_t temp_c, od_epd_cfg_mode_t *out)
{
    od_epd_cfg_mode_t hdr;

    if (mode == NULL || !od_epd_cfg_select(*mode, &hdr)) {
        return OD_ERR_DRIVE_BAD_MODE;
    }
    if (temp_c != OD_EPD_CFG_TEMP_UNKNOWN && !mode_temp_ok(&hdr, temp_c)) {
        if (hdr.temp_policy == OD_DRIVE_TEMP_REJECT) {
            return OD_ERR_DRIVE_TEMP_RANGE;
        }
        /* SUBSTITUTE falls back to mode 0, the panel's default full refresh, and only when
         * mode 0's own window covers the reading. */
        if (hdr.temp_policy == OD_DRIVE_TEMP_SUBSTITUTE) {
            if (*mode == 0u || !od_epd_cfg_select(0u, &hdr) || !mode_temp_ok(&hdr, temp_c)) {
                return OD_ERR_DRIVE_TEMP_RANGE;
            }
            *mode = 0u;
        }
    }
    if (out != NULL) {
        *out = hdr;
    }
    return 0u;
}

int od_epd_cfg_find_usable_mode(const char *name, int8_t temp_c)
{
    const int m = od_epd_cfg_find_mode(name);
    od_epd_mode_t h;

    if (m < 0 || !io_mode_read((uint8_t)m, &h)) {
        return -1;
    }
    if (temp_c != OD_EPD_CFG_TEMP_UNKNOWN && h.temp_policy != OD_DRIVE_TEMP_ADVISORY &&
        !((h.temp_min_c == 0 && h.temp_max_c == 0) ||
          (temp_c >= h.temp_min_c && temp_c <= h.temp_max_c))) {
        return -1;
    }
    return m;
}

void od_epd_cfg_set_window(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    od_epd_set_window(&s_epd, x, y, w, h);
}

bool od_epd_cfg_finish_with_tail(uint8_t mode)
{
    if (!s_in_pass) {
        return false;
    }
    s_last_rc = od_epd_finish_with_tail(&s_epd, mode);   /* still held: the tail is read from storage */
    set_in_pass(false);
    return s_last_rc == OD_EPD_RC_OK && !bus_faulted();
}

bool od_epd_cfg_begin_pass(uint8_t pass)
{
    if (!s_loaded) {
        s_last_rc = OD_EPD_RC_ARG;
        return false;
    }
    set_in_pass(true);   /* before the walk reads the stored sequence */
    s_last_rc = od_epd_begin_pass(&s_epd, pass);
    set_in_pass(s_last_rc == OD_EPD_RC_OK && !bus_faulted());
    return s_in_pass;
}

void od_epd_cfg_raw_detach(void)
{
    set_in_pass(false);
}

bool od_epd_cfg_write(const uint8_t *data, size_t len)
{
    return s_in_pass && od_epd_write_image_chunk(&s_epd, data, len) == OD_EPD_RC_OK &&
           !bus_faulted();
}

bool od_epd_cfg_next_plane(void)
{
    if (!s_in_pass) {
        return false;
    }
    s_last_rc = od_epd_next_plane(&s_epd);
    if (s_last_rc != OD_EPD_RC_OK) {
        set_in_pass(false);        /* the walk is over; nothing reads the config any more */
    }
    return s_last_rc == OD_EPD_RC_OK;
}

bool od_epd_cfg_finish(void)
{
    if (!s_in_pass) {
        return false;
    }
    s_last_rc = od_epd_finish_pass(&s_epd);   /* still held: the tail is read from storage */
    set_in_pass(false);
    return s_last_rc == OD_EPD_RC_OK && !bus_faulted();
}

void od_epd_cfg_sleep(void)
{
    set_in_pass(false);
    if (s_loaded) {
        (void)od_epd_sleep(&s_epd);
    }
}

int od_epd_cfg_last_rc(void)
{
    return s_last_rc;
}

/* --- multi-pass stream ---------------------------------------------------------------------- */

/* Bytes the marker the walk is paused on takes (see od_epd_cfg_stream_t). */
static uint32_t marker_budget(const od_epd_cfg_stream_t *s)
{
    const uint8_t part = s_epd.img_part;
    const uint8_t of = s_epd.img_of == 0u ? 1u : s_epd.img_of;
    uint32_t unit;
    uint32_t b;

    if (s->layout == OD_COLOR_LAYOUT_SPLIT_HALVES && of == 2u && part < 2u) {
        return s->part_bytes[part];
    }
    unit = (of == 1u && s->plane_size != 0u) ? s->plane_size : s->frame_bytes;
    b = unit / of;
    return part + 1u == of ? unit - b * (uint32_t)(of - 1u) : b;
}

bool od_epd_cfg_inline_expect(uint16_t prefix_len)
{
#if OD_EPD_LUT_CACHE > 0
    s_inline_expect = prefix_len;
    return s_loaded && prefix_len > OD_LUT_INLINE_HDR;
#else
    (void)prefix_len;
    return false;
#endif
}

int od_epd_cfg_check_pass(uint8_t pass)
{
    return s_loaded ? od_epd_check_pass(&s_epd, pass) : OD_EPD_RC_ARG;
}

#if OD_EPD_LUT_CACHE > 0
/* One inline table header or body chunk. Tables are checked as each completes; the pass begins
 * when the whole prefix is in (od_epd_cfg_stream_write). */
static bool inline_feed(od_epd_cfg_stream_t *s, const uint8_t *d, uint32_t n)
{
    while (n > 0u) {
        od_lut_cache_entry_t *c = (od_lut_cache_entry_t *)s->in_entry;

        if (s->in_hdr_n < OD_LUT_INLINE_HDR) {
            s->in_hdr[s->in_hdr_n++] = *d++;
            n--;
            if (s->in_hdr_n < OD_LUT_INLINE_HDR) {
                continue;
            }
            s->in_len = rd16(&s->in_hdr[1]);
            s->in_got = 0u;
            if (s->in_hdr[0] >= OD_LUT_SLOT_MAX || s->in_len == 0u ||
                s->in_len > OD_LUT_MAX_BYTES ||
                od_lut_cache_find(s_instance, s->in_hdr[0], true, 0u) != NULL ||
                (c = od_lut_cache_claim()) == NULL) {
                od_log_error("drive: inline table %u refused", (unsigned)s->in_hdr[0]);
                return false;
            }
            c->used = false;
            c->is_inline = false;
            s->in_entry = c;
            continue;
        }
        {
            uint32_t take = (uint32_t)(s->in_len - s->in_got);

            if (take > n) {
                take = n;
            }
            memcpy(&c->data[s->in_got], d, take);
            s->in_got = (uint16_t)(s->in_got + take);
            d += take;
            n -= take;
        }
        if (s->in_got == s->in_len) {
            if (od_lut_crc16(0xFFFFu, c->data, s->in_len) != rd16(&s->in_hdr[3]) ||
                !od_lut_lint(s_desc.lut_family, c->data, s->in_len)) {
                od_log_error("drive: inline table %u CRC/lint failed", (unsigned)s->in_hdr[0]);
                return false;
            }
            c->used = true;
            c->is_inline = true;
            c->instance = s_instance;
            c->slot = s->in_hdr[0];
            c->len = s->in_len;
            c->gen = 0u;
            s->in_entry = NULL;
            s->in_hdr_n = 0u;
            s->in_tables++;
        }
    }
    return true;
}
#endif

bool od_epd_cfg_stream_init(od_epd_cfg_stream_t *s, const od_color_geometry_t *frame,
                            uint8_t passes, bool compressed, const uint8_t *tail,
                            uint16_t tail_len)
{
    const uint32_t prefix = s_inline_expect;
    const uint64_t total = frame != NULL
                         ? (uint64_t)frame->total_bytes * passes + prefix : 0u;

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
    s_inline_expect = 0u;
    s->prefix_len = prefix;
    s->frame_bytes = frame->total_bytes;
    s->total_bytes = (uint32_t)total;
    s->passes = passes;
    s->layout = (uint8_t)frame->layout;
    s->part_bytes[0] = frame->part_bytes[0];
    s->part_bytes[1] = frame->part_bytes[1];
    s->plane_size = frame->layout == OD_COLOR_LAYOUT_CONTROLLER_PLANES ? frame->part_bytes[0] : 0u;
    s->marker_end = marker_budget(s);
    return true;
}

bool od_epd_cfg_stream_write(od_epd_cfg_stream_t *s, uint32_t stream_offset,
                             const uint8_t *data, size_t len, void (*phase)(bool refreshing))
{
    size_t off = 0u;

    if (s == NULL || data == NULL) {
        return false;
    }
    while (off < len && stream_offset + (uint32_t)off < s->prefix_len) {
#if OD_EPD_LUT_CACHE > 0
        const uint32_t at = stream_offset + (uint32_t)off;
        uint32_t take = s->prefix_len - at;

        if (take > len - off) {
            take = (uint32_t)(len - off);
        }
        if (!inline_feed(s, data + off, take)) {
            return false;
        }
        off += take;
        if (at + take == s->prefix_len) {
            if (s->in_hdr_n != 0u || s->in_tables == 0u) {
                od_log_error("drive: inline prefix ends inside a table");
                return false;
            }
            if (!od_epd_cfg_begin_pass(0u)) {
                od_log_error("drive: pass 0 begin failed after inline tables (rc %d)",
                             od_epd_cfg_last_rc());
                return false;
            }
            /* The budget depends on the marker the walk is paused on, which only exists now. */
            s->marker_end = marker_budget(s);
        }
#else
        return false;
#endif
    }
    if (off < len && stream_offset + (uint32_t)off < s->prefix_len) {
        return false;
    }
    if (off == len) {
        return true;
    }
    stream_offset -= s->prefix_len;
    while (off < len) {
        const uint32_t logical = stream_offset + (uint32_t)off;
        const uint32_t frame_end = (uint32_t)(s->pass + 1u) * s->frame_bytes;
        const uint32_t frame_start = frame_end - s->frame_bytes;
        uint32_t chunk = (uint32_t)(len - off);

        if (logical >= frame_end || logical < frame_start) {
            return false;
        }
        /* The marker's budget is spent but the frame is not: the next bytes belong to the
         * sequence's next image marker (plane 1, slice 1, ...). */
        if (logical == s->marker_end) {
            if (!od_epd_cfg_next_plane()) {
                od_log_error("drive: sequence has no image marker for offset %u",
                             (unsigned)logical);
                return false;
            }
            s->marker_end = logical + marker_budget(s);
        }
        if (s->marker_end > frame_end) {
            s->marker_end = frame_end;
        }
        if (logical > s->marker_end) {
            return false;
        }
        if (chunk > s->marker_end - logical) {
            chunk = s->marker_end - logical;
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
                od_log_error("drive: pass %u refresh failed (rc %d)", (unsigned)s->pass, s_last_rc);
                return false;
            }
            s->pass++;
            if (!od_epd_cfg_begin_pass(s->pass)) {
                od_log_error("drive: pass %u begin failed", (unsigned)s->pass);
                return false;
            }
            s->marker_end = frame_end + marker_budget(s);
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
