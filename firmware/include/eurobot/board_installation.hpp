#pragma once
#include "eurobot/source_2026.hpp"

namespace eurobot::board {
// New wiring plan for the supplied 20-output board. Electrical/mechanical limits
// and device identities cannot be inferred from its connector poster.
[[nodiscard]] inline Deployment eight_pump_installation() {
  Deployment config{};
  config.pump_outputs.fill(0);
  config.valve_outputs.fill(0);
  for (std::size_t i = 0; i < connectors::installation_pumps.size(); ++i) {
    config.pump_outputs[i] = connectors::installation_pumps[i];
    config.valve_outputs[i] = connectors::installation_valves[i];
  }
  source_2026::apply_known_parameters(config);
  for (std::size_t i = 0; i < config.grippers.size(); ++i) {
    auto &grip = config.grippers[i];
    for (std::size_t channel = 0; channel < grip.pump_channels.size(); ++channel) {
      const auto route = static_cast<std::uint8_t>(4 * i + channel);
      grip.pump_channels[channel] = route;
      grip.valve_channels[channel] = route;
    }
  }
  config.application.mode = application::Mode::game;
  config.application.match_duration_ms = 99000;
  config.application.strategy_count = 3;
  // commissioned/reviewed stay false; unknown power, IDs and travel stay unknown.
  return config;
}
} // namespace eurobot::board
