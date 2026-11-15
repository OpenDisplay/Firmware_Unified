#include <assert.h>
#include <stdbool.h>
#include <stdint.h>

#include "od_board.h"
#include "od_gpio.h"

uint32_t fake_k_sleep_ms;

#if defined(OD_TEST_POLICY_NRF52840)
void od_board_flash_powerdown(uint8_t mosi_cfg, uint8_t sck_cfg, uint8_t cs_cfg,
                              uint8_t miso_cfg, uint8_t wp_cfg, uint8_t hold_cfg)
{
    (void)mosi_cfg; (void)sck_cfg; (void)cs_cfg; (void)miso_cfg; (void)wp_cfg; (void)hold_cfg;
}
#endif

void k_msleep(int32_t ms)
{
	fake_k_sleep_ms += (uint32_t)ms;
}

void od_gpio_configure_output(uint8_t cfg, bool initial_high)
{
	(void)cfg;
	(void)initial_high;
}

void od_gpio_write(uint8_t cfg, bool level_high)
{
	(void)cfg;
	(void)level_high;
}

void od_gpio_park(uint8_t cfg)
{
	(void)cfg;
}

int main(void)
{
#if defined(OD_TEST_POLICY_NRF52840)
	assert(od_board_spim_pin_ok(1u, 13u, 1u, 15u));
	assert(od_board_spim_pin_ok(0u, 29u, 0u, 28u));
	assert(!od_board_spim_pin_ok(2u, 1u, 0u, 2u));
	assert(od_board_epd_pin_reserved(0u, 13u));
	assert(od_board_epd_pin_reserved(0u, 20u));
	assert(od_board_epd_pin_reserved(0u, 25u));
	assert(!od_board_epd_pin_reserved(1u, 13u));
#elif defined(OD_TEST_POLICY_NRF54L15)
	assert(od_board_spim_pin_ok(2u, 1u, 2u, 2u));
	assert(od_board_spim_pin_ok(2u, 6u, 2u, 8u));
	assert(!od_board_spim_pin_ok(2u, 1u, 2u, 8u));
	assert(!od_board_spim_pin_ok(2u, 6u, 2u, 2u));
	assert(od_board_epd_pin_reserved(2u, 3u));
	assert(od_board_epd_pin_reserved(2u, 5u));
	assert(od_board_epd_pin_reserved(2u, 10u));
	assert(!od_board_epd_pin_reserved(2u, 4u));
#elif defined(OD_TEST_POLICY_NRF54LM20A)
	assert(od_board_spim_pin_ok(1u, 4u, 1u, 6u));
	assert(od_board_spim_pin_ok(1u, 3u, 1u, 5u));
	assert(!od_board_spim_pin_ok(1u, 4u, 1u, 5u));
	assert(!od_board_spim_pin_ok(3u, 4u, 3u, 6u));
	for (uint8_t pin = 10u; pin <= 14u; ++pin) {
		assert(od_board_epd_pin_reserved(1u, pin));
	}
	assert(!od_board_epd_pin_reserved(1u, 9u));
#else
#error "No EPD policy selected"
#endif
	return 0;
}
