/* A simulated TLSR825x flash behind od_tlsr_flash.h, for the Telink store and updater tests.
 *
 * NOR semantics: erase sets a 4 KB sector to 0xFF, program can only clear bits (new = old & data).
 * The write guard is the target's own (od_tlsr_flash_writable is reimplemented from the same
 * layout constants, because the target file carries the SDK calls).
 *
 * Power cuts: fake_flash_cut_after(n) lets n erase/program operations complete and interrupts
 * the next one part-way -- an erase leaves the first half of the sector erased and the rest as it
 * was, a program lands only its first half -- and then longjmps to the test's restart point. */
#include "fake_tlsr_flash.h"

#include "od_tlsr_flash.h"
#include "od_tlsr_layout.h"

#include <string.h>

uint8_t fake_flash[FAKE_FLASH_SIZE];
static long s_budget = -1;
static jmp_buf *s_cut;
unsigned long fake_flash_ops;

void fake_flash_reset(void)
{
    memset(fake_flash, 0xFF, sizeof fake_flash);
    s_budget = -1;
    s_cut = NULL;
    fake_flash_ops = 0u;
}

void fake_flash_cut_after(long n, jmp_buf *where)
{
    s_budget = n;
    s_cut = where;
}

void fake_flash_no_cut(void)
{
    s_budget = -1;
    s_cut = NULL;
}

static bool cut_now(void)
{
    fake_flash_ops++;
    if (s_budget < 0) {
        return false;
    }
    if (s_budget == 0) {
        return true;
    }
    s_budget--;
    return false;
}

static bool within(uint32_t addr, uint32_t end, uint32_t lo, uint32_t hi)
{
    return addr >= lo && end <= hi;
}

bool od_tlsr_flash_writable(uint32_t addr, uint32_t len)
{
    uint32_t end = addr + len;

    if (len == 0u || end < addr) {
        return false;
    }
    return within(addr, end, OD_TLSR_WR0_START, OD_TLSR_WR0_END) ||
           within(addr, end, OD_TLSR_WR1_START, OD_TLSR_WR1_END);
}

bool od_tlsr_flash_erase(uint32_t addr)
{
    if ((addr & (OD_TLSR_SECTOR - 1u)) != 0u || !od_tlsr_flash_writable(addr, OD_TLSR_SECTOR)) {
        return false;
    }
    if (cut_now()) {
        memset(&fake_flash[addr], 0xFF, OD_TLSR_SECTOR / 2u);
        longjmp(*s_cut, 1);
    }
    memset(&fake_flash[addr], 0xFF, OD_TLSR_SECTOR);
    return true;
}

bool od_tlsr_flash_program(uint32_t addr, const uint8_t *data, uint32_t len)
{
    uint32_t i, n = len;

    if (data == NULL || !od_tlsr_flash_writable(addr, len)) {
        return false;
    }
    if (cut_now()) {
        n = len / 2u;
    }
    for (i = 0u; i < n; ++i) {
        fake_flash[addr + i] &= data[i];
    }
    if (n != len) {
        longjmp(*s_cut, 1);
    }
    return true;
}

void od_tlsr_flash_read(uint32_t addr, uint8_t *buf, uint32_t len)
{
    memcpy(buf, &fake_flash[addr], len);
}
