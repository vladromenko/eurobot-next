#pragma once
#include "FreeRTOS.h"
inline QueueHandle_t xQueueCreateStatic(unsigned, unsigned, std::uint8_t *, StaticQueue_t *queue) {
  return queue;
}
inline int xQueueReceive(QueueHandle_t, void *, unsigned) { return 0; }
inline int xQueueSend(QueueHandle_t, const void *, unsigned) { return pdPASS; }
