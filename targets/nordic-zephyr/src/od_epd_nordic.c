/* See od_epd_nordic.h. The packet offsets below are asserted against the wire structs. */
#include "od_epd_nordic.h"

#include "od_cfg_index.h"
#include "od_epd.h"
#include "od_epd_spi.h"
#include "od_gpio.h"
#include "od_log.h"
#include "od_zephyr_compat.h"
#include "opendisplay_structs.h"

#include <stddef.h>
#include <string.h>
#include <zephyr/sys/util.h>

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

#define PIN_UNUSED 0xFFu

static od_epd_t s_epd;
static od_panel_desc_t s_desc;
static bool s_loaded;
static bool s_in_pass;
static int s_last_rc;
static uint8_t s_dc = PIN_UNUSED;
static uint8_t s_cs = PIN_UNUSED;
static uint8_t s_rst = PIN_UNUSED;
static uint8_t s_busy = PIN_UNUSED;
static uint8_t s_mode_entry[OD_CFG_INDEX_MAX];

static uint16_t rd16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static void pin_write(uint8_t pin, bool high)
{
    if (pin != PIN_UNUSED) {
        od_gpio_write(pin, high);
    }
}

static void io_cmd(uint8_t c)
{
    pin_write(s_dc, false);
    pin_write(s_cs, false);
    (void)od_epd_spi_write(&c, 1u);
    pin_write(s_cs, true);
    pin_write(s_dc, true);
}

static void io_data(const uint8_t *d, size_t n)
{
    if (d == NULL || n == 0u) {
        return;
    }
    pin_write(s_cs, false);
    (void)od_epd_spi_write(d, n);
    pin_write(s_cs, true);
}

static void io_reset(uint32_t hold_ms)
{
    const int32_t ms = hold_ms == 0u ? 1 : (int32_t)hold_ms;

    if (s_rst == PIN_UNUSED) {
        return;
    }
    od_gpio_write(s_rst, true);
    od_msleep(ms);
    od_gpio_write(s_rst, false);
    od_msleep(ms);
    od_gpio_write(s_rst, true);
    od_msleep(ms);
}

static bool io_busy_level(void)
{
    if (s_busy == PIN_UNUSED) {
        return s_desc.busy_idle_level != 0u;
    }
    return od_gpio_read(s_busy) != 0;
}

static void io_delay_ms(uint32_t ms)
{
    od_msleep((int32_t)ms);
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
        const uint16_t chunk = (uint16_t)MIN(len - off, (int)sizeof(buf));

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

bool od_epd_nordic_load(const struct DisplayConfig *d)
{
    const od_cfg_entry_t *panel = NULL;
    uint8_t body[PANEL_OFF_SLEEPSEQ + OD_EPD_SLEEP_SEQ_MAX];
    uint16_t got;

    s_loaded = false;
    s_in_pass = false;
    if (d == NULL || od_cfg_index_build() < 0) {
        return false;
    }
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
        return false;
    }
    memset(body, 0, sizeof(body));
    got = (uint16_t)MIN(panel->len, (uint16_t)sizeof(body));
    if (od_cfg_read(panel, 0u, body, got) != got) {
        return false;
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
        return false;
    }
    s_loaded = true;
    return true;
}

void od_epd_nordic_claim_pins(const struct DisplayConfig *d)
{
    const bool idle_high = s_desc.busy_idle_level != 0u;

    s_dc = d->dc_pin;
    s_cs = d->cs_pin;
    s_rst = d->reset_pin;
    s_busy = d->busy_pin;
    if (s_dc != PIN_UNUSED) {
        od_gpio_configure_output(s_dc, true);
    }
    if (s_cs != PIN_UNUSED) {
        od_gpio_configure_output(s_cs, true);
    }
    if (s_rst != PIN_UNUSED) {
        od_gpio_configure_output(s_rst, true);
    }
    if (s_busy != PIN_UNUSED) {
        od_gpio_configure_input(s_busy, idle_high, !idle_high);
    }
}

bool od_epd_nordic_select(uint8_t mode, od_epd_nordic_mode_t *out)
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

uint8_t od_epd_nordic_mode_count(void)
{
    return s_loaded ? s_desc.mode_count : 0u;
}

bool od_epd_nordic_begin_pass(uint8_t pass)
{
    s_last_rc = od_epd_begin_pass(&s_epd, pass);
    s_in_pass = s_last_rc == OD_EPD_RC_OK && !od_epd_spi_faulted();
    return s_in_pass;
}

bool od_epd_nordic_write(const uint8_t *data, size_t len)
{
    return s_in_pass && od_epd_write_image_chunk(&s_epd, data, len) == OD_EPD_RC_OK &&
           !od_epd_spi_faulted();
}

bool od_epd_nordic_next_plane(void)
{
    return s_in_pass && od_epd_next_plane(&s_epd) == OD_EPD_RC_OK;
}

bool od_epd_nordic_finish(void)
{
    if (!s_in_pass) {
        return false;
    }
    s_in_pass = false;
    return od_epd_finish_pass(&s_epd) == OD_EPD_RC_OK && !od_epd_spi_faulted();
}

void od_epd_nordic_sleep(void)
{
    s_in_pass = false;
    if (s_loaded) {
        (void)od_epd_sleep(&s_epd);
    }
}

int od_epd_nordic_last_rc(void)
{
    return s_last_rc;
}
