#include "od_board.h"
#include "od_gpio.h"

#include <zephyr/sys/util.h>

#if defined(CONFIG_USB_DEVICE_STACK_NEXT) && \
	defined(CONFIG_CDC_ACM_SERIAL_INITIALIZE_AT_BOOT)
#include "od_log.h"

#include <errno.h>
#include <string.h>

#include <nrfx_power.h>
#include <zephyr/drivers/usb/udc.h>
#include <zephyr/kernel.h>
#include <zephyr/sys/iterable_sections.h>
#include <zephyr/usb/usbd.h>
#endif

#define NRF52840_EPD_BS_PIN 13u /* absolute P0.13, inherited board contract */

BUILD_ASSERT(IS_ENABLED(CONFIG_OD_PLATFORM_NRF52840),
	     "nRF52840 board support compiled for the wrong platform");

#if defined(CONFIG_USB_DEVICE_STACK_NEXT) && \
	defined(CONFIG_CDC_ACM_SERIAL_INITIALIZE_AT_BOOT)
#define OD_USB_ENABLE_RETRY_MS 250u
#define OD_USB_SUPERVISE_MS   1000u
#define OD_USB_ENABLE_ATTEMPTS 3u

static struct usbd_context *s_usbd;
static uint8_t s_usb_enable_attempts;

static void usb_power_work_handler(struct k_work *work);
static K_WORK_DELAYABLE_DEFINE(s_usb_power_work, usb_power_work_handler);

static void usb_power_schedule(uint32_t delay_ms)
{
	(void)k_work_reschedule(&s_usb_power_work, K_MSEC(delay_ms));
}

static void usb_power_work_handler(struct k_work *work)
{
	bool power_present;
	bool enabled;
	int err;

	ARG_UNUSED(work);

	power_present = nrfx_power_usbstatus_get() != NRFX_POWER_USB_STATE_DISCONNECTED;
	enabled = udc_is_enabled(s_usbd->dev);

	if (!power_present) {
		s_usb_enable_attempts = 0u;
		if (!enabled) {
			return;
		}

		err = usbd_disable(s_usbd);
		if (err != 0 && err != -EALREADY) {
			/* The stack marks the controller disabled even when driver cleanup fails,
			 * so retrying cannot finish that cleanup. */
			od_log_error("usb: disable failed: %d", err);
		}
		return;
	}

	if (enabled) {
		usb_power_schedule(OD_USB_SUPERVISE_MS);
		return;
	}

	if (s_usb_enable_attempts >= OD_USB_ENABLE_ATTEMPTS) {
		return;
	}

	s_usb_enable_attempts++;
	err = usbd_enable(s_usbd);
	if (err == 0 || err == -EALREADY) {
		usb_power_schedule(OD_USB_SUPERVISE_MS);
		return;
	}

	if (s_usb_enable_attempts == 1u) {
		od_log_error("usb: enable failed: %d", err);
	}
	if (s_usb_enable_attempts < OD_USB_ENABLE_ATTEMPTS) {
		usb_power_schedule(OD_USB_ENABLE_RETRY_MS);
	}
}

static void usb_power_msg_cb(struct usbd_context *const ctx,
			     const struct usbd_msg *const msg)
{
	ARG_UNUSED(ctx);

	if (msg->type == USBD_MSG_VBUS_READY || msg->type == USBD_MSG_VBUS_REMOVED) {
		usb_power_schedule(0u);
	}
}

static void usb_power_enable_fallback(void)
{
	int err = usbd_enable(s_usbd);

	if (err != 0 && err != -EALREADY) {
		od_log_error("usb: fallback enable failed: %d", err);
	}
}

static void usb_power_init(void)
{
	struct usbd_context *only = NULL;
	struct usbd_context *named = NULL;
	size_t count = 0u;
	int err;

	STRUCT_SECTION_FOREACH(usbd_context, ctx) {
		only = ctx;
		count++;
		if (strcmp(ctx->name, "cdc_acm_serial") == 0) {
			named = ctx;
		}
	}

	if (named != NULL) {
		s_usbd = named;
	} else if (count == 1u) {
		s_usbd = only;
		od_log_warn("usb: using sole context %s", s_usbd->name);
	} else {
		od_log_error("usb: cdc_acm_serial context unavailable (%u found)",
			     (unsigned)count);
		return;
	}

	if (!usbd_can_detect_vbus(s_usbd)) {
		od_log_error("usb: controller cannot detect VBUS; enabling continuously");
		usb_power_enable_fallback();
		return;
	}

	err = usbd_msg_register_cb(s_usbd, usb_power_msg_cb);
	if (err != 0) {
		od_log_error("usb: message callback registration failed: %d", err);
		usb_power_enable_fallback();
		return;
	}

	/* Read the level after callback registration so a cable already present at
	 * application start does not depend on an earlier edge notification. */
	usb_power_schedule(0u);
}
#endif

const char *od_board_name(void)
{
	return "XIAO nRF52840";
}

void od_board_early_init(void)
{
	/* The XIAO's QSPI flash is on the always-on 3V3 and nothing here uses it. The driver only
	 * enters deep power-down under CONFIG_PM_DEVICE, so standby would otherwise be permanent. */
	od_board_flash_powerdown(0x14u, 0x15u, 0x19u, 0x18u, 0x16u, 0x17u); /* P0.20/21/25/24/22/23 */
	/* The donor Bluefruit firmware's xiaoinit() drives this line low before
	 * any display work. It selects the EPD boost path on deployed hardware. */
	od_gpio_configure_output(NRF52840_EPD_BS_PIN, false);
#if defined(CONFIG_USB_DEVICE_STACK_NEXT) && \
	defined(CONFIG_CDC_ACM_SERIAL_INITIALIZE_AT_BOOT)
	usb_power_init();
#endif
}

void od_board_prepare_epd_rail(void)
{
	od_gpio_configure_output(NRF52840_EPD_BS_PIN, false);
}

bool od_board_epd_requires_cold_cycle(void)
{
	/* Matches prepareEpdRailForBoot() in the donor firmware. */
	return true;
}

bool od_board_epd_pin_reserved(uint8_t port, uint8_t pin)
{
	/* P0.13 selects the EPD boost path. P0.20..25 belong to the
	 * board's enabled QSPI flash. */
	return port == 0u && (pin == 13u || (pin >= 20u && pin <= 25u));
}

bool od_board_spim_pin_ok(uint8_t sck_port, uint8_t sck_pin,
			  uint8_t mosi_port, uint8_t mosi_pin)
{
	(void)sck_pin;
	(void)mosi_pin;
	return sck_port <= 1u && mosi_port <= 1u;
}
