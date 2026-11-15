/* Flash map of a 512 KB TLSR825x tag under OpenDisplay.
 *
 * Writes stay inside the two ranges the legacy updater itself writes -- its image at
 * [0, 0x33000) and its staging area at [0x40000, 0x73000) -- so a tag updated from the legacy firmware to
 * OpenDisplay and back keeps whatever the legacy firmware stores elsewhere. Never written: 0x33000-0x3FFFF,
 * and everything from 0x73000 (SMP pairing 0x74000, MAC 0x76000, RF calibration 0x77000,
 * master pairing 0x78000). od_tlsr_flash.c refuses any erase or program outside the ranges.
 *
 *   0x00000 +---------------------+
 *           | running image       |  OD_TLSR_IMAGE_MAX
 *   0x2D000 +---------------------+
 *           | (not written)       |
 *   0x40000 +---------------------+
 *           | update staging      |  OD_TLSR_IMAGE_MAX: an OpenDisplay or a legacy image
 *   0x6D000 +---------------------+
 *           | config bank A, B    |  2 x 8 KB: a 4096-byte config plus record header
 *   0x71000 +---------------------+
 *           | secrets bank A, B   |  2 x 4 KB
 *   0x73000 +---------------------+
 *
 * The image cap is the staging area's size, so whatever runs can always stage its successor. */
#ifndef OD_TLSR_LAYOUT_H
#define OD_TLSR_LAYOUT_H

#define OD_TLSR_SECTOR          0x1000u
#define OD_TLSR_PAGE            0x100u

#define OD_TLSR_IMAGE_BASE      0x00000u
#define OD_TLSR_IMAGE_MAX       0x2D000u
#define OD_TLSR_STAGE_BASE      0x40000u

#define OD_TLSR_CFG_BANK_A      0x6D000u
#define OD_TLSR_CFG_BANK_B      0x6F000u
#define OD_TLSR_CFG_BANK_SIZE   0x2000u
#define OD_TLSR_SEC_BANK_A      0x71000u
#define OD_TLSR_SEC_BANK_B      0x72000u
#define OD_TLSR_SEC_BANK_SIZE   0x1000u

/* The two writable ranges, [start, end). */
#define OD_TLSR_WR0_START       OD_TLSR_IMAGE_BASE
#define OD_TLSR_WR0_END         (OD_TLSR_IMAGE_BASE + OD_TLSR_IMAGE_MAX)
#define OD_TLSR_WR1_START       OD_TLSR_STAGE_BASE
#define OD_TLSR_WR1_END         0x73000u

/* Read only: the factory MAC record (6 bytes public, 2 bytes random-static tail). */
#define OD_TLSR_MAC_ADDR        0x76000u

/* Telink image header: "KNLT" at 8 (the ROM's boot flag is the byte at 8), total length at
 * 0x18. A sealed image is the body padded to 16 bytes plus a trailing CRC-32 without the final
 * inversion (tools/tlsr_image.py). */
#define OD_TLSR_HDR_FLAG_OFF    8u
#define OD_TLSR_HDR_FLAG        0x4Bu /* 'K' */
#define OD_TLSR_HDR_SIZE_OFF    0x18u
#define OD_TLSR_IMAGE_MIN       0x40u

/* The B85 ROM's multi-boot fallback slots (the SDK's multi_boot_addr_e). While sector 0 has no
 * boot flag -- an install in progress -- a 0x4B at slot + 8 inside the image would be booted as
 * an image of its own, so no image may carry one there (tools/tlsr_image.py, od_tlsr_ota.c). */
#define OD_TLSR_MULTI_BOOT_A    0x20000u
#define OD_TLSR_MULTI_BOOT_B    0x40000u

#if (OD_TLSR_STAGE_BASE + OD_TLSR_IMAGE_MAX) > OD_TLSR_CFG_BANK_A
#error "staging overlaps the config banks"
#endif
#if (OD_TLSR_CFG_BANK_B + OD_TLSR_CFG_BANK_SIZE) > OD_TLSR_SEC_BANK_A
#error "config banks overlap the secrets banks"
#endif
#if (OD_TLSR_SEC_BANK_B + OD_TLSR_SEC_BANK_SIZE) > OD_TLSR_WR1_END
#error "secrets banks leave the writable range"
#endif

#endif /* OD_TLSR_LAYOUT_H */
