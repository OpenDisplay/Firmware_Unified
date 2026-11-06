/* od_flash_probe -- "is there an SPI NOR on the configured pins, and what is it?"
 *
 * A diagnostic that runs once at boot, before anything is built on the assumption that a chip
 * is present. The storage driver belongs behind od_hal_spi; this only answers the question
 * that decides whether that driver is worth writing for a given board.
 */
#ifndef OD_FLASH_PROBE_H
#define OD_FLASH_PROBE_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* `GlobalConfig` is a #define for od_config (opendisplay_runtime.h); naming the real type
 * here keeps this declaration identical whether or not that macro is in scope. */
struct od_config;

enum {
    OD_FLASH_PROBE_IDLE = 0,
    OD_FLASH_PROBE_OK,          /* a plausible JEDEC id came back */
    OD_FLASH_PROBE_NO_CONFIG,   /* no enabled FlashConfig packet */
    OD_FLASH_PROBE_BAD_PINS,    /* MOSI/SCK/CS missing or undecodable */
    OD_FLASH_PROBE_NO_MISO,     /* no read path -- distinct from "chip did not answer" */
    OD_FLASH_PROBE_NO_ANSWER    /* bus read all-zero or all-ones */
};

typedef struct {
    uint8_t status;
    uint8_t manufacturer;   /* JEDEC id byte 1: 0xEF Winbond, 0xC2 Macronix, 0x20 Micron... */
    uint8_t mem_type;       /* byte 2 */
    uint8_t capacity;       /* byte 3: size is 1 << capacity bytes on most parts */
    uint8_t status_reg;     /* 0x05 read: bit 0 is WIP */
} od_flash_probe_result_t;

const od_flash_probe_result_t *od_flash_probe_run(const struct od_config *cfg);
const od_flash_probe_result_t *od_flash_probe_last(void);

#ifdef __cplusplus
}
#endif
#endif /* OD_FLASH_PROBE_H */
