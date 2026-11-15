#ifndef FAKE_ESP_TIMER_H
#define FAKE_ESP_TIMER_H
#include "fake_esp32.h"
#ifdef __cplusplus
extern "C" {
#endif
/* Advances 1 ms per call, so a wait loop with a timeout always ends. */
int64_t esp_timer_get_time(void);
#ifdef __cplusplus
}
#endif
#endif
