/* The display half of CMD_DRIVE_WRITE_START, as a link-time stub.
 *
 * The WIRE half (targets/efr32bg22-slc/od_cmd_drive.c) is compiled for real into the silabs
 * executables, so header validation, reserved-byte rejection, the ACK/NACK shapes and the
 * hand-off to od_xfer_adopt_full are all under test. Only this seam is faked: the production
 * implementation is C++ and reaches the panel through the display stack and em_emu's die
 * sensor, none of which these C executables link.
 *
 * IT MODELS A SUCCESSFUL THREE-PASS START, because that is the case the opcode exists for. A
 * stub that kept answering OD_ERR_DRIVE_UNSUPPORTED -- which is what the target did before the
 * panel half landed -- would leave every byte after the NACK untested while still linking, and
 * PIPELINED is then the only refusal the wire half can still be seen to make on its own.
 */
#include "od_caps.h"

#include "od_color.h"
#include "opendisplay_protocol.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#if OD_CAP_DRIVE_WRITE

/* 168 x 384 at 2bpp is the 2.9in BWRY frame, and 3 passes is its colour16 mode -- the exact
 * shape the multi-pass path was written for. */
#define STUB_FRAME_BYTES 8064u
#define STUB_PASSES      3u

static bool s_started;

/* Visible so a test can assert the handler reported what the mode declared. */
uint8_t od_test_drive_last_mode;

int od_display_drive_write_start(uint8_t mode, uint8_t refresh, uint8_t flags,
                                 const uint8_t *payload, uint16_t payload_len,
                                 uint8_t *passes_out, uint8_t *mode_out,
                                 int8_t *temp_out)
{
    (void)refresh;
    (void)payload;
    (void)payload_len;

    /* Kept real: this target genuinely has no windowed path, so the refusal is the production
     * answer rather than a stub artefact. */
    if ((flags & DRIVE_WRITE_FLAG_PIPELINED) != 0u) {
        return -OD_ERR_DRIVE_UNSUPPORTED;
    }

    s_started = true;
    od_test_drive_last_mode = mode;
    if (passes_out != NULL) {
        *passes_out = STUB_PASSES;
    }
    if (mode_out != NULL) {
        *mode_out = mode;
    }
    if (temp_out != NULL) {
        /* A real value, not 0x7F: the ACK carries it through as a byte, so an unknown reading
         * would hide a mangled sign or a dropped field. */
        *temp_out = (int8_t)25;
    }
    return 0;
}

bool opendisplay_display_drive_stream_info(od_color_geometry_t *geometry_out,
                                           uint32_t *expected_out)
{
    if (!s_started) {
        return false;
    }
    if (geometry_out != NULL) {
        memset(geometry_out, 0, sizeof *geometry_out);
        geometry_out->layout = OD_COLOR_LAYOUT_PACKED_ROWS;
        geometry_out->bits_per_pixel = 2u;
        geometry_out->part_count = 1u;
        geometry_out->part_width[0] = 168u;
        geometry_out->row_bytes[0] = 42u;
        geometry_out->part_bytes[0] = STUB_FRAME_BYTES;
        /* passes x frame, as the real adopt reports it. */
        geometry_out->total_bytes = STUB_FRAME_BYTES * STUB_PASSES;
    }
    if (expected_out != NULL) {
        *expected_out = STUB_FRAME_BYTES * STUB_PASSES;
    }
    return true;
}

#endif
