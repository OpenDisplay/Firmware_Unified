/* od_controller_esp -- which built-in controller driver, if any, drives display 0, and with which
 * parameters.
 *
 * Read from the 0xB3 display_controller packet (docs/CONFIG_DRIVEN_DISPLAY.md, "Built-in
 * controller drivers") and from nothing else: panel_ic_type never selects a driver. A config
 * without 0xB3 is command-driven, so on an IT8951 / parallel board without drive packets it draws
 * nothing and READ_IDENT reports NEEDS_MIGRATION. panels/panels.yaml `native_controllers` holds
 * the 0xB3 packet for every legacy id of those boards. */
#ifndef OD_CONTROLLER_ESP_H
#define OD_CONTROLLER_ESP_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct DisplayConfig;

/* 0xB3 body byte 2. */
#define OD_CTRL_CLASS_COMMAND  0u   /* a command-driven controller: the drive packets drive it */
#define OD_CTRL_CLASS_IT8951   1u   /* IT8951 over SPI (16-bit word protocol, HRDY, readback) */
#define OD_CTRL_CLASS_PARALLEL 2u   /* no controller: the MCU clocks the waveform (FastEPD) */

/* 0xB3 body byte 3. */
#define OD_CTRL_FLAG_MIRRORED    0x01u   /* panel mounted mirrored: rows reversed */
#define OD_CTRL_FLAG_GRAY16      0x02u   /* 4 bpp frame whatever the colour scheme says */
#define OD_CTRL_FLAG_TEMP_SENSOR 0x04u   /* refresh temperature from the configured sensor */

/* 0xB3 body byte 7, class PARALLEL: the board's panel/bus/PMIC wiring, which FastEPD owns. */
#define OD_CTRL_PARALLEL_INKPLATE5V2 1u
#define OD_CTRL_PARALLEL_INKPLATE10  2u
#define OD_CTRL_PARALLEL_M5PAPERS3   3u

#define OD_CTRL_TEMP_DEFAULT ((int8_t)0x7F)
#define OD_CTRL_PIN_NONE 0xFFu

struct od_controller_desc {
    uint8_t  cls;               /* OD_CTRL_CLASS_* */
    uint8_t  flags;             /* OD_CTRL_FLAG_* */
    uint16_t vcom_mv;           /* 0: keep the controller's stored VCOM */
    int8_t   default_temp_c;    /* without a sensor reading; OD_CTRL_TEMP_DEFAULT: driver's own */
    uint8_t  parallel_panel;    /* OD_CTRL_PARALLEL_*, class PARALLEL only */
    uint8_t  miso_pin;          /* IT8951 read line */
    uint8_t  en_pin;            /* IT8951 board supply enable */
    uint8_t  ite_en_pin;        /* IT8951 controller enable */
    bool     from_packet;       /* a valid 0xB3 for this display was found */
};

/* The description of display 0. Cached; od_controller_esp_reload() after every config load. */
const struct od_controller_desc *od_controller_esp(void);
/* The description for any display (uncached; reads the stored config). */
void od_controller_esp_describe(const struct DisplayConfig *d, struct od_controller_desc *out);
void od_controller_esp_reload(void);

#ifdef __cplusplus
}
#endif

#endif /* OD_CONTROLLER_ESP_H */
