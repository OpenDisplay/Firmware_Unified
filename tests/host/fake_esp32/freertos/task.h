#ifndef FAKE_FREERTOS_TASK_H
#define FAKE_FREERTOS_TASK_H
#include "freertos/FreeRTOS.h"
#ifdef __cplusplus
extern "C" {
#endif
void vTaskDelay(TickType_t ticks);
#define taskYIELD() ((void)0)
#ifdef __cplusplus
}
#endif
#endif
