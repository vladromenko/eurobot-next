#pragma once
#include "eurobot/profile.hpp"
// This template intentionally DOES NOT arm. Supply only measured/reviewed deployment values.
inline eurobot::board::Deployment make_deployment() {
  eurobot::board::Deployment config{};
  // Confirm the physical oscillator is16MHz and main controller STM32F407VG.
  // channels>=8; pump_outputs[4..7] need verified wiring; zero means UNKNOWN.
  // max_active comes from PSU/MOSFET/connector ratings. No current limit is guessed here.
  // config.chassis.geometry = eurobot::legacy_2026_geometry(); // motor IDs must matchyear.
  // chassis velocity/acceleration/TPM/encoder delta limits must be filled.
  // Configure optional servo allowlists/limits/calibration, stepper electrical settings,
  // home input polarity and hardware ESTOP only after examining the actual robot.
  // Final 2026 is authoritative. The factories supply documented angles/IDs only:
  // config.grippers[0] = eurobot::gripper::legacy_2026_reference(0);
  // config.grippers[1] = eurobot::gripper::legacy_2026_reference(1);
  // Bind pump_channels/valve_channels to logical PumpBank indexes (not physical outputs).
  // Source primary main selects112 only; former254 slot is UNASSIGNED and inactive.
  // Configure real lift bounds/speed, servo allowlists, PCA calibration and safe currents.
  // For capture/release select Composite::base_strategy_2026 and explicit chassis speeds/
  // durations; old HL SimpleMove payload does not provide usable matched STM parameters.
  // Set each enabled gripper's reviewed=true only after its complete mapping is checked.
  // Set commissioned=true LAST, after reviewing all deployment parameters and HIL procedure.
  return config;
}
