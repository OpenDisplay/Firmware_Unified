/* Copy the staged image over the running one, then reboot. Runs entirely from RAM because it
 * erases the flash the application executes from: it may call only RAM-resident code, which
 * tools/check_ram_install.py verifies on every build from the linked image.
 *
 * Order, so that at every instant at most one complete image carries a boot flag and the one
 * that does is intact:
 *   1. sector 0 is erased, so the image at 0 is no longer bootable; the staged copy at
 *      OD_TLSR_STAGE_BASE is (od_tlsr_ota.c sets its flag only after the CRC pass);
 *   2. sectors 1..n are erased, copied and read back, each tried up to COPY_TRIES times;
 *   3. sector 0 is copied with its flag byte left erased, and read back;
 *   4. the flag byte at 0 is written and read back;
 *   5. only then is the staged copy's flag cleared.
 * If a sector will not read back, the install stops before step 4: sector 0 stays without a
 * flag and the staged copy keeps its own -- the state a power cut in steps 1-3 also leaves.
 * Whether the ROM then boots the staged copy (B85 multi-boot at 0x40000) is unverified on this
 * part; SWS is the recovery. tests/host/tlsr_install_test.c cuts power at every operation.
 *
 * OD_TLSR_INSTALL_HOST builds the same code for that test, over a simulated flash. */
#ifndef OD_TLSR_INSTALL_HOST
#include "od_tlsr_sdk.h"

#define INSTALL_RAM       _attribute_ram_code_sec_noinline_
#define INSTALL_WD()      (reg_tmr_sta = FLD_TMR_STA_WD) /* wd_clear(), inlined */
#define INSTALL_IRQ_OFF() (reg_irq_en = 0)
#define INSTALL_RESET()   (REG_ADDR8(0x6f) = 0x20)       /* system reset */

/* RAM-resident SDK primitives (drivers/B85/flash.c); not in its header. */
void flash_mspi_read_ram(unsigned char cmd, unsigned long addr, unsigned char addr_en,
                         unsigned char dummy_cnt, unsigned char *data, unsigned long data_len);
void flash_mspi_write_ram(unsigned char cmd, unsigned long addr, unsigned char addr_en,
                          unsigned char *data, unsigned long data_len);
#else
#include "tlsr_install_host.h"
#endif

#include "od_tlsr_ota.h"
#include "od_tlsr_layout.h"

#define COPY_TRIES 3u
#define CMP_CHUNK  32u

static unsigned char s_page[OD_TLSR_PAGE];

/* Copy one sector's image bytes from staging, holding back the boot-flag byte when asked. */
INSTALL_RAM static void copy_sector(unsigned long base, unsigned long len, int hold_flag)
{
    unsigned long a, end = base + OD_TLSR_SECTOR;

    if (end > len) {
        end = len;
    }
    for (a = base; a < end; a += OD_TLSR_PAGE) {
        unsigned long n = end - a < OD_TLSR_PAGE ? end - a : OD_TLSR_PAGE;
        flash_mspi_read_ram(FLASH_READ_CMD, OD_TLSR_STAGE_BASE + a, 1, 0, s_page, n);
        if (hold_flag && a == 0u) {
            s_page[OD_TLSR_HDR_FLAG_OFF] = 0xFFu;
        }
        flash_mspi_write_ram(FLASH_WRITE_CMD, a, 1, s_page, n);
    }
}

/* The sector at base reads back as staging does, the held-back flag byte reading erased. */
INSTALL_RAM static int sector_matches(unsigned long base, unsigned long len, int hold_flag)
{
    unsigned char want[CMP_CHUNK], got[CMP_CHUNK];
    unsigned long a, end = base + OD_TLSR_SECTOR;
    unsigned int i;

    if (end > len) {
        end = len;
    }
    for (a = base; a < end; a += CMP_CHUNK) {
        unsigned long n = end - a < CMP_CHUNK ? end - a : CMP_CHUNK;
        flash_mspi_read_ram(FLASH_READ_CMD, OD_TLSR_STAGE_BASE + a, 1, 0, want, n);
        flash_mspi_read_ram(FLASH_READ_CMD, a, 1, 0, got, n);
        if (hold_flag && a == 0u) {
            want[OD_TLSR_HDR_FLAG_OFF] = 0xFFu;
        }
        for (i = 0u; i < n; ++i) {
            if (want[i] != got[i]) {
                return 0;
            }
        }
    }
    return 1;
}

INSTALL_RAM static int install_sector(unsigned long base, unsigned long len, int hold_flag)
{
    unsigned int t;

    for (t = 0u; t < COPY_TRIES; ++t) {
        INSTALL_WD();
        flash_mspi_write_ram(FLASH_SECT_ERASE_CMD, base, 1, 0, 0);
        copy_sector(base, len, hold_flag);
        if (sector_matches(base, len, hold_flag)) {
            return 1;
        }
    }
    return 0;
}

INSTALL_RAM void od_tlsr_install(uint32_t len)
{
    unsigned long sectors = ((unsigned long)len + OD_TLSR_SECTOR - 1u) >> 12;
    unsigned long sec;
    unsigned char b;

    INSTALL_IRQ_OFF();
    INSTALL_WD();
    flash_mspi_write_ram(FLASH_SECT_ERASE_CMD, OD_TLSR_IMAGE_BASE, 1, 0, 0);
    for (sec = 1u; sec < sectors; ++sec) {
        if (!install_sector(sec << 12, len, 0)) {
            goto out;
        }
    }
    if (!install_sector(0u, len, 1)) {
        goto out;
    }
    b = OD_TLSR_HDR_FLAG;
    flash_mspi_write_ram(FLASH_WRITE_CMD, OD_TLSR_HDR_FLAG_OFF, 1, &b, 1);
    flash_mspi_read_ram(FLASH_READ_CMD, OD_TLSR_HDR_FLAG_OFF, 1, 0, &b, 1);
    if (b != OD_TLSR_HDR_FLAG) {
        goto out;
    }
    b = 0u;
    flash_mspi_write_ram(FLASH_WRITE_CMD, OD_TLSR_STAGE_BASE + OD_TLSR_HDR_FLAG_OFF, 1, &b, 1);
out:
    INSTALL_RESET();
    for (;;) {
    }
}
