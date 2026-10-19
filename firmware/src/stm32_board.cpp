#include "eurobot/stm32_board.hpp"
#include "stm32f407xx.h"
#include <atomic>
#include <cmath>
namespace {
using eurobot::Error;
namespace connector = eurobot::board::connectors;
GPIO_TypeDef *gpio_port(connector::Port port) {
  return std::array<GPIO_TypeDef *, 5>{GPIOA, GPIOB, GPIOC, GPIOD,
                                       GPIOE}[static_cast<unsigned>(port)];
}
struct Serial {
  USART_TypeDef *uart;
  GPIO_TypeDef *de;
  std::uint32_t pin;
};
Serial motors{USART6, GPIOC, 7}, servos{USART3, GPIOD, 9}, battery{UART4, GPIOA, 15};
std::array<std::uint8_t, 1024> rx_ring{};
std::atomic<std::uint32_t> rx_head{}, rx_tail{};
std::array<std::uint8_t, 256> debug_ring{};
std::atomic<std::uint32_t> debug_head{}, debug_tail{};
std::atomic<bool> debug_overflow{};
std::uint32_t boot_reset_flags{};
std::atomic<bool> overflow{}, step_error{};
void *step_context{};
Error (*step_tick)(void *, std::uint32_t){};
alignas(4) std::array<std::uint16_t, 60 * 24 + 400> led_dma{};
static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
using Stepper = connector::Stepper;
auto steppers = connector::steppers;
void bit_set(volatile std::uint32_t &reg, std::uint32_t bits) { reg = reg | bits; }
void bit_clear(volatile std::uint32_t &reg, std::uint32_t bits) { reg = reg & ~bits; }
void pin_write(GPIO_TypeDef *port, unsigned pin, bool on) {
  port->BSRR = 1U << (pin + (on ? 0 : 16));
}
void pin_mode(GPIO_TypeDef *port, unsigned pin, unsigned mode, unsigned af = 0, bool open = false) {
  port->MODER = (port->MODER & ~(3U << (pin * 2))) | (mode << (pin * 2));
  port->OSPEEDR = (port->OSPEEDR & ~(3U << (pin * 2))) | (2U << (pin * 2));
  port->PUPDR = port->PUPDR & ~(3U << (pin * 2));
  port->OTYPER = (port->OTYPER & ~(1U << pin)) | (open ? 1U << pin : 0);
  if (mode == 2)
    port->AFR[pin / 8] = (port->AFR[pin / 8] & ~(15U << ((pin % 8) * 4))) | (af << ((pin % 8) * 4));
}
template <class Predicate>
bool wait_until(Predicate predicate, std::uint32_t started, std::uint32_t timeout) {
  while (!predicate()) {
    if (static_cast<std::uint32_t>(TIM2->CNT - started) >= timeout)
      return false;
  }
  return true;
}
bool boot_wait(volatile std::uint32_t &reg, std::uint32_t mask, std::uint32_t value) {
  for (std::uint32_t n = 0; n < 5000000; ++n) {
    if ((reg & mask) == value)
      return true;
  }
  return false;
}
void uart_init(USART_TypeDef *u, std::uint32_t clock, std::uint32_t baud, bool half) {
  u->CR1 = 0;
  u->CR2 = 0;
  u->CR3 = half ? USART_CR3_HDSEL : 0;
  u->BRR = (clock + baud / 2) / baud;
  u->CR1 = USART_CR1_UE | USART_CR1_RE | (half ? 0 : USART_CR1_TE);
}
Error uart_tx(USART_TypeDef *uart, std::span<const std::uint8_t> data, std::uint32_t start,
              std::uint32_t timeout) {
  for (auto byte : data) {
    if (!wait_until([&] { return (uart->SR & USART_SR_TXE) != 0; }, start, timeout))
      return Error::timeout;
    uart->DR = byte;
  }
  return wait_until([&] { return (uart->SR & USART_SR_TC) != 0; }, start, timeout) ? Error::ok
                                                                                   : Error::timeout;
}
Error spi_byte(std::uint8_t tx, std::uint8_t &rx, std::uint32_t start, std::uint32_t timeout) {
  if (!wait_until([] { return (SPI1->SR & SPI_SR_TXE) != 0; }, start, timeout))
    return Error::timeout;
  *reinterpret_cast<volatile std::uint8_t *>(&SPI1->DR) = tx;
  if (!wait_until([] { return (SPI1->SR & SPI_SR_RXNE) != 0; }, start, timeout))
    return Error::timeout;
  rx = static_cast<std::uint8_t>(SPI1->DR);
  return wait_until([] { return (SPI1->SR & SPI_SR_BSY) == 0; }, start, timeout) ? Error::ok
                                                                                 : Error::timeout;
}
Error l6474_transfer(void *ctx, std::span<const std::uint8_t> tx, std::span<std::uint8_t> rx,
                     std::uint32_t timeout) {
  if (!rx.empty() && tx.size() != rx.size())
    return Error::invalid_argument;
  auto &s = *static_cast<Stepper *>(ctx);
  auto start = TIM2->CNT;
  Error result = Error::ok;
  bit_clear(SPI1->CR1, SPI_CR1_SPE);
  SPI1->CR1 = (SPI1->CR1 & ~SPI_CR1_BR) | (4U << 3) | SPI_CR1_SPE;
  for (std::size_t i = 0; i < tx.size() && result == Error::ok; ++i) {
    pin_write(gpio_port(s.chip_select.port), s.chip_select.bit, false);
    (void)eurobot::board::delay_us(1);
    std::uint8_t received = 0;
    result = spi_byte(tx[i], received, start, timeout);
    if (!rx.empty())
      rx[i] = received;
    pin_write(gpio_port(s.chip_select.port), s.chip_select.bit, true);
    (void)eurobot::board::delay_us(1);
  }
  bit_clear(SPI1->CR1, SPI_CR1_SPE);
  SPI1->CR1 = (SPI1->CR1 & ~SPI_CR1_BR) | SPI_CR1_SPE;
  return result;
}
Error step_config(void *ctx, bool forward, std::uint32_t period) {
  if (period < 4)
    return Error::invalid_argument;
  auto &s = *static_cast<Stepper *>(ctx);
  pin_write(gpio_port(s.direction.port), s.direction.bit, forward);
  return eurobot::board::delay_us(2);
}
Error step_pulse(void *ctx) {
  auto &s = *static_cast<Stepper *>(ctx);
  pin_write(gpio_port(s.step.port), s.step.bit, true);
  auto e = eurobot::board::delay_us(2);
  pin_write(gpio_port(s.step.port), s.step.bit, false);
  return e;
}
Error step_stop(void *ctx) {
  auto &s = *static_cast<Stepper *>(ctx);
  pin_write(gpio_port(s.step.port), s.step.bit, false);
  return Error::ok;
}
Error driver_direction(void *ctx, bool forward) {
  auto &s = *static_cast<Stepper *>(ctx);
  pin_write(gpio_port(s.direction.port), s.direction.bit, forward);
  return eurobot::board::delay_us(2);
}
Error i2c_abort(Error e) {
  bit_set(I2C2->CR1, I2C_CR1_STOP);
  I2C2->SR1 = I2C2->SR1 & ~(I2C_SR1_AF | I2C_SR1_BERR | I2C_SR1_ARLO | I2C_SR1_OVR);
  bit_clear(I2C2->CR1, I2C_CR1_POS);
  bit_set(I2C2->CR1, I2C_CR1_ACK);
  return e;
}
bool i2c_flag(std::uint32_t flag, std::uint32_t start, std::uint32_t timeout) {
  while ((I2C2->SR1 & flag) == 0) {
    if ((I2C2->SR1 & (I2C_SR1_AF | I2C_SR1_BERR | I2C_SR1_ARLO | I2C_SR1_OVR)) != 0 ||
        static_cast<std::uint32_t>(TIM2->CNT - start) >= timeout)
      return false;
  }
  return true;
}
Error i2c_begin(std::uint8_t address, bool read, std::uint32_t start, std::uint32_t timeout) {
  bit_set(I2C2->CR1, I2C_CR1_START);
  if (!i2c_flag(I2C_SR1_SB, start, timeout))
    return i2c_abort(Error::timeout);
  I2C2->DR = (address << 1) | (read ? 1 : 0);
  if (!i2c_flag(I2C_SR1_ADDR, start, timeout))
    return i2c_abort(Error::output_failure);
  return Error::ok;
}
void clear_addr() {
  [[maybe_unused]] volatile auto discard = I2C2->SR1;
  discard = I2C2->SR2;
}
Error i2c_write(void *, std::uint8_t addr, std::uint8_t reg, std::span<const std::uint8_t> bytes,
                std::uint32_t timeout) {
  auto start = TIM2->CNT;
  if (!wait_until([] { return (I2C2->SR2 & I2C_SR2_BUSY) == 0; }, start, timeout))
    return i2c_abort(Error::timeout);
  auto e = i2c_begin(addr, false, start, timeout);
  if (e != Error::ok)
    return e;
  clear_addr();
  I2C2->DR = reg;
  for (auto v : bytes) {
    if (!i2c_flag(I2C_SR1_TXE, start, timeout))
      return i2c_abort(Error::timeout);
    I2C2->DR = v;
  }
  if (!i2c_flag(I2C_SR1_BTF, start, timeout))
    return i2c_abort(Error::timeout);
  bit_set(I2C2->CR1, I2C_CR1_STOP);
  return Error::ok;
}
Error i2c_read(void *, std::uint8_t addr, std::uint8_t reg, std::span<std::uint8_t> bytes,
               std::uint32_t timeout) {
  if (bytes.empty())
    return Error::invalid_argument;
  auto start = TIM2->CNT;
  if (!wait_until([] { return (I2C2->SR2 & I2C_SR2_BUSY) == 0; }, start, timeout))
    return i2c_abort(Error::timeout);
  auto e = i2c_begin(addr, false, start, timeout);
  if (e != Error::ok)
    return e;
  clear_addr();
  I2C2->DR = reg;
  if (!i2c_flag(I2C_SR1_BTF, start, timeout))
    return i2c_abort(Error::timeout);
  e = i2c_begin(addr, true, start, timeout);
  if (e != Error::ok)
    return e;
  if (bytes.size() == 1) {
    bit_clear(I2C2->CR1, I2C_CR1_ACK);
    auto primask = __get_PRIMASK();
    __disable_irq();
    clear_addr();
    bit_set(I2C2->CR1, I2C_CR1_STOP);
    __set_PRIMASK(primask);
    if (!i2c_flag(I2C_SR1_RXNE, start, timeout))
      return i2c_abort(Error::timeout);
    bytes[0] = static_cast<std::uint8_t>(I2C2->DR);
  } else if (bytes.size() == 2) {
    bit_set(I2C2->CR1, I2C_CR1_POS);
    auto primask = __get_PRIMASK();
    __disable_irq();
    clear_addr();
    bit_clear(I2C2->CR1, I2C_CR1_ACK);
    __set_PRIMASK(primask);
    if (!i2c_flag(I2C_SR1_BTF, start, timeout))
      return i2c_abort(Error::timeout);
    primask = __get_PRIMASK();
    __disable_irq();
    bit_set(I2C2->CR1, I2C_CR1_STOP);
    bytes[0] = static_cast<std::uint8_t>(I2C2->DR);
    bytes[1] = static_cast<std::uint8_t>(I2C2->DR);
    __set_PRIMASK(primask);
  } else {
    bit_set(I2C2->CR1, I2C_CR1_ACK);
    clear_addr();
    std::size_t i = 0;
    while (bytes.size() - i > 3) {
      if (!i2c_flag(I2C_SR1_RXNE, start, timeout))
        return i2c_abort(Error::timeout);
      bytes[i++] = static_cast<std::uint8_t>(I2C2->DR);
    }
    if (!i2c_flag(I2C_SR1_BTF, start, timeout))
      return i2c_abort(Error::timeout);
    bit_clear(I2C2->CR1, I2C_CR1_ACK);
    auto primask = __get_PRIMASK();
    __disable_irq();
    bytes[i++] = static_cast<std::uint8_t>(I2C2->DR);
    __set_PRIMASK(primask);
    if (!i2c_flag(I2C_SR1_BTF, start, timeout))
      return i2c_abort(Error::timeout);
    primask = __get_PRIMASK();
    __disable_irq();
    bit_set(I2C2->CR1, I2C_CR1_STOP);
    bytes[i++] = static_cast<std::uint8_t>(I2C2->DR);
    __set_PRIMASK(primask);
    if (!i2c_flag(I2C_SR1_RXNE, start, timeout))
      return i2c_abort(Error::timeout);
    bytes[i] = static_cast<std::uint8_t>(I2C2->DR);
  }
  bit_clear(I2C2->CR1, I2C_CR1_POS);
  bit_set(I2C2->CR1, I2C_CR1_ACK);
  return Error::ok;
}
Error i2c_delay(void *, std::uint32_t us) { return eurobot::board::delay_us(us); }
TIM_TypeDef *pwm_timer(std::uint8_t ch) { return connector::pwm[ch].timer == 4 ? TIM4 : TIM5; }
unsigned pwm_index(std::uint8_t ch) { return connector::pwm[ch].channel - 1; }
void pwm_init(std::uint8_t ch, std::uint32_t period) {
  auto *timer = pwm_timer(ch);
  auto index = pwm_index(ch);
  bool initialize = (timer->CR1 & TIM_CR1_CEN) == 0 || timer->ARR != period - 1;
  timer->PSC = 83;
  timer->ARR = period - 1;
  auto &ccmr = index < 2 ? timer->CCMR1 : timer->CCMR2;
  auto shift = (index % 2) * 8;
  ccmr = (ccmr & ~(0xffU << shift)) | ((6U << 4 | 1U << 3) << shift);
  if (initialize)
    timer->EGR = TIM_EGR_UG;
  bit_set(timer->CR1, TIM_CR1_CEN);
}
void pwm_compare(std::uint8_t ch, std::uint32_t compare) {
  auto *timer = pwm_timer(ch);
  auto idx = pwm_index(ch);
  std::array<volatile std::uint32_t *, 4>{&timer->CCR1, &timer->CCR2, &timer->CCR3,
                                          &timer->CCR4}[idx][0] = compare;
  timer->CCER = timer->CCER | (1U << (idx * 4));
  const auto pin = connector::pwm[ch].pin;
  pin_mode(gpio_port(pin.port), pin.bit, 2, 2);
}
} // namespace
namespace eurobot::board {
Error initialize() noexcept {
  boot_reset_flags = RCC->CSR & 0xfe000000U;
  bit_set(RCC->AHB1ENR, 0x1f);
  [[maybe_unused]] auto sync = RCC->AHB1ENR;
  for (auto &s : steppers) {
    pin_write(gpio_port(s.chip_select.port), s.chip_select.bit, true);
    pin_mode(gpio_port(s.chip_select.port), s.chip_select.bit, 1);
    pin_write(gpio_port(s.step.port), s.step.bit, false);
    pin_mode(gpio_port(s.step.port), s.step.bit, 1);
    pin_mode(gpio_port(s.direction.port), s.direction.bit, 1);
  }
  for (const auto &channel : connector::pwm) {
    pin_write(gpio_port(channel.pin.port), channel.pin.bit, false);
    pin_mode(gpio_port(channel.pin.port), channel.pin.bit, 1);
  }
  bit_set(RCC->APB1ENR, RCC_APB1ENR_PWREN);
  PWR->CR = (PWR->CR & ~PWR_CR_VOS) | PWR_CR_VOS;
  FLASH->ACR = FLASH_ACR_LATENCY_5WS | FLASH_ACR_ICEN | FLASH_ACR_DCEN | FLASH_ACR_PRFTEN;
  bit_set(RCC->CR, RCC_CR_HSEON);
  if (!boot_wait(RCC->CR, RCC_CR_HSERDY, RCC_CR_HSERDY))
    return Error::timeout;
  RCC->PLLCFGR = 8U | (168U << 6) | RCC_PLLCFGR_PLLSRC_HSE | (7U << 24);
  bit_set(RCC->CR, RCC_CR_PLLON);
  if (!boot_wait(RCC->CR, RCC_CR_PLLRDY, RCC_CR_PLLRDY))
    return Error::timeout;
  RCC->CFGR = (RCC->CFGR & ~(RCC_CFGR_HPRE | RCC_CFGR_PPRE1 | RCC_CFGR_PPRE2 | RCC_CFGR_SW)) |
              RCC_CFGR_PPRE1_DIV4 | RCC_CFGR_PPRE2_DIV2 | RCC_CFGR_SW_PLL;
  if (!boot_wait(RCC->CFGR, RCC_CFGR_SWS, RCC_CFGR_SWS_PLL))
    return Error::timeout;
  bit_set(RCC->APB1ENR, RCC_APB1ENR_TIM2EN | RCC_APB1ENR_TIM4EN | RCC_APB1ENR_TIM5EN |
                            RCC_APB1ENR_USART3EN | RCC_APB1ENR_UART4EN | RCC_APB1ENR_UART5EN |
                            RCC_APB1ENR_I2C2EN);
  bit_set(RCC->APB2ENR, RCC_APB2ENR_USART1EN | RCC_APB2ENR_USART6EN | RCC_APB2ENR_SPI1EN);
  TIM2->PSC = 83;
  TIM2->ARR = 0xffffffffU;
  TIM2->EGR = TIM_EGR_UG;
  TIM2->CR1 = TIM_CR1_CEN;
  pin_write(GPIOA, 4, true);
  pin_mode(GPIOA, 4, 1);
  pin_mode(GPIOA, 5, 2, 5);
  pin_mode(GPIOA, 6, 2, 5);
  pin_mode(GPIOA, 7, 2, 5);
  SPI1->CR1 = SPI_CR1_MSTR | SPI_CR1_SSM | SPI_CR1_SSI | SPI_CR1_CPOL | SPI_CR1_CPHA | SPI_CR1_SPE;
  auto e = shift_write(0);
  if (e != Error::ok)
    return e;
  pin_mode(gpio_port(connector::host_tx.port), connector::host_tx.bit, 2, 7);
  pin_mode(gpio_port(connector::host_rx.port), connector::host_rx.bit, 2, 7);
  uart_init(USART1, 84000000, 115200, false);
  pin_mode(gpio_port(connector::debug_tx.port), connector::debug_tx.bit, 2, 8);
  pin_mode(gpio_port(connector::debug_rx.port), connector::debug_rx.bit, 2, 8);
  uart_init(UART5, 42000000, 115200, false);
  for (auto *s : {&motors, &servos, &battery}) {
    pin_write(s->de, s->pin, false);
    pin_mode(s->de, s->pin, 1);
  }
  pin_mode(GPIOC, 6, 2, 8);
  uart_init(USART6, 84000000, 2000000, true);
  pin_mode(GPIOD, 8, 2, 7);
  uart_init(USART3, 42000000, 1000000, true);
  pin_mode(GPIOC, 10, 2, 8);
  uart_init(UART4, 42000000, 2000000, true);
  pin_mode(GPIOB, 10, 2, 4, true);
  pin_mode(GPIOB, 11, 2, 4, true);
  I2C2->CR1 = I2C_CR1_SWRST;
  I2C2->CR1 = 0;
  I2C2->CR2 = 42;
  I2C2->CCR = 210;
  I2C2->TRISE = 43;
  I2C2->CR1 = I2C_CR1_PE | I2C_CR1_ACK;
  for (auto pin : connector::inputs)
    pin_mode(gpio_port(pin.port), pin.bit, 0);
  NVIC_SetPriorityGrouping(3);
  NVIC_SetPriority(USART1_IRQn, 6);
  NVIC_ClearPendingIRQ(USART1_IRQn);
  NVIC_EnableIRQ(USART1_IRQn);
  bit_set(USART1->CR1, USART_CR1_RXNEIE);
  return Error::ok;
}
std::uint32_t micros() noexcept { return TIM2->CNT; }
std::uint32_t
millis() noexcept { // Expand the 71-minute hardware wrap into a wrap-safe millisecond clock.
  auto primask = __get_PRIMASK();
  __disable_irq();
  static std::uint32_t previous = 0, total_ms = 0, remainder = 0;
  auto now = TIM2->CNT;
  auto delta = now - previous;
  previous = now;
  auto sum = static_cast<std::uint64_t>(remainder) + delta;
  total_ms += static_cast<std::uint32_t>(sum / 1000);
  remainder = static_cast<std::uint32_t>(sum % 1000);
  auto result = total_ms;
  __set_PRIMASK(primask);
  return result;
}
Error delay_us(std::uint32_t us) noexcept {
  if (us > 10000)
    return Error::invalid_argument;
  auto start = micros();
  while (static_cast<std::uint32_t>(micros() - start) < us) {
  }
  return Error::ok;
}
Error host_write(std::span<const std::uint8_t> bytes, std::uint32_t timeout) noexcept {
  return uart_tx(USART1, bytes, micros(), timeout);
}
void debug_enable(bool enabled) noexcept {
  if (enabled) {
    NVIC_SetPriority(UART5_IRQn, 6);
    NVIC_ClearPendingIRQ(UART5_IRQn);
    NVIC_EnableIRQ(UART5_IRQn);
    bit_set(UART5->CR1, USART_CR1_RXNEIE);
  } else {
    bit_clear(UART5->CR1, USART_CR1_RXNEIE);
    NVIC_DisableIRQ(UART5_IRQn);
  }
}
Error debug_write(std::span<const std::uint8_t> bytes, std::uint32_t timeout) noexcept {
  return uart_tx(UART5, bytes, micros(), timeout);
}
std::optional<std::uint8_t> debug_read() noexcept {
  const auto tail = debug_tail.load(std::memory_order_relaxed);
  if (tail == debug_head.load(std::memory_order_acquire)) {
    return {};
  }
  const auto byte = debug_ring[tail % debug_ring.size()];
  debug_tail.store(tail + 1, std::memory_order_release);
  return byte;
}
std::uint32_t reset_cause() noexcept { return boot_reset_flags; }
void clear_reset_cause() noexcept {
  boot_reset_flags = 0;
  bit_set(RCC->CSR, RCC_CSR_RMVF);
}
[[noreturn]] void system_reset() noexcept { NVIC_SystemReset(); }
std::optional<std::uint8_t> host_read() noexcept {
  auto tail = rx_tail.load(std::memory_order_relaxed);
  if (tail == rx_head.load(std::memory_order_acquire))
    return {};
  auto b = rx_ring[tail % rx_ring.size()];
  rx_tail.store(tail + 1, std::memory_order_release);
  return b;
}
bool receive_overflow() noexcept {
  const auto host = overflow.exchange(false), debug = debug_overflow.exchange(false);
  return host || debug;
}
Error shift_write(std::uint32_t word) noexcept {
  auto start = micros();
  std::uint8_t rx = 0;
  Error e = Error::ok;
  pin_write(GPIOA, 4, false);
  for (unsigned i = 0; i < 3 && e == Error::ok; ++i)
    e = spi_byte(static_cast<std::uint8_t>(word >> (8 * i)), rx, start, 1000);
  if (e == Error::ok)
    pin_write(GPIOA, 4, true);
  return e;
}
Error serial_exchange(void *ctx, std::span<const std::uint8_t> tx, std::span<std::uint8_t> rx,
                      std::size_t &received, std::uint32_t timeout) noexcept {
  received = 0;
  if (timeout == 0 || timeout > 20000)
    return Error::invalid_argument;
  auto &s = *static_cast<Serial *>(ctx);
  auto start = micros();
  while ((s.uart->SR & USART_SR_RXNE) != 0) {
    [[maybe_unused]] auto discard = s.uart->DR;
    if (static_cast<std::uint32_t>(micros() - start) >= timeout)
      return Error::timeout;
  }
  bit_clear(s.uart->CR1, USART_CR1_RE);
  bit_set(s.uart->CR1, USART_CR1_TE);
  pin_write(s.de, s.pin, true);
  auto e = uart_tx(s.uart, tx, start, timeout);
  auto saved = __get_PRIMASK();
  __disable_irq();
  bit_clear(s.uart->CR1, USART_CR1_TE);
  pin_write(s.de, s.pin, false);
  bit_set(s.uart->CR1, USART_CR1_RE);
  __set_PRIMASK(saved);
  if (e != Error::ok || rx.empty())
    return e;
  std::size_t expected = 6;
  while (received < expected) {
    if (!wait_until(
            [&] {
              return (s.uart->SR & (USART_SR_RXNE | USART_SR_ORE | USART_SR_FE | USART_SR_NE)) != 0;
            },
            start, timeout))
      return Error::timeout;
    auto sr = s.uart->SR;
    auto b = static_cast<std::uint8_t>(s.uart->DR);
    if ((sr & (USART_SR_ORE | USART_SR_FE | USART_SR_NE)) != 0)
      return Error::output_failure;
    if (received >= rx.size())
      return Error::malformed;
    rx[received++] = b;
    if (received == 4) {
      expected = static_cast<std::size_t>(b) + 4;
      if (expected > rx.size() || expected < 6)
        return Error::malformed;
    }
  }
  return Error::ok;
}
devices::SerialBus motor_bus() noexcept { return {&motors, serial_exchange}; }
devices::SerialBus servo_bus() noexcept { return {&servos, serial_exchange}; }
devices::SerialBus battery_bus() noexcept { return {&battery, serial_exchange}; }
devices::I2cBus i2c_bus() noexcept { return {nullptr, i2c_write, i2c_read, i2c_delay}; }
devices::SpiBus stepper_bus(std::size_t channel) noexcept {
  return channel < 4 ? devices::SpiBus{&steppers[channel], l6474_transfer} : devices::SpiBus{};
}
devices::StepPulseSink stepper_pulses(std::size_t channel) noexcept {
  return channel < 4
             ? devices::StepPulseSink{&steppers[channel], step_config, step_pulse, step_stop}
             : devices::StepPulseSink{};
}
Error stepper_direction(std::size_t channel, bool forward) noexcept {
  return channel < 4 ? driver_direction(&steppers[channel], forward) : Error::invalid_argument;
}
Error pwm_pulse(std::uint8_t ch, std::uint16_t us) noexcept {
  if (ch >= 8 || us == 0 || us >= 20000)
    return Error::invalid_argument;
  pwm_init(ch, 20000);
  pwm_compare(ch, us);
  return Error::ok;
}
Error pwm_disable(std::uint8_t ch) noexcept {
  if (ch >= 8)
    return Error::invalid_argument;
  auto *timer = pwm_timer(ch);
  bit_clear(timer->CCER, 1U << (pwm_index(ch) * 4));
  const auto pin = connector::pwm[ch].pin;
  pin_write(gpio_port(pin.port), pin.bit, false);
  pin_mode(gpio_port(pin.port), pin.bit, 1);
  return Error::ok;
}
Error pwm_group_frequency(std::uint8_t bank, std::uint32_t hz) noexcept {
  if (bank > 1 || hz < 16 || hz > 1000000 || 1000000U % hz != 0) {
    return Error::invalid_argument;
  }
  auto *timer = bank == 0 ? TIM4 : TIM5;
  if ((timer->CCER & 0x1111U) != 0) {
    return Error::lease_conflict;
  }
  timer->PSC = 83;
  timer->ARR = 1000000U / hz - 1;
  timer->EGR = TIM_EGR_UG;
  timer->CR1 = TIM_CR1_CEN;
  return Error::ok;
}
std::array<std::uint32_t, 2> pwm_group_info(std::uint8_t bank) noexcept {
  if (bank > 1) {
    return {};
  }
  auto *timer = bank == 0 ? TIM4 : TIM5;
  const auto period = timer->ARR + 1;
  return {(timer->CR1 & TIM_CR1_CEN) != 0 && period != 0 ? 1000000U / period : 0U, timer->ARR};
}
Error dc_drive(std::uint8_t ch, float duty, DcMode mode, std::uint32_t frequency_hz) noexcept {
  if (ch >= 4 || frequency_hz < 16 || frequency_hz > 1000000 || 1000000U % frequency_hz != 0)
    return Error::invalid_argument;
  auto pair = dc_pwm_pair(duty, mode);
  if (!pair)
    return Error::invalid_config;
  auto a = static_cast<std::uint8_t>(ch * 2), b = static_cast<std::uint8_t>(a + 1);
  (void)pwm_disable(a);
  (void)pwm_disable(b);
  (void)delay_us(10);
  if (duty != 0) {
    const auto period = 1000000U / frequency_hz;
    pwm_init(a, period);
    pwm_init(b, period);
    pwm_compare(a, static_cast<std::uint32_t>((*pair)[0] * static_cast<float>(period)));
    pwm_compare(b, static_cast<std::uint32_t>((*pair)[1] * static_cast<float>(period)));
  }
  return Error::ok;
}
Error pwm_duty(std::uint8_t channel, float duty, std::uint32_t frequency) noexcept {
  if (channel >= 8 || !std::isfinite(duty) || duty < 0 || duty > 1 || frequency < 16 ||
      frequency > 1000000 || 1000000U % frequency != 0) {
    return Error::invalid_argument;
  }
  const auto period = 1000000U / frequency;
  pwm_init(channel, period);
  pwm_compare(channel, static_cast<std::uint32_t>(duty * static_cast<float>(period)));
  return Error::ok;
}
Error dc_brake(std::uint8_t channel, bool high, std::uint32_t frequency) noexcept {
  if (channel >= 4) {
    return Error::invalid_argument;
  }
  const auto a = static_cast<std::uint8_t>(channel * 2), b = static_cast<std::uint8_t>(a + 1);
  (void)pwm_disable(a);
  (void)pwm_disable(b);
  (void)delay_us(10);
  const auto first = pwm_duty(a, high ? 1.0F : 0.0F, frequency);
  if (first != Error::ok) {
    return first;
  }
  const auto second = pwm_duty(b, high ? 1.0F : 0.0F, frequency);
  if (second != Error::ok) {
    (void)pwm_disable(a);
  }
  return second;
}

bool input(std::uint8_t index) noexcept {
  if (index >= connector::inputs.size())
    return false;
  const auto pin = connector::inputs[index];
  return (gpio_port(pin.port)->IDR & (1U << pin.bit)) != 0;
}
void stepper_service(void *context, Error (*tick)(void *, std::uint32_t)) noexcept {
  step_context = context;
  step_tick = tick;
  bit_set(RCC->APB2ENR, RCC_APB2ENR_TIM1EN);
  TIM1->PSC = 167;
  TIM1->ARR = 99;
  TIM1->EGR = TIM_EGR_UG;
  TIM1->SR = 0;
  TIM1->DIER = TIM_DIER_UIE;
  NVIC_SetPriority(TIM1_UP_TIM10_IRQn, 4);
  NVIC_ClearPendingIRQ(TIM1_UP_TIM10_IRQn);
  NVIC_EnableIRQ(TIM1_UP_TIM10_IRQn);
  TIM1->CR1 = TIM_CR1_CEN;
}
void stepper_irq(bool enabled) noexcept {
  if (enabled)
    NVIC_EnableIRQ(TIM1_UP_TIM10_IRQn);
  else
    NVIC_DisableIRQ(TIM1_UP_TIM10_IRQn);
  __DSB();
  __ISB();
}
bool stepper_failed() noexcept { return step_error.exchange(false); }
Error leds_write(std::span<const devices::Rgb> leds) noexcept {
  auto count = devices::ws2812_waveform(leds, {210, 68, 142, 400}, led_dma);
  if (count == 0)
    return Error::invalid_argument;
  bit_set(RCC->AHB1ENR, RCC_AHB1ENR_DMA2EN);
  bit_set(RCC->APB2ENR, RCC_APB2ENR_TIM8EN);
  auto start = micros();
  bit_clear(DMA2_Stream1->CR, DMA_SxCR_EN);
  if (!wait_until([] { return (DMA2_Stream1->CR & DMA_SxCR_EN) == 0; }, start, 1000))
    return Error::timeout;
  DMA2->LIFCR = 0xf40;
  DMA2_Stream1->CR =
      (7U << 25) | DMA_SxCR_MSIZE_0 | DMA_SxCR_PSIZE_0 | DMA_SxCR_MINC | DMA_SxCR_DIR_0;
  DMA2_Stream1->FCR = 0;
  DMA2_Stream1->PAR = reinterpret_cast<std::uint32_t>(&TIM8->CCR3);
  DMA2_Stream1->M0AR = reinterpret_cast<std::uint32_t>(led_dma.data());
  DMA2_Stream1->NDTR = static_cast<std::uint32_t>(count);
  TIM8->CR1 = 0;
  TIM8->PSC = 0;
  TIM8->ARR = 209;
  TIM8->CCR3 = 0;
  TIM8->CCMR2 = TIM_CCMR2_OC3PE | TIM_CCMR2_OC3M_1 | TIM_CCMR2_OC3M_2;
  TIM8->CCER = TIM_CCER_CC3E;
  TIM8->BDTR = TIM_BDTR_MOE;
  TIM8->EGR = TIM_EGR_UG;
  TIM8->DIER = TIM_DIER_UDE;
  pin_mode(GPIOC, 8, 2, 3);
  __DMB();
  bit_set(DMA2_Stream1->CR, DMA_SxCR_EN);
  TIM8->CR1 = TIM_CR1_CEN;
  auto ok = wait_until(
      [] {
        return (DMA2->LISR & ((1U << 11) | (1U << 9) | (1U << 8) | (1U << 6) | (1U << 6))) != 0;
      },
      start, 5000);
  auto flags = DMA2->LISR;
  TIM8->DIER = 0;
  TIM8->CR1 = 0;
  TIM8->CCER = 0;
  bit_clear(DMA2_Stream1->CR, DMA_SxCR_EN);
  pin_write(GPIOC, 8, false);
  pin_mode(GPIOC, 8, 1);
  return ok && (flags & ((1U << 9) | (1U << 8) | (1U << 6))) == 0 ? Error::ok
                                                                  : Error::output_failure;
}
void watchdog_start() noexcept {
  bit_set(RCC->CSR, RCC_CSR_LSION);
  if (!boot_wait(RCC->CSR, RCC_CSR_LSIRDY, RCC_CSR_LSIRDY))
    fail_safe();
  IWDG->KR = 0x5555;
  IWDG->PR = 3;
  IWDG->RLR = 999;
  const auto start = micros();
  if (!wait_until([] { return IWDG->SR == 0; }, start, 10000))
    fail_safe();
  IWDG->KR = 0xaaaa;
  IWDG->KR = 0xcccc;
}
void watchdog_feed() noexcept { IWDG->KR = 0xaaaa; }
[[noreturn]] void fail_safe() noexcept {
  __disable_irq();
  for (auto &s : steppers)
    pin_write(gpio_port(s.step.port), s.step.bit, false);
  for (unsigned i = 0; i < 8; ++i)
    (void)pwm_disable(static_cast<std::uint8_t>(i));
  if ((RCC->APB2ENR & RCC_APB2ENR_SPI1EN) != 0)
    (void)shift_write(0);
  for (;;) {
    __NOP();
  }
}
} // namespace eurobot::board
extern "C" void USART1_IRQHandler() {
  auto sr = USART1->SR;
  if ((sr & (USART_SR_RXNE | USART_SR_ORE | USART_SR_FE | USART_SR_NE)) != 0) {
    auto b = static_cast<std::uint8_t>(USART1->DR);
    if ((sr & (USART_SR_ORE | USART_SR_FE | USART_SR_NE)) != 0)
      overflow.store(true);
    else {
      auto h = rx_head.load(std::memory_order_relaxed), t = rx_tail.load(std::memory_order_acquire);
      if (h - t >= rx_ring.size())
        overflow.store(true);
      else {
        rx_ring[h % rx_ring.size()] = b;
        rx_head.store(h + 1, std::memory_order_release);
      }
    }
  }
}
extern "C" void UART5_IRQHandler() {
  const auto status = UART5->SR;
  if ((status & (USART_SR_RXNE | USART_SR_ORE | USART_SR_FE | USART_SR_NE)) != 0) {
    const auto byte = static_cast<std::uint8_t>(UART5->DR);
    if ((status & (USART_SR_ORE | USART_SR_FE | USART_SR_NE)) != 0) {
      debug_overflow.store(true);
    } else {
      const auto head = debug_head.load(std::memory_order_relaxed),
                 tail = debug_tail.load(std::memory_order_acquire);
      if (head - tail >= debug_ring.size()) {
        debug_overflow.store(true);
      } else {
        debug_ring[head % debug_ring.size()] = byte;
        debug_head.store(head + 1, std::memory_order_release);
      }
    }
  }
}

extern "C" void TIM1_UP_TIM10_IRQHandler() {
  if ((TIM1->SR & TIM_SR_UIF) != 0) {
    TIM1->SR = 0;
    if (step_tick && step_tick(step_context, TIM2->CNT) != Error::ok)
      step_error.store(true);
  }
}
