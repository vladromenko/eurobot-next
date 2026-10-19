#include "eurobot/application.hpp"
#include <limits>

namespace eurobot::application {
namespace {
constexpr std::uint16_t all_inputs = 0x1fffU;
constexpr std::uint16_t critical_inputs = 0x0009U;
constexpr std::uint16_t bit(std::size_t index) noexcept {
  return static_cast<std::uint16_t>(1U << index);
}
ButtonState button_state(std::uint16_t known, std::uint16_t pressed, std::size_t index) noexcept {
  return (known & bit(index)) == 0
             ? ButtonState::unknown
             : ((pressed & bit(index)) != 0 ? ButtonState::pressed : ButtonState::released);
}
} // namespace
Application::Application(Config config, Sinks sinks) noexcept
    : config_(config), sinks_(sinks), inputs_(config.inputs) {}
bool Application::valid() const noexcept {
  return config_.strategy_count > 0 && config_.match_duration_ms > 0 &&
         config_.match_duration_ms < 0x80000000U &&
         (config_.mode == Mode::game || config_.mode == Mode::debug) && inputs_.valid() &&
         sinks_.guard && sinks_.stop &&
         (!(config_.open_reserve_side_at_start || config_.legacy_2025_motor_button_control) ||
          sinks_.action);
}
Error Application::initialize(std::uint32_t now) noexcept {
  (void)now;
  initialized_ = false;
  state_ = {};
  state_.mode = config_.mode;
  head_ = size_ = 0;
  inputs_.invalidate();
  if (!valid()) {
    state_.error = Error::invalid_config;
    return state_.error;
  }
  initialized_ = true;
  return Error::ok;
}
void Application::publish(Event event) noexcept {
  if (size_ == events_.size()) {
    if (state_.dropped_events != std::numeric_limits<std::uint32_t>::max()) {
      ++state_.dropped_events;
    }
  } else {
    events_[(head_ + size_) % events_.size()] = event;
    ++size_;
  }
}
bool Application::pop_event(Event &event) noexcept {
  const bool available = size_ != 0;
  if (available) {
    event = events_[head_];
    head_ = (head_ + 1) % events_.size();
    --size_;
  }
  return available;
}
void Application::update_state(inputs::BankState snapshot) noexcept {
  state_.input_known = static_cast<std::uint16_t>(snapshot.known & all_inputs);
  state_.input_pressed = static_cast<std::uint16_t>(snapshot.pressed & snapshot.known & all_inputs);
  state_.cord_attached = (state_.input_pressed & bit(0)) != 0;
  state_.side_right = (state_.input_pressed & bit(1)) != 0;
  state_.motor_enable_requested = (state_.input_pressed & bit(3)) != 0;
  state_.switch_known = static_cast<std::uint16_t>(state_.input_known >> 4);
  // Legacy ButtonsCallback maps electrical high/PRESSED to mechanical RELEASED.
  state_.switches_pressed = static_cast<std::uint16_t>(
      (~static_cast<std::uint32_t>(state_.input_pressed) >> 4) & state_.switch_known & 0x1ffU);
}
bool Application::match_active() const noexcept {
  return state_.phase == Phase::waiting_for_release || state_.phase == Phase::running;
}
Error Application::stop(StopReason reason, Phase phase) noexcept {
  state_.stop_reason = reason;
  state_.phase = phase; // Publish before callback, which may revoke controller authority.
  state_.stop_error = sinks_.stop ? sinks_.stop(sinks_.context, reason) : Error::invalid_config;
  if (state_.stop_error != Error::ok) {
    state_.phase = Phase::faulted;
    state_.communication_enabled = false;
    state_.error = state_.stop_error;
  }
  return state_.stop_error;
}
Error Application::sample_inputs(std::uint16_t electrical_high, std::uint32_t now) noexcept {
  if (!initialized_) {
    return Error::invalid_config;
  }
  if ((electrical_high & ~all_inputs) != 0) {
    inputs_.invalidate();
    update_state(inputs_.snapshot(now));
    state_.error = Error::invalid_argument;
    if (match_active()) {
      (void)stop(StopReason::inputs_unknown, Phase::faulted);
    }
    return Error::invalid_argument;
  }
  const auto previous_known = state_.input_known;
  const auto previous_pressed = state_.input_pressed;
  const auto snapshot = inputs_.sample(electrical_high, now);
  update_state(snapshot);
  for (std::size_t index = 0; index < 13; ++index) {
    const auto mask = bit(index);
    const auto prior = button_state(previous_known, previous_pressed, index);
    const auto current = button_state(state_.input_known, state_.input_pressed, index);
    if (current != ButtonState::unknown && prior == ButtonState::unknown) {
      publish(
          {static_cast<Button>(index), Callback::init_complete, ButtonAction::none, current, now});
    } else if (current != prior) {
      publish(
          {static_cast<Button>(index), Callback::state_changed, ButtonAction::none, current, now});
    }
    if ((snapshot.clicked_edges & mask) != 0) {
      publish({static_cast<Button>(index), Callback::action, ButtonAction::click, current, now});
      if (index == static_cast<std::size_t>(Button::strategy)) {
        state_.strategy = static_cast<std::uint8_t>((static_cast<unsigned>(state_.strategy) + 1U) %
                                                    config_.strategy_count);
      }
    }
    if ((snapshot.held_edges & mask) != 0) {
      publish({static_cast<Button>(index), Callback::action, ButtonAction::hold, current, now});
    }
  }
  if (config_.legacy_2025_motor_button_control && (previous_known & bit(3)) != 0 &&
      (state_.input_known & bit(3)) != 0 &&
      ((state_.input_pressed ^ previous_pressed) & bit(3)) != 0) {
    if (state_.motor_enable_requested) {
      // A physical edge is not a deferred arm command. If authority is absent now,
      // no enable is queued or replayed after the operator obtains a lease.
      if (sinks_.guard(sinks_.context, now) == Error::ok) {
        const auto enabled = sinks_.action(sinks_.context, LocalAction::enable_motors, now);
        if (enabled != Error::ok) {
          state_.error = enabled;
          const auto stopped = stop(StopReason::action_failed, Phase::faulted);
          return stopped == Error::ok ? enabled : stopped;
        }
      }
    } else if (!match_active()) {
      const auto stopped = stop(StopReason::motor_disabled, Phase::disarmed);
      if (stopped != Error::ok) {
        return stopped;
      }
    }
  }
  const auto safety = tick(now);
  if (safety != Error::ok) {
    return safety;
  }
  if (snapshot.error != Error::ok && state_.phase != Phase::faulted) {
    state_.error = snapshot.error;
  }
  return snapshot.error;
}
Error Application::tick(std::uint32_t now) noexcept {
  if (!initialized_) {
    return Error::invalid_config;
  }
  update_state(inputs_.snapshot(now));
  if (state_.phase == Phase::running) {
    state_.match_elapsed_ms = now - match_started_;
  }
  if (!match_active()) {
    return Error::ok;
  }
  if (state_.phase == Phase::running && state_.mode == Mode::game &&
      elapsed(now, match_started_, config_.match_duration_ms)) {
    state_.communication_enabled = false;
    return stop(StopReason::match_timeout, Phase::finished);
  }
  if ((state_.input_known & critical_inputs) != critical_inputs) {
    state_.error = Error::timeout;
    const auto stopped = stop(StopReason::inputs_unknown, Phase::faulted);
    return stopped == Error::ok ? Error::timeout : stopped;
  }
  if (!state_.motor_enable_requested) {
    return stop(StopReason::motor_disabled, Phase::finished);
  }
  if (!state_.communication_enabled) {
    return stop(StopReason::communication_disabled, Phase::finished);
  }
  const auto guarded = sinks_.guard(sinks_.context, now);
  if (guarded != Error::ok) {
    state_.error = guarded;
    state_.communication_enabled = false;
    const auto stopped = stop(StopReason::authority_lost, Phase::faulted);
    return stopped == Error::ok ? guarded : stopped;
  }
  if (state_.phase == Phase::waiting_for_release && !state_.cord_attached) {
    state_.phase = Phase::running;
    match_started_ = now;
    state_.match_elapsed_ms = 0;
    state_.start_action_skipped = false;
    if (config_.open_reserve_side_at_start) {
      const auto acted = sinks_.action(sinks_.context, LocalAction::open_reserve_side, now);
      if (acted == Error::lease_conflict) {
        // Legacy game_task only dispatched the start side-grip command when its task was idle.
        state_.start_action_skipped = true;
      } else if (acted != Error::ok) {
        state_.error = acted;
        const auto stopped = stop(StopReason::action_failed, Phase::faulted);
        return stopped == Error::ok ? acted : stopped;
      }
    }
  } else if (state_.phase == Phase::running && state_.cord_attached) {
    return stop(StopReason::cord_reinserted, Phase::finished);
  }
  return Error::ok;
}
Error Application::arm_match(std::uint32_t now) noexcept {
  if (!initialized_) {
    return Error::invalid_config;
  }
  if (match_active()) {
    return Error::lease_conflict;
  }
  update_state(inputs_.snapshot(now));
  if (!state_.communication_enabled || (state_.input_known & critical_inputs) != critical_inputs ||
      !state_.cord_attached || !state_.motor_enable_requested) {
    return Error::not_armed;
  }
  const auto guarded = sinks_.guard(sinks_.context, now);
  if (guarded != Error::ok) {
    return guarded;
  }
  state_.phase = Phase::waiting_for_release;
  state_.match_elapsed_ms = 0;
  state_.error = state_.stop_error = Error::ok;
  state_.start_action_skipped = false;
  return Error::ok;
}
Error Application::disarm_match(std::uint32_t now) noexcept {
  (void)now;
  if (!initialized_) {
    return Error::invalid_config;
  }
  return stop(StopReason::operator_request, Phase::disarmed);
}
Error Application::set_communication(bool enabled, std::uint32_t now) noexcept {
  (void)now;
  if (!initialized_) {
    return Error::invalid_config;
  }
  state_.communication_enabled = enabled;
  return enabled ? Error::ok : stop(StopReason::communication_disabled, Phase::disarmed);
}
Error Application::set_mode(Mode mode) noexcept {
  if (!initialized_ || (mode != Mode::game && mode != Mode::debug)) {
    return Error::invalid_argument;
  }
  if (match_active()) {
    return Error::lease_conflict;
  }
  state_.mode = mode;
  return Error::ok;
}
Error Application::request_reset(std::uint32_t now) noexcept {
  (void)now;
  if (!initialized_) {
    return Error::invalid_config;
  }
  const auto stopped = stop(StopReason::board_reset, Phase::disarmed);
  if (stopped == Error::ok) {
    state_.reset_requested = true;
    state_.reset_latched = true;
  }
  return stopped;
}
bool Application::take_reset_request() noexcept {
  const bool requested = state_.reset_requested;
  state_.reset_requested = false;
  return requested;
}
} // namespace eurobot::application
