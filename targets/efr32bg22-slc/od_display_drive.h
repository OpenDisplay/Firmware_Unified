/* Display-side seam for CMD_DRIVE_WRITE_START (0x0085).
 *
 * Separated from od_cmd_drive.c so the wire half and the panel half can land independently:
 * the wire half validates and shapes the ACK, this decides what the panel actually does.
 *
 * THE DEVICE DECIDES THE MODE, NOT THE HOST. *mode_out is the mode actually selected -- under
 * a mode's temp_policy SUBSTITUTE the device may answer with a different, temperature-legal
 * mode than the one requested -- and *passes_out is read from the CONFIG so a host does not
 * have to duplicate the mode table. Both go into the ACK verbatim.
 */
#ifndef OD_DISPLAY_DRIVE_H
#define OD_DISPLAY_DRIVE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Begin a mode-named full-frame write.
 *
 * mode         index into the config's drive modes (0xAE packets)
 * refresh      accepted at START; the mode already names the waveform
 * flags        DRIVE_WRITE_FLAG_* -- COMPRESSED, and PIPELINED which this target lacks
 * payload/len  the tail after the 5-byte header; the first bytes of the stream
 * passes_out   how many frames the device now expects, from the config
 * mode_out     the mode ACTUALLY selected
 * temp_out     device temperature estimate, int8 degC, 0x7F = unknown
 *
 * Returns 0, or -OD_ERR_DRIVE_* (negated so the caller can hand it straight to a NACK).
 */
int od_display_drive_write_start(uint8_t mode, uint8_t refresh, uint8_t flags,
                                 const uint8_t *payload, uint16_t payload_len,
                                 uint8_t *passes_out, uint8_t *mode_out,
                                 int8_t *temp_out);

#ifdef __cplusplus
}
#endif

#endif /* OD_DISPLAY_DRIVE_H */
