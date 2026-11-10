/* od_epd -- config-driven e-paper driver. See od_epd.h for the rationale.
 *
 * The whole point of this file is that it contains NO panel knowledge. Search
 * it for a panel name, a register number or a delay and you will not find one:
 * every byte it writes came out of the device config.
 */
#include "od_epd.h"

#include <string.h>

#define OD_OK           OD_EPD_RC_OK
#define OD_ERR_ARG      OD_EPD_RC_ARG
#define OD_ERR_MODE     OD_EPD_RC_MODE
#define OD_ERR_SEQ      OD_EPD_RC_SEQ
#define OD_ERR_LUT      OD_EPD_RC_LUT
#define OD_ERR_BUSY     OD_EPD_RC_BUSY
#define OD_ERR_STATE    OD_EPD_RC_STATE
#define OD_ERR_UNSUP    OD_EPD_RC_UNSUP

/* Streaming chunk for the waveform. 64 B of stack against a 535-byte upload:
 * the alternative is a resident 535-byte buffer, which is 1.7% of this part's
 * RAM held permanently for bytes that are only ever clocked straight out. */
#define LUT_CHUNK 64u

/* ------------------------------------------------------------------ helpers */

bool od_epd_is_busy(od_epd_t *e)
{
    if (!e->desc->busy_usable) {
        return false;
    }
    return e->io->busy_level() != (e->desc->busy_idle_level != 0u);
}

bool od_epd_wait_idle(od_epd_t *e, uint32_t timeout_ms)
{
    /* A panel whose BUSY line lies gets a fixed delay instead of a poll.
     * This is not a workaround bolted on for one panel -- it is a declared
     * per-panel property. The 4.2" GDEM042F52 reports busy immediately after
     * power-up with nothing sent and never releases after a refresh; polling it
     * stalls forever. bb_epaper encoded the same fact as an UNDOCUMENTED local
     * patch (BBEP_SKIP_BUSY_WAIT) to its vendored tree, which is exactly the
     * kind of hidden knowledge this driver exists to make explicit. */
    if (!e->desc->busy_usable) {
        uint32_t d = e->desc->busy_fixed_delay_ms;
        e->io->delay_ms(d ? d : timeout_ms);
        return true;
    }
    uint32_t waited = 0;
    while (od_epd_is_busy(e)) {
        if (waited >= timeout_ms) {
            return false;
        }
        e->io->delay_ms(5);
        waited += 5;
    }
    return true;
}

/* Substitute a byte that the sequence takes from the waveform's analog header.
 * PWRR/PLL/VCOM are properties of the WAVEFORM, not of the init sequence, so a
 * mode that uploads a custom waveform must drive the analog rails that waveform
 * was authored for. Sending a custom waveform with the stock rails plays the
 * right waveform at the wrong voltages -- which does not fail, it renders a
 * plausible-but-wrong image. */
static uint8_t wave_byte(const uint8_t *hdr, bool have_hdr, uint8_t idx)
{
    if (!have_hdr) {
        return 0u;
    }
    if (idx == OD_EPD_WAVE_VCOM) {
        return (uint8_t)(hdr[5] ^ 0x80u);
    }
    return (idx < 7u) ? hdr[idx] : 0u;
}

/* Bytes of escape arguments, -1 for variable (SUBST) or unknown. Shared by the
 * validator and the runtime walk so the two cannot disagree about framing. */
static int esc_arg_bytes(uint8_t esc)
{
    switch (esc) {
        case OD_EPD_SEQ_END:
        case OD_EPD_ESC_BUSY:
        case OD_EPD_ESC_IMAGE:
        case OD_EPD_ESC_LUT:           return 0;
        case OD_EPD_ESC_DELAY:
        case OD_EPD_ESC_SET_PIN:
        case OD_EPD_ESC_IMAGE_SLICE:   return 2;
        case OD_EPD_ESC_CS_SELECT:     return 1;
        case OD_EPD_ESC_READ_SCRATCH:  return 2;
        case OD_EPD_ESC_WRITE_SCRATCH: return 3;
        case OD_EPD_ESC_LUT_SLICE:     return 4;
        case OD_EPD_ESC_FILL:          return 3;
        default:                       return -1;
    }
}

