/* Host fakes for the ESP-IDF calls the ESP32 native display drivers make (display_it8951.cpp,
 * display_fastepd.cpp). Only what those files use; behaviour is controlled by the test. */
#ifndef FAKE_ESP32_H
#define FAKE_ESP32_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_STATE 0x103

/* Every 16-bit word the fake SPI device clocks back. */
extern uint16_t fake_esp32_rx_word;
/* Level gpio_get_level() reports for every pin. */
extern int fake_esp32_input_level;
/* Last level written per pin (-1 never written). */
int fake_esp32_pin_level(int pin);

/* Allocations: each block carries guard bytes after its requested size. */
size_t fake_esp32_live_blocks(void);
bool fake_esp32_guards_intact(void);
void fake_esp32_reset(void);

#ifdef __cplusplus
}
#endif
#endif
