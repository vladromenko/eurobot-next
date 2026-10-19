#pragma once
#include "eurobot/core.hpp"
#include "eurobot/inputs.hpp"
#include <array>
#include <cstddef>
#include <cstdint>

namespace eurobot::application {
// Canonical2026 button order, OS_Tasks/buttons_poll_task.c:62-76.
enum class Button : std::uint8_t {
  starting_cord,
  playground_side,
  strategy,
  motor_enable,
  switch1,
  switch2,
  switch3,
  switch4,
  switch5,
  switch6,
  switch7,
  switch8,
  switch9
};
enum class ButtonState : std::uint8_t { unknown, pressed, released };
enum class Callback : std::uint8_t { init_complete, state_changed, action };
enum class ButtonAction : std::uint8_t { none, click, hold };
struct Event {
  Button button{};
  Callback callback{};
  ButtonAction action{};
  ButtonState state{ButtonState::unknown};
  std::uint32_t at_ms{};
};
enum class Mode : std::uint8_t { game, debug };
enum class Phase : std::uint8_t { disarmed, waiting_for_release, running, finished, faulted };
enum class StopReason : std::uint8_t {
  operator_request,
  cord_reinserted,
  match_timeout,
  motor_disabled,
  inputs_unknown,
  authority_lost,
  communication_disabled,
  board_reset,
  action_failed
};
enum class LocalAction : std::uint8_t { open_reserve_side, enable_motors };
struct Config {
  std::array<inputs::InputConfig, 13> inputs{};
  std::uint8_t strategy_count{3};
  std::uint32_t match_duration_ms{99000}; // Active2026 game_task.c:112, despite100s comment.
  Mode mode{Mode::game};
  bool legacy_2025_motor_button_control{false}; // Explicit2025compatibility, never boot enabling.
  bool open_reserve_side_at_start{true};        // Active2026 game_task.c:88-91, only when idle.
};
struct Sinks {
  void *context{};
  Error (*guard)(void *, std::uint32_t now_ms){}; // Existing explicit controller arm/lease.
  Error (*stop)(void *, StopReason){};            // Must stop all, even after one backend error.
  Error (*action)(void *, LocalAction, std::uint32_t now_ms){};
};
struct State {
  std::uint16_t input_known{}, input_pressed{};
  std::uint16_t switch_known{}, switches_pressed{}; // 9mechanical switches, inverted legacy logic.
  bool cord_attached{}, side_right{}, motor_enable_requested{};
  std::uint8_t strategy{};
  Mode mode{Mode::game};
  Phase phase{Phase::disarmed};
  bool communication_enabled{true}, reset_latched{true}, reset_requested{};
  bool start_action_skipped{};
  std::uint32_t match_elapsed_ms{}, dropped_events{};
  Error error{Error::ok}, stop_error{Error::ok};
  StopReason stop_reason{StopReason::operator_request};
};
// Single control-task owner; callbacks never implicitly enable hardware on initialization.
class Application {
public:
  static constexpr std::size_t event_capacity = 64;
  Application(Config config, Sinks sinks) noexcept;
  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] Error initialize(std::uint32_t now_ms) noexcept;
  [[nodiscard]] Error sample_inputs(std::uint16_t electrical_high_bits,
                                    std::uint32_t now_ms) noexcept;
  [[nodiscard]] Error tick(std::uint32_t now_ms) noexcept;
  [[nodiscard]] Error arm_match(std::uint32_t now_ms) noexcept;
  [[nodiscard]] Error disarm_match(std::uint32_t now_ms) noexcept;
  [[nodiscard]] Error set_communication(bool enabled, std::uint32_t now_ms) noexcept;
  [[nodiscard]] Error set_mode(Mode mode) noexcept;
  [[nodiscard]] Error request_reset(std::uint32_t now_ms) noexcept;
  void clear_reset() noexcept { state_.reset_latched = false; }
  [[nodiscard]] bool take_reset_request() noexcept;
  [[nodiscard]] bool pop_event(Event &event) noexcept;
  [[nodiscard]] const State &state() const noexcept { return state_; }

private:
  Config config_{};
  Sinks sinks_{};
  inputs::InputBank<13> inputs_;
  State state_{};
  std::array<Event, event_capacity> events_{};
  std::size_t head_{}, size_{};
  std::uint32_t match_started_{};
  bool initialized_{};
  void publish(Event event) noexcept;
  void update_state(inputs::BankState snapshot) noexcept;
  [[nodiscard]] Error stop(StopReason reason, Phase phase) noexcept;
  [[nodiscard]] bool match_active() const noexcept;
};
} // namespace eurobot::application
