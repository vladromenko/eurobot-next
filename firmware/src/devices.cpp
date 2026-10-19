#include "eurobot/devices.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
namespace eurobot::devices {
namespace {
bool timeout_valid(std::uint32_t value) noexcept { return value > 0 && value < 0x80000000U; }
std::uint16_t u16(const std::uint8_t *value) noexcept {
  return static_cast<std::uint16_t>(value[0] | static_cast<std::uint16_t>(value[1]) << 8);
}
void put16(std::uint8_t *output, std::uint16_t value) noexcept {
  output[0] = static_cast<std::uint8_t>(value);
  output[1] = static_cast<std::uint8_t>(value >> 8);
}
Error signed_read(DxlProtocol1 &bus, std::uint8_t id, std::uint8_t address,
                  std::int32_t &output) noexcept {
  std::array<std::uint8_t, 4> data{};
  const auto error = bus.read(id, address, data);
  if (succeeded(error)) {
    output = std::bit_cast<std::int32_t>(wire::read_u32(data.data()));
  }
  return error;
}
Error signed_write(DxlProtocol1 &bus, std::uint8_t id, std::uint8_t address,
                   std::int32_t value) noexcept {
  std::array<std::uint8_t, 4> data{};
  wire::write_u32(data.data(), std::bit_cast<std::uint32_t>(value));
  return bus.write(id, address, data);
}
std::int16_t sign_magnitude(std::uint16_t value) noexcept {
  const auto magnitude = static_cast<std::int16_t>(value & 0x3ffU);
  return (value & 0x400U) != 0 ? static_cast<std::int16_t>(-magnitude) : magnitude;
}
struct Register {
  std::uint8_t bytes{};
  std::uint32_t mask{};
  bool writable{};
};
Register l6474_register(std::uint8_t address) noexcept {
  switch (address) {
  case 0x01:
  case 0x03:
    return {3, 0x3fffff, true};
  case 0x02:
    return {2, 0x1ff, true};
  case 0x09:
  case 0x0f:
  case 0x10:
    return {1, 0x7f, true};
  case 0x0e:
    return {1, 0xff, true};
  case 0x12:
    return {1, 0x3f, false};
  case 0x13:
    return {1, 0xf, true};
  case 0x16:
    return {1, 0xff, true};
  case 0x17:
    return {1, 0xcf, true};
  case 0x18:
    return {2, 0x7faf, true};
  case 0x19:
    return {2, 0xffff, false};
  default:
    return {};
  }
}
} // namespace
Error DxlProtocol1::transact(std::uint8_t id, std::uint8_t instruction,
                             std::span<const std::uint8_t> parameters,
                             std::span<std::uint8_t> output, std::size_t &received,
                             bool expect_reply) noexcept {
  received = 0;
  device_error_ = 0;
  if (!bus_.exchange || !timeout_valid(timeout_us_)) {
    return Error::invalid_config;
  }
  if (id > 254 || (expect_reply && id == 254) || parameters.size() > 64 || output.size() > 64) {
    return Error::invalid_argument;
  }
  std::array<std::uint8_t, 70> tx{}, rx{};
  const auto length = parameters.size() + 2;
  tx[0] = 0xff;
  tx[1] = 0xff;
  tx[2] = id;
  tx[3] = static_cast<std::uint8_t>(length);
  tx[4] = instruction;
  std::copy(parameters.begin(), parameters.end(), tx.begin() + 5);
  unsigned sum = id + length + instruction;
  for (const auto value : parameters) {
    sum += value;
  }
  tx[5 + parameters.size()] = static_cast<std::uint8_t>(~sum);
  std::size_t actual = 0;
  const auto error = bus_.exchange(
      bus_.context, {tx.data(), parameters.size() + 6},
      expect_reply ? std::span<std::uint8_t>{rx} : std::span<std::uint8_t>{}, actual, timeout_us_);
  if (!succeeded(error)) {
    return error;
  }
  if (!expect_reply) {
    return Error::ok;
  }
  if (actual < 6 || actual > rx.size() || rx[0] != 0xff || rx[1] != 0xff || rx[2] != id ||
      rx[3] < 2 || static_cast<std::size_t>(rx[3]) + 4 != actual ||
      static_cast<std::size_t>(rx[3] - 2) > output.size()) {
    return Error::malformed;
  }
  sum = 0;
  for (std::size_t i = 2; i < actual; ++i) {
    sum += rx[i];
  }
  if ((sum & 0xffU) != 0xffU) {
    return Error::crc;
  }
  device_error_ = rx[4];
  if (device_error_ != 0) {
    return Error::output_failure;
  }
  received = rx[3] - 2;
  std::copy_n(rx.begin() + 5, received, output.begin());
  return Error::ok;
}
Error DxlProtocol1::ping(std::uint8_t id) noexcept {
  std::size_t count = 0;
  return transact(id, 1, {}, {}, count);
}
Error DxlProtocol1::read(std::uint8_t id, std::uint8_t address,
                         std::span<std::uint8_t> output) noexcept {
  if (output.empty() || output.size() > 64 || output.size() + address > 256) {
    return Error::invalid_argument;
  }
  const std::array<std::uint8_t, 2> parameters{address, static_cast<std::uint8_t>(output.size())};
  std::array<std::uint8_t, 64> temporary{};
  std::size_t count = 0;
  const auto error = transact(id, 2, parameters, std::span{temporary}.first(output.size()), count);
  if (!succeeded(error)) {
    return error;
  }
  if (count != output.size()) {
    return Error::malformed;
  }
  std::copy_n(temporary.begin(), output.size(), output.begin());
  return Error::ok;
}
Error DxlProtocol1::write(std::uint8_t id, std::uint8_t address,
                          std::span<const std::uint8_t> values, bool acknowledge) noexcept {
  if (values.empty() || values.size() > 63 || values.size() + address > 256) {
    return Error::invalid_argument;
  }
  std::array<std::uint8_t, 64> parameters{};
  parameters[0] = address;
  std::copy(values.begin(), values.end(), parameters.begin() + 1);
  std::size_t count = 0;
  return transact(id, 3, std::span{parameters}.first(values.size() + 1), {}, count, acknowledge);
}
Error DxlProtocol1::sync_write(std::uint8_t address, std::uint8_t width,
                               std::span<const std::uint8_t> ids,
                               std::span<const std::uint8_t> values) noexcept {
  if (ids.empty() || width == 0 || width > 62 ||
      ids.size() > 62U / (static_cast<std::size_t>(width) + 1) ||
      values.size() != ids.size() * width || static_cast<unsigned>(address) + width > 256) {
    return Error::invalid_argument;
  }
  std::array<std::uint8_t, 64> parameters{};
  parameters[0] = address;
  parameters[1] = width;
  for (std::size_t i = 0; i < ids.size(); ++i) {
    if (ids[i] > 253) {
      return Error::invalid_argument;
    }
    for (std::size_t j = 0; j < i; ++j) {
      if (ids[j] == ids[i]) {
        return Error::invalid_argument;
      }
    }
    const auto offset = 2 + i * (width + 1);
    parameters[offset] = ids[i];
    std::copy_n(values.begin() + static_cast<std::ptrdiff_t>(i * width), width,
                parameters.begin() + static_cast<std::ptrdiff_t>(offset + 1));
  }
  std::size_t count = 0;
  return transact(254, 0x83, std::span{parameters}.first(2 + ids.size() * (width + 1)), {}, count,
                  false);
}
Error Maxon::set_id(std::uint8_t id) noexcept {
  if (id > 253) {
    return Error::invalid_argument;
  }
  const std::array<std::uint8_t, 1> data{id};
  const auto error = protocol_.write(id_, 1, data);
  if (succeeded(error)) {
    id_ = id;
  }
  return error;
}
Error Maxon::set_tpm_range(std::uint32_t lower, std::uint32_t upper) noexcept {
  if (lower > upper) {
    return Error::invalid_argument;
  }
  std::array<std::uint8_t, 8> data{};
  wire::write_u32(data.data(), lower);
  wire::write_u32(data.data() + 4, upper);
  return protocol_.write(id_, 4, data);
}
Error Maxon::enable(bool enabled) noexcept {
  const std::array<std::uint8_t, 1> data{static_cast<std::uint8_t>(enabled)};
  return protocol_.write(id_, 0x18, data);
}
Error Maxon::set_tpm(std::int32_t value) noexcept {
  return signed_write(protocol_, id_, 0x10, value);
}
Error Maxon::get_tpm(std::int32_t &value) noexcept {
  return signed_read(protocol_, id_, 0x10, value);
}
Error Maxon::total_ticks(std::int32_t &value) noexcept {
  return signed_read(protocol_, id_, 0x14, value);
}
Error Maxon::set_total_ticks(std::int32_t value) noexcept {
  return signed_write(protocol_, id_, 0x14, value);
}
Error Maxon::tpm_range(std::uint32_t &lower, std::uint32_t &upper) noexcept {
  std::array<std::uint8_t, 8> data{};
  const auto error = protocol_.read(id_, 0x04, data);
  if (succeeded(error)) {
    lower = wire::read_u32(data.data());
    upper = wire::read_u32(data.data() + 4);
  }
  return error;
}
Error DynamixelServo::set_id(std::uint8_t id) noexcept {
  if (id > 253) {
    return Error::invalid_argument;
  }
  const std::array<std::uint8_t, 1> data{id};
  const auto error = protocol_.write(id_, 3, data);
  if (succeeded(error)) {
    id_ = id;
  }
  return error;
}
Error DynamixelServo::wheel_mode(bool enabled) noexcept {
  if (config_.model != ServoModel::ax_300 && config_.model != ServoModel::mx_360) {
    return Error::invalid_config;
  }
  std::array<std::uint8_t, 4> limits{};
  if (!enabled) {
    put16(limits.data() + 2, config_.model == ServoModel::ax_300 ? 1023 : 4095);
  }
  return protocol_.write(id_, 6, limits);
}
Error DynamixelServo::led(bool enabled) noexcept {
  const std::array<std::uint8_t, 1> data{static_cast<std::uint8_t>(enabled)};
  return protocol_.write(id_, 25, data);
}
Error DynamixelServo::ping() noexcept { return protocol_.ping(id_); }
Error DynamixelServo::present_position_deg(float &output) noexcept {
  if (config_.model != ServoModel::ax_300 && config_.model != ServoModel::mx_360) {
    return Error::invalid_config;
  }
  std::array<std::uint8_t, 2> bytes{};
  const auto error = protocol_.read(id_, 36, bytes);
  if (error != Error::ok) {
    return error;
  }
  const auto maximum = config_.model == ServoModel::ax_300 ? 1023U : 4095U;
  if (u16(bytes.data()) > maximum) {
    return Error::malformed;
  }
  output = static_cast<float>(u16(bytes.data())) *
           (config_.model == ServoModel::ax_300 ? 300.0F : 360.0F) / static_cast<float>(maximum);
  return Error::ok;
}
namespace {
Error servo_signed_read(DxlProtocol1 &protocol, std::uint8_t id, std::uint8_t address,
                        std::int16_t &output) noexcept {
  std::array<std::uint8_t, 2> bytes{};
  const auto error = protocol.read(id, address, bytes);
  if (error != Error::ok) {
    return error;
  }
  const auto raw = u16(bytes.data());
  if (raw > 2047) {
    return Error::malformed;
  }
  output = sign_magnitude(raw);
  return Error::ok;
}
} // namespace
Error DynamixelServo::moving_speed_raw(std::int16_t &output) noexcept {
  return servo_signed_read(protocol_, id_, 32, output);
}
Error DynamixelServo::present_speed_raw(std::int16_t &output) noexcept {
  return servo_signed_read(protocol_, id_, 38, output);
}
Error DynamixelServo::present_load_raw(std::int16_t &output) noexcept {
  return servo_signed_read(protocol_, id_, 40, output);
}
Error DynamixelServo::position_deg(float degrees) noexcept {
  const bool ax = config_.model == ServoModel::ax_300;
  if (!ax && config_.model != ServoModel::mx_360) {
    return Error::invalid_config;
  }
  const auto maximum = ax ? 300.0F : 360.0F;
  if (!std::isfinite(config_.minimum_deg) || !std::isfinite(config_.maximum_deg) ||
      config_.minimum_deg < 0 || config_.maximum_deg > maximum ||
      config_.minimum_deg > config_.maximum_deg) {
    return Error::invalid_config;
  }
  if (!std::isfinite(degrees) || degrees < config_.minimum_deg || degrees > config_.maximum_deg) {
    return Error::invalid_argument;
  }
  const auto raw =
      static_cast<std::uint16_t>(std::lround(degrees * (ax ? 1023.0F : 4095.0F) / maximum));
  std::array<std::uint8_t, 2> data{};
  put16(data.data(), raw);
  return protocol_.write(id_, 30, data);
}
Error DynamixelServo::speed_raw(std::int16_t speed) noexcept {
  if (speed < -1023 || speed > 1023) {
    return Error::invalid_argument;
  }
  const auto raw = static_cast<std::uint16_t>(speed < 0 ? -speed + 1024 : speed);
  std::array<std::uint8_t, 2> data{};
  put16(data.data(), raw);
  return protocol_.write(id_, 32, data);
}
Error DynamixelServo::torque(bool enabled) noexcept {
  const std::array<std::uint8_t, 1> data{static_cast<std::uint8_t>(enabled)};
  return protocol_.write(id_, 24, data);
}
Error DynamixelServo::torque_limit(std::uint16_t raw) noexcept {
  if (raw > 1023) {
    return Error::invalid_argument;
  }
  std::array<std::uint8_t, 2> data{};
  put16(data.data(), raw);
  return protocol_.write(id_, 34, data);
}
Error DynamixelServo::feedback(ServoFeedback &output) noexcept {
  if (config_.model != ServoModel::ax_300 && config_.model != ServoModel::mx_360) {
    return Error::invalid_config;
  }
  std::array<std::uint8_t, 11> bytes{};
  const auto error = protocol_.read(id_, 36, bytes);
  if (!succeeded(error)) {
    return error;
  }
  const auto maximum = config_.model == ServoModel::ax_300 ? 1023U : 4095U;
  if (u16(bytes.data()) > maximum || u16(bytes.data() + 2) > 2047 || u16(bytes.data() + 4) > 2047 ||
      bytes[10] > 1) {
    return Error::malformed;
  }
  ServoFeedback result{};
  result.position_deg = static_cast<float>(u16(bytes.data())) *
                        (config_.model == ServoModel::ax_300 ? 300.0F : 360.0F) /
                        static_cast<float>(maximum);
  result.speed_raw = sign_magnitude(u16(bytes.data() + 2));
  result.load_raw = sign_magnitude(u16(bytes.data() + 4));
  result.voltage_decivolts = bytes[6];
  result.temperature_c = bytes[7];
  result.moving = bytes[10] != 0;
  output = result;
  return Error::ok;
}
Error decode_battery(std::span<const std::uint8_t> bytes, BatteryState &output) noexcept {
  if (bytes.size() != 18 || bytes[16] > 100 || bytes[17] == 0 || bytes[17] > 4) {
    return Error::malformed;
  }
  BatteryState result{};
  result.power_mw = wire::read_u32(bytes.data());
  for (unsigned i = 0; i < 4; ++i) {
    result.cell_mv[i] = u16(bytes.data() + 4 + 2 * i);
  }
  result.voltage_mv = u16(bytes.data() + 12);
  result.current_ma = u16(bytes.data() + 14);
  result.soc_percent = bytes[16];
  result.cells = bytes[17];
  output = result;
  return Error::ok;
}
Error read_battery(DxlProtocol1 &protocol, std::uint8_t id, BatteryState &output) noexcept {
  std::array<std::uint8_t, 18> bytes{};
  const auto error = protocol.read(id, 0x04, bytes);
  return succeeded(error) ? decode_battery(bytes, output) : error;
}
Error calibrate_battery(DxlProtocol1 &protocol, std::uint8_t id, std::uint8_t address,
                        std::uint16_t value) noexcept {
  if (address < 0x16 || address > 0x20 || (address & 1U) != 0) {
    return Error::invalid_argument;
  }
  std::array<std::uint8_t, 2> data{};
  put16(data.data(), value);
  return protocol.write(id, address, data, true);
}
Error set_battery_id(DxlProtocol1 &protocol, std::uint8_t current_id,
                     std::uint8_t new_id) noexcept {
  if (current_id > 253 || new_id > 253) {
    return Error::invalid_argument;
  }
  const std::array<std::uint8_t, 1> value{new_id};
  return protocol.write(current_id, 0x01, value);
}
Error read_battery_cells(DxlProtocol1 &protocol, std::uint8_t id,
                         std::array<std::uint16_t, 4> &millivolts) noexcept {
  std::array<std::uint8_t, 8> bytes{};
  const auto error = protocol.read(id, 0x08, bytes);
  if (error == Error::ok) {
    for (std::size_t i = 0; i < millivolts.size(); ++i) {
      millivolts[i] = u16(bytes.data() + 2 * i);
    }
  }
  return error;
}
namespace {
Error battery_word(DxlProtocol1 &protocol, std::uint8_t id, std::uint8_t address,
                   std::uint16_t &output) noexcept {
  std::array<std::uint8_t, 2> bytes{};
  const auto error = protocol.read(id, address, bytes);
  if (error == Error::ok) {
    output = u16(bytes.data());
  }
  return error;
}
Error battery_byte(DxlProtocol1 &protocol, std::uint8_t id, std::uint8_t address,
                   std::uint8_t &output, std::uint8_t minimum, std::uint8_t maximum) noexcept {
  std::array<std::uint8_t, 1> bytes{};
  const auto error = protocol.read(id, address, bytes);
  if (error != Error::ok) {
    return error;
  }
  if (bytes[0] < minimum || bytes[0] > maximum) {
    return Error::malformed;
  }
  output = bytes[0];
  return Error::ok;
}
} // namespace
Error read_battery_voltage(DxlProtocol1 &protocol, std::uint8_t id,
                           std::uint16_t &millivolts) noexcept {
  return battery_word(protocol, id, 0x10, millivolts);
}
Error read_battery_current(DxlProtocol1 &protocol, std::uint8_t id,
                           std::uint16_t &milliamps) noexcept {
  return battery_word(protocol, id, 0x12, milliamps);
}
Error read_battery_power(DxlProtocol1 &protocol, std::uint8_t id,
                         std::uint32_t &milliwatts) noexcept {
  std::array<std::uint8_t, 4> bytes{};
  const auto error = protocol.read(id, 0x04, bytes);
  if (error == Error::ok) {
    milliwatts = wire::read_u32(bytes.data());
  }
  return error;
}
Error read_battery_soc(DxlProtocol1 &protocol, std::uint8_t id, std::uint8_t &percent) noexcept {
  return battery_byte(protocol, id, 0x14, percent, 0, 100);
}
Error read_battery_cell_count(DxlProtocol1 &protocol, std::uint8_t id,
                              std::uint8_t &cells) noexcept {
  return battery_byte(protocol, id, 0x15, cells, 1, 4);
}
std::optional<std::uint16_t> servo_pulse(float degrees, PwmCalibration config) noexcept {
  if (!std::isfinite(degrees) || !std::isfinite(config.minimum_deg) ||
      !std::isfinite(config.maximum_deg) || config.minimum_deg >= config.maximum_deg ||
      config.minimum_us == 0 || config.minimum_us >= config.maximum_us ||
      degrees < config.minimum_deg || degrees > config.maximum_deg) {
    return std::nullopt;
  }
  const auto fraction = (static_cast<double>(degrees) - config.minimum_deg) /
                        (static_cast<double>(config.maximum_deg) - config.minimum_deg);
  const auto pulse = config.minimum_us + fraction * (config.maximum_us - config.minimum_us);
  if (!std::isfinite(pulse) || pulse < 1 || pulse > 65535) {
    return std::nullopt;
  }
  return static_cast<std::uint16_t>(std::lround(pulse));
}
Error Pca9685::write(std::uint8_t reg, std::span<const std::uint8_t> bytes) noexcept {
  if (!bus_.write || address_ < 8 || address_ > 0x77 || !timeout_valid(timeout_us_)) {
    return Error::invalid_config;
  }
  const auto result = bus_.write(bus_.context, address_, reg, bytes, timeout_us_);
  if (!succeeded(result)) {
    initialized_ = false;
  }
  return result;
}
Error Pca9685::initialize(float frequency_hz, float oscillator_hz) noexcept {
  initialized_ = false;
  if (!bus_.read || !bus_.write || !bus_.delay_us || !std::isfinite(frequency_hz) ||
      !std::isfinite(oscillator_hz) || frequency_hz <= 0 || oscillator_hz <= 0 ||
      !timeout_valid(timeout_us_) || address_ < 8 || address_ > 0x77) {
    return Error::invalid_config;
  }
  const auto prescale = static_cast<double>(oscillator_hz) / (4096.0 * frequency_hz) - 1;
  if (!std::isfinite(prescale) || prescale < 3 || prescale > 255) {
    return Error::invalid_argument;
  }
  const auto divisor = static_cast<std::uint8_t>(std::lround(prescale));
  std::array<std::uint8_t, 1> old{};
  auto error = bus_.read(bus_.context, address_, 0, old, timeout_us_);
  if (!succeeded(error)) {
    return error;
  }
  error = full_off();
  if (!succeeded(error)) {
    return error;
  }
  if ((old[0] & 0x40U) != 0) {
    return Error::invalid_config;
  } // EXTCLK needs power-cycle or software reset.
  // Internal oscillator only; preserve output-addressing bits, explicitly clear
  // EXTCLK/RESTART.
  const auto base = static_cast<std::uint8_t>((old[0] & 0x0fU) | 0x20U);
  const std::array<std::uint8_t, 1> sleep{static_cast<std::uint8_t>(base | 0x10U)}, scale{divisor},
      mode2{4}, wake{base}, restart{static_cast<std::uint8_t>(base | 0x80U)};
  error = write(0, sleep);
  if (!succeeded(error)) {
    return error;
  }
  error = write(0xfe, scale);
  if (!succeeded(error)) {
    return error;
  }
  error = write(1, mode2);
  if (!succeeded(error)) {
    return error;
  }
  error = write(0, wake);
  if (!succeeded(error)) {
    return error;
  }
  error = bus_.delay_us(bus_.context, 500);
  if (!succeeded(error)) {
    return error;
  }
  error = write(0, restart);
  if (!succeeded(error)) {
    return error;
  }
  sleeping_ = false;
  frequency_ = oscillator_hz / (4096.0F * (static_cast<float>(divisor) + 1));
  initialized_ = true;
  return Error::ok;
}
Error Pca9685::full_off() noexcept {
  // ALL_LED_OFF_H fills bit4 of every LEDn_OFF_H, including when auto-increment
  // is disabled.
  const std::array<std::uint8_t, 1> off{0x10};
  return write(0xfd, off);
}
Error Pca9685::sleep() noexcept {
  if (!initialized_) {
    return Error::not_armed;
  }
  auto error = full_off();
  if (!succeeded(error)) {
    return error;
  }
  std::array<std::uint8_t, 1> mode{};
  error = bus_.read(bus_.context, address_, 0, mode, timeout_us_);
  if (!succeeded(error)) {
    initialized_ = false;
    return error;
  }
  mode[0] = static_cast<std::uint8_t>((mode[0] & 0x7fU) | 0x10U);
  error = write(0, mode);
  if (succeeded(error)) {
    sleeping_ = true;
  }
  return error;
}
Error Pca9685::wakeup() noexcept {
  if (!initialized_) {
    return Error::not_armed;
  }
  std::array<std::uint8_t, 1> mode{};
  auto error = bus_.read(bus_.context, address_, 0, mode, timeout_us_);
  if (!succeeded(error)) {
    initialized_ = false;
    return error;
  }
  if ((mode[0] & 0x40U) != 0) {
    return Error::invalid_config;
  }
  mode[0] = static_cast<std::uint8_t>((mode[0] & 0x0fU) | 0x20U);
  error = write(0, mode);
  if (!succeeded(error)) {
    return error;
  }
  error = bus_.delay_us(bus_.context, 500);
  if (!succeeded(error)) {
    initialized_ = false;
    return error;
  }
  mode[0] |= 0x80U;
  error = write(0, mode);
  if (succeeded(error)) {
    sleeping_ = false;
  }
  return error;
}
Error Pca9685::all_pulse_us(std::uint16_t pulse) noexcept {
  if (!initialized_ || sleeping_) {
    return Error::not_armed;
  }
  const auto count = static_cast<double>(pulse) * frequency_ * 4096 / 1000000;
  if (pulse == 0 || !std::isfinite(count) || count < 1 || count > 4095) {
    return Error::invalid_argument;
  }
  std::array<std::uint8_t, 4> data{};
  put16(data.data() + 2, static_cast<std::uint16_t>(std::lround(count)));
  return write(0xfa, data);
}
Error Pca9685::set_ticks(std::uint8_t channel, std::uint16_t on, std::uint16_t off) noexcept {
  if (!initialized_ || sleeping_) {
    return Error::not_armed;
  }
  if (channel > 15 || on > 4096 || off > 4096 || on == off) {
    return Error::invalid_argument;
  }
  std::array<std::uint8_t, 4> data{};
  put16(data.data(), on);
  put16(data.data() + 2, off);
  return write(static_cast<std::uint8_t>(6 + 4 * channel), data);
}
Error Pca9685::pulse_us(std::uint8_t channel, std::uint16_t pulse) noexcept {
  if (!initialized_) {
    return Error::not_armed;
  }
  const auto count = static_cast<double>(pulse) * frequency_ * 4096 / 1000000;
  if (pulse == 0 || !std::isfinite(count) || count < 1 || count > 4095) {
    return Error::invalid_argument;
  }
  return set_ticks(channel, 0, static_cast<std::uint16_t>(std::lround(count)));
}
Error Pca9685::position_deg(std::uint8_t channel, float degrees,
                            PwmCalibration calibration) noexcept {
  const auto pulse = servo_pulse(degrees, calibration);
  return pulse ? pulse_us(channel, *pulse) : Error::invalid_argument;
}
Error HBridge::stop(bool brake) noexcept {
  if (!sink_.write) {
    known_ = false;
    return Error::invalid_config;
  }
  const auto result = sink_.write(sink_.context, 0, brake);
  known_ = succeeded(result);
  if (known_) {
    previous_ = 0;
  }
  return result;
}
Error HBridge::drive(float duty) noexcept {
  if (!std::isfinite(duty) || duty < -1 || duty > 1) {
    return Error::invalid_argument;
  }
  if (!sink_.write) {
    return Error::invalid_config;
  }
  if (!known_ || (previous_ < 0 && duty > 0) || (previous_ > 0 && duty < 0)) {
    const auto error = stop();
    if (!succeeded(error)) {
      return error;
    }
  }
  const auto result = sink_.write(sink_.context, duty, false);
  known_ = succeeded(result);
  if (known_) {
    previous_ = duty;
  }
  return result;
}
Error L6474::set_parameter(std::uint8_t address, std::uint32_t value) noexcept {
  const auto info = l6474_register(address);
  if (!bus_.transfer || !timeout_valid(timeout_us_)) {
    return Error::invalid_config;
  }
  if (info.bytes == 0 || !info.writable || (value & ~info.mask) != 0 ||
      (address == 0x16 && (value & 7U) > 4)) {
    return Error::invalid_argument;
  }
  if (address == 0x16) {
    value |= 0x88U;
  } // STEP_MODE fixed bits7/3 must be1 (ST §9.1.10).
  std::array<std::uint8_t, 4> tx{};
  tx[0] = address;
  for (unsigned i = 0; i < info.bytes; ++i) {
    tx[i + 1] = static_cast<std::uint8_t>(value >> (8 * (info.bytes - i - 1)));
  }
  return bus_.transfer(bus_.context, std::span{tx}.first(info.bytes + 1), {}, timeout_us_);
}
Error L6474::set_step_mode(std::uint8_t logical_mode) noexcept {
  if (logical_mode > 4) {
    return Error::invalid_argument;
  }
  return set_parameter(0x16, logical_mode);
}
Error L6474::set_current_ma(float milliamps) noexcept {
  if (!std::isfinite(milliamps) || milliamps < 31.25F || milliamps > 4000.0F) {
    return Error::invalid_argument;
  }
  // Current is (TVAL + 1) * 31.25mA. Quantize down, never silently wrap/clamp
  // an unsafe request.
  const auto tval = static_cast<std::uint32_t>(std::floor((milliamps - 31.25F) / 31.25F));
  return set_parameter(0x09, tval);
}
Error L6474::set_current_raw(std::uint8_t tval) noexcept {
  return tval <= 127 ? set_parameter(0x09, tval) : Error::invalid_argument;
}
Error L6474::direction(bool forward) noexcept {
  return pins_.direction ? pins_.direction(pins_.context, forward) : Error::unsupported;
}
Error L6474::hard_reset() noexcept {
  if (!pins_.reset || !pins_.delay_us) {
    return Error::unsupported;
  }
  auto error = pins_.reset(pins_.context, true);
  if (error == Error::ok) {
    error = pins_.delay_us(pins_.context, 10000);
  }
  if (error == Error::ok) {
    error = pins_.reset(pins_.context, false);
  }
  if (error == Error::ok) {
    error = pins_.delay_us(pins_.context, 10000);
  }
  if (error != Error::ok) {
    const auto safe = pins_.reset(pins_.context, true);
    if (safe != Error::ok) {
      return safe;
    }
  }
  return error;
}
Error L6474::get_parameter(std::uint8_t address, std::uint32_t &output) noexcept {
  const auto info = l6474_register(address);
  if (!bus_.transfer || !timeout_valid(timeout_us_)) {
    return Error::invalid_config;
  }
  if (info.bytes == 0) {
    return Error::invalid_argument;
  }
  std::array<std::uint8_t, 4> tx{}, rx{};
  tx[0] = 0x20U | address;
  const auto error = bus_.transfer(bus_.context, std::span{tx}.first(info.bytes + 1),
                                   std::span{rx}.first(info.bytes + 1), timeout_us_);
  if (!succeeded(error)) {
    return error;
  }
  std::uint32_t value = 0;
  for (unsigned i = 0; i < info.bytes; ++i) {
    value = value << 8 | rx[i + 1];
  }
  output = value & info.mask;
  return Error::ok;
}
Error L6474::enable(bool enabled) noexcept {
  if (!bus_.transfer || !timeout_valid(timeout_us_)) {
    return Error::invalid_config;
  }
  const std::array<std::uint8_t, 1> command{static_cast<std::uint8_t>(enabled ? 0xb8 : 0xa8)};
  return bus_.transfer(bus_.context, command, {}, timeout_us_);
}
Error L6474::status(std::uint16_t &output) noexcept {
  if (!bus_.transfer || !timeout_valid(timeout_us_)) {
    return Error::invalid_config;
  }
  const std::array<std::uint8_t, 3> tx{0xd0, 0, 0};
  std::array<std::uint8_t, 3> rx{};
  const auto error = bus_.transfer(bus_.context, tx, rx, timeout_us_);
  if (succeeded(error)) {
    output = static_cast<std::uint16_t>(static_cast<std::uint16_t>(rx[1]) << 8 | rx[2]);
  }
  return error;
}
bool StepperPlanner::valid() const noexcept {
  return config_.minimum_steps < config_.maximum_steps && config_.maximum_steps_per_second > 0 &&
         config_.maximum_steps_per_second <= 1000000 && timeout_valid(config_.maximum_tick_us) &&
         sink_.configure && sink_.pulse && sink_.stop;
}
Error StepperPlanner::reference(std::int64_t steps) noexcept {
  if (!valid()) {
    return Error::invalid_config;
  }
  if (running_) {
    return Error::lease_conflict;
  }
  if (steps < config_.minimum_steps || steps > config_.maximum_steps) {
    return Error::invalid_argument;
  }
  position_ = steps;
  known_ = true;
  return Error::ok;
}
Error StepperPlanner::start(bool forward, std::uint32_t speed, std::uint32_t timeout,
                            std::uint32_t now) noexcept {
  if (!valid()) {
    return Error::invalid_config;
  }
  if (running_) {
    return Error::lease_conflict;
  }
  if (speed == 0 || speed > config_.maximum_steps_per_second || !timeout_valid(timeout)) {
    return Error::invalid_argument;
  }
  period_ = (1000000U + speed - 1) / speed;
  const auto error = sink_.configure(sink_.context, forward, period_);
  if (!succeeded(error)) {
    const auto stopped = sink_.stop(sink_.context);
    known_ = false;
    return succeeded(stopped) ? error : stopped;
  }
  forward_ = forward;
  started_ = now;
  last_tick_ = now;
  last_pulse_ = now;
  timeout_ = timeout;
  running_ = true;
  profile_active_ = false;
  return Error::ok;
}
Error StepperPlanner::move(std::int64_t target, std::uint32_t speed, std::uint32_t timeout,
                           std::uint32_t now) noexcept {
  if (!known_) {
    return Error::not_armed;
  }
  if (target < config_.minimum_steps || target > config_.maximum_steps) {
    return Error::invalid_argument;
  }
  if (target == position_) {
    return running_ ? Error::lease_conflict : Error::ok;
  }
  const auto error = start(target > position_, speed, timeout, now);
  if (succeeded(error)) {
    target_ = target;
    homing_ = false;
  }
  return error;
}
Error StepperPlanner::move_relative(std::int64_t delta, std::uint32_t speed, std::uint32_t timeout,
                                    std::uint32_t now) noexcept {
  if ((delta > 0 && position_ > std::numeric_limits<std::int64_t>::max() - delta) ||
      (delta < 0 && position_ < std::numeric_limits<std::int64_t>::min() - delta)) {
    return Error::invalid_argument;
  }
  return move(position_ + delta, speed, timeout, now);
}
Error StepperPlanner::build_profile(std::uint64_t distance, StepperProfile profile) noexcept {
  if (profile.base_frequency_hz == 0 || profile.base_frequency_hz > 1000000 ||
      profile.working_division == 0 || profile.working_division > 32 ||
      profile.edge_division < profile.working_division || profile.acceleration_ms > 60000) {
    return Error::invalid_argument;
  }
  auto edge = std::min<std::uint32_t>(profile.edge_division, 32);
  auto working = profile.working_division;
  if (profile.acceleration_ms < 10) {
    edge = working;
  }
  const auto frequency = profile.base_frequency_hz;
  // No hidden pulse burst can compensate for a service cadence too slow for the
  // base clock.
  if ((static_cast<std::uint64_t>(frequency) * config_.maximum_tick_us + 999999) / 1000000 > 64 ||
      (frequency + working - 1) / working > config_.maximum_steps_per_second) {
    return Error::invalid_argument;
  }
  std::uint32_t length = 0;
  std::uint64_t steps = 0;
  std::array<StepperRampEntry, 64> entries{};
  // At most32 retries; short moves progressively reduce peak speed, as in2026.
  for (unsigned attempt = 0; attempt < 32; ++attempt) {
    if (working == edge) {
      length = 0;
      steps = 0;
      break;
    }
    const auto acceleration_ticks =
        static_cast<std::uint64_t>(frequency) * profile.acceleration_ms / 1000;
    if (acceleration_ticks == 0) {
      return Error::invalid_argument;
    }
    const float slope = (1.0F / working - 1.0F / edge) / static_cast<float>(acceleration_ticks);
    float v1 = (1.0F / working - 1.0F / (working + 1)) * 0.78F;
    auto dx = static_cast<std::uint32_t>(v1 * 2 / slope);
    // The old dx/8-1 counter underflowed for tiny ramps. A stage is at least
    // one base tick.
    const auto center_ticks = std::max<std::uint32_t>(1, dx / 8);
    entries[32] = entries[31] = {working, center_ticks};
    steps = center_ticks / working;
    length = 1;
    auto previous = working;
    for (auto next = previous + 1; next <= edge; ++next) {
      const float difference = 1.0F / previous - 1.0F / next;
      const float v2 = difference - v1;
      if (v2 > 0) {
        const auto new_dx = static_cast<std::uint32_t>(dx * v2 / v1);
        if (new_dx != 0) {
          dx = new_dx;
          previous = next;
          v1 = v2;
          entries[32 + length] = entries[31 - length] = {next, dx};
          ++length;
          steps += dx / next;
          if (steps > distance / 2) {
            break;
          }
        }
      }
    }
    if (steps <= distance / 2) {
      break;
    }
    ++working;
  }
  base_frequency_ = frequency;
  profile_distance_ = distance;
  deceleration_steps_ = steps;
  ramp_ = entries;
  ramp_begin_ = 32 - length;
  ramp_end_ = 32 + length;
  ramp_index_ = ramp_begin_;
  ramp_limit_ = 32;
  division_reload_ = working - 1;
  acceleration_count_ = division_count_ = base_fraction_ = 0;
  travelled_ = 0;
  pulse_pending_ = false;
  return Error::ok;
}
Error StepperPlanner::move_profile(std::int64_t target, StepperProfile profile,
                                   std::uint32_t timeout, std::uint32_t now) noexcept {
  if (!valid()) {
    return Error::invalid_config;
  }
  if (!known_) {
    return Error::not_armed;
  }
  if (running_) {
    return Error::lease_conflict;
  }
  if (target < config_.minimum_steps || target > config_.maximum_steps || !timeout_valid(timeout)) {
    return Error::invalid_argument;
  }
  if (target == position_) {
    return Error::ok;
  }
  const auto distance =
      target > position_
          ? static_cast<std::uint64_t>(target) - static_cast<std::uint64_t>(position_)
          : static_cast<std::uint64_t>(position_) - static_cast<std::uint64_t>(target);
  const auto built = build_profile(distance, profile);
  if (built != Error::ok) {
    return built;
  }
  const auto maximum_speed =
      (profile.base_frequency_hz + profile.working_division - 1) / profile.working_division;
  const auto error = start(target > position_, maximum_speed, timeout, now);
  if (error == Error::ok) {
    target_ = target;
    homing_ = false;
    profile_active_ = true;
  }
  return error;
}
Error StepperPlanner::move_relative_profile(std::int64_t delta, StepperProfile profile,
                                            std::uint32_t timeout, std::uint32_t now) noexcept {
  if ((delta > 0 && position_ > std::numeric_limits<std::int64_t>::max() - delta) ||
      (delta < 0 && position_ < std::numeric_limits<std::int64_t>::min() - delta)) {
    return Error::invalid_argument;
  }
  return move_profile(position_ + delta, profile, timeout, now);
}
Error StepperPlanner::tick_profile(std::uint32_t delta_us) noexcept {
  const auto scaled = static_cast<std::uint64_t>(delta_us) * base_frequency_ + base_fraction_;
  const auto due = scaled / 1000000;
  base_fraction_ = static_cast<std::uint32_t>(scaled % 1000000);
  bool emitted = false;
  for (std::uint64_t tick = 0; tick < due && running_; ++tick) {
    if (pulse_pending_) {
      if (emitted || (!homing_ && ((forward_ && position_ >= config_.maximum_steps) ||
                                   (!forward_ && position_ <= config_.minimum_steps)))) {
        const auto stopped = stop();
        known_ = false;
        return stopped == Error::ok ? Error::timeout : stopped;
      }
      const auto error = sink_.pulse(sink_.context);
      if (error != Error::ok) {
        const auto stopped = stop();
        known_ = false;
        return stopped == Error::ok ? error : stopped;
      }
      emitted = true;
      if (!homing_) {
        position_ += forward_ ? 1 : -1;
      }
      ++travelled_;
      if (!homing_ && position_ == target_) {
        return stop();
      }
      if (homing_ && travelled_ >= profile_distance_) {
        const auto stopped = stop();
        known_ = false;
        return stopped == Error::ok ? Error::timeout : stopped;
      }
      const auto remaining = profile_distance_ - travelled_;
      if (remaining <= deceleration_steps_) {
        ramp_limit_ = ramp_end_;
      }
    }
    if (acceleration_count_ == 0) {
      if (ramp_index_ < ramp_limit_) {
        division_reload_ = ramp_[ramp_index_].division - 1;
        acceleration_count_ = ramp_[ramp_index_].base_ticks - 1;
        ++ramp_index_;
      }
    } else {
      --acceleration_count_;
    }
    pulse_pending_ = division_count_ == 0;
    division_count_ = pulse_pending_ ? division_reload_ : division_count_ - 1;
  }
  return Error::ok;
}
Error StepperPlanner::home(bool forward, std::uint32_t speed, std::uint32_t timeout,
                           std::uint32_t now) noexcept {
  const auto error = start(forward, speed, timeout, now);
  if (succeeded(error)) {
    homing_ = true;
    known_ = false;
    travelled_ = 0;
    profile_distance_ = static_cast<std::uint64_t>(config_.maximum_steps) -
                        static_cast<std::uint64_t>(config_.minimum_steps);
  }
  return error;
}
Error StepperPlanner::home_profile(bool forward, StepperProfile profile, std::uint32_t timeout,
                                   std::uint32_t now) noexcept {
  if (!valid()) {
    return Error::invalid_config;
  }
  if (running_) {
    return Error::lease_conflict;
  }
  if (!timeout_valid(timeout)) {
    return Error::invalid_argument;
  }
  const auto distance = static_cast<std::uint64_t>(config_.maximum_steps) -
                        static_cast<std::uint64_t>(config_.minimum_steps);
  const auto built = build_profile(distance, profile);
  if (built != Error::ok) {
    return built;
  }
  const auto speed =
      (profile.base_frequency_hz + profile.working_division - 1) / profile.working_division;
  const auto error = start(forward, speed, timeout, now);
  if (error == Error::ok) {
    homing_ = true;
    known_ = false;
    profile_active_ = true;
  }
  return error;
}
Error StepperPlanner::stop() noexcept {
  const bool was_homing = homing_;
  running_ = false;
  homing_ = false;
  profile_active_ = false;
  if (!sink_.stop) {
    known_ = false;
    return Error::invalid_config;
  }
  const auto error = sink_.stop(sink_.context);
  if (!succeeded(error) || was_homing) {
    known_ = false;
  }
  return error;
}
Error StepperPlanner::tick(std::uint32_t now, bool home_switch) noexcept {
  if (!running_) {
    return Error::ok;
  }
  if (elapsed(now, started_, timeout_) ||
      static_cast<std::uint32_t>(now - last_tick_) > config_.maximum_tick_us) {
    const auto error = stop();
    known_ = false;
    return succeeded(error) ? Error::timeout : error;
  }
  const auto delta_us = now - last_tick_;
  last_tick_ = now;
  if (homing_ && home_switch) {
    const auto reference_position = forward_ ? config_.maximum_steps : config_.minimum_steps;
    const auto error = stop();
    if (succeeded(error)) {
      position_ = reference_position;
      known_ = true;
    }
    return error;
  }
  if (profile_active_) {
    return tick_profile(delta_us);
  }
  if (!elapsed(now, last_pulse_, period_)) {
    return Error::ok;
  }
  if (!homing_ && ((forward_ && position_ >= config_.maximum_steps) ||
                   (!forward_ && position_ <= config_.minimum_steps))) {
    const auto error = stop();
    known_ = false;
    return succeeded(error) ? Error::invalid_argument : error;
  }
  const auto error = sink_.pulse(sink_.context);
  if (!succeeded(error)) {
    const auto stopped = stop();
    known_ = false;
    return succeeded(stopped) ? error : stopped;
  }
  last_pulse_ = now;
  if (!homing_) {
    position_ += forward_ ? 1 : -1;
    if (position_ == target_) {
      return stop();
    }
  } else {
    ++travelled_;
    if (travelled_ >= profile_distance_) {
      const auto stopped = stop();
      known_ = false;
      return stopped == Error::ok ? Error::timeout : stopped;
    }
  }
  return Error::ok;
}
std::optional<Rgb> hsv_rgb(Hsv color) noexcept {
  if (!std::isfinite(color.hue_deg) || !std::isfinite(color.saturation) ||
      !std::isfinite(color.value) || color.hue_deg < 0 || color.hue_deg > 360 ||
      color.saturation < 0 || color.saturation > 1 || color.value < 0 || color.value > 1) {
    return {};
  }
  const auto hue = color.hue_deg == 360 ? 0 : color.hue_deg / 60;
  const auto chroma = color.value * color.saturation;
  const auto x = chroma * (1.0F - std::fabs(std::fmod(hue, 2.0F) - 1.0F));
  const auto m = color.value - chroma;
  const auto sector = static_cast<unsigned>(hue);
  const std::array<std::array<float, 3>, 6> colors{{{chroma, x, 0},
                                                    {x, chroma, 0},
                                                    {0, chroma, x},
                                                    {0, x, chroma},
                                                    {x, 0, chroma},
                                                    {chroma, 0, x}}};
  const auto &rgb = colors[sector];
  return Rgb{static_cast<std::uint8_t>(std::lround((rgb[0] + m) * 255)),
             static_cast<std::uint8_t>(std::lround((rgb[1] + m) * 255)),
             static_cast<std::uint8_t>(std::lround((rgb[2] + m) * 255))};
}
Error hsv_rgb(std::span<const Hsv> colors, std::span<Rgb> output) noexcept {
  if (colors.size() > output.size()) {
    return Error::invalid_argument;
  }
  for (const auto color : colors) {
    if (!hsv_rgb(color)) {
      return Error::invalid_argument;
    }
  }
  for (std::size_t i = 0; i < colors.size(); ++i) {
    output[i] = *hsv_rgb(colors[i]);
  }
  return Error::ok;
}
std::size_t ws2812_waveform(std::span<const Rgb> leds, Ws2812Timing timing,
                            std::span<std::uint16_t> output) noexcept {
  if (timing.zero_high_ticks == 0 || timing.one_high_ticks <= timing.zero_high_ticks ||
      timing.one_high_ticks >= timing.timer_period_ticks || timing.reset_periods == 0 ||
      output.size() < timing.reset_periods ||
      leds.size() > (output.size() - timing.reset_periods) / 24) {
    return 0;
  }
  std::size_t index = 0;
  for (const auto led : leds) {
    const std::array<std::uint8_t, 3> grb{led.green, led.red, led.blue};
    for (const auto channel : grb) {
      for (unsigned bit = 0; bit < 8; ++bit) {
        output[index++] =
            (channel & (0x80U >> bit)) ? timing.one_high_ticks : timing.zero_high_ticks;
      }
    }
  }
  std::fill_n(output.begin() + static_cast<std::ptrdiff_t>(index), timing.reset_periods, 0);
  return index + timing.reset_periods;
}
} // namespace eurobot::devices
