/* The display half of CMD_DRIVE_WRITE_START, as a link-time stub.
 *
 * The WIRE half (targets/efr32bg22-slc/od_cmd_drive.c) is compiled for real into the silabs
 * executables, so header validation, reserved-byte rejection and the ACK/NACK shapes are under
 * test here. Only this seam is faked: the production implementation is C++ and reaches the panel
 * through the display stack, which these C executables do not link.
 *
 * It answers exactly what the target answers today -- OD_ERR_DRIVE_UNSUPPORTED -- so the NACK
 * path a host sees is the path a fixture sees. When the panel half lands, this stub becomes the
 * place to script mode selection and temp_policy outcomes rather than something to delete.
 */
#include "od_caps.h"

#include "opendisplay_protocol.h"

#include <stddef.h>
#include <stdint.h>

#if OD_CAP_DRIVE_WRITE

int od_display_drive_write_start(uint8_t mode, uint8_t refresh, uint8_t flags,
                                 const uint8_t *payload, uint16_t payload_len,
                                 uint8_t *passes_out, uint8_t *mode_out,
                                 int8_t *temp_out)
{
    (void)refresh;
    (void)flags;
    (void)payload;
    (void)payload_len;

    if (passes_out != NULL) {
        *passes_out = 0u;
    }
    if (mode_out != NULL) {
        *mode_out = mode;
    }
    if (temp_out != NULL) {
        *temp_out = (int8_t)0x7F;   /* unknown */
    }
    return -OD_ERR_DRIVE_UNSUPPORTED;
}

#endif
