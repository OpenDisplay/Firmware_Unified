/* od_imgstore.c -- see od_imgstore.h. */
#include "od_imgstore.h"

#include <string.h>

#include "od_flash_nor.h"
#include "od_log.h"

#define SB_MAGIC   0x53494449u      /* "IDIS" little-endian */
#define SB_VERSION 1u
#define SB_SECTORS 1u               /* the superblock owns the first sector */

#define HDR_MAGIC  0x4C53444Fu      /* "ODSL" */
#define HDR_SIZE   32u              /* fits one page, so VALID is a single-page rewrite */

#define ERASE_TIMEOUT_MS  400u
#define PROGRAM_TIMEOUT_MS 20u

static od_imgstore_info_t s_info;
static uint32_t s_geometry_hash;

/* In-flight write. No payload buffer -- see the header. */
static struct {
    bool     active;
    uint8_t  index;
    uint32_t addr;        /* next flash address to program */
    uint32_t remaining;
    uint32_t crc;
} s_w;

/* CRC-32 reflected, polynomial 0xEDB88320 -- the same function od_config_store uses, written
 * bit-serial for the same reason: a 1 KB table is not available on this part. */
uint32_t od_imgstore_crc32(uint32_t crc, const uint8_t *data, uint32_t len)
{
    crc = ~crc;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= data[i];
        for (unsigned b = 0; b < 8u; b++) {
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
        }
    }
    return ~crc;
}

static void put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)v; p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16); p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t slot_addr(uint8_t i)
{
    return (SB_SECTORS * OD_NOR_SECTOR_SIZE) + ((uint32_t)i * s_info.slot_stride);
}

static bool erase_range(uint32_t addr, uint32_t bytes)
{
    for (uint32_t o = 0; o < bytes; o += OD_NOR_SECTOR_SIZE) {
        if (!od_flash_nor_erase_sector_sync(addr + o, ERASE_TIMEOUT_MS)) {
            return false;
        }
    }
    return true;
}

static bool write_superblock(void)
{
    uint8_t sb[32];

    memset(sb, 0, sizeof sb);
    put32(&sb[0], SB_MAGIC);
    sb[4] = SB_VERSION;
    sb[5] = s_info.slot_count;
    put32(&sb[8], s_info.slot_stride);
    put32(&sb[12], s_geometry_hash);

    return erase_range(0u, SB_SECTORS * OD_NOR_SECTOR_SIZE) &&
           od_flash_nor_program_page_sync(0u, sb, sizeof sb, PROGRAM_TIMEOUT_MS);
}

bool od_imgstore_mount(const od_hal_spi_pins_t *pins, uint32_t geometry_hash,
                       uint32_t slot_payload_max)
{
    uint8_t sb[32];
    od_flash_nor_info_t nor;

    memset(&s_info, 0, sizeof s_info);
    memset(&s_w, 0, sizeof s_w);
    s_geometry_hash = geometry_hash;

    if (!od_flash_nor_init(pins, &nor) || slot_payload_max == 0u) {
        return false;
    }

    /* One slot = header + payload, rounded up to whole sectors so a rewrite erases nothing
     * that belongs to a neighbour. */
    const uint32_t need = HDR_SIZE + slot_payload_max;
    s_info.slot_stride = ((need + OD_NOR_SECTOR_SIZE - 1u) / OD_NOR_SECTOR_SIZE)
                         * OD_NOR_SECTOR_SIZE;
    s_info.capacity = nor.size_bytes;

    const uint32_t usable = nor.size_bytes - (SB_SECTORS * OD_NOR_SECTOR_SIZE);
    uint32_t n = usable / s_info.slot_stride;
    if (n > OD_IMGSTORE_MAX_SLOTS) {
        n = OD_IMGSTORE_MAX_SLOTS;
    }
    if (n == 0u) {
        od_log_warn("imgstore: %u B payload does not fit %u B of flash",
                    (unsigned)slot_payload_max, (unsigned)nor.size_bytes);
        return false;
    }
    s_info.slot_count = (uint8_t)n;

    if (!od_flash_nor_read(0u, sb, sizeof sb)) {
        return false;
    }
    const bool fresh = get32(&sb[0]) != SB_MAGIC;
    const bool stale = !fresh && (get32(&sb[12]) != geometry_hash ||
                                  get32(&sb[8]) != s_info.slot_stride ||
                                  sb[5] != s_info.slot_count);
    if (fresh || stale) {
        /* Reformat rather than adopt. A superblock describing a different panel or a
         * different slot geometry cannot be reconciled with the slots behind it, and a
         * mismatched plane set rendered on this panel looks like a broken driver. */
        od_log_info("imgstore: formatting (%s)", fresh ? "no superblock" : "geometry changed");
        if (!write_superblock()) {
            return false;
        }
        /* Slot headers are left erased: 0xFF is OD_SLOT_FREE by construction, so formatting
         * costs one sector erase rather than one per slot. */
        for (uint8_t i = 0; i < s_info.slot_count; i++) {
            if (!erase_range(slot_addr(i), OD_NOR_SECTOR_SIZE)) {
                return false;
            }
        }
    }

    s_info.mounted = true;
    od_log_info("imgstore: %u slots x %u B", (unsigned)s_info.slot_count,
                (unsigned)s_info.slot_stride);
    return true;
}

