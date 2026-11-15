#include "od_tlsr_sdk.h"

#include "od_tlsr_flash.h"
#include "od_tlsr_layout.h"

static bool s_unlocked;

bool od_tlsr_flash_writable(uint32_t addr, uint32_t len)
{
    uint32_t end = addr + len;

    if (len == 0u || end < addr) {
        return false;
    }
    return (addr >= OD_TLSR_WR0_START && end <= OD_TLSR_WR0_END) ||
           (addr >= OD_TLSR_WR1_START && end <= OD_TLSR_WR1_END);
}

/* The tag's previous firmware, or a factory image, may leave status-register block protection
 * set; an erase under it fails silently. Clear it once, on the first write this boot. */
static void unlock_once(void)
{
    if (!s_unlocked) {
        flash_unlock();
        s_unlocked = true;
    }
}

bool od_tlsr_flash_erase(uint32_t addr)
{
    if ((addr & (OD_TLSR_SECTOR - 1u)) != 0u || !od_tlsr_flash_writable(addr, OD_TLSR_SECTOR)) {
        return false;
    }
    unlock_once();
    flash_erase_sector(addr);
    return true;
}

bool od_tlsr_flash_program(uint32_t addr, const uint8_t *data, uint32_t len)
{
    if (data == NULL || !od_tlsr_flash_writable(addr, len)) {
        return false;
    }
    unlock_once();
    /* flash_page_program splits at page boundaries; a source in flash is copied to the stack
     * there, so callers pass RAM buffers. */
    flash_page_program(addr, len, (unsigned char *)data);
    return true;
}

void od_tlsr_flash_read(uint32_t addr, uint8_t *buf, uint32_t len)
{
    if (buf != NULL && len != 0u) {
        flash_read_data(addr, len, buf);
    }
}
