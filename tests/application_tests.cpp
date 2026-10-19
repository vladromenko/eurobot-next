#include "eurobot/application.hpp"
#include <array>
#include <cstdio>
#include <cstdlib>
#include <limits>

namespace app = eurobot::application;
using eurobot::Error;
namespace {
unsigned checks{};
void check(bool condition, const char *expression, int line) {
  ++checks;
  if (!condition) {
    std::fprintf(stderr, "Application check failed line%d: %s\n", line, expression);
    std::exit(1);
  }
}
#define CHECK(value) check((value), #value, __LINE__)
struct Rig {
  Error authority{Error::ok}, stopping{Error::ok}, action_result{Error::ok};
  unsigned stops{}, actions{}, guards{}, motor_enables{};
  app::StopReason reason{};
  app::Application application;
  explicit Rig(app::Config config = {}) : application(config, {this, guard, stop, action}) {}
  static Error guard(void *pointer, std::uint32_t) {
    auto &self = *static_cast<Rig *>(pointer);
    ++self.guards;
    return self.authority;
  }
  static Error stop(void *pointer, app::StopReason reason) {
    auto &self = *static_cast<Rig *>(pointer);
    ++self.stops;
    self.reason = reason;
    return self.stopping;
  }
  static Error action(void *pointer, app::LocalAction action, std::uint32_t) {
    auto &self = *static_cast<Rig *>(pointer);
    CHECK(action == app::LocalAction::open_reserve_side ||
          action == app::LocalAction::enable_motors);
    self.motor_enables += action == app::LocalAction::enable_motors;
    ++self.actions;
    return self.action_result;
  }
};
void samples(Rig &rig, std::uint16_t raw, std::uint32_t &now, unsigned duration) {
  for (unsigned i = 0; i < duration; ++i) {
    ++now;
    CHECK(rig.application.sample_inputs(raw, now) == Error::ok);
  }
}
void drain(Rig &rig) {
  app::Event event{};
  while (rig.application.pop_event(event)) {
  }
}
void initial(Rig &rig, std::uint16_t raw, std::uint32_t &now) {
  CHECK(rig.application.initialize(now) == Error::ok);
  CHECK(rig.application.sample_inputs(raw, now) == Error::ok);
  samples(rig, raw, now, 8);
}
void config_and_boot() {
  Rig rig;
  std::uint32_t now{};
  CHECK(rig.application.tick(now) == Error::invalid_config);
  CHECK(rig.application.arm_match(now) == Error::invalid_config);
  initial(rig, 0x1fff, now);
  CHECK(rig.application.valid());
  CHECK(rig.stops == 0 && rig.actions == 0 && rig.guards == 0);
  CHECK(rig.application.state().phase == app::Phase::disarmed);
  CHECK(rig.application.state().input_known == 0x1fff);
  CHECK(rig.application.state().input_pressed == 0x1fff);
  CHECK(rig.application.state().switch_known == 0x1ff);
  CHECK(rig.application.state().switches_pressed == 0);
  CHECK(rig.application.state().cord_attached && rig.application.state().side_right);
  CHECK(rig.application.state().motor_enable_requested);
  CHECK(rig.application.state().reset_latched);
  unsigned initial_events{};
  app::Event event{};
  while (rig.application.pop_event(event)) {
    CHECK(event.callback == app::Callback::init_complete);
    CHECK(event.action == app::ButtonAction::none);
    ++initial_events;
  }
  CHECK(initial_events == 13);
  samples(rig, 0, now, 10);
  CHECK(rig.application.state().input_known == 0x1fff);
  CHECK(rig.application.state().switches_pressed == 0x1ff);
  CHECK(!rig.application.state().side_right && !rig.application.state().cord_attached);
  CHECK(!rig.application.state().motor_enable_requested);
  CHECK(rig.stops == 0 && rig.actions == 0);
  app::Config invalid{};
  invalid.strategy_count = 0;
  Rig bad(invalid);
  CHECK(!bad.application.valid());
  CHECK(bad.application.initialize(now) == Error::invalid_config);
  invalid = {};
  invalid.match_duration_ms = 0x80000000U;
  Rig bad_interval(invalid);
  CHECK(!bad_interval.application.valid());
}
void debounce_click_hold_strategy() {
  Rig rig;
  std::uint32_t now{};
  constexpr std::uint16_t idle = 0x1ffb; // strategy starts RELEASED, other inputs HIGH.
  initial(rig, idle, now);
  drain(rig);
  // Short bounce must not change stable state or create strategy action.
  samples(rig, 0x1fff, now, 3);
  samples(rig, idle, now, 8);
  CHECK(rig.application.state().strategy == 0);
  app::Event event{};
  CHECK(!rig.application.pop_event(event));
  for (std::uint8_t expected : {1U, 2U, 0U}) {
    samples(rig, 0x1fff, now, 20);
    samples(rig, idle, now, 10);
    CHECK(rig.application.state().strategy == expected);
  }
  drain(rig);
  samples(rig, 0x1fff, now, 520);
  unsigned holds{}, clicks{};
  while (rig.application.pop_event(event)) {
    holds += event.button == app::Button::strategy && event.action == app::ButtonAction::hold;
    clicks += event.button == app::Button::strategy && event.action == app::ButtonAction::click;
  }
  CHECK(holds == 1 && clicks == 0);
  samples(rig, 0x1fff, now, 20);
  samples(rig, idle, now, 10);
  while (rig.application.pop_event(event)) {
    clicks += event.button == app::Button::strategy && event.action == app::ButtonAction::click;
  }
  CHECK(clicks == 0 && rig.application.state().strategy == 0);
  // Initially held input never generates a phantom hold or click.
  Rig held;
  now = 0;
  initial(held, 0x1fff, now);
  drain(held);
  samples(held, 0x1fff, now, 550);
  samples(held, idle, now, 10);
  while (held.application.pop_event(event)) {
    CHECK(event.action == app::ButtonAction::none);
  }
  CHECK(held.application.state().strategy == 0);
}
void match_start_stop_and_busy() {
  Rig rig;
  std::uint32_t now{};
  initial(rig, 0x1fff, now);
  rig.authority = Error::not_armed;
  CHECK(rig.application.arm_match(now) == Error::not_armed);
  CHECK(rig.application.state().phase == app::Phase::disarmed);
  rig.authority = Error::ok;
  CHECK(rig.application.arm_match(now) == Error::ok);
  CHECK(rig.application.arm_match(now) == Error::lease_conflict);
  CHECK(rig.application.state().phase == app::Phase::waiting_for_release);
  CHECK(rig.actions == 0);
  samples(rig, 0x1ffe, now, 10);
  CHECK(rig.application.state().phase == app::Phase::running);
  CHECK(rig.actions == 1);
  samples(rig, 0x1ffe, now, 20);
  CHECK(rig.actions == 1);
  CHECK(rig.application.set_mode(app::Mode::debug) == Error::lease_conflict);
  samples(rig, 0x1fff, now, 10);
  CHECK(rig.application.state().phase == app::Phase::finished);
  CHECK(rig.reason == app::StopReason::cord_reinserted && rig.stops == 1);
  samples(rig, 0x1ffe, now, 10);
  CHECK(rig.actions == 1); // No automatic restart after reinsertion.
  CHECK(rig.application.arm_match(now) == Error::not_armed);
  // Existing busy gripper makes the optional source startup action explicitly skipped.
  Rig busy;
  now = 0;
  initial(busy, 0x1fff, now);
  busy.action_result = Error::lease_conflict;
  CHECK(busy.application.arm_match(now) == Error::ok);
  samples(busy, 0x1ffe, now, 10);
  CHECK(busy.application.state().phase == app::Phase::running);
  CHECK(busy.application.state().start_action_skipped && busy.stops == 0);
}
void match_deadline_and_wrap() {
  for (auto start : {0U, 0xfffffff0U}) {
    app::Config config{};
    config.match_duration_ms = 99;
    Rig rig(config);
    auto now = start;
    initial(rig, 0x1fff, now);
    CHECK(rig.application.arm_match(now) == Error::ok);
    samples(rig, 0x1ffe, now, 10);
    // Stable release was accepted1ms before this sample loop ended.
    CHECK(rig.application.state().phase == app::Phase::running);
    for (unsigned i = 0; i < 97; ++i) {
      ++now;
      CHECK(rig.application.sample_inputs(0x1ffe, now) == Error::ok);
      CHECK(rig.application.state().phase == app::Phase::running);
    }
    ++now;
    CHECK(rig.application.sample_inputs(0x1ffe, now) == Error::ok);
    CHECK(rig.application.state().phase == app::Phase::finished);
    CHECK(rig.application.state().match_elapsed_ms == 99);
    CHECK(!rig.application.state().communication_enabled);
    CHECK(rig.reason == app::StopReason::match_timeout && rig.stops == 1);
    CHECK(rig.application.arm_match(now) == Error::not_armed);
  }
  // Actual2026default is99000ms, not100000 or99ticks.
  Rig normal;
  std::uint32_t now{};
  initial(normal, 0x1fff, now);
  CHECK(normal.application.arm_match(now) == Error::ok);
  samples(normal, 0x1ffe, now, 10);
  CHECK(normal.application.tick(now + 98999) == Error::ok);
  CHECK(normal.application.state().phase == app::Phase::finished);
  CHECK(normal.reason == app::StopReason::match_timeout);
}
void safety_and_repoll() {
  Rig rig;
  std::uint32_t now{};
  initial(rig, 0x1fff, now);
  CHECK(rig.application.arm_match(now) == Error::ok);
  samples(rig, 0x1ffe, now, 10);
  rig.authority = Error::lease_expired;
  CHECK(rig.application.tick(now) == Error::lease_expired);
  CHECK(rig.reason == app::StopReason::authority_lost && rig.stops == 1);
  CHECK(rig.application.state().phase == app::Phase::faulted);
  CHECK(!rig.application.state().communication_enabled);
  CHECK(rig.application.tick(now + 1) == Error::ok && rig.stops == 1);
  Rig stale;
  now = 0;
  initial(stale, 0x1fff, now);
  CHECK(stale.application.arm_match(now) == Error::ok);
  CHECK(stale.application.tick(now + 21) == Error::timeout);
  CHECK(stale.application.state().input_known == 0);
  CHECK(stale.reason == app::StopReason::inputs_unknown && stale.stops == 1);
  now += 21;
  CHECK(stale.application.sample_inputs(0x1fff, now) == Error::timeout);
  samples(stale, 0x1fff, now, 8);
  CHECK(stale.application.state().input_known == 0x1fff);
  CHECK(stale.application.state().phase == app::Phase::faulted);
  CHECK(stale.stops == 1); // Repoll does not resume a match.
  Rig disabled;
  now = 0;
  initial(disabled, 0x1fff, now);
  CHECK(disabled.application.arm_match(now) == Error::ok);
  samples(disabled, 0x1ff7, now, 10);
  CHECK(disabled.reason == app::StopReason::motor_disabled && disabled.stops == 1);
  CHECK(disabled.application.state().phase == app::Phase::finished);
  Rig failed;
  now = 0;
  initial(failed, 0x1fff, now);
  CHECK(failed.application.arm_match(now) == Error::ok);
  failed.action_result = Error::output_failure;
  samples(failed, 0x1ffe, now, 8);
  CHECK(failed.application.sample_inputs(0x1ffe, ++now) == Error::output_failure);
  CHECK(failed.application.state().phase == app::Phase::faulted && failed.stops == 1);
}
void event_overflow_does_not_disable_safety() {
  app::Config config{};
  config.match_duration_ms = 100;
  Rig rig(config);
  std::uint32_t now{};
  initial(rig, 0x1fff, now);
  for (unsigned i = 0; i < 20; ++i) {
    samples(rig, 0x1ffb, now, 10);
    samples(rig, 0x1fff, now, 10);
  }
  CHECK(rig.application.state().dropped_events > 0);
  CHECK(rig.application.arm_match(now) == Error::ok);
  samples(rig, 0x1ffe, now, 10);
  CHECK(rig.application.tick(now + 100) == Error::ok);
  CHECK(rig.reason == app::StopReason::match_timeout && rig.stops == 1);
  unsigned count{};
  app::Event event{};
  while (rig.application.pop_event(event)) {
    ++count;
  }
  CHECK(count == app::Application::event_capacity);
}
void debug_mode_and_released_boot() {
  Rig released;
  std::uint32_t now{};
  initial(released, 0x1ffe, now);
  CHECK(released.actions == 0 && released.stops == 0);
  CHECK(released.application.arm_match(now) == Error::not_armed);
  samples(released, 0x1fff, now, 10);
  CHECK(released.application.arm_match(now) == Error::ok);
  const auto guards = released.guards;
  samples(released, 0x1fff, now, 10);
  CHECK(released.guards > guards);
  CHECK(released.application.disarm_match(now) == Error::ok);
  app::Config config{};
  config.match_duration_ms = 30;
  config.mode = app::Mode::debug;
  config.open_reserve_side_at_start = false;
  Rig debug(config);
  now = 0;
  initial(debug, 0x1fff, now);
  CHECK(debug.application.arm_match(now) == Error::ok);
  samples(debug, 0x1ffe, now, 80);
  CHECK(debug.application.state().phase == app::Phase::running);
  CHECK(debug.actions == 0 && debug.stops == 0);
  CHECK(debug.application.state().match_elapsed_ms > config.match_duration_ms);
  CHECK(debug.application.state().communication_enabled);
  CHECK(debug.application.disarm_match(now) == Error::ok);
  CHECK(debug.application.state().phase == app::Phase::disarmed);
}
void explicit_2025_motor_button_policy() {
  app::Config config{};
  config.legacy_2025_motor_button_control = true;
  Rig rig(config);
  std::uint32_t now{};
  initial(rig, 0x1fff, now);
  samples(rig, 0x1fff, now, 20);
  CHECK(rig.motor_enables == 0 && rig.actions == 0); // Initial high is never boot-enable.
  samples(rig, 0x1ff7, now, 10);
  CHECK(rig.reason == app::StopReason::motor_disabled && rig.stops == 1);
  rig.authority = Error::not_armed;
  samples(rig, 0x1fff, now, 10);
  CHECK(rig.motor_enables == 0);
  rig.authority = Error::ok;
  samples(rig, 0x1fff, now, 20);
  CHECK(rig.motor_enables == 0); // No latent physical command when authorization later appears.
  samples(rig, 0x1ff7, now, 10);
  samples(rig, 0x1fff, now, 10);
  CHECK(rig.motor_enables == 1);
  samples(rig, 0x1fff, now, 10);
  CHECK(rig.motor_enables == 1);
}
void reset_communication_mode_and_failures() {
  Rig rig;
  std::uint32_t now{};
  initial(rig, 0x1fff, now);
  rig.application.clear_reset();
  CHECK(!rig.application.state().reset_latched);
  CHECK(!rig.application.take_reset_request());
  CHECK(rig.application.request_reset(now) == Error::ok);
  CHECK(rig.application.state().reset_latched && rig.application.state().reset_requested);
  CHECK(rig.reason == app::StopReason::board_reset && rig.stops == 1);
  CHECK(rig.application.take_reset_request());
  CHECK(!rig.application.take_reset_request());
  CHECK(rig.application.set_communication(false, now) == Error::ok);
  CHECK(rig.reason == app::StopReason::communication_disabled && rig.stops == 2);
  CHECK(rig.application.arm_match(now) == Error::not_armed);
  CHECK(rig.application.set_communication(true, now) == Error::ok);
  CHECK(rig.stops == 2 && rig.actions == 0);
  CHECK(rig.application.set_mode(app::Mode::debug) == Error::ok);
  CHECK(rig.application.arm_match(now) == Error::ok);
  CHECK(rig.application.disarm_match(now) == Error::ok);
  rig.stopping = Error::output_failure;
  CHECK(rig.application.request_reset(now) == Error::output_failure);
  CHECK(!rig.application.take_reset_request());
  CHECK(rig.application.state().phase == app::Phase::faulted);
  CHECK(rig.application.state().stop_error == Error::output_failure);
  CHECK(!rig.application.state().communication_enabled);
  CHECK(rig.application.sample_inputs(0x8000, now) == Error::invalid_argument);
  CHECK(rig.application.state().input_known == 0);
}
} // namespace
int main() {
  config_and_boot();
  debounce_click_hold_strategy();
  match_start_stop_and_busy();
  match_deadline_and_wrap();
  safety_and_repoll();
  event_overflow_does_not_disable_safety();
  debug_mode_and_released_boot();
  explicit_2025_motor_button_policy();
  reset_communication_mode_and_failures();
  std::printf("Application: %u checks PASS\n", checks);
}
