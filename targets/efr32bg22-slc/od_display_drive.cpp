/* Display half of CMD_DRIVE_WRITE_START -- see od_display_drive.h.
 *
 * Selects a config-declared drive mode, enforces that mode's temp_policy, and powers the glass
 * for pass 0. The pass SEQUENCING lives in opendisplay_display.cpp with the rest of the transfer
 * state, because a pass boundary is a byte offset in the incoming stream.
 *
 * ONE PASS IS ONE REFRESH, not one plane. A 16-colour BWRY frame is driven three times with
 * different pixel codes and a different waveform each time (measured 20637 / 1823 / 1823 ms on
 * the 2.9in), so each pass is begin -> stream a whole frame -> finish, where finish runs the
 * panel's own refresh and blocks. That is deliberate and it is also the flow control: the host
 * cannot usefully send pass N+1 until pass N is off the glass, so the late ACK IS the signal.
 * Hosts are already sized for it -- a pass boundary can land on ANY chunk, so their per-chunk
 * ack bound has to be the wide one (120 s in render16.py).
 */
#include "od_display_drive.h"

#include "od_caps.h"
#include "od_color.h"
#include "od_epd.h"
#include "od_epd_shim.h"
#include "od_xfer.h"
#include <stdio.h>
#include "opendisplay_config_parser.h"
#include "opendisplay_display.h"
#include "opendisplay_protocol.h"
#include "opendisplay_structs.h"

#include "em_emu.h"

#if OD_CAP_DRIVE_WRITE

/* The EFR32 die sensor, which is what the 0x0085 ACK's temp_c reports. It is the chip, not the
 * glass -- on the 2.9in it read 27-29 C against 25 C chamber air -- so it is an estimate for
 * policy purposes and the protocol says as much. 0x7F means unknown. */
static int8_t drive_temp_c(void)
{
    const float t = EMU_TemperatureGet();

    if (t < -128.0f || t > 126.0f) {
        return (int8_t)0x7F;
    }
    return (int8_t)(t + (t >= 0.0f ? 0.5f : -0.5f));
}

static bool mode_temp_ok(const od_epd_shim_mode_t &m, int8_t t)
{
    /* min == max == 0 is "no window declared", which is the zero-filled state of every config
     * written before temperature enforcement existed. Treating it as a 0..0 window would refuse
     * every such device at room temperature. */
    if (m.temp_min_c == 0 && m.temp_max_c == 0) {
        return true;
    }
    return t >= m.temp_min_c && t <= m.temp_max_c;
}

