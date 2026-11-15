#ifndef FAKE_DRIVER_GPIO_H
#define FAKE_DRIVER_GPIO_H
#include "fake_esp32.h"
#ifdef __cplusplus
extern "C" {
#endif
typedef int gpio_num_t;
typedef enum { GPIO_MODE_INPUT = 1, GPIO_MODE_OUTPUT = 2 } gpio_mode_t;
esp_err_t gpio_reset_pin(gpio_num_t pin);
esp_err_t gpio_set_direction(gpio_num_t pin, gpio_mode_t mode);
esp_err_t gpio_set_level(gpio_num_t pin, uint32_t level);
int gpio_get_level(gpio_num_t pin);
#ifdef __cplusplus
}
#endif
#endif
