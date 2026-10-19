#pragma once
#include "eurobot/robot_2025.hpp"
// ROBOT_2025_HARDWARE_PROFILE: first-flash candidate, always diagnostic.
[[nodiscard]] inline eurobot::board::Deployment make_deployment() {
  return eurobot::board::robot_2025();
}
