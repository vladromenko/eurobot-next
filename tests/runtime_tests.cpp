#include "../firmware/src/runtime.cpp"
#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
namespace fake {
std::uint32_t micros{}, shift{}, inputs{};
unsigned spi_calls{}, pulses{}, shift_calls{}, led_calls{};
std::array<std::uint8_t, 8> last_spi{};
std::array<std::array<std::uint8_t, 8>, 128> spi_trace{};
std::array<std::int32_t, 3> measured{}, ticks{100, 200, 300};
bool bus_fail{}, irq_enabled{true};
bool probe_mode{};
unsigned probe_calls{}, probe_fail_at{}, probe_writes{};
std::uint8_t probe_actual_id{98}, probe_torque{1}, probe_return_level{2};
Error serial(void *, std::span<const std::uint8_t> tx, std::span<std::uint8_t> rx,
             std::size_t &received, std::uint32_t) {
  received = 0;
  if (probe_mode) {
    ++probe_calls;
    if (tx[4] != 2) {
      ++probe_writes;
      return Error::output_failure;
    }
    if (probe_calls == probe_fail_at) {
      return Error::timeout;
    }
  }
  if (bus_fail)
    return Error::timeout;
  if (rx.empty())
    return Error::ok;
  const auto count = tx[6];
  rx[0] = 255;
  rx[1] = 255;
  rx[2] = tx[2];
  rx[3] = count + 2;
  rx[4] = 0;
  std::fill_n(rx.begin() + 5, count, 0);
  if (count == 4) {
    unsigned index = tx[2] == 0x22 ? 0 : tx[2] == 0x21 ? 1 : 2;
    wire::write_u32(rx.data() + 5,
                    std::bit_cast<std::uint32_t>(tx[5] == 0x10 ? measured[index] : ticks[index]));
  }
  if (count == 18) {
    wire::write_u32(rx.data() + 5, 123456);
    rx[5 + 16] = 80;
    rx[5 + 17] = 4;
  }
  if (probe_mode) {
    if (tx[5] == 0 && count == 18) {
      std::fill_n(rx.begin() + 5, count, 0);
      rx[5] = 12; // AX-12 model, not assumed by production code.
      rx[7] = 34;
      rx[8] = probe_actual_id;
      rx[11] = 10; // CW10.
      rx[13] = 0xff;
      rx[14] = 3; // CCW1023.
      rx[21] = probe_return_level;
    } else if (tx[5] == 24 && count == 1) {
      rx[5] = probe_torque;
    } else if (tx[5] == 36 && count == 2) {
      rx[5] = 0x34;
      rx[6] = 0x12; // Unknown model units are retained raw.
    } else {
      return Error::malformed;
    }
  }
  unsigned sum = 0;
  for (unsigned i = 2; i < 5U + count; ++i)
    sum += rx[i];
  rx[5 + count] = static_cast<std::uint8_t>(~sum);
  received = count + 6;
  return Error::ok;
}
Error spi(void *, std::span<const std::uint8_t> tx, std::span<std::uint8_t>, std::uint32_t) {
  if (spi_calls < spi_trace.size()) {
    std::copy(tx.begin(), tx.end(), spi_trace[spi_calls].begin());
  }
  ++spi_calls;
  std::copy(tx.begin(), tx.end(), last_spi.begin());
  return Error::ok;
}
Error step_config(void *, bool, std::uint32_t) { return Error::ok; }
Error step_pulse(void *) {
  ++pulses;
  return Error::ok;
}
Error step_stop(void *) { return Error::ok; }
} // namespace fake
namespace eurobot::board {
Error initialize() noexcept { return Error::ok; }
std::uint32_t micros() noexcept { return fake::micros; }
std::uint32_t millis() noexcept { return fake::micros / 1000; }
Error delay_us(std::uint32_t duration) noexcept {
  fake::micros += duration;
  return Error::ok;
}
Error host_write(std::span<const std::uint8_t>, std::uint32_t) noexcept { return Error::ok; }
std::optional<std::uint8_t> host_read() noexcept { return {}; }
bool receive_overflow() noexcept { return false; }
void debug_enable(bool) noexcept {}
Error debug_write(std::span<const std::uint8_t>, std::uint32_t) noexcept { return Error::ok; }
std::optional<std::uint8_t> debug_read() noexcept { return {}; }
std::uint32_t reset_cause() noexcept { return 0; }
void clear_reset_cause() noexcept {}
[[noreturn]] void system_reset() noexcept { std::abort(); }
Error stepper_direction(std::size_t, bool) noexcept { return Error::ok; }
Error pwm_group_frequency(std::uint8_t, std::uint32_t) noexcept { return Error::ok; }
std::array<std::uint32_t, 2> pwm_group_info(std::uint8_t) noexcept { return {50, 19999}; }
Error pwm_duty(std::uint8_t, float, std::uint32_t) noexcept { return Error::ok; }
Error dc_brake(std::uint8_t, bool, std::uint32_t) noexcept { return Error::ok; }

Error shift_write(std::uint32_t word) noexcept {
  fake::shift = word;
  ++fake::shift_calls;
  return Error::ok;
}
Error serial_exchange(void *context, std::span<const std::uint8_t> tx, std::span<std::uint8_t> rx,
                      std::size_t &received, std::uint32_t timeout) noexcept {
  return fake::serial(context, tx, rx, received, timeout);
}
devices::SerialBus motor_bus() noexcept { return {nullptr, fake::serial}; }
devices::SerialBus servo_bus() noexcept { return {nullptr, fake::serial}; }
devices::SerialBus battery_bus() noexcept { return {nullptr, fake::serial}; }
devices::I2cBus i2c_bus() noexcept { return {}; }
devices::SpiBus stepper_bus(std::size_t) noexcept { return {nullptr, fake::spi}; }
devices::StepPulseSink stepper_pulses(std::size_t) noexcept {
  return {nullptr, fake::step_config, fake::step_pulse, fake::step_stop};
}
Error pwm_pulse(std::uint8_t, std::uint16_t) noexcept { return Error::ok; }
Error pwm_disable(std::uint8_t) noexcept { return Error::ok; }
Error dc_drive(std::uint8_t, float, DcMode, std::uint32_t) noexcept { return Error::ok; }
bool input(std::uint8_t i) noexcept { return (fake::inputs & (1U << i)) != 0; }
void stepper_service(void *, Error (*)(void *, std::uint32_t)) noexcept {}
void stepper_irq(bool enabled) noexcept { fake::irq_enabled = enabled; }
bool stepper_failed() noexcept { return false; }
Error leds_write(std::span<const devices::Rgb>) noexcept {
  ++fake::led_calls;
  return Error::ok;
}
void watchdog_start() noexcept {}
void watchdog_feed() noexcept {}
[[noreturn]] void fail_safe() noexcept { std::abort(); }
} // namespace eurobot::board
unsigned checks = 0;
#define CHECK(condition)                                                                           \
  do {                                                                                             \
    ++checks;                                                                                      \
    if (!(condition)) {                                                                            \
      std::fprintf(stderr, "runtime:%d %s\n", __LINE__, #condition);                               \
      std::abort();                                                                                \
    }                                                                                              \
  } while (false)
Deployment deployment() {
  Deployment value{};
  value.commissioned = true;
  value.max_active = 8;
  value.valve_outputs = {};
  for (unsigned i = 0; i < 8; ++i)
    value.pump_outputs[i] = i + 1;
  value.chassis.max_linear_mps = 1;
  value.chassis.max_angular_radps = 1;
  value.chassis.max_accel_mps2 = 1;
  value.chassis.max_angular_accel_radps2 = 1;
  value.chassis.max_wheel_tpm = 10000000;
  value.chassis.max_encoder_delta = 100000;
  value.stepper_enabled[0] = true;
  value.steppers[0] = {-100, 100, 1000, 1000};
  value.stepper_current_tval[0] = 16;
  value.stepper_ocd_threshold[0] = 2;
  value.stepper_driver_config[0] = 0x4083;
  value.home_input[0] = 0;
  value.home_forward[0] = false;
  value.dc_channels[0] = true;
  value.dc_frequency_hz[0] = 1000;
  value.dc_modes[0] = board::DcMode::coast_pwm;
  value.leds_enabled = true;
  value.battery_enabled = true;
  return value;
}
Error actuate(Hardware &h, std::uint8_t kind, std::uint8_t id, float value, float rate,
              std::uint32_t now = 0) {
  std::array<std::uint8_t, 10> bytes{kind, id};
  wire::write_f32(bytes.data() + 2, value);
  wire::write_f32(bytes.data() + 6, rate);
  return Hardware::actuator(&h, bytes, now);
}
int main() {
  Deployment invalid{};
  Hardware uncommissioned{invalid};
  CHECK(uncommissioned.initialize() == Error::invalid_config);
  CHECK(Hardware::write_outputs(&uncommissioned, 0, 0) == Error::invalid_config);
  CHECK(fake::shift == 0);
  CHECK(actuate(uncommissioned, 6, 0, 0.5, 0) == Error::invalid_config);
  // Uncommissioned startup can discover one servo, without any output writes or allowlist.
  PumpBank probe_pumps{{8, 0, 250, 0}, {&uncommissioned, Hardware::write_outputs}};
  Chassis probe_chassis{invalid.chassis, {&uncommissioned, Hardware::write_wheels}};
  Controller probe_controller{probe_pumps, probe_chassis, {&uncommissioned, Hardware::stop_hook}};
  uncommissioned.controller = &probe_controller;
  uncommissioned.chassis = &probe_chassis;
  wire::Dispatcher probe_dispatcher{probe_controller,
                                    {&uncommissioned, Hardware::actuator, Hardware::sensor_read,
                                     Hardware::job_request, Hardware::command_guard}};
  wire::Frame probe{};
  probe.type = 0x24;
  probe.length = 22;
  probe.payload[0] = 36;
  probe.payload[1] = 98;
  fake::probe_mode = true;
  const auto before_probe_shift = fake::shift_calls, before_probe_spi = fake::spi_calls;
  auto probe_reply = probe_dispatcher.dispatch(probe, 0);
  CHECK(probe_reply.length == 13 && probe_reply.payload[0] == 0);
  CHECK(probe_reply.payload[1] == 12 && probe_reply.payload[3] == 34 &&
        probe_reply.payload[4] == 98);
  CHECK(probe_reply.payload[5] == 10 && probe_reply.payload[7] == 255 &&
        probe_reply.payload[8] == 3);
  CHECK(probe_reply.payload[9] == 0x34 && probe_reply.payload[10] == 0x12 &&
        probe_reply.payload[11] == 1 && probe_reply.payload[12] == 2);
  CHECK(fake::probe_calls == 3 && fake::probe_writes == 0 &&
        fake::shift_calls == before_probe_shift && fake::spi_calls == before_probe_spi);
  for (auto id : {0U, 254U, 255U}) {
    probe.payload[1] = static_cast<std::uint8_t>(id);
    CHECK(probe_dispatcher.dispatch(probe, 0).payload[0] ==
          static_cast<unsigned>(Error::invalid_argument));
  }
  CHECK(fake::probe_calls == 3);
  probe.payload[1] = 98;
  probe.payload[2] = 1;
  CHECK(probe_dispatcher.dispatch(probe, 0).payload[0] ==
        static_cast<unsigned>(Error::invalid_argument));
  CHECK(fake::probe_calls == 3);
  probe.payload[2] = 0;
  for (unsigned fail_at = 1; fail_at <= 3; ++fail_at) {
    fake::probe_calls = 0;
    fake::probe_fail_at = fail_at;
    probe_reply = probe_dispatcher.dispatch(probe, 0);
    CHECK(probe_reply.length == 1 &&
          probe_reply.payload[0] == static_cast<unsigned>(Error::timeout));
    CHECK(fake::probe_calls == fail_at && !probe_controller.state().fault);
  }
  fake::probe_fail_at = 0;
  fake::probe_actual_id = 99;
  CHECK(probe_dispatcher.dispatch(probe, 0).payload[0] == static_cast<unsigned>(Error::malformed));
  fake::probe_actual_id = 98;
  fake::probe_torque = 2;
  CHECK(probe_dispatcher.dispatch(probe, 0).payload[0] == static_cast<unsigned>(Error::malformed));
  fake::probe_torque = 1;
  fake::probe_return_level = 3;
  CHECK(probe_dispatcher.dispatch(probe, 0).payload[0] == static_cast<unsigned>(Error::malformed));
  fake::probe_return_level = 2;
  fake::probe_mode = false;
  auto config = deployment();
  CHECK(config.valid());
  Hardware hw{config};
  CHECK(hw.initialize() == Error::ok);
  CHECK(fake::shift == 0);
  CHECK(fake::spi_calls == 0);
  Chassis chassis{config.chassis, {&hw, Hardware::write_wheels}};
  hw.chassis = &chassis;
  CHECK(chassis.initialize(0) == Error::ok);
  CHECK(actuate(hw, 5, 0, 10, 1000) == Error::not_armed);
  CHECK(fake::spi_calls == 0);
  CHECK(actuate(hw, 11, 0, 1, 1000) == Error::invalid_argument);
  CHECK(fake::spi_calls == 0);
  CHECK(actuate(hw, 11, 0, 0, 1000) == Error::ok);
  CHECK(fake::spi_calls == 8);
  CHECK(fake::spi_trace[1][0] == 0x17 && fake::spi_trace[1][1] == 3);
  CHECK(fake::spi_trace[6][0] == 0x10 && fake::spi_trace[6][1] == 31);
  CHECK(fake::shift == (1U << 8));
  CHECK(hw.planners[0].busy());
  CHECK(fake::irq_enabled);
  fake::micros = 1500;
  CHECK(Hardware::step_tick(&hw, fake::micros) == Error::ok);
  fake::inputs = 1;
  fake::micros = 2000;
  CHECK(Hardware::step_tick(&hw, fake::micros) == Error::ok);
  CHECK(hw.planners[0].position_known());
  CHECK(hw.planners[0].commanded_steps() == -100);
  CHECK(actuate(hw, 5, 0, 10, 1000) == Error::ok);
  CHECK(fake::spi_calls == 9);
  for (unsigned time = 3000; time <= 12000; time += 1000) {
    fake::micros = time;
    CHECK(Hardware::step_tick(&hw, time) == Error::ok);
  }
  CHECK(fake::pulses == 10 && hw.planners[0].commanded_steps() == -90);
  CHECK(!hw.aux_expired(1000));
  CHECK(actuate(hw, 6, 0, 0.5, 0, 0) == Error::ok);
  CHECK(!hw.aux_expired(249));
  CHECK(actuate(hw, 9, 0, 255, 0, 249) == Error::ok);
  CHECK(hw.aux_expired(250));
  CHECK(actuate(hw, 9, 59, 0x102030, 0, 249) == Error::ok);
  CHECK(actuate(hw, 9, 60, 0, 0, 249) == Error::invalid_argument);
  CHECK(hw.stop() == Error::ok);
  CHECK(fake::shift == 0);
  CHECK(!hw.planners[0].position_known());
  CHECK(!hw.aux_expired(1000));
  CHECK(actuate(hw, 5, 0, 1, 1000) == Error::not_armed);
  const auto before = fake::spi_calls;
  fake::inputs = 0;
  CHECK(actuate(hw, 11, 0, 0, 1000) == Error::ok);
  CHECK(fake::spi_calls == before + 8);
  CHECK(hw.stop() == Error::ok);
  CHECK(actuate(hw, 10, 0, 0, 0) == Error::ok);
  CHECK(chassis.state().pose_known);
  CHECK(hw.encoder_previous == fake::ticks);
  fake::measured[0] = 1;
  CHECK(actuate(hw, 10, 0, 0, 0) == Error::invalid_argument);
  fake::measured[0] = 0;
  fake::bus_fail = true;
  CHECK(actuate(hw, 10, 0, 0, 0) == Error::timeout);
  CHECK(!chassis.state().pose_known);
  fake::bus_fail = false;
  std::array<std::uint8_t, 24> sensors{};
  std::size_t count = 0;
  CHECK(Hardware::sensor_read(&hw, sensors, count, 0) == Error::ok);
  CHECK(count == 24 && sensors[0] == 1 && sensors[17] == 80 && sensors[18] == 4);
  CHECK(wire::read_u32(sensors.data() + 1) == 123456);
  CHECK(sensors[21] == 255 && sensors[22] == 31 && sensors[23] == 0);
  fake::bus_fail = true;
  CHECK(Hardware::sensor_read(&hw, sensors, count, 0) == Error::ok);
  CHECK(sensors[0] == 0 && sensors[23] == static_cast<unsigned>(Error::timeout));
  CHECK(count == 24);
  CHECK(Hardware::sensor_read(&hw, std::span{sensors}.first(23), count, 0) ==
        Error::invalid_argument);
  CHECK(count == 0);
  fake::bus_fail = false;
  CHECK(hw.stop() == Error::ok);
  CHECK(!hw.encoder_seeded && !chassis.state().pose_known && chassis.state().measured_known == 0);
  // Synthetic integration profile: zero-position servo feedback is supplied by fake::serial.
  auto job_config = deployment();
  job_config.grippers[0] = gripper::legacy_2026_reference(1);
  job_config.grippers[0].reviewed = true;
  job_config.grippers[0].cursor_vertical_deg = 0;
  job_config.grippers[0].valve_channels = {0, 1, 2, 3};
  job_config.ax_ids[98] = true;
  job_config.servo_max_deg[98] = 300;
  Hardware job_hw{job_config};
  PumpBank job_pumps{{8, 8, 250, 0}, {&job_hw, Hardware::write_outputs}};
  Chassis job_chassis{job_config.chassis, {&job_hw, Hardware::write_wheels}};
  Controller job_controller{job_pumps, job_chassis, {&job_hw, Hardware::stop_hook}};
  gripper::Engine engine{job_config.grippers,
                         {&job_hw, Hardware::job_guard, Hardware::job_validate, Hardware::job_issue,
                          Hardware::job_feedback, Hardware::job_stop}};
  job_hw.chassis = &job_chassis;
  job_hw.pumps = &job_pumps;
  job_hw.controller = &job_controller;
  job_hw.gripper_engine = &engine;
  CHECK(job_hw.initialize() == Error::ok);
  CHECK(job_controller.initialize(0) == Error::ok);
  CHECK(job_controller.claim(77, 1, 1000, 0) == Error::ok);
  CHECK(job_controller.rearm(77, 2, 0) == Error::ok);
  wire::Dispatcher dispatcher{job_controller,
                              {&job_hw, Hardware::actuator, Hardware::sensor_read,
                               Hardware::job_request, Hardware::command_guard}};
  wire::Frame request{};
  request.type = 0x21;
  request.owner = 77;
  request.sequence = 3;
  request.length = 22;
  wire::write_u32(request.payload.data() + 10, 1000);
  auto reply = dispatcher.dispatch(request, 0);
  CHECK(reply.type == 0xa1 && reply.length == 25 && reply.payload[0] == 0);
  CHECK(wire::read_u32(reply.payload.data() + 13) == 1);
  CHECK(engine.busy());
  job_hw.job_tick(0);
  CHECK(engine.busy() && job_hw.servo_deadlines[98].active);
  CHECK(Hardware::command_guard(&job_hw, 5, {}) ==
        Error::ok); // Servo-only job permits independent chassis control.
  job_hw.job_tick(50);
  CHECK(!engine.busy() && engine.state().phase == gripper::Phase::completed);
  CHECK(engine.state().completion == gripper::Evidence::measured);
  job_hw.servo_deadlines[97] = {true, 0}; // An unrelated resource is not part of this plan.
  request.type = 0x22;
  request.sequence = 4;
  request.length = 4;
  wire::write_u32(request.payload.data(), 1);
  reply = dispatcher.dispatch(request, 50);
  CHECK(reply.length == 25 && reply.payload[1] == 2 && reply.payload[10] == 1);
  CHECK(wire::read_u32(reply.payload.data() + 21) == 50);
  request = {};
  request.type = 0x21;
  request.owner = 77;
  request.sequence = 5;
  request.length = 22;
  request.payload[1] = 4; // Valve operation must reject missing wiring before issuing anything.
  wire::write_u32(request.payload.data() + 10, 1000);
  reply = dispatcher.dispatch(request, 60);
  CHECK(reply.payload[0] == static_cast<unsigned>(Error::invalid_config));
  CHECK(engine.state().phase == gripper::Phase::completed && job_hw.job_id == 1);
  request.payload[1] = 0;
  request.sequence = 6;
  CHECK(dispatcher.dispatch(request, 60).payload[0] == 0);
  CHECK(job_hw.servo_deadlines[98].command_at == 60);
  CHECK(job_hw.servo_deadlines[97].command_at == 0);
  job_hw.job_tick(60);
  request.type = 0x23;
  request.length = 4;
  request.sequence = 7;
  wire::write_u32(request.payload.data(), 2);
  reply = dispatcher.dispatch(request, 70);
  CHECK(reply.length == 25 && reply.payload[1] == 3 && reply.payload[12] == 0);
  CHECK(!engine.busy() && !job_hw.servo_deadlines[98].active && fake::shift == 0);
  request.type = 0x21;
  request.sequence = 8;
  request.length = 22;
  std::fill(request.payload.begin(), request.payload.end(), 0);
  wire::write_u32(request.payload.data() + 10, 1000);
  CHECK(dispatcher.dispatch(request, 80).payload[0] == 0);
  job_hw.job_tick(80);
  CHECK(job_controller.emergency_stop() == Error::ok);
  CHECK(engine.state().phase == gripper::Phase::faulted && engine.state().error == Error::estop);
  CHECK(!job_hw.servo_deadlines[98].active && !job_controller.state().armed);
  // Discovery refuses even a disarmed active owner, before accessing the bus.
  fake::probe_mode = true;
  fake::probe_calls = 0;
  std::size_t probe_size = 0;
  CHECK(Hardware::maintenance_request(&job_hw, std::span(probe.payload).first(22), reply.payload,
                                      probe_size, 100) == Error::lease_conflict);
  CHECK(fake::probe_calls == 0);
  fake::probe_mode = false;
  // Runtime services exercise real handler paths with explicit fake buses.
  application::Application app{job_config.application,
                               {&job_hw, Hardware::application_guard, Hardware::application_stop,
                                Hardware::application_action}};
  job_hw.application = &app;
  CHECK(app.initialize(100) == Error::ok);
  request = {};
  request.type = 0x24;
  request.length = 22;
  request.sequence = 123;
  request.owner = 999;
  request.payload[0] = static_cast<std::uint8_t>(maintenance::Operation::application_state);
  reply = dispatcher.dispatch(request, 100);
  CHECK(reply.type == 0xa4 && reply.length == 25 && reply.payload[0] == 0);
  CHECK(reply.payload[4] == 3); // communication enabled + reset cause latched.
  request.payload[0] = static_cast<std::uint8_t>(maintenance::Operation::gpio);
  reply = dispatcher.dispatch(request, 100);
  CHECK(reply.length == 9 && wire::read_u32(reply.payload.data() + 5) == 0x1fff);
  request.payload[0] = static_cast<std::uint8_t>(maintenance::Operation::stepper_parameter);
  const auto read_spi = fake::spi_calls, read_shift = fake::shift_calls;
  reply = dispatcher.dispatch(request, 100);
  CHECK(reply.payload[0] == static_cast<unsigned>(Error::not_armed));
  CHECK(fake::spi_calls == read_spi &&
        fake::shift_calls == read_shift); // Read cannot release NRST.
  request.payload[0] = static_cast<std::uint8_t>(maintenance::Operation::wheel_ticks);
  reply = dispatcher.dispatch(request, 100);
  CHECK(reply.length == 14 && reply.payload[1] == 7);
  CHECK(std::bit_cast<std::int32_t>(wire::read_u32(reply.payload.data() + 2)) == fake::ticks[0]);
  request.payload[0] = static_cast<std::uint8_t>(maintenance::Operation::wheel_path);
  reply = dispatcher.dispatch(request, 100);
  CHECK(reply.length == 14 && reply.payload[1] == 7 &&
        wire::read_f32(reply.payload.data() + 2) > 0);
  request.payload[0] = static_cast<std::uint8_t>(maintenance::Operation::motor_id);
  request.owner = 77;
  request.sequence = 9;
  request.payload[1] = 0;
  wire::write_u32(request.payload.data() + 2, 0x24);
  CHECK(dispatcher.dispatch(request, 100).payload[0] == 0 && job_hw.motors[0].id() == 0x24);
  request.sequence = 10;
  request.payload[1] = 1;
  CHECK(dispatcher.dispatch(request, 100).payload[0] ==
        static_cast<unsigned>(Error::invalid_argument));
  CHECK(job_hw.motors[1].id() ==
        0x21); // Duplicate live ID is refused after previous successful rename.
  request.payload[0] = static_cast<std::uint8_t>(maintenance::Operation::board_reset);
  request.payload[1] = 0;
  request.owner = 77;
  request.sequence = 11;
  reply = dispatcher.dispatch(request, 100);
  CHECK(reply.length == 1 && reply.payload[0] == 0 && job_hw.reset_pending);
  CHECK(!job_controller.state().armed && app.state().reset_requested);
  CHECK(Hardware::command_guard(&job_hw, 2, {}) == Error::not_armed);
  CHECK(Hardware::job_guard(&job_hw, 77, 100) == Error::not_armed);
  request.payload[0] = static_cast<std::uint8_t>(maintenance::Operation::board_info);
  reply = dispatcher.dispatch(request, 100);
  CHECK(reply.length == 17 && reply.payload[0] == 0 && reply.payload[2] == 0);
  auto second_config = deployment();
  second_config.stepper_enabled[1] = true;
  second_config.steppers[1] = {-100, 100, 1000, 1000};
  Hardware second{second_config};
  CHECK(second.initialize() == Error::ok);
  const auto second_start = fake::spi_calls;
  CHECK(second.prepare_stepper(1) == Error::ok);
  CHECK(fake::spi_calls == second_start + 6); // No TOFF_MIN override for source motors2..4.
  CHECK(fake::spi_trace[second_start + 1][0] == 0x17 &&
        fake::spi_trace[second_start + 1][1] == 0x87);
  for (unsigned i = second_start; i < fake::spi_calls; ++i) {
    CHECK(fake::spi_trace[i][0] != 0x10);
  }
  CHECK(second.stop() == Error::ok && !second.step_ready[1]);
  const auto restored = fake::spi_calls;
  CHECK(second.prepare_stepper(1) == Error::ok);
  CHECK(fake::spi_trace[restored + 1][0] == 0x17 && fake::spi_trace[restored + 1][1] == 0x87);
  std::printf("runtime: %u checks passed\n", checks);
}