/* Per-sequence state every sequence starts from: an empty scratch and CS 0.
 * CS_SELECT is sticky, so without the reset a pass that ended on cs_pin_2 would
 * route the next pass's first steps to the wrong controller. */
static void seq_start(od_epd_t *e)
{
    memset(e->scratch, 0, sizeof e->scratch);
    if (e->io->cs_select != NULL) {
        (void)e->io->cs_select(0u);
    }
}

int od_epd_validate(const od_epd_t *e)
{
    const uint8_t *seq;
    uint16_t len;
    uint16_t i = 0;
    uint16_t steps = 0;

    if (e == NULL || e->io == NULL) {
        return OD_ERR_ARG;
    }
    seq = e->seq;
    len = e->seq_len;

    while (i < len) {
        if (++steps > OD_EPD_MAX_SEQ_STEPS) {
            return OD_ERR_SEQ;
        }
        uint8_t n = seq[i];
        if (n != OD_EPD_ESC) {
            if ((uint32_t)i + 1u + n > len) {
                return OD_ERR_SEQ;
            }
            i += 1u + n;
            continue;
        }
        if (i + 1u >= len) {
            return OD_ERR_SEQ;
        }
        uint8_t esc = seq[i + 1u];
        if (esc == OD_EPD_SEQ_END) {
            return OD_OK;
        }
        if (esc == OD_EPD_ESC_WAVE) {
            if (i + 2u >= len) {
                return OD_ERR_SEQ;
            }
            uint8_t cnt = seq[i + 2u];
            if (cnt == 0u || cnt > 32u ||
                (uint32_t)i + 3u + (uint32_t)cnt * 2u > len) {
                return OD_ERR_SEQ;
            }
            i += 3u + (uint16_t)cnt * 2u;
            continue;
        }
        if (esc == OD_EPD_ESC_WINDOW) {
#if OD_EPD_WITH_WINDOW
            if (i + 2u >= len) {
                return OD_ERR_SEQ;
            }
            uint8_t cnt = seq[i + 2u];
            if (cnt == 0u || cnt > 32u ||
                (uint32_t)i + 3u + (uint32_t)cnt * 2u > len) {
                return OD_ERR_SEQ;
            }
            for (uint8_t k = 0u; k < cnt; k++) {
                const uint8_t kind = seq[i + 3u + k * 2u];
                const uint8_t v = seq[i + 4u + k * 2u];
                if (kind == OD_EPD_WIN_KIND_GEOM) {
                    if ((v & 0xC0u) != 0u ||
                        ((v & OD_EPD_WIN_MIRROR) != 0u && (v & 0x07u) >= OD_EPD_WIN_WIDTH)) {
                        return OD_ERR_SEQ;
                    }
                } else if (kind != OD_EPD_WIN_KIND_LITERAL) {
                    return OD_ERR_SEQ;
                }
            }
            i += 3u + (uint16_t)cnt * 2u;
            continue;
#else
            return OD_ERR_UNSUP;
#endif
        }
        int args = esc_arg_bytes(esc);
        if (args < 0) {
            /* Unknown escape -- including the reserved 0x80+ length-carrying
             * range, which this executor does not implement either. Refusing
             * the WHOLE sequence here (nothing has touched the panel yet) is
             * the contract behind OD_ERR_DRIVE_SEQ_UNSUPPORTED. */
            return OD_ERR_UNSUP;
        }
        if ((uint32_t)i + 2u + (uint32_t)args > len) {
            return OD_ERR_SEQ;
        }
        /* Known but not implemented by THIS io table. */
        if ((esc == OD_EPD_ESC_SET_PIN && e->io->set_pin == NULL) ||
            (esc == OD_EPD_ESC_CS_SELECT && e->io->cs_select == NULL) ||
            (esc == OD_EPD_ESC_READ_SCRATCH && e->io->read_bytes == NULL)) {
            return OD_ERR_UNSUP;
        }
        if (esc == OD_EPD_ESC_CS_SELECT && seq[i + 2u] > 1u) {
            return OD_ERR_SEQ;      /* 0 = cs_pin, 1 = cs_pin_2 */
        }
        if (esc == OD_EPD_ESC_READ_SCRATCH &&
            (seq[i + 3u] == 0u || seq[i + 3u] > OD_EPD_SCRATCH_LEN)) {
            return OD_ERR_SEQ;
        }
        if (esc == OD_EPD_ESC_WRITE_SCRATCH &&
            (uint32_t)seq[i + 3u] + seq[i + 4u] > OD_EPD_SCRATCH_LEN) {
            return OD_ERR_SEQ;
        }
        if (esc == OD_EPD_ESC_IMAGE_SLICE) {
            uint8_t part = seq[i + 2u];
            uint8_t of = seq[i + 3u];
            if (of == 0u || part >= of) {
                return OD_ERR_SEQ;
            }
        }
        if (esc == OD_EPD_ESC_SET_PIN && seq[i + 3u] > 2u) {
            return OD_ERR_SEQ;      /* level is 0, 1 or 2 (Hi-Z) */
        }
        if (esc == OD_EPD_ESC_FILL) {
#if OD_EPD_WITH_WINDOW
            if ((seq[i + 4u] & 0x03u) > 2u || (seq[i + 4u] & 0x7Cu) != 0u) {
                return OD_ERR_SEQ;
            }
#else
            return OD_ERR_UNSUP;
#endif
        }
        i += 2u + (uint16_t)args;
    }
    /* Running off the end without an END escape is legal -- the runtime walk
     * treats it as end-of-sequence, so the validator must agree with it. */
    return OD_OK;
}

