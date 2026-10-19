#pragma once
#include <cstdint>
namespace eurobot::maintenance {
enum class Operation : std::uint8_t {
  board_info = 0,
  board_reset = 1,
  clear_reset = 2,
  wheel_ticks = 3,
  wheel_path = 4,
  reset_wheel_ticks = 5,
  set_pose = 6,
  servo_feedback = 7,
  servo_led = 8,
  servo_speed = 9,
  servo_torque_limit = 10,
  servo_wheel_mode = 11,
  servo_id = 12,
  motor_range = 13,
  set_motor_range = 14,
  motor_id = 15,
  stepper_reset = 16,
  pwm_info = 17,
  set_pwm_frequency = 18,
  gpio = 19,
  battery_calibration = 20,
  application_state = 21,
  match_arm = 22,
  match_disarm = 23,
  application_mode = 24,
  communication = 25,
  wheel_holding = 26,
  diagnostic_text = 27,
  application_event = 28,
  battery_id = 29,
  battery_value = 30,
  device_ping = 31,
  pca_control = 32,
  pca_allpulse = 33,
  stepper_parameter = 34,
  stepper_set_parameter = 35,
  servo_probe = 36,
  deployment_info = 37
};
enum class Access : std::uint8_t { unsupported, read_only, disarmed, armed, owned };
constexpr Access access(std::uint8_t operation) noexcept {
  switch (static_cast<Operation>(operation)) {
  case Operation::board_info:
  case Operation::deployment_info:
  case Operation::wheel_ticks:
  case Operation::wheel_path:
  case Operation::servo_feedback:
  case Operation::motor_range:
  case Operation::pwm_info:
  case Operation::gpio:
  case Operation::application_state:
  case Operation::diagnostic_text:
  case Operation::application_event:
  case Operation::battery_value:
  case Operation::device_ping:
  case Operation::stepper_parameter:
  case Operation::servo_probe:
    return Access::read_only;
  case Operation::match_arm:
  case Operation::wheel_holding:
  case Operation::pca_allpulse:
    return Access::armed;
  case Operation::match_disarm:
  case Operation::communication:
    return Access::owned;
  case Operation::board_reset:
  case Operation::clear_reset:
  case Operation::reset_wheel_ticks:
  case Operation::set_pose:
  case Operation::servo_led:
  case Operation::servo_speed:
  case Operation::servo_torque_limit:
  case Operation::servo_wheel_mode:
  case Operation::servo_id:
  case Operation::set_motor_range:
  case Operation::motor_id:
  case Operation::stepper_reset:
  case Operation::set_pwm_frequency:
  case Operation::battery_calibration:
  case Operation::application_mode:
  case Operation::battery_id:
  case Operation::pca_control:
  case Operation::stepper_set_parameter:
    return Access::disarmed;
  }
  return Access::unsupported;
}
} // namespace eurobot::maintenance
