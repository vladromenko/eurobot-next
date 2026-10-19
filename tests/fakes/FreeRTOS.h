#pragma once
#include <cstddef>
#include <cstdint>
using TickType_t = std::uint32_t;
using StackType_t = std::uint32_t;
struct StaticTask_t {
  unsigned unused;
};
struct StaticQueue_t {
  unsigned unused;
};
using QueueHandle_t = void *;
constexpr int pdPASS = 1;
constexpr TickType_t pdMS_TO_TICKS(unsigned value) { return value; }
