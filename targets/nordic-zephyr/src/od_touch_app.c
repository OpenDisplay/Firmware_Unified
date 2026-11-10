/* Nordic's half of od_touch_app.h, and the wiring that keeps the shared machine scheduled.
 *
 * FIRST CONSUMER OF THE GPIO IRQ SEAM (od_gpio.h). That seam was built dormant for this driver;
 * until now no board image linked a symbol from it.
 *
 * NO BOARD IN THIS FLEET HAS A TOUCH CONTROLLER, so everything here is compiled and never run.
 * docs/HARDWARE_VERIFICATION_CHECKLIST.md carries the open rows; do not cite a build as evidence.
 */

#include "od_touch_app.h"

#include "od_gpio.h"
#include "od_touch_gt911.h"
#include "opendisplay_ble.h"
#include "opendisplay_touch.h"

#include <zephyr/kernel.h>

/* One ISR per controller: od_gpio_config_irq() takes a bare handler with no context pointer, so
 * the controller index has to come from somewhere, and a distinct function per slot is where.
 * This is the GPIO IRQ seam's only caller. */
static volatile uint8_t s_irq_mask;

static void touch_isr_0(void) { s_irq_mask |= 1u << 0; }
static void touch_isr_1(void) { s_irq_mask |= 1u << 1; }
static void touch_isr_2(void) { s_irq_mask |= 1u << 2; }
static void touch_isr_3(void) { s_irq_mask |= 1u << 3; }

static void (*const s_isrs[OD_TOUCH_MAX_CONTROLLERS])(void) = {
	touch_isr_0, touch_isr_1, touch_isr_2, touch_isr_3
};

void od_touch_app_delay_ms(uint16_t ms)
{
	k_msleep((int32_t)ms);
}

void od_touch_app_delay_us(uint32_t us)
{
	k_busy_wait(us);
}

void od_touch_app_gpio_set_mode_output(uint8_t pin)
{
	od_gpio_set_mode_output(pin);
}

void od_touch_app_gpio_config_input(uint8_t pin, bool pull_up)
{
	od_gpio_configure_input(pin, pull_up, /*pull_down=*/false);
}

void od_touch_app_gpio_write(uint8_t pin, bool level_high)
{
	od_gpio_write(pin, level_high);
}

int od_touch_app_gpio_read(uint8_t pin)
{
	uint8_t port;
	uint8_t p;

	/* ASK BEFORE READING -- but only about the pin ENCODING, which is all this can answer.
	 * od_gpio_read() folds every failure to 0 by design (callers test it as a boolean, and an
	 * errno would read as HIGH), so a gpio_pin_get() that fails on a pin which DOES decode is
	 * still indistinguishable from LOW here. On the shipped nRF driver that is latent --
	 * gpio_nrfx_port_get_raw() always succeeds -- but it is a real limit of this seam, not a
	 * distinction it makes. Compare DIVERGENCE_MATRIX 21, where the same fold was live. */
	if (!od_pin_decode(pin, &port, &p)) {
		return -1;
	}
	return od_gpio_read(pin);
}

bool od_touch_app_gpio_attach_int(uint8_t idx, uint8_t pin)
{
	if (idx >= OD_TOUCH_MAX_CONTROLLERS) {
		return false;
	}
	return od_gpio_config_irq(pin, OD_GPIO_EDGE_FALLING, s_isrs[idx]) == 0;
}

void od_touch_app_gpio_detach_int(uint8_t pin)
{
	od_gpio_clear_irq(pin);
}

bool od_touch_app_bus_prepare(uint8_t bus_id)
{
	/* This target re-initialises the bus inside every I2C operation, so there is nothing to
	 * select here and nothing that can fail. ESP32 caches one live IDF bus and must re-select. */
	(void)bus_id;
	return true;
}

void od_touch_app_bus_invalidate(void)
{
	/* Nothing is cached, so nothing to drop -- see od_touch_app_bus_prepare(). */
}

void od_touch_app_irq_consume(uint8_t bits)
{
	if (bits == 0u) {
		return;
	}
	od_gpio_irq_lock();
	s_irq_mask = (uint8_t)(s_irq_mask & ~bits);
	od_gpio_irq_unlock();
}

void od_touch_app_msd_write(uint8_t index, uint8_t value)
{
	opendisplay_ble_set_dynamic_byte(index, value);
}

void od_touch_app_msd_publish(void)
{
	opendisplay_ble_update_msd(true);
}

/* ------------------------------------------------------------------ scheduling */

static uint32_t s_next_due_ms;

void opendisplay_touch_init(void)
{
	const uint32_t now = k_uptime_get_32();

	s_irq_mask = 0u;
	s_next_due_ms = now + od_touch_gt911_init(opendisplay_get_global_config(), now);
}

void opendisplay_touch_process(void)
{
	uint32_t now;
	uint8_t  mask;

	/* AN EDGE PULLS SERVICE FORWARD, so the mask is read before the deadline is tested. Gating
	 * on the deadline alone would leave a controller with a long configured interval unable to
	 * be serviced by its own interrupt until that interval expired. */
	od_gpio_irq_lock();
	mask = s_irq_mask;
	od_gpio_irq_unlock();

	now = k_uptime_get_32();
	if (mask == 0u && (int32_t)(now - s_next_due_ms) < 0) {
		return;
	}

	/* The machine clears bits itself through od_touch_app_irq_consume(), at the instant it acts
	 * on one and immediately for one it never will. Nothing is cleared here: doing it after the
	 * walk is what discarded edges arriving during the I2C. */
	s_next_due_ms = now + od_touch_gt911_service(opendisplay_get_global_config(), now, mask);
}

void opendisplay_touch_resume_after_refresh(void)
{
	const uint32_t now = k_uptime_get_32();

	/* REESTABLISH, NOT RESUME. This target's refresh hook is unpaired -- there is one call site
	 * (opendisplay_display.c) and no suspend anywhere -- so the suspend-counted resume() would
	 * do nothing at all here. The deleted implementation probed the retained PID unconditionally
	 * and fell back to a full reset, which is what this restores. */
	s_next_due_ms = now + od_touch_gt911_reestablish(opendisplay_get_global_config(), now);
}

bool opendisplay_touch_gpio_is_touch_int(uint8_t pin)
{
	return od_touch_gt911_is_int_pin(opendisplay_get_global_config(), pin);
}