extern "C" int od_display_drive_write_start(uint8_t mode, uint8_t refresh, uint8_t flags,
                                            const uint8_t *payload, uint16_t payload_len,
                                            uint8_t *passes_out, uint8_t *mode_out,
                                            int8_t *temp_out)
{
    const struct DisplayConfig *d = opendisplay_display_config();
    od_epd_shim_mode_t hdr;
    od_color_geometry_t geom;
    int8_t temp_c;

    /* PIPELINED is a Firmware-target feature and this target has no windowed path, so it is
     * refused specifically rather than lumped in with a generic failure. */
    if ((flags & DRIVE_WRITE_FLAG_PIPELINED) != 0u) {
        return -OD_ERR_DRIVE_UNSUPPORTED;
    }
    if (d == nullptr || !od_epd_shim_available(d)) {
        /* No drive packets means no init sequence, no pin sense and no colour map. There is
         * nothing to fall back to -- bb_epaper cannot run a config-declared mode. */
        return -OD_ERR_DRIVE_NO_CONFIG;
    }

    /* Takes ownership: a drive session and a direct write share one panel object. The transfer
     * that held it ends here too, or a refused START leaves it active with no panel. */
    od_xfer_replace_active();
    opendisplay_display_abort();

    temp_c = drive_temp_c();
    if (temp_out != nullptr) {
        *temp_out = temp_c;
    }
    /* `refresh` is accepted at START, where it could still have mattered, but the mode already
     * names the waveform so there is nothing left for it to select. Logged rather than silently
     * dropped so a host sending it can see it had no effect. */
    if (refresh != 0u) {
        printf("[OD] drive refresh=%u ignored, mode %u names the waveform\r\n",
               (unsigned)refresh, (unsigned)mode);
    }

    opendisplay_display_power(true);
    if (!od_epd_shim_drive_begin(d->instance_number, mode, d->rotation,
                                 d->dc_pin, d->reset_pin, d->busy_pin, d->cs_pin,
                                 d->data_pin, d->clk_pin, &hdr)) {
        opendisplay_display_power(false);
        return -OD_ERR_DRIVE_BAD_MODE;
    }

    /* TEMP POLICY SITS HERE, between selection and powering the glass: selecting the mode is
     * what loaded its window, and under SUBSTITUTE the mode that runs is not the one asked for.
     * The ACK reports whichever ran, which is why a host must read the mode back. */
    if (temp_c != (int8_t)0x7F && !mode_temp_ok(hdr, temp_c)) {
        if (hdr.temp_policy == OD_DRIVE_TEMP_REJECT) {
            printf("[OD] drive mode %u rejected at %d C (window %d..%d)\r\n",
                   (unsigned)mode, (int)temp_c, (int)hdr.temp_min_c, (int)hdr.temp_max_c);
            opendisplay_display_power(false);
            return -OD_ERR_DRIVE_TEMP_RANGE;
        }
        /* SUBSTITUTE falls back to mode 0, the panel's default full refresh, and only when
         * mode 0's own window covers the reading. */
        if (hdr.temp_policy == OD_DRIVE_TEMP_SUBSTITUTE) {
            if (mode == 0u || !od_epd_shim_reselect(0u, &hdr) || !mode_temp_ok(hdr, temp_c)) {
                printf("[OD] drive: mode 0 does not cover %d C\r\n", (int)temp_c);
                opendisplay_display_power(false);
                return -OD_ERR_DRIVE_TEMP_RANGE;
            }
            printf("[OD] drive %d C: mode 0 substituted for %u\r\n", (int)temp_c,
                   (unsigned)mode);
            mode = 0u;
        }
        /* ADVISORY: run as asked. The window is documentation, not a gate. */
    }

    /* The frame geometry is the MODE's scheme when it declares one (BWRY16 for a 16-colour
     * mode), else the panel's. Both pack identically, so this decides nothing about bytes -- it
     * exists so a mode can describe what it renders without restating the panel. */
    if (od_color_direct_geometry(hdr.color_scheme == 0xFFu ? d->color_scheme : hdr.color_scheme,
                                 d->pixel_width, d->pixel_height, &geom) != OD_COLOR_OK) {
        opendisplay_display_power(false);
        return -OD_ERR_DRIVE_NO_CONFIG;
    }

    if (!od_epd_shim_begin_pass(0u)) {
        const int rc = od_epd_shim_last_rc();
        opendisplay_display_power(false);
        /* A sequence using an escape this target does not implement is its own error, so a host
         * can tell "I cannot run this waveform" from "your mode index is wrong". */
        return (rc == OD_EPD_RC_UNSUP) ? -OD_ERR_DRIVE_SEQ_UNSUPPORTED : -OD_ERR_DRIVE_NO_CONFIG;
    }

    if (!opendisplay_display_drive_adopt(&geom, hdr.passes, mode,
                                         (flags & DRIVE_WRITE_FLAG_COMPRESSED) != 0u,
                                         payload, payload_len)) {
        opendisplay_display_abort();
        return -OD_ERR_DRIVE_BAD_SIZE;
    }

    if (passes_out != nullptr) {
        *passes_out = hdr.passes;
    }
    if (mode_out != nullptr) {
        *mode_out = mode;
    }
    printf("[OD] drive mode=%u passes=%u frame=%lu total=%lu temp=%d\r\n",
           (unsigned)mode, (unsigned)hdr.passes, (unsigned long)geom.total_bytes,
           (unsigned long)(geom.total_bytes * hdr.passes), (int)temp_c);
    return 0;
}

#endif /* OD_CAP_DRIVE_WRITE */
