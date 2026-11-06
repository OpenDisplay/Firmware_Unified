/* od_flash_nor -- a real SPI NOR driver: identify, read, program, erase.
 *
 * Portable C over od_hal_spi. No part numbers compiled in: capacity comes from the JEDEC id,
 * and 4 KB sector erase / 256 B page program are asserted rather than parsed from SFDP,
 * because every part in this class agrees on them and SFDP is a lot of code for a value that
 * does not vary here.
 *
 * EVERY WRITE AND ERASE IS BLOCKING AND SLOW -- a 4 KB sector erase is tens of milliseconds.
 * od_flash_nor_poll_ready() is separate from the operations that start them so a caller on a
 * superloop can yield between passes instead of spinning past its watchdog. The convenience
 * wrappers that wait inline are marked, and are only for boot-time use.
 */
#ifndef OD_FLASH_NOR_H
#define OD_FLASH_NOR_H

#include <stdbool.h>
#include <stdint.h>

#include "od_hal_spi.h"

#ifdef __cplusplus
extern "C" {
#endif

#define OD_NOR_PAGE_SIZE    256u
#define OD_NOR_SECTOR_SIZE  4096u

typedef struct {
    uint8_t  manufacturer;
    uint8_t  mem_type;
    uint8_t  capacity_code;   /* size is 1 << capacity_code bytes */
    uint32_t size_bytes;
    bool     present;
} od_flash_nor_info_t;

/* Open the bus, release the chip from deep power-down, and read the JEDEC id.
 *
 * THE RELEASE IS THE POINT. OpenDisplay sends 0xB9 (deep power-down) at sleep and has never
 * sent 0xAB to release it, so a part that has slept once ignores every command and reads back
 * 0xFF -- identical to absent. A CS pulse precedes 0xAB for MX25-style parts that wake on CS. */
bool od_flash_nor_init(const od_hal_spi_pins_t *pins, od_flash_nor_info_t *out);

/* Put the chip back to sleep and release the pins. */
void od_flash_nor_deinit(void);

const od_flash_nor_info_t *od_flash_nor_info(void);

/* Read is the only operation with no wait: NOR reads are combinatorial. */
bool od_flash_nor_read(uint32_t addr, uint8_t *dst, uint32_t len);

/* True when the chip has finished the last program or erase (status bit 0 clear). */
bool od_flash_nor_ready(void);

/* Start a 4 KB sector erase. Returns false if the address is unaligned or out of range.
 * Does NOT wait -- poll od_flash_nor_ready(). */
bool od_flash_nor_erase_sector(uint32_t addr);

/* Program up to one page, and NEVER ACROSS A PAGE BOUNDARY: a NOR page program that crosses
 * one wraps to the start of the same page and silently overwrites what was just written. The
 * caller splits; this refuses rather than wrapping. Does NOT wait. */
bool od_flash_nor_program_page(uint32_t addr, const uint8_t *src, uint32_t len);

/* Boot-time convenience: start and spin until ready, bounded. Not for the superloop. */
bool od_flash_nor_erase_sector_sync(uint32_t addr, uint32_t timeout_ms);
bool od_flash_nor_program_page_sync(uint32_t addr, const uint8_t *src, uint32_t len,
                                    uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif
#endif /* OD_FLASH_NOR_H */
