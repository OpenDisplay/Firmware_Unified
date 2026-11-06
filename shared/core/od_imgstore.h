/* od_imgstore -- fixed slots of panel-ready image data in SPI NOR.
 *
 * NO BUFFER. A 2.6" BWRY frame is 16560 bytes and this part has 480 bytes of RAM to spare, so
 * bytes are programmed as they arrive and the digest is accumulated over the same stream.
 * Nothing is ever held, which is also why the image cannot be verified by reading it back
 * during the write.
 *
 * 4 KB SECTORS, NOT 64 KB BLOCKS. The measured part is 512 KiB; at a 64 KB stride it would
 * hold seven slots and waste most of itself. Sector granularity gives 25 slots of a 2.6"
 * frame. Slots are sector-aligned so rewriting one cannot disturb a neighbour.
 *
 * state = WRITING IS THE TORN-WRITE GUARD. The header goes down first as WRITING, the payload
 * streams, and only then is the header's first page rewritten to VALID. Power loss at any
 * point leaves a WRITING slot, which mount treats as free. No slot can ever be half-valid, so
 * there is no scan-time repair and no partially-correct image to display.
 */
#ifndef OD_IMGSTORE_H
#define OD_IMGSTORE_H

#include <stdbool.h>
#include <stdint.h>

#include "od_hal_spi.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OD_IMGSTORE_MAX_SLOTS 32u

/* STATE VALUES ARE A BIT LATTICE, NOT LABELS. A NOR program can only clear bits, so each
 * state must be reachable from the one before it by clearing bits alone -- otherwise the
 * header would need an erase to advance, and an erase cannot be atomic with the payload it
 * is meant to validate. FREE is the erased value, and
 *
 *     FREE 0xFF  ->  WRITING 0xFE  ->  VALID 0xFC  ->  BAD 0x00
 *
 * is monotonically decreasing in set bits. Picking a memorable constant like 0xA5 for VALID
 * instead silently ANDs with 0xFE and stores 0xA4, which reads back as no state at all. */
enum {
    OD_SLOT_FREE = 0xFFu,
    OD_SLOT_WRITING = 0xFEu,
    OD_SLOT_VALID = 0xFCu,
    OD_SLOT_BAD = 0x00u
};

typedef struct {
    uint8_t  state;
    uint16_t width;
    uint16_t height;
    uint8_t  colour_scheme;
    uint8_t  pass_count;
    uint32_t payload_len;
    uint32_t digest;          /* CRC-32 over the stored payload */
} od_imgstore_slot_t;

typedef struct {
    bool     mounted;
    uint8_t  slot_count;
    uint32_t slot_stride;     /* bytes, a multiple of the sector size */
    uint32_t capacity;
} od_imgstore_info_t;

/* Mount, or format if the superblock is absent or describes a different panel.
 *
 * `geometry_hash` fingerprints (width, height, colour_scheme, pass_count). A stored plane set
 * is meaningful only for the panel it was rendered for, and a board can be re-provisioned onto
 * a different one -- so a mismatch drops the store rather than displaying a 4.2" frame on a
 * 2.6" panel, which looks like a corrupt driver rather than a stale cache. */
bool od_imgstore_mount(const od_hal_spi_pins_t *pins, uint32_t geometry_hash,
                       uint32_t slot_payload_max);

void od_imgstore_unmount(void);

const od_imgstore_info_t *od_imgstore_status(void);

bool od_imgstore_slot(uint8_t index, od_imgstore_slot_t *out);

/* Erase one slot, or every slot. Blocking: a sector erase is tens of ms and a slot is
 * several sectors. Called from command handling, never from a refresh path. */
bool od_imgstore_erase(uint8_t index);
bool od_imgstore_erase_all(void);

/* Streaming write. begin -> write* -> finish, and abort if anything goes wrong in between.
 * `payload_len` must be known up front: it sizes the erase and is written into the header
 * before any payload byte exists to describe it. */
bool od_imgstore_begin(uint8_t index, uint16_t w, uint16_t h, uint8_t scheme,
                       uint8_t passes, uint32_t payload_len);
bool od_imgstore_write(const uint8_t *data, uint32_t len);
bool od_imgstore_finish(uint32_t *digest_out);
void od_imgstore_abort(void);

/* Read payload bytes back out of a VALID slot. */
bool od_imgstore_read(uint8_t index, uint32_t offset, uint8_t *dst, uint32_t len);

/* CRC-32 of a payload, for a host that wants to predict a digest before uploading. */
uint32_t od_imgstore_crc32(uint32_t crc, const uint8_t *data, uint32_t len);

#ifdef __cplusplus
}
#endif
#endif /* OD_IMGSTORE_H */