void od_imgstore_unmount(void)
{
    memset(&s_w, 0, sizeof s_w);
    s_info.mounted = false;
    od_flash_nor_deinit();
}

const od_imgstore_info_t *od_imgstore_status(void) { return &s_info; }

bool od_imgstore_slot(uint8_t index, od_imgstore_slot_t *out)
{
    uint8_t h[HDR_SIZE];

    if (!s_info.mounted || index >= s_info.slot_count || out == NULL) {
        return false;
    }
    if (!od_flash_nor_read(slot_addr(index), h, sizeof h)) {
        return false;
    }
    memset(out, 0, sizeof *out);
    if (get32(&h[0]) != HDR_MAGIC) {
        out->state = OD_SLOT_FREE;
        return true;
    }
    out->state         = h[4];
    out->colour_scheme = h[5];
    out->pass_count    = h[6];
    out->width         = (uint16_t)(h[8] | (h[9] << 8));
    out->height        = (uint16_t)(h[10] | (h[11] << 8));
    out->payload_len   = get32(&h[12]);
    out->digest        = get32(&h[16]);
    return true;
}

bool od_imgstore_erase(uint8_t index)
{
    if (!s_info.mounted || index >= s_info.slot_count || s_w.active) {
        return false;
    }
    return erase_range(slot_addr(index), s_info.slot_stride);
}

bool od_imgstore_erase_all(void)
{
    if (!s_info.mounted || s_w.active) {
        return false;
    }
    for (uint8_t i = 0; i < s_info.slot_count; i++) {
        if (!od_imgstore_erase(i)) {
            return false;
        }
    }
    return true;
}

bool od_imgstore_begin(uint8_t index, uint16_t w, uint16_t h, uint8_t scheme,
                       uint8_t passes, uint32_t payload_len)
{
    uint8_t hdr[HDR_SIZE];

    if (!s_info.mounted || index >= s_info.slot_count || s_w.active ||
        payload_len == 0u || payload_len > s_info.slot_stride - HDR_SIZE) {
        return false;
    }
    if (!erase_range(slot_addr(index), s_info.slot_stride)) {
        return false;
    }

    memset(hdr, 0, sizeof hdr);
    put32(&hdr[0], HDR_MAGIC);
    hdr[4] = OD_SLOT_WRITING;      /* flipped to VALID only after the payload is down */
    hdr[5] = scheme;
    hdr[6] = passes;
    hdr[8] = (uint8_t)w; hdr[9] = (uint8_t)(w >> 8);
    hdr[10] = (uint8_t)h; hdr[11] = (uint8_t)(h >> 8);
    put32(&hdr[12], payload_len);
    /* Digest left ERASED (0xFF), not zeroed. Zeroing it would make the real CRC
     * unreachable: programming can only clear bits, and 0 AND anything is 0. */
    hdr[16] = 0xFFu; hdr[17] = 0xFFu; hdr[18] = 0xFFu; hdr[19] = 0xFFu;

    if (!od_flash_nor_program_page_sync(slot_addr(index), hdr, sizeof hdr,
                                        PROGRAM_TIMEOUT_MS)) {
        return false;
    }

    s_w.active = true;
    s_w.index = index;
    s_w.addr = slot_addr(index) + HDR_SIZE;
    s_w.remaining = payload_len;
    s_w.crc = 0u;
    return true;
}