/* Stream the active pass's waveform straight to the panel.
 *
 * vsn1 patching happens here rather than at authoring time because the same
 * waveform slot is shared by more than one pass with DIFFERENT patches: the
 * 16-colour mode's passes 1 and 2 both use the edit waveform but patch byte 3
 * to 0x14 and 0x41 respectively. Storing them pre-patched would cost a second
 * 535-byte copy in the config to hold one differing byte. */
static int stream_lut(od_epd_t *e, uint8_t slot, uint8_t vsn1,
                      uint16_t start, uint16_t len)
{
    uint8_t buf[LUT_CHUNK];
    uint16_t off = start;
    uint16_t end = (uint16_t)(start + len);

    while (off < end) {
        uint16_t want = (uint16_t)((uint16_t)(end - off) < (uint16_t)LUT_CHUNK
                                 ? (uint16_t)(end - off) : (uint16_t)LUT_CHUNK);
        uint16_t got = e->io->lut_read(slot, off, buf, want);
        if (got != want) {
            return OD_ERR_LUT;
        }
        /* The vsn1 patch targets ABSOLUTE slot offset 3 -- also when the slot
         * is streamed in register-sized slices (LUT_SLICE). */
        if (vsn1 != 0u && off <= 3u && (uint16_t)(off + got) > 3u) {
            buf[3u - off] = vsn1;
        }
        e->io->data(buf, (size_t)got);
        off += got;
    }
    return OD_OK;
}

/* Forward-only walk over a length-prefixed sequence.
 *
 * Deliberately NOT a bytecode VM. Sequences arrive in the config, which is
 * parsed pre-auth, so this is untrusted input. There are no jumps, no loops and
 * no addressing: the walk only moves forward, every step is bounded by the
 * sequence length, and the step count is capped. The worst a malformed sequence
 * can do is clock out nonsense and time out -- which the raw-SPI debug command
 * can already do deliberately.
 *
 * Returns OD_OK at end of sequence, 1 when it paused on the image marker, or a
 * negative error. `e->seq_pos` is where to resume.
 */
