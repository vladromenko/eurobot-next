#pragma once
#include "eurobot/profile.hpp"

namespace eurobot::board {
namespace source_2025 {
// Canonical2025 Core/App/bus_device.c:27; AX gripper_task.c:47-49.
inline constexpr std::array<std::uint8_t, 3> motor_ids{0x21, 0x22, 0x23};
inline constexpr std::array<std::uint8_t, 2> known_ax_ids{103, 102};
inline constexpr std::uint8_t battery_id = 0x11;
} // namespace source_2025
// Candidate recovered from2025 source. The installed firmware year does not
// identify actual wiring. This describes source facts,
// not verified wiring. Enabling operation requires editing/reviewing the actual
// routes, actuator allowlists and operating limits, then selecting
// OPERATIONAL_MODE.
[[nodiscard]] inline Deployment robot_2025() {
  Deployment config{};
  config.hardware_profile = HardwareProfile::ROBOT_2025_HARDWARE_PROFILE;
  config.mode = DeploymentMode::DIAGNOSTIC_MODE;
  config.motor_ids = source_2025::motor_ids;
  config.battery_id = source_2025::battery_id;
  config.chassis.geometry = legacy_2025_geometry();
  // No canonical2025 pump/valve assignment proves either the2026 four-pair
  // mapping or the proposed eight-pair wiring. Zero explicitly means unknown.
  config.pump_outputs.fill(0);
  config.valve_outputs.fill(0);
  config.max_active = 0;               // No source-backed electrical concurrency limit.
  config.input_pulldown_mask = 0x000f; // robot_gpio_config.c:34-68, four active-high buttons.
  // stepper.c:44-134; dormant electrical facts, no enable, movement or guessed
  // travel.
  config.stepper_current_tval = {24, 24, 24, 24}; // Legacy800mA quantization, actual781.25mA.
  config.stepper_ocd_threshold = {8, 8, 8, 8};    // Datasheet reset; source does not write OCD.
  config.stepper_mode = {4, 4, 4, 4};             // 1/16, encoded STEP_MODE0x8c by the new driver.
  config.stepper_driver_config = {0x7c03, 0x2883, 0x2883, 0x2883};
  config.stepper_alarm_enable = {0x03, 0x87, 0x87, 0x87};
  config.stepper_toff_min = {0x1f, 255, 255, 255};
  config.grippers[0] = gripper::legacy_2025_reference();
  // The second mechanism, PCA, pump routing, homing switches and safe speed
  // are absent from this canonical2025 task; no2026 values are substituted.
  config.application.mode = application::Mode::debug;
  config.application.legacy_2025_motor_button_control = false;
  config.application.open_reserve_side_at_start = false;
  return config; // commissioned/reviewed/actuator enables remain false.
}
} // namespace eurobot::board
