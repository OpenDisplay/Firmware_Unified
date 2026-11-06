/* The display half of CMD_SPI_RAW, as a link-time stub.
 *
 * The WIRE half (targets/efr32bg22-slc/od_cmd_spi_raw.c) is compiled for real into the silabs
 * executables, so sub-opcode routing, the short-argument refusals, the BUSY reply shape and
 * DATA_NOACK's deliberate silence are all under test. Only this seam is faked: the production
 * implementation powers a rail and clocks a panel through em_gpio.
 *
 * IT MODELS THE SESSION rather than returning a constant. "Open" is the one piece of state the
 * handler's contract depends on -- every sub-opcode except OPEN and CLOSE must refuse with
 * NOT_OPEN outside a session, and CLOSE must stay idempotent -- so a stub that always succeeded
 * would make those paths untestable while still linking.
 */
#include "od_caps.h"

#include "opendisplay_protocol.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if OD_CAP_SPI_RAW

static bool s_open;

/* od_epd_desc_diag() lives in shared/core/od_epd.c, which is in the APP_EPD tier -- a tier no
 * host archive composes, so the symbol is not linkable here. Faked at the same seam as the
 * display functions: the NACK's byte 4 is a diagnostic, and what the wire half is being tested
 * on is that the frame is 5 bytes carrying the right err and sub. A recognisable constant
 * rather than 0, so a test asserting the byte cannot pass by accident on a zeroed buffer. */
uint8_t od_epd_desc_diag(void) { return 0x5Au; }

/* Visible to a test that wants to assert the rail was released. */
unsigned od_test_spi_raw_closes;

bool opendisplay_display_spi_raw_is_open(void) { return s_open; }

int opendisplay_display_spi_raw_open(bool run_panel_init)
{
    (void)run_panel_init;
    s_open = true;
    return 0;
}

int opendisplay_display_spi_raw_cmd(const uint8_t *data, uint16_t len)
{
    if (!s_open) {
        return -OD_ERR_SPI_RAW_NOT_OPEN;
    }
    return (data == NULL || len == 0u) ? -OD_ERR_SPI_RAW_BAD_ARGS : 0;
}

int opendisplay_display_spi_raw_data(const uint8_t *data, uint16_t len)
{
    if (!s_open) {
        return -OD_ERR_SPI_RAW_NOT_OPEN;
    }
    return (data == NULL || len == 0u) ? -OD_ERR_SPI_RAW_BAD_ARGS : 0;
}

int opendisplay_display_spi_raw_busy(uint32_t timeout_ms, uint16_t *elapsed_ms_out,
                                     bool *still_busy_out)
{
    if (!s_open) {
        return -OD_ERR_SPI_RAW_NOT_OPEN;
    }
    /* Reports the timeout back as the elapsed time and "still busy", i.e. the timed-out case:
     * it is the shape with a non-zero payload in both fields, so a wrong byte order or a
     * dropped flag shows up rather than reading as a plausible zero. */
    if (elapsed_ms_out != NULL) {
        *elapsed_ms_out = (uint16_t)((timeout_ms > 0xFFFFu) ? 0xFFFFu : timeout_ms);
    }
    if (still_busy_out != NULL) {
        *still_busy_out = true;
    }
    return 0;
}

int opendisplay_display_spi_raw_reset(uint8_t hold_ms)
{
    (void)hold_ms;
    return s_open ? 0 : -OD_ERR_SPI_RAW_NOT_OPEN;
}

void opendisplay_display_spi_raw_close(void)
{
    if (s_open) {
        ++od_test_spi_raw_closes;
    }
    s_open = false;
}

#endif
