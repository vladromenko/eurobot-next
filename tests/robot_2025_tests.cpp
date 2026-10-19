#include "eurobot/robot_2025.hpp"
#include "eurobot/robot_2026.hpp"
#include <gtest/gtest.h>
using namespace eurobot;
TEST(Robot2025, DiagnosticCandidateNeverAssumes2026Wiring) {
  const auto config = board::robot_2025();
  EXPECT_EQ(config.mode, board::DeploymentMode::DIAGNOSTIC_MODE);
  EXPECT_EQ(config.hardware_profile, board::HardwareProfile::ROBOT_2025_HARDWARE_PROFILE);
  EXPECT_FALSE(config.commissioned);
  EXPECT_FALSE(config.valid());
  for (const auto output : config.pump_outputs)
    EXPECT_EQ(output, 0);
  for (const auto output : config.valve_outputs)
    EXPECT_EQ(output, 0);
  EXPECT_EQ(config.max_active, 0);
  for (const auto enabled : config.ax_ids)
    EXPECT_FALSE(enabled);
  for (const auto enabled : config.mx_ids)
    EXPECT_FALSE(enabled);
  for (const auto enabled : config.stepper_enabled)
    EXPECT_FALSE(enabled);
  for (const auto enabled : config.pwm_channels)
    EXPECT_FALSE(enabled);
  EXPECT_FALSE(config.pca_enabled);
  EXPECT_FALSE(config.battery_enabled);
  EXPECT_FALSE(config.leds_enabled);
  for (const auto &grip : config.grippers)
    EXPECT_FALSE(grip.reviewed);
}
TEST(Robot2025, SourceMotorOrderAndGeometryDifferFrom2026) {
  const auto config = board::robot_2025();
  EXPECT_EQ(config.motor_ids, (std::array<std::uint8_t, 3>{0x21, 0x22, 0x23}));
  EXPECT_EQ(config.battery_id, 0x11);
  EXPECT_EQ(config.chassis.geometry.layout, OmniLayout::legacy_2025);
  EXPECT_FLOAT_EQ(config.chassis.geometry.wheel_radius_m, 0.03F);
  EXPECT_FLOAT_EQ(config.chassis.geometry.center_to_wheel_m, 0.1075F);
  EXPECT_FLOAT_EQ(config.chassis.geometry.gear_ratio, 21);
  EXPECT_EQ(config.chassis.geometry.encoder_ticks_per_motor_turn, 4096U);
  EXPECT_FLOAT_EQ(config.chassis.geometry.odometry_scale, 1);
  EXPECT_FLOAT_EQ(config.chassis.geometry.command_scale, 1);
  EXPECT_EQ(config.input_pulldown_mask, 0x000f);
  EXPECT_FALSE(config.application.open_reserve_side_at_start);
}
TEST(Robot2025, KnownMechanismsRemainDormantAndUnbounded) {
  const auto config = board::robot_2025();
  EXPECT_EQ(config.grippers[0].main[0].id, 103);
  EXPECT_EQ(config.grippers[0].main[1].id, 102);
  EXPECT_FLOAT_EQ(config.grippers[0].main_up_deg, 0);
  EXPECT_FLOAT_EQ(config.grippers[0].main_speed_raw, 0);
  for (const auto input : config.home_input)
    EXPECT_EQ(input, 255);
  for (const auto &stepper : config.steppers)
    EXPECT_EQ(stepper.maximum_steps, 0);
  for (const auto value : config.stepper_current_tval)
    EXPECT_EQ(value, 24);
  for (const auto value : config.stepper_mode)
    EXPECT_EQ(value, 4);
  EXPECT_EQ(config.stepper_driver_config,
            (std::array<std::uint16_t, 4>{0x7c03, 0x2883, 0x2883, 0x2883}));
  EXPECT_EQ(config.stepper_alarm_enable, (std::array<std::uint8_t, 4>{3, 0x87, 0x87, 0x87}));
  EXPECT_EQ(config.stepper_toff_min, (std::array<std::uint8_t, 4>{31, 255, 255, 255}));
}
TEST(Robot2025, OperationalSelectionAloneCannotCommissionCandidate) {
  auto config = board::robot_2025();
  config.mode = board::DeploymentMode::OPERATIONAL_MODE;
  EXPECT_FALSE(config.valid());
  config.commissioned = true;
  EXPECT_FALSE(config.valid()); // Routing, limits and concurrency remain
                                // deliberately unset.
  EXPECT_EQ(board::Deployment{}.mode, board::DeploymentMode::DIAGNOSTIC_MODE);
  const auto previous = board::robot_2026();
  EXPECT_EQ(previous.mode, board::DeploymentMode::OPERATIONAL_MODE);
  EXPECT_EQ(previous.hardware_profile, board::HardwareProfile::ROBOT_2026_SOURCE_PROFILE);
  EXPECT_TRUE(previous.valid());
}
