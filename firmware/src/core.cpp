#include "eurobot/core.hpp"
#include "eurobot/maintenance.hpp"
#include <algorithm>
#include <cmath>
#include <limits>

namespace eurobot {
namespace {
constexpr float pi = 3.14159265358979323846F, cos30 = 0.8660254037844386F;
bool positive(float v) noexcept { return std::isfinite(v) && v > 0; }
bool finite(Twist v) noexcept {
  return std::isfinite(v.vx) && std::isfinite(v.vy) && std::isfinite(v.wz);
}
bool moving(Twist v) noexcept { return v.vx != 0 || v.vy != 0 || v.wz != 0; }
bool interval_valid(std::uint32_t v) noexcept { return v != 0 && v < 0x80000000U; }
Error output_result(Error error) noexcept {
  return succeeded(error) ? Error::ok : Error::output_failure;
}
} // namespace
PumpBank::PumpBank(PumpConfig config, OutputSink sink) noexcept : config_(config), sink_(sink) {}
bool PumpBank::valid() const noexcept {
  return config_.channels > 0 && config_.channels <= 32 && config_.max_active <= config_.channels &&
         interval_valid(config_.command_timeout_ms) && config_.max_on_ms < 0x80000000U &&
         sink_.write;
}
Error PumpBank::initialize(std::uint32_t now) noexcept {
  initialized_ = false;
  if (!valid()) {
    state_.fault = Error::invalid_config;
    return state_.fault;
  }
  pump_command_at_.fill(now);
  valve_command_at_.fill(now);
  const auto result = stop();
  initialized_ = succeeded(result);
  return result;
}
Error PumpBank::stop() noexcept {
  state_.requested = 0;
  state_.valves_requested = 0;
  if (!sink_.write) {
    state_.commanded_known = 0;
    state_.valves_commanded_known = 0;
    state_.fault = Error::output_failure;
    return state_.fault;
  }
  const auto result = output_result(sink_.write(sink_.context, 0, 0));
  state_.commanded_known = succeeded(result) ? channel_mask(config_.channels) : 0;
  state_.valves_commanded_known = state_.commanded_known;
  if (succeeded(result)) {
    state_.commanded = 0;
    state_.valves_commanded = 0;
  }
  state_.fault = result;
  return result;
}
Error PumpBank::set(std::uint32_t select, std::uint32_t values, std::uint32_t valve_select,
                    std::uint32_t valve_values, std::uint32_t now) noexcept {
  if (!initialized_) {
    return Error::not_armed;
  }
  const auto mask = channel_mask(config_.channels);
  if (((select | values | valve_select | valve_values) & ~mask) != 0 || (values & ~select) != 0 ||
      (valve_values & ~valve_select) != 0) {
    return Error::invalid_argument;
  }
  const auto next = (state_.requested & ~select) | values;
  const auto valves = (state_.valves_requested & ~valve_select) | valve_values;
  if (std::popcount(next) > config_.max_active) {
    return Error::invalid_argument;
  }
  const auto newly_on = next & ~state_.requested;
  state_.requested = next;
  state_.valves_requested = valves;
  for (std::uint8_t i = 0; i < config_.channels; ++i) {
    if ((newly_on & (1U << i)) != 0) {
      on_since_[i] = now;
    }
    if ((select & (1U << i)) != 0) {
      pump_command_at_[i] = now;
    }
    if ((valve_select & (1U << i)) != 0) {
      valve_command_at_[i] = now;
    }
  }
  const auto result = output_result(sink_.write(sink_.context, next, valves));
  if (succeeded(result)) {
    state_.commanded = next;
    state_.valves_commanded = valves;
    state_.commanded_known = mask;
    state_.valves_commanded_known = mask;
  } else {
    state_.commanded_known = 0;
    state_.valves_commanded_known = 0;
    initialized_ = false;
  }
  state_.fault = result;
  return result;
}
Error PumpBank::tick(std::uint32_t now) noexcept {
  if (!valid()) {
    return Error::invalid_config;
  }
  if (!initialized_) {
    return Error::not_armed;
  }
  bool timed_out = false;
  for (std::uint8_t i = 0; i < config_.channels; ++i) {
    const auto bit = 1U << i;
    timed_out = timed_out ||
                ((state_.requested & bit) != 0 &&
                 elapsed(now, pump_command_at_[i], config_.command_timeout_ms)) ||
                ((state_.valves_requested & bit) != 0 &&
                 elapsed(now, valve_command_at_[i], config_.command_timeout_ms));
  }
  if (config_.max_on_ms != 0) {
    for (std::uint8_t i = 0; i < config_.channels; ++i) {
      timed_out = timed_out || ((state_.requested & (1U << i)) != 0 &&
                                elapsed(now, on_since_[i], config_.max_on_ms));
    }
  }
  if (timed_out) {
    const auto result = stop();
    return succeeded(result) ? Error::timeout : result;
  }
  return Error::ok;
}
Error PumpBank::measure(std::uint32_t known, std::uint32_t on) noexcept {
  if (((known | on) & ~channel_mask(config_.channels)) != 0 || (on & ~known) != 0) {
    return Error::invalid_argument;
  }
  state_.measured_known = known;
  state_.measured_on = on;
  return Error::ok;
}
OmniConfig legacy_2025_geometry() noexcept {
  OmniConfig value{};
  value.wheel_radius_m = 0.03F;
  value.center_to_wheel_m = 0.1075F;
  value.gear_ratio = 21;
  value.encoder_ticks_per_motor_turn = 4096;
  value.layout = OmniLayout::legacy_2025;
  return value;
}
OmniConfig legacy_2026_geometry() noexcept {
  OmniConfig value{};
  value.wheel_radius_m = 0.0295F;
  value.center_to_wheel_m = 0.1217F;
  value.gear_ratio = 28;
  value.encoder_ticks_per_motor_turn = 4096;
  value.layout = OmniLayout::legacy_2026;
  value.odometry_scale = 0.988F;
  value.odometry_x_scale = 0.988F;
  value.angular_odometry_scale = 0.988F;
  value.wheel_odometry_scale = {1.003F, 0.984F, 0.997F};
  return value;
}
OmniKinematics::OmniKinematics(OmniConfig config) noexcept : config_(config) {}
bool OmniKinematics::valid() const noexcept {
  bool result =
      positive(config_.wheel_radius_m) && positive(config_.center_to_wheel_m) &&
      positive(config_.gear_ratio) && config_.encoder_ticks_per_motor_turn > 0 &&
      positive(config_.command_scale) && positive(config_.angular_command_scale) &&
      positive(config_.odometry_scale) && positive(config_.odometry_x_scale) &&
      positive(config_.odometry_y_scale) && positive(config_.angular_odometry_scale) &&
      (config_.layout == OmniLayout::legacy_2025 || config_.layout == OmniLayout::legacy_2026);
  for (const auto scale : config_.wheel_odometry_scale) {
    result = result && positive(scale);
  }
  return result;
}
std::optional<std::array<std::int32_t, 3>> OmniKinematics::wheel_tpm(Twist body) const noexcept {
  if (!valid() || !finite(body)) {
    return std::nullopt;
  }
  const auto arm_rotation = config_.center_to_wheel_m * body.wz * config_.angular_command_scale;
  std::array<float, 3> linear{};
  if (config_.layout == OmniLayout::legacy_2026) {
    linear = {cos30 * body.vx - 0.5F * body.vy - arm_rotation, body.vy - arm_rotation,
              -cos30 * body.vx - 0.5F * body.vy - arm_rotation};
  } else {
    linear = {cos30 * body.vx + 0.5F * body.vy + arm_rotation,
              -cos30 * body.vx + 0.5F * body.vy + arm_rotation, -body.vy + arm_rotation};
  }
  const auto factor = 60.0F * config_.gear_ratio *
                      static_cast<float>(config_.encoder_ticks_per_motor_turn) /
                      (config_.command_scale * 2 * pi * config_.wheel_radius_m);
  std::array<std::int32_t, 3> output{};
  for (std::size_t i = 0; i < 3; ++i) {
    const auto tpm = linear[i] * factor;
    if (!std::isfinite(tpm) ||
        static_cast<double>(tpm) > std::numeric_limits<std::int32_t>::max() ||
        static_cast<double>(tpm) < std::numeric_limits<std::int32_t>::min()) {
      return std::nullopt;
    }
    output[i] = static_cast<std::int32_t>(tpm);
  }
  return output;
}
std::optional<Twist> OmniKinematics::decode(const std::array<std::int32_t, 3> &ticks,
                                            float time_scale) const noexcept {
  if (!valid()) {
    return std::nullopt;
  }
  const auto factor =
      config_.odometry_scale * 2 * pi * config_.wheel_radius_m /
      (config_.gear_ratio * static_cast<float>(config_.encoder_ticks_per_motor_turn) * time_scale);
  std::array<float, 3> travel{};
  for (std::size_t i = 0; i < 3; ++i) {
    travel[i] = static_cast<float>(ticks[i]) * factor * config_.wheel_odometry_scale[i];
  }
  Twist out{};
  if (config_.layout == OmniLayout::legacy_2026) {
    out.vx = (travel[0] - travel[2]) / (2 * cos30);
    out.vy = (-travel[0] + 2 * travel[1] - travel[2]) / 3;
    out.wz = -(travel[0] + travel[1] + travel[2]) / (3 * config_.center_to_wheel_m);
  } else {
    out.vx = (travel[0] - travel[1]) / (2 * cos30);
    out.vy = (travel[0] + travel[1] - 2 * travel[2]) / 3;
    out.wz = (travel[0] + travel[1] + travel[2]) / (3 * config_.center_to_wheel_m);
  }
  out.vx *= config_.odometry_x_scale;
  out.vy *= config_.odometry_y_scale;
  out.wz *= config_.angular_odometry_scale;
  return finite(out) ? std::optional<Twist>{out} : std::nullopt;
}
std::optional<Twist>
OmniKinematics::body_speed(const std::array<std::int32_t, 3> &tpm) const noexcept {
  return decode(tpm, 60);
}
std::optional<Twist>
OmniKinematics::body_delta(const std::array<std::int32_t, 3> &ticks) const noexcept {
  return decode(ticks, 1);
}
Chassis::Chassis(ChassisConfig config, WheelSink sink) noexcept
    : config_(config), sink_(sink), kinematics_(config.geometry) {}
bool Chassis::valid() const noexcept {
  return kinematics_.valid() && sink_.write && positive(config_.max_linear_mps) &&
         positive(config_.max_angular_radps) && positive(config_.max_accel_mps2) &&
         positive(config_.max_angular_accel_radps2) && config_.max_wheel_tpm > 0 &&
         config_.max_encoder_delta > 0 && interval_valid(config_.command_timeout_ms) &&
         interval_valid(config_.max_tick_ms);
}
Error Chassis::initialize(std::uint32_t now) noexcept {
  initialized_ = false;
  if (!valid()) {
    state_.fault = Error::invalid_config;
    return state_.fault;
  }
  last_tick_ = now;
  last_command_ = now;
  const auto result = stop();
  initialized_ = succeeded(result);
  return result;
}
Error Chassis::set(Twist request, std::uint32_t now) noexcept {
  if (!initialized_) {
    return Error::not_armed;
  }
  if (!finite(request)) {
    return Error::invalid_argument;
  }
  const auto magnitude = std::hypot(request.vx, request.vy);
  if (!std::isfinite(magnitude)) {
    return Error::invalid_argument;
  }
  if (magnitude > config_.max_linear_mps) {
    request.vx *= config_.max_linear_mps / magnitude;
    request.vy *= config_.max_linear_mps / magnitude;
  }
  request.wz = std::clamp(request.wz, -config_.max_angular_radps, config_.max_angular_radps);
  const auto wheels = kinematics_.wheel_tpm(request);
  if (!wheels) {
    return Error::invalid_argument;
  }
  double peak = 0;
  for (const auto value : *wheels) {
    peak = std::max(peak, std::fabs(static_cast<double>(value)));
  }
  if (peak > config_.max_wheel_tpm) {
    const auto scale = static_cast<float>(static_cast<double>(config_.max_wheel_tpm) / peak);
    request.vx *= scale;
    request.vy *= scale;
    request.wz *= scale;
  }
  state_.requested = request;
  last_command_ = now;
  return Error::ok;
}
Error Chassis::tick(std::uint32_t now) noexcept {
  if (!initialized_) {
    return Error::not_armed;
  }
  const auto dt_ms = static_cast<std::uint32_t>(now - last_tick_);
  if ((moving(state_.requested) || moving(state_.commanded)) &&
      elapsed(now, last_command_, config_.command_timeout_ms)) {
    const auto result = stop();
    return succeeded(result) ? Error::timeout : result;
  }
  if (dt_ms == 0) {
    return Error::ok;
  }
  if (dt_ms > config_.max_tick_ms) {
    const auto result = stop();
    initialized_ = false;
    return succeeded(result) ? Error::timeout : result;
  }
  last_tick_ = now;
  const float dt = static_cast<float>(dt_ms) * 0.001F;
  Twist next = state_.commanded;
  const float dx = state_.requested.vx - next.vx, dy = state_.requested.vy - next.vy;
  const auto distance = std::hypot(dx, dy), maximum = config_.max_accel_mps2 * dt;
  const auto angle_distance = std::fabs(state_.requested.wz - next.wz);
  const auto linear_scale = distance > maximum ? maximum / distance : 1.0F;
  const auto angular_maximum = config_.max_angular_accel_radps2 * dt;
  const auto angular_scale =
      angle_distance > angular_maximum ? angular_maximum / angle_distance : 1.0F;
  // One interpolation factor preserves the convex wheel limits as well as both acceleration bounds.
  const auto scale = std::min(linear_scale, angular_scale);
  next.vx += dx * scale;
  next.vy += dy * scale;
  next.wz += (state_.requested.wz - next.wz) * scale;
  auto wheels = kinematics_.wheel_tpm(next);
  if (!wheels) {
    const auto ignored = stop();
    (void)ignored;
    initialized_ = false;
    return Error::invalid_argument;
  }
  for (auto &value : *wheels) {
    value = std::clamp(value, -config_.max_wheel_tpm, config_.max_wheel_tpm);
  }
  const auto result = output_result(sink_.write(sink_.context, *wheels));
  state_.commanded_known = succeeded(result);
  state_.fault = result;
  if (succeeded(result)) {
    state_.commanded = next;
    state_.wheel_commanded = *wheels;
  } else {
    initialized_ = false;
  }
  return result;
}
Error Chassis::stop() noexcept {
  state_.requested = {};
  const std::array<std::int32_t, 3> zero{};
  const auto result =
      sink_.write ? output_result(sink_.write(sink_.context, zero)) : Error::output_failure;
  state_.commanded_known = succeeded(result);
  state_.fault = result;
  if (succeeded(result)) {
    state_.commanded = {};
    state_.wheel_commanded = {};
  }
  return result;
}
Error Chassis::reset_pose(Pose reference) noexcept {
  if (!std::isfinite(reference.x) || !std::isfinite(reference.y) || !std::isfinite(reference.yaw)) {
    return Error::invalid_argument;
  }
  reference.yaw = std::remainder(reference.yaw, 2 * pi);
  state_.pose = reference;
  state_.pose_known = true;
  return Error::ok;
}
Error Chassis::feedback(const std::array<std::int32_t, 3> &tpm,
                        const std::array<std::int32_t, 3> &deltas, std::uint8_t known) noexcept {
  if ((known & ~7U) != 0 || !valid()) {
    return Error::invalid_argument;
  }
  bool plausible = true;
  for (std::size_t i = 0; i < 3; ++i) {
    plausible = plausible &&
                std::fabs(static_cast<double>(deltas[i])) <= config_.max_encoder_delta &&
                std::fabs(static_cast<double>(tpm[i])) <= config_.max_wheel_tpm;
  }
  if (!plausible) {
    invalidate_feedback();
    return Error::invalid_argument;
  }
  state_.wheel_measured = tpm;
  state_.measured_known = known;
  if (known != 7) {
    state_.pose_known = false;
    return Error::ok;
  }
  const auto speed = kinematics_.body_speed(tpm), delta = kinematics_.body_delta(deltas);
  if (!speed || !delta) {
    invalidate_feedback();
    return Error::invalid_argument;
  }
  const auto midpoint = state_.pose.yaw + delta->wz * 0.5F;
  const auto x = state_.pose.x + delta->vx * std::cos(midpoint) - delta->vy * std::sin(midpoint);
  const auto y = state_.pose.y + delta->vx * std::sin(midpoint) + delta->vy * std::cos(midpoint);
  const auto yaw = std::remainder(state_.pose.yaw + delta->wz, 2 * pi);
  if (!std::isfinite(x) || !std::isfinite(y) || !std::isfinite(yaw)) {
    invalidate_feedback();
    return Error::invalid_argument;
  }
  state_.measured = *speed;
  state_.pose = {x, y, yaw};
  // A relative origin is defined by the first valid sample; failed samples invalidate its
  // certainty.
  return Error::ok;
}
bool Pid::valid() const noexcept {
  return std::isfinite(config_.kp) && std::isfinite(config_.ki) && std::isfinite(config_.kd) &&
         config_.kp >= 0 && config_.ki >= 0 && config_.kd >= 0 &&
         std::isfinite(config_.output_min) && std::isfinite(config_.output_max) &&
         config_.output_min < config_.output_max && std::isfinite(config_.integral_min) &&
         std::isfinite(config_.integral_max) && config_.integral_min <= 0 &&
         config_.integral_max >= 0 && config_.integral_min <= config_.integral_max;
}
std::optional<float> Pid::update(float target, float measured, float dt) noexcept {
  if (!valid() || !std::isfinite(target) || !std::isfinite(measured) || !positive(dt)) {
    return std::nullopt;
  }
  const auto error = target - measured;
  const auto derivative = seeded_ ? -(measured - previous_measurement_) / dt : 0;
  const auto integrated = integral_ + error * dt;
  if (!std::isfinite(integrated)) {
    return std::nullopt;
  }
  const auto candidate = std::clamp(integrated, config_.integral_min, config_.integral_max);
  const auto raw = config_.kp * error + config_.ki * candidate + config_.kd * derivative;
  if (!std::isfinite(error) || !std::isfinite(derivative) || !std::isfinite(raw)) {
    return std::nullopt;
  }
  // Conditional integration: permit unwinding, forbid integration deeper into saturation.
  if ((raw <= config_.output_max || error < 0) && (raw >= config_.output_min || error > 0)) {
    integral_ = candidate;
  }
  const auto output = config_.kp * error + config_.ki * integral_ + config_.kd * derivative;
  if (!std::isfinite(output)) {
    return std::nullopt;
  }
  previous_measurement_ = measured;
  seeded_ = true;
  return std::clamp(output, config_.output_min, config_.output_max);
}
Error Controller::stop_all() noexcept {
  const auto pumps = pumps_.stop(), chassis = chassis_.stop();
  const auto auxiliary = auxiliary_.stop ? auxiliary_.stop(auxiliary_.context) : Error::ok;
  return succeeded(pumps) && succeeded(chassis) && succeeded(auxiliary) ? Error::ok
                                                                        : Error::output_failure;
}
Error Controller::initialize(std::uint32_t now) noexcept {
  const bool latched_estop = state_.estop || physical_estop_;
  state_ = {};
  state_.estop = latched_estop;
  initialized_ = false;
  const auto stopped = stop_all();
  const auto p = pumps_.initialize(now), c = chassis_.initialize(now);
  initialized_ = succeeded(stopped) && succeeded(p) && succeeded(c);
  state_.fault = !initialized_;
  state_.last_error = !succeeded(stopped) ? stopped : (!succeeded(p) ? p : c);
  return state_.last_error;
}
bool Controller::lease_active(std::uint32_t now) const noexcept {
  return state_.owner != 0 && !elapsed(now, lease_renewed_, lease_duration_);
}
Error Controller::claim(std::uint32_t owner, std::uint32_t sequence, std::uint32_t lease_ms,
                        std::uint32_t now) noexcept {
  if (!initialized_) {
    return Error::invalid_config;
  }
  if (owner == 0 || !interval_valid(lease_ms)) {
    return Error::invalid_argument;
  }
  if (lease_active(now)) {
    return Error::lease_conflict;
  }
  if (owner == last_owner_) {
    return Error::stale_sequence;
  }
  if (state_.owner != 0) {
    state_.armed = false;
    if (!succeeded(stop_all())) {
      return fail(Error::output_failure);
    }
  }
  state_.armed = false;
  state_.owner = owner;
  last_owner_ = owner;
  last_sequence_ = sequence;
  lease_duration_ = lease_ms;
  lease_renewed_ = now;
  return Error::ok;
}
Error Controller::authorize(std::uint32_t owner, std::uint32_t sequence, std::uint32_t now,
                            bool require_armed) noexcept {
  if (!lease_active(now)) {
    const auto ignored = tick(now);
    (void)ignored;
    return Error::lease_expired;
  }
  if (owner == 0 || state_.owner != owner) {
    return Error::lease_conflict;
  }
  const auto difference = static_cast<std::uint32_t>(sequence - last_sequence_);
  if (difference == 0 || difference >= 0x80000000U) {
    return Error::stale_sequence;
  }
  last_sequence_ = sequence;
  if (require_armed && state_.estop) {
    return Error::estop;
  }
  if (require_armed && !state_.armed) {
    return Error::not_armed;
  }
  return Error::ok;
}
Error Controller::rearm(std::uint32_t owner, std::uint32_t sequence, std::uint32_t now) noexcept {
  const auto authorized = authorize(owner, sequence, now, false);
  if (!succeeded(authorized)) {
    return authorized;
  }
  if (physical_estop_) {
    return Error::estop;
  }
  state_.armed = false;
  const auto stopped = stop_all();
  const auto p = pumps_.initialize(now), c = chassis_.initialize(now);
  if (!succeeded(stopped) || !succeeded(p) || !succeeded(c)) {
    return fail(!succeeded(stopped) ? stopped : (!succeeded(p) ? p : c));
  }
  state_.estop = false;
  state_.fault = false;
  state_.armed = true;
  state_.last_error = Error::ok;
  return Error::ok;
}
Error Controller::heartbeat(std::uint32_t owner, std::uint32_t sequence,
                            std::uint32_t now) noexcept {
  const auto result = authorize(owner, sequence, now, false);
  if (succeeded(result)) {
    lease_renewed_ = now;
  }
  return result;
}
Error Controller::release(std::uint32_t owner, std::uint32_t sequence, std::uint32_t now) noexcept {
  const auto result = authorize(owner, sequence, now, false);
  if (!succeeded(result)) {
    return result;
  }
  state_.armed = false;
  state_.owner = 0;
  const auto stopped = stop_all();
  return succeeded(stopped) ? Error::ok : fail(stopped);
}
Error Controller::pumps(std::uint32_t owner, std::uint32_t sequence, std::uint32_t select,
                        std::uint32_t values, std::uint32_t valve_select,
                        std::uint32_t valve_values, std::uint32_t now) noexcept {
  const auto authorized = authorize(owner, sequence, now);
  if (!succeeded(authorized)) {
    return authorized;
  }
  const auto result = pumps_.set(select, values, valve_select, valve_values, now);
  return result == Error::output_failure ? fail(result) : result;
}
Error Controller::velocity(std::uint32_t owner, std::uint32_t sequence, Twist request,
                           std::uint32_t now) noexcept {
  const auto authorized = authorize(owner, sequence, now);
  if (!succeeded(authorized)) {
    return authorized;
  }
  return chassis_.set(request, now);
}
Error Controller::fail(Error error) noexcept {
  state_.armed = false;
  state_.fault = true;
  state_.last_error = error;
  const auto stopped = stop_all();
  if (!succeeded(stopped)) {
    state_.last_error = stopped;
  }
  return state_.last_error;
}
Error Controller::emergency_stop() noexcept {
  state_.estop = true;
  state_.armed = false;
  const auto stopped = stop_all();
  if (!succeeded(stopped)) {
    state_.fault = true;
  }
  state_.last_error = succeeded(stopped) ? Error::estop : stopped;
  return stopped;
}
void Controller::physical_estop(bool asserted) noexcept {
  const auto newly_asserted = asserted && (!physical_estop_ || !state_.estop);
  physical_estop_ = asserted;
  if (newly_asserted) {
    const auto ignored = emergency_stop();
    (void)ignored;
  }
}
Error Controller::tick(std::uint32_t now) noexcept {
  if (state_.owner != 0 && !lease_active(now)) {
    state_.owner = 0;
    return fail(Error::lease_expired);
  }
  if (!state_.armed) {
    return Error::ok;
  }
  const auto p = pumps_.tick(now), c = chassis_.tick(now);
  if (!succeeded(p) || !succeeded(c)) {
    return fail(!succeeded(p) ? p : c);
  }
  return Error::ok;
}
namespace wire {
std::uint32_t read_u32(const std::uint8_t *p) noexcept {
  return static_cast<std::uint32_t>(p[0]) | (static_cast<std::uint32_t>(p[1]) << 8) |
         (static_cast<std::uint32_t>(p[2]) << 16) | (static_cast<std::uint32_t>(p[3]) << 24);
}
void write_u32(std::uint8_t *p, std::uint32_t value) noexcept {
  for (unsigned i = 0; i < 4; ++i) {
    p[i] = static_cast<std::uint8_t>(value >> (8 * i));
  }
}
float read_f32(const std::uint8_t *p) noexcept { return std::bit_cast<float>(read_u32(p)); }
void write_f32(std::uint8_t *p, float value) noexcept {
  write_u32(p, std::bit_cast<std::uint32_t>(value));
}
std::uint16_t crc16(std::span<const std::uint8_t> bytes) noexcept {
  std::uint16_t crc = 0xffffU;
  for (const auto byte : bytes) {
    crc ^= static_cast<std::uint16_t>(byte) << 8;
    for (unsigned bit = 0; bit < 8; ++bit) {
      crc = static_cast<std::uint16_t>((crc & 0x8000U) ? (crc << 1) ^ 0x1021U : crc << 1);
    }
  }
  return crc;
}
std::size_t encode_frame(const Frame &frame, std::span<std::uint8_t> output) noexcept {
  const auto size = frame_overhead + frame.length;
  if (frame.length > max_payload || output.size() < size) {
    return 0;
  }
  output[0] = 0xa5;
  output[1] = 0x5a;
  output[2] = version;
  output[3] = frame.type;
  write_u32(output.data() + 4, frame.sequence);
  write_u32(output.data() + 8, frame.owner);
  output[12] = static_cast<std::uint8_t>(frame.length);
  output[13] = static_cast<std::uint8_t>(frame.length >> 8);
  std::copy_n(frame.payload.begin(), frame.length, output.begin() + 14);
  const auto crc = crc16(output.subspan(2, 12 + frame.length));
  output[14 + frame.length] = static_cast<std::uint8_t>(crc);
  output[15 + frame.length] = static_cast<std::uint8_t>(crc >> 8);
  return size;
}
std::optional<Frame> FrameParser::push(std::uint8_t byte, std::uint32_t now) noexcept {
  if (used_ != 0 && elapsed(now, last_byte_, timeout_ms_)) {
    reset();
    ++rejected_;
  }
  last_byte_ = now;
  if (used_ == 0) {
    if (byte == 0xa5) {
      bytes_[used_++] = byte;
    }
    return std::nullopt;
  }
  if (used_ == 1) {
    if (byte == 0x5a) {
      bytes_[used_++] = byte;
    } else if (byte != 0xa5) {
      reset();
    }
    return std::nullopt;
  }
  bytes_[used_++] = byte;
  if (used_ == 3 && bytes_[2] != version) {
    reset();
    ++rejected_;
    if (byte == 0xa5) {
      bytes_[used_++] = byte;
    }
    return std::nullopt;
  }
  if (used_ == 14) {
    const auto length =
        static_cast<std::uint16_t>(bytes_[12] | (static_cast<std::uint16_t>(bytes_[13]) << 8));
    if (length > max_payload) {
      reset();
      ++rejected_;
      return std::nullopt;
    }
    expected_ = length + frame_overhead;
  }
  if (expected_ == 0 || used_ != expected_) {
    return std::nullopt;
  }
  const auto length = expected_ - frame_overhead;
  const auto crc = static_cast<std::uint16_t>(
      bytes_[14 + length] | (static_cast<std::uint16_t>(bytes_[15 + length]) << 8));
  const auto actual = crc16(std::span<const std::uint8_t>{bytes_.data() + 2, 12 + length});
  reset();
  if (crc != actual) {
    ++rejected_;
    if (byte == 0xa5) {
      bytes_[used_++] = byte;
    }
    return std::nullopt;
  }
  Frame frame{};
  frame.type = bytes_[3];
  frame.sequence = read_u32(bytes_.data() + 4);
  frame.owner = read_u32(bytes_.data() + 8);
  frame.length = static_cast<std::uint16_t>(length);
  std::copy_n(bytes_.begin() + 14, length, frame.payload.begin());
  return frame;
}
Frame Dispatcher::dispatch(const Frame &request, std::uint32_t now) noexcept {
  // Expiration is processed before every request; status reads cannot preserve stale outputs.
  const auto prior_tick = controller_.tick(now);
  (void)prior_tick;
  if (request.type >= 0x21 && request.type <= 0x24) {
    Frame response{};
    response.type = request.type | 0x80U;
    response.sequence = request.sequence;
    response.owner = request.owner;
    std::size_t size = 0;
    bool mutation_executed = false;
    Error error = Error::malformed;
    if (request.type == 0x24 && request.length == 22) {
      const auto access = maintenance::access(request.payload[0]);
      if (access == maintenance::Access::unsupported) {
        error = Error::unsupported;
      } else if (access == maintenance::Access::read_only) {
        error = Error::ok;
      } else {
        error = controller_.authorize(request.owner, request.sequence, now,
                                      access == maintenance::Access::armed);
        if (error == Error::ok && access == maintenance::Access::disarmed &&
            controller_.state().armed) {
          error = Error::lease_conflict;
        }
        if (error == Error::ok && extension_.guard) {
          error = extension_.guard(extension_.context, request.type,
                                   {request.payload.data(), request.length});
        }
      }
    } else if (request.type != 0x24 && request.length <= max_payload) {
      error = controller_.authorize(request.owner, request.sequence, now, request.type == 0x21);
    }
    if (error == Error::ok) {
      mutation_executed = request.type == 0x24 && extension_.request &&
                          maintenance::access(request.payload[0]) != maintenance::Access::read_only;
      error = extension_.request
                  ? extension_.request(extension_.context, request.type, request.owner,
                                       {request.payload.data(), request.length},
                                       std::span{response.payload}.subspan(1), size, now)
                  : Error::unsupported;
    }
    if (size > max_payload - 1) {
      size = 0;
      error = Error::malformed;
    }
    if (mutation_executed && (error == Error::output_failure || error == Error::timeout ||
                              error == Error::malformed || error == Error::crc)) {
      // A bus error after a write can leave an energized partial result without
      // a command deadline. Stop every backend rather than retaining authority.
      error = controller_.fail(error);
    }
    response.payload[0] = static_cast<std::uint8_t>(error);
    response.length = static_cast<std::uint16_t>(size + 1);
    return response;
  }
  if (request.type == static_cast<std::uint8_t>(Type::sensors)) {
    Frame response{};
    response.type = request.type | 0x80U;
    response.sequence = request.sequence;
    response.owner = request.owner;
    std::size_t size = 0;
    auto error =
        request.length != 0
            ? Error::malformed
            : (extension_.read ? extension_.read(extension_.context,
                                                 std::span{response.payload}.subspan(1), size, now)
                               : Error::unsupported);
    if (size > max_payload - 1) {
      size = 0;
      error = Error::malformed;
    }
    response.payload[0] = static_cast<std::uint8_t>(error);
    response.length = static_cast<std::uint16_t>(size + 1);
    return response;
  }
  Error error = Error::malformed;
  const auto owner = request.owner, seq = request.sequence;
  const auto *data = request.payload.data();
  const bool guarded = request.type == static_cast<std::uint8_t>(Type::pumps) ||
                       request.type == static_cast<std::uint8_t>(Type::twist) ||
                       request.type == static_cast<std::uint8_t>(Type::actuator) ||
                       request.type == static_cast<std::uint8_t>(Type::rearm);
  const auto gate = guarded && extension_.guard
                        ? extension_.guard(extension_.context, request.type,
                                           {request.payload.data(), request.length})
                        : Error::ok;
  if (gate != Error::ok) {
    error = gate;
  } else if (request.length <= max_payload) {
    switch (static_cast<Type>(request.type)) {
    case Type::claim:
      if (request.length == 4) {
        error = controller_.claim(owner, seq, read_u32(data), now);
      }
      break;
    case Type::rearm:
      if (request.length == 0) {
        error = controller_.rearm(owner, seq, now);
      }
      break;
    case Type::heartbeat:
      if (request.length == 0) {
        error = controller_.heartbeat(owner, seq, now);
      }
      break;
    case Type::pumps:
      if (request.length == 16) {
        error = controller_.pumps(owner, seq, read_u32(data), read_u32(data + 4),
                                  read_u32(data + 8), read_u32(data + 12), now);
      }
      break;
    case Type::twist:
      if (request.length == 12) {
        error = controller_.velocity(owner, seq,
                                     {read_f32(data), read_f32(data + 4), read_f32(data + 8)}, now);
      }
      break;
    case Type::estop:
      if (request.length == 0) {
        error = controller_.emergency_stop();
      }
      break;
    case Type::release:
      if (request.length == 0) {
        error = controller_.release(owner, seq, now);
      }
      break;
    case Type::sensors:
      break; // handled by read-only extension above
    case Type::status:
      if (request.length == 0) {
        error = Error::ok;
      }
      break;
    case Type::actuator:
      error = controller_.authorize(owner, seq, now);
      if (succeeded(error)) {
        error = extension_.command
                    ? extension_.command(extension_.context, {data, request.length}, now)
                    : Error::unsupported;
        if (error == Error::output_failure || error == Error::timeout) {
          error = controller_.fail(error);
        }
      }
      break;
    default:
      error = Error::unsupported;
      break;
    }
  }
  Frame response{};
  response.type = request.type | 0x80U;
  response.owner = owner;
  response.sequence = seq;
  response.length = status_size;
  auto *out = response.payload.data();
  const auto &state = controller_.state();
  const auto &pump = controller_.pump_bank().state();
  const auto &chassis = controller_.chassis().state();
  out[0] = static_cast<std::uint8_t>(error);
  out[1] =
      static_cast<std::uint8_t>((state.armed ? 1 : 0) | (state.estop ? 2 : 0) |
                                (controller_.lease_active(now) ? 4 : 0) | (state.fault ? 8 : 0));
  out[2] = controller_.pump_bank().config().channels;
  out[3] = chassis.measured_known;
  write_u32(out + 4, state.owner);
  write_u32(out + 8, pump.requested);
  write_u32(out + 12, pump.commanded);
  write_u32(out + 16, pump.commanded_known);
  write_u32(out + 20, pump.measured_on);
  write_u32(out + 24, pump.measured_known);
  write_u32(out + 28, pump.valves_requested);
  write_u32(out + 32, pump.valves_commanded);
  write_u32(out + 36, pump.valves_commanded_known);
  for (std::size_t i = 0; i < 3; ++i) {
    write_u32(out + 40 + 4 * i, std::bit_cast<std::uint32_t>(chassis.wheel_commanded[i]));
    write_u32(out + 52 + 4 * i, std::bit_cast<std::uint32_t>(chassis.wheel_measured[i]));
  }
  write_f32(out + 64, chassis.pose.x);
  write_f32(out + 68, chassis.pose.y);
  write_f32(out + 72, chassis.pose.yaw);
  out[76] =
      static_cast<std::uint8_t>((chassis.measured_known == 7 ? 1 : 0) |
                                (chassis.pose_known ? 2 : 0) | (chassis.commanded_known ? 4 : 0));
  out[77] = static_cast<std::uint8_t>(state.last_error);
  out[78] = static_cast<std::uint8_t>(pump.fault);
  out[79] = static_cast<std::uint8_t>(chassis.fault);
  write_u32(out + 80, now);
  return response;
}
} // namespace wire
} // namespace eurobot
