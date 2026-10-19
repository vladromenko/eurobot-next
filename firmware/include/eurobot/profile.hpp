#pragma once
#include "eurobot/application.hpp"
#include "eurobot/board_connectors.hpp"
#include "eurobot/core.hpp"
#include "eurobot/devices.hpp"
#include "eurobot/gripper.hpp"
#include <cmath>
namespace eurobot::board {
inline constexpr auto output_bits = connectors::power_register_bits;
enum class DeploymentMode : std::uint8_t { DIAGNOSTIC_MODE = 0, OPERATIONAL_MODE = 1 };
enum class HardwareProfile : std::uint8_t {
  CUSTOM = 0,
  ROBOT_2025_HARDWARE_PROFILE = 1,
  ROBOT_2026_SOURCE_PROFILE = 2
};
enum class DcMode : std::uint8_t { unconfigured, legacy_high_pair, coast_pwm };
[[nodiscard]] inline std::optional<std::array<float, 2>> dc_pwm_pair(float duty,
                                                                     DcMode mode) noexcept {
  if (!std::isfinite(duty) || std::fabs(duty) > 1 || mode == DcMode::unconfigured)
    return {};
  if (duty == 0)
    return std::array<float, 2>{0, 0};
  if (mode == DcMode::legacy_high_pair)
    return duty > 0 ? std::array<float, 2>{duty, 1} : std::array<float, 2>{1, -duty};
  if (mode == DcMode::coast_pwm)
    return duty > 0 ? std::array<float, 2>{duty, 0} : std::array<float, 2>{0, -duty};
  return {};
}
struct Deployment {
  DeploymentMode mode{DeploymentMode::DIAGNOSTIC_MODE};
  HardwareProfile hardware_profile{HardwareProfile::CUSTOM};
  std::uint16_t input_pulldown_mask{}; // InputBank indices; zero preserves no
                                       // internal pull.
  bool commissioned{};
  std::uint8_t channels{8}, max_active{};
  std::array<std::uint8_t, 32> pump_outputs{1, 3, 5,
                                            7}; // Physical output number; zero = unassigned.
  std::array<std::uint8_t, 32> valve_outputs{2, 4, 6, 8};
  ChassisConfig chassis{};
  std::array<std::uint8_t, 3> motor_ids{0x22, 0x21, 0x23};
  std::array<bool, 254> ax_ids{}, mx_ids{};
  std::array<float, 254> servo_min_deg{}, servo_max_deg{};
  std::array<bool, 16> pca_channels{};
  std::array<devices::PwmCalibration, 16> pca_calibration{};
  std::array<bool, 8> pwm_channels{};
  std::array<bool, 8> pwm_generic_channels{};
  std::array<std::uint32_t, 2> pwm_frequency_hz{};
  std::array<devices::PwmCalibration, 8> pwm_calibration{};
  std::array<bool, 4> dc_channels{};
  std::array<DcMode, 4> dc_modes{};
  std::array<bool, 4> dc_brake_configured{}, dc_brake_high{};
  std::array<std::uint32_t, 2> dc_frequency_hz{}; // Per TIM4/TIM5 group; zero = unknown.
  std::array<devices::StepperConfig, 4> steppers{};
  std::array<bool, 4> stepper_enabled{};
  std::array<std::uint8_t, 4> stepper_current_tval{}, stepper_ocd_threshold{}, stepper_mode{};
  std::array<std::uint16_t, 4> stepper_driver_config{};
  // Active final2026 initialization;255 leaves the reset TOFF_MIN of motors2..4
  // unchanged.
  std::array<std::uint8_t, 4> stepper_alarm_enable{0x03, 0x87, 0x87, 0x87};
  std::array<std::uint8_t, 4> stepper_toff_min{0x1f, 255, 255, 255};
  std::uint32_t max_pump_on_ms{};
  bool pca_enabled{}, leds_enabled{}, battery_enabled{};
  std::uint8_t battery_id{0x11};
  std::uint8_t led_count{60};
  std::uint8_t estop_input{255};
  bool estop_active_high{true};
  std::array<std::uint8_t, 4> home_input{255, 255, 255, 255};
  std::array<bool, 4> home_active_high{true, true, true, true};
  std::array<bool, 4> home_forward{false, false, false, false};
  std::array<gripper::Config, 2> grippers{};
  application::Config application{};
  bool debug_uart_enabled{};
  std::array<bool, 4> stepper_profile_enabled{};
  std::array<devices::StepperProfile, 4> stepper_profiles{};
  bool valid() const noexcept {
    if ((mode != DeploymentMode::DIAGNOSTIC_MODE && mode != DeploymentMode::OPERATIONAL_MODE) ||
        (input_pulldown_mask & ~0x1fffU) != 0)
      return false;
    if (!commissioned || channels < 8 || channels > 32 || max_active == 0 || max_active > channels)
      return false;
    if (leds_enabled && (led_count == 0 || led_count > 60)) {
      return false;
    }
    if (estop_input != 255 && estop_input >= 13)
      return false;
    if (application.strategy_count == 0 || application.match_duration_ms == 0 ||
        application.match_duration_ms >= 0x80000000U ||
        (application.mode != application::Mode::game &&
         application.mode != application::Mode::debug)) {
      return false;
    }
    for (const auto &input : application.inputs) {
      if (!inputs::DebouncedInput{input}.valid()) {
        return false;
      }
    }
    for (std::size_t bank = 0; bank < 2; ++bank) {
      bool dc = dc_channels[bank * 2] || dc_channels[bank * 2 + 1];
      if (dc && (dc_frequency_hz[bank] < 16 || dc_frequency_hz[bank] > 1000000 ||
                 1000000U % dc_frequency_hz[bank] != 0))
        return false;
      if (dc)
        for (std::size_t i = bank * 4; i < bank * 4 + 4; ++i)
          if (pwm_channels[i] || pwm_generic_channels[i])
            return false;
      bool generic = false, servo = false;
      for (std::size_t i = bank * 4; i < bank * 4 + 4; ++i) {
        generic = generic || pwm_generic_channels[i];
        servo = servo || pwm_channels[i];
      }
      if (generic &&
          (pwm_frequency_hz[bank] < 16 || pwm_frequency_hz[bank] > 1000000 ||
           1000000U % pwm_frequency_hz[bank] != 0 || (servo && pwm_frequency_hz[bank] != 50))) {
        return false;
      }
    }
    for (std::size_t i = 0; i < ax_ids.size(); ++i) {
      if (ax_ids[i] && mx_ids[i])
        return false;
      if (ax_ids[i] || mx_ids[i]) {
        if (i == 0 || !std::isfinite(servo_min_deg[i]) || !std::isfinite(servo_max_deg[i]) ||
            servo_min_deg[i] < 0 || servo_max_deg[i] <= servo_min_deg[i] ||
            servo_max_deg[i] > (ax_ids[i] ? 300 : 360))
          return false;
      }
    }
    if (!OmniKinematics{chassis.geometry}.valid() || chassis.command_timeout_ms == 0 ||
        chassis.max_tick_ms == 0)
      return false;
    for (std::size_t i = 0; i < 4; ++i)
      if (stepper_enabled[i] &&
          (steppers[i].maximum_steps_per_second > 10000 || steppers[i].maximum_tick_us < 200 ||
           (home_input[i] != 255 && home_input[i] >= 13)))
        return false;
    std::uint32_t used = 0;
    for (std::size_t i = 0; i < channels; ++i) {
      if (pump_outputs[i] == 0 || pump_outputs[i] > 20 || valve_outputs[i] > 20)
        return false;
      for (auto pin : {pump_outputs[i], valve_outputs[i]}) {
        if (pin != 0) {
          auto bit = 1U << output_bits[pin - 1];
          if ((used & bit) != 0)
            return false;
          used |= bit;
        }
      }
    }
    for (std::size_t i = 0; i < 3; ++i) {
      if (motor_ids[i] == 0 || motor_ids[i] >= 254)
        return false;
      for (std::size_t j = 0; j < i; ++j)
        if (motor_ids[i] == motor_ids[j])
          return false;
    }
    for (std::size_t i = 0; i < 4; ++i) {
      if (dc_channels[i] && !dc_pwm_pair(0, dc_modes[i]))
        return false;
      if (dc_channels[i] && (pwm_channels[2 * i] || pwm_channels[2 * i + 1]))
        return false;
      if (stepper_enabled[i] && stepper_profile_enabled[i]) {
        const auto &p = stepper_profiles[i];
        if (p.base_frequency_hz == 0 || p.base_frequency_hz > 1000000 || p.working_division == 0 ||
            p.working_division > 32 || p.edge_division < p.working_division ||
            p.edge_division > 32 || p.acceleration_ms > 60000 ||
            (p.base_frequency_hz + p.working_division - 1) / p.working_division >
                steppers[i].maximum_steps_per_second ||
            (static_cast<std::uint64_t>(p.base_frequency_hz) * steppers[i].maximum_tick_us +
             999999) /
                    1000000 >
                64) {
          return false;
        }
      }
      if (stepper_enabled[i]) {
        if ((stepper_alarm_enable[i] & ~0xcfU) != 0 ||
            (stepper_toff_min[i] != 255 && stepper_toff_min[i] > 0x7f)) {
          return false;
        }
        const auto &s = steppers[i];
        if (s.minimum_steps >= s.maximum_steps || s.maximum_steps_per_second == 0 ||
            s.maximum_tick_us == 0)
          return false;
      }
    }
    return std::isfinite(chassis.max_linear_mps) && std::isfinite(chassis.max_angular_radps) &&
           std::isfinite(chassis.max_accel_mps2) &&
           std::isfinite(chassis.max_angular_accel_radps2) && chassis.max_linear_mps > 0 &&
           chassis.max_angular_radps > 0 && chassis.max_accel_mps2 > 0 &&
           chassis.max_angular_accel_radps2 > 0 && chassis.max_wheel_tpm > 0 &&
           chassis.max_encoder_delta > 0;
  }
  [[nodiscard]] std::optional<std::uint32_t>
  output_word(std::uint32_t pumps, std::uint32_t valves,
              std::uint32_t preserved = 0) const noexcept {
    if (channels == 0 || channels > 32)
      return {};
    if (((pumps | valves) & ~channel_mask(channels)) != 0)
      return {};
    std::uint32_t word = preserved & ((1U << 8) | (1U << 9) | (1U << 17) | (1U << 16));
    for (std::size_t i = 0; i < channels; ++i) {
      if ((pumps & (1U << i)) != 0) {
        if (pump_outputs[i] == 0 || pump_outputs[i] > 20)
          return {};
        word |= 1U << output_bits[pump_outputs[i] - 1];
      }
      if ((valves & (1U << i)) != 0) {
        if (valve_outputs[i] == 0 || valve_outputs[i] > 20)
          return {};
        word |= 1U << output_bits[valve_outputs[i] - 1];
      }
    }
    return word;
  }
};
} // namespace eurobot::board
