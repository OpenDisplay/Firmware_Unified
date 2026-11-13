/* BG22 half of shared/core/od_claim_app.h (except od_claim_app_draw, opendisplay_display.cpp)
 * and shared/hal/od_hal_reset.h. */

#include "od_claim_app.h"
#include "od_hal_reset.h"

#include <string.h>

#include "em_cmu.h"
#include "em_device.h"
#include "em_gpio.h"
#include "sl_bt_api.h"
#include "sl_udelay.h"
#include "nvm3_default.h"

#include "od_config_asm.h"
#include "od_config_store.h"
#include "opendisplay_ble.h"
#include "opendisplay_config_storage.h"

/* ------------------------------------------------------------------- reset cause --- */

/* EMU->RSTCAUSE accumulates until cleared, so it is read once and cleared (SECURITY_V2 7.2).
 * A POR is reported alone: supply ramp-up may also latch a brown-out flag, and a power-on is
 * what the gesture counts. That reading of RSTCAUSE at power-up is a hardware check (H8). */
uint32_t od_hal_reset_cause(void)
{
  static uint32_t s_cause;
  uint32_t r;
  uint32_t c = 0u;

  if (s_cause != 0u) {
    return s_cause;
  }
  r = EMU->RSTCAUSE;
  EMU->CMD = EMU_CMD_RSTCAUSECLR;
  if ((r & EMU_RSTCAUSE_POR) != 0u) {
    c = OD_HAL_RESET_POWER_ON;
  } else {
    if ((r & EMU_RSTCAUSE_PIN) != 0u) {
      c |= OD_HAL_RESET_PIN;
    }
    if ((r & EMU_RSTCAUSE_EM4) != 0u) {
      c |= OD_HAL_RESET_SLEEP_WAKE;
    }
    if ((r & EMU_RSTCAUSE_WDOG0) != 0u) {
      c |= OD_HAL_RESET_WATCHDOG;
    }
    if ((r & EMU_RSTCAUSE_LOCKUP) != 0u) {
      c |= OD_HAL_RESET_LOCKUP;
    }
    if ((r & EMU_RSTCAUSE_SYSREQ) != 0u) {
      c |= OD_HAL_RESET_SOFTWARE;
    }
    if ((r & (EMU_RSTCAUSE_DVDDBOD | EMU_RSTCAUSE_DVDDLEBOD | EMU_RSTCAUSE_DECBOD |
              EMU_RSTCAUSE_AVDDBOD | EMU_RSTCAUSE_IOVDD0BOD | EMU_RSTCAUSE_VREGIN)) != 0u) {
      c |= OD_HAL_RESET_BROWNOUT;
    }
    if ((r & EMU_RSTCAUSE_DCI) != 0u) {
      c |= OD_HAL_RESET_DEBUG;
    }
    if (c == 0u) {
      c = OD_HAL_RESET_OTHER;
    }
  }
  s_cause = c;
  return c;
}

/* ------------------------------------------------------------------ retained RAM --- */

/* BURAM retention registers 28-31 (16 bytes): kept across resets and EM4, lost on POR. No SDK
 * component this build links uses BURAM. */
#define OD_BURAM_FIRST 28u

bool od_hal_retained_read(uint8_t buf[OD_HAL_RETAINED_LEN])
{
  unsigned i;

  CMU_ClockEnable(cmuClock_BURAM, true);
  for (i = 0; i < 4u; ++i) {
    const uint32_t v = BURAM->RET[OD_BURAM_FIRST + i].REG;
    memcpy(&buf[4u * i], &v, 4u);
  }
  return true;
}

bool od_hal_retained_write(const uint8_t buf[OD_HAL_RETAINED_LEN])
{
  unsigned i;

  CMU_ClockEnable(cmuClock_BURAM, true);
  for (i = 0; i < 4u; ++i) {
    uint32_t v;
    memcpy(&v, &buf[4u * i], 4u);
    BURAM->RET[OD_BURAM_FIRST + i].REG = v;
  }
  return true;
}

void od_hal_retained_clear(void)
{
  static const uint8_t zero[OD_HAL_RETAINED_LEN];

  (void)od_hal_retained_write(zero);
}

void od_hal_reset_reboot(void)
{
  NVIC_SystemReset();
}

/* --------------------------------------------------------------------- signalling --- */

/* The panel is the primary signal; no LED/buzzer pattern on BG22 (flash). */
void od_claim_app_signal(enum od_claim_signal sig, uint8_t arg)
{
  (void)sig;
  (void)arg;
}

/* ---------------------------------------------------------------------- reset pin --- */

/* The binary-input encoding the button code uses: port in the high nibble, pin in the low. */
static bool pin_decode(uint8_t v, GPIO_Port_TypeDef *port, uint8_t *pin)
{
  const unsigned pr = (unsigned)(v >> 4) & 0x0Fu;
  const unsigned pn = (unsigned)(v & 0x0Fu);

  if (v == 0xFFu || pr > (unsigned)GPIO_PORT_MAX || !GPIO_PORT_PIN_VALID(pr, pn)) {
    return false;
  }
  *port = (GPIO_Port_TypeDef)(gpioPortA + pr);
  *pin = (uint8_t)pn;
  return true;
}

bool od_claim_app_pin_input(uint8_t pin, bool pull_up, bool pull_down)
{
  GPIO_Port_TypeDef port;
  uint8_t p;

  if (!pin_decode(pin, &port, &p)) {
    return false;
  }
  CMU_ClockEnable(cmuClock_GPIO, true);
  if (pull_up || pull_down) {
    GPIO_PinModeSet(port, p, gpioModeInputPull, pull_up ? 1u : 0u);
  } else {
    GPIO_PinModeSet(port, p, gpioModeInput, 0u);
  }
  return true;
}

int od_claim_app_pin_read(uint8_t pin)
{
  GPIO_Port_TypeDef port;
  uint8_t p;

  if (!pin_decode(pin, &port, &p)) {
    return -1;
  }
  return GPIO_PinInGet(port, p) != 0u ? 1 : 0;
}

/* No watchdog runs on BG22. */
void od_claim_app_wait_ms(uint32_t ms)
{
  while (ms > 0u) {
    sl_udelay_wait(1000u);
    --ms;
  }
}

/* ------------------------------------------------------------------ factory reset --- */

bool od_claim_app_erase_images(void)
{
  return opendisplay_ble_erase_images();
}

bool od_claim_app_erase_bonds(void)
{
  /* No FMDN runtime objects are kept outside the config on this target. */
  return sl_bt_sm_delete_bondings() == SL_STATUS_OK;
}

bool od_claim_app_scrub_extras(void)
{
  return true;
}

/* ----------------------------------------------------------------- config workspace --- */

/* The config assembler is the record workspace (opendisplay_config_storage.c). A record save
 * writes its header over the assembler's state words, so every hand-out and every change
 * resets the assembler afterwards. */
uint8_t *od_claim_app_workspace(uint32_t *cap)
{
  struct od_config_asm *a = opendisplay_config_assembler();

  od_config_asm_reset(a);
  *cap = (uint32_t)sizeof(*a);
  return (uint8_t *)a;
}

bool od_claim_app_workspace_busy(void)
{
  return opendisplay_config_assembler()->active;
}

void od_claim_app_config_changed(void)
{
  od_config_asm_reset(opendisplay_config_assembler());
  opendisplay_ble_config_changed();
}
