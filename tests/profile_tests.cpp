#include "eurobot/profile.hpp"
#include <array>
#include <cmath>
#include <gtest/gtest.h>
#include <limits>

namespace {
using eurobot::board::Deployment;
// Synthetic TEST wiring only; these extra outputs are not a commissioned robot profile.
Deployment fixture() {
  Deployment p{};
  p.commissioned = true;
  p.channels = 8;
  p.max_active = 8;
  p.pump_outputs = {1, 3, 5, 7, 9, 11, 13, 15};
  p.valve_outputs = {2, 4, 6, 8, 10, 12, 14, 16};
  p.chassis.geometry = eurobot::legacy_2026_geometry();
  p.chassis.max_linear_mps = 0.2F;
  p.chassis.max_angular_radps = 0.5F;
  p.chassis.max_accel_mps2 = 0.3F;
  p.chassis.max_angular_accel_radps2 = 0.6F;
  p.chassis.max_wheel_tpm = 1000000;
  p.chassis.max_encoder_delta = 10000;
  return p;
}

TEST(Deployment, SafeDefaultRejectsArmingConfiguration) {
  const Deployment p{};
  EXPECT_EQ(p.channels, 8);
  EXPECT_FALSE(p.commissioned);
  EXPECT_FALSE(p.valid());
  EXPECT_EQ(p.pump_outputs[0], 1);
  EXPECT_EQ(p.pump_outputs[3], 7);
  EXPECT_EQ(p.pump_outputs[4], 0);
  EXPECT_EQ(p.pump_outputs[7], 0);
}

TEST(Deployment, ExplicitSyntheticProfileValidates) { EXPECT_TRUE(fixture().valid()); }

TEST(Deployment, AllTwentyPhysicalOutputsUseSourceVerifiedBitPositions) {
  constexpr std::array<unsigned, 20> expected{20, 21, 18, 19, 22, 23, 10, 11, 14, 15,
                                              12, 13, 6,  7,  4,  5,  2,  3,  1,  0};
  for (std::size_t i = 0; i < expected.size(); ++i) {
    auto p = fixture();
    p.pump_outputs[0] = static_cast<std::uint8_t>(i + 1);
    const auto word = p.output_word(1, 0);
    ASSERT_TRUE(word.has_value());
    EXPECT_EQ(*word, 1U << expected[i]) << "physical output " << i + 1;
  }
}

TEST(Deployment, MaskUpdatesPreserveStepperResetBitsAndNoTransientBits) {
  const auto p = fixture();
  constexpr auto reset = (1U << 8) | (1U << 9) | (1U << 17) | (1U << 16);
  const auto word = p.output_word(0x05, 0x0a, 0xffffffU);
  ASSERT_TRUE(word);
  EXPECT_EQ(*word, reset | (1U << 20) | (1U << 22) | (1U << 19) | (1U << 11));
  const auto off = p.output_word(0, 0, reset);
  ASSERT_TRUE(off);
  EXPECT_EQ(*off, reset);
}

TEST(Deployment, UnassignedAndOutOfRangeActiveOutputRejected) {
  auto p = fixture();
  p.pump_outputs[7] = 0;
  EXPECT_FALSE(p.valid());
  EXPECT_FALSE(p.output_word(0x80, 0));
  p = fixture();
  p.pump_outputs[7] = 21;
  EXPECT_FALSE(p.valid());
  EXPECT_FALSE(p.output_word(0x80, 0));
  p = fixture();
  p.valve_outputs[7] = 21;
  EXPECT_FALSE(p.valid());
  EXPECT_FALSE(p.output_word(0, 0x80));
  EXPECT_FALSE(fixture().output_word(0x100, 0));
  EXPECT_FALSE(fixture().output_word(0, 0x100));
}

TEST(Deployment, PublicMaskTranslatorBoundsChannelsBeforeAnyShift) {
  for (auto count : {0U, 33U, 255U}) {
    auto p = fixture();
    p.channels = static_cast<std::uint8_t>(count);
    EXPECT_FALSE(p.valid());
    EXPECT_FALSE(p.output_word(0, 0));
  }
}

TEST(Deployment, PumpAndValveAliasesRejectedAcrossAllChannels) {
  auto p = fixture();
  p.pump_outputs[7] = p.pump_outputs[0];
  EXPECT_FALSE(p.valid());
  p = fixture();
  p.valve_outputs[7] = p.valve_outputs[0];
  EXPECT_FALSE(p.valid());
  p = fixture();
  p.valve_outputs[7] = p.pump_outputs[0];
  EXPECT_FALSE(p.valid());
  p = fixture();
  p.valve_outputs[7] = 0; // An intentionally absent valve is allowed.
  EXPECT_TRUE(p.valid());
}

TEST(Deployment, PhysicalCapacityDoesNotInventThirtyTwoOutputs) {
  auto p = fixture();
  p.channels = 16;
  EXPECT_FALSE(p.valid());
  p.channels = 32;
  EXPECT_FALSE(p.valid());
  p = fixture();
  p.max_active = 0;
  EXPECT_FALSE(p.valid());
  p.max_active = 9;
  EXPECT_FALSE(p.valid());
}

TEST(Deployment, MotorIdsAreUniqueUnicastValues) {
  for (auto bad : {0U, 254U, 255U}) {
    auto p = fixture();
    p.motor_ids[2] = static_cast<std::uint8_t>(bad);
    EXPECT_FALSE(p.valid());
  }
  auto p = fixture();
  p.motor_ids[2] = p.motor_ids[0];
  EXPECT_FALSE(p.valid());
}

TEST(Deployment, DcAndServoCannotShareTimerEvenOnDifferentChannels) {
  for (std::size_t dc = 0; dc < 4; ++dc) {
    for (std::size_t pwm = 0; pwm < 8; ++pwm) {
      auto p = fixture();
      p.dc_channels[dc] = true;
      p.dc_frequency_hz[dc / 2] = 1000;
      p.dc_modes[dc] = eurobot::board::DcMode::legacy_high_pair;
      p.pwm_channels[pwm] = true;
      EXPECT_EQ(p.valid(), (dc / 2) != (pwm / 4)) << "DC " << dc << " PWM " << pwm;
    }
  }
}

TEST(Deployment, NonfiniteAndZeroChassisLimitsRejected) {
  using Member = float eurobot::ChassisConfig::*;
  constexpr std::array<Member, 4> limits{
      &eurobot::ChassisConfig::max_linear_mps, &eurobot::ChassisConfig::max_angular_radps,
      &eurobot::ChassisConfig::max_accel_mps2, &eurobot::ChassisConfig::max_angular_accel_radps2};
  for (const auto member : limits) {
    for (auto value : {0.0F, -1.0F, std::numeric_limits<float>::infinity(),
                       std::numeric_limits<float>::quiet_NaN()}) {
      auto p = fixture();
      p.chassis.*member = value;
      EXPECT_FALSE(p.valid());
    }
  }
}

TEST(Deployment, GeometryAndPulseLimitsRejectInvalidCommissioning) {
  auto p = fixture();
  p.chassis.geometry.wheel_radius_m = std::numeric_limits<float>::quiet_NaN();
  EXPECT_FALSE(p.valid());
  p = fixture();
  p.stepper_enabled[0] = true;
  EXPECT_FALSE(p.valid());
  p.steppers[0] = {-100, 100, 1000, 2000};
  EXPECT_TRUE(p.valid());
}
} // namespace

