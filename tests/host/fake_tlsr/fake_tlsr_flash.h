#ifndef FAKE_TLSR_FLASH_H
#define FAKE_TLSR_FLASH_H

#include <setjmp.h>
#include <stdbool.h>
#include <stdint.h>

#define FAKE_FLASH_SIZE 0x80000u

extern uint8_t fake_flash[FAKE_FLASH_SIZE];
extern unsigned long fake_flash_ops;

void fake_flash_reset(void);
/* Let n operations complete, interrupt the next, longjmp to *where. */
void fake_flash_cut_after(long n, jmp_buf *where);
void fake_flash_no_cut(void);
/* od_tlsr_flash_supply_ok() answers !fake_flash_supply_low; fake_flash_reset() clears it. */
extern bool fake_flash_supply_low;

#endif
