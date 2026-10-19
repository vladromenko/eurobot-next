#pragma once
#include "eurobot/profile.hpp"
namespace eurobot::board {
[[nodiscard]] Error initialize() noexcept;
[[nodiscard]] std::uint32_t micros() noexcept;
[[nodiscard]] std::uint32_t millis() noexcept;
[[nodiscard]] Error delay_us(std::uint32_t us) noexcept;
[[nodiscard]] Error host_write(std::span<const std::uint8_t> bytes,
                               std::uint32_t timeout_us) noexcept;
[[nodiscard]] std::optional<std::uint8_t> host_read() noexcept;
void debug_enable(bool enabled) noexcept;
[[nodiscard]] Error debug_write(std::span<const std::uint8_t>, std::uint32_t timeout_us) noexcept;
[[nodiscard]] std::optional<std::uint8_t> debug_read() noexcept;
[[nodiscard]] std::uint32_t reset_cause() noexcept;
void clear_reset_cause() noexcept;
[[noreturn]] void system_reset() noexcept;
[[nodiscard]] bool receive_overflow() noexcept;
[[nodiscard]] Error shift_write(std::uint32_t word) noexcept;
[[nodiscard]] Error serial_exchange(void *context, std::span<const std::uint8_t> tx,
                                    std::span<std::uint8_t> rx, std::size_t &received,
                                    std::uint32_t timeout_us) noexcept;
[[nodiscard]] devices::SerialBus motor_bus() noexcept;
[[nodiscard]] devices::SerialBus servo_bus() noexcept;
[[nodiscard]] devices::SerialBus battery_bus() noexcept;
[[nodiscard]] devices::I2cBus i2c_bus() noexcept;
[[nodiscard]] devices::SpiBus stepper_bus(std::size_t channel) noexcept;
[[nodiscard]] devices::StepPulseSink stepper_pulses(std::size_t channel) noexcept;
[[nodiscard]] Error stepper_direction(std::size_t channel, bool forward) noexcept;
[[nodiscard]] Error pwm_pulse(std::uint8_t channel, std::uint16_t us) noexcept;
[[nodiscard]] Error pwm_disable(std::uint8_t channel) noexcept;
[[nodiscard]] Error pwm_group_frequency(std::uint8_t bank, std::uint32_t hz) noexcept;
[[nodiscard]] std::array<std::uint32_t, 2> pwm_group_info(std::uint8_t bank) noexcept;
[[nodiscard]] Error pwm_duty(std::uint8_t channel, float duty, std::uint32_t frequency) noexcept;
[[nodiscard]] Error dc_brake(std::uint8_t channel, bool high, std::uint32_t frequency) noexcept;
[[nodiscard]] Error dc_drive(std::uint8_t channel, float duty, DcMode mode,
                             std::uint32_t frequency_hz) noexcept;
[[nodiscard]] bool input(std::uint8_t index) noexcept;
void stepper_service(void *context, Error (*tick)(void *, std::uint32_t)) noexcept;
void stepper_irq(bool enabled) noexcept;
[[nodiscard]] bool stepper_failed() noexcept;
[[nodiscard]] Error leds_write(std::span<const devices::Rgb> leds) noexcept;
void watchdog_start() noexcept;
void watchdog_feed() noexcept;
[[noreturn]] void fail_safe() noexcept;
} // namespace eurobot::board
