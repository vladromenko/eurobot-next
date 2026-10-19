#include "eurobot/gripper.hpp"
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
using namespace eurobot;
namespace grip = eurobot::gripper;
namespace {
unsigned checks = 0;
#define CHECK(condition)                                                                           \
  do {                                                                                             \
    ++checks;                                                                                      \
    if (!(condition)) {                                                                            \
      std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);                         \
      std::abort();                                                                                \
    }                                                                                              \
  } while (false)
struct Fake {
  bool armed{true}, known{true}, done{true}, recurse_stop{};
  Error guard_error{Error::ok}, issue_error{Error::ok}, feedback_error{Error::ok},
      stop_error{Error::ok};
  grip::Evidence override_evidence{grip::Evidence::unknown};
  std::array<grip::Step, 64> issued{};
  unsigned issue_count{}, feedback_count{}, stop_count{}, validation_count{}, reject_step{1000};
  std::uint32_t pump_state{}, valve_state{};
  grip::Engine *engine{};
  static Error guard(void *context, std::uint32_t owner, std::uint32_t) {
    auto &self = *static_cast<Fake *>(context);
    if (self.guard_error != Error::ok) {
      return self.guard_error;
    }
    if (!self.armed) {
      return Error::not_armed;
    }
    return owner == 42 ? Error::ok : Error::lease_conflict;
  }
  static Error validate(void *context, const grip::Step &) {
    auto &self = *static_cast<Fake *>(context);
    return self.validation_count++ == self.reject_step ? Error::invalid_config : Error::ok;
  }
  static Error issue(void *context, const grip::Step &step, std::uint32_t) {
    auto &self = *static_cast<Fake *>(context);
    self.issued[self.issue_count++] = step;
    if (self.issue_error != Error::ok) {
      return self.issue_error;
    }
    if (step.action == grip::Action::outputs) {
      self.pump_state = (self.pump_state & ~step.pump_select) | step.pump_values;
      self.valve_state = (self.valve_state & ~step.valve_select) | step.valve_values;
    }
    return Error::ok;
  }
  static Error feedback(void *context, const grip::Step &step, grip::Completion &output,
                        std::uint32_t) {
    auto &self = *static_cast<Fake *>(context);
    ++self.feedback_count;
    output = {self.known, self.done,
              self.override_evidence == grip::Evidence::unknown ? step.required
                                                                : self.override_evidence};
    return self.feedback_error;
  }
  static Error stop(void *context, Error) {
    auto &self = *static_cast<Fake *>(context);
    ++self.stop_count;
    self.pump_state = 0;
    self.valve_state = 0;
    if (self.recurse_stop && self.engine) {
      CHECK(!self.engine->busy());
      CHECK(self.engine->abort(Error::estop) == Error::ok);
    }
    return self.stop_error;
  }
  grip::Sinks sinks() { return {this, guard, validate, issue, feedback, stop}; }
};
grip::Config configured() {
  auto config = grip::legacy_2026_reference(1);
  config.reviewed = true;
  config.pump_channels = {2, 7, 12, 31};
  config.valve_channels = {0, 3, 10, 19};
  config.lift_speed_steps_s = 1000;
  config.minimum_height_mm = 0;
  config.maximum_height_mm = 200;
  config.minimum_lift_steps = 0;
  config.maximum_lift_steps = 5000;
  config.composite = grip::Composite::base_strategy_2026;
  // Synthetic test-only movement parameters; not a deployment profile or
  // hardware recommendation.
  config.capture_forward_mps = 0.05F;
  config.capture_backward_mps = 0.04F;
  config.capture_forward_ms = 1000;
  config.capture_backward_ms = 900;
  config.capture_axis = 1;
  return config;
}
struct Fixture {
  std::array<grip::Config, 2> configs{configured(), configured()};
  Fake fake{};
  grip::Engine engine{configs, fake.sinks()};
  Fixture() { fake.engine = &engine; }
  Error start(grip::Operation operation, float value = 0, std::uint32_t mask = 0,
              std::uint32_t now = 0, std::uint32_t timeout = 10000, float argument = 0,
              float speed = 0) {
    return engine.start(42, {operation, 0, value, mask, timeout, argument, speed}, now);
  }
  void finish(std::uint32_t start = 0, std::uint32_t limit = 20000) {
    for (std::uint32_t now = start; now < start + limit && engine.busy(); now += 10) {
      CHECK(engine.tick(now) == Error::ok);
    }
    CHECK(engine.state().phase == grip::Phase::completed);
  }
};
void source_profiles_and_rejection() {
  auto primary = grip::legacy_2026_reference(0), reserve = grip::legacy_2026_reference(1);
  CHECK(!primary.reviewed && !reserve.reviewed);
  CHECK(primary.main[0].id == grip::unassigned);
  CHECK(primary.main[1].id == 112);
  CHECK(primary.side[0].id == 7 && primary.side[1].id == 8);
  CHECK(reserve.main[0].id == 1 && reserve.main[1].id == 2);
  CHECK(primary.block_channels == (std::array<std::uint8_t, 4>{1, 2, 3, 4}));
  CHECK(reserve.shift_channels == (std::array<std::uint8_t, 2>{11, 12}));
  CHECK(primary.lift_steps_per_mm == 25 && primary.lift_speed_steps_s == 0);
  CHECK(primary.lift_up_current_ma == 1100 && primary.lift_down_current_ma == 320);
  CHECK(primary.pinion_start_deg == 148 && primary.pinion_deg_per_mm == 3.979F);
  CHECK(primary.pump_channels[0] == grip::unassigned &&
        reserve.pump_channels[0] == grip::unassigned);
  CHECK(primary.drive_reference.wheel_goal_ticks == 28 * 4096);
  CHECK(primary.drive_reference.wheel_tpm == 400000);
  Fake fake;
  grip::Engine disabled{std::span<const grip::Config>{&reserve, 1}, fake.sinks()};
  CHECK(disabled.start(42, {grip::Operation::cursor, 0, 1, 0, 10000}, 0) == Error::invalid_config);
  primary.reviewed = true;
  grip::Engine unsafe_id{std::span<const grip::Config>{&primary, 1}, fake.sinks()};
  CHECK(unsafe_id.start(42, {grip::Operation::rotate_main, 0, 1, 0, 10000}, 0) ==
        Error::invalid_config);
  CHECK(fake.issue_count == 0);
  CHECK(unsafe_id.start(42, {grip::Operation::side_grips, 0, 1, 0, 10000}, 0) == Error::ok);
  CHECK(unsafe_id.plan()[0].targets[0].id == 7 && unsafe_id.plan()[0].targets[1].id == 8);
}
void paired_measured_completion() {
  Fixture fixture;
  CHECK(fixture.start(grip::Operation::rotate_main, 1) == Error::ok);
  CHECK(fixture.engine.plan()[0].count == 2);
  CHECK(fixture.engine.plan()[0].targets[0].value == 149 &&
        fixture.engine.plan()[0].targets[1].value == 149);
  fixture.fake.known = false;
  CHECK(fixture.engine.tick(0) == Error::ok);
  CHECK(fixture.fake.issue_count == 1);
  CHECK(fixture.engine.tick(200) == Error::ok);
  CHECK(fixture.engine.busy());
  fixture.fake.known = true;
  fixture.fake.done = false;
  CHECK(fixture.engine.tick(300) == Error::ok);
  CHECK(fixture.engine.busy());
  fixture.fake.done = true;
  fixture.fake.override_evidence = grip::Evidence::commanded;
  CHECK(fixture.engine.tick(400) == Error::ok);
  CHECK(fixture.engine.busy());
  fixture.fake.override_evidence = grip::Evidence::measured;
  CHECK(fixture.engine.tick(500) == Error::ok);
  CHECK(!fixture.engine.busy());
  CHECK(fixture.engine.state().completion == grip::Evidence::measured);
  CHECK(fixture.engine.state().measured_steps == 1);
  CHECK(fixture.engine.state().commanded_steps == 0);
  CHECK(fixture.fake.issue_count == 1);
  CHECK(fixture.start(grip::Operation::side_grips, 0) == Error::ok);
  CHECK(fixture.engine.plan()[0].targets[0].value == 123 &&
        fixture.engine.plan()[0].targets[1].value == 86);
}
void masks_are_atomic_and_do_not_fake_measurement() {
  Fixture fixture;
  CHECK(fixture.start(grip::Operation::pumps, 0, 5) == Error::ok);
  const auto expected_select = (1U << 2) | (1U << 7) | (1U << 12) | (1U << 31);
  CHECK(fixture.engine.plan()[0].pump_select == expected_select);
  CHECK(fixture.engine.plan()[0].pump_values == ((1U << 2) | (1U << 12)));
  CHECK(fixture.engine.tick(0) == Error::ok);
  CHECK(fixture.fake.issue_count == 1);
  CHECK(fixture.fake.pump_state == ((1U << 2) | (1U << 12)));
  CHECK(fixture.engine.tick(49) == Error::ok);
  CHECK(fixture.engine.busy());
  CHECK(fixture.engine.tick(50) == Error::ok);
  CHECK(!fixture.engine.busy());
  CHECK(fixture.engine.state().completion == grip::Evidence::commanded);
  CHECK(fixture.engine.state().measured_steps == 0 && fixture.fake.feedback_count == 0);
  CHECK(fixture.start(grip::Operation::pumps, 0, 16) == Error::invalid_argument);
  CHECK(fixture.fake.issue_count == 1);
  CHECK(fixture.start(grip::Operation::valves, 0, 0) == Error::ok);
  CHECK(fixture.engine.plan()[0].valve_select == ((1U << 0) | (1U << 3) | (1U << 10) | (1U << 19)));
  CHECK(fixture.engine.plan()[0].valve_values == 0);
}
void block_order_and_open_loop_evidence() {
  Fixture fixture;
  CHECK(fixture.start(grip::Operation::blocks_rotate, 0, 8) == Error::ok);
  const auto plan = fixture.engine.plan();
  CHECK(plan.size() == 7);
  CHECK(plan[0].action == grip::Action::pca_group && plan[0].count == 2 &&
        plan[0].settle_ms == 200);
  CHECK(plan[0].targets[0].value == 16 && plan[0].targets[1].value == 112);
  CHECK(plan[1].targets[0].id == 7 && plan[1].targets[0].value == 180);
  CHECK(plan[2].targets[0].id == 8 && plan[2].targets[0].value == 0);
  CHECK(plan[3].targets[0].id == 9 && plan[3].targets[0].value == 0);
  CHECK(plan[4].targets[0].id == 10 && plan[4].targets[0].value == 0);
  CHECK(plan[5].action == grip::Action::delay && plan[5].settle_ms == 500);
  CHECK(plan[6].targets[0].value == 103 && plan[6].targets[1].value == 18);
  fixture.finish();
  CHECK(fixture.engine.state().completion == grip::Evidence::settled);
  CHECK(fixture.engine.state().measured_steps == 0 && fixture.fake.feedback_count == 0);
  CHECK(fixture.fake.issue_count ==
        6); // Delay is internal and never sent as a fictitious device command.
}
void cancellation_failures_deadlines() {
  Fixture fixture;
  CHECK(fixture.start(grip::Operation::pumps, 0, 15) == Error::ok);
  CHECK(fixture.engine.tick(0) == Error::ok);
  CHECK(fixture.engine.cancel(43) == Error::lease_conflict);
  CHECK(fixture.fake.stop_count == 0);
  fixture.fake.recurse_stop = true;
  CHECK(fixture.engine.cancel(42) == Error::ok);
  CHECK(fixture.engine.state().phase == grip::Phase::cancelled);
  CHECK(fixture.fake.stop_count == 1 && fixture.fake.pump_state == 0);
  CHECK(fixture.start(grip::Operation::cursor, 1) == Error::ok);
  fixture.fake.known = false;
  CHECK(fixture.engine.tick(0) == Error::ok);
  CHECK(fixture.engine.tick(1500) == Error::timeout);
  CHECK(fixture.engine.state().phase == grip::Phase::faulted);
  CHECK(fixture.engine.state().completion == grip::Evidence::unknown);
  CHECK(fixture.start(grip::Operation::cursor, 1, 0, 2000) == Error::ok);
  fixture.fake.guard_error = Error::lease_expired;
  CHECK(fixture.engine.tick(2000) == Error::lease_expired);
  fixture.fake.guard_error = Error::ok;
  CHECK(fixture.start(grip::Operation::cursor, 1, 0, 3000) == Error::ok);
  fixture.fake.issue_error = Error::output_failure;
  CHECK(fixture.engine.tick(3000) == Error::output_failure);
  fixture.fake.issue_error = Error::ok;
  CHECK(fixture.start(grip::Operation::cursor, 1, 0, 4000) == Error::ok);
  fixture.fake.feedback_error = Error::crc;
  CHECK(fixture.engine.tick(4000) == Error::crc);
  fixture.fake.feedback_error = Error::ok;
  CHECK(fixture.start(grip::Operation::cursor, 1, 0, 5000) == Error::ok);
  fixture.fake.stop_error = Error::output_failure;
  CHECK(fixture.engine.cancel(42) == Error::output_failure);
  CHECK(fixture.engine.state().phase == grip::Phase::faulted &&
        fixture.engine.state().stop_error == Error::output_failure);
}
void preflight_is_transactional() {
  Fixture fixture;
  CHECK(fixture.start(grip::Operation::cursor, 1) == Error::ok);
  fixture.finish();
  const auto prior = fixture.engine.state();
  const auto prior_target = fixture.engine.plan()[0].targets[0];
  CHECK(fixture.start(grip::Operation::rotate_main, 10) == Error::invalid_argument);
  CHECK(fixture.engine.state().phase == prior.phase &&
        fixture.engine.state().owner == prior.owner &&
        fixture.engine.state().operation == prior.operation);
  CHECK(fixture.engine.plan()[0].targets[0].id == prior_target.id &&
        fixture.engine.plan()[0].targets[0].value == prior_target.value);
  const auto before = fixture.fake.issue_count;
  fixture.fake.reject_step = fixture.fake.validation_count + 6;
  CHECK(fixture.start(grip::Operation::blocks_rotate, 0, 15) == Error::invalid_config);
  CHECK(fixture.fake.issue_count == before);
  CHECK(fixture.engine.state().operation == prior.operation &&
        fixture.engine.state().phase == grip::Phase::completed);
  CHECK(fixture.engine.plan()[0].targets[0].id == prior_target.id &&
        fixture.engine.plan()[0].targets[0].value == prior_target.value);
  CHECK(fixture.start(grip::Operation::cursor, std::numeric_limits<float>::quiet_NaN()) ==
        Error::invalid_argument);
  CHECK(fixture.start(grip::Operation::cursor, 1, 0, 0, 99) == Error::invalid_argument);
  CHECK(fixture.start(grip::Operation::cursor, 1, 0, 0, 60001) == Error::invalid_argument);
}
void lift_and_slide() {
  Fixture fixture;
  CHECK(fixture.start(grip::Operation::lift_height, 125) == Error::ok);
  auto plan = fixture.engine.plan();
  CHECK(plan.size() == 5);
  CHECK(plan[0].action == grip::Action::stepper_current && plan[0].targets[0].value == 1100);
  CHECK(plan[0].alternate_current_ma == 320 && plan[0].targets[0].position_steps == 3125);
  CHECK(plan[0].enable_before_settle);
  CHECK(plan[1].action == grip::Action::delay && plan[1].settle_ms == 10);
  CHECK(plan[2].action == grip::Action::stepper_move &&
        plan[2].required == grip::Evidence::referenced_steps);
  CHECK(plan[2].targets[0].position_steps == 3125 && plan[2].targets[0].rate == 1000);
  CHECK(plan[3].alternate_current_ma == 0);
  CHECK(!plan[3].enable_before_settle);
  fixture.finish();
  CHECK(fixture.engine.state().referenced_steps == 1 && fixture.engine.state().measured_steps == 0);
  CHECK(fixture.start(grip::Operation::lift_height, 201) == Error::invalid_config);
  CHECK(fixture.start(grip::Operation::slide, 10) == Error::ok);
  CHECK(std::fabs(fixture.engine.plan()[0].targets[0].value - 187.79F) < 0.001F);
  CHECK(fixture.engine.plan()[0].targets[0].id == 13);
}
void motion_plans_and_source_composites() {
  Fixture fixture;
  CHECK(fixture.start(grip::Operation::timed_move, -0.1F, 1, 0, 5000, 1000) == Error::ok);
  CHECK(fixture.engine.plan()[0].action == grip::Action::timed_drive);
  CHECK(fixture.engine.plan()[0].drive.velocity.vy == -0.1F &&
        fixture.engine.plan()[0].drive.duration_ms == 1000);
  CHECK(fixture.engine.plan()[0].required == grip::Evidence::settled);
  CHECK(fixture.engine.cancel(42) == Error::ok);
  CHECK(fixture.start(grip::Operation::relative_move, 0.1F, 0, 0, 10000, -0.2F, 0.1F) == Error::ok);
  auto plan = fixture.engine.plan();
  CHECK(plan[0].action == grip::Action::relative_drive && plan[0].drive.axis == 1 &&
        plan[0].drive.displacement_m == -0.2F);
  CHECK(plan[3].action == grip::Action::relative_drive && plan[3].drive.axis == 0 &&
        plan[3].drive.displacement_m == 0.1F);
  CHECK(plan[0].drive.tolerance_m == 0.003F && plan[0].required == grip::Evidence::measured);
  CHECK(fixture.engine.cancel(42) == Error::ok);
  CHECK(fixture.start(grip::Operation::wheel_calibration, 0, 2, 0, 60000) == Error::ok);
  CHECK(fixture.engine.plan()[2].action == grip::Action::wheel_calibration);
  CHECK(fixture.engine.plan()[2].drive.axis == 2 &&
        fixture.engine.plan()[2].drive.wheel_goal_ticks == 114688);
  CHECK(fixture.engine.cancel(42) == Error::ok);
  CHECK(fixture.start(grip::Operation::capture, 0, 0, 0, 60000) == Error::ok);
  CHECK(fixture.engine.state().count <= 64);
  plan = fixture.engine.plan();
  CHECK(plan[0].action == grip::Action::servo_group && plan[0].targets[0].value == 123); // SideOff.
  CHECK(plan[1].targets[0].value == 149); // Main down.
  CHECK(plan[4].action == grip::Action::outputs && plan[4].valve_values != 0);
  CHECK(plan[5].action == grip::Action::outputs && plan[5].pump_values != 0);
  CHECK(plan[6].action == grip::Action::timed_drive && plan[6].settle_ms == 2500);
  CHECK(plan[8].targets[0].value == 65); // Main up.
  CHECK(plan[9].drive.velocity.vy < 0);
  CHECK(fixture.engine.cancel(42) == Error::ok);
  CHECK(fixture.start(grip::Operation::release, 0, 0, 0, 60000) == Error::ok);
  plan = fixture.engine.plan();
  CHECK(plan[1].action == grip::Action::outputs && plan[1].pump_values == 0);
  CHECK(plan[2].valve_values == 0 && plan[3].valve_values != 0);
  CHECK(plan.back().action == grip::Action::delay && plan.back().settle_ms == 4500);
}
void clock_wrap_and_unknown_feedback_timeout() {
  Fixture fixture;
  CHECK(fixture.start(grip::Operation::cursor, 1, 0, 0xfffffff0U, 1000) == Error::ok);
  fixture.fake.known = false;
  CHECK(fixture.engine.tick(0xfffffff0U) == Error::ok);
  CHECK(fixture.engine.tick(0x100U) == Error::ok);
  CHECK(fixture.engine.busy());
  CHECK(fixture.engine.tick(0x3d8U) == Error::timeout);
  CHECK(!fixture.engine.busy());
  CHECK(fixture.engine.state().completion == grip::Evidence::unknown);
}
} // namespace
int main() {
  source_profiles_and_rejection();
  paired_measured_completion();
  masks_are_atomic_and_do_not_fake_measurement();
  block_order_and_open_loop_evidence();
  cancellation_failures_deadlines();
  preflight_is_transactional();
  lift_and_slide();
  motion_plans_and_source_composites();
  clock_wrap_and_unknown_feedback_timeout();
  std::printf("gripper: %u checks passed\n", checks);
}
