#pragma once
#include "eurobot/profile.hpp"

namespace eurobot::board::source_2026 {
// These are source/wire domains, not measured mechanical stops or new safety
// limits.
inline constexpr std::uint16_t lift_command_max_height_mm = 255;
inline constexpr std::int64_t lift_command_max_steps = 6375;
inline constexpr float lift_steps_per_mm = 25;
inline constexpr std::uint32_t stepper_base_frequency_hz = 19200;
inline constexpr devices::StepperProfile lift_profile{19200, 32, 10, 200};
inline constexpr devices::StepperProfile home_profile{19200, 32, 16, 500};
inline constexpr std::array<std::uint8_t, 9> known_ax_ids{1, 2, 7, 8, 13, 16, 25, 98, 112};

// Populate proven source parameters without commissioning, reviewing, or
// enabling mechanisms. Preserve caller-owned routing, limit policy and enable
// flags. Call while constructing a deployment, before the caller explicitly
// selects any new operating envelope.
inline void apply_known_parameters(Deployment &config) noexcept {
  // Core/App/bus_device.c:27 and Core/App/kinematics.c:30-44, final2026.
  config.motor_ids = {0x22, 0x21, 0x23};
  config.battery_id = 0x11;
  config.chassis.geometry = legacy_2026_geometry();

  // Core/BSP/stepper.c:44-137. TVAL follows l6474_drvr.c:199 integer
  // quantization.
  config.stepper_current_tval = {34, 47, 34, 34}; // Requested1100/1500/1100/1100mA.
  // Source does not write OCD_TH; official ST table9 reset value is0x08.
  config.stepper_ocd_threshold = {8, 8, 8, 8};
  config.stepper_mode = {0, 0, 0, 0}; // Full-step; driver adds required fixed bits0x88.
  config.stepper_driver_config = {0x7c03, 0x2883, 0x2883, 0x2883};
  config.stepper_alarm_enable = {0x03, 0x87, 0x87, 0x87};
  config.stepper_toff_min = {0x1f, 255, 255, 255}; // 255=leave reset TOFF_MIN0x29.
  config.stepper_profiles[0] = lift_profile;
  // stepper_task.c:61-85 + buttons_poll_task.c:69 +
  // robot_behavior_routine.c:147. Mechanical switch2 is PC13/GPIO26 = InputBank
  // index5; active electrical low.
  config.home_input[0] = 5;
  config.home_active_high[0] = false;
  config.home_forward[0] = false;

  // gripper_task.c:138-197. The former254 slot is inactive; no replacement
  // is needed for the supported112-only main target group.
  // 0..300deg is the AX protocol domain, not a measured collision-free
  // interval.
  for (const auto id : known_ax_ids) {
    config.ax_ids[id] = true;
    config.servo_min_deg[id] = 0;
    config.servo_max_deg[id] = 300;
  }
  for (std::size_t index = 0; index < config.grippers.size(); ++index) {
    const auto source = gripper::legacy_2026_reference(static_cast<std::uint8_t>(index));
    auto &grip = config.grippers[index];
    // Copy only known mechanism parameters. Existing routing/review/limits stay
    // caller-owned.
    grip.main = source.main;
    grip.main_active_mask = source.main_active_mask;
    grip.side = source.side;
    grip.cursor = source.cursor;
    grip.pinion = source.pinion;
    grip.block_channels = source.block_channels;
    grip.shift_channels = source.shift_channels;
    grip.side_closed_deg = source.side_closed_deg;
    grip.side_open_deg = source.side_open_deg;
    grip.shift_closed_deg = source.shift_closed_deg;
    grip.shift_open_deg = source.shift_open_deg;
    grip.main_up_deg = source.main_up_deg;
    grip.main_down_deg = source.main_down_deg;
    grip.main_cursor_deg = source.main_cursor_deg;
    grip.cursor_horizontal_deg = source.cursor_horizontal_deg;
    grip.cursor_vertical_deg = source.cursor_vertical_deg;
    grip.pinion_start_deg = source.pinion_start_deg;
    grip.pinion_deg_per_mm = source.pinion_deg_per_mm;
    grip.main_speed_raw = source.main_speed_raw; // Active case2 raw150, boot profile raw300.
    grip.side_speed_raw = source.side_speed_raw;
    grip.cursor_speed_raw = source.cursor_speed_raw;
    grip.pinion_speed_raw = source.pinion_speed_raw;
    grip.angle_tolerance_deg = source.angle_tolerance_deg;
    grip.block_zero_deg = source.block_zero_deg;
    grip.block_rotated_deg = source.block_rotated_deg;
    grip.lift_stepper = 0;
    grip.lift_steps_per_mm = lift_steps_per_mm;
    grip.lift_speed_steps_s =
        1920; // 19200/working_div10; source operating peak, not motor maximum.
    grip.lift_up_current_ma = 1100;
    grip.lift_down_current_ma = 320;
    grip.servo_timeout_ms = 1500;
    grip.lift_timeout_ms = 4000;
    grip.disable_lift_after_move = true; // Active source case7; reference invalidates on disable.
    grip.drive_reference = source.drive_reference;
  }
  // main.c:73: PCA address0x40,50Hz,500..2500us over0..180deg. Used
  // channels1..12. The runtime already defaults to0x40/50Hz.
  // Calibration/allowlist is not actuator enable.
  for (std::size_t channel = 1; channel <= 12; ++channel) {
    config.pca_channels[channel] = true;
    config.pca_calibration[channel] = {0, 180, 500, 2500};
  }
  // No commissioned/reviewed, actuator enable, supply budget, motion maximum,
  // mechanical travel bounds, ESTOP wiring or missing Dynamixel ID is
  // fabricated here.
}
} // namespace eurobot::board::source_2026
