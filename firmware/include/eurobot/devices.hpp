#pragma once
#include "eurobot/core.hpp"
namespace eurobot::devices {
// Callbacks MUST bound the entire operation by timeout_us, including bus
// contention. Empty rx requests a transmit-only transaction; a success is not a
// device acknowledgement.
struct SerialBus {
  void *context{};
  Error (*exchange)(void *, std::span<const std::uint8_t> tx, std::span<std::uint8_t> rx,
                    std::size_t &received, std::uint32_t timeout_us){};
};
class DxlProtocol1 {
public:
  explicit DxlProtocol1(SerialBus bus, std::uint32_t timeout_us = 5000) noexcept
      : bus_(bus), timeout_us_(timeout_us) {}
  [[nodiscard]] Error transact(std::uint8_t id, std::uint8_t instruction,
                               std::span<const std::uint8_t> parameters,
                               std::span<std::uint8_t> output, std::size_t &received,
                               bool expect_reply = true) noexcept;
  [[nodiscard]] Error ping(std::uint8_t id) noexcept;
  [[nodiscard]] Error sync_write(std::uint8_t address, std::uint8_t bytes_per_device,
                                 std::span<const std::uint8_t> ids,
                                 std::span<const std::uint8_t> packed_values) noexcept;
  [[nodiscard]] Error read(std::uint8_t id, std::uint8_t address,
                           std::span<std::uint8_t> output) noexcept;
  [[nodiscard]] Error write(std::uint8_t id, std::uint8_t address,
                            std::span<const std::uint8_t> values,
                            bool acknowledge = false) noexcept;
  [[nodiscard]] std::uint8_t device_error() const noexcept { return device_error_; }

private:
  SerialBus bus_;
  std::uint32_t timeout_us_;
  std::uint8_t device_error_{};
};
class Maxon {
public:
  Maxon(DxlProtocol1 &protocol, std::uint8_t id) noexcept : protocol_(protocol), id_(id) {}
  [[nodiscard]] Error ping() noexcept { return protocol_.ping(id_); }
  [[nodiscard]] std::uint8_t id() const noexcept { return id_; }
  [[nodiscard]] Error enable(bool enabled) noexcept;
  [[nodiscard]] Error set_id(std::uint8_t id) noexcept;
  [[nodiscard]] Error set_tpm_range(std::uint32_t lower, std::uint32_t upper) noexcept;
  [[nodiscard]] Error set_tpm(std::int32_t value) noexcept;
  [[nodiscard]] Error get_tpm(std::int32_t &output) noexcept;
  [[nodiscard]] Error total_ticks(std::int32_t &output) noexcept;
  [[nodiscard]] Error set_total_ticks(std::int32_t value) noexcept;
  [[nodiscard]] Error tpm_range(std::uint32_t &lower, std::uint32_t &upper) noexcept;

private:
  DxlProtocol1 &protocol_;
  std::uint8_t id_;
};
enum class ServoModel : std::uint8_t { ax_300, mx_360 };
struct ServoConfig {
  ServoModel model{ServoModel::ax_300};
  float minimum_deg{}, maximum_deg{300};
};
struct ServoFeedback {
  float position_deg{};
  std::int16_t speed_raw{}, load_raw{};
  std::uint8_t voltage_decivolts{}, temperature_c{};
  bool moving{};
};
class DynamixelServo {
public:
  DynamixelServo(DxlProtocol1 &protocol, std::uint8_t id, ServoConfig config) noexcept
      : protocol_(protocol), id_(id), config_(config) {}
  [[nodiscard]] Error ping() noexcept;
  [[nodiscard]] Error set_id(std::uint8_t id) noexcept;
  [[nodiscard]] Error wheel_mode(bool enabled) noexcept;
  [[nodiscard]] Error led(bool enabled) noexcept;
  [[nodiscard]] Error position_deg(float degrees) noexcept;
  [[nodiscard]] Error speed_raw(std::int16_t speed) noexcept;
  [[nodiscard]] Error torque(bool enabled) noexcept;
  [[nodiscard]] Error torque_limit(std::uint16_t raw) noexcept;
  [[nodiscard]] Error feedback(ServoFeedback &output) noexcept;
  [[nodiscard]] Error present_position_deg(float &output) noexcept;
  // Legacy GetMovingSpeed reads commanded speed (register32), not present
  // speed38.
  [[nodiscard]] Error moving_speed_raw(std::int16_t &output) noexcept;
  [[nodiscard]] Error present_speed_raw(std::int16_t &output) noexcept;
  [[nodiscard]] Error present_load_raw(std::int16_t &output) noexcept;

private:
  DxlProtocol1 &protocol_;
  std::uint8_t id_;
  ServoConfig config_;
};
struct BatteryState {
  std::uint32_t power_mw{};
  std::array<std::uint16_t, 4> cell_mv{};
  std::uint16_t voltage_mv{}, current_ma{};
  std::uint8_t soc_percent{}, cells{};
};
[[nodiscard]] Error decode_battery(std::span<const std::uint8_t> bytes,
                                   BatteryState &output) noexcept;
[[nodiscard]] Error read_battery(DxlProtocol1 &protocol, std::uint8_t id,
                                 BatteryState &output) noexcept;
[[nodiscard]] Error set_battery_id(DxlProtocol1 &protocol, std::uint8_t current_id,
                                   std::uint8_t new_id) noexcept;
[[nodiscard]] Error read_battery_cells(DxlProtocol1 &protocol, std::uint8_t id,
                                       std::array<std::uint16_t, 4> &millivolts) noexcept;
[[nodiscard]] Error read_battery_voltage(DxlProtocol1 &protocol, std::uint8_t id,
                                         std::uint16_t &millivolts) noexcept;
[[nodiscard]] Error read_battery_current(DxlProtocol1 &protocol, std::uint8_t id,
                                         std::uint16_t &milliamps) noexcept;
[[nodiscard]] Error read_battery_power(DxlProtocol1 &protocol, std::uint8_t id,
                                       std::uint32_t &milliwatts) noexcept;
[[nodiscard]] Error read_battery_soc(DxlProtocol1 &protocol, std::uint8_t id,
                                     std::uint8_t &percent) noexcept;
[[nodiscard]] Error read_battery_cell_count(DxlProtocol1 &protocol, std::uint8_t id,
                                            std::uint8_t &cells) noexcept;
// Privileged maintenance only; writes persistent calibration. Never called
// during boot/control.
[[nodiscard]] Error calibrate_battery(DxlProtocol1 &protocol, std::uint8_t id, std::uint8_t address,
                                      std::uint16_t value) noexcept;
struct I2cBus {
  void *context{};
  Error (*write)(void *, std::uint8_t address, std::uint8_t reg,
                 std::span<const std::uint8_t> values, std::uint32_t timeout_us){};
  Error (*read)(void *, std::uint8_t address, std::uint8_t reg, std::span<std::uint8_t> values,
                std::uint32_t timeout_us){};
  Error (*delay_us)(void *, std::uint32_t){};
};
struct PwmCalibration {
  float minimum_deg{}, maximum_deg{180};
  std::uint16_t minimum_us{500}, maximum_us{2500};
};
[[nodiscard]] std::optional<std::uint16_t> servo_pulse(float degrees,
                                                       PwmCalibration config) noexcept;
class Pca9685 {
public:
  Pca9685(I2cBus bus, std::uint8_t address = 0x40, std::uint32_t timeout_us = 5000) noexcept
      : bus_(bus), address_(address), timeout_us_(timeout_us) {}
  [[nodiscard]] Error initialize(float frequency_hz = 50, float oscillator_hz = 25000000) noexcept;
  [[nodiscard]] Error set_ticks(std::uint8_t channel, std::uint16_t on, std::uint16_t off) noexcept;
  [[nodiscard]] Error pulse_us(std::uint8_t channel, std::uint16_t pulse) noexcept;
  [[nodiscard]] Error position_deg(std::uint8_t channel, float degrees,
                                   PwmCalibration calibration) noexcept;
  [[nodiscard]] Error full_off() noexcept;
  [[nodiscard]] Error sleep() noexcept;
  [[nodiscard]] Error wakeup() noexcept;
  [[nodiscard]] Error all_pulse_us(std::uint16_t pulse) noexcept;
  [[nodiscard]] float actual_frequency() const noexcept { return frequency_; }

private:
  I2cBus bus_;
  std::uint8_t address_;
  std::uint32_t timeout_us_;
  float frequency_{};
  bool initialized_{}, sleeping_{};
  [[nodiscard]] Error write(std::uint8_t reg, std::span<const std::uint8_t> bytes) noexcept;
};
struct BridgeSink {
  void *context{};
  Error (*write)(void *, float signed_duty, bool brake){};
};
class HBridge {
public:
  explicit HBridge(BridgeSink sink) noexcept : sink_(sink) {}
  [[nodiscard]] Error drive(float duty) noexcept;
  [[nodiscard]] Error stop(bool brake = false) noexcept;
  [[nodiscard]] bool commanded_known() const noexcept { return known_; }

private:
  BridgeSink sink_;
  float previous_{};
  bool known_{};
};
struct SpiBus {
  void *context{};
  Error (*transfer)(void *, std::span<const std::uint8_t> tx, std::span<std::uint8_t> rx,
                    std::uint32_t timeout_us){};
};
struct L6474Pins {
  void *context{};
  Error (*direction)(void *, bool forward){};
  Error (*reset)(void *, bool asserted){};
  Error (*delay_us)(void *, std::uint32_t){};
};
class L6474 {
public:
  explicit L6474(SpiBus bus, std::uint32_t timeout_us = 5000, L6474Pins pins = {}) noexcept
      : bus_(bus), timeout_us_(timeout_us), pins_(pins) {}
  [[nodiscard]] Error set_parameter(std::uint8_t address, std::uint32_t value) noexcept;
  [[nodiscard]] Error set_step_mode(std::uint8_t logical_mode) noexcept;
  // TVAL quantization follows ST/legacy 31.25mA steps. Caller must confirm
  // motor/electrical limits.
  [[nodiscard]] Error set_current_ma(float milliamps) noexcept;
  [[nodiscard]] Error set_current_raw(std::uint8_t tval) noexcept;
  [[nodiscard]] Error direction(bool forward) noexcept;
  // Real NRST, asserted10ms and released10ms. Callback failures leave NRST
  // asserted.
  [[nodiscard]] Error hard_reset() noexcept;
  [[nodiscard]] Error get_parameter(std::uint8_t address, std::uint32_t &output) noexcept;
  [[nodiscard]] Error enable(bool enabled) noexcept;
  [[nodiscard]] Error status(std::uint16_t &output) noexcept;

private:
  SpiBus bus_;
  std::uint32_t timeout_us_;
  L6474Pins pins_;
};
struct StepPulseSink {
  void *context{};
  Error (*configure)(void *, bool forward, std::uint32_t period_us){};
  Error (*pulse)(void *){}; // One full pulse with required high/low timing,
                            // bounded by hardware adapter.
  Error (*stop)(void *){};
};
struct StepperConfig {
  std::int64_t minimum_steps{}, maximum_steps{};
  std::uint32_t maximum_steps_per_second{}, maximum_tick_us{1000};
};
struct StepperProfile {
  std::uint32_t base_frequency_hz{19200};
  std::uint32_t edge_division{32}, working_division{10}, acceleration_ms{200};
};
struct StepperRampEntry {
  std::uint32_t division{}, base_ticks{};
};
class StepperPlanner {
public:
  StepperPlanner(StepperConfig config, StepPulseSink sink) noexcept
      : config_(config), sink_(sink) {}
  [[nodiscard]] Error reference(std::int64_t steps) noexcept;
  [[nodiscard]] Error move(std::int64_t target, std::uint32_t speed, std::uint32_t timeout_us,
                           std::uint32_t now_us) noexcept;
  [[nodiscard]] Error move_relative(std::int64_t delta, std::uint32_t speed,
                                    std::uint32_t timeout_us, std::uint32_t now_us) noexcept;
  [[nodiscard]] Error move_profile(std::int64_t target, StepperProfile profile,
                                   std::uint32_t timeout_us, std::uint32_t now_us) noexcept;
  [[nodiscard]] Error move_relative_profile(std::int64_t delta, StepperProfile profile,
                                            std::uint32_t timeout_us,
                                            std::uint32_t now_us) noexcept;
  [[nodiscard]] Error home(bool forward, std::uint32_t speed, std::uint32_t timeout_us,
                           std::uint32_t now_us) noexcept;
  // Profiled seek is bounded by timeout and one configured full travel. Only
  // the mapped physical switch establishes the minimum/maximum reference.
  [[nodiscard]] Error home_profile(bool forward, StepperProfile profile, std::uint32_t timeout_us,
                                   std::uint32_t now_us) noexcept;
  [[nodiscard]] Error tick(std::uint32_t now_us, bool home_switch = false) noexcept;
  [[nodiscard]] Error stop() noexcept;
  [[nodiscard]] bool busy() const noexcept { return running_; }
  [[nodiscard]] bool position_known() const noexcept { return known_; }
  [[nodiscard]] std::int64_t commanded_steps() const noexcept { return position_; }
  [[nodiscard]] std::span<const StepperRampEntry> ramp() const noexcept {
    return {ramp_.data() + ramp_begin_, ramp_end_ - ramp_begin_};
  }
  [[nodiscard]] bool profiled() const noexcept { return profile_active_; }

private:
  StepperConfig config_;
  StepPulseSink sink_;
  std::int64_t position_{}, target_{};
  std::uint32_t started_{}, last_tick_{}, last_pulse_{}, period_{}, timeout_{};
  bool running_{}, known_{}, homing_{}, forward_{};
  std::array<StepperRampEntry, 64> ramp_{};
  std::size_t ramp_begin_{}, ramp_end_{}, ramp_index_{}, ramp_limit_{};
  std::uint32_t base_frequency_{}, base_fraction_{}, acceleration_count_{}, division_count_{},
      division_reload_{};
  std::uint64_t deceleration_steps_{}, travelled_{}, profile_distance_{};
  bool profile_active_{}, pulse_pending_{};
  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] Error start(bool forward, std::uint32_t speed, std::uint32_t timeout,
                            std::uint32_t now) noexcept;
  [[nodiscard]] Error build_profile(std::uint64_t distance, StepperProfile profile) noexcept;
  [[nodiscard]] Error tick_profile(std::uint32_t elapsed_us) noexcept;
};
struct Rgb {
  std::uint8_t red{}, green{}, blue{};
};
struct Hsv {
  float hue_deg{}, saturation{}, value{}; // Hue0..360; saturation/value0..1.
};
[[nodiscard]] std::optional<Rgb> hsv_rgb(Hsv color) noexcept;
// Whole span is validated before any output is changed.
[[nodiscard]] Error hsv_rgb(std::span<const Hsv> colors, std::span<Rgb> output) noexcept;
struct Ws2812Timing {
  std::uint16_t timer_period_ticks{}, zero_high_ticks{}, one_high_ticks{}, reset_periods{};
};
[[nodiscard]] std::size_t ws2812_waveform(std::span<const Rgb> leds, Ws2812Timing timing,
                                          std::span<std::uint16_t> output) noexcept;
} // namespace eurobot::devices