TEST(Deployment, BridgeElectricalModeIsExplicitAndLegacyMappingPreserved) {
  using namespace eurobot::board;
  EXPECT_FALSE(dc_pwm_pair(0.5F, DcMode::unconfigured));
  auto forward = dc_pwm_pair(.25F, DcMode::legacy_high_pair);
  ASSERT_TRUE(forward);
  EXPECT_FLOAT_EQ((*forward)[0], .25F);
  EXPECT_FLOAT_EQ((*forward)[1], 1);
  auto reverse = dc_pwm_pair(-.75F, DcMode::legacy_high_pair);
  ASSERT_TRUE(reverse);
  EXPECT_FLOAT_EQ((*reverse)[0], 1);
  EXPECT_FLOAT_EQ((*reverse)[1], .75F);
  auto stop = dc_pwm_pair(0, DcMode::legacy_high_pair);
  ASSERT_TRUE(stop);
  EXPECT_FLOAT_EQ((*stop)[0], 0);
  EXPECT_FLOAT_EQ((*stop)[1], 0);
  auto coast = dc_pwm_pair(.25F, DcMode::coast_pwm);
  ASSERT_TRUE(coast);
  EXPECT_FLOAT_EQ((*coast)[0], .25F);
  EXPECT_FLOAT_EQ((*coast)[1], 0);
  EXPECT_FALSE(dc_pwm_pair(1.1F, DcMode::coast_pwm));
}

