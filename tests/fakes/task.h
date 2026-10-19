#pragma once
#include "FreeRTOS.h"
inline TickType_t xTaskGetTickCount() { return 0; }
inline void vTaskDelayUntil(TickType_t *, TickType_t) {}
inline void vTaskDelay(TickType_t) {}
inline void *xTaskCreateStatic(void (*)(void *), const char *, std::uint32_t, void *, unsigned,
                               StackType_t *, StaticTask_t *task) {
  return task;
}
inline void vTaskStartScheduler() {}
