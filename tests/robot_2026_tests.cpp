#include "../examples/legacy_2026_reference.hpp"
#include "eurobot/robot_2026.hpp"
#include <array>
#include <bit>
#include <cmath>
#include <gtest/gtest.h>
#include <numbers>

namespace {
using namespace eurobot;
namespace grip = eurobot::gripper;

struct PlanIo {
  Error authority{Error::ok};
  unsigned writes{}, stops{};
  grip::Step last{};
  static Error guard(void *context, std::uint32_t owner, std::uint32_t) {
    auto &self = *static_cast<PlanIo *>(context);
    return owner == 77 ? self.authority : Error::lease_conflict;
  }
  static Error validate(void *, const grip::Step &) { return Error::ok; }
  static Error issue(void *context, const grip::Step &step, std::uint32_t) {
    auto &self = *static_cast<PlanIo *>(context);
    ++self.writes;
    self.last = step;
    return Error::ok;
  }
  static Error feedback(void *, const grip::Step &step, grip::Completion &result, std::uint32_t) {
    result = {true, true, step.required};
    return Error::ok;
  }
  static Error stop(void *context, Error) {
    ++static_cast<PlanIo *>(context)->stops;
    return Error::ok;
  }
  grip::Sinks sinks() { return {this, guard, validate, issue, feedback, stop}; }
};

TEST(Robot2026, ReviewedProfileDoesNotArmControllerOrEnergizeBootOutputs) {
  struct Hardware {
    std::uint32_t pumps{}, valves{};
    std::array<std::int32_t, 3> wheels{};
    static Error outputs(void *context, std::uint32_t pumps, std::uint32_t valves) {
      auto &self = *static_cast<Hardware *>(context);
      self.pumps = pumps;
      self.valves = valves;
      return Error::ok;
    }
    static Error motors(void *context, const std::array<std::int32_t, 3> &values) {
      static_cast<Hardware *>(context)->wheels = values;
      return Error::ok;
    }
  } io;
  const auto p = board::robot_2026();
  PumpBank pumps{{p.channels, p.max_active, 250, p.max_pump_on_ms}, {&io, Hardware::outputs}};
  Chassis chassis{p.chassis, {&io, Hardware::motors}};
  Controller controller{pumps, chassis};
  ASSERT_EQ(controller.initialize(0), Error::ok);
  EXPECT_FALSE(controller.state().armed);
  EXPECT_EQ(io.pumps, 0U);
  EXPECT_EQ(io.valves, 0U);
  EXPECT_EQ(io.wheels, (std::array<std::int32_t, 3>{}));
  EXPECT_EQ(controller.pumps(77, 1, 1, 1, 0, 0, 0), Error::lease_expired);
  ASSERT_EQ(controller.claim(77, 1, 1000, 0), Error::ok);
  EXPECT_EQ(controller.pumps(77, 2, 1, 1, 0, 0, 0), Error::not_armed);
  EXPECT_EQ(io.pumps, 0U);
}

TEST(Robot2026, CompleteSourceConfigurationValidatesWithoutFabricatingPrimaryIdentity) {
  const auto reference = eurobot::source_2026::reference();
  EXPECT_EQ(reference.grippers[0].lift_speed_steps_s, 1920);
  EXPECT_TRUE(reference.grippers[0].disable_lift_after_move);
  EXPECT_EQ(reference.stepper_current_tval[0], 34);
  EXPECT_EQ(reference.home_input[0], 5);
  EXPECT_FALSE(reference.commissioned);
  EXPECT_FALSE(reference.valid());
  const auto p = board::robot_2026();
  EXPECT_TRUE(p.valid());
  EXPECT_TRUE(p.commissioned); // Source configuration review, not a hardware result.
  EXPECT_TRUE(p.grippers[0].reviewed && p.grippers[1].reviewed);
  EXPECT_EQ(p.grippers[0].main[0].id, grip::unassigned);
  EXPECT_EQ(p.channels, 8);
  EXPECT_EQ(p.max_active, 4);
  EXPECT_EQ(p.max_pump_on_ms, 99000U);
  EXPECT_EQ(p.motor_ids, (std::array<std::uint8_t, 3>{0x22, 0x21, 0x23}));
  EXPECT_EQ(p.battery_id, 0x11);
  EXPECT_TRUE(p.battery_enabled && p.leds_enabled && p.pca_enabled);
  EXPECT_EQ(p.led_count, 60);
  EXPECT_TRUE(p.stepper_enabled[0] && p.stepper_profile_enabled[0]);
  for (std::size_t i = 1; i < 4; ++i) {
    EXPECT_FALSE(p.stepper_enabled[i]);
    EXPECT_FALSE(p.stepper_profile_enabled[i]);
  }
}

TEST(Robot2026, StepperRegisterBytesMatchFinalSourceAndDocumentedResetDefaults) {
  struct Spi {
    std::array<std::uint8_t, 4> command{};
    std::size_t size{};
    static Error transfer(void *context, std::span<const std::uint8_t> tx, std::span<std::uint8_t>,
                          std::uint32_t) {
      auto &self = *static_cast<Spi *>(context);
      self.command.fill(0);
      self.size = tx.size();
      if (tx.size() > self.command.size())
        return Error::malformed;
      std::copy(tx.begin(), tx.end(), self.command.begin());
      return Error::ok;
    }
  } spi;
  const auto p = board::robot_2026();
  EXPECT_EQ(p.stepper_current_tval, (std::array<std::uint8_t, 4>{34, 47, 34, 34}));
  EXPECT_EQ(p.stepper_ocd_threshold, (std::array<std::uint8_t, 4>{8, 8, 8, 8}));
  EXPECT_EQ(p.stepper_alarm_enable, (std::array<std::uint8_t, 4>{3, 0x87, 0x87, 0x87}));
  EXPECT_EQ(p.stepper_toff_min, (std::array<std::uint8_t, 4>{31, 255, 255, 255}));
  devices::L6474 driver{{&spi, Spi::transfer}};
  ASSERT_EQ(driver.set_parameter(0x18, p.stepper_driver_config[0]), Error::ok);
  EXPECT_EQ(spi.size, 3U);
  EXPECT_EQ(spi.command, (std::array<std::uint8_t, 4>{0x18, 0x7c, 0x03, 0}));
  for (std::size_t i = 1; i < 4; ++i) {
    ASSERT_EQ(driver.set_parameter(0x18, p.stepper_driver_config[i]), Error::ok);
    EXPECT_EQ(spi.command, (std::array<std::uint8_t, 4>{0x18, 0x28, 0x83, 0}));
  }
  ASSERT_EQ(driver.set_step_mode(p.stepper_mode[0]), Error::ok);
  EXPECT_EQ(spi.command[0], 0x16);
  EXPECT_EQ(spi.command[1], 0x88);
  ASSERT_EQ(driver.set_current_raw(p.stepper_current_tval[0]), Error::ok);
  EXPECT_EQ(spi.command[0], 0x09);
  EXPECT_EQ(spi.command[1], 0x22);
}

TEST(Robot2026, HomeAtSwitchEstablishesZeroInsteadOfOldSearchTarget) {
  struct Pulse {
    unsigned pulses{};
    static Error configure(void *, bool, std::uint32_t) { return Error::ok; }
    static Error pulse(void *context) {
      ++static_cast<Pulse *>(context)->pulses;
      return Error::ok;
    }
    static Error stop(void *) { return Error::ok; }
  } io;
  const auto p = board::robot_2026();
  EXPECT_EQ(p.home_input[0], 5);
  EXPECT_FALSE(p.home_active_high[0]);
  EXPECT_FALSE(p.home_forward[0]);
  EXPECT_EQ(p.steppers[0].minimum_steps, 0);
  EXPECT_EQ(p.steppers[0].maximum_steps, 6375);
  EXPECT_EQ(p.steppers[0].maximum_steps_per_second, 1920U);
  devices::StepperPlanner planner{p.steppers[0],
                                  {&io, Pulse::configure, Pulse::pulse, Pulse::stop}};
  ASSERT_EQ(planner.home_profile(p.home_forward[0], board::source_2026::home_profile, 10000000, 0),
            Error::ok);
  ASSERT_EQ(planner.tick(100, true), Error::ok);
  EXPECT_TRUE(planner.position_known());
  EXPECT_EQ(planner.commanded_steps(), 0);
  EXPECT_EQ(io.pulses, 0U);
  EXPECT_EQ(planner.move_profile(0, p.stepper_profiles[0], 4000000, 100), Error::ok);
  EXPECT_FALSE(planner.busy());
}

TEST(Robot2026, LiftPlanRetainsKnownSpeedAndFullWireDomainThroughFactoryComposition) {
  const auto p = board::robot_2026();
  for (const auto &config : p.grippers) {
    EXPECT_EQ(config.minimum_height_mm, 0);
    EXPECT_EQ(config.maximum_height_mm, 255);
    EXPECT_EQ(config.minimum_lift_steps, 0);
    EXPECT_EQ(config.maximum_lift_steps, 6375);
    EXPECT_EQ(config.lift_steps_per_mm, 25);
    EXPECT_EQ(config.lift_speed_steps_s, 1920);
    EXPECT_EQ(config.lift_up_current_ma, 1100);
    EXPECT_EQ(config.lift_down_current_ma, 320);
    EXPECT_TRUE(config.disable_lift_after_move);
  }
  PlanIo io;
  grip::Engine engine{p.grippers, io.sinks()};
  ASSERT_EQ(engine.start(77, {grip::Operation::lift_height, 0, 255, 0, 10000}, 0), Error::ok);
  const auto plan = engine.plan();
  ASSERT_GE(plan.size(), 4U);
  EXPECT_EQ(plan[2].action, grip::Action::stepper_move);
  EXPECT_EQ(plan[2].targets[0].position_steps, 6375);
  EXPECT_EQ(plan[2].targets[0].rate, 1920);
  EXPECT_EQ(plan[3].action, grip::Action::stepper_disable);
  ASSERT_EQ(engine.cancel(77), Error::ok);
  EXPECT_EQ(engine.start(77, {grip::Operation::lift_height, 0, 256, 0, 10000}, 0),
            Error::invalid_config);
  EXPECT_EQ(io.writes, 0U);
}

TEST(Robot2026, KnownServoAndTwelvePcaBindingsRetainTheirSourceCalibrations) {
  const auto p = board::robot_2026();
  unsigned ax_count = 0, pca_count = 0;
  for (std::size_t id = 0; id < p.ax_ids.size(); ++id) {
    ax_count += p.ax_ids[id];
    if (p.ax_ids[id]) {
      EXPECT_EQ(p.servo_min_deg[id], 0);
      EXPECT_EQ(p.servo_max_deg[id], 300);
      EXPECT_FALSE(p.mx_ids[id]);
    }
  }
  EXPECT_EQ(ax_count, 9U);
  for (const auto id : board::source_2026::known_ax_ids)
    EXPECT_TRUE(p.ax_ids[id]);
  for (std::size_t i = 0; i < p.pca_channels.size(); ++i) {
    pca_count += p.pca_channels[i];
    EXPECT_EQ(p.pca_channels[i], i >= 1 && i <= 12);
    if (p.pca_channels[i]) {
      EXPECT_EQ(p.pca_calibration[i].minimum_deg, 0);
      EXPECT_EQ(p.pca_calibration[i].maximum_deg, 180);
      EXPECT_EQ(p.pca_calibration[i].minimum_us, 500);
      EXPECT_EQ(p.pca_calibration[i].maximum_us, 2500);
    }
  }
  EXPECT_EQ(pca_count, 12U);
  EXPECT_EQ(p.grippers[0].cursor.id, 98);
  EXPECT_EQ(p.grippers[1].pinion.id, 13);
  EXPECT_EQ(p.grippers[1].main[0].id, 1);
  EXPECT_EQ(p.grippers[1].main[1].id, 2);
}

TEST(Robot2026, EightIndependentPumpRoutesEnforceFourConcurrentSoftwareLimitAtomically) {
  struct Outputs {
    std::uint32_t pumps{}, valves{};
    unsigned writes{};
    static Error write(void *context, std::uint32_t pumps, std::uint32_t valves) {
      auto &self = *static_cast<Outputs *>(context);
      self.pumps = pumps;
      self.valves = valves;
      ++self.writes;
      return Error::ok;
    }
  } io;
  const auto p = board::robot_2026();
  PumpBank bank{{p.channels, p.max_active, 250, p.max_pump_on_ms}, {&io, Outputs::write}};
  ASSERT_EQ(bank.initialize(0), Error::ok);
  ASSERT_EQ(bank.set(0xff, 0x0f, 0xff, 0x0f, 0), Error::ok);
  const auto before = io.writes;
  EXPECT_EQ(bank.set(0x10, 0x10, 0, 0, 1), Error::invalid_argument);
  EXPECT_EQ(io.writes, before);
  EXPECT_EQ(bank.state().requested, 0x0fU);
  ASSERT_EQ(bank.set(0xff, 0xf0, 0xff, 0xf0, 2), Error::ok);
  const auto word = p.output_word(io.pumps, io.valves);
  ASSERT_TRUE(word);
  EXPECT_EQ(*word, 0x00f0f0U); // PO9..16, separate from the first four pairs.
}

TEST(Robot2026, DerivedWheelAndEncoderBoundsCoverSelectedSourceBodyEnvelope) {
  const auto p = board::robot_2026();
  EXPECT_EQ(p.chassis.max_linear_mps, 1);
  EXPECT_EQ(p.chassis.max_angular_radps, 5);
  EXPECT_FLOAT_EQ(p.chassis.max_accel_mps2, 0.2F);
  EXPECT_EQ(p.chassis.max_angular_accel_radps2, 5);
  OmniKinematics kinematics{p.chassis.geometry};
  for (unsigned angle = 0; angle < 360; ++angle) {
    const auto radians = static_cast<float>(angle * std::numbers::pi / 180);
    for (const auto rotation : {-5.0F, 0.0F, 5.0F}) {
      const auto wheels = kinematics.wheel_tpm({std::cos(radians), std::sin(radians), rotation});
      ASSERT_TRUE(wheels);
      for (const auto value : *wheels) {
        EXPECT_LE(std::abs(static_cast<std::int64_t>(value)), p.chassis.max_wheel_tpm);
      }
    }
  }
  const auto expected =
      std::ceil(static_cast<double>(p.chassis.max_wheel_tpm) * p.chassis.max_tick_ms / 60000.0) + 1;
  EXPECT_EQ(p.chassis.max_encoder_delta, expected);
}

TEST(Robot2026, PrimaryIdRequiresUniqueExplicitUnicastAndNeverUsesBroadcast) {
  for (const auto id : {0U, 1U, 2U, 7U, 8U, 13U, 16U, 25U, 98U, 112U, 254U, 255U}) {
    const auto p = board::robot_2026(static_cast<std::uint8_t>(id));
    EXPECT_EQ(p.grippers[0].main[0].id, grip::unassigned);
    EXPECT_TRUE(p.valid());
  }
  const auto explicit_id = board::robot_2026(42);
  EXPECT_EQ(explicit_id.grippers[0].main[0].id, 42);
  EXPECT_TRUE(explicit_id.ax_ids[42]);
  EXPECT_EQ(explicit_id.servo_min_deg[42], 0);
  EXPECT_EQ(explicit_id.servo_max_deg[42], 300);
  EXPECT_TRUE(explicit_id.valid());
}

TEST(Robot2026, UnknownPrimaryRejectsBeforeIssueWhileKnownGripperOperationsStillExecute) {
  const auto p = board::robot_2026();
  PlanIo io;
  grip::Engine engine{p.grippers, io.sinks()};
  EXPECT_EQ(engine.start(77, {grip::Operation::rotate_main, 0, 1, 0, 10000}, 0),
            Error::invalid_config);
  EXPECT_EQ(io.writes, 0U);
  EXPECT_FALSE(engine.busy());
  ASSERT_EQ(engine.start(77, {grip::Operation::rotate_main, 1, 1, 0, 10000}, 0), Error::ok);
  ASSERT_EQ(engine.tick(0), Error::ok);
  EXPECT_EQ(io.last.targets[0].id, 1);
  EXPECT_EQ(io.last.targets[1].id, 2);
  ASSERT_EQ(engine.tick(200), Error::ok);
  EXPECT_EQ(engine.state().phase, grip::Phase::completed);
  ASSERT_EQ(engine.start(77, {grip::Operation::side_grips, 0, 0, 0, 10000}, 300), Error::ok);
  ASSERT_EQ(engine.tick(300), Error::ok);
  EXPECT_EQ(io.last.targets[0].id, 7);
  EXPECT_EQ(io.last.targets[1].id, 8);
  ASSERT_EQ(engine.tick(400), Error::ok);
  ASSERT_EQ(engine.start(77, {grip::Operation::pumps, 1, 0, 15, 10000}, 500), Error::ok);
  ASSERT_EQ(engine.tick(500), Error::ok);
  EXPECT_EQ(io.last.pump_select, 0xf0U);
  EXPECT_EQ(io.last.pump_values, 0xf0U);
  ASSERT_EQ(engine.tick(550), Error::ok);
  EXPECT_EQ(engine.state().phase, grip::Phase::completed);
  io.authority = Error::not_armed;
  EXPECT_EQ(engine.start(77, {grip::Operation::cursor, 0, 1, 0, 10000}, 600), Error::not_armed);
}
} // namespace
