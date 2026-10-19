#include "eurobot/gripper.hpp"
#include <algorithm>
#include <array>
#include <gtest/gtest.h>
#include <limits>

namespace {
using namespace eurobot;
namespace grip = eurobot::gripper;
struct PlanBackend {
  unsigned issued{}, stopped{};
  Error feedback_error{Error::ok};
  static Error guard(void *, std::uint32_t owner, std::uint32_t) {
    return owner == 77 ? Error::ok : Error::lease_conflict;
  }
  static Error validate(void *, const grip::Step &) { return Error::ok; }
  static Error issue(void *context, const grip::Step &, std::uint32_t) {
    ++static_cast<PlanBackend *>(context)->issued;
    return Error::ok;
  }
  static Error feedback(void *context, const grip::Step &step, grip::Completion &completion,
                        std::uint32_t) {
    completion = {true, true, step.required};
    return static_cast<PlanBackend *>(context)->feedback_error;
  }
  static Error stop(void *context, Error) {
    ++static_cast<PlanBackend *>(context)->stopped;
    return Error::ok;
  }
  grip::Sinks sinks() { return {this, guard, validate, issue, feedback, stop}; }
};
grip::Config primary() {
  auto config = grip::legacy_2026_reference(0);
  config.reviewed = true;
  config.pump_channels = config.valve_channels = {0, 1, 2, 3};
  config.composite = grip::Composite::base_strategy_2026;
  // Test-only motion values: these are not a physical deployment approval.
  config.capture_forward_mps = 0.05F;
  config.capture_backward_mps = 0.04F;
  config.capture_forward_ms = config.capture_backward_ms = 100;
  return config;
}
TEST(PrimaryGripper, SourceSelectionUses112AndKeepsOtherSlotUnassigned) {
  const auto first = grip::legacy_2026_reference(0), reserve = grip::legacy_2026_reference(1);
  EXPECT_EQ(first.main_active_mask, 2);
  EXPECT_EQ(first.main[0].id, grip::unassigned);
  EXPECT_EQ(first.main[1].id, 112);
  EXPECT_EQ(reserve.main_active_mask, 3);
  EXPECT_EQ(reserve.main[0].id, 1);
  EXPECT_EQ(reserve.main[1].id, 2);
  EXPECT_EQ(grip::Config{}.main_active_mask, 3);
  EXPECT_EQ(grip::legacy_2025_reference().main_active_mask, 3);
}
TEST(PrimaryGripper, EverySourceMainPoseRequiresMeasuredCompletionFrom112Only) {
  for (unsigned state = 0; state < 3; ++state) {
    SCOPED_TRACE(state);
    auto config = primary();
    PlanBackend backend;
    grip::Engine engine{{&config, 1}, backend.sinks()};
    ASSERT_EQ(engine.start(77, {grip::Operation::rotate_main, 0, static_cast<float>(state)}, 0),
              Error::ok);
    ASSERT_EQ(engine.plan().size(), 1U);
    const auto &step = engine.plan()[0];
    ASSERT_EQ(step.count, 1);
    EXPECT_EQ(step.targets[0].id, 112);
    EXPECT_FLOAT_EQ(step.targets[0].value, (std::array<float, 3>{65, 149, 99})[state]);
    EXPECT_EQ(step.required, grip::Evidence::measured);
    ASSERT_EQ(engine.tick(0), Error::ok);
    EXPECT_TRUE(engine.busy());
    EXPECT_EQ(backend.issued, 1U);
    ASSERT_EQ(engine.tick(200), Error::ok);
    EXPECT_EQ(engine.state().phase, grip::Phase::completed);
    EXPECT_EQ(engine.state().measured_steps, 1);
  }
}
TEST(PrimaryGripper, InactiveSlotIsExplicitlyExcludedRatherThanAddressGuessed) {
  auto config = primary();
  config.main[0] = {grip::Device::ax, 254, std::numeric_limits<float>::quiet_NaN(), 0};
  PlanBackend backend;
  grip::Engine engine{{&config, 1}, backend.sinks()};
  ASSERT_EQ(engine.start(77, {grip::Operation::rotate_main}, 0), Error::ok);
  ASSERT_EQ(engine.plan()[0].count, 1);
  EXPECT_EQ(engine.plan()[0].targets[0].id, 112);
  config = primary();
  config.main_active_mask = 1;
  config.main[0] = {grip::Device::ax, 42, 0, 300};
  config.main[1].id = grip::unassigned;
  grip::Engine first_slot{{&config, 1}, backend.sinks()};
  ASSERT_EQ(first_slot.start(77, {grip::Operation::rotate_main}, 0), Error::ok);
  ASSERT_EQ(first_slot.plan()[0].count, 1);
  EXPECT_EQ(first_slot.plan()[0].targets[0].id, 42);
}
TEST(PrimaryGripper, SecondAddressIsUsedOnlyWhenBothSlotsExplicitlySelected) {
  auto config = primary();
  config.main[0] = {grip::Device::ax, 42, 0, 300};
  PlanBackend backend;
  grip::Engine single{{&config, 1}, backend.sinks()};
  ASSERT_EQ(single.start(77, {grip::Operation::rotate_main}, 0), Error::ok);
  EXPECT_EQ(single.plan()[0].count, 1);
  config.main_active_mask = 3;
  grip::Engine paired{{&config, 1}, backend.sinks()};
  ASSERT_EQ(paired.start(77, {grip::Operation::rotate_main}, 0), Error::ok);
  ASSERT_EQ(paired.plan()[0].count, 2);
  EXPECT_EQ(paired.plan()[0].targets[0].id, 42);
  EXPECT_EQ(paired.plan()[0].targets[1].id, 112);
}
TEST(PrimaryGripper, InvalidMaskOrInvalidSelectedAddressNeverIssuesAnything) {
  for (auto mask : {0U, 4U, 255U}) {
    auto config = primary();
    config.main_active_mask = static_cast<std::uint8_t>(mask);
    PlanBackend backend;
    grip::Engine engine{{&config, 1}, backend.sinks()};
    EXPECT_EQ(engine.start(77, {grip::Operation::rotate_main}, 0), Error::invalid_config);
    EXPECT_EQ(backend.issued, 0U);
  }
  for (auto id : {0U, 254U, 255U}) {
    auto config = primary();
    config.main[1].id = static_cast<std::uint8_t>(id);
    PlanBackend backend;
    grip::Engine engine{{&config, 1}, backend.sinks()};
    EXPECT_EQ(engine.start(77, {grip::Operation::rotate_main}, 0), Error::invalid_config);
    EXPECT_EQ(backend.issued, 0U);
  }
  auto config = primary();
  config.main_active_mask = 3; // The unassigned slot is active: reject, never silently drop it.
  PlanBackend backend;
  grip::Engine engine{{&config, 1}, backend.sinks()};
  EXPECT_EQ(engine.start(77, {grip::Operation::rotate_main}, 0), Error::invalid_config);
  EXPECT_EQ(backend.issued, 0U);
}
TEST(PrimaryGripper, EveryMainRecipeExcludesInactiveSlotFromItsEntirePlan) {
  for (auto operation : {grip::Operation::rotate_main, grip::Operation::capture,
                         grip::Operation::release, grip::Operation::prepare,
                         grip::Operation::legacy_2025_reset, grip::Operation::legacy_2025_cycle}) {
    SCOPED_TRACE(static_cast<unsigned>(operation));
    auto config = primary();
    PlanBackend backend;
    grip::Engine engine{{&config, 1}, backend.sinks()};
    ASSERT_EQ(engine.start(77, {operation}, 0), Error::ok);
    unsigned main_steps = 0;
    for (const auto &step : engine.plan()) {
      if (step.action == grip::Action::servo_group) {
        for (unsigned i = 0; i < step.count; ++i) {
          EXPECT_GT(step.targets[i].id, 0);
          EXPECT_LT(step.targets[i].id, 254);
          if (step.targets[i].id == 112) {
            ++main_steps;
            EXPECT_EQ(step.count, 1);
          }
        }
      }
    }
    EXPECT_GT(main_steps, 0U);
  }
}
TEST(PrimaryGripper, ReserveStillCommandsBothSupportedSourceAddresses) {
  auto config = grip::legacy_2026_reference(1);
  config.reviewed = true;
  PlanBackend backend;
  grip::Engine engine{{&config, 1}, backend.sinks()};
  ASSERT_EQ(engine.start(77, {grip::Operation::rotate_main}, 0), Error::ok);
  ASSERT_EQ(engine.plan()[0].count, 2);
  EXPECT_EQ(engine.plan()[0].targets[0].id, 1);
  EXPECT_EQ(engine.plan()[0].targets[1].id, 2);
}
TEST(PrimaryGripper, Missing112FeedbackNeverProducesMeasuredSuccess) {
  auto config = primary();
  PlanBackend backend;
  backend.feedback_error = Error::timeout;
  grip::Engine engine{{&config, 1}, backend.sinks()};
  ASSERT_EQ(engine.start(77, {grip::Operation::rotate_main}, 0), Error::ok);
  EXPECT_EQ(engine.tick(0), Error::timeout);
  EXPECT_EQ(engine.state().phase, grip::Phase::faulted);
  EXPECT_EQ(engine.state().measured_steps, 0);
  EXPECT_EQ(backend.stopped, 1U);
}
} // namespace
