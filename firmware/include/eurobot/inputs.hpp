#pragma once
#include "eurobot/core.hpp"
#include <array>
#include <cstddef>
#include <cstdint>

namespace eurobot::inputs {
struct InputConfig {
  bool active_high{true};
  std::uint32_t debounce_ms{8}, hold_ms{500}, maximum_sample_gap_ms{20};
};
enum class Event : std::uint8_t { none = 0, pressed = 1, released = 2, held = 4, clicked = 8 };
constexpr bool has_event(std::uint8_t events, Event event) noexcept {
  return (events & static_cast<std::uint8_t>(event)) != 0;
}
struct InputState {
  bool known{}, pressed{};
  std::uint32_t changed_at_ms{};
};
struct Update {
  InputState state{};
  std::uint8_t events{};
  Error error{Error::ok};
};

// A single owner samples and consumes edges. An initially held switch becomes
// known after debounce, but generates no press/hold action until released and pressed again.
class DebouncedInput {
public:
  DebouncedInput() noexcept = default;
  explicit DebouncedInput(InputConfig config) noexcept : config_(config) {}
  [[nodiscard]] bool valid() const noexcept {
    return config_.debounce_ms > 0 && config_.debounce_ms < 0x80000000U &&
           config_.hold_ms < 0x80000000U &&
           (config_.hold_ms == 0 || config_.hold_ms >= config_.debounce_ms) &&
           config_.maximum_sample_gap_ms > 0 && config_.maximum_sample_gap_ms < 0x80000000U;
  }
  void invalidate() noexcept {
    state_ = {};
    seeded_ = false;
    hold_eligible_ = false;
    hold_sent_ = false;
  }
  [[nodiscard]] InputState snapshot(std::uint32_t now) const noexcept {
    auto state = state_;
    if (!valid() || !seeded_ ||
        static_cast<std::uint32_t>(now - last_sample_) > config_.maximum_sample_gap_ms) {
      state.known = false;
      state.pressed = false;
    }
    return state;
  }
  [[nodiscard]] Update sample(bool electrical_high, std::uint32_t now) noexcept {
    if (!valid()) {
      invalidate();
      return {state_, 0, Error::invalid_config};
    }
    const bool pressed = electrical_high == config_.active_high;
    if (!seeded_) {
      seed(pressed, now);
      return {state_, 0, Error::ok};
    }
    const auto gap = static_cast<std::uint32_t>(now - last_sample_);
    if (gap >= 0x80000000U || gap > config_.maximum_sample_gap_ms) {
      seed(pressed, now);
      return {state_, 0, gap >= 0x80000000U ? Error::invalid_argument : Error::timeout};
    }
    last_sample_ = now;
    if (candidate_ != pressed) {
      candidate_ = pressed;
      candidate_since_ = now;
    }
    std::uint8_t events = 0;
    if (elapsed(now, candidate_since_, config_.debounce_ms)) {
      if (!state_.known) {
        state_ = {true, candidate_, now};
        hold_eligible_ = false;
        hold_sent_ = false;
      } else if (state_.pressed != candidate_) {
        const bool clicked =
            !candidate_ && hold_eligible_ && !hold_sent_ &&
            (config_.hold_ms == 0 || !elapsed(now, pressed_since_, config_.hold_ms));
        state_ = {true, candidate_, now};
        hold_eligible_ = candidate_;
        hold_sent_ = false;
        pressed_since_ = now;
        events = static_cast<std::uint8_t>(candidate_ ? Event::pressed : Event::released);
        if (clicked) {
          events |= static_cast<std::uint8_t>(Event::clicked);
        }
      }
    }
    if (state_.known && state_.pressed && hold_eligible_ && !hold_sent_ && config_.hold_ms != 0 &&
        elapsed(now, pressed_since_, config_.hold_ms)) {
      events |= static_cast<std::uint8_t>(Event::held);
      hold_sent_ = true;
    }
    return {state_, events, Error::ok};
  }

private:
  InputConfig config_{};
  InputState state_{};
  std::uint32_t candidate_since_{}, last_sample_{}, pressed_since_{};
  bool seeded_{}, candidate_{}, hold_eligible_{}, hold_sent_{};
  void seed(bool pressed, std::uint32_t now) noexcept {
    state_ = {};
    candidate_ = pressed;
    candidate_since_ = now;
    last_sample_ = now;
    seeded_ = true;
    hold_eligible_ = false;
    hold_sent_ = false;
  }
};

struct BankState {
  std::uint32_t known{}, pressed{}, pressed_edges{}, released_edges{}, held_edges{},
      clicked_edges{};
  Error error{Error::ok};
};
// Edges belong only to this sample result; there is no hidden unbounded event queue.
template <std::size_t Count = 13> class InputBank {
  static_assert(Count > 0 && Count <= 32);

public:
  explicit InputBank(const std::array<InputConfig, Count> &configs = {}) noexcept {
    for (std::size_t i = 0; i < Count; ++i) {
      inputs_[i] = DebouncedInput{configs[i]};
    }
  }
  [[nodiscard]] bool valid() const noexcept {
    bool result = true;
    for (const auto &input : inputs_) {
      result = result && input.valid();
    }
    return result;
  }
  void invalidate() noexcept {
    for (auto &input : inputs_) {
      input.invalidate();
    }
  }
  [[nodiscard]] BankState snapshot(std::uint32_t now) const noexcept {
    BankState result{};
    for (std::size_t i = 0; i < Count; ++i) {
      const auto state = inputs_[i].snapshot(now);
      const auto bit = 1U << i;
      if (state.known) {
        result.known |= bit;
        if (state.pressed) {
          result.pressed |= bit;
        }
      }
    }
    if (!valid()) {
      result.error = Error::invalid_config;
    }
    return result;
  }
  [[nodiscard]] BankState sample(std::uint32_t electrical_high, std::uint32_t now) noexcept {
    if ((electrical_high & ~channel_mask(static_cast<std::uint8_t>(Count))) != 0) {
      auto result = snapshot(now);
      result.error = Error::invalid_argument;
      return result;
    }
    BankState result{};
    for (std::size_t i = 0; i < Count; ++i) {
      const auto bit = 1U << i;
      const auto update = inputs_[i].sample((electrical_high & bit) != 0, now);
      if (update.error != Error::ok) {
        result.error = update.error;
      }
      if (update.state.known) {
        result.known |= bit;
        if (update.state.pressed) {
          result.pressed |= bit;
        }
      }
      if (has_event(update.events, Event::pressed)) {
        result.pressed_edges |= bit;
      }
      if (has_event(update.events, Event::released)) {
        result.released_edges |= bit;
      }
      if (has_event(update.events, Event::held)) {
        result.held_edges |= bit;
      }
      if (has_event(update.events, Event::clicked)) {
        result.clicked_edges |= bit;
      }
    }
    return result;
  }

private:
  std::array<DebouncedInput, Count> inputs_{};
};
} // namespace eurobot::inputs
