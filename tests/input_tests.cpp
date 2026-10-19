#include "eurobot/inputs.hpp"
#include <gtest/gtest.h>
using namespace eurobot;
using namespace eurobot::inputs;

TEST(Inputs, InitializationRequiresStableSamplesWithoutPhantomPress) {
  DebouncedInput input;
  EXPECT_FALSE(input.sample(false, 0).state.known);
  EXPECT_FALSE(input.sample(true, 7).state.known);
  EXPECT_FALSE(input.sample(true, 14).state.known);
  const auto stable = input.sample(true, 15);
  EXPECT_TRUE(stable.state.known);
  EXPECT_TRUE(stable.state.pressed);
  EXPECT_EQ(stable.events, 0);
  for (unsigned now = 25; now < 1000; now += 10) {
    EXPECT_EQ(input.sample(true, now).events, 0);
  }
}

TEST(Inputs, BounceMustBeContinuouslyStableBeforeEachEdge) {
  DebouncedInput input;
  static_cast<void>(input.sample(false, 0));
  EXPECT_TRUE(input.sample(false, 8).state.known);
  EXPECT_EQ(input.sample(true, 10).events, 0);
  EXPECT_EQ(input.sample(false, 14).events, 0);
  EXPECT_EQ(input.sample(true, 17).events, 0);
  EXPECT_EQ(input.sample(true, 24).events, 0);
  const auto press = input.sample(true, 25);
  EXPECT_TRUE(has_event(press.events, Event::pressed));
  EXPECT_EQ(input.sample(true, 26).events, 0);
  EXPECT_EQ(input.sample(false, 30).events, 0);
  EXPECT_TRUE(has_event(input.sample(false, 38).events, Event::released));
  EXPECT_EQ(input.sample(false, 39).events, 0);
}

TEST(Inputs, HoldOccursOncePerDebouncedPressAndCanBeDisabled) {
  DebouncedInput input{{true, 3, 20, 100}};
  static_cast<void>(input.sample(false, 0));
  static_cast<void>(input.sample(false, 3));
  static_cast<void>(input.sample(true, 10));
  EXPECT_TRUE(has_event(input.sample(true, 13).events, Event::pressed));
  EXPECT_EQ(input.sample(true, 32).events, 0);
  EXPECT_TRUE(has_event(input.sample(true, 33).events, Event::held));
  EXPECT_EQ(input.sample(true, 34).events, 0);
  static_cast<void>(input.sample(false, 40));
  EXPECT_TRUE(has_event(input.sample(false, 43).events, Event::released));
  static_cast<void>(input.sample(true, 50));
  static_cast<void>(input.sample(true, 53));
  EXPECT_TRUE(has_event(input.sample(true, 73).events, Event::held));
  DebouncedInput disabled{{true, 3, 0, 100}};
  static_cast<void>(disabled.sample(false, 0));
  static_cast<void>(disabled.sample(false, 3));
  static_cast<void>(disabled.sample(true, 5));
  static_cast<void>(disabled.sample(true, 8));
  for (unsigned now = 18; now < 1000; now += 10) {
    EXPECT_EQ(disabled.sample(true, now).events, 0);
  }
}

TEST(Inputs, ClickIsShortPressReleaseAndNeverAnInitialHeldRelease) {
  DebouncedInput input{{true, 3, 20, 100}};
  static_cast<void>(input.sample(true, 0));
  static_cast<void>(input.sample(true, 3));
  static_cast<void>(input.sample(false, 5));
  const auto initial_release = input.sample(false, 8);
  EXPECT_TRUE(has_event(initial_release.events, Event::released));
  EXPECT_FALSE(has_event(initial_release.events, Event::clicked));
  static_cast<void>(input.sample(true, 10));
  static_cast<void>(input.sample(true, 13));
  static_cast<void>(input.sample(false, 15));
  EXPECT_TRUE(has_event(input.sample(false, 18).events, Event::clicked));
  static_cast<void>(input.sample(true, 20));
  static_cast<void>(input.sample(true, 23));
  EXPECT_TRUE(has_event(input.sample(true, 43).events, Event::held));
  static_cast<void>(input.sample(false, 45));
  EXPECT_FALSE(has_event(input.sample(false, 48).events, Event::clicked));
}

TEST(Inputs, ActiveLowUsesElectricalPolarityWithoutChangingEventMeaning) {
  DebouncedInput input{{false, 8, 500, 20}};
  static_cast<void>(input.sample(true, 0));
  EXPECT_FALSE(input.sample(true, 8).state.pressed);
  static_cast<void>(input.sample(false, 10));
  const auto pressed = input.sample(false, 18);
  EXPECT_TRUE(pressed.state.pressed);
  EXPECT_TRUE(has_event(pressed.events, Event::pressed));
}

