/* Dual-controller "TI" panels as ATC_BLE_OEPL drives them (atc1441/ATC_BLE_OEPL_CH573, epd_ti.c):
 * the 9.7" 960 x 672 BWR (TC097SC1B8), split into a left and a right controller with one
 * chip-select each (cs = left/master, cs2 = right/slave). Telink-target addition.
 *
 * The sequence is ATC's, transcribed: panel parameters (voltages, timing) are read from the
 * master's OTP at init, with ATC's 9.7" defaults -- and bit-reversed data -- when the read comes
 * back blank. Each 120-byte row goes 60 bytes to the master, then 60 to the slave. Only the
 * master's busy line is waited on, as ATC does.
 *
 * Plane 0 is OpenDisplay's 1 = white; this controller takes ATC's 1 = black, so it is inverted
 * on the way out. Plane 1 (1 = red) goes as is. */

#include "EPD_driver.h"
#include "epd_port.h"

#define TI_M 0x01u   /* left half, cs */
#define TI_S 0x02u   /* right half, cs2 */
#define TI_OTP_LEN 128u
#define TI_TEMPERATURE_C 20   /* ATC's fixed value; the panel has no sensor on this path */

static uint8_t s_otp[TI_OTP_LEN];
static bool s_reverse;          /* OTP read blank: ATC's fallback sends bits LSB first */
static bool s_black;            /* plane being written */
static uint16_t s_col;          /* byte position within the current row */

static void ti_cmd(uint8_t sel, uint8_t cmd, const uint8_t *data, uint8_t n)
{
    epd_io_cs_manual(sel);
    EPD_WriteCmd(cmd);
    if (n != 0u) {
        EPD_WriteData((uint8_t *)data, n);
    }
    epd_io_cs_manual(0u);
}

static void ti_cmd1(uint8_t cmd, uint8_t v)
{
    ti_cmd(TI_M | TI_S, cmd, &v, 1u);
}

static void ti_cmd2(uint8_t cmd, uint8_t a, uint8_t b)
{
    uint8_t d[2] = {a, b};
    ti_cmd(TI_M | TI_S, cmd, d, 2u);
}

static uint8_t reverse_bits(uint8_t b)
{
    b = (uint8_t)((b & 0xF0u) >> 4 | (b & 0x0Fu) << 4);
    b = (uint8_t)((b & 0xCCu) >> 2 | (b & 0x33u) << 2);
    b = (uint8_t)((b & 0xAAu) >> 1 | (b & 0x55u) << 1);
    return b;
}

static void read_otp(void)
{
    uint8_t dummy;

    epd_io_cs_manual(TI_M);
    EPD_WriteCmd(0xB9);
    EPD_ReadData(&dummy, 1u);
    EPD_ReadData(s_otp, TI_OTP_LEN);
    epd_io_cs_manual(0u);
}

/* ATC's "Error reading OTP forcing 9.7 Screen" values. */
static void otp_defaults_970(void)
{
    s_otp[0x10] = 0x09;
    s_otp[0x15] = 0x00; s_otp[0x16] = 0x3B; s_otp[0x17] = 0x00;
    s_otp[0x18] = 0x00; s_otp[0x19] = 0x9F; s_otp[0x1A] = 0x02;
    s_otp[0x0C] = 0x00; s_otp[0x0D] = 0x3B; s_otp[0x0E] = 0x00; s_otp[0x0F] = 0xA8;
    s_otp[0x12] = 0x00; s_otp[0x13] = 0x00; s_otp[0x14] = 0x00;
    s_otp[0x1C] = 0x80; s_otp[0x1D] = 0x00;
    s_otp[0x0B] = 0x25; s_otp[0x1B] = 0x01; s_otp[0x11] = 0x00;
}

static bool otp_blank(void)
{
    uint8_t i;
    for (i = 0x15u; i <= 0x1Au; i++) {
        if (s_otp[i] != 0xFFu) return false;
    }
    return true;
}

void TI97xx_Init(epd_model_t *epd)
{
    (void)epd;
    delay(1);
    read_otp();
    s_reverse = otp_blank();
    if (s_reverse) {
        otp_defaults_970();
    }
    delay(200);
    EPD_Reset(true, 200);
    ti_cmd(TI_M | TI_S, 0x01, &s_otp[0x10], 1u);
    ti_cmd(TI_M | TI_S, 0x13, &s_otp[0x15], 6u);
    ti_cmd(TI_M | TI_S, 0x90, &s_otp[0x0C], 4u);
}

/* cfg as the other drivers read it: high nibble 0 opens a plane, low nibble 0xF = black. */
void TI97xx_WriteRam(epd_model_t *epd, uint8_t cfg, uint8_t *data, uint8_t len)
{
    const uint16_t row = (uint16_t)(epd->width / 8u);
    const uint16_t half = (uint16_t)(row / 2u);
    uint8_t buf[64];

    if ((cfg >> 4) == 0x00u) {
        s_black = (cfg & 0x0Fu) == 0x0Fu;
        ti_cmd(TI_M | TI_S, 0x12, &s_otp[0x12], 3u);
        epd_io_cs_manual(TI_M | TI_S);
        EPD_WriteCmd(s_black ? 0x10 : 0x11);
        epd_io_cs_manual(TI_M);
        s_col = 0u;
    }
    while (len != 0u) {
        uint16_t room = (uint16_t)((s_col < half ? half : row) - s_col);
        uint8_t n = (uint8_t)(len < room ? len : room);
        uint8_t i;

        if (n > sizeof(buf)) n = sizeof(buf);
        for (i = 0u; i < n; i++) {
            uint8_t v = s_black ? (uint8_t)~data[i] : data[i];
            buf[i] = s_reverse ? reverse_bits(v) : v;
        }
        EPD_WriteData(buf, n);
        data += n;
        len = (uint8_t)(len - n);
        s_col = (uint16_t)(s_col + n);
        if (s_col == half) {
            epd_io_cs_manual(TI_S);
        } else if (s_col == row) {
            s_col = 0u;
            epd_io_cs_manual(TI_M);
        }
    }
}

