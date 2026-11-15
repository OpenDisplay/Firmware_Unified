/* See fake_esp32.h. */
#include "fake_esp32.h"

#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/task.h"

#include <stdlib.h>
#include <string.h>

#define GUARD 64u
#define GUARD_BYTE 0xA5u
#define MAX_BLOCKS 64u
#define MAX_PINS 64

uint16_t fake_esp32_rx_word;
int fake_esp32_input_level = 1;

static int s_pin[MAX_PINS];
static bool s_pins_init;
static int64_t s_now_us;

static struct { uint8_t *p; size_t n; } s_blk[MAX_BLOCKS];

static void pins_init(void)
{
    if (!s_pins_init) {
        for (int i = 0; i < MAX_PINS; i++) {
            s_pin[i] = -1;
        }
        s_pins_init = true;
    }
}

int fake_esp32_pin_level(int pin)
{
    pins_init();
    return pin >= 0 && pin < MAX_PINS ? s_pin[pin] : -1;
}

void fake_esp32_reset(void)
{
    s_pins_init = false;
    pins_init();
    fake_esp32_input_level = 1;
    fake_esp32_rx_word = 0u;
}

esp_err_t gpio_reset_pin(gpio_num_t pin) { (void)pin; return ESP_OK; }
esp_err_t gpio_set_direction(gpio_num_t pin, gpio_mode_t mode) { (void)pin; (void)mode; return ESP_OK; }

esp_err_t gpio_set_level(gpio_num_t pin, uint32_t level)
{
    pins_init();
    if (pin >= 0 && pin < MAX_PINS) {
        s_pin[pin] = level ? 1 : 0;
    }
    return ESP_OK;
}

int gpio_get_level(gpio_num_t pin) { (void)pin; return fake_esp32_input_level; }

static int s_dev_token;

esp_err_t spi_bus_initialize(spi_host_device_t h, const spi_bus_config_t *c, int d)
{
    (void)h; (void)c; (void)d;
    return ESP_OK;
}
esp_err_t spi_bus_free(spi_host_device_t h) { (void)h; return ESP_OK; }

esp_err_t spi_bus_add_device(spi_host_device_t h, const spi_device_interface_config_t *c,
                             spi_device_handle_t *out)
{
    (void)h; (void)c;
    *out = (spi_device_handle_t)&s_dev_token;
    return ESP_OK;
}
esp_err_t spi_bus_remove_device(spi_device_handle_t d) { (void)d; return ESP_OK; }

esp_err_t spi_device_polling_transmit(spi_device_handle_t d, spi_transaction_t *t)
{
    (void)d;
    if ((t->flags & SPI_TRANS_USE_RXDATA) != 0u) {
        t->rx_data[0] = (uint8_t)(fake_esp32_rx_word >> 8);
        t->rx_data[1] = (uint8_t)fake_esp32_rx_word;
    }
    return ESP_OK;
}

void *heap_caps_malloc(size_t size, uint32_t caps)
{
    (void)caps;
    for (unsigned i = 0; i < MAX_BLOCKS; i++) {
        if (s_blk[i].p == NULL) {
            uint8_t *p = (uint8_t *)malloc(size + GUARD);

            if (p == NULL) {
                return NULL;
            }
            memset(p + size, GUARD_BYTE, GUARD);
            s_blk[i].p = p;
            s_blk[i].n = size;
            return p;
        }
    }
    return NULL;
}

void *heap_caps_aligned_alloc(size_t align, size_t size, uint32_t caps)
{
    (void)align;
    return heap_caps_malloc(size, caps);
}

void heap_caps_free(void *p)
{
    for (unsigned i = 0; p != NULL && i < MAX_BLOCKS; i++) {
        if (s_blk[i].p == p) {
            free(p);
            s_blk[i].p = NULL;
            return;
        }
    }
}

size_t fake_esp32_live_blocks(void)
{
    size_t n = 0u;

    for (unsigned i = 0; i < MAX_BLOCKS; i++) {
        n += s_blk[i].p != NULL;
    }
    return n;
}

bool fake_esp32_guards_intact(void)
{
    for (unsigned i = 0; i < MAX_BLOCKS; i++) {
        for (unsigned k = 0; s_blk[i].p != NULL && k < GUARD; k++) {
            if (s_blk[i].p[s_blk[i].n + k] != GUARD_BYTE) {
                return false;
            }
        }
    }
    return true;
}

int64_t esp_timer_get_time(void)
{
    s_now_us += 1000;
    return s_now_us;
}

void vTaskDelay(TickType_t ticks) { s_now_us += (int64_t)ticks * 1000; }
