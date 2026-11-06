/* od_flash_nor.c -- see od_flash_nor.h. */
#include "od_flash_nor.h"

#include <string.h>

#include "od_hal_time.h"
#include "od_log.h"

#define CMD_WRITE_ENABLE   0x06u
#define CMD_READ_STATUS    0x05u
#define CMD_READ           0x03u
#define CMD_PAGE_PROGRAM   0x02u
#define CMD_SECTOR_ERASE   0x20u
#define CMD_JEDEC_ID       0x9Fu
#define CMD_POWER_DOWN     0xB9u
#define CMD_RELEASE_PD     0xABu

#define STATUS_WIP 0x01u

static od_flash_nor_info_t s_info;

static void addr3(uint8_t *p, uint32_t a)
{
    p[0] = (uint8_t)(a >> 16);
    p[1] = (uint8_t)(a >> 8);
    p[2] = (uint8_t)a;
}

static bool write_enable(void)
{
    const uint8_t c = CMD_WRITE_ENABLE;
    return od_hal_spi_xfer(&c, 1u, NULL, 0u) == OD_HAL_SPI_OK;
}

bool od_flash_nor_ready(void)
{
    const uint8_t c = CMD_READ_STATUS;
    uint8_t sr = 0xFFu;

    if (od_hal_spi_xfer(&c, 1u, &sr, 1u) != OD_HAL_SPI_OK) {
        return false;
    }
    return (sr & STATUS_WIP) == 0u;
}

/* Bounded spin, NOT a deadline against the ambient clock. Shared code takes its time as a
 * parameter rather than reading od_hal_uptime_ms -- the policy tools/check.sh ratchets -- and
 * a blocking status poll has nowhere to take one from. Counting delays is honest about what
 * this is: it busy-waits, and the bound is the only thing keeping an unresponsive chip from
 * hanging the loop. */
#define WAIT_STEP_US 200u

static bool wait_ready(uint32_t timeout_ms)
{
    uint32_t steps = (timeout_ms * 1000u) / WAIT_STEP_US;

    do {
        if (od_flash_nor_ready()) {
            return true;
        }
        od_hal_delay_us(WAIT_STEP_US);
    } while (steps-- > 0u);
    return false;
}

bool od_flash_nor_init(const od_hal_spi_pins_t *pins, od_flash_nor_info_t *out)
{
    uint8_t cmd;
    uint8_t id[3] = { 0, 0, 0 };

    memset(&s_info, 0, sizeof s_info);

    if (od_hal_spi_open(pins) != OD_HAL_SPI_OK) {
        od_log_warn("nor: bus not available (pins, or no MISO)");
        goto done;
    }

    /* Wake before anything else. A CS pulse covers MX25-style parts that wake on CS; 0xAB is
     * the standard release. Without this a previously-slept part answers 0xFF to everything
     * and looks like an empty socket. */
    (void)od_hal_spi_begin();
    od_hal_delay_us(20u);
    (void)od_hal_spi_end();
    od_hal_delay_us(35u);
    cmd = CMD_RELEASE_PD;
    (void)od_hal_spi_xfer(&cmd, 1u, NULL, 0u);
    od_hal_delay_us(50u);

    cmd = CMD_JEDEC_ID;
    if (od_hal_spi_xfer(&cmd, 1u, id, sizeof id) != OD_HAL_SPI_OK) {
        goto done;
    }
    /* 0x00 and 0xFF are what a floating or held bus reads as. Neither is a valid JEDEC
     * manufacturer, so neither is allowed to look like a chip. */
    if (id[0] == 0x00u || id[0] == 0xFFu) {
        od_log_warn("nor: no answer (id %02X %02X %02X)",
                    (unsigned)id[0], (unsigned)id[1], (unsigned)id[2]);
        goto done;
    }

    s_info.manufacturer = id[0];
    s_info.mem_type = id[1];
    s_info.capacity_code = id[2];
    /* Codes outside this range are not a size. Refusing is better than deriving a capacity
     * that would let the store address past the end of the part. */
    if (id[2] >= 0x10u && id[2] <= 0x1Cu) {
        s_info.size_bytes = 1uL << id[2];
        s_info.present = true;
    } else {
        od_log_warn("nor: id %02X %02X %02X has no usable capacity code",
                    (unsigned)id[0], (unsigned)id[1], (unsigned)id[2]);
    }
    od_log_info("nor: %02X %02X %02X, %u bytes",
                (unsigned)id[0], (unsigned)id[1], (unsigned)id[2],
                (unsigned)s_info.size_bytes);

done:
    if (out != NULL) {
        *out = s_info;
    }
    return s_info.present;
}

