/* What targets/telink-tlsr/src/od_tlsr_install.c takes from the SDK, for the host build
 * (OD_TLSR_INSTALL_HOST): the flash command codes, the two RAM-resident mspi primitives over the
 * simulated flash (tests/host/tlsr_install_test.c), and the watchdog / IRQ / reset hooks. */
#ifndef TLSR_INSTALL_HOST_H
#define TLSR_INSTALL_HOST_H

#include <stdint.h>

#define FLASH_WRITE_CMD      0x02u
#define FLASH_READ_CMD       0x03u
#define FLASH_SECT_ERASE_CMD 0x20u

void flash_mspi_read_ram(unsigned char cmd, unsigned long addr, unsigned char addr_en,
                         unsigned char dummy_cnt, unsigned char *data, unsigned long data_len);
void flash_mspi_write_ram(unsigned char cmd, unsigned long addr, unsigned char addr_en,
                          unsigned char *data, unsigned long data_len);

/* The system reset: longjmps back into the test. */
void install_host_reset(void);

#define INSTALL_RAM
#define INSTALL_WD()      ((void)0)
#define INSTALL_IRQ_OFF() ((void)0)
#define INSTALL_RESET()   install_host_reset()

#endif