static void pulse_a7(void)
{
    ti_cmd1(0xA7, 0x10);
    delay(100);
    ti_cmd1(0xA7, 0x00);
    delay(100);
}

void TI97xx_Refresh(epd_model_t *epd)
{
    int i;

    (void)epd;
    epd_io_cs_manual(0u);
    ti_cmd1(0x05, 0x7D);
    delay(200);
    ti_cmd1(0x05, 0x00);
    delay(20);
    ti_cmd1(0xD8, s_otp[0x1C]);
    ti_cmd1(0xD6, s_otp[0x1D]);
    pulse_a7();
    ti_cmd1(0x44, 0x00);
    ti_cmd1(0x45, 0x80);
    pulse_a7();
    ti_cmd1(0x44, 0x06);
    ti_cmd1(0x45, (uint8_t)((TI_TEMPERATURE_C + 40) * 2));
    pulse_a7();
    ti_cmd1(0x60, s_otp[0x0B]);
    ti_cmd(TI_M, 0x61, &s_otp[0x1B], 1u);
    ti_cmd1(0x02, s_otp[0x11]);
    for (i = 0; i < 4; i++) {
        ti_cmd1(0x09, 0x1F); ti_cmd2(0x51, 0x50, (uint8_t)(i + 1)); ti_cmd1(0x09, 0x9F); delay(1);
    }
    for (i = 0; i < 10; i++) {
        ti_cmd1(0x09, 0x1F); ti_cmd2(0x51, 0x0A, (uint8_t)(i + 1)); ti_cmd1(0x09, 0x9F); delay(1);
    }
    for (i = 0; i < 10; i++) {
        ti_cmd1(0x09, 0x7F); ti_cmd2(0x51, 0x0A, (uint8_t)(i + 3)); ti_cmd1(0x09, 0xFF); delay(3);
    }
    for (i = 0; i < 7; i++) {
        ti_cmd1(0x09, 0x7F); ti_cmd2(0x51, (uint8_t)(0x09 - i), 0x0C); ti_cmd1(0x09, 0xFF); delay(1);
    }
    ti_cmd1(0x15, 0x3C);
    delay(100);
    EPD_WaitBusy(false, UINT16_MAX);   /* master busy, low while refreshing */
}

void TI97xx_Sleep(epd_model_t *epd)
{
    (void)epd;
    delay(10);
    ti_cmd1(0x09, 0x7F);
    ti_cmd1(0x05, 0x3D);
    ti_cmd1(0x09, 0x7E);
    delay(20);
    ti_cmd1(0x09, 0x00);
    delay(20);
    epd_io_cs_manual(EPD_IO_CS_AUTO);
}

static void fill_plane(epd_model_t *epd, bool black, uint8_t value)
{
    uint8_t buf[120];
    uint32_t left = (uint32_t)(epd->width / 8u) * epd->height;
    uint8_t cfg = black ? 0x0Fu : 0x00u;
    uint8_t i;

    for (i = 0u; i < sizeof(buf); i++) buf[i] = value;
    while (left != 0u) {
        uint8_t n = (uint8_t)(left < sizeof(buf) ? left : sizeof(buf));
        TI97xx_WriteRam(epd, cfg, buf, n);
        cfg |= 0x10u;
        left -= n;
    }
}

void TI97xx_Clear(epd_model_t *epd, bool refresh)
{
    fill_plane(epd, true, 0xFFu);    /* white in OpenDisplay's plane-0 sense */
    fill_plane(epd, false, 0x00u);   /* no red */
    if (refresh) TI97xx_Refresh(epd);
}

int8_t TI97xx_ReadTemp(epd_model_t *epd)
{
    (void)epd;
    return 0;   /* no sensor read on this path; 0 is discarded by the caller */
}

bool TI97xx_ReadBusy(epd_model_t *epd)
{
    (void)epd;
    return EPD_ReadBusy() == false;
}

static void TI97xx_SetWindow(epd_model_t *epd, uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
    (void)epd; (void)x; (void)y; (void)w; (void)h;   /* full frames only */
}

static const epd_driver_t epd_drv_ti97xx = {
    .init = TI97xx_Init,
    .clear = TI97xx_Clear,
    .write_ram = TI97xx_WriteRam,
    .refresh = TI97xx_Refresh,
    .sleep = TI97xx_Sleep,
    .read_temp = TI97xx_ReadTemp,
    .read_busy = TI97xx_ReadBusy,
    .set_window = TI97xx_SetWindow,
};

const epd_model_t epd_ti_970_bwr = {TI_970_BWR, COLOR_BWR, &epd_drv_ti97xx, DRV_IC_TI, 960, 672};