#if OD_EPD_WITH_WINDOW
static uint16_t win_value(const od_epd_t *e, uint8_t desc)
{
    const uint8_t sel = (uint8_t)(desc & 0x07u);
    uint32_t v;
    uint32_t dim;

    switch (sel) {
        case OD_EPD_WIN_X0:     v = e->win_x; break;
        case OD_EPD_WIN_X1:     v = (uint32_t)e->win_x + e->win_w - 1u; break;
        case OD_EPD_WIN_Y0:     v = e->win_y; break;
        case OD_EPD_WIN_Y1:     v = (uint32_t)e->win_y + e->win_h - 1u; break;
        case OD_EPD_WIN_W_LAST: v = (uint32_t)e->desc->width - 1u; break;
        case OD_EPD_WIN_H_LAST: v = (uint32_t)e->desc->height - 1u; break;
        case OD_EPD_WIN_WIDTH:  v = e->win_w; break;
        default:                v = e->win_h; break;
    }
    if ((desc & OD_EPD_WIN_MIRROR) != 0u) {
        dim = (sel == OD_EPD_WIN_Y0 || sel == OD_EPD_WIN_Y1 || sel == OD_EPD_WIN_H_LAST)
            ? e->desc->height : e->desc->width;
        v = (v < dim) ? dim - 1u - v : 0u;
    }
    if ((desc & OD_EPD_WIN_DIV8) != 0u) {
        v >>= 3;
    }
    return (uint16_t)v;
}

static void run_fill(od_epd_t *e, uint8_t cmd, uint8_t value, uint8_t flags)
{
    static const uint8_t bpp_of[3] = { 1u, 2u, 4u };
    const bool rect = (flags & OD_EPD_FILL_RECT) != 0u;
    const uint32_t w = rect ? e->win_w : e->desc->width;
    const uint32_t h = rect ? e->win_h : e->desc->height;
    uint32_t left = ((w * bpp_of[flags & 0x03u] + 7u) / 8u) * h;
    uint8_t buf[32];

    memset(buf, value, sizeof buf);
    e->io->cmd(cmd);
    while (left > 0u) {
        const uint32_t n = left < sizeof buf ? left : (uint32_t)sizeof buf;
        e->io->data(buf, n);
        left -= n;
    }
}
#endif

