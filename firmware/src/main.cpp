#include "FreeRTOS.h"
#include "eurobot/stm32_board.hpp"
#include "stm32f407xx.h"
#include "task.h"
extern "C" {
void _init() {}
void _fini() {}
std::uint32_t SystemCoreClock = 168000000U;
void SystemInit() {
  SCB->CPACR = SCB->CPACR | (0xfU << 20);
  SCB->VTOR = 0x08000000U;
  __DSB();
  __ISB();
}
void SystemCoreClockUpdate() { SystemCoreClock = 168000000U; }
void eurobot_assert_failed() { eurobot::board::fail_safe(); }
void vApplicationStackOverflowHook(TaskHandle_t, char *) { eurobot::board::fail_safe(); }
void HardFault_Handler() { eurobot::board::fail_safe(); }
void MemManage_Handler() { eurobot::board::fail_safe(); }
void BusFault_Handler() { eurobot::board::fail_safe(); }
void UsageFault_Handler() { eurobot::board::fail_safe(); }
void NMI_Handler() { eurobot::board::fail_safe(); }
void vApplicationGetIdleTaskMemory(StaticTask_t **task, StackType_t **stack, std::uint32_t *size) {
  static StaticTask_t storage;
  static StackType_t memory[256];
  *task = &storage;
  *stack = memory;
  *size = 256;
}
}
[[noreturn]] void eurobot_run();
int main() {
  if (eurobot::board::initialize() != eurobot::Error::ok)
    eurobot::board::fail_safe();
  eurobot_run();
}
