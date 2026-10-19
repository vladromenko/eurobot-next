#include "eurobot_hardware/transport.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstring>
#include <fcntl.h>
#include <poll.h>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <termios.h>
#include <unistd.h>

namespace eurobot::ros {
using Clock = std::chrono::steady_clock;
ArmDecision Authority::decide_arm(std::uint32_t requested, bool armed,
                                  std::size_t publisher_count) const noexcept {
  if (publisher_count > 1)
    return ArmDecision::reject;
  if (armed)
    return token_ != 0 && requested == token_ ? ArmDecision::idempotent : ArmDecision::reject;
  return token_ == 0 && requested == 0 ? ArmDecision::acquire : ArmDecision::reject;
}
bool Authority::grant(std::uint32_t token) noexcept {
  if (token == 0 || token_ != 0)
    return false;
  token_ = token;
  publisher_size_ = 0;
  return true;
}
bool Authority::allows(std::uint32_t requested, bool armed) const noexcept {
  return armed && token_ != 0 && requested == token_;
}
void Authority::reset() noexcept {
  token_ = 0;
  publisher_size_ = 0;
  publisher_.fill(0);
}
bool Authority::observe_publisher(std::span<const std::uint8_t> gid) noexcept {
  const bool valid = token_ != 0 && !gid.empty() && gid.size() <= publisher_.size() &&
                     std::any_of(gid.begin(), gid.end(), [](auto byte) { return byte != 0; });
  if (!valid) {
    reset();
    return false;
  }
  if (publisher_size_ == 0) {
    std::copy(gid.begin(), gid.end(), publisher_.begin());
    publisher_size_ = gid.size();
    return true;
  }
  if (gid.size() != publisher_size_ || !std::equal(gid.begin(), gid.end(), publisher_.begin())) {
    reset();
    return false;
  }
  return true;
}
bool GripperReply::terminal() const noexcept {
  return phase == GripperPhase::completed || phase == GripperPhase::cancelled ||
         phase == GripperPhase::faulted;
}
bool GripperReply::fully_measured() const noexcept {
  return snapshot_known && phase == GripperPhase::completed && error == 0 && job_error == 0 &&
         stop_error == 0 && count > 0 && measured_steps == count &&
         evidence == CompletionEvidence::measured;
}
bool valid_gripper_request(const GripperRequest &request) noexcept {
  if (!std::isfinite(request.value) || !std::isfinite(request.argument) ||
      !std::isfinite(request.speed) || request.gripper > 1 || request.mask > 15 ||
      request.timeout_ms < 100 || request.timeout_ms > 60000)
    return false;
  if (request.operation == 6) {
    if (request.mask > 2 || request.speed != 0 || request.argument < 1 ||
        request.argument >= 60000 || std::trunc(request.argument) != request.argument)
      return false;
  } else if (request.operation == 8 || request.operation == 12) {
    if (request.speed <= 0)
      return false;
  } else if (request.argument != 0 || request.speed != 0)
    return false;
  if (request.operation == 9 && (request.value != 0 || request.mask > 2))
    return false;
  switch (request.operation) {
  case 0:
  case 1:
  case 2:
  case 3:
  case 4:
  case 5:
  case 6:
  case 7:
  case 8:
  case 9:
  case 10:
  case 11:
  case 12:
  case 13:
  case 20:
  case 21:
  case 22:
  case 23:
  case 24:
    return true;
  default:
    return false;
  }
}
std::size_t encode_gripper_request(const GripperRequest &request,
                                   std::span<std::uint8_t> output) noexcept {
  if (!valid_gripper_request(request) || output.size() < 22)
    return 0;
  output[0] = request.gripper;
  output[1] = request.operation;
  put_float(output.subspan(2, 4), request.value);
  put_u32(output.subspan(6, 4), request.mask);
  put_u32(output.subspan(10, 4), request.timeout_ms);
  put_float(output.subspan(14, 4), request.argument);
  put_float(output.subspan(18, 4), request.speed);
  return 22;
}
std::optional<GripperReply> decode_gripper_reply(std::span<const std::uint8_t> payload) noexcept {
  if (payload.empty() || payload[0] > 12) {
    return {};
  }
  if (payload.size() == 1 && payload[0] != 0) {
    GripperReply reply{};
    reply.error = payload[0];
    return reply;
  }
  if (payload.size() != 25)
    return {};
  GripperReply reply{};
  reply.error = payload[0];
  // A late CANCEL can return invalid_argument with the already-terminal snapshot.
  // Preserve that body only if all structural invariants validate; short NACKs stay unknown.
  const auto invalid_snapshot = [&]() -> std::optional<GripperReply> {
    return reply.error != 0 ? std::optional<GripperReply>{reply} : std::nullopt;
  };
  if (payload[1] > 4 || payload[3] > 1 || payload[5] > 64 || payload[4] > payload[5] ||
      payload[6] > 4 || payload[7] > payload[5] || payload[8] > payload[5] ||
      payload[9] > payload[5] || payload[10] > payload[5] || payload[11] > 12 || payload[12] > 12)
    return invalid_snapshot();
  const auto completed_steps =
      static_cast<unsigned>(payload[7]) + payload[8] + payload[9] + payload[10];
  if (completed_steps > payload[4] || (payload[1] == 1 && payload[4] >= payload[5]) ||
      (payload[1] == 2 &&
       (payload[5] == 0 || payload[4] != payload[5] || completed_steps != payload[5]))) {
    return invalid_snapshot();
  }
  reply.phase = static_cast<GripperPhase>(payload[1]);
  reply.operation = payload[2];
  reply.gripper = payload[3];
  reply.step = payload[4];
  reply.count = payload[5];
  reply.evidence = static_cast<CompletionEvidence>(payload[6]);
  reply.commanded_steps = payload[7];
  reply.settled_steps = payload[8];
  reply.referenced_steps = payload[9];
  reply.measured_steps = payload[10];
  reply.job_error = payload[11];
  reply.stop_error = payload[12];
  reply.job_id = get_u32(payload.subspan(13, 4));
  reply.owner = get_u32(payload.subspan(17, 4));
  reply.elapsed_ms = get_u32(payload.subspan(21, 4));
  if (reply.phase != GripperPhase::idle && (reply.job_id == 0 || reply.owner == 0)) {
    if (reply.error != 0) {
      GripperReply nack{};
      nack.error = reply.error;
      return nack;
    }
    return {};
  }
  reply.snapshot_known = true;
  return reply;
}
bool GripperReservation::reserve(bool session_valid, bool motion_idle) noexcept {
  if (busy_ || !session_valid || !motion_idle)
    return false;
  busy_ = true;
  return true;
}
bool maintenance_read_only(std::uint8_t operation) noexcept {
  switch (operation) {
  case 0:
  case 3:
  case 4:
  case 7:
  case 13:
  case 17:
  case 19:
  case 21:
  case 27:
  case 28:
  case 30:
  case 31:
  case 34:
  case 36:
  case 37:
    return true;
  default:
    return false;
  }
}
bool valid_maintenance_request(const MaintenanceRequest &request) noexcept {
  if (request.operation > 37 || !std::all_of(request.values.begin(), request.values.end(),
                                             [](float value) { return std::isfinite(value); }))
    return false;
  switch (request.operation) {
  case 37:
    return request.channel == 0 && request.argument == 0 && request.secondary == 0 &&
           request.values == std::array<float, 3>{};
  case 36:
    return request.channel > 0 && request.channel < 254 && request.argument == 0 &&
           request.secondary == 0 && request.values == std::array<float, 3>{};
  case 8:
  case 11:
  case 25:
  case 26:
    return request.argument <= 1;
  case 10:
    return request.argument <= 1023;
  case 12:
    return request.argument >= 1 && request.argument <= 253;
  case 14:
    return request.argument <= request.secondary;
  case 18:
    return request.argument > 0;
  case 20:
    return request.argument <= 65535;
  case 30:
    return request.channel <= 5;
  case 31:
    return request.channel <= 2;
  case 32:
    return request.argument <= 3;
  case 34:
  case 35:
    return request.channel < 4;
  default:
    return true;
  }
}
std::size_t encode_maintenance_request(const MaintenanceRequest &request,
                                       std::span<std::uint8_t> output) noexcept {
  if (!valid_maintenance_request(request) || output.size() < 22)
    return 0;
  output[0] = request.operation;
  output[1] = request.channel;
  put_u32(output.subspan(2, 4), request.argument);
  put_u32(output.subspan(6, 4), request.secondary);
  for (unsigned index = 0; index < 3; ++index)
    put_float(output.subspan(10 + index * 4, 4), request.values[index]);
  return 22;
}
std::optional<MaintenanceReply> decode_maintenance_reply(std::uint8_t operation,
                                                         std::span<const std::uint8_t> payload,
                                                         std::uint8_t channel,
                                                         std::uint32_t argument) noexcept {
  if (operation > 37 || payload.empty() || payload.size() > 128 || payload[0] > 12)
    return {};
  MaintenanceReply reply{};
  reply.error = payload[0];
  if (reply.error != 0)
    return operation == 36 && payload.size() != 1 ? std::nullopt : std::optional{reply};
  const auto size = payload.size() - 1;
  bool valid = false;
  switch (operation) {
  case 0:
    valid = size == 16;
    break;
  case 3:
  case 4:
  case 5:
    valid = size == 13;
    break;
  case 7:
    valid = size == 12;
    break;
  case 13:
  case 17:
  case 19:
    valid = size == 8;
    break;
  case 21:
    valid = size == 24;
    break;
  case 27:
    valid = size <= 96;
    break;
  case 28:
    valid = size == 9;
    break;
  case 30:
    valid =
        channel <= 5 &&
        size ==
            (channel == 0 ? 5U : (channel == 1 ? 9U : ((channel == 2 || channel == 3) ? 3U : 2U)));
    break;
  case 37:
    valid = size == 7 && payload[1] == 1 && payload[2] <= 1 && payload[3] <= 2 && payload[4] <= 1 &&
            payload[5] <= 1 && payload[6] >= 1 && payload[6] <= 32 && payload[7] <= payload[6];
    break;
  case 36:
    valid = size == 12 && channel > 0 && channel < 254 && payload[4] == channel &&
            payload[11] <= 1 && payload[12] <= 2;
    break;
  case 34:
    valid = size == (argument == 0 ? 2U : 4U);
    break;
  default:
    valid = size == 0;
    break;
  }
  if (!valid)
    return {};
  reply.length = static_cast<std::uint8_t>(size);
  std::copy(payload.begin() + 1, payload.end(), reply.data.begin());
  return reply;
}
std::optional<SensorSample> decode_sensors(std::span<const std::uint8_t> payload,
                                           int current_sign) {
  if (current_sign < -1 || current_sign > 1)
    return {};
  // Dispatcher can return a one-byte NACK when no sensor extension is installed.
  if (payload.size() == 1 && payload[0] != 0) {
    SensorSample unavailable{};
    unavailable.error = payload[0];
    return unavailable;
  }
  if (payload.size() != 25 || payload[1] > 1)
    return {};
  SensorSample result{};
  result.error = payload[0];
  result.battery_error = payload[24];
  const auto word = [payload](unsigned at) {
    return static_cast<std::uint16_t>(payload[at] |
                                      (static_cast<std::uint16_t>(payload[at + 1]) << 8));
  };
  if (result.error == 0) {
    result.levels = word(20);
    result.known_mask = word(22);
  }
  if (result.error == 0 && payload[1] != 0 && result.battery_error == 0) {
    if (payload[18] > 100 || payload[19] == 0 || payload[19] > 4) {
      result.battery_error = 11;
      return result;
    }
    result.battery_known = true;
    result.cells = payload[19];
    result.raw_power_mw = get_u32(payload.subspan(2, 4));
    result.raw_current_ma = word(16);
    result.voltage = static_cast<float>(word(14)) * 0.001F;
    result.percentage = static_cast<float>(payload[18]) * 0.01F;
    if (current_sign != 0)
      result.current =
          static_cast<float>(current_sign) * static_cast<float>(result.raw_current_ma) * 0.001F;
    for (unsigned i = 0; i < result.cells; ++i)
      result.cell_voltage[i] = static_cast<float>(word(6 + 2 * i)) * 0.001F;
  }
  return result;
}
bool valid_actuator(std::uint8_t kind, std::uint8_t id, float value, float rate) {
  if (!std::isfinite(value) || !std::isfinite(rate))
    return false;
  const bool integer = std::trunc(value) == value;
  switch (kind) {
  case 1:
  case 2:
    return id < 254 && rate == 0 && value >= 0; // Profile supplies exact model angle limit.
  case 3:
    return id < 16 && rate == 0 && value >= 0;
  case 4:
    return id < 8 && rate == 0 && value >= 0;
  case 5:
    return id < 4 && integer && value >= -2147483648.0F && value < 2147483648.0F && rate > 0;
  case 6:
    return id < 4 && rate == 0 && value >= -1 && value <= 1;
  case 7:
  case 8:
    return id < 254 && rate == 0 && (value == 0 || value == 1);
  case 9:
    return id < 60 && rate == 0 && integer && value >= 0 && value <= 16777215;
  case 10:
    return id == 0 && value == 0 && rate == 0;
  case 11:
    return id < 4 && (value == 0 || value == 1) && rate > 0 && std::trunc(rate) == rate;
  case 12:
    return id < 4 && integer && value >= -2147483648.0F && value < 2147483648.0F && rate == 0;
  case 13:
    return id < 4 && (value == 0 || value == 1) && rate == 0;
  case 14:
    return id < 8 && value >= 0 && value <= 1 && rate > 0 && rate < 4294967296.0F &&
           std::trunc(rate) == rate;
  case 15:
    return id < 8 && value == 0 && rate == 0;
  default:
    return false;
  }
}
std::uint32_t monotonic_ms() {
  return static_cast<std::uint32_t>(
      std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now().time_since_epoch())
          .count());
}
void put_u32(std::span<std::uint8_t> out, std::uint32_t value) {
  for (unsigned i = 0; i < 4; ++i)
    out[i] = static_cast<std::uint8_t>(value >> (8 * i));
}
std::uint32_t get_u32(std::span<const std::uint8_t> in) {
  std::uint32_t value = 0;
  for (unsigned i = 0; i < 4; ++i)
    value |= std::uint32_t(in[i]) << (8 * i);
  return value;
}
void put_float(std::span<std::uint8_t> out, float value) {
  put_u32(out, std::bit_cast<std::uint32_t>(value));
}
Transport::~Transport() { close(); }
void Transport::close() {
  if (fd_ >= 0) {
#ifdef TIOCNXCL
    if (exclusive_owned_)
      static_cast<void>(ioctl(fd_, TIOCNXCL));
#endif
    ::close(fd_);
    fd_ = -1;
    exclusive_owned_ = false;
  }
  parser_ = FrameParser{};
}
bool Transport::open(const std::string &path, unsigned baud) {
  close();
  speed_t speed{};
  switch (baud) {
  case 115200:
    speed = B115200;
    break;
#ifdef B230400
  case 230400:
    speed = B230400;
    break;
#endif
#ifdef B460800
  case 460800:
    speed = B460800;
    break;
#endif
#ifdef B921600
  case 921600:
    speed = B921600;
    break;
#endif
  default:
    error_ = "unsupported baud rate";
    return false;
  }
  fd_ = ::open(path.c_str(), O_RDWR | O_NOCTTY | O_NONBLOCK | O_CLOEXEC);
  if (fd_ < 0) {
    error_ = std::strerror(errno);
    return false;
  }
  if (flock(fd_, LOCK_EX | LOCK_NB) != 0) {
    error_ = "serial port is already owned";
    close();
    return false;
  }
  termios config{};
  if (tcgetattr(fd_, &config) != 0) {
    error_ = std::strerror(errno);
    close();
    return false;
  }
  cfmakeraw(&config);
  config.c_cflag |= CLOCAL | CREAD;
  config.c_cflag &= ~CSTOPB;
#ifdef CRTSCTS
  config.c_cflag &= ~CRTSCTS;
#endif
  config.c_cc[VMIN] = 0;
  config.c_cc[VTIME] = 0;
  if (cfsetispeed(&config, speed) != 0 || cfsetospeed(&config, speed) != 0 ||
      tcsetattr(fd_, TCSANOW, &config) != 0) {
    error_ = std::strerror(errno);
    close();
    return false;
  }
  if (tcflush(fd_, TCIOFLUSH) != 0) {
    error_ = std::strerror(errno);
    close();
    return false;
  }
#ifdef TIOCEXCL
  if (ioctl(fd_, TIOCEXCL) != 0) {
    error_ = "cannot obtain exclusive serial ownership";
    close();
    return false;
  }
  exclusive_owned_ = true;
#endif
  error_ = "connected, disarmed";
  return true;
}
std::optional<Frame> Transport::transact(const Frame &request, std::chrono::milliseconds timeout) {
  if (fd_ < 0 || timeout.count() <= 0 || timeout.count() > 1000) {
    error_ = "invalid transport state or timeout";
    return {};
  }
  std::array<std::uint8_t, 160> encoded{};
  const auto length = encode_frame(request, encoded);
  if (length == 0) {
    error_ = "invalid outgoing frame";
    return {};
  }
  const auto deadline = Clock::now() + timeout;
  std::size_t written = 0;
  while (Clock::now() < deadline) {
    const auto remaining =
        std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
    pollfd descriptor{fd_, static_cast<short>(written < length ? POLLOUT : POLLIN), 0};
    const int result = poll(&descriptor, 1, static_cast<int>(std::max<std::int64_t>(1, remaining)));
    if (result < 0 && errno != EINTR) {
      error_ = std::strerror(errno);
      close();
      return {};
    }
    if (result > 0 && (descriptor.revents & (POLLERR | POLLHUP | POLLNVAL))) {
      error_ = "serial device disconnected";
      close();
      return {};
    }
    if (result > 0 && (descriptor.revents & POLLOUT)) {
      const auto count = ::write(fd_, encoded.data() + written, length - written);
      if (count > 0)
        written += static_cast<std::size_t>(count);
      else if (count < 0 && errno != EAGAIN && errno != EINTR) {
        error_ = std::strerror(errno);
        close();
        return {};
      }
    }
    if (result > 0 && (descriptor.revents & POLLIN)) {
      std::array<std::uint8_t, 256> bytes{};
      const auto count = ::read(fd_, bytes.data(), bytes.size());
      if (count == 0) {
        error_ = "serial EOF";
        close();
        return {};
      }
      if (count < 0 && errno != EAGAIN && errno != EINTR) {
        error_ = std::strerror(errno);
        close();
        return {};
      }
      for (ssize_t i = 0; i < count; ++i) {
        const auto frame = parser_.push(bytes[static_cast<std::size_t>(i)], monotonic_ms());
        if (frame && frame->type == static_cast<std::uint8_t>(request.type | 0x80U) &&
            frame->sequence == request.sequence && frame->owner == request.owner) {
          error_ = "ACK received";
          return frame;
        }
      }
    }
  }
  error_ = "controller ACK deadline exceeded; outcome unknown";
  close();
  return {};
}
} // namespace eurobot::ros