TEST(Deployment, DcFrequencyRequiresReviewedTimerCompatibleConfiguration) {
  auto p = fixture();
  p.dc_channels[0] = true;
  p.dc_modes[0] = eurobot::board::DcMode::legacy_high_pair;
  EXPECT_FALSE(p.valid());
  p.dc_frequency_hz[0] = 1000;
  EXPECT_TRUE(p.valid());
  for (auto frequency : {1U, 15U, 1000001U, 30000U}) {
    p.dc_frequency_hz[0] = frequency;
    EXPECT_FALSE(p.valid());
  }
}

TEST(Deployment, ProfiledStepperRejectsUnserviceablePeakBeforeEnablingHardware) {
  auto p = fixture();
  p.stepper_enabled[0] = true;
  p.steppers[0] = {-1000, 1000, 2000, 1000};
  p.stepper_profile_enabled[0] = true;
  EXPECT_TRUE(p.valid());
  p.stepper_profiles[0].working_division = 1;
  EXPECT_FALSE(p.valid());
  p.stepper_profiles[0] = {19200, 32, 10, 200};
  p.steppers[0].maximum_tick_us = 4000;
  EXPECT_FALSE(p.valid()); // More than64 virtual base ticks per service would lose pulses.
  p.steppers[0].maximum_tick_us = 1000;
  p.stepper_profiles[0].acceleration_ms = 60001;
  EXPECT_FALSE(p.valid());
}
TEST(Deployment, GenericPwmCannotRetuneAnActiveServoOrHBridgeGroup) {
  auto p = fixture();
  p.pwm_generic_channels[0] = true;
  p.pwm_frequency_hz[0] = 1000;
  EXPECT_TRUE(p.valid());
  p.pwm_channels[1] = true;
  EXPECT_FALSE(p.valid());
  p.pwm_frequency_hz[0] = 50;
  EXPECT_TRUE(p.valid());
  p.dc_channels[0] = true;
  p.dc_modes[0] = eurobot::board::DcMode::coast_pwm;
  p.dc_frequency_hz[0] = 1000;
  EXPECT_FALSE(p.valid());
}
TEST(Deployment, LedChainSupportsAllSixtyLegacyPixelsButRejectsBufferOverflow) {
  auto p = fixture();
  p.leds_enabled = true;
  p.led_count = 60;
  EXPECT_TRUE(p.valid());
  p.led_count = 61;
  EXPECT_FALSE(p.valid());
  p.led_count = 0;
  EXPECT_FALSE(p.valid());
}

TEST(Deployment, InvalidLocalSafetyPolicyCannotBeClearedByTransportRearm) {
  auto p = fixture();
  p.application.inputs[0].maximum_sample_gap_ms = 0;
  EXPECT_FALSE(p.valid());
  p = fixture();
  p.application.strategy_count = 0;
  EXPECT_FALSE(p.valid());
  p = fixture();
  p.application.match_duration_ms = 0x80000000U;
  EXPECT_FALSE(p.valid());
}
