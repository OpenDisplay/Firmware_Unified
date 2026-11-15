/* Copy the staged image over the running one, then reboot. Runs entirely from RAM because it
 * erases the flash the application executes from: it may call only RAM-resident code, which
 * tools/check_ram_install.py verifies on every build from the linked image.
 *
 * Order: sector 0 is erased first and programmed last, after every other sector is in place.
 * Until the very end the image at 0 therefore has no boot flag, while the staged copy at
 * OD_TLSR_STAGE_BASE does (od_tlsr_ota.c sets it only after the CRC pass). The staged copy's
 * flag is cleared last, so that afterwards only the installed image is bootable. A power cut
 * before that leaves no bootable image at 0; whether the ROM then boots the staged copy (B85
 * multi-boot at 0x40000) is unverified -- REPORT.md records it, and SWS is the recovery. */
#include "od_tlsr_sdk.h"

#include "od_tlsr_ota.h"
#include "od_tlsr_layout.h"

/* RAM-resident SDK primitives (drivers/B85/flash.c); not in its header. */
void flash_mspi_read_ram(unsigned char cmd, unsigned long addr, unsigned char addr_en,
                         unsigned char dummy_cnt, unsigned char *data, unsigned long data_len);
void flash_mspi_write_ram(unsigned char cmd, unsigned long addr, unsigned char addr_en,
                          unsigned char *data, unsigned long data_len);

static unsigned char s_page[OD_TLSR_PAGE];

_attribute_ram_code_sec_noinline_ static void copy_sector(unsigned long base, unsigned long len)
{
    unsigned long a, end = base + OD_TLSR_SECTOR;

    if (end > len) {
        end = len;
    }
    for (a = base; a < end; a += OD_TLSR_PAGE) {
        unsigned long n = end - a < OD_TLSR_PAGE ? end - a : OD_TLSR_PAGE;
        flash_mspi_read_ram(FLASH_READ_CMD, OD_TLSR_STAGE_BASE + a, 1, 0, s_page, n);
        flash_mspi_write_ram(FLASH_WRITE_CMD, a, 1, s_page, n);
    }
}

_attribute_ram_code_sec_noinline_ void od_tlsr_install(uint32_t len)
{
    unsigned long sectors = ((unsigned long)len + OD_TLSR_SECTOR - 1u) >> 12;
    unsigned long sec;

    reg_irq_en = 0;
    reg_tmr_sta = FLD_TMR_STA_WD; /* feed the watchdog (wd_clear) */
    flash_mspi_write_ram(FLASH_SECT_ERASE_CMD, OD_TLSR_IMAGE_BASE, 1, 0, 0);
    for (sec = 1u; sec < sectors; ++sec) {
        reg_tmr_sta = FLD_TMR_STA_WD;
        flash_mspi_write_ram(FLASH_SECT_ERASE_CMD, sec << 12, 1, 0, 0);
        copy_sector(sec << 12, len);
    }
    reg_tmr_sta = FLD_TMR_STA_WD;
    copy_sector(0u, len);
    s_page[0] = 0u;
    flash_mspi_write_ram(FLASH_WRITE_CMD, OD_TLSR_STAGE_BASE + OD_TLSR_HDR_FLAG_OFF, 1, s_page, 1);
    REG_ADDR8(0x6f) = 0x20; /* system reset */
    for (;;) {
    }
}
