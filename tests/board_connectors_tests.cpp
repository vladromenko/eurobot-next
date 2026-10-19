#include "eurobot/board_connectors.hpp"
#include "eurobot/board_installation.hpp"
#include <array>
#include <bit>
#include <gtest/gtest.h>

namespace {
namespace pins = eurobot::board::connectors;
using pins::Pin;
using pins::Port;

TEST(BoardConnectors, AllTwentyEightHeaderNumbersMatchSuppliedPoster) {
  constexpr std::array<Pin, 28> expected{
      {{Port::d, 12}, {Port::d, 13}, {Port::d, 15}, {Port::d, 14}, {Port::a, 8}, {Port::c, 11},
       {Port::d, 1},  {Port::d, 0},  {Port::b, 9},  {Port::b, 8},  {Port::d, 4}, {Port::d, 3},
       {Port::d, 7},  {Port::b, 3},  {Port::b, 5},  {Port::b, 4},  {Port::b, 6}, {Port::b, 7},
       {Port::e, 1},  {Port::e, 0},  {Port::e, 2},  {Port::e, 3},  {Port::e, 5}, {Port::e, 4},
       {Port::e, 6},  {Port::c, 13}, {Port::c, 15}, {Port::c, 14}}};
  EXPECT_EQ(pins::gpio, expected);
  for (std::size_t i = 0; i < pins::gpio.size(); ++i) {
    for (std::size_t j = 0; j < i; ++j) {
      EXPECT_NE(pins::gpio[i], pins::gpio[j]) << "header pins " << i + 1 << '/' << j + 1;
    }
  }
}

TEST(BoardConnectors, RobotInputOrderUsesActualConnectorAliases) {
  constexpr std::array<std::uint8_t, 13> numbers{27, 25, 23, 21, 28, 26, 24,
                                                 22, 20, 18, 16, 14, 12};
  constexpr std::array<Pin, 13> expected{{{Port::c, 15},
                                          {Port::e, 6},
                                          {Port::e, 5},
                                          {Port::e, 2},
                                          {Port::c, 14},
                                          {Port::c, 13},
                                          {Port::e, 4},
                                          {Port::e, 3},
                                          {Port::e, 0},
                                          {Port::b, 7},
                                          {Port::b, 4},
                                          {Port::b, 3},
                                          {Port::d, 3}}};
  EXPECT_EQ(pins::input_gpio_numbers, numbers);
  EXPECT_EQ(pins::inputs, expected);
  for (std::size_t i = 0; i < numbers.size(); ++i) {
    EXPECT_EQ(pins::inputs[i], pins::gpio[numbers[i] - 1]);
    // The I2C2 aux pins alias inputs, but its SCL/SDA pins do not.
    EXPECT_NE(pins::inputs[i], (Pin{Port::b, 10}));
    EXPECT_NE(pins::inputs[i], (Pin{Port::b, 11}));
  }
}

TEST(BoardConnectors, PwmNumberingPreservesReversedTimFourAndForwardTimFive) {
  constexpr std::array<Pin, 8> expected{{{Port::d, 15},
                                         {Port::d, 14},
                                         {Port::d, 13},
                                         {Port::d, 12},
                                         {Port::a, 0},
                                         {Port::a, 1},
                                         {Port::a, 2},
                                         {Port::a, 3}}};
  constexpr std::array<unsigned, 8> channels{4, 3, 2, 1, 1, 2, 3, 4};
  for (std::size_t i = 0; i < expected.size(); ++i) {
    EXPECT_EQ(pins::pwm[i].pin, expected[i]);
    EXPECT_EQ(pins::pwm[i].timer, i < 4 ? 4 : 5);
    EXPECT_EQ(pins::pwm[i].channel, channels[i]);
  }
  EXPECT_EQ(pins::pwm[0].pin, pins::gpio[2]); // PWM1 aliases GPIO3, not GPIO1.
  EXPECT_EQ(pins::pwm[3].pin, pins::gpio[0]); // PWM4 aliases GPIO1.
}

TEST(BoardConnectors, StepperOneIsBottomConnectorWithOriginalElectricalPins) {
  constexpr std::array<Pin, 4> chip_select{
      {{Port::e, 7}, {Port::d, 10}, {Port::d, 11}, {Port::c, 9}}};
  constexpr std::array<Pin, 4> direction{
      {{Port::e, 8}, {Port::e, 10}, {Port::e, 12}, {Port::e, 15}}};
  constexpr std::array<Pin, 4> step{{{Port::e, 9}, {Port::e, 11}, {Port::e, 13}, {Port::e, 14}}};
  constexpr std::array<unsigned, 4> reset{8, 9, 17, 16};
  for (std::size_t i = 0; i < chip_select.size(); ++i) {
    EXPECT_EQ(pins::steppers[i].chip_select, chip_select[i]);
    EXPECT_EQ(pins::steppers[i].direction, direction[i]);
    EXPECT_EQ(pins::steppers[i].step, step[i]);
  }
  EXPECT_EQ(pins::stepper_reset_bits, reset);
}

TEST(BoardConnectors, PowerAndResetResourcesPartitionAllTwentyFourShiftBits) {
  constexpr std::array<std::uint8_t, 20> expected{20, 21, 18, 19, 22, 23, 10, 11, 14, 15,
                                                  12, 13, 6,  7,  4,  5,  2,  3,  1,  0};
  EXPECT_EQ(pins::power_register_bits, expected);
  std::uint32_t power = 0, reset = 0;
  for (const auto position : pins::power_register_bits) {
    ASSERT_LT(position, 24);
    EXPECT_EQ(power & (1U << position), 0U);
    power |= 1U << position;
  }
  for (const auto position : pins::stepper_reset_bits) {
    ASSERT_LT(position, 24);
    EXPECT_EQ(reset & (1U << position), 0U);
    reset |= 1U << position;
  }
  EXPECT_EQ(std::popcount(power), 20);
  EXPECT_EQ(std::popcount(reset), 4);
  EXPECT_EQ(reset, 0x030300U);
  EXPECT_EQ(power & reset, 0U);
  EXPECT_EQ(power | reset, 0xffffffU);
}

TEST(BoardInstallation, EightPumpPlanAllocatesEveryNewChannelAndLeavesFourOutputsSpare) {
  const auto config = eurobot::board::eight_pump_installation();
  std::uint32_t physical_used = 0;
  for (std::size_t i = 0; i < 8; ++i) {
    EXPECT_EQ(config.pump_outputs[i], 2 * i + 1);
    EXPECT_EQ(config.valve_outputs[i], 2 * i + 2);
    for (const auto output : {config.pump_outputs[i], config.valve_outputs[i]}) {
      if (output < 1 || output > 16) {
        ADD_FAILURE() << "physical output outside installation range: " << unsigned(output);
        return;
      }
      EXPECT_EQ(physical_used & (1U << (output - 1)), 0U);
      physical_used |= 1U << (output - 1);
    }
  }
  EXPECT_EQ(physical_used, 0xffffU);
  for (std::size_t i = 8; i < config.pump_outputs.size(); ++i) {
    EXPECT_EQ(config.pump_outputs[i], 0);
    EXPECT_EQ(config.valve_outputs[i], 0);
  }
}

TEST(BoardInstallation, ChannelsFiveThroughEightReachTheirRealPowerBitsAndPreserveReset) {
  const auto config = eurobot::board::eight_pump_installation();
  constexpr std::array<unsigned, 4> pump_bits{14, 12, 6, 4};
  constexpr std::array<unsigned, 4> valve_bits{15, 13, 7, 5};
  constexpr auto reset = 0x030300U;
  for (std::size_t i = 0; i < 4; ++i) {
    const auto logical = 1U << (i + 4);
    const auto pump = config.output_word(logical, 0, 0xffffffU);
    const auto valve = config.output_word(0, logical, 0xffffffU);
    ASSERT_TRUE(pump);
    ASSERT_TRUE(valve);
    EXPECT_EQ(*pump, reset | (1U << pump_bits[i]));
    EXPECT_EQ(*valve, reset | (1U << valve_bits[i]));
  }
  const auto all = config.output_word(0xff, 0xff, reset);
  ASSERT_TRUE(all);
  EXPECT_EQ(*all, 0xfcfcf0U | reset);
  const auto off = config.output_word(0, 0, *all);
  ASSERT_TRUE(off);
  EXPECT_EQ(*off, reset);
  EXPECT_FALSE(config.output_word(0x100, 0));
  EXPECT_FALSE(config.output_word(0, 0x100));
}

TEST(BoardInstallation, GrippersHaveIndependentFourChannelPumpAndValveResources) {
  const auto config = eurobot::board::eight_pump_installation();
  std::array<std::uint32_t, 2> pumps{}, valves{};
  for (std::size_t gripper = 0; gripper < 2; ++gripper) {
    for (std::size_t i = 0; i < 4; ++i) {
      const auto channel = static_cast<unsigned>(4 * gripper + i);
      EXPECT_EQ(config.grippers[gripper].pump_channels[i], channel);
      EXPECT_EQ(config.grippers[gripper].valve_channels[i], channel);
      pumps[gripper] |= 1U << channel;
      valves[gripper] |= 1U << channel;
    }
  }
  EXPECT_EQ(pumps[0], 0x0fU);
  EXPECT_EQ(pumps[1], 0xf0U);
  EXPECT_EQ(pumps[0] & pumps[1], 0U);
  EXPECT_EQ(valves[0] & valves[1], 0U);
  const auto first = config.output_word(pumps[0], valves[0]);
  const auto second = config.output_word(pumps[1], valves[1]);
  ASSERT_TRUE(first);
  ASSERT_TRUE(second);
  EXPECT_EQ(*first & *second, 0U);
  EXPECT_EQ(*first | *second, 0xfcfcf0U);
}

TEST(BoardInstallation, RoutingPlanDoesNotInventCommissioningOrUnknownServoIdentity) {
  auto config = eurobot::board::eight_pump_installation();
  EXPECT_EQ(config.channels, 8);
  EXPECT_FALSE(config.commissioned);
  EXPECT_FALSE(config.valid());
  EXPECT_EQ(config.max_active, 0);
  EXPECT_FALSE(config.grippers[0].reviewed);
  EXPECT_FALSE(config.grippers[1].reviewed);
  EXPECT_EQ(config.grippers[0].main[0].id, eurobot::gripper::unassigned);
  EXPECT_EQ(config.application.mode, eurobot::application::Mode::game);
  EXPECT_EQ(config.application.match_duration_ms, 99000U);
  EXPECT_EQ(config.application.strategy_count, 3);
  config.commissioned = true;
  EXPECT_FALSE(config.valid()); // Supply budget and motion limits are still unset.
  config.max_active = 8;
  EXPECT_FALSE(config.valid()); // A wiring table alone cannot provide safe
                                // mechanical limits.
}
} // namespace