TEST(Inputs, StaleSnapshotAndSamplingGapInvalidateUntilStableAgain) {
  DebouncedInput input{{true, 8, 500, 20}};
  static_cast<void>(input.sample(false, 0));
  static_cast<void>(input.sample(false, 8));
  EXPECT_TRUE(input.snapshot(28).known);
  EXPECT_FALSE(input.snapshot(29).known);
  const auto gap = input.sample(true, 40);
  EXPECT_EQ(gap.error, Error::timeout);
  EXPECT_FALSE(gap.state.known);
  EXPECT_EQ(gap.events, 0);
  const auto initial = input.sample(true, 48);
  EXPECT_TRUE(initial.state.known);
  EXPECT_TRUE(initial.state.pressed);
  EXPECT_EQ(initial.events, 0);
  input.invalidate();
  EXPECT_FALSE(input.snapshot(48).known);
}

TEST(Inputs, UnsignedMillisWrapRetainsDebounceAndHoldTiming) {
  DebouncedInput input{{true, 8, 20, 50}};
  static_cast<void>(input.sample(false, 0xfffffff0U));
  EXPECT_TRUE(input.sample(false, 0xfffffff8U).state.known);
  static_cast<void>(input.sample(true, 0xfffffff9U));
  EXPECT_TRUE(has_event(input.sample(true, 1).events, Event::pressed));
  EXPECT_EQ(input.sample(true, 20).events, 0);
  EXPECT_TRUE(has_event(input.sample(true, 21).events, Event::held));
}

TEST(Inputs, BackwardTimeCannotCreateAnEdge) {
  DebouncedInput input;
  static_cast<void>(input.sample(false, 100));
  static_cast<void>(input.sample(false, 108));
  const auto backward = input.sample(true, 50);
  EXPECT_EQ(backward.error, Error::invalid_argument);
  EXPECT_FALSE(backward.state.known);
  EXPECT_EQ(backward.events, 0);
  EXPECT_EQ(input.sample(true, 58).events, 0);
}

TEST(Inputs, InvalidConfigurationFailsUnknown) {
  for (const auto config :
       {InputConfig{true, 0, 500, 20}, InputConfig{true, 8, 7, 20}, InputConfig{true, 8, 500, 0},
        InputConfig{true, 0x80000000U, 0, 20}, InputConfig{true, 8, 0x80000000U, 20},
        InputConfig{true, 8, 500, 0x80000000U}}) {
    DebouncedInput input{config};
    EXPECT_FALSE(input.valid());
    const auto update = input.sample(true, 100);
    EXPECT_EQ(update.error, Error::invalid_config);
    EXPECT_FALSE(update.state.known);
  }
}

TEST(Inputs, ThirteenInputsReturnIndependentMasksAndEdges) {
  InputBank bank;
  EXPECT_TRUE(bank.valid());
  EXPECT_EQ(bank.sample(0, 0).known, 0U);
  EXPECT_EQ(bank.sample(0, 8).known, 0x1fffU);
  EXPECT_EQ(bank.sample(0x1001, 10).pressed_edges, 0U);
  const auto press = bank.sample(0x1001, 18);
  EXPECT_EQ(press.known, 0x1fffU);
  EXPECT_EQ(press.pressed, 0x1001U);
  EXPECT_EQ(press.pressed_edges, 0x1001U);
  EXPECT_EQ(bank.sample(0x1001, 19).pressed_edges, 0U);
  static_cast<void>(bank.sample(0, 20));
  EXPECT_EQ(bank.sample(0, 28).released_edges, 0x1001U);
  EXPECT_EQ(bank.sample(1U << 13, 29).error, Error::invalid_argument);
  EXPECT_EQ(bank.snapshot(29).pressed, 0U);
  EXPECT_EQ(bank.snapshot(49).known, 0U);
}

TEST(Inputs, FullThirtyTwoBitBankDoesNotShiftByThirtyTwo) {
  InputBank<32> bank;
  static_cast<void>(bank.sample(0, 0));
  EXPECT_EQ(bank.sample(0, 8).known, 0xffffffffU);
  static_cast<void>(bank.sample(0x80000000U, 10));
  EXPECT_EQ(bank.sample(0x80000000U, 18).pressed_edges, 0x80000000U);
}