static int run_sequence(od_epd_t *e)
{
    const uint8_t *hdr = e->lut_hdr;
    const bool have_hdr = e->lut_have;
    const uint8_t slot = e->lut_slot;
    const uint8_t vsn1 = e->lut_vsn1;
    const uint16_t lut_len = e->lut_len;
    const uint8_t *seq = e->seq;
    uint16_t len = e->seq_len;
    uint16_t i = e->seq_pos;
    uint16_t steps = 0;
    uint8_t  buf[32];

    while (i < len) {
        if (++steps > OD_EPD_MAX_SEQ_STEPS) {
            e->seq_pos = i;
            return OD_ERR_SEQ;
        }
        uint8_t n = seq[i];

        if (n != OD_EPD_ESC) {
            /* plain step: [len][cmd][data...] */
            if ((uint32_t)i + 1u + n > len) {
                return OD_ERR_SEQ;
            }
            e->io->cmd(seq[i + 1u]);
            if (n > 1u) {
                e->io->data(&seq[i + 2u], (size_t)(n - 1u));
            }
            i += 1u + n;
            continue;
        }

        /* escape */
        if (i + 1u >= len) {
            return OD_ERR_SEQ;
        }
        uint8_t esc = seq[i + 1u];

        if (esc == OD_EPD_SEQ_END) {
            e->seq_pos = len;
            return OD_OK;
        }
        if (esc == OD_EPD_ESC_BUSY) {
            if (!od_epd_wait_idle(e, e->desc->busy_timeout_ms)) {
                return OD_ERR_BUSY;
            }
            i += 2u;
            continue;
        }
        if (esc == OD_EPD_ESC_DELAY) {
            if ((uint32_t)i + 4u > len) {
                return OD_ERR_SEQ;
            }
            e->io->delay_ms((uint32_t)seq[i + 2u] | ((uint32_t)seq[i + 3u] << 8));
            i += 4u;
            continue;
        }
        if (esc == OD_EPD_ESC_IMAGE) {
            e->img_part = 0u;
            e->img_of = 1u;
            e->seq_pos = (uint16_t)(i + 2u);
            return 1;            /* caller streams the frame, then resumes */
        }
        if (esc == OD_EPD_ESC_IMAGE_SLICE) {
            if ((uint32_t)i + 4u > len) {
                return OD_ERR_SEQ;
            }
            e->img_part = seq[i + 2u];
            e->img_of = seq[i + 3u];
            e->seq_pos = (uint16_t)(i + 4u);
            return 1;            /* caller streams frame_bytes / img_of bytes */
        }
        if (esc == OD_EPD_ESC_SET_PIN) {
            if ((uint32_t)i + 4u > len) {
                return OD_ERR_SEQ;
            }
            if (e->io->set_pin == NULL ||
                !e->io->set_pin(seq[i + 2u], seq[i + 3u])) {
                /* Validation keeps a NULL hook from ever landing here; a false
                 * return means the pin reference is unwired on this board. */
                return OD_ERR_UNSUP;
            }
            i += 4u;
            continue;
        }
        if (esc == OD_EPD_ESC_CS_SELECT) {
            if ((uint32_t)i + 3u > len) {
                return OD_ERR_SEQ;
            }
            if (seq[i + 2u] > 1u) {
                return OD_ERR_SEQ;
            }
            if (e->io->cs_select == NULL || !e->io->cs_select(seq[i + 2u])) {
                return OD_ERR_UNSUP;
            }
            i += 3u;
            continue;
        }
        if (esc == OD_EPD_ESC_READ_SCRATCH) {
            const uint8_t n_rd = ((uint32_t)i + 4u <= len) ? seq[i + 3u] : 0u;
            if (n_rd == 0u || n_rd > OD_EPD_SCRATCH_LEN) {
                return OD_ERR_SEQ;
            }
            if (e->io->read_bytes == NULL ||
                !e->io->read_bytes(seq[i + 2u], e->scratch, n_rd)) {
                return OD_ERR_UNSUP;
            }
            i += 4u;
            continue;
        }
        if (esc == OD_EPD_ESC_WRITE_SCRATCH) {
            if ((uint32_t)i + 5u > len) {
                return OD_ERR_SEQ;
            }
            const uint8_t off = seq[i + 3u];
            const uint8_t n_wr = seq[i + 4u];
            if ((uint32_t)off + n_wr > OD_EPD_SCRATCH_LEN) {
                return OD_ERR_SEQ;
            }
            e->io->cmd(seq[i + 2u]);
            if (n_wr > 0u) {
                e->io->data(&e->scratch[off], n_wr);
            }
            i += 5u;
            continue;
        }
        if (esc == OD_EPD_ESC_LUT) {
            /* Loud, not silent: a mode whose sequence uploads a waveform but
             * whose pass resolved no slot would otherwise refresh with whatever
             * the panel had in its waveform register. */
            if (!have_hdr) {
                return OD_ERR_LUT;
            }
            int rc = stream_lut(e, slot, vsn1, 0u, lut_len);
            if (rc != OD_OK) {
                return rc;
            }
            i += 2u;
            continue;
        }
        if (esc == OD_EPD_ESC_LUT_SLICE) {
            if ((uint32_t)i + 6u > len) {
                return OD_ERR_SEQ;
            }
            uint16_t s_off = (uint16_t)(seq[i + 2u] | ((uint16_t)seq[i + 3u] << 8));
            uint16_t s_len = (uint16_t)(seq[i + 4u] | ((uint16_t)seq[i + 5u] << 8));
            if (!have_hdr) {
                return OD_ERR_LUT;
            }
            if ((uint32_t)s_off + s_len > lut_len) {
                return OD_ERR_SEQ;   /* slice runs past the stored slot */
            }
            int rc = stream_lut(e, slot, vsn1, s_off, s_len);
            if (rc != OD_OK) {
                return rc;
            }
            i += 6u;
            continue;
        }
#if OD_EPD_WITH_WINDOW
        if (esc == OD_EPD_ESC_FILL) {
            if ((uint32_t)i + 5u > len) {
                return OD_ERR_SEQ;
            }
            if ((seq[i + 4u] & 0x03u) > 2u) {
                return OD_ERR_SEQ;  /* bpp code indexes bpp_of[3] */
            }
            run_fill(e, seq[i + 2u], seq[i + 3u], seq[i + 4u]);
            i += 5u;
            continue;
        }
        if (esc == OD_EPD_ESC_WINDOW) {
            if (i + 2u >= len) {
                return OD_ERR_SEQ;
            }
            uint8_t cnt = seq[i + 2u];
            if (cnt == 0u || cnt > sizeof(buf) ||
                (uint32_t)i + 3u + (uint32_t)cnt * 2u > len) {
                return OD_ERR_SEQ;
            }
            const uint8_t *p = &seq[i + 3u];
            for (uint8_t k = 0; k < cnt; k++) {
                if (p[k * 2u] == OD_EPD_WIN_KIND_GEOM) {
                    const uint16_t v = win_value(e, p[k * 2u + 1u]);
                    buf[k] = (uint8_t)(((p[k * 2u + 1u] & OD_EPD_WIN_HIGH) != 0u) ? (v >> 8) : v);
                } else {
                    buf[k] = p[k * 2u + 1u];
                }
            }
            e->io->cmd(buf[0]);
            if (cnt > 1u) {
                e->io->data(&buf[1], (size_t)(cnt - 1u));
            }
            i += 3u + (uint16_t)cnt * 2u;
            continue;
        }
#endif
        if (esc == OD_EPD_ESC_WAVE) {
            if (i + 2u >= len) {
                return OD_ERR_SEQ;
            }
            uint8_t cnt = seq[i + 2u];
            if (cnt == 0u || cnt > sizeof(buf) ||
                (uint32_t)i + 3u + (uint32_t)cnt * 2u > len) {
                return OD_ERR_SEQ;
            }
            const uint8_t *p = &seq[i + 3u];
            for (uint8_t k = 0; k < cnt; k++) {
                buf[k] = (p[k * 2u] == 0xFFu)
                       ? wave_byte(hdr, have_hdr, p[k * 2u + 1u])
                       : p[k * 2u + 1u];
            }
            e->io->cmd(buf[0]);
            if (cnt > 1u) {
                e->io->data(&buf[1], (size_t)(cnt - 1u));
            }
            i += 3u + (uint16_t)cnt * 2u;
            continue;
        }
        return OD_ERR_SEQ;
    }
    e->seq_pos = len;
    return OD_OK;
}

