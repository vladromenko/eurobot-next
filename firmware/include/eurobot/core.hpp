#pragma once
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>

namespace eurobot {
enum class Error : std::uint8_t {
  ok,
  invalid_config,
  invalid_argument,
  not_armed,
  estop,
  lease_conflict,
  lease_expired,
  stale_sequence,
  output_failure,
  timeout,
  unsupported,
  malformed,
  crc
};
constexpr bool succeeded(Error value) noexcept { return value == Error::ok; }
constexpr std::uint32_t channel_mask(std::uint8_t count) noexcept {
  return count == 32 ? 0xffffffffU : (count == 0 || count > 32 ? 0U : (1U << count) - 1U);
}
constexpr bool elapsed(std::uint32_t now, std::uint32_t then, std::uint32_t interval) noexcept {
  return static_cast<std::uint32_t>(now - then) >= interval;
}
// All mutable core objects belong to ONE control task. ISR/ROS producers must enqueue commands.
// Callback success means accepted by the output driver; it never implies physical actuation.
struct OutputSink {
  void *context{};
  Error (*write)(void *, std::uint32_t pumps, std::uint32_t valves){};
};
struct PumpConfig {
  std::uint8_t channels{8};
  std::uint8_t max_active{8}; // Policy only: must be approved against actual power hardware.
  std::uint32_t command_timeout_ms{250};
  std::uint32_t max_on_ms{}; // Zero disables per-channel dwell limit.
};
struct PumpState {
  std::uint32_t requested{}, commanded{}, commanded_known{}, measured_on{}, measured_known{};
  std::uint32_t valves_requested{}, valves_commanded{}, valves_commanded_known{};
  Error fault{Error::ok};
};
class PumpBank {
public:
  PumpBank(PumpConfig config, OutputSink sink) noexcept;
  [[nodiscard]] Error initialize(std::uint32_t now) noexcept;
  [[nodiscard]] Error set(std::uint32_t select, std::uint32_t values, std::uint32_t valve_select,
                          std::uint32_t valve_values, std::uint32_t now) noexcept;
  [[nodiscard]] Error stop() noexcept;
  [[nodiscard]] Error tick(std::uint32_t now) noexcept;
  [[nodiscard]] Error measure(std::uint32_t known, std::uint32_t on) noexcept;
  [[nodiscard]] const PumpState &state() const noexcept { return state_; }
  [[nodiscard]] const PumpConfig &config() const noexcept { return config_; }
  [[nodiscard]] bool valid() const noexcept;

private:
  PumpConfig config_;
  OutputSink sink_;
  PumpState state_{};
  std::array<std::uint32_t, 32> on_since_{}, pump_command_at_{}, valve_command_at_{};
  bool initialized_{};
};
struct Twist {
  float vx{}, vy{}, wz{};
};
struct Pose {
  float x{}, y{}, yaw{};
};
enum class OmniLayout : std::uint8_t { legacy_2025, legacy_2026 };
struct OmniConfig {
  float wheel_radius_m{};
  float center_to_wheel_m{};
  float gear_ratio{};
  std::uint32_t encoder_ticks_per_motor_turn{};
  OmniLayout layout{OmniLayout::legacy_2026};
  float command_scale{1.0F}, angular_command_scale{1.0F};
  float odometry_scale{1.0F}, odometry_x_scale{1.0F}, odometry_y_scale{1.0F},
      angular_odometry_scale{1.0F};
  std::array<float, 3> wheel_odometry_scale{1.0F, 1.0F, 1.0F};
};
[[nodiscard]] OmniConfig legacy_2025_geometry() noexcept;
[[nodiscard]] OmniConfig legacy_2026_geometry() noexcept;
class OmniKinematics {
public:
  explicit OmniKinematics(OmniConfig config) noexcept;
  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] std::optional<std::array<std::int32_t, 3>> wheel_tpm(Twist body) const noexcept;
  [[nodiscard]] std::optional<Twist>
  body_speed(const std::array<std::int32_t, 3> &tpm) const noexcept;
  [[nodiscard]] std::optional<Twist>
  body_delta(const std::array<std::int32_t, 3> &ticks) const noexcept;
  [[nodiscard]] const OmniConfig &config() const noexcept { return config_; }

private:
  OmniConfig config_;
  [[nodiscard]] std::optional<Twist> decode(const std::array<std::int32_t, 3> &ticks,
                                            float time_scale) const noexcept;
};
struct WheelSink {
  void *context{};
  Error (*write)(void *, const std::array<std::int32_t, 3> &tpm){};
};
struct ChassisConfig {
  OmniConfig geometry{legacy_2026_geometry()};
  float max_linear_mps{}, max_angular_radps{}, max_accel_mps2{}, max_angular_accel_radps2{};
  std::int32_t max_wheel_tpm{}; // Required deployment configuration; no guessed motor limit.
  std::uint32_t command_timeout_ms{250}, max_tick_ms{100};
  std::int32_t max_encoder_delta{}; // Required before accepting measured encoder increments.
};
struct ChassisState {
  Twist requested{}, commanded{}, measured{};
  Pose pose{};
  std::array<std::int32_t, 3> wheel_commanded{}, wheel_measured{};
  std::uint8_t measured_known{};
  bool commanded_known{}, pose_known{};
  Error fault{Error::ok};
};
class Chassis {
public:
  Chassis(ChassisConfig config, WheelSink sink) noexcept;
  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] Error initialize(std::uint32_t now) noexcept;
  [[nodiscard]] Error set(Twist request, std::uint32_t now) noexcept;
  [[nodiscard]] Error tick(std::uint32_t now) noexcept;
  [[nodiscard]] Error stop() noexcept;
  [[nodiscard]] Error feedback(const std::array<std::int32_t, 3> &tpm,
                               const std::array<std::int32_t, 3> &deltas,
                               std::uint8_t known = 7) noexcept;
  [[nodiscard]] Error reset_pose(Pose reference = {}) noexcept;
  void invalidate_feedback() noexcept {
    state_.measured_known = 0;
    state_.pose_known = false;
  }
  [[nodiscard]] const ChassisState &state() const noexcept { return state_; }
  [[nodiscard]] const ChassisConfig &config() const noexcept { return config_; }

