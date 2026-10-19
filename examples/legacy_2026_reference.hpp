#pragma once
#include "eurobot/source_2026.hpp"
#include <array>
#include <cstdint>

namespace eurobot::source_2026 {
// Source facts, not electrical/mechanical approval. Original files remain read-only:
// 02_FIRMWARE_2026/SOURCE/Core/App/{bus_device,kinematics}.c
// 02_FIRMWARE_2026/SOURCE/Core/OS_Tasks/gripper_task.c:138 and game_task.c:112.
inline constexpr std::array<std::uint8_t, 4> known_pump_outputs{1, 3, 5, 7};
inline constexpr std::array<std::uint8_t, 4> known_valve_outputs{2, 4, 6, 8};
inline constexpr std::array<std::uint8_t, 3> known_motor_ids{0x22, 0x21, 0x23};
inline constexpr std::array<std::uint8_t, 9> source_ax_ids{112, 7, 8, 1, 2, 16, 25, 98, 13};
inline constexpr std::uint8_t ambiguous_main_broadcast_id = 254; // NEVER an enabled binding.
inline constexpr std::array<float, 4> source_stepper_current_ma{1100, 1500, 1100, 1100};
inline constexpr std::uint32_t source_match_duration_ms = 99000;

[[nodiscard]] inline board::Deployment reference() {
  board::Deployment config{};
  config.commissioned = false;
  config.channels = 8;
  config.max_active = 0; // TODO_UNKNOWN: confirmed PSU/connector/current budget.
  config.pump_outputs.fill(0);
  config.valve_outputs.fill(0);
  for (std::size_t index = 0; index < known_pump_outputs.size(); ++index) {
    config.pump_outputs[index] = known_pump_outputs[index];
    config.valve_outputs[index] = known_valve_outputs[index];
  }
  // TODO_UNKNOWN: independent routes for pumps5..8 and their valves, or documented extension.
  // Both source grippers share the SAME four confirmed logical routes, never eight invented ones.
  board::source_2026::apply_known_parameters(config);
  config.motor_ids = known_motor_ids;
  config.chassis.geometry = legacy_2026_geometry();
  // TODO_UNKNOWN: measured velocity/acceleration/TPM/encoder-delta limits remain zero.
  for (std::size_t index = 0; index < config.grippers.size(); ++index) {
    auto &grip = config.grippers[index];
    grip.reviewed = false;
    grip.pump_channels = {0, 1, 2, 3};
    grip.valve_channels = {0, 1, 2, 3};
    // Factory already leaves ambiguous ID254 UNASSIGNED, not replaced with a guessed ID.
    // Source models/IDs/protocol ranges, PCA calibration, lift speed and disable
    // behaviour are now filled. Verify physical travel, current suitability and
    // the remaining primary identity before activation.
  }
  config.application.mode = application::Mode::game;
  config.application.match_duration_ms = source_match_duration_ms;
  config.application.strategy_count = 3;
  config.application.legacy_2025_motor_button_control = false;
  config.debug_uart_enabled = false; // Explicit enable only after wiring PC12/PD2 is verified.
  // Known AX/PCA allowlists, L6474 settings and home polarity are filled from source.
  // Global commissioning and PCA/PWM/DC/stepper activation remain false in this reference.
  // TODO_UNKNOWN: physical travel and ESTOP wiring; optional devices outside source roles.
  // TODO_UNKNOWN: valve electrical polarity and safe unpowered state; hardware energy isolation.
  // Do not change commissioned/reviewed solely to make validation pass.
  return config;
}
} // namespace eurobot::source_2026

// Compatible with EUROBOT_BOARD_CONFIG, but intentionally cannot arm in this state.
inline eurobot::board::Deployment make_deployment() { return eurobot::source_2026::reference(); }