/* -------------------------------------------------------------------- API */

int od_epd_init(od_epd_t *e, const od_panel_desc_t *d, const od_epd_io_t *io)
{
    if (!e || !d || !io || !io->cmd || !io->data || !io->reset || !io->delay_ms ||
        !io->busy_level || !io->seq_read || !io->mode_read) {
        return OD_ERR_ARG;
    }
    memset(e, 0, sizeof(*e));
    e->desc = d;
    e->io = io;
    return OD_OK;
}

int od_epd_select_mode(od_epd_t *e, uint8_t mode)
{
    if (!e || mode >= e->desc->mode_count || !e->io->mode_read) {
        return OD_ERR_MODE;
    }
    if (!e->io->mode_read(mode, &e->active)) {
        return OD_ERR_MODE;
    }
    e->mode = mode;
    e->pass = 0;
    e->paused = false;
    od_epd_set_window(e, 0u, 0u, e->desc->width, e->desc->height);
    return OD_OK;
}

/* Resolve a pass's waveform. Only the 7-byte analog header is read into RAM -- the body is
 * streamed later, at the marker. An absent slot is not an error here; a LUT step that needs
 * one is (run_sequence). */
static int resolve_lut(od_epd_t *e, uint8_t slot, uint8_t vsn1)
{
    uint16_t lut_len = 0u;

    e->lut_have = false;
    e->lut_slot = slot;
    e->lut_vsn1 = vsn1;
    e->lut_len = 0u;
    if (slot != 0xFFu && e->io->lut_info && e->io->lut_read &&
        e->io->lut_info(slot, &lut_len)) {
        if (lut_len < 7u) {
            return OD_ERR_LUT;   /* too short to carry an analog header */
        }
        if (e->io->lut_read(slot, 0u, e->lut_hdr, 7u) != 7u) {
            return OD_ERR_LUT;
        }
        if (vsn1 != 0u) {
            e->lut_hdr[3] = vsn1;
        }
        e->lut_len = lut_len;
        e->lut_have = true;
    }
    return OD_OK;
}

void od_epd_set_window(od_epd_t *e, uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    if (e == NULL) {
        return;
    }
    e->win_x = x;
    e->win_y = y;
    e->win_w = w;
    e->win_h = h;
}

