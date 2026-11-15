#ifndef OD_TEST_FAKE_SILABS_SLEEPTIMER_H
#define OD_TEST_FAKE_SILABS_SLEEPTIMER_H

#include <stdint.h>

#include "sl_status.h"

#ifdef __cplusplus
extern "C" {
#endif

uint32_t sl_sleeptimer_get_tick_count(void);
uint32_t sl_sleeptimer_tick_to_ms(uint32_t ticks);
uint64_t sl_sleeptimer_get_tick_count64(void);
sl_status_t sl_sleeptimer_tick64_to_ms(uint64_t ticks, uint64_t *ms);
void sl_sleeptimer_delay_millisecond(uint16_t time_ms);

/* Timer surface. The fake never fires a callback on its own: a test decides when a scheduled
 * delay has elapsed, which is what makes "did this call yield?" observable. */
typedef struct sl_sleeptimer_timer_handle {
    int in_use;
} sl_sleeptimer_timer_handle_t;

typedef void (*sl_sleeptimer_timer_callback_t)(sl_sleeptimer_timer_handle_t *handle, void *data);

sl_status_t sl_sleeptimer_start_timer_ms(sl_sleeptimer_timer_handle_t *handle, uint32_t ms,
                                         sl_sleeptimer_timer_callback_t cb, void *data,
                                         uint8_t priority, uint16_t option_flags);
sl_status_t sl_sleeptimer_stop_timer(sl_sleeptimer_timer_handle_t *handle);

#ifdef __cplusplus
}
#endif

#endif /* OD_TEST_FAKE_SILABS_SLEEPTIMER_H */
