#pragma once
#include "eurobot/profile.hpp"
// Same STM32F407 board, independent of previously installed firmware and harness.
inline eurobot::board::Deployment make_deployment() {
  eurobot::board::Deployment config{};
  config.mode = eurobot::board::DeploymentMode::DIAGNOSTIC_MODE;
  config.hardware_profile = eurobot::board::HardwareProfile::CUSTOM;
  config.pump_outputs.fill(0);
  config.valve_outputs.fill(0);
  config.max_active = 0;
  return config;
}
