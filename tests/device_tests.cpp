#include "eurobot/devices.hpp"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
using namespace eurobot;
using namespace eurobot::devices;
namespace {
unsigned checks = 0;
#define CHECK(condition)                                                                           \
  do {                                                                                             \
    ++checks;                                                                                      \
    if (!(condition)) {                                                                            \
      std::fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #condition);                         \
      std::abort();                                                                                \
    }                                                                                              \
  } while (false)
struct SerialMock {
  std::array<std::uint8_t, 70> last{};
  std::array<std::uint8_t, 64> response{};
  std::size_t last_size{}, response_size{};
  unsigned calls{};
  Error result{Error::ok};
  std::uint8_t error{}, wrong_id{};
  bool corrupt{}, truncate{}, excessive{};
  static Error transfer(void *context, std::span<const std::uint8_t> tx, std::span<std::uint8_t> rx,
                        std::size_t &count, std::uint32_t timeout) {
    auto &self = *static_cast<SerialMock *>(context);
    ++self.calls;
    CHECK(timeout == 5000);
    self.last_size = tx.size();
    std::copy(tx.begin(), tx.end(), self.last.begin());
    count = 0;
    if (!succeeded(self.result)) {
      return self.result;
    }
    if (rx.empty()) {
      return Error::ok;
    }
    CHECK(rx.size() >= self.response_size + 6);
    rx[0] = 255;
    rx[1] = 255;
    rx[2] = static_cast<std::uint8_t>(tx[2] + self.wrong_id);
    rx[3] = static_cast<std::uint8_t>(self.response_size + 2);
    rx[4] = self.error;
    std::copy_n(self.response.begin(), self.response_size, rx.begin() + 5);
    unsigned sum = 0;
    for (std::size_t i = 2; i < self.response_size + 5; ++i) {
      sum += rx[i];
    }
    rx[5 + self.response_size] = static_cast<std::uint8_t>(~sum);
    if (self.corrupt) {
      rx[5 + self.response_size] ^= 1;
    }
    count = self.excessive ? 1000 : (self.response_size + (self.truncate ? 5 : 6));
    return Error::ok;
  }
  SerialBus bus() { return {this, transfer}; }
};
void dynamixel_and_maxon() {
  SerialMock serial;
  DxlProtocol1 protocol{serial.bus()};
  CHECK(protocol.ping(1) == Error::ok);
  const std::array<std::uint8_t, 6> ping{255, 255, 1, 2, 1, 251};
  CHECK(std::equal(ping.begin(), ping.end(), serial.last.begin()));
  Maxon maxon{protocol, 3};
  CHECK(maxon.set_tpm(-123456) == Error::ok);
  CHECK(serial.last_size == 11);
  CHECK(serial.last[2] == 3 && serial.last[4] == 3 && serial.last[5] == 0x10);
  CHECK(wire::read_u32(serial.last.data() + 6) ==
        std::bit_cast<std::uint32_t>(std::int32_t{-123456}));
  CHECK(maxon.set_tpm_range(100, 200) == Error::ok);
  CHECK(serial.last[5] == 4 && wire::read_u32(serial.last.data() + 6) == 100 &&
        wire::read_u32(serial.last.data() + 10) == 200);
  CHECK(maxon.set_tpm_range(200, 100) == Error::invalid_argument);
  CHECK(maxon.enable(true) == Error::ok);
  CHECK(serial.last[5] == 0x18 && serial.last[6] == 1);
  serial.response_size = 4;
  wire::write_u32(serial.response.data(), 0xfffffffeU);
  std::int32_t measured = 99;
  CHECK(maxon.get_tpm(measured) == Error::ok);
  CHECK(measured == -2);
  CHECK(serial.last[4] == 2 && serial.last[5] == 0x10 && serial.last[6] == 4);
  CHECK(maxon.total_ticks(measured) == Error::ok);
  CHECK(serial.last[5] == 0x14);
  serial.corrupt = true;
  measured = 99;
  CHECK(maxon.get_tpm(measured) == Error::crc);
  CHECK(measured == 99);
  serial.corrupt = false;
  serial.error = 4;
  CHECK(maxon.get_tpm(measured) == Error::output_failure);
  CHECK(protocol.device_error() == 4);
  CHECK(measured == 99);
  serial.error = 0;
  serial.wrong_id = 1;
  CHECK(maxon.get_tpm(measured) == Error::malformed);
  CHECK(measured == 99);
  serial.wrong_id = 0;
  serial.truncate = true;
  CHECK(maxon.get_tpm(measured) == Error::malformed);
  CHECK(measured == 99);
  serial.truncate = false;
  serial.excessive = true;
  CHECK(maxon.get_tpm(measured) == Error::malformed);
  serial.excessive = false;
  serial.response_size = 3;
  CHECK(maxon.get_tpm(measured) == Error::malformed);
  CHECK(measured == 99);
  serial.result = Error::timeout;
  CHECK(maxon.get_tpm(measured) == Error::timeout);
  CHECK(measured == 99);
  serial.result = Error::ok;
  CHECK(protocol.ping(254) == Error::invalid_argument);
  CHECK(protocol.write(254, 24, std::array<std::uint8_t, 1>{0}) == Error::ok);
  const auto calls = serial.calls;
  std::array<std::uint8_t, 65> large{};
  CHECK(protocol.read(1, 0, large) == Error::invalid_argument);
  CHECK(protocol.write(1, 0, large) == Error::invalid_argument);
  CHECK(protocol.read(1, 255, std::span{large}.first(2)) == Error::invalid_argument);
  CHECK(serial.calls == calls);
  serial.response_size = 8;
  wire::write_u32(serial.response.data(), 100);
  wire::write_u32(serial.response.data() + 4, 200);
  std::uint32_t low = 0, high = 0;
  CHECK(maxon.tpm_range(low, high) == Error::ok);
  CHECK(low == 100 && high == 200);
  CHECK(maxon.set_id(254) == Error::invalid_argument);
  CHECK(maxon.set_id(7) == Error::ok);
  CHECK(serial.last[2] == 3 && serial.last[5] == 1 && serial.last[6] == 7);
  CHECK(maxon.set_tpm(1) == Error::ok);
  CHECK(serial.last[2] == 7);
  const std::array<std::uint8_t, 2> ids{1, 2};
  const std::array<std::uint8_t, 4> values{1, 2, 3, 4};
  CHECK(protocol.sync_write(30, 2, ids, values) == Error::ok);
  const std::array<std::uint8_t, 8> expected{30, 2, 1, 1, 2, 2, 3, 4};
  CHECK(serial.last[2] == 254 && serial.last[4] == 0x83 && serial.last_size == 14);
  CHECK(std::equal(expected.begin(), expected.end(), serial.last.begin() + 5));
  CHECK(protocol.sync_write(30, 2, {}, {}) == Error::invalid_argument);
  CHECK(protocol.sync_write(30, 2, std::array<std::uint8_t, 2>{1, 1}, values) ==
        Error::invalid_argument);
  CHECK(protocol.sync_write(30, 2, ids, std::span{values}.first(3)) == Error::invalid_argument);
  CHECK(protocol.sync_write(255, 2, ids, values) == Error::invalid_argument);
}
void servo_and_battery() {
  SerialMock serial;
  DxlProtocol1 protocol{serial.bus()};
  DynamixelServo ax{protocol, 1, {ServoModel::ax_300, 10, 290}};
  CHECK(ax.position_deg(150) == Error::ok);
  CHECK(serial.last[5] == 30 && serial.last[6] == 0 && serial.last[7] == 2);
  CHECK(ax.position_deg(0) == Error::invalid_argument);
  CHECK(ax.position_deg(std::numeric_limits<float>::quiet_NaN()) == Error::invalid_argument);
  CHECK(ax.speed_raw(-25) == Error::ok);
  CHECK(serial.last[5] == 32 && serial.last[6] == 25 && serial.last[7] == 4);
  CHECK(ax.speed_raw(-1024) == Error::invalid_argument);
  CHECK(ax.torque_limit(1023) == Error::ok);
  CHECK(ax.torque_limit(1024) == Error::invalid_argument);
  CHECK(ax.led(true) == Error::ok);
  CHECK(serial.last[5] == 25 && serial.last[6] == 1);
  CHECK(ax.wheel_mode(true) == Error::ok);
  CHECK(wire::read_u32(serial.last.data() + 6) == 0);
  CHECK(ax.wheel_mode(false) == Error::ok);
  CHECK(serial.last[8] == 255 && serial.last[9] == 3);
  CHECK(ax.set_id(5) == Error::ok);
  CHECK(serial.last[2] == 1 && serial.last[5] == 3 && serial.last[6] == 5);
  CHECK(ax.torque(false) == Error::ok);
  CHECK(serial.last[5] == 24 && serial.last[6] == 0);
  DynamixelServo mx{protocol, 2, {ServoModel::mx_360, 0, 360}};
  CHECK(mx.position_deg(360) == Error::ok);
  CHECK(serial.last[6] == 255 && serial.last[7] == 15);
  serial.response_size = 11;
  serial.response = {};
  serial.response[0] = 255;
  serial.response[1] = 3; // AX maximum.
  serial.response[2] = 5;
  serial.response[3] = 4; // negative raw speed5.
  serial.response[4] = 7;
  serial.response[5] = 0;
  serial.response[6] = 120;
  serial.response[7] = 35;
  serial.response[10] = 1;
  ServoFeedback feedback{};
  CHECK(ax.feedback(feedback) == Error::ok);
  CHECK(feedback.position_deg == 300);
  CHECK(feedback.speed_raw == -5 && feedback.load_raw == 7 && feedback.moving &&
        feedback.voltage_decivolts == 120);
  serial.corrupt = true;
  feedback.position_deg = 77;
  CHECK(ax.feedback(feedback) == Error::crc);
  CHECK(feedback.position_deg == 77);
  serial.corrupt = false;
  serial.response[10] = 2;
  CHECK(ax.feedback(feedback) == Error::malformed);
  CHECK(feedback.position_deg == 77);
  std::array<std::uint8_t, 18> bytes{};
  wire::write_u32(bytes.data(), 123456);
  for (unsigned i = 0; i < 4; ++i) {
    bytes[4 + 2 * i] = 0xa0;
    bytes[5 + 2 * i] = 0xf;
  }
  bytes[12] = 0x80;
  bytes[13] = 0x3e;
  bytes[14] = 0xe8;
  bytes[15] = 3;
  bytes[16] = 80;
  bytes[17] = 4;
  BatteryState battery{};
  CHECK(decode_battery(bytes, battery) == Error::ok);
  CHECK(battery.power_mw == 123456 && battery.voltage_mv == 16000 && battery.current_ma == 1000);
  CHECK(battery.cell_mv[3] == 4000 && battery.cells == 4 && battery.soc_percent == 80);
  bytes[16] = 101;
  CHECK(decode_battery(bytes, battery) == Error::malformed);
  CHECK(battery.soc_percent == 80);
  CHECK(decode_battery(std::span{bytes}.first(17), battery) == Error::malformed);
  serial.response_size = 0;
  CHECK(calibrate_battery(protocol, 1, 0x16, 4000) == Error::ok);
  CHECK(serial.last[5] == 0x16 && serial.last[6] == 0xa0 && serial.last[7] == 0x0f);
  CHECK(calibrate_battery(protocol, 1, 0x17, 4000) == Error::invalid_argument);
  CHECK(calibrate_battery(protocol, 1, 0x22, 4000) == Error::invalid_argument);
}
struct I2cMock {
  struct Entry {
    std::uint8_t reg{};
    std::array<std::uint8_t, 4> values{};
    std::size_t length{};
  };
  std::array<Entry, 128> entries{};
  std::array<std::uint8_t, 64> pwm{};
  unsigned writes{}, delays{};
  std::uint8_t mode{};
  Error result{Error::ok};
  static Error write(void *context, std::uint8_t address, std::uint8_t reg,
                     std::span<const std::uint8_t> data, std::uint32_t timeout) {
    auto &self = *static_cast<I2cMock *>(context);
    CHECK(address == 0x40 && timeout == 5000);
    CHECK(data.size() <= 4);
    if (!succeeded(self.result)) {
      return self.result;
    }
    auto &entry = self.entries[self.writes++];
    entry.reg = reg;
    entry.length = data.size();
    std::copy(data.begin(), data.end(), entry.values.begin());
    for (std::size_t i = 0; i < data.size(); ++i) {
      const auto target = reg + ((self.mode & 0x20U) ? i : 0);
      if (target >= 6 && target <= 0x45) {
        self.pwm[target - 6] = data[i];
      }
      if (target >= 0xfa && target <= 0xfd) {
        for (unsigned channel = 0; channel < 16; ++channel) {
          self.pwm[channel * 4 + target - 0xfa] = data[i];
        }
      }
    }
    if (reg == 0) {
      self.mode = data[0];
    }
    return Error::ok;
  }
  static Error read(void *context, std::uint8_t address, std::uint8_t reg,
                    std::span<std::uint8_t> data, std::uint32_t timeout) {
    auto &self = *static_cast<I2cMock *>(context);
    CHECK(address == 0x40 && reg == 0 && timeout == 5000 && data.size() == 1);
    data[0] = self.mode;
    return self.result;
  }
  static Error delay(void *context, std::uint32_t microseconds) {
    auto &self = *static_cast<I2cMock *>(context);
    CHECK(microseconds == 500);
    ++self.delays;
    CHECK(self.entries[self.writes - 1].reg == 0 &&
          (self.entries[self.writes - 1].values[0] & 0x10) == 0);
    return self.result;
  }
  I2cBus bus() { return {this, write, read, delay}; }
};
void pwm() {
  CHECK(*servo_pulse(0, {}) == 500);
  CHECK(*servo_pulse(90, {}) == 1500);
  CHECK(*servo_pulse(180, {}) == 2500);
  CHECK(!servo_pulse(-1, {}));
  CHECK(!servo_pulse(std::numeric_limits<float>::quiet_NaN(), {}));
  CHECK(!servo_pulse(90, {0, 0, 500, 2500}));
  CHECK(!servo_pulse(90, {0, 180, 2500, 500}));
  I2cMock i2c;
  Pca9685 pca{i2c.bus()};
  CHECK(pca.pulse_us(1, 1500) == Error::not_armed);
  CHECK(pca.initialize() == Error::ok);
  CHECK(i2c.delays == 1);
  CHECK(i2c.writes == 6);
  CHECK(i2c.entries[0].reg == 0xfd && i2c.entries[0].values[0] == 0x10);
  CHECK((i2c.entries[1].values[0] & 0x30) == 0x30);
  CHECK(i2c.entries[2].reg == 0xfe && i2c.entries[2].values[0] == 121);
  CHECK((i2c.entries[5].values[0] & 0x80) != 0);
  for (unsigned channel = 0; channel < 16; ++channel) {
    CHECK((i2c.pwm[4 * channel + 3] & 0x10U) != 0);
  }
  CHECK(std::fabs(pca.actual_frequency() - 50.0288F) < 0.001F);
  CHECK(pca.position_deg(3, 90, {}) == Error::ok);
  CHECK(i2c.entries[6].reg == 18 && i2c.entries[6].values[2] == 0x33 &&
        i2c.entries[6].values[3] == 1);
  CHECK((i2c.pwm[4 * 3 + 3] & 0x10U) == 0);
  CHECK(pca.full_off() == Error::ok);
  CHECK(i2c.entries[7].reg == 0xfd && i2c.entries[7].values[0] == 0x10);
  CHECK(pca.pulse_us(2, 1000) == Error::ok);
  CHECK(i2c.writes == 9);
  for (unsigned channel = 0; channel < 16; ++channel) {
    CHECK(((i2c.pwm[4 * channel + 3] & 0x10U) == 0) == (channel == 2));
  }
  CHECK(pca.set_ticks(16, 0, 1) == Error::invalid_argument);
  CHECK(pca.set_ticks(0, 4096, 4096) == Error::invalid_argument);
  CHECK(pca.set_ticks(0, 0, 0) == Error::invalid_argument);
  CHECK(pca.pulse_us(0, 65535) == Error::invalid_argument);
  CHECK(pca.all_pulse_us(1500) == Error::ok);
  for (unsigned channel = 0; channel < 16; ++channel) {
    CHECK(i2c.pwm[channel * 4 + 2] == 0x33 && i2c.pwm[channel * 4 + 3] == 1);
  }
  CHECK(pca.sleep() == Error::ok);
  CHECK((i2c.mode & 0x10U) != 0);
  CHECK(pca.pulse_us(0, 1500) == Error::not_armed);
  CHECK(pca.wakeup() == Error::ok);
  CHECK(i2c.delays == 2 && (i2c.mode & 0x10U) == 0);
  for (unsigned channel = 0; channel < 16; ++channel) {
    CHECK((i2c.pwm[4 * channel + 3] & 0x10U) != 0);
  }
  i2c.result = Error::timeout;
  CHECK(pca.pulse_us(0, 1500) == Error::timeout);
  CHECK(pca.pulse_us(0, 1500) == Error::not_armed);
  i2c.result = Error::ok;
  i2c.mode = 0x40;
  CHECK(pca.initialize() == Error::invalid_config);
}
struct SpiMock {
  std::array<std::uint8_t, 4> last{}, response{};
  std::size_t size{};
  Error result{Error::ok};
  static Error transfer(void *context, std::span<const std::uint8_t> tx, std::span<std::uint8_t> rx,
                        std::uint32_t timeout) {
    auto &self = *static_cast<SpiMock *>(context);
    CHECK(timeout == 5000);
    self.size = tx.size();
    std::copy(tx.begin(), tx.end(), self.last.begin());
    if (!succeeded(self.result)) {
      return self.result;
    }
    std::copy_n(self.response.begin(), rx.size(), rx.begin());
    return Error::ok;
  }
};
void l6474() {
  SpiMock spi;
  L6474 driver{{&spi, SpiMock::transfer}};
  CHECK(driver.set_parameter(1, 0x123456) == Error::ok);
  CHECK(spi.size == 4);
  CHECK(spi.last == (std::array<std::uint8_t, 4>{1, 0x12, 0x34, 0x56}));
  CHECK(driver.set_parameter(1, 0x400000) == Error::invalid_argument);
  CHECK(driver.set_parameter(0x19, 0) == Error::invalid_argument);
  CHECK(driver.set_parameter(0x16, 5) == Error::invalid_argument);
  CHECK(driver.set_parameter(0x16, 0) == Error::ok);
  CHECK(spi.last[0] == 0x16 && spi.last[1] == 0x88);
  CHECK(driver.set_parameter(0x16, 0x8c) == Error::ok);
  CHECK(spi.last[1] == 0x8c);
  CHECK(driver.set_parameter(0x16, 0x100) == Error::invalid_argument);
  CHECK(driver.set_parameter(0x18, 0x10) == Error::invalid_argument);
  CHECK(driver.set_step_mode(0) == Error::ok);
  CHECK(spi.last[0] == 0x16 && spi.last[1] == 0x88);
  CHECK(driver.set_step_mode(4) == Error::ok);
  CHECK(spi.last[1] == 0x8c);
  CHECK(driver.set_step_mode(5) == Error::invalid_argument);
  CHECK(driver.set_step_mode(255) == Error::invalid_argument);
  CHECK(driver.set_current_ma(31.25F) == Error::ok);
  CHECK(spi.last[0] == 0x09 && spi.last[1] == 0);
  CHECK(driver.set_current_ma(62.5F) == Error::ok);
  CHECK(spi.last[1] == 1);
  CHECK(driver.set_current_ma(1100) == Error::ok);
  CHECK(spi.last[1] == 34);
  CHECK(driver.set_current_ma(1500) == Error::ok);
  CHECK(spi.last[1] == 47);
  CHECK(driver.set_current_ma(4000) == Error::ok);
  CHECK(spi.last[1] == 127);
  CHECK(driver.set_current_ma(31.24F) == Error::invalid_argument);
  CHECK(driver.set_current_ma(4000.1F) == Error::invalid_argument);
  CHECK(driver.set_current_ma(-1) == Error::invalid_argument);
  CHECK(driver.set_current_ma(std::numeric_limits<float>::quiet_NaN()) == Error::invalid_argument);
  CHECK(driver.set_current_ma(std::numeric_limits<float>::infinity()) == Error::invalid_argument);
  CHECK(driver.set_parameter(0x09, 0x7f) == Error::ok);
  CHECK(driver.enable(true) == Error::ok);
  CHECK(spi.last[0] == 0xb8);
  CHECK(driver.enable(false) == Error::ok);
  CHECK(spi.last[0] == 0xa8);
  spi.response = {0, 0x12, 0x34, 0x56};
  std::uint32_t value = 0;
  CHECK(driver.get_parameter(1, value) == Error::ok);
  CHECK(value == 0x123456);
  CHECK(spi.last[0] == 0x21);
  std::uint16_t status = 0;
  CHECK(driver.status(status) == Error::ok);
  CHECK(status == 0x1234);
  CHECK(spi.last[0] == 0xd0);
  spi.result = Error::timeout;
  value = 99;
  CHECK(driver.get_parameter(1, value) == Error::timeout);
  CHECK(value == 99);
}
void independent_battery_reads() {
  SerialMock serial;
  DxlProtocol1 protocol{serial.bus()};
  CHECK(set_battery_id(protocol, 17, 18) == Error::ok);
  CHECK(serial.last[2] == 17 && serial.last[5] == 1 && serial.last[6] == 18);
  CHECK(set_battery_id(protocol, 254, 18) == Error::invalid_argument);
  CHECK(set_battery_id(protocol, 17, 254) == Error::invalid_argument);
  std::array<std::uint16_t, 4> cells{};
  serial.response_size = 8;
  for (std::size_t i = 0; i < 4; ++i) {
    serial.response[2 * i] = static_cast<std::uint8_t>(i + 1);
    serial.response[2 * i + 1] = 16;
  }
  CHECK(read_battery_cells(protocol, 17, cells) == Error::ok);
  CHECK(serial.last[5] == 8 && serial.last[6] == 8);
  CHECK(cells == (std::array<std::uint16_t, 4>{4097, 4098, 4099, 4100}));
  serial.response_size = 2;
  std::uint16_t voltage = 0, current = 0;
  CHECK(read_battery_voltage(protocol, 17, voltage) == Error::ok && voltage == 4097);
  CHECK(serial.last[5] == 0x10);
  CHECK(read_battery_current(protocol, 17, current) == Error::ok && current == 4097);
  CHECK(serial.last[5] == 0x12);
  std::uint32_t power = 0;
  serial.response_size = 4;
  wire::write_u32(serial.response.data(), 123456);
  CHECK(read_battery_power(protocol, 17, power) == Error::ok && power == 123456);
  CHECK(serial.last[5] == 4);
  serial.response_size = 1;
  serial.response[0] = 100;
  std::uint8_t soc = 0, count = 0;
  CHECK(read_battery_soc(protocol, 17, soc) == Error::ok && soc == 100);
  CHECK(serial.last[5] == 0x14);
  serial.response[0] = 101;
  CHECK(read_battery_soc(protocol, 17, soc) == Error::malformed && soc == 100);
  serial.response[0] = 4;
  CHECK(read_battery_cell_count(protocol, 17, count) == Error::ok && count == 4);
  CHECK(serial.last[5] == 0x15);
  serial.response[0] = 0;
  CHECK(read_battery_cell_count(protocol, 17, count) == Error::malformed && count == 4);
  serial.result = Error::timeout;
  CHECK(read_battery_voltage(protocol, 17, voltage) == Error::timeout && voltage == 4097);
  CHECK(read_battery_current(protocol, 17, current) == Error::timeout && current == 4097);
  CHECK(read_battery_power(protocol, 17, power) == Error::timeout && power == 123456);
  CHECK(read_battery_soc(protocol, 17, soc) == Error::timeout && soc == 100);
  CHECK(read_battery_cell_count(protocol, 17, count) == Error::timeout && count == 4);
  CHECK(read_battery_cells(protocol, 17, cells) == Error::timeout && cells[0] == 4097);
  serial.result = Error::ok;
  serial.response_size = 0;
  Maxon motor{protocol, 17};
  CHECK(motor.ping() == Error::ok && serial.last[4] == 1 && serial.last[2] == 17);
}
void l6474_gpio() {
  struct Pins {
    std::array<unsigned, 12> events{};
    unsigned count{};
    Error fail_delay{Error::ok};
    static Error direction(void *context, bool forward) {
      auto &self = *static_cast<Pins *>(context);
      self.events[self.count++] = forward ? 1 : 2;
      return Error::ok;
    }
    static Error reset(void *context, bool asserted) {
      auto &self = *static_cast<Pins *>(context);
      self.events[self.count++] = asserted ? 3 : 4;
      return Error::ok;
    }
    static Error delay(void *context, std::uint32_t us) {
      auto &self = *static_cast<Pins *>(context);
      self.events[self.count++] = us;
      return self.fail_delay;
    }
  } pins;
  SpiMock spi;
  L6474 driver{{&spi, SpiMock::transfer}, 5000, {&pins, Pins::direction, Pins::reset, Pins::delay}};
  CHECK(driver.direction(true) == Error::ok);
  CHECK(driver.direction(false) == Error::ok);
  CHECK(pins.events[0] == 1 && pins.events[1] == 2);
  CHECK(driver.hard_reset() == Error::ok);
  CHECK(pins.events[2] == 3 && pins.events[3] == 10000 && pins.events[4] == 4 &&
        pins.events[5] == 10000);
  pins.fail_delay = Error::timeout;
  CHECK(driver.hard_reset() == Error::timeout);
  CHECK(pins.events[6] == 3 && pins.events[7] == 10000 && pins.events[8] == 3);
  CHECK(driver.set_current_raw(127) == Error::ok && spi.last[1] == 127);
  CHECK(driver.set_current_raw(128) == Error::invalid_argument);
  L6474 no_pins{{&spi, SpiMock::transfer}};
  CHECK(no_pins.hard_reset() == Error::unsupported);
  CHECK(no_pins.direction(true) == Error::unsupported);
}
void individual_servo_reads() {
  SerialMock serial;
  DxlProtocol1 protocol{serial.bus()};
  DynamixelServo ax{protocol, 7, {ServoModel::ax_300, 0, 300}};
  CHECK(ax.ping() == Error::ok && serial.last[4] == 1);
  serial.response_size = 2;
  serial.response[0] = 255;
  serial.response[1] = 3;
  float position = -1;
  CHECK(ax.present_position_deg(position) == Error::ok && position == 300);
  CHECK(serial.last[5] == 36 && serial.last[6] == 2);
  serial.response[0] = 123;
  serial.response[1] = 4;
  std::int16_t speed = 99;
  CHECK(ax.moving_speed_raw(speed) == Error::ok && speed == -123);
  CHECK(serial.last[5] == 32);
  CHECK(ax.present_speed_raw(speed) == Error::ok && speed == -123);
  CHECK(serial.last[5] == 38);
  CHECK(ax.present_load_raw(speed) == Error::ok && speed == -123);
  CHECK(serial.last[5] == 40);
  serial.response[1] = 8;
  speed = 99;
  CHECK(ax.present_load_raw(speed) == Error::malformed && speed == 99);
  CHECK(ax.present_position_deg(position) == Error::malformed && position == 300);
  serial.result = Error::timeout;
  CHECK(ax.present_position_deg(position) == Error::timeout && position == 300);
  CHECK(ax.moving_speed_raw(speed) == Error::timeout && speed == 99);
  serial.result = Error::ok;
  serial.response[0] = 255;
  serial.response[1] = 15;
  DynamixelServo mx{protocol, 8, {ServoModel::mx_360, 0, 360}};
  CHECK(mx.present_position_deg(position) == Error::ok && position == 360);
}
struct StepMock {
  unsigned pulses{}, stopped{};
  bool forward{};
  std::uint32_t period{};
  Error result{Error::ok};
  static Error configure(void *context, bool forward, std::uint32_t period) {
    auto &self = *static_cast<StepMock *>(context);
    self.forward = forward;
    self.period = period;
    return self.result;
  }
  static Error pulse(void *context) {
    auto &self = *static_cast<StepMock *>(context);
    ++self.pulses;
    return self.result;
  }
  static Error stop(void *context) {
    auto &self = *static_cast<StepMock *>(context);
    ++self.stopped;
    return self.result;
  }
  StepPulseSink sink() { return {this, configure, pulse, stop}; }
};
void stepper() {
  StepMock mock;
  StepperPlanner planner{{-100, 100, 1000, 1000}, mock.sink()};
  CHECK(planner.move(10, 1000, 100000, 0) == Error::not_armed);
  CHECK(planner.reference(0) == Error::ok);
  CHECK(planner.move(101, 1000, 100000, 0) == Error::invalid_argument);
  CHECK(planner.move(10, 1001, 100000, 0) == Error::invalid_argument);
  CHECK(planner.move(10, 1000, 100000, 0) == Error::ok);
  CHECK(mock.forward && mock.period == 1000);
  CHECK(planner.reference(0) == Error::lease_conflict);
  CHECK(planner.move(1, 1000, 100000, 0) == Error::lease_conflict);
  for (unsigned time = 500; time <= 10000; time += 500) {
    CHECK(planner.tick(time) == Error::ok);
  }
  CHECK(mock.pulses == 10);
  CHECK(!planner.busy());
  CHECK(planner.commanded_steps() == 10 && planner.position_known());
  CHECK(planner.move(-2, 1000, 100000, 10000) == Error::ok);
  CHECK(!mock.forward);
  CHECK(planner.tick(12000) == Error::timeout);
  CHECK(!planner.busy() && !planner.position_known());
  CHECK(planner.home(false, 500, 5000, 0xfffffff0U) == Error::ok);
  CHECK(planner.tick(0x1e4U) == Error::ok);
  CHECK(planner.tick(0x3d8U, true) == Error::ok);
  CHECK(planner.position_known() && planner.commanded_steps() == -100 && !planner.busy());
  CHECK(planner.home(true, 500, 1000, 0) == Error::ok);
  CHECK(planner.tick(1000) == Error::timeout);
  CHECK(!planner.position_known());
  CHECK(planner.reference(0) == Error::ok);
  CHECK(planner.move(1, 1000, 2000, 0) == Error::ok);
  mock.result = Error::output_failure;
  CHECK(planner.tick(1000) == Error::output_failure);
  CHECK(!planner.position_known());
}
void stepper_profiles() {
  struct Pulses {
    std::array<std::uint32_t, 1100> times{};
    std::uint32_t now{};
    unsigned count{};
    bool forward{};
    static Error configure(void *context, bool forward, std::uint32_t) {
      static_cast<Pulses *>(context)->forward = forward;
      return Error::ok;
    }
    static Error pulse(void *context) {
      auto &self = *static_cast<Pulses *>(context);
      CHECK(self.count < self.times.size());
      self.times[self.count++] = self.now;
      return Error::ok;
    }
    static Error stop(void *) { return Error::ok; }
    StepPulseSink sink() { return {this, configure, pulse, stop}; }
  } pulses;
  StepperPlanner planner{{-2000, 2000, 10000, 100}, pulses.sink()};
  CHECK(planner.reference(0) == Error::ok);
  CHECK(planner.move_profile(1000, {19200, 64, 10, 200}, 2000000, 0) == Error::ok);
  CHECK(planner.profiled() && pulses.forward);
  const auto ramp = planner.ramp();
  CHECK(ramp.size() == 36);
  CHECK(ramp.front().division == 32 && ramp.front().base_ticks == 32);
  CHECK(ramp[17].division == 10 && ramp[17].base_ticks == 99);
  for (std::size_t i = 0; i < ramp.size() / 2; ++i) {
    CHECK(ramp[i].division == ramp[ramp.size() - 1 - i].division);
    CHECK(ramp[i].base_ticks > 0);
  }
  while (planner.busy() && pulses.now < 2000000) {
    pulses.now += 100;
    CHECK(planner.tick(pulses.now) == Error::ok);
  }
  CHECK(!planner.busy() && planner.position_known() && planner.commanded_steps() == 1000);
  CHECK(pulses.count == 1000);
  // Acceleration reaches a faster cruise and deceleration leaves a slower final
  // interval.
  const auto first = pulses.times[20] - pulses.times[19];
  const auto cruise = pulses.times[500] - pulses.times[499];
  const auto final = pulses.times[999] - pulses.times[998];
  CHECK(first > cruise && final > cruise);
  const auto stopped_at = pulses.now;
  CHECK(planner.move_relative_profile(-1, {19200, 32, 10, 200}, 100000, stopped_at) == Error::ok);
  CHECK(!pulses.forward && planner.ramp().empty()); // Short move reduces peak speed to edge.
  while (planner.busy()) {
    pulses.now += 100;
    CHECK(planner.tick(pulses.now) == Error::ok);
  }
  CHECK(planner.commanded_steps() == 999);
  CHECK(planner.move_relative(std::numeric_limits<std::int64_t>::max(), 100, 10000, pulses.now) ==
        Error::invalid_argument);
  CHECK(planner.move_profile(0, {19200, 0, 0, 200}, 10000, pulses.now) == Error::invalid_argument);
  CHECK(planner.move_profile(0, {19200, 8, 10, 200}, 10000, pulses.now) == Error::invalid_argument);
  CHECK(planner.move_profile(0, {19200, 32, 10, 60001}, 10000, pulses.now) ==
        Error::invalid_argument);
  CHECK(planner.move_profile(0, {0, 32, 10, 200}, 10000, pulses.now) == Error::invalid_argument);
  CHECK(planner.move_profile(0, {19200, 32, 10, 9}, 1000000, pulses.now) == Error::ok);
  CHECK(planner.ramp().empty()); // Legacy ramp<10ms selects immediate working division.
  CHECK(planner.stop() == Error::ok);
  CHECK(planner.move_profile(0, {19200, 32, 10, 200}, 1000000, 0xfffffff0U) == Error::ok);
  CHECK(planner.tick(84) == Error::ok);
  CHECK(planner.tick(185) == Error::timeout); // Missed service deadline, including wrap.
  CHECK(!planner.position_known());
  StepMock late;
  StepperPlanner burst{{-100, 100, 10000, 1000}, late.sink()};
  CHECK(burst.reference(0) == Error::ok);
  CHECK(burst.move_profile(50, {19200, 2, 2, 0}, 100000, 0) == Error::ok);
  CHECK(burst.tick(1000) == Error::timeout); // Never emit catch-up pulse bursts.
  CHECK(late.pulses == 1 && !burst.position_known());
}
void profiled_homing() {
  CHECK(sizeof(StepperPlanner) <= 1024); // Fixed SRAM budget per motor, including64 stages.
  StepMock mock;
  StepperPlanner planner{{-1000, 1000, 2000, 100}, mock.sink()};
  CHECK(planner.home_profile(false, {19200, 32, 16, 500}, 1000000, 0) == Error::ok);
  CHECK(planner.profiled() && !planner.position_known() && !mock.forward);
  CHECK(planner.tick(100, true) == Error::ok);
  CHECK(planner.position_known() && planner.commanded_steps() == -1000 && !planner.busy());
  CHECK(mock.pulses == 0); // Already asserted switch never emits a seek pulse.
  CHECK(planner.home_profile(true, {19200, 32, 16, 500}, 1000000, 100) == Error::ok);
  CHECK(planner.home_profile(false, {19200, 32, 16, 500}, 1000000, 100) == Error::lease_conflict);
  for (std::uint32_t now = 200; now <= 10000; now += 100) {
    CHECK(planner.tick(now) == Error::ok);
  }
  CHECK(mock.pulses > 0 && !planner.position_known());
  CHECK(planner.tick(10100, true) == Error::ok);
  CHECK(planner.position_known() && planner.commanded_steps() == 1000 && !planner.busy());
  StepMock missing_switch;
  StepperPlanner bounded{{-4, 4, 2000, 100}, missing_switch.sink()};
  CHECK(bounded.home_profile(false, {19200, 32, 16, 500}, 100000, 0) == Error::ok);
  Error result = Error::ok;
  for (std::uint32_t now = 100; now < 100000 && bounded.busy(); now += 100) {
    result = bounded.tick(now);
  }
  CHECK(result == Error::timeout && missing_switch.pulses == 8);
  CHECK(!bounded.busy() && !bounded.position_known());
  const auto before_constant = missing_switch.pulses;
  CHECK(bounded.home(false, 2000, 100000, 0) == Error::ok);
  for (std::uint32_t now = 100; now <= 4000; now += 100) {
    const auto error = bounded.tick(now);
    CHECK(error == (now == 4000 ? Error::timeout : Error::ok));
  }
  CHECK(missing_switch.pulses - before_constant == 8);
  CHECK(!bounded.busy() && !bounded.position_known());
  CHECK(bounded.home_profile(true, {19200, 32, 16, 500}, 1000, 0) == Error::ok);
  CHECK(bounded.tick(101) == Error::timeout && !bounded.position_known());
  CHECK(bounded.home_profile(false, {19200, 32, 16, 500}, 1000, 0xfffffff0U) == Error::ok);
  for (std::uint32_t i = 1; i < 10; ++i) {
    CHECK(bounded.tick(0xfffffff0U + i * 100) == Error::ok);
  }
  CHECK(bounded.tick(984) == Error::timeout); // Elapsed1000us across timer wrap.
  CHECK(!bounded.position_known());
}
void hsv_colors() {
  const auto red = hsv_rgb({0, 1, 1}), green = hsv_rgb({120, 1, 1}), blue = hsv_rgb({240, 1, 1});
  CHECK(red && red->red == 255 && red->green == 0 && red->blue == 0);
  CHECK(green && green->red == 0 && green->green == 255 && green->blue == 0);
  CHECK(blue && blue->red == 0 && blue->green == 0 && blue->blue == 255);
  const auto white = hsv_rgb({360, 0, 0.5F});
  CHECK(white && white->red == 128 && white->green == 128 && white->blue == 128);
  CHECK(!hsv_rgb({361, 1, 1}));
  CHECK(!hsv_rgb({0, -1, 1}));
  CHECK(!hsv_rgb({std::numeric_limits<float>::quiet_NaN(), 0, 0}));
  std::array<Rgb, 2> output{Rgb{1, 2, 3}, Rgb{4, 5, 6}};
  std::array<Hsv, 2> input{Hsv{0, 1, 1}, Hsv{30, 2, 1}};
  CHECK(hsv_rgb(input, output) == Error::invalid_argument);
  CHECK(output[0].red == 1 && output[1].red == 4);
  input[1].saturation = 1;
  CHECK(hsv_rgb(input, output) == Error::ok);
  CHECK(output[0].red == 255 && output[1].red == 255 && output[1].green == 128);
}
void bridge_and_leds() {
  struct Bridge {
    std::array<float, 10> duty{};
    unsigned writes{};
    Error result{Error::ok};
    static Error write(void *context, float value, bool) {
      auto &self = *static_cast<Bridge *>(context);
      self.duty[self.writes++] = value;
      return self.result;
    }
  } bridge;
  HBridge driver{{&bridge, Bridge::write}};
  CHECK(driver.drive(0.5F) == Error::ok);
  CHECK(bridge.writes == 2 && bridge.duty[0] == 0 && bridge.duty[1] == 0.5F);
  CHECK(driver.drive(-0.5F) == Error::ok);
  CHECK(bridge.writes == 4 && bridge.duty[2] == 0 && bridge.duty[3] == -0.5F);
  CHECK(driver.drive(2) == Error::invalid_argument);
  CHECK(driver.drive(std::numeric_limits<float>::infinity()) == Error::invalid_argument);
  bridge.result = Error::output_failure;
  CHECK(driver.stop() == Error::output_failure);
  CHECK(!driver.commanded_known());
  std::array<std::uint16_t, 64> waveform{};
  const std::array<Rgb, 1> leds{Rgb{0x80, 1, 0}};
  CHECK(ws2812_waveform(leds, {210, 68, 142, 40}, waveform) == 64);
  CHECK(waveform[0] == 68 && waveform[7] == 142 && waveform[8] == 142 && waveform[9] == 68);
  CHECK(std::all_of(waveform.begin() + 24, waveform.end(), [](auto value) { return value == 0; }));
  CHECK(ws2812_waveform(leds, {210, 68, 142, 41}, waveform) == 0);
  CHECK(ws2812_waveform(leds, {100, 68, 142, 40}, waveform) == 0);
}
} // namespace
int main() {
  dynamixel_and_maxon();
  servo_and_battery();
  pwm();
  l6474();
  independent_battery_reads();
  l6474_gpio();
  individual_servo_reads();
  stepper();
  stepper_profiles();
  profiled_homing();
  hsv_colors();
  bridge_and_leds();
  std::printf("devices: %u checks passed\n", checks);
}
