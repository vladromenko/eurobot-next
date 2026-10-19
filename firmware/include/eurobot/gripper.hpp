#pragma once
#include "eurobot/core.hpp"
#include <limits>
namespace eurobot::gripper {
constexpr std::uint8_t unassigned = 255;
constexpr std::size_t maximum_steps = 64;
constexpr float unknown_parameter = std::numeric_limits<float>::quiet_NaN();
enum class Operation : std::uint8_t {
  cursor = 0,
  side_grips = 1,
  rotate_main = 2,
  pumps = 3,
  valves = 4,
  blocks_rotate = 5,
  timed_move = 6,
  lift_height = 7,
  relative_move = 8,
  wheel_calibration = 9,
  blocks_shift = 10,
  blocks_rotate_only = 11,
  relative_move_poll = 12,
  slide = 13,
  legacy_2025_reset = 20,
  legacy_2025_cycle = 21,
  capture = 22,
  release = 23,
  prepare = 24
};
enum class Device : std::uint8_t { ax, mx, pca, stepper };
enum class Action : std::uint8_t {
  servo_group,
  pca_group,
  outputs,
  stepper_current,
  stepper_move,
  stepper_disable,
  wait_input,
  delay,
  timed_drive,
  relative_drive,
  wheel_calibration,
  chassis_stop
};
// Different evidence categories, not interchangeable levels of physical
// certainty.
enum class Evidence : std::uint8_t { unknown, commanded, settled, referenced_steps, measured };
enum class Phase : std::uint8_t { idle, running, completed, cancelled, faulted };
struct Target {
  Device device{Device::ax};
  std::uint8_t id{unassigned};
  float value{};                 // Degrees, milliamps, or absolute step count, depending on
                                 // action.
  float rate{};                  // Dynamixel raw speed or stepper steps/s; zero for PCA.
  std::int64_t position_steps{}; // Exact absolute step coordinate for stepper actions.
};
struct Drive {
  Twist velocity{};
  float displacement_m{}, maximum_speed_mps{};
  std::uint8_t axis{}; // 0=robot X, 1=robot Y, 2=rotation for timed drive only.
  std::uint32_t duration_ms{};
  float tolerance_m{}, minimum_speed_mps{}, acceleration_mps2{};
  float heading_kp{}, lateral_kp{}, maximum_angular_radps{};
  std::int32_t wheel_goal_ticks{}, wheel_tpm{}, wheel_stop_margin{};
};
struct Step {
  Action action{Action::delay};
  std::array<Target, 4> targets{};
  std::uint8_t count{};
  Evidence required{Evidence::commanded};
  std::uint32_t timeout_ms{1500}, settle_ms{};
  float tolerance{}, alternate_current_ma{}; // Optional lowering current
                                             // selected from real reference.
  bool enable_before_settle{}; // Lift holding torque must settle before step pulses begin.
  std::uint32_t pump_select{}, pump_values{}, valve_select{}, valve_values{};
  std::uint32_t input_select{}, input_values{};
  Drive drive{};
};
struct ServoBinding {
  Device device{Device::ax};
  std::uint8_t id{unassigned};
  float minimum_deg{}, maximum_deg{};
};
// Exact, source-selected composite. No universal capture sequence is inferred
// from competing BTs.
enum class Composite : std::uint8_t { disabled, base_strategy_2026 };
struct Config {
  bool reviewed{};
  std::array<ServoBinding, 2> main{}, side{};
  ServoBinding cursor{}, pinion{};
  std::array<std::uint8_t, 4> pump_channels{unassigned, unassigned, unassigned, unassigned};
  std::array<std::uint8_t, 4> valve_channels{unassigned, unassigned, unassigned, unassigned};
  std::array<std::uint8_t, 4> block_channels{unassigned, unassigned, unassigned, unassigned};
  std::array<std::uint8_t, 2> shift_channels{unassigned, unassigned};
  std::array<float, 2> side_closed_deg{unknown_parameter, unknown_parameter};
  std::array<float, 2> side_open_deg{unknown_parameter, unknown_parameter};
  std::array<float, 2> shift_closed_deg{unknown_parameter, unknown_parameter};
  std::array<float, 2> shift_open_deg{unknown_parameter, unknown_parameter};
  float main_up_deg{unknown_parameter}, main_down_deg{unknown_parameter},
      main_cursor_deg{unknown_parameter};
  float cursor_horizontal_deg{unknown_parameter}, cursor_vertical_deg{unknown_parameter};
  float pinion_start_deg{unknown_parameter}, pinion_deg_per_mm{};
  float main_speed_raw{}, side_speed_raw{}, cursor_speed_raw{}, pinion_speed_raw{};
  float angle_tolerance_deg{};
  float block_zero_deg{unknown_parameter}, block_rotated_deg{unknown_parameter};
  std::uint8_t lift_stepper{unassigned};
  float lift_steps_per_mm{}, lift_speed_steps_s{}, lift_up_current_ma{}, lift_down_current_ma{};
  float minimum_height_mm{}, maximum_height_mm{};
  std::int64_t minimum_lift_steps{}, maximum_lift_steps{};
  std::uint32_t servo_timeout_ms{1500}, lift_timeout_ms{4000}, operation_timeout_ms{10000};
  bool disable_lift_after_move{};
  Composite composite{Composite::disabled};
  Drive drive_reference{};
  float capture_forward_mps{}, capture_backward_mps{};
  std::uint32_t capture_forward_ms{}, capture_backward_ms{};
  std::uint8_t capture_axis{};
};
// Technical source references, never commissioned deployment configurations.
[[nodiscard]] Config legacy_2026_reference(std::uint8_t gripper_index) noexcept;
[[nodiscard]] Config legacy_2025_reference() noexcept;
struct Request {
  Operation operation{Operation::cursor};
  std::uint8_t gripper{};
  float value{}; // Boolean/state, height mm, or slide mm, depending on operation.
  std::uint32_t mask{};
  std::uint32_t timeout_ms{10000};
  float argument{}, speed{}; // Timed: duration ms; relative: dy m and max speed m/s.
};
struct Completion {
  bool known{}, done{};
  Evidence evidence{Evidence::unknown};
};
struct Sinks {
  void *context{};
  Error (*guard)(void *, std::uint32_t owner, std::uint32_t now_ms){};
  Error (*validate)(void *, const Step &){};
  Error (*issue)(void *, const Step &, std::uint32_t now_ms){};
  Error (*feedback)(void *, const Step &, Completion &, std::uint32_t now_ms){};
  Error (*stop)(void *, Error reason){};
};
struct State {
  Phase phase{Phase::idle};
  Operation operation{Operation::cursor};
  std::uint8_t gripper{}, step{}, count{};
  std::uint32_t owner{};
  Error error{Error::ok}, stop_error{Error::ok};
  Evidence completion{Evidence::unknown};
  std::uint8_t commanded_steps{}, settled_steps{}, referenced_steps{}, measured_steps{};
};
class Engine {
public:
  Engine(std::span<const Config> configs, Sinks sinks) noexcept;
  [[nodiscard]] Error start(std::uint32_t owner, Request request, std::uint32_t now_ms) noexcept;
  [[nodiscard]] Error tick(std::uint32_t now_ms) noexcept;
  [[nodiscard]] Error cancel(std::uint32_t owner) noexcept;
  [[nodiscard]] Error abort(Error reason) noexcept;
  [[nodiscard]] bool busy() const noexcept { return state_.phase == Phase::running; }
  [[nodiscard]] const State &state() const noexcept { return state_; }
  [[nodiscard]] std::span<const Step> plan() const noexcept {
    return {steps_.data(), state_.count};
  }
  [[nodiscard]] const Step *current_step() const noexcept {
    return busy() ? &steps_[state_.step] : nullptr;
  }

private:
  std::array<Config, 2> configs_{};
  std::size_t config_count_{};
  Sinks sinks_;
  State state_{};
  std::array<Step, maximum_steps> steps_{};
  std::uint32_t operation_started_{}, step_started_{}, operation_timeout_{};
  bool issued_{}, has_plan_{};
  Request accepted_request_{};
  [[nodiscard]] Error make_plan(Request request) noexcept;
  [[nodiscard]] Error append(Step step) noexcept;
  void restore(State previous) noexcept;
  void advance(Evidence evidence, std::uint32_t now_ms) noexcept;
  [[nodiscard]] Error terminate(Phase phase, Error reason) noexcept;
};
} // namespace eurobot::gripper
