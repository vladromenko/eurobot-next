#pragma once
#include "eurobot/core.hpp"
#include <array>
#include <chrono>
#include <cstdint>
#include <limits>
#include <optional>
#include <span>
#include <string>

namespace eurobot::ros {
using wire::encode_frame;
using wire::Frame;
using wire::FrameParser;
// Single-threaded ownership. ROS node uses a SingleThreadedExecutor.
class Transport {
public:
  Transport() = default;
  ~Transport();
  Transport(const Transport &) = delete;
  Transport &operator=(const Transport &) = delete;
  bool open(const std::string &path, unsigned baud);
  void close();
  bool connected() const { return fd_ >= 0; }
  std::optional<Frame> transact(const Frame &request, std::chrono::milliseconds timeout);
  const std::string &error() const { return error_; }

private:
  int fd_{-1};
  bool exclusive_owned_{false};
  FrameParser parser_{};
  std::string error_{"disconnected"};
};
enum class ArmDecision : std::uint8_t { acquire, idempotent, reject };
// Cooperative ROS client ownership; the public token is not a secret or authentication key.
class Authority {
public:
  ArmDecision decide_arm(std::uint32_t requested, bool armed,
                         std::size_t publisher_count = 0) const noexcept;
  bool grant(std::uint32_t token) noexcept;
  bool allows(std::uint32_t requested, bool armed) const noexcept;
  // Opaque RMW GID bytes, bounded without depending on a ROS runtime in core tests.
  // A changed or unavailable publisher immediately invalidates the authority.
  bool observe_publisher(std::span<const std::uint8_t> gid) noexcept;
  void reset() noexcept;
  std::uint32_t token() const noexcept { return token_; }

private:
  std::uint32_t token_{};
  std::array<std::uint8_t, 64> publisher_{};
  std::size_t publisher_size_{};
};
// Publisher stamps are required to be clock-synchronized within this permitted skew.
// After arm/stop, quarantine every stamp that could have been created before that boundary.
class MotionEpoch {
public:
  static constexpr std::int64_t permitted_future_ns = 50000000;
  void reset(std::int64_t local_now_ns) noexcept {
    const auto maximum = std::numeric_limits<std::int64_t>::max();
    not_before_ns_ =
        local_now_ns > maximum - permitted_future_ns ? maximum : local_now_ns + permitted_future_ns;
  }
  bool accepts(std::int64_t source_stamp_ns) const noexcept {
    return source_stamp_ns > not_before_ns_;
  }

private:
  std::int64_t not_before_ns_{};
};
struct Gate {
  bool armed{false};
  std::uint32_t faults{0};
  void fault() {
    armed = false;
    ++faults;
  }
};
// Payload contract for the single firmware gripper sequencer (not a second serial protocol).
struct GripperRequest {
  std::uint8_t gripper{}, operation{};
  float value{}, argument{}, speed{};
  std::uint32_t mask{}, timeout_ms{};
};
enum class GripperPhase : std::uint8_t { idle, running, completed, cancelled, faulted };
enum class CompletionEvidence : std::uint8_t {
  unknown,
  commanded,
  settled,
  referenced_steps,
  measured
};
struct GripperReply {
  std::uint8_t error{}, operation{}, gripper{}, step{}, count{}, commanded_steps{}, settled_steps{},
      referenced_steps{}, measured_steps{}, job_error{}, stop_error{};
  GripperPhase phase{GripperPhase::idle};
  CompletionEvidence evidence{CompletionEvidence::unknown};
  std::uint32_t job_id{}, owner{}, elapsed_ms{};
  bool snapshot_known{};
  bool terminal() const noexcept;
  bool fully_measured() const noexcept;
};
bool valid_gripper_request(const GripperRequest &request) noexcept;
std::size_t encode_gripper_request(const GripperRequest &request,
                                   std::span<std::uint8_t> output) noexcept;
std::optional<GripperReply> decode_gripper_reply(std::span<const std::uint8_t> payload) noexcept;
// Local motion/job exclusivity is reset only by a terminal result or explicit safe stop.
class GripperReservation {
public:
  bool reserve(bool session_valid, bool motion_idle) noexcept;
  void reset() noexcept { busy_ = false; }
  bool busy() const noexcept { return busy_; }

private:
  bool busy_{};
};
struct MaintenanceRequest {
  std::uint8_t operation{}, channel{};
  std::uint32_t argument{}, secondary{};
  std::array<float, 3> values{};
};
struct MaintenanceReply {
  std::uint8_t error{}, length{};
  std::array<std::uint8_t, 127> data{};
};
bool maintenance_read_only(std::uint8_t operation) noexcept;
bool valid_maintenance_request(const MaintenanceRequest &request) noexcept;
std::size_t encode_maintenance_request(const MaintenanceRequest &request,
                                       std::span<std::uint8_t> output) noexcept;
std::optional<MaintenanceReply> decode_maintenance_reply(std::uint8_t operation,
                                                         std::span<const std::uint8_t> payload,
                                                         std::uint8_t channel = 0,
                                                         std::uint32_t argument = 0) noexcept;
constexpr float unknown_measurement = std::numeric_limits<float>::quiet_NaN();
struct SensorSample {
  std::uint8_t error{10}, battery_error{10}, cells{};
  bool battery_known{};
  std::uint16_t levels{}, known_mask{}, raw_current_ma{};
  std::uint32_t raw_power_mw{};
  float voltage{unknown_measurement}, current{unknown_measurement}, percentage{unknown_measurement};
  std::array<float, 4> cell_voltage{unknown_measurement, unknown_measurement, unknown_measurement,
                                    unknown_measurement};
};
// current_sign=0 preserves unknown direction, +/-1 requires a reviewed convention.
std::optional<SensorSample> decode_sensors(std::span<const std::uint8_t> payload,
                                           int current_sign = 0);
// Shared bridge validation is tested without a ROS runtime. Firmware performs its own
// limits/configuration checks.
bool valid_actuator(std::uint8_t kind, std::uint8_t id, float value, float rate);
std::uint32_t monotonic_ms();
void put_u32(std::span<std::uint8_t> out, std::uint32_t value);
std::uint32_t get_u32(std::span<const std::uint8_t> in);
void put_float(std::span<std::uint8_t> out, float value);
} // namespace eurobot::ros
