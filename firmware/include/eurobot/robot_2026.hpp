#pragma once
#include "eurobot/board_installation.hpp"
#include <algorithm>
#include <cmath>
#include <numbers>

namespace eurobot::board {
// Source-derived operating profile for the same 2026 robot, with the prescribed
// eight-pair wiring. It is NOT a claim of hardware validation. All boot outputs
// remain off; a live owner and explicit arm are still mandatory.
// Source command domains and NEW software policies below are distinguished from
// measured mechanical stops and electrical ratings.
[[nodiscard]] inline Deployment robot_2026(std::uint8_t primary_main_id = 255) {
  auto config = eight_pump_installation();
  config.mode = DeploymentMode::OPERATIONAL_MODE;
  config.hardware_profile = HardwareProfile::ROBOT_2026_SOURCE_PROFILE;
  // This profile has been reviewed against the 2026 sources. The flag selects
  // this complete software configuration; physical acceptance remains separate.
  config.commissioned = true;
  // NEW policy: preserve the old four-pump concurrent operating count while
  // exposing eight distinct channels. This is not a PSU current rating.
  config.max_active = 4;
  config.max_pump_on_ms = 99000; // NEW continuous-on cap: one source-length match.
  auto &chassis = config.chassis;
  // Selected2026 HL: trajectory_regulator.launch.py14,16; robot1 YAML6.
  chassis.max_linear_mps = 1;
  chassis.max_angular_radps = 5;
  chassis.max_accel_mps2 = 0.2F;        // MCU gripper_task MoveAxisByOdometryAsync.
  chassis.max_angular_accel_radps2 = 5; // NEW policy: one second to angular cap.
  // Derived command envelope with10ppm numerical rounding allowance, NOT an
  // invented external motor rating.
  const auto &geometry = chassis.geometry;
  const auto factor = 60.0 * geometry.gear_ratio * geometry.encoder_ticks_per_motor_turn /
                      (geometry.command_scale * 2.0 * std::numbers::pi * geometry.wheel_radius_m);
  const auto envelope =
      factor * (chassis.max_linear_mps + geometry.center_to_wheel_m * chassis.max_angular_radps *
                                             geometry.angular_command_scale);
  chassis.max_wheel_tpm = static_cast<std::int32_t>(std::ceil(envelope * 1.00001));
  chassis.max_encoder_delta = static_cast<std::int32_t>(
      std::ceil(static_cast<double>(chassis.max_wheel_tpm) * chassis.max_tick_ms / 60000.0) + 1);
  // Source API accepts uint8 height in mm. Zero is established by switch2,
  // not by treating the old -100000 search target as the zero coordinate.
  config.steppers[0] = {0, source_2026::lift_command_max_steps, 1920, 1000};
  config.stepper_enabled[0] = true;
  config.stepper_profile_enabled[0] = true;
  config.pca_enabled = true;
  config.leds_enabled = true;
  config.battery_enabled = true;
  for (auto &grip : config.grippers) {
    grip.reviewed = true; // Source bindings/recipes reviewed, not physically measured.
    grip.minimum_height_mm = 0;
    grip.maximum_height_mm = source_2026::lift_command_max_height_mm;
    grip.minimum_lift_steps = 0;
    grip.maximum_lift_steps = source_2026::lift_command_max_steps;
    // Match the source disable behaviour. The stricter reference policy
    // requires a new finite home after torque removal, rather than trusting an
    // unpowered lift.
    grip.disable_lift_after_move = true;
    grip.composite = gripper::Composite::base_strategy_2026;
    // NEW explicit recipe replaces malformed old one-byte SimpleMove calls.
    // Speed is existing2026 Mac console default; durations follow BT's2500ms
    // waits.
    grip.capture_axis = 1;
    grip.capture_forward_mps = 0.12F;
    grip.capture_backward_mps = 0.12F;
    grip.capture_forward_ms = 2500;
    grip.capture_backward_ms = 2500;
  }
  // Never infer a physical role from a spare ID or emit legacy broadcast254.
  // The source-addressed primary group is complete with112 alone.
  // An additional, physically identified main motor can be supplied explicitly.
  if (primary_main_id > 0 && primary_main_id < 254 && !config.ax_ids[primary_main_id] &&
      !config.mx_ids[primary_main_id]) {
    config.grippers[0].main_active_mask = 0x03;
    config.grippers[0].main[0] = {gripper::Device::ax, primary_main_id, 0, 300};
    config.ax_ids[primary_main_id] = true;
    config.servo_min_deg[primary_main_id] = 0;
    config.servo_max_deg[primary_main_id] = 300;
  }
  // Default main0 recipes address and verify112, never the inactive source254
  // slot.
  return config;
}
} // namespace eurobot::board
