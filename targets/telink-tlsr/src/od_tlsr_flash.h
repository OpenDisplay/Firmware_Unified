/* Guarded access to the on-chip flash.
 *
 * Every erase and program is checked against the writable ranges in od_tlsr_layout.h and
 * refused outside them. This file is the only caller of the SDK's flash write primitives
 * besides the RAM install routine, so the guard is the whole of the "never touch the legacy firmware's
 * sectors" rule. The interface is plain C so the store and the updater can be host-tested
 * against a simulated flash (tests/host/tlsr_store_test.c). */
#ifndef OD_TLSR_FLASH_H
#define OD_TLSR_FLASH_H

#include <stdbool.h>
#include <stdint.h>

/* true when [addr, addr+len) lies inside one writable range. */
bool od_tlsr_flash_writable(uint32_t addr, uint32_t len);

/* Erase the 4 KB sector at addr (sector-aligned). false when refused. */
bool od_tlsr_flash_erase(uint32_t addr);

/* Program len bytes (any alignment; split at page boundaries here). Programming only clears
 * bits, so the target range must be erased or hold a superset of the new bits. */
bool od_tlsr_flash_program(uint32_t addr, const uint8_t *data, uint32_t len);

/* Read any address, including the read-only sectors. */
void od_tlsr_flash_read(uint32_t addr, uint8_t *buf, uint32_t len);

/* Target hook: false while the measured supply is below OD_TLSR_FLASH_MIN_MV. The SDK's flash
 * driver warns that an erase or program below a safe voltage can corrupt the image and data, so
 * the updater and the record store refuse to start one then. True when the config names no
 * sense pin: the supply cannot be measured, and is then usually not a battery. */
#define OD_TLSR_FLASH_MIN_MV 2100u
bool od_tlsr_flash_supply_ok(void);

#endif /* OD_TLSR_FLASH_H */