#if OD_EPD_WITH_WINDOW
/* A validated sequence only. A rectangle smaller than the panel can only be addressed by a
 * sequence that places the RAM window itself; one without WINDOW would write the rectangle's
 * bytes from the panel origin. */
static bool seq_uses_window(const od_epd_t *e)
{
    uint16_t i = 0u;

    while ((uint32_t)i + 1u < e->seq_len) {
        const uint8_t n = e->seq[i];
        uint8_t esc;
        int args;

        if (n != OD_EPD_ESC) {
            i = (uint16_t)(i + 1u + n);
            continue;
        }
        esc = e->seq[i + 1u];
        if (esc == OD_EPD_SEQ_END) {
            return false;
        }
        if (esc == OD_EPD_ESC_WINDOW) {
            return true;
        }
        if (esc == OD_EPD_ESC_WAVE) {
            i = (uint16_t)(i + 3u + (uint16_t)e->seq[i + 2u] * 2u);
            continue;
        }
        args = esc_arg_bytes(esc);
        if (args < 0) {
            return false;
        }
        i = (uint16_t)(i + 2u + (uint16_t)args);
    }
    return false;
}

static bool window_is_panel(const od_epd_t *e)
{
    return e->win_x == 0u && e->win_y == 0u &&
           e->win_w == e->desc->width && e->win_h == e->desc->height;
}
#endif

int od_epd_begin_pass(od_epd_t *e, uint8_t pass)
{
    const od_epd_mode_t *m = &e->active;

    if (pass >= m->passes) {
        return OD_ERR_MODE;
    }
    if (m->seq_len == 0u || m->seq_len > OD_EPD_SEQ_MAX) {
        return OD_ERR_SEQ;
    }
    e->pass = pass;
    e->paused = false;

    /* Pull this pass's recipe out of NVM3. One read, 128 B max. */
    e->seq_len = e->io->seq_read(e->mode, 0u, e->seq, m->seq_len);
    if (e->seq_len != m->seq_len) {
        return OD_ERR_SEQ;
    }
    e->seq_pos = 0u;

    /* Capability + structure pre-scan BEFORE any panel I/O. A sequence using an
     * escape this target does not implement is refused wholesale here -- no
     * reset, no power, nothing half-executed (wire: SEQ_UNSUPPORTED). */
    {
        int vrc = od_epd_validate(e);
        if (vrc != OD_OK) {
            return vrc;
        }
    }
#if OD_EPD_WITH_WINDOW
    if (!window_is_panel(e) && !seq_uses_window(e)) {
        return OD_ERR_UNSUP;
    }
#endif
    seq_start(e);
    {
        int lrc = resolve_lut(e, m->lut_slot[pass], m->vsn1[pass]);
        if (lrc != OD_OK) {
            return lrc;
        }
    }

    e->io->reset(10);
    if (!od_epd_wait_idle(e, e->desc->busy_timeout_ms)) {
        return OD_ERR_BUSY;
    }

    int rc = run_sequence(e);
    if (rc < 0) {
        return rc;
    }
    if (rc == 1) {
        e->paused = true;        /* stopped on the image marker, as expected */
        return OD_OK;
    }
    /* A sequence with no image marker is a programming error in the config,
     * not something to paper over: the frame would have nowhere to go. */
    return OD_ERR_SEQ;
}

int od_epd_write_image_chunk(od_epd_t *e, const uint8_t *buf, size_t n)
{
    if (!e || (!buf && n)) {
        return OD_ERR_ARG;
    }
    if (!e->paused) {
        return OD_ERR_STATE;
    }
    if (n) {
        e->io->data(buf, n);   /* streamed; never buffered */
    }
    return OD_OK;
}

int od_epd_next_plane(od_epd_t *e)
{
    /* Panels that take two 1bpp planes (3-colour black/white/red) have a second
     * image command between them -- bb_epaper spelled this startWrite(PLANE_1).
     * Here it is a second image marker in the same sequence, so the register
     * that opens plane 1 is config data like every other byte.
     *
     * No panel in this branch uses it: the three BWRY panels are single-plane
     * 2bpp. It exists so a 3-colour panel is EXPRESSIBLE in a sequence rather
     * than blocked on a driver change. */
    if (!e || !e->paused) {
        return OD_ERR_STATE;
    }
    e->paused = false;
    int rc = run_sequence(e);
    if (rc < 0) {
        return rc;
    }
    if (rc != 1) {
        return OD_ERR_SEQ;      /* caller wanted another plane; sequence has none */
    }
    e->paused = true;
    return OD_OK;
}