void od_flash_nor_deinit(void)
{
    if (s_info.present) {
        const uint8_t c = CMD_POWER_DOWN;
        (void)od_hal_spi_xfer(&c, 1u, NULL, 0u);
    }
    od_hal_spi_close();
}

const od_flash_nor_info_t *od_flash_nor_info(void) { return &s_info; }

bool od_flash_nor_read(uint32_t addr, uint8_t *dst, uint32_t len)
{
    uint8_t hdr[4];

    if (!s_info.present || dst == NULL || len == 0u ||
        addr > s_info.size_bytes || len > s_info.size_bytes - addr) {
        return false;
    }
    hdr[0] = CMD_READ;
    addr3(&hdr[1], addr);
    return od_hal_spi_xfer(hdr, sizeof hdr, dst, len) == OD_HAL_SPI_OK;
}

bool od_flash_nor_erase_sector(uint32_t addr)
{
    uint8_t hdr[4];

    if (!s_info.present || (addr % OD_NOR_SECTOR_SIZE) != 0u ||
        addr >= s_info.size_bytes) {
        return false;
    }
    if (!write_enable()) {
        return false;
    }
    hdr[0] = CMD_SECTOR_ERASE;
    addr3(&hdr[1], addr);
    return od_hal_spi_xfer(hdr, sizeof hdr, NULL, 0u) == OD_HAL_SPI_OK;
}

bool od_flash_nor_program_page(uint32_t addr, const uint8_t *src, uint32_t len)
{
    uint8_t hdr[4];

    if (!s_info.present || src == NULL || len == 0u || len > OD_NOR_PAGE_SIZE ||
        addr > s_info.size_bytes || len > s_info.size_bytes - addr) {
        return false;
    }
    /* A program that crosses a page boundary WRAPS to the start of the same page and
     * overwrites the bytes just written, reporting success throughout. Refuse instead. */
    if ((addr % OD_NOR_PAGE_SIZE) + len > OD_NOR_PAGE_SIZE) {
        return false;
    }
    if (!write_enable()) {
        return false;
    }
    hdr[0] = CMD_PAGE_PROGRAM;
    addr3(&hdr[1], addr);

    /* One CS assertion for opcode, address and data: dropping CS between them makes the chip
     * discard the write while the status register still reports success. */
    if (od_hal_spi_begin() != OD_HAL_SPI_OK) {
        return false;
    }
    if (od_hal_spi_cont(hdr, sizeof hdr) != OD_HAL_SPI_OK ||
        od_hal_spi_cont(src, len) != OD_HAL_SPI_OK) {
        (void)od_hal_spi_end();
        return false;
    }
    return od_hal_spi_end() == OD_HAL_SPI_OK;
}

bool od_flash_nor_erase_sector_sync(uint32_t addr, uint32_t timeout_ms)
{
    return od_flash_nor_erase_sector(addr) && wait_ready(timeout_ms);
}

bool od_flash_nor_program_page_sync(uint32_t addr, const uint8_t *src, uint32_t len,
                                    uint32_t timeout_ms)
{
    return od_flash_nor_program_page(addr, src, len) && wait_ready(timeout_ms);
}
