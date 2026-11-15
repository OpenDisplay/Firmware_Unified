/* od_hal_spi -- SPI transactions for the flash, keyed by the pins a config names.
 *
 * THE SEAM IS THE TRANSACTION, not the byte, for the reason od_hal_i2c.h gives about its own
 * shape. A NOR page program is opcode + 3 address bytes + up to 256 data bytes inside ONE CS
 * assertion; if CS drops between the address and the data the chip ignores the write and the
 * status register still reports ready. That is a corrupt image with no error anywhere, so CS
 * framing is explicit here and never inferred.
 *
 * PINS, NOT A BUS ID. od_hal_i2c keys on DataBus.instance_number because its devices are
 * addressed on a shared bus. The flash is point-to-point on pins FlashConfig names, and this
 * target bit-bangs them -- there is no bus instance to resolve. Passing the pins through keeps
 * the HAL from needing its own copy of the config.
 *
 * MISO IS REQUIRED. Every operation that matters is a read: the JEDEC id, the status register
 * that says whether an erase finished, and reading an image back. A caller with no MISO pin
 * should not open the bus at all rather than get writes that cannot be verified.
 */
#ifndef OD_HAL_SPI_H
#define OD_HAL_SPI_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define OD_HAL_SPI_OK        0
#define OD_HAL_SPI_EIO     (-1)
#define OD_HAL_SPI_ENODEV  (-2)   /* pins undecodable, or MISO absent */

/* Pin encoding is the config's own: (port << 4) | pin. */
typedef struct {
    uint8_t mosi;
    uint8_t miso;
    uint8_t sck;
    uint8_t cs;
} od_hal_spi_pins_t;

/* Claim the pins. Safe to call repeatedly; the last open wins. */
int od_hal_spi_open(const od_hal_spi_pins_t *pins);

/* Release the pins to a low-power state. */
void od_hal_spi_close(void);

/* ONE TRANSACTION, CS asserted for the whole of it.
 *
 * `tx`/`tx_len` are clocked out first, then `rx_len` bytes are clocked in -- sequential within
 * one CS assertion, NOT full duplex. No NOR command needs duplex, and pretending otherwise
 * would make the common case (write opcode+address, then read) harder to express than the
 * rare one. Either half may be empty.
 */
int od_hal_spi_xfer(const uint8_t *tx, uint32_t tx_len, uint8_t *rx, uint32_t rx_len);

/* A transaction held open across calls, so a page program can be assembled from chunks that
 * arrive separately without staging 256 bytes of RAM -- which this part does not have spare.
 * begin/cont/end must be balanced; nothing else may use the bus in between. */
int od_hal_spi_begin(void);
int od_hal_spi_cont(const uint8_t *tx, uint32_t len);
int od_hal_spi_end(void);

#ifdef __cplusplus
}
#endif
#endif /* OD_HAL_SPI_H */