int od_epd_finish_pass(od_epd_t *e)
{
    if (!e || !e->paused) {
        return OD_ERR_STATE;
    }
    e->paused = false;
    /* The tail -- power-on, refresh opcode, BUSY wait -- is the remainder of
     * the same sequence. A panel needing a different refresh command or an
     * extra register poke after the frame needs no code change here. */
    int rc = run_sequence(e);
    return (rc < 0) ? rc : OD_OK;
}

/* Offset just past the LAST image marker of a validated sequence, or 0 when it has none. */
static uint16_t after_last_image(const uint8_t *seq, uint16_t len)
{
    uint16_t i = 0u;
    uint16_t after = 0u;

    while (i < len) {
        const uint8_t n = seq[i];
        if (n != OD_EPD_ESC) {
            i = (uint16_t)(i + 1u + n);
            continue;
        }
        if (i + 1u >= len) {
            break;
        }
        const uint8_t esc = seq[i + 1u];
        if (esc == OD_EPD_SEQ_END) {
            break;
        }
        if (esc == OD_EPD_ESC_WAVE || esc == OD_EPD_ESC_WINDOW) {
            i = (uint16_t)(i + 3u + (uint16_t)seq[i + 2u] * 2u);
            continue;
        }
        i = (uint16_t)(i + 2u + (uint16_t)esc_arg_bytes(esc));
        if (esc == OD_EPD_ESC_IMAGE || esc == OD_EPD_ESC_IMAGE_SLICE) {
            after = i;
        }
    }
    return after;
}

int od_epd_finish_with_tail(od_epd_t *e, uint8_t mode)
{
    od_epd_mode_t m;
    uint16_t at;
    int rc;

    if (!e || !e->paused) {
        return OD_ERR_STATE;
    }
    if (mode >= e->desc->mode_count || !e->io->mode_read(mode, &m) ||
        m.seq_len == 0u || m.seq_len > OD_EPD_SEQ_MAX) {
        return OD_ERR_MODE;
    }
    e->paused = false;
    e->seq_len = e->io->seq_read(mode, 0u, e->seq, m.seq_len);
    if (e->seq_len != m.seq_len) {
        return OD_ERR_SEQ;
    }
    rc = od_epd_validate(e);
    if (rc != OD_OK) {
        return rc;
    }
    at = after_last_image(e->seq, e->seq_len);
    if (at == 0u) {
        return OD_ERR_SEQ;      /* a tail is defined by the image marker it follows */
    }
    rc = resolve_lut(e, m.lut_slot[0], m.vsn1[0]);
    if (rc != OD_OK) {
        return rc;
    }
    e->seq_pos = at;
    rc = run_sequence(e);
    if (rc == 1) {
        return OD_ERR_SEQ;      /* only the last marker's tail may run here */
    }
    return (rc < 0) ? rc : OD_OK;
}

int od_epd_sleep(od_epd_t *e)
{
    if (!e || e->desc->sleep_seq_len == 0u) {
        return OD_OK;            /* panel declares no sleep sequence */
    }
    if (e->desc->sleep_seq_len > OD_EPD_SLEEP_SEQ_MAX) {
        return OD_ERR_SEQ;
    }
    memcpy(e->seq, e->desc->sleep_seq, e->desc->sleep_seq_len);
    e->seq_len = e->desc->sleep_seq_len;
    e->seq_pos = 0u;
    e->paused = false;
    e->lut_have = false;
    /* The sleep sequence is config data like any mode sequence: same validator, before any
     * panel I/O. run_sequence() relies on it (FILL's bpp code, CS_SELECT's range). */
    int rc = od_epd_validate(e);
    if (rc != OD_OK) {
        return rc;
    }
    seq_start(e);
    rc = run_sequence(e);
    if (rc == 1) {
        return OD_ERR_SEQ;      /* an image marker has no frame to take here */
    }
    return (rc < 0) ? rc : OD_OK;
}