bool od_imgstore_write(const uint8_t *data, uint32_t len)
{
    if (!s_w.active || data == NULL || len == 0u || len > s_w.remaining) {
        return false;
    }
    uint32_t off = 0u;

    while (off < len) {
        /* Split at the page boundary. A program that crosses one wraps within the page and
         * silently destroys the bytes already written, reporting success. */
        uint32_t room = OD_NOR_PAGE_SIZE - (s_w.addr % OD_NOR_PAGE_SIZE);
        uint32_t chunk = len - off;
        if (chunk > room) {
            chunk = room;
        }
        if (!od_flash_nor_program_page_sync(s_w.addr, data + off, chunk,
                                            PROGRAM_TIMEOUT_MS)) {
            return false;
        }
        s_w.crc = od_imgstore_crc32(s_w.crc, data + off, chunk);
        s_w.addr += chunk;
        off += chunk;
    }
    s_w.remaining -= len;
    return true;
}

bool od_imgstore_finish(uint32_t *digest_out)
{
    uint8_t hdr[HDR_SIZE];

    if (!s_w.active) {
        return false;
    }
    /* Short payloads stay WRITING, which mount reads as free. Declaring a length and then
     * sending less is a failed transfer, not a smaller image. */
    if (s_w.remaining != 0u) {
        od_log_warn("imgstore: %u bytes short, slot left invalid", (unsigned)s_w.remaining);
        od_imgstore_abort();
        return false;
    }
    /* 0xFF EVERYWHERE EXCEPT THE TWO FIELDS THAT CHANGE. A page program ANDs with what is
     * already there, so 0xFF leaves a byte untouched -- that is what makes this a partial
     * update of a page that already holds the geometry, without an erase. Reading the header
     * back and rewriting it whole would work too, but only by accident: it would re-AND every
     * byte with itself and mask a field that was not actually reachable. */
    memset(hdr, 0xFF, sizeof hdr);
    hdr[4] = OD_SLOT_VALID;        /* 0xFE -> 0xFC, bits cleared only */
    put32(&hdr[16], s_w.crc);      /* 0xFFFFFFFF -> crc, bits cleared only */
    const bool ok = od_flash_nor_program_page_sync(slot_addr(s_w.index), hdr, sizeof hdr,
                                                   PROGRAM_TIMEOUT_MS);
    if (ok && digest_out != NULL) {
        *digest_out = s_w.crc;
    }
    memset(&s_w, 0, sizeof s_w);
    return ok;
}

void od_imgstore_abort(void)
{
    if (s_w.active) {
        /* Left as WRITING deliberately. Erasing here would take hundreds of milliseconds on a
         * path that may be unwinding from a transport failure; mount treats WRITING as free
         * and reclaims it lazily. */
        memset(&s_w, 0, sizeof s_w);
    }
}

bool od_imgstore_read(uint8_t index, uint32_t offset, uint8_t *dst, uint32_t len)
{
    od_imgstore_slot_t slot;

    if (!od_imgstore_slot(index, &slot) || slot.state != OD_SLOT_VALID) {
        return false;
    }
    if (offset > slot.payload_len || len > slot.payload_len - offset) {
        return false;
    }
    return od_flash_nor_read(slot_addr(index) + HDR_SIZE + offset, dst, len);
}
