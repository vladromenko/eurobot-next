#pragma once
#include "eurobot/board_installation.hpp"

// Copy to your own deployment.hpp, then fill measured hardware parameters as
// described in docs/BOARD_WIRING.md and docs/HARDWARE.md. No automatic arming.
inline eurobot::board::Deployment make_deployment() {
  auto config = eurobot::board::eight_pump_installation();
  // Known connector assignments already cover pumps1..8 and valves1..8 on PO1..16.
  // Fill supply budget/max_active/max_pump_on_ms, servo IDs and bounds, chassis
  // limits, enabled mechanisms, stepper protection/travel, home/ESTOP and polarities.
  // Set reviewed/commissioned only after checking those values on your robot.
  return config;
}
