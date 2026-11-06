/* Display half of CMD_DRIVE_WRITE_START -- see od_display_drive.h.
 *
 * NOT IMPLEMENTED YET, AND SAYS SO ON THE WIRE. The dispatch half (od_cmd_drive.c) is in
 * place, so the device answers OD_ERR_DRIVE_UNSUPPORTED rather than the unknown-opcode NACK.
 * Those are different claims and only this one is true: the opcode is understood, the panel
 * path cannot yet honour it.
 *
 * WHAT IS MISSING, so the next change knows its own shape. Firmware_Silabs implements this on
 * feat/od-epd-driver and the port needs three things this target does not have:
 *
 *   1. Pass count from the mode. opendisplay_display.cpp hardcodes `is_gray4 ? 2 : 1`;
 *      OdEpdAdapter::passCount() already exists and is called by nothing.
 *   2. Plane N routed to pass N. Today planes are DERIVED locally by bit-splitting one buffer
 *      (make_gray4_plane_row). A 16-colour mode needs three frames whose pixel codes genuinely
 *      differ -- palette[cell][pass] -- streamed from the host, because the palette is
 *      panel-specific and still has an open yellow/red conflict on entries 2/3. It must not be
 *      frozen into firmware.
 *   3. temp_policy enforcement between mode selection and the first pass: REJECT, SUBSTITUTE
 *      (answer with a different temperature-legal mode) and ADVISORY.
 *
 * Flash budget is the other reason this is staged: the BG22 app slot is at 242,588 of 253,952
 * bytes and the drive path plus temp policy may not fit as-is.
 */
#include "od_display_drive.h"

#include "od_caps.h"
#include "opendisplay_protocol.h"

#if OD_CAP_DRIVE_WRITE

extern "C" int od_display_drive_write_start(uint8_t mode, uint8_t refresh, uint8_t flags,
                                            const uint8_t *payload, uint16_t payload_len,
                                            uint8_t *passes_out, uint8_t *mode_out,
                                            int8_t *temp_out)
{
    (void)mode;
    (void)refresh;
    (void)payload;
    (void)payload_len;

    /* PIPELINED is a Firmware-target feature and this target has no windowed path, so it is
     * refused specifically rather than lumped in with "not implemented". That distinction
     * survives the rest of this function being a stub. */
    if ((flags & DRIVE_WRITE_FLAG_PIPELINED) != 0u) {
        return -OD_ERR_DRIVE_UNSUPPORTED;
    }

    if (passes_out != nullptr) {
        *passes_out = 0u;
    }
    if (mode_out != nullptr) {
        *mode_out = mode;
    }
    if (temp_out != nullptr) {
        *temp_out = (int8_t)0x7F;   /* unknown */
    }
    return -OD_ERR_DRIVE_UNSUPPORTED;
}

#endif /* OD_CAP_DRIVE_WRITE */