private:
  ChassisConfig config_;
  WheelSink sink_;
  OmniKinematics kinematics_;
  ChassisState state_{};
  std::uint32_t last_command_{}, last_tick_{};
  bool initialized_{};
};
struct PidConfig {
  float kp{}, ki{}, kd{}, output_min{}, output_max{}, integral_min{}, integral_max{};
};
class Pid {
public:
  explicit Pid(PidConfig config) noexcept : config_(config) {}
  [[nodiscard]] bool valid() const noexcept;
  [[nodiscard]] std::optional<float> update(float target, float measured, float dt) noexcept;
  void reset() noexcept {
    integral_ = 0;
    previous_measurement_ = 0;
    seeded_ = false;
  }
  [[nodiscard]] float integral() const noexcept { return integral_; }

private:
  PidConfig config_;
  float integral_{}, previous_measurement_{};
  bool seeded_{};
};
struct StopSink {
  void *context{};
  Error (*stop)(void *){};
};
struct ControllerState {
  bool armed{}, estop{}, fault{};
  std::uint32_t owner{};
  Error last_error{Error::ok};
};
class Controller {
public:
  Controller(PumpBank &pumps, Chassis &chassis, StopSink auxiliary = {}) noexcept
      : pumps_(pumps), chassis_(chassis), auxiliary_(auxiliary) {}
  [[nodiscard]] Error initialize(std::uint32_t now) noexcept;
  [[nodiscard]] Error claim(std::uint32_t owner, std::uint32_t sequence, std::uint32_t lease_ms,
                            std::uint32_t now) noexcept;
  [[nodiscard]] Error rearm(std::uint32_t owner, std::uint32_t sequence,
                            std::uint32_t now) noexcept;
  [[nodiscard]] Error heartbeat(std::uint32_t owner, std::uint32_t sequence,
                                std::uint32_t now) noexcept;
  [[nodiscard]] Error release(std::uint32_t owner, std::uint32_t sequence,
                              std::uint32_t now) noexcept;
  [[nodiscard]] Error pumps(std::uint32_t owner, std::uint32_t sequence, std::uint32_t select,
                            std::uint32_t values, std::uint32_t valve_select,
                            std::uint32_t valve_values, std::uint32_t now) noexcept;
  [[nodiscard]] Error velocity(std::uint32_t owner, std::uint32_t sequence, Twist request,
                               std::uint32_t now) noexcept;
  [[nodiscard]] Error emergency_stop() noexcept;
  void physical_estop(bool asserted) noexcept;
  [[nodiscard]] Error tick(std::uint32_t now) noexcept;
  [[nodiscard]] const ControllerState &state() const noexcept { return state_; }
  [[nodiscard]] PumpBank &pump_bank() noexcept { return pumps_; }
  [[nodiscard]] Chassis &chassis() noexcept { return chassis_; }
  [[nodiscard]] bool lease_active(std::uint32_t now) const noexcept;
  // Extension commands must pass this same owner/sequence/armed gate, never a parallel API.
  [[nodiscard]] Error authorize(std::uint32_t owner, std::uint32_t sequence, std::uint32_t now,
                                bool require_armed = true) noexcept;
  [[nodiscard]] Error fail(Error error) noexcept;

private:
  PumpBank &pumps_;
  Chassis &chassis_;
  StopSink auxiliary_;
  ControllerState state_{};
  std::uint32_t last_sequence_{}, lease_duration_{}, lease_renewed_{}, last_owner_{};
  bool initialized_{}, physical_estop_{};
  [[nodiscard]] Error stop_all() noexcept;
};
namespace wire {
constexpr std::size_t max_payload = 128, frame_overhead = 16, status_size = 84;
constexpr std::uint8_t version = 1;
enum class Type : std::uint8_t {
  claim = 1,
  rearm = 2,
  heartbeat = 3,
  pumps = 4,
  twist = 5,
  estop = 6,
  release = 7,
  status = 8,
  sensors = 9,
  actuator = 0x20
};
struct Frame {
  std::uint8_t type{};
  std::uint32_t sequence{}, owner{};
  std::uint16_t length{};
  std::array<std::uint8_t, max_payload> payload{};
};
[[nodiscard]] std::uint16_t crc16(std::span<const std::uint8_t> bytes) noexcept;
[[nodiscard]] std::size_t encode_frame(const Frame &frame, std::span<std::uint8_t> output) noexcept;
[[nodiscard]] std::uint32_t read_u32(const std::uint8_t *p) noexcept;
void write_u32(std::uint8_t *p, std::uint32_t value) noexcept;
[[nodiscard]] float read_f32(const std::uint8_t *p) noexcept;
void write_f32(std::uint8_t *p, float value) noexcept;
class FrameParser {
public:
  explicit FrameParser(std::uint32_t timeout_ms = 100) noexcept : timeout_ms_(timeout_ms) {}
  [[nodiscard]] std::optional<Frame> push(std::uint8_t byte, std::uint32_t now) noexcept;
  void reset() noexcept {
    used_ = 0;
    expected_ = 0;
  }
  [[nodiscard]] std::uint32_t rejected() const noexcept { return rejected_; }

private:
  std::array<std::uint8_t, max_payload + frame_overhead> bytes_{};
  std::size_t used_{}, expected_{};
  std::uint32_t timeout_ms_, last_byte_{}, rejected_{};
};
struct Extension {
  void *context{};
  Error (*command)(void *, std::span<const std::uint8_t>, std::uint32_t now){};
  Error (*read)(void *, std::span<std::uint8_t>, std::size_t &, std::uint32_t now){};
  Error (*request)(void *, std::uint8_t type, std::uint32_t owner, std::span<const std::uint8_t>,
                   std::span<std::uint8_t>, std::size_t &, std::uint32_t now){};
  Error (*guard)(void *, std::uint8_t type, std::span<const std::uint8_t>){};
};
class Dispatcher {
public:
  explicit Dispatcher(Controller &controller, Extension extension = {}) noexcept
      : controller_(controller), extension_(extension) {}
  [[nodiscard]] Frame dispatch(const Frame &request, std::uint32_t now) noexcept;

private:
  Controller &controller_;
  Extension extension_;
};
} // namespace wire
} // namespace eurobot
