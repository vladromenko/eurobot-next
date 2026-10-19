#include "FreeRTOS.h"
#include "eurobot/maintenance.hpp"
#include "eurobot/stm32_board.hpp"
#include "queue.h"
#include "task.h"
#include <algorithm>
#include <atomic>
#include <cmath>
#include <limits>
#ifdef EUROBOT_DEPLOYMENT_CONFIG
#include "deployment.hpp"
#endif
namespace {
using namespace eurobot;
using namespace eurobot::devices;
using eurobot::board::Deployment;
constexpr auto reset_bits = board::connectors::stepper_reset_bits;
struct StepGuard {
  StepGuard() { board::stepper_irq(false); }
  ~StepGuard() { board::stepper_irq(true); }
  StepGuard(const StepGuard &) = delete;
  StepGuard &operator=(const StepGuard &) = delete;
};
class Hardware {
public:
  struct StepDevice {
    Hardware *owner{};
    std::size_t channel{};
  };
  explicit Hardware(const Deployment &c)
      : config(c), motor_protocol(board::motor_bus(), 1500),
        servo_protocol(board::servo_bus(), 3000), battery_protocol(board::battery_bus(), 1500),
        motors{Maxon{motor_protocol, c.motor_ids[0]}, Maxon{motor_protocol, c.motor_ids[1]},
               Maxon{motor_protocol, c.motor_ids[2]}},
        pca(board::i2c_bus()), step_devices{{{this, 0}, {this, 1}, {this, 2}, {this, 3}}},
        step_driver{L6474{board::stepper_bus(0),
                          5000,
                          {&step_devices[0], step_direction, step_reset, step_delay}},
                    L6474{board::stepper_bus(1),
                          5000,
                          {&step_devices[1], step_direction, step_reset, step_delay}},
                    L6474{board::stepper_bus(2),
                          5000,
                          {&step_devices[2], step_direction, step_reset, step_delay}},
                    L6474{board::stepper_bus(3),
                          5000,
                          {&step_devices[3], step_direction, step_reset, step_delay}}},
        planners{StepperPlanner{c.steppers[0], board::stepper_pulses(0)},
                 StepperPlanner{c.steppers[1], board::stepper_pulses(1)},
                 StepperPlanner{c.steppers[2], board::stepper_pulses(2)},
                 StepperPlanner{c.steppers[3], board::stepper_pulses(3)}} {}
  const Deployment &config;
  DxlProtocol1 motor_protocol, servo_protocol, battery_protocol;
  std::array<Maxon, 3> motors;
  Pca9685 pca;
  std::array<StepDevice, 4> step_devices;
  std::array<L6474, 4> step_driver;
  std::array<StepperPlanner, 4> planners;
  std::array<std::int32_t, 3> encoder_previous{};
  std::array<Rgb, 60> leds{};
  Chassis *chassis{};
  Controller *controller{};
  PumpBank *pumps{};
  gripper::Engine *gripper_engine{};
  application::Application *application{};
  bool wheel_holding{}, reset_pending{};
  std::uint32_t job_id{}, job_started{}, job_finished{};
  std::array<bool, 254> job_servos{};
  std::array<bool, 16> job_pca{};
  std::uint32_t job_pump_select{}, job_valve_select{};
  Pose motion_origin{};
  std::uint32_t motion_started{}, motion_updated{};
  std::int32_t motion_encoder{};
  float motion_speed{};
  bool motion_stopping{};
  std::uint32_t shift_word{};
  bool encoder_seeded{}, startup_ready{}, pca_ready{};
  std::array<bool, 4> step_ready{};
  struct Deadline {
    bool active{};
    std::uint32_t command_at{};
  };
  std::array<Deadline, 254> servo_deadlines{};
  std::array<Deadline, 16> pca_deadlines{};
  std::array<Deadline, 8> pwm_deadlines{};
  std::array<Deadline, 4> dc_deadlines{};
  static Error step_direction(void *context, bool forward) {
    auto &device = *static_cast<StepDevice *>(context);
    return board::stepper_direction(device.channel, forward);
  }
  static Error step_reset(void *context, bool asserted) {
    auto &device = *static_cast<StepDevice *>(context);
    auto &h = *device.owner;
    const auto word = asserted ? h.shift_word & ~(1U << reset_bits[device.channel])
                               : h.shift_word | (1U << reset_bits[device.channel]);
    const auto error = board::shift_write(word);
    if (error == Error::ok) {
      h.shift_word = word;
    }
    return error;
  }
  static Error step_delay(void *, std::uint32_t us) { return board::delay_us(us); }
  bool diagnostic() const { return config.mode != board::DeploymentMode::OPERATIONAL_MODE; }
  Error initialize() {
    startup_ready = false;
    const auto pulls = board::set_input_pulldowns(config.input_pulldown_mask);
    if (pulls != Error::ok)
      return pulls;
    auto safe = stop();
    if (safe != Error::ok) {
      return safe;
    }
    if (diagnostic()) {
      startup_ready = true;
      return Error::ok; // Missing external devices and unreviewed motion limits
                        // are expected.
    }
    if (!config.valid()) {
      return Error::invalid_config;
    }
    if (config.pca_enabled) {
      auto error = pca.initialize();
      if (error != Error::ok) {
        return error;
      }
      pca_ready = true;
    }
    // NRST remains asserted until a validated, armed stepper command requests
    // preparation.
    startup_ready = true;
    return Error::ok;
  }
  static Error write_outputs(void *context, std::uint32_t pumps, std::uint32_t valves) {
    auto &h = *static_cast<Hardware *>(context);
    if (h.diagnostic())
      return Error::not_armed;
    if (!h.startup_ready || !h.config.valid()) {
      const auto safe = board::shift_write(0);
      if (safe == Error::ok) {
        h.shift_word = 0;
      }
      return safe == Error::ok ? Error::invalid_config : safe;
    }
    auto word = h.config.output_word(pumps, valves, h.shift_word);
    if (!word) {
      return Error::invalid_config;
    }
    auto error = board::shift_write(*word);
    if (error == Error::ok) {
      h.shift_word = *word;
    }
    return error;
  }
  static Error write_wheels(void *context, const std::array<std::int32_t, 3> &values) {
    auto &h = *static_cast<Hardware *>(context);
    if (h.diagnostic())
      return Error::not_armed;
    bool moving = false;
    for (auto value : values) {
      moving = moving || value != 0;
    }
    if (moving && (!h.startup_ready || !h.config.valid())) {
      return Error::invalid_config;
    }
    Error result = Error::ok;
    for (std::size_t i = 0; i < 3; ++i) {
      auto error = h.motors[i].set_tpm(values[i]);
      if (error != Error::ok) {
        result = error;
      }
      error = h.motors[i].enable(moving ||
                                 (h.wheel_holding && h.controller && h.controller->state().armed));
      if (error != Error::ok) {
        result = error;
      }
    }
    if (result != Error::ok) {
      for (auto &motor : h.motors) {
        (void)motor.set_tpm(0);
        (void)motor.enable(false);
      }
    }
    return result;
  }
  static Error stop_hook(void *context) {
    auto &hardware = *static_cast<Hardware *>(context);
    const auto stopped = hardware.stop();
    if (stopped != Error::ok) {
      return stopped;
    }
    // A failed startup cannot be cleared by a transport rearm that only writes
    // safe zeros.
    return hardware.startup_ready && (hardware.diagnostic() || hardware.config.valid())
               ? Error::ok
               : Error::invalid_config;
  }
  Error stop() {
    if (gripper_engine && gripper_engine->busy()) {
      job_finished = board::millis();
      const auto reason =
          controller && controller->state().estop
              ? Error::estop
              : (controller && controller->state().fault ? controller->state().last_error
                                                         : Error::not_armed);
      (void)gripper_engine->abort(reason);
      // abort invokes job_stop synchronously after publishing its terminal
      // state.
      return gripper_engine->state().stop_error;
    }
    Error result = Error::ok;
    wheel_holding = false;
    encoder_seeded = false;
    if (chassis) {
      chassis->invalidate_feedback();
    }
    servo_deadlines = {};
    pca_deadlines = {};
    pwm_deadlines = {};
    dc_deadlines = {};
    {
      StepGuard lock;
      for (std::size_t i = 0; i < planners.size(); ++i) {
        if (!diagnostic() && planners[i].stop() != Error::ok) {
          result = Error::output_failure;
        }
        // Removing holding torque can move the mechanism. A new home is
        // mandatory after stop.
        planners[i] = StepperPlanner{config.steppers[i], board::stepper_pulses(i)};
      }
    }
    // Reset steppers before any slower serial/I2C shutdown transactions.
    step_ready = {};
    shift_word = 0;
    if (board::shift_write(0) != Error::ok) {
      result = Error::output_failure;
    }
    if (!diagnostic() && write_wheels(this, {}) != Error::ok) {
      result = Error::output_failure;
    }
    for (std::uint8_t i = 0; i < 8; ++i) {
      if (board::pwm_disable(i) != Error::ok) {
        result = Error::output_failure;
      }
    }
    for (std::size_t id = 1; id < config.ax_ids.size(); ++id) {
      if (!diagnostic() && (config.ax_ids[id] || config.mx_ids[id])) {
        const std::array<std::uint8_t, 1> off{0};
        if (servo_protocol.write(static_cast<std::uint8_t>(id), 0x18, off) != Error::ok) {
          result = Error::output_failure;
        }
      }
    }
    if (!diagnostic() && config.pca_enabled && pca.full_off() != Error::ok) {
      pca_ready = false;
      result = Error::output_failure;
    }
    return result;
  }
  bool aux_expired(std::uint32_t now) const {
    const auto expired = [now](const auto &channels) {
      for (const auto &channel : channels) {
        if (channel.active && elapsed(now, channel.command_at, 250)) {
          return true;
        }
      }
      return false;
    };
    return expired(servo_deadlines) || expired(pca_deadlines) || expired(pwm_deadlines) ||
           expired(dc_deadlines);
  }
  Error prepare_stepper(std::uint8_t id) {
    if (step_ready[id]) {
      return Error::ok;
    }
    auto next = shift_word | (1U << reset_bits[id]);
    auto error = board::shift_write(next);
    if (error != Error::ok) {
      return error;
    }
    shift_word = next;
    error = board::delay_us(1000);
    if (error == Error::ok) {
      error = step_driver[id].enable(false);
    }
    const std::array<std::pair<std::uint8_t, std::uint32_t>, 5> settings{
        {{0x17, config.stepper_alarm_enable[id]},
         {0x18, config.stepper_driver_config[id]},
         {0x16, config.stepper_mode[id]},
         {0x09, config.stepper_current_tval[id]},
         {0x13, config.stepper_ocd_threshold[id]}}};
    for (const auto &[reg, value] : settings) {
      if (error == Error::ok) {
        error = step_driver[id].set_parameter(reg, value);
      }
    }
    if (error == Error::ok && config.stepper_toff_min[id] != 255) {
      error = step_driver[id].set_parameter(0x10, config.stepper_toff_min[id]);
    }
    if (error != Error::ok) {
      shift_word &= ~(1U << reset_bits[id]);
      const auto safe = board::shift_write(shift_word);
      return safe == Error::ok ? error : safe;
    }
    step_ready[id] = true;
    return Error::ok;
  }
  static Error step_tick(void *context, std::uint32_t now) {
    auto &hardware = *static_cast<Hardware *>(context);
    if (hardware.diagnostic())
      return Error::ok;
    Error result = Error::ok;
    for (std::size_t i = 0; i < 4; ++i) {
      if (hardware.config.stepper_enabled[i]) {
        const bool home =
            hardware.config.home_input[i] != 255 &&
            board::input(hardware.config.home_input[i]) == hardware.config.home_active_high[i];
        const auto error = hardware.planners[i].tick(now, home);
        if (error != Error::ok) {
          result = error;
        }
      }
    }
    return result;
  }
  Error read_encoders(std::array<std::int32_t, 3> &speed, std::array<std::int32_t, 3> &ticks) {
    bool valid = true;
    for (std::size_t i = 0; i < 3; ++i) {
      const auto velocity = motors[i].get_tpm(speed[i]);
      const auto position = motors[i].total_ticks(ticks[i]);
      valid = valid && velocity == Error::ok && position == Error::ok;
    }
    return valid ? Error::ok : Error::timeout;
  }
  Error poll_feedback(Chassis &target) {
    std::array<std::int32_t, 3> speed{}, ticks{}, delta{};
    const auto read = read_encoders(speed, ticks);
    if (read != Error::ok) {
      encoder_seeded = false;
      target.invalidate_feedback();
      return read;
    }
    if (encoder_seeded) {
      for (std::size_t i = 0; i < 3; ++i) {
        delta[i] = std::bit_cast<std::int32_t>(std::bit_cast<std::uint32_t>(ticks[i]) -
                                               std::bit_cast<std::uint32_t>(encoder_previous[i]));
      }
    } else {
      target.invalidate_feedback();
    }
    encoder_previous = ticks;
    encoder_seeded = true;
    return target.feedback(speed, delta);
  }
  Error reset_odometry() {
    if (!chassis) {
      return Error::invalid_config;
    }
    const auto &state = chassis->state();
    if (state.requested.vx != 0 || state.requested.vy != 0 || state.requested.wz != 0 ||
        !state.commanded_known) {
      return Error::invalid_argument;
    }
    for (auto value : state.wheel_commanded) {
      if (value != 0) {
        return Error::invalid_argument;
      }
    }
    std::array<std::int32_t, 3> speed{}, ticks{};
    const auto read = read_encoders(speed, ticks);
    if (read != Error::ok) {
      encoder_seeded = false;
      chassis->invalidate_feedback();
      return read;
    }
    for (auto value : speed) {
      if (value != 0) {
        return Error::invalid_argument;
      }
    }
    const auto feedback = chassis->feedback(speed, {});
    if (feedback != Error::ok) {
      return feedback;
    }
    encoder_previous = ticks;
    encoder_seeded = true;
    return chassis->reset_pose({});
  }
  static Error sensor_read(void *context, std::span<std::uint8_t> output, std::size_t &count,
                           std::uint32_t) {
    count = 0;
    if (output.size() < 24) {
      return Error::invalid_argument;
    }
    auto &hardware = *static_cast<Hardware *>(context);
    std::fill_n(output.begin(), 24, 0);
    Error battery_error = Error::unsupported;
    if (!hardware.diagnostic() && hardware.config.battery_enabled) {
      if (hardware.config.battery_id == 0 || hardware.config.battery_id >= 254) {
        battery_error = Error::invalid_config;
      } else {
        BatteryState battery{};
        battery_error =
            read_battery(hardware.battery_protocol, hardware.config.battery_id, battery);
        if (battery_error == Error::ok) {
          output[0] = 1;
          wire::write_u32(output.data() + 1, battery.power_mw);
          const auto put16 = [&output](unsigned offset, std::uint16_t value) {
            output[offset] = static_cast<std::uint8_t>(value);
            output[offset + 1] = static_cast<std::uint8_t>(value >> 8);
          };
          for (unsigned i = 0; i < 4; ++i) {
            put16(5 + 2 * i, battery.cell_mv[i]);
          }
          put16(13, battery.voltage_mv);
          put16(15, battery.current_ma);
          output[17] = battery.soc_percent;
          output[18] = battery.cells;
        }
      }
    }
    std::uint16_t inputs = 0;
    for (std::uint8_t i = 0; i < 13; ++i) {
      if (board::input(i)) {
        inputs |= static_cast<std::uint16_t>(1U << i);
      }
    }
    output[19] = static_cast<std::uint8_t>(inputs);
    output[20] = static_cast<std::uint8_t>(inputs >> 8);
    output[21] = 0xff;
    output[22] = 0x1f;
    output[23] = static_cast<std::uint8_t>(battery_error);
    count = 24;
    return Error::ok; // Optional BMS failure does not fabricate battery
                      // feedback or fault unrelated actuators.
  }
  static Error actuator(void *context, std::span<const std::uint8_t> bytes, std::uint32_t now) {
    auto &h = *static_cast<Hardware *>(context);
    if (h.diagnostic())
      return Error::not_armed;
    if (bytes.size() != 10) {
      return Error::invalid_argument;
    }
    const auto kind = bytes[0], id = bytes[1];
    const auto value = wire::read_f32(bytes.data() + 2), rate = wire::read_f32(bytes.data() + 6);
    if (!h.startup_ready || !h.config.valid()) {
      return Error::invalid_config;
    }
    if (!std::isfinite(value) || !std::isfinite(rate) || rate < 0) {
      return Error::invalid_argument;
    }
    Error error = Error::unsupported;
    if (kind == 1 || kind == 2 || kind == 7 || kind == 8) {
      const bool ax = kind == 1 || kind == 7;
      if (id >= h.config.ax_ids.size() || !(ax ? h.config.ax_ids[id] : h.config.mx_ids[id])) {
        return Error::unsupported;
      }
      DynamixelServo servo{h.servo_protocol,
                           id,
                           {ax ? ServoModel::ax_300 : ServoModel::mx_360,
                            h.config.servo_min_deg[id], h.config.servo_max_deg[id]}};
      if (kind == 7 || kind == 8) {
        if ((value != 0 && value != 1) || rate != 0) {
          return Error::invalid_argument;
        }
        error = servo.torque(value == 1);
        if (error == Error::ok) {
          h.servo_deadlines[id] = {value == 1, now};
        }
      } else {
        if (rate != 0) {
          return Error::unsupported;
        }
        error = servo.position_deg(value * 57.295779513F);
        if (error == Error::ok) {
          h.servo_deadlines[id] = {true, now};
        }
      }
    } else if (kind == 3) {
      if (id >= 16 || !h.config.pca_enabled || !h.config.pca_channels[id]) {
        return Error::unsupported;
      }
      if (rate != 0) {
        return Error::unsupported;
      }
      if (!h.pca_ready) {
        error = h.pca.initialize();
        if (error != Error::ok) {
          return error;
        }
        h.pca_ready = true;
      }
      error = h.pca.position_deg(id, value, h.config.pca_calibration[id]);
      if (error == Error::ok) {
        h.pca_deadlines[id] = {true, now};
      } else if (error != Error::invalid_argument) {
        h.pca_ready = false;
      }
    } else if (kind == 4) {
      if (id >= 8 || !h.config.pwm_channels[id]) {
        return Error::unsupported;
      }
      if (rate != 0) {
        return Error::unsupported;
      }
      const auto pulse = servo_pulse(value, h.config.pwm_calibration[id]);
      if (!pulse) {
        return Error::invalid_argument;
      }
      error = board::pwm_pulse(id, *pulse);
      if (error == Error::ok) {
        h.pwm_deadlines[id] = {true, now};
      }
    } else if (kind == 5 || kind == 11 || kind == 12) {
      if (id >= 4 || !h.config.stepper_enabled[id]) {
        return Error::unsupported;
      }
      if ((kind == 12 && (!h.config.stepper_profile_enabled[id] || rate != 0)) ||
          (kind != 12 && (rate < 1 || rate > h.config.steppers[id].maximum_steps_per_second ||
                          std::trunc(rate) != rate))) {
        return Error::invalid_argument;
      }
      if (kind == 11 && ((value != 0 && value != 1) || h.config.home_input[id] == 255 ||
                         (value == 1) != h.config.home_forward[id])) {
        return Error::invalid_argument;
      }
      std::int64_t target = 0;
      {
        StepGuard lock;
        if (h.planners[id].busy()) {
          return Error::lease_conflict;
        }
        if (kind == 5 || kind == 12) {
          if (value < std::numeric_limits<std::int32_t>::min() || value >= 2147483648.0F ||
              std::trunc(value) != value) {
            return Error::invalid_argument;
          }
          if (!h.planners[id].position_known()) {
            return Error::not_armed;
          }
          const auto steps = static_cast<std::int64_t>(value),
                     base = h.planners[id].commanded_steps();
          if ((steps > 0 && base > std::numeric_limits<std::int64_t>::max() - steps) ||
              (steps < 0 && base < std::numeric_limits<std::int64_t>::min() - steps)) {
            return Error::invalid_argument;
          }
          target = base + steps;
          if (target < h.config.steppers[id].minimum_steps ||
              target > h.config.steppers[id].maximum_steps) {
            return Error::invalid_argument;
          }
        }
      }
      error = h.prepare_stepper(id);
      if (error == Error::ok) {
        error = h.step_driver[id].enable(true);
      }
      if (error == Error::ok) {
        StepGuard lock;
        if (kind == 11) {
          if (h.config.stepper_profile_enabled[id]) {
            auto profile = h.config.stepper_profiles[id];
            profile.working_division = 16;
            profile.acceleration_ms = 500;
            profile.base_frequency_hz =
                std::min(profile.base_frequency_hz,
                         static_cast<std::uint32_t>(rate) * profile.working_division);
            error = h.planners[id].home_profile(value == 1, profile, 10000000, board::micros());
          } else {
            error = h.planners[id].home(value == 1, static_cast<std::uint32_t>(rate), 10000000,
                                        board::micros());
          }
        } else if (kind == 12) {
          error = h.planners[id].move_profile(target, h.config.stepper_profiles[id], 10000000,
                                              board::micros());
        } else {
          error = h.planners[id].move(target, static_cast<std::uint32_t>(rate), 10000000,
                                      board::micros());
        }
      }
      if (error != Error::ok) {
        (void)h.step_driver[id].enable(false);
      }
      // Finite stepper/home commands have their own 10s deadline; lease loss
      // still stops all outputs.
    } else if (kind == 6) {
      if (id >= 4 || !h.config.dc_channels[id]) {
        return Error::unsupported;
      }
      if (rate != 0) {
        return Error::unsupported;
      }
      error = board::dc_drive(id, value, h.config.dc_modes[id], h.config.dc_frequency_hz[id / 2]);
      if (error == Error::ok) {
        h.dc_deadlines[id] = {value != 0, now};
      }
    } else if (kind == 9) {
      if (!h.config.leds_enabled || id >= h.config.led_count || value < 0 || value > 16777215 ||
          std::trunc(value) != value || rate != 0) {
        return Error::invalid_argument;
      }
      const auto rgb = static_cast<std::uint32_t>(value);
      h.leds[id] = {static_cast<std::uint8_t>(rgb >> 16), static_cast<std::uint8_t>(rgb >> 8),
                    static_cast<std::uint8_t>(rgb)};
      error = board::leds_write(std::span{h.leds}.first(h.config.led_count));
    } else if (kind == 13) {
      if (id >= 4 || !h.config.dc_channels[id] || !h.config.dc_brake_configured[id] || rate != 0 ||
          (value != 0 && value != 1)) {
        return Error::invalid_argument;
      }
      error =
          value == 1
              ? board::dc_brake(id, h.config.dc_brake_high[id], h.config.dc_frequency_hz[id / 2])
              : board::dc_drive(id, 0, h.config.dc_modes[id], h.config.dc_frequency_hz[id / 2]);
      if (error == Error::ok) {
        h.dc_deadlines[id] = {value == 1, now};
      }
    } else if (kind == 14) {
      if (id >= 8 || !h.config.pwm_generic_channels[id] || value < 0 || value > 1 ||
          rate != h.config.pwm_frequency_hz[id / 4]) {
        return Error::invalid_argument;
      }
      error = board::pwm_duty(id, value, h.config.pwm_frequency_hz[id / 4]);
      if (error == Error::ok) {
        h.pwm_deadlines[id] = {value != 0, now};
      }
    } else if (kind == 15) {
      if (id >= 8 || (!h.config.pwm_generic_channels[id] && !h.config.pwm_channels[id]) ||
          value != 0 || rate != 0) {
        return Error::invalid_argument;
      }
      error = board::pwm_disable(id);
      if (error == Error::ok) {
        h.pwm_deadlines[id] = {};
      }
    } else if (kind == 10) {
      if (id != 0 || value != 0 || rate != 0) {
        return Error::invalid_argument;
      }
      error = h.reset_odometry();
    }
    return error;
  }
  static Error job_guard(void *context, std::uint32_t owner, std::uint32_t now) {
    auto &h = *static_cast<Hardware *>(context);
    if (h.diagnostic())
      return Error::not_armed;
    if (!h.controller || !h.startup_ready || !h.config.valid()) {
      return Error::invalid_config;
    }
    if (h.reset_pending || (h.application && !h.application->state().communication_enabled)) {
      return Error::not_armed;
    }
    const auto &state = h.controller->state();
    if (state.estop) {
      return Error::estop;
    }
    if (!state.armed || state.fault) {
      return Error::not_armed;
    }
    if (state.owner != owner) {
      return Error::lease_conflict;
    }
    return h.controller->lease_active(now) ? Error::ok : Error::lease_expired;
  }
  static Error job_validate(void *context, const gripper::Step &step) {
    auto &h = *static_cast<Hardware *>(context);
    using enum gripper::Action;
    if (step.count > step.targets.size() || step.timeout_ms == 0 || step.timeout_ms > 60000 ||
        !std::isfinite(step.tolerance) || step.tolerance < 0) {
      return Error::invalid_argument;
    }
    const auto mask = h.config.channels == 32 ? 0xffffffffU : (1U << h.config.channels) - 1;
    if (step.action == outputs) {
      for (std::size_t i = 0; i < h.config.channels; ++i) {
        const auto bit = 1U << i;
        if (((step.pump_select & bit) != 0 && h.config.pump_outputs[i] == 0) ||
            ((step.valve_select & bit) != 0 && h.config.valve_outputs[i] == 0)) {
          return Error::invalid_config;
        }
      }
      return ((step.pump_select | step.pump_values | step.valve_select | step.valve_values) &
              ~mask) == 0 &&
                     (step.pump_values & ~step.pump_select) == 0 &&
                     (step.valve_values & ~step.valve_select) == 0
                 ? Error::ok
                 : Error::invalid_argument;
    }
    if (step.action == wait_input) {
      return step.input_select != 0 && (step.input_select & ~0x1fffU) == 0 &&
                     (step.input_values & ~step.input_select) == 0
                 ? Error::ok
                 : Error::invalid_config;
    }
    if (step.action == timed_drive) {
      const auto &v = step.drive.velocity;
      return step.drive.duration_ms > 0 && step.drive.duration_ms < step.timeout_ms &&
                     std::isfinite(v.vx) && std::isfinite(v.vy) && std::isfinite(v.wz) &&
                     std::hypot(v.vx, v.vy) <= h.config.chassis.max_linear_mps &&
                     std::fabs(v.wz) <= h.config.chassis.max_angular_radps
                 ? Error::ok
                 : Error::invalid_argument;
    }
    if (step.action == relative_drive) {
      const auto &d = step.drive;
      if (!h.chassis || !h.chassis->state().pose_known || h.chassis->state().measured_known != 7) {
        return Error::not_armed;
      }
      return d.axis <= 1 && std::isfinite(d.displacement_m) && std::isfinite(d.maximum_speed_mps) &&
                     d.maximum_speed_mps > 0 &&
                     d.maximum_speed_mps <= h.config.chassis.max_linear_mps &&
                     std::isfinite(d.minimum_speed_mps) && d.minimum_speed_mps > 0 &&
                     d.minimum_speed_mps <= d.maximum_speed_mps && std::isfinite(d.tolerance_m) &&
                     d.tolerance_m > 0 && std::isfinite(d.acceleration_mps2) &&
                     d.acceleration_mps2 > 0 &&
                     d.acceleration_mps2 <= h.config.chassis.max_accel_mps2 &&
                     std::isfinite(d.heading_kp) && d.heading_kp >= 0 &&
                     std::isfinite(d.lateral_kp) && d.lateral_kp >= 0 &&
                     std::isfinite(d.maximum_angular_radps) && d.maximum_angular_radps > 0 &&
                     d.maximum_angular_radps <= h.config.chassis.max_angular_radps
                 ? Error::ok
                 : Error::invalid_config;
    }
    if (step.action == wheel_calibration) {
      if (step.drive.axis >= 3 || step.drive.wheel_tpm == 0 || step.drive.wheel_goal_ticks <= 0 ||
          step.drive.wheel_stop_margin < 0 ||
          step.drive.wheel_stop_margin >= step.drive.wheel_goal_ticks ||
          std::abs(static_cast<std::int64_t>(step.drive.wheel_tpm)) >
              h.config.chassis.max_wheel_tpm ||
          !h.encoder_seeded || h.chassis->state().measured_known != 7) {
        return Error::invalid_config;
      }
      const auto velocity = h.calibration_velocity(step.drive);
      return velocity &&
                     std::hypot(velocity->vx, velocity->vy) <= h.config.chassis.max_linear_mps &&
                     std::fabs(velocity->wz) <= h.config.chassis.max_angular_radps
                 ? Error::ok
                 : Error::invalid_config;
    }
    for (std::size_t i = 0; i < step.count; ++i) {
      const auto &target = step.targets[i];
      if (!std::isfinite(target.value) || !std::isfinite(target.rate)) {
        return Error::invalid_argument;
      }
      for (std::size_t j = 0; j < i; ++j) {
        if (target.id == step.targets[j].id && target.device == step.targets[j].device) {
          return Error::invalid_config;
        }
      }
      if (step.action == servo_group) {
        const bool ax = target.device == gripper::Device::ax;
        if ((!ax && target.device != gripper::Device::mx) || target.id == 0 || target.id >= 254 ||
            !(ax ? h.config.ax_ids[target.id] : h.config.mx_ids[target.id]) ||
            target.value < h.config.servo_min_deg[target.id] ||
            target.value > h.config.servo_max_deg[target.id] || target.rate < 1 ||
            target.rate > 1023 || std::trunc(target.rate) != target.rate) {
          return Error::invalid_config;
        }
      } else if (step.action == pca_group) {
        if (target.device != gripper::Device::pca || target.id >= 16 || !h.config.pca_enabled ||
            !h.config.pca_channels[target.id] ||
            !servo_pulse(target.value, h.config.pca_calibration[target.id])) {
          return Error::invalid_config;
        }
      } else if (step.action == stepper_current || step.action == stepper_move ||
                 step.action == stepper_disable) {
        if (target.device != gripper::Device::stepper || target.id >= 4 ||
            !h.config.stepper_enabled[target.id]) {
          return Error::invalid_config;
        }
        if (step.action == stepper_move &&
            (target.position_steps < h.config.steppers[target.id].minimum_steps ||
             target.position_steps > h.config.steppers[target.id].maximum_steps ||
             target.rate < 1 ||
             target.rate > h.config.steppers[target.id].maximum_steps_per_second ||
             std::trunc(target.rate) != target.rate)) {
          return Error::invalid_argument;
        }
        if (step.action == stepper_current &&
            (target.value < 31.25F || target.value > 4000 ||
             (step.alternate_current_ma != 0 &&
              (step.alternate_current_ma < 31.25F || step.alternate_current_ma > 4000)))) {
          return Error::invalid_argument;
        }
        StepGuard lock;
        if (step.action != stepper_disable && !h.planners[target.id].position_known()) {
          return Error::not_armed;
        }
      }
    }
    return Error::ok;
  }
  std::optional<Twist> calibration_velocity(const gripper::Drive &drive) const {
    auto geometry = config.chassis.geometry;
    // Invert the command model, excluding empirical odometry corrections.
    geometry.odometry_scale = geometry.command_scale;
    geometry.odometry_x_scale = geometry.odometry_y_scale = 1;
    geometry.angular_odometry_scale = 1 / geometry.angular_command_scale;
    geometry.wheel_odometry_scale = {1, 1, 1};
    std::array<std::int32_t, 3> wheels{};
    if (drive.axis >= wheels.size()) {
      return {};
    }
    wheels[drive.axis] = drive.wheel_tpm;
    return OmniKinematics{geometry}.body_speed(wheels);
  }
  static Error job_issue(void *context, const gripper::Step &step, std::uint32_t now) {
    auto &h = *static_cast<Hardware *>(context);
    if (h.diagnostic())
      return Error::not_armed;
    using enum gripper::Action;
    if (step.action == chassis_stop) {
      return h.chassis->stop();
    }
    if (step.action == timed_drive || step.action == relative_drive ||
        step.action == wheel_calibration) {
      h.motion_started = h.motion_updated = now;
      h.motion_origin = h.chassis->state().pose;
      h.motion_speed = 0;
      h.motion_stopping = false;
      if (step.action == wheel_calibration) {
        h.motion_encoder = h.encoder_previous[step.drive.axis];
      }
      return Error::ok;
    }
    if (step.action == outputs) {
      h.job_pump_select |= step.pump_select;
      h.job_valve_select |= step.valve_select;
      return h.pumps->set(step.pump_select, step.pump_values, step.valve_select, step.valve_values,
                          now);
    }
    std::array<std::uint8_t, 4> ids{};
    std::array<std::uint8_t, 8> positions{}, speeds{};
    std::array<std::uint8_t, 4> torque{1, 1, 1, 1};
    if (step.action == servo_group) {
      for (std::size_t i = 0; i < step.count; ++i) {
        const auto &target = step.targets[i];
        ids[i] = target.id;
        const bool ax = target.device == gripper::Device::ax;
        const auto raw = static_cast<std::uint16_t>(
            std::lround(target.value * (ax ? 1023.0F / 300.0F : 4095.0F / 360.0F)));
        positions[2 * i] = static_cast<std::uint8_t>(raw);
        positions[2 * i + 1] = static_cast<std::uint8_t>(raw >> 8);
        const auto speed = static_cast<std::uint16_t>(target.rate);
        speeds[2 * i] = static_cast<std::uint8_t>(speed);
        speeds[2 * i + 1] = static_cast<std::uint8_t>(speed >> 8);
        h.job_servos[target.id] = true;
        h.servo_deadlines[target.id] = {true, now};
      }
      const auto selected = std::span{ids}.first(step.count);
      auto error =
          h.servo_protocol.sync_write(0x20, 2, selected, std::span{speeds}.first(step.count * 2));
      if (error == Error::ok) {
        error = h.servo_protocol.sync_write(0x1e, 2, selected,
                                            std::span{positions}.first(step.count * 2));
      }
      if (error == Error::ok) {
        error = h.servo_protocol.sync_write(0x18, 1, selected, std::span{torque}.first(step.count));
      }
      return error;
    }
    for (std::size_t i = 0; i < step.count; ++i) {
      const auto &target = step.targets[i];
      Error error = Error::ok;
      if (step.action == pca_group) {
        if (!h.pca_ready) {
          error = h.pca.initialize();
          h.pca_ready = error == Error::ok;
        }
        if (error == Error::ok) {
          error = h.pca.position_deg(target.id, target.value, h.config.pca_calibration[target.id]);
        }
        if (error == Error::ok) {
          h.job_pca[target.id] = true;
          h.pca_deadlines[target.id] = {true, now};
        } else if (error != Error::invalid_argument) {
          h.pca_ready = false;
        }
      } else if (step.action == stepper_current || step.action == stepper_move) {
        error = h.prepare_stepper(target.id);
        if (error == Error::ok && step.action == stepper_current) {
          float current = target.value;
          {
            StepGuard lock;
            if (step.alternate_current_ma != 0 &&
                target.position_steps < h.planners[target.id].commanded_steps()) {
              current = step.alternate_current_ma;
            }
          }
          error = h.step_driver[target.id].set_current_ma(current);
          if (error == Error::ok && step.enable_before_settle) {
            error = h.step_driver[target.id].enable(true);
          }
        } else if (error == Error::ok) {
          error = h.step_driver[target.id].enable(true);
          if (error == Error::ok) {
            StepGuard lock;
            if (h.config.stepper_profile_enabled[target.id]) {
              auto profile = h.config.stepper_profiles[target.id];
              profile.base_frequency_hz =
                  std::min(profile.base_frequency_hz,
                           static_cast<std::uint32_t>(target.rate) * profile.working_division);
              error = h.planners[target.id].move_profile(target.position_steps, profile,
                                                         step.timeout_ms * 1000U, board::micros());
            } else {
              error = h.planners[target.id].move(target.position_steps,
                                                 static_cast<std::uint32_t>(target.rate),
                                                 step.timeout_ms * 1000U, board::micros());
            }
          }
        }
      } else if (step.action == stepper_disable) {
        {
          StepGuard lock;
          error = h.planners[target.id].stop();
        }
        const auto disabled = h.step_driver[target.id].enable(false);
        if (disabled != Error::ok) {
          error = disabled;
        }
        // Removing holding current invalidates an open-loop lift reference.
        {
          StepGuard lock;
          h.planners[target.id] =
              StepperPlanner{h.config.steppers[target.id], board::stepper_pulses(target.id)};
        }
      }
      if (error != Error::ok) {
        return error;
      }
    }
    return Error::ok;
  }
  static Error job_feedback(void *context, const gripper::Step &step,
                            gripper::Completion &completion, std::uint32_t now) {
    auto &h = *static_cast<Hardware *>(context);
    using enum gripper::Action;
    completion = {true, true, gripper::Evidence::commanded};
    if (step.action == timed_drive || step.action == relative_drive ||
        step.action == wheel_calibration) {
      completion.done = false;
      completion.evidence =
          step.action == timed_drive ? gripper::Evidence::settled : gripper::Evidence::measured;
      Twist velocity{};
      bool reached = false;
      if (step.action == timed_drive) {
        reached = elapsed(now, h.motion_started, step.drive.duration_ms);
        const float ramp = std::min<std::uint32_t>(step.drive.duration_ms, 350);
        const float scale = std::min(1.0F, static_cast<float>(now - h.motion_started) / ramp);
        velocity = {step.drive.velocity.vx * scale, step.drive.velocity.vy * scale,
                    step.drive.velocity.wz * scale};
      } else if (step.action == relative_drive) {
        const auto &state = h.chassis->state();
        if (!state.pose_known || state.measured_known != 7) {
          completion = {};
          return Error::timeout;
        }
        const auto dx = state.pose.x - h.motion_origin.x, dy = state.pose.y - h.motion_origin.y;
        const float c = std::cos(h.motion_origin.yaw), s = std::sin(h.motion_origin.yaw);
        const float local_x = dx * c + dy * s, local_y = -dx * s + dy * c;
        const auto &d = step.drive;
        const float error = d.displacement_m - (d.axis == 0 ? local_x : local_y);
        reached = std::fabs(error) <= d.tolerance_m;
        const float dt = static_cast<float>(now - h.motion_updated) * 0.001F;
        const float limit = std::clamp(std::sqrt(2 * d.acceleration_mps2 * std::fabs(error)),
                                       d.minimum_speed_mps, d.maximum_speed_mps);
        h.motion_speed = std::min(limit, h.motion_speed + d.acceleration_mps2 * dt);
        const float translation = std::copysign(h.motion_speed, error);
        const float angular =
            d.axis == 0
                ? d.heading_kp * std::remainder(h.motion_origin.yaw - state.pose.yaw, 6.283185307F)
                : d.lateral_kp * std::atan(std::clamp(-local_x / std::max(std::fabs(error), 0.05F),
                                                      -0.8F, 0.8F));
        // Translation is expressed in the axis frame captured at the start of
        // this segment.
        const float relative_yaw = h.motion_origin.yaw - state.pose.yaw;
        const float vx0 = d.axis == 0 ? translation : 0, vy0 = d.axis == 1 ? translation : 0;
        velocity = {vx0 * std::cos(relative_yaw) - vy0 * std::sin(relative_yaw),
                    vx0 * std::sin(relative_yaw) + vy0 * std::cos(relative_yaw),
                    std::clamp(angular, -d.maximum_angular_radps, d.maximum_angular_radps)};
      } else {
        if (!h.encoder_seeded || h.chassis->state().measured_known != 7) {
          completion = {};
          return Error::timeout;
        }
        const auto delta = std::bit_cast<std::int32_t>(
            std::bit_cast<std::uint32_t>(h.encoder_previous[step.drive.axis]) -
            std::bit_cast<std::uint32_t>(h.motion_encoder));
        const auto directed = step.drive.wheel_tpm > 0 ? static_cast<std::int64_t>(delta)
                                                       : -static_cast<std::int64_t>(delta);
        reached = directed >= static_cast<std::int64_t>(step.drive.wheel_goal_ticks) -
                                  step.drive.wheel_stop_margin;
        const auto command = h.calibration_velocity(step.drive);
        if (!command) {
          return Error::invalid_config;
        }
        velocity = *command;
      }
      h.motion_updated = now;
      if (step.action == relative_drive && h.motion_stopping && !reached &&
          h.chassis->state().measured_known == 7 &&
          h.chassis->state().wheel_measured == std::array<std::int32_t, 3>{}) {
        h.motion_stopping = false;
        h.motion_speed = 0;
      }
      h.motion_stopping = h.motion_stopping || reached;
      if (h.motion_stopping) {
        const auto stopped = h.chassis->stop();
        if (stopped != Error::ok) {
          return stopped;
        }
        // Timed motion proves bounded command execution; measured modes
        // additionally wait for rest.
        completion.done = step.action == timed_drive ||
                          (h.chassis->state().measured_known == 7 &&
                           h.chassis->state().wheel_measured == std::array<std::int32_t, 3>{});
        return Error::ok;
      }
      return h.chassis->set(velocity, now);
    } else if (step.action == servo_group) {
      completion.evidence = gripper::Evidence::measured;
      for (std::size_t i = 0; i < step.count; ++i) {
        const auto &target = step.targets[i];
        DynamixelServo servo{
            h.servo_protocol,
            target.id,
            {target.device == gripper::Device::ax ? ServoModel::ax_300 : ServoModel::mx_360,
             h.config.servo_min_deg[target.id], h.config.servo_max_deg[target.id]}};
        ServoFeedback feedback{};
        const auto error = servo.feedback(feedback);
        if (error != Error::ok) {
          completion = {};
          return error;
        }
        completion.done = completion.done && !feedback.moving &&
                          std::fabs(feedback.position_deg - target.value) <= step.tolerance;
      }
    } else if (step.action == stepper_move) {
      completion.evidence = gripper::Evidence::referenced_steps;
      StepGuard lock;
      for (std::size_t i = 0; i < step.count; ++i) {
        const auto &target = step.targets[i];
        completion.known = completion.known && h.planners[target.id].position_known();
        completion.done = completion.done && !h.planners[target.id].busy() &&
                          h.planners[target.id].commanded_steps() == target.position_steps;
      }
    } else if (step.action == wait_input) {
      std::uint32_t inputs = 0;
      for (std::uint8_t i = 0; i < 13; ++i) {
        if (board::input(i)) {
          inputs |= 1U << i;
        }
      }
      completion.done = (inputs & step.input_select) == step.input_values;
      completion.evidence = gripper::Evidence::measured;
    } else if (step.action == pca_group || step.action == delay) {
      completion.evidence = gripper::Evidence::settled;
    }
    return Error::ok;
  }
  static Error job_stop(void *context, Error) {
    auto &h = *static_cast<Hardware *>(context);
    auto error = h.pumps ? h.pumps->stop() : Error::ok;
    const auto wheels = h.chassis ? h.chassis->stop() : Error::ok;
    const auto auxiliary = h.stop();
    if (wheels != Error::ok) {
      error = wheels;
    }
    return auxiliary != Error::ok ? auxiliary : error;
  }
  Error job_keepalive(std::uint32_t now) {
    const auto guard = job_guard(this, gripper_engine->state().owner, now);
    if (guard != Error::ok) {
      return guard;
    }
    // The complete accepted plan owns these resources, including an existing
    // hold that a later step replaces. It never enables an inactive servo or an
    // unrelated channel.
    for (std::size_t i = 0; i < job_servos.size(); ++i) {
      if (job_servos[i] && servo_deadlines[i].active) {
        servo_deadlines[i].command_at = now;
      }
    }
    for (std::size_t i = 0; i < job_pca.size(); ++i) {
      if (job_pca[i] && pca_deadlines[i].active) {
        pca_deadlines[i].command_at = now;
      }
    }
    if (job_pump_select != 0 || job_valve_select != 0) {
      const auto &state = pumps->state();
      return pumps->set(job_pump_select, state.requested & job_pump_select, job_valve_select,
                        state.valves_requested & job_valve_select, now);
    }
    return Error::ok;
  }
  void job_tick(std::uint32_t now) {
    if (!gripper_engine || !gripper_engine->busy()) {
      return;
    }
    const auto tick = gripper_engine->tick(now);
    if (!gripper_engine->busy()) {
      job_finished = now;
    }
    if (tick != Error::ok) {
      (void)controller->fail(tick);
      return;
    }
    if (!gripper_engine->busy()) {
      return;
    }
    const auto renewed = job_keepalive(now);
    if (renewed != Error::ok) {
      (void)controller->fail(renewed);
    }
  }
  bool job_owns_motion() const {
    if (!gripper_engine || !gripper_engine->busy()) {
      return false;
    }
    for (const auto &step : gripper_engine->plan()) {
      if (step.action == gripper::Action::timed_drive ||
          step.action == gripper::Action::relative_drive ||
          step.action == gripper::Action::wheel_calibration ||
          step.action == gripper::Action::chassis_stop) {
        return true;
      }
    }
    return false;
  }
  static Error command_guard(void *context, std::uint8_t type,
                             std::span<const std::uint8_t> payload) {
    const auto &h = *static_cast<Hardware *>(context);
    if (h.diagnostic()) {
      if (type == static_cast<std::uint8_t>(wire::Type::rearm) ||
          type == static_cast<std::uint8_t>(wire::Type::twist) ||
          type == static_cast<std::uint8_t>(wire::Type::pumps) ||
          type == static_cast<std::uint8_t>(wire::Type::actuator) || type == 0x21 || type == 0x23)
        return Error::not_armed;
      if (type == 0x24 && !payload.empty() &&
          maintenance::access(payload[0]) != maintenance::Access::read_only)
        return Error::not_armed;
    }
    if (h.reset_pending) {
      return Error::not_armed;
    }
    if (h.application && !h.application->state().communication_enabled && type != 0x24) {
      return Error::not_armed;
    }
    if (!h.gripper_engine || !h.gripper_engine->busy()) {
      return Error::ok;
    }
    if (type == static_cast<std::uint8_t>(wire::Type::twist)) {
      return h.job_owns_motion() ? Error::lease_conflict : Error::ok;
    }
    if (type == static_cast<std::uint8_t>(wire::Type::pumps) && payload.size() == 16) {
      return ((wire::read_u32(payload.data()) & h.job_pump_select) != 0 ||
              (wire::read_u32(payload.data() + 8) & h.job_valve_select) != 0)
                 ? Error::lease_conflict
                 : Error::ok;
    }
    if (type == static_cast<std::uint8_t>(wire::Type::actuator) && payload.size() == 10) {
      const auto kind = payload[0], id = payload[1];
      if ((kind == 1 || kind == 2 || kind == 7 || kind == 8) && id < 254) {
        return h.job_servos[id] ? Error::lease_conflict : Error::ok;
      }
      if (kind == 3 && id < 16) {
        return h.job_pca[id] ? Error::lease_conflict : Error::ok;
      }
      if ((kind == 5 || kind == 11 || kind == 12) && id < 4) {
        for (const auto &step : h.gripper_engine->plan()) {
          for (std::size_t i = 0; i < step.count; ++i) {
            if (step.targets[i].device == gripper::Device::stepper && step.targets[i].id == id) {
              return Error::lease_conflict;
            }
          }
        }
        return Error::ok;
      }
      if (kind == 4 || kind == 6 || kind == 9 || kind == 13 || kind == 14 || kind == 15) {
        return Error::ok;
      }
      if (kind == 10) {
        return h.job_owns_motion() ? Error::lease_conflict : Error::ok;
      }
    }
    return Error::lease_conflict;
  }
  Error begin_job(std::uint32_t owner, gripper::Request request, std::uint32_t now) {
    auto error = gripper_engine->start(owner, request, now);
    if (error != Error::ok) {
      return error;
    }
    ++job_id;
    if (job_id == 0) {
      ++job_id;
    }
    job_started = job_finished = now;
    job_servos = {};
    job_pca = {};
    job_pump_select = job_valve_select = 0;
    for (const auto &step : gripper_engine->plan()) {
      job_pump_select |= step.pump_select;
      job_valve_select |= step.valve_select;
      for (std::size_t i = 0; i < step.count; ++i) {
        const auto &target = step.targets[i];
        if (step.action == gripper::Action::servo_group) {
          job_servos[target.id] = true;
        }
        if (step.action == gripper::Action::pca_group) {
          job_pca[target.id] = true;
        }
      }
    }
    error = job_keepalive(now);
    if (error != Error::ok) {
      (void)controller->fail(error);
    }
    return error;
  }
  static Error application_guard(void *context, std::uint32_t now) {
    auto &h = *static_cast<Hardware *>(context);
    return h.controller ? job_guard(&h, h.controller->state().owner, now) : Error::not_armed;
  }
  static Error application_stop(void *context, application::StopReason) {
    auto &h = *static_cast<Hardware *>(context);
    return h.controller ? h.controller->emergency_stop() : h.stop();
  }
  static Error application_action(void *context, application::LocalAction action,
                                  std::uint32_t now) {
    auto &h = *static_cast<Hardware *>(context);
    if (application_guard(&h, now) != Error::ok) {
      return Error::not_armed;
    }
    if (action == application::LocalAction::enable_motors) {
      h.wheel_holding = true;
      return h.chassis->stop();
    }
    if (!h.gripper_engine || h.gripper_engine->busy()) {
      return Error::lease_conflict;
    }
    gripper::Request request{};
    request.operation = gripper::Operation::side_grips;
    request.gripper = 1;
    request.value = 0;
    request.timeout_ms = 1500;
    return h.begin_job(h.controller->state().owner, request, now);
  }
  static Error maintenance_request(void *context, std::span<const std::uint8_t> input,
                                   std::span<std::uint8_t> output, std::size_t &size,
                                   std::uint32_t now) {
    auto &h = *static_cast<Hardware *>(context);
    size = 0;
    if (input.size() != 22 || output.size() < 96 || !h.controller || !h.chassis) {
      return Error::malformed;
    }
    const auto operation = static_cast<maintenance::Operation>(input[0]);
    if (h.diagnostic() && maintenance::access(input[0]) != maintenance::Access::read_only)
      return Error::not_armed;
    const auto channel = input[1];
    const auto argument = wire::read_u32(input.data() + 2),
               secondary = wire::read_u32(input.data() + 6);
    const std::array<float, 3> values{wire::read_f32(input.data() + 10),
                                      wire::read_f32(input.data() + 14),
                                      wire::read_f32(input.data() + 18)};
    for (auto value : values) {
      if (!std::isfinite(value)) {
        return Error::invalid_argument;
      }
    }
    const auto put16 = [&](std::size_t offset, std::uint16_t value) {
      output[offset] = static_cast<std::uint8_t>(value);
      output[offset + 1] = static_cast<std::uint8_t>(value >> 8);
    };
    const auto &state = h.controller->state();
    using enum maintenance::Operation;
    switch (operation) {
    case deployment_info:
      output[0] = 1; // Schema version. Wire response prepends the existing Error byte.
      output[1] = static_cast<std::uint8_t>(h.config.mode);
      output[2] = static_cast<std::uint8_t>(h.config.hardware_profile);
      output[3] = h.config.commissioned;
      output[4] = h.config.valid();
      output[5] = h.config.channels;
      output[6] = h.config.max_active;
      size = 7;
      return Error::ok;
    case board_info:
      output[0] = h.application && h.application->state().reset_latched;
      output[1] = state.armed;
      output[2] = state.estop;
      output[3] = state.fault;
      wire::write_u32(output.data() + 4, board::reset_cause());
      wire::write_u32(output.data() + 8, now);
      wire::write_u32(output.data() + 12, state.owner);
      size = 16;
      return Error::ok;
    case board_reset: {
      if (!h.application) {
        return Error::invalid_config;
      }
      const auto error = h.application->request_reset(now);
      if (error == Error::ok) {
        h.reset_pending = true;
      }
      return error;
    }
    case clear_reset:
      if (h.application) {
        h.application->clear_reset();
      }
      board::clear_reset_cause();
      return Error::ok;
    case wheel_ticks:
    case wheel_path:
    case reset_wheel_ticks: {
      if (!h.startup_ready || (!h.diagnostic() && !h.config.valid())) {
        return Error::invalid_config;
      }
      if (operation == reset_wheel_ticks) {
        const auto stationary = h.reset_odometry();
        if (stationary != Error::ok) {
          return stationary;
        }
        for (auto &motor : h.motors) {
          const auto error = motor.set_total_ticks(0);
          if (error != Error::ok) {
            h.encoder_seeded = false;
            h.chassis->invalidate_feedback();
            return error;
          }
        }
        h.encoder_seeded = false;
        h.chassis->invalidate_feedback();
      }
      std::array<std::int32_t, 3> speed{}, ticks{};
      const auto error = h.read_encoders(speed, ticks);
      if (error != Error::ok) {
        return error;
      }
      if (operation == reset_wheel_ticks && ticks != std::array<std::int32_t, 3>{}) {
        return Error::output_failure;
      }
      output[0] = 7;
      const auto &geometry = h.config.chassis.geometry;
      const auto factor =
          geometry.odometry_scale * 6.283185307F * geometry.wheel_radius_m /
          (geometry.gear_ratio * static_cast<float>(geometry.encoder_ticks_per_motor_turn));
      for (std::size_t i = 0; i < 3; ++i) {
        if (operation == wheel_path) {
          wire::write_f32(output.data() + 1 + 4 * i,
                          static_cast<float>(ticks[i]) * factor * geometry.wheel_odometry_scale[i]);
        } else {
          wire::write_u32(output.data() + 1 + 4 * i, std::bit_cast<std::uint32_t>(ticks[i]));
        }
      }
      size = 13;
      return Error::ok;
    }
    case set_pose: {
      const auto error = h.reset_odometry();
      return error == Error::ok ? h.chassis->reset_pose({values[0], values[1], values[2]}) : error;
    }
    case servo_probe: {
      // Discovery is read-only and deliberately independent of
      // commissioning/allowlists. A free bus is required; probing must never
      // delay an active owner's commands.
      if (channel == 0 || channel >= 254 || argument != 0 || secondary != 0 ||
          values != std::array<float, 3>{}) {
        return Error::invalid_argument;
      }
      if (state.armed || h.controller->lease_active(now)) {
        return Error::lease_conflict;
      }
      std::array<std::uint8_t, 18> identity{};
      std::array<std::uint8_t, 1> torque{};
      std::array<std::uint8_t, 2> position{};
      auto error = h.servo_protocol.read(channel, 0, identity);
      if (error == Error::ok) {
        error = h.servo_protocol.read(channel, 24, torque);
      }
      if (error == Error::ok) {
        error = h.servo_protocol.read(channel, 36, position);
      }
      if (error != Error::ok) {
        return error;
      }
      if (identity[3] != channel || torque[0] > 1 || identity[16] > 2) {
        return Error::malformed;
      }
      std::copy_n(identity.begin(), 4, output.begin());
      std::copy_n(identity.begin() + 6, 4, output.begin() + 4);
      std::copy(position.begin(), position.end(), output.begin() + 8);
      output[10] = torque[0];
      output[11] = identity[16];
      size = 12;
      return Error::ok;
    }
    case servo_feedback:
    case servo_led:
    case servo_speed:
    case servo_torque_limit:
    case servo_wheel_mode:
    case servo_id: {
      if (channel == 0 || channel >= 254 ||
          (!h.config.ax_ids[channel] && !h.config.mx_ids[channel])) {
        return Error::invalid_config;
      }
      DynamixelServo servo{h.servo_protocol,
                           channel,
                           {h.config.ax_ids[channel] ? ServoModel::ax_300 : ServoModel::mx_360,
                            h.config.servo_min_deg[channel], h.config.servo_max_deg[channel]}};
      if (operation == servo_feedback) {
        ServoFeedback feedback{};
        const auto error = servo.feedback(feedback);
        if (error != Error::ok) {
          return error;
        }
        output[0] = 1;
        wire::write_f32(output.data() + 1, feedback.position_deg);
        put16(5, std::bit_cast<std::uint16_t>(feedback.speed_raw));
        put16(7, std::bit_cast<std::uint16_t>(feedback.load_raw));
        output[9] = feedback.voltage_decivolts;
        output[10] = feedback.temperature_c;
        output[11] = feedback.moving;
        size = 12;
        return Error::ok;
      }
      if ((operation == servo_led || operation == servo_wheel_mode) && argument > 1) {
        return Error::invalid_argument;
      }
      if (operation == servo_led) {
        return servo.led(argument == 1);
      }
      if (operation == servo_wheel_mode) {
        return servo.wheel_mode(argument == 1);
      }
      if (operation == servo_speed) {
        const auto speed = std::bit_cast<std::int32_t>(argument);
        if (speed < -1023 || speed > 1023) {
          return Error::invalid_argument;
        }
        auto error = servo.speed_raw(static_cast<std::int16_t>(speed));
        std::int16_t actual{};
        if (error == Error::ok) {
          error = servo.moving_speed_raw(actual);
        }
        return error == Error::ok && actual != speed ? Error::output_failure : error;
      }
      if (operation == servo_torque_limit) {
        return argument <= 1023 ? servo.torque_limit(static_cast<std::uint16_t>(argument))
                                : Error::invalid_argument;
      }
      if (argument == 0 || argument >= 254 || h.config.ax_ids[argument] ||
          h.config.mx_ids[argument]) {
        return Error::invalid_argument;
      }
      return servo.set_id(static_cast<std::uint8_t>(argument));
    }
    case motor_range:
    case set_motor_range:
    case motor_id:
      if (channel >= 3) {
        return Error::invalid_argument;
      }
      if (operation == motor_id) {
        if (argument == 0 || argument >= 254) {
          return Error::invalid_argument;
        }
        for (const auto &motor : h.motors) {
          if (motor.id() == argument) {
            return Error::invalid_argument;
          }
        }
        return h.motors[channel].set_id(static_cast<std::uint8_t>(argument));
      }
      if (operation == set_motor_range) {
        return h.motors[channel].set_tpm_range(argument, secondary);
      }
      {
        std::uint32_t lower{}, upper{};
        const auto error = h.motors[channel].tpm_range(lower, upper);
        if (error == Error::ok) {
          wire::write_u32(output.data(), lower);
          wire::write_u32(output.data() + 4, upper);
          size = 8;
        }
        return error;
      }
    case stepper_reset:
    case stepper_parameter:
    case stepper_set_parameter: {
      if (channel >= 4 || !h.config.stepper_enabled[channel]) {
        return Error::invalid_config;
      }
      if (operation == stepper_reset) {
        {
          StepGuard lock;
          (void)h.planners[channel].stop();
          h.planners[channel] =
              StepperPlanner{h.config.steppers[channel], board::stepper_pulses(channel)};
        }
        h.step_ready[channel] = false;
        return h.step_driver[channel].hard_reset();
      }
      if (operation == stepper_parameter && !h.step_ready[channel]) {
        return Error::not_armed;
      }
      auto error = operation == stepper_parameter ? Error::ok : h.prepare_stepper(channel);
      if (error != Error::ok) {
        return error;
      }
      if (argument > 255) {
        return Error::invalid_argument;
      }
      if (operation == stepper_set_parameter) {
        error =
            h.step_driver[channel].set_parameter(static_cast<std::uint8_t>(argument), secondary);
        {
          StepGuard lock;
          h.planners[channel] =
              StepperPlanner{h.config.steppers[channel], board::stepper_pulses(channel)};
        }
        h.step_ready[channel] = false;
        return error;
      }
      if (argument == 0) {
        std::uint16_t status{};
        error = h.step_driver[channel].status(status);
        if (error == Error::ok) {
          put16(0, status);
          size = 2;
        }
        return error;
      }
      std::uint32_t value{};
      error = h.step_driver[channel].get_parameter(static_cast<std::uint8_t>(argument), value);
      if (error == Error::ok) {
        wire::write_u32(output.data(), value);
        size = 4;
      }
      return error;
    }
    case pwm_info:
      if (channel > 1) {
        return Error::invalid_argument;
      }
      {
        const auto info = board::pwm_group_info(channel);
        wire::write_u32(output.data(), info[0]);
        wire::write_u32(output.data() + 4, info[1]);
        size = 8;
      }
      return Error::ok;
    case set_pwm_frequency:
      return board::pwm_group_frequency(channel, argument);
    case gpio: {
      std::uint32_t raw = 0;
      for (std::uint8_t i = 0; i < 13; ++i) {
        if (board::input(i)) {
          raw |= 1U << i;
        }
      }
      wire::write_u32(output.data(), raw);
      wire::write_u32(output.data() + 4, 0x1fff);
      size = 8;
    }
      return Error::ok;
    case battery_calibration:
      if (!h.config.battery_enabled || argument > 65535) {
        return Error::invalid_config;
      }
      return calibrate_battery(h.battery_protocol, h.config.battery_id, channel,
                               static_cast<std::uint16_t>(argument));
    case battery_id:
      if (!h.config.battery_enabled || argument == 0 || argument >= 254) {
        return Error::invalid_config;
      }
      return set_battery_id(h.battery_protocol, h.config.battery_id,
                            static_cast<std::uint8_t>(argument));
    case battery_value: {
      if (!h.config.battery_enabled && !h.diagnostic()) {
        return Error::invalid_config;
      }
      Error error = Error::invalid_argument;
      std::uint32_t wide{};
      std::uint16_t word{};
      std::uint8_t byte{};
      if (channel == 0) {
        error = read_battery_power(h.battery_protocol, h.config.battery_id, wide);
        wire::write_u32(output.data() + 1, wide);
        size = 5;
      } else if (channel == 1) {
        std::array<std::uint16_t, 4> cells{};
        error = read_battery_cells(h.battery_protocol, h.config.battery_id, cells);
        for (std::size_t i = 0; i < 4; ++i) {
          put16(1 + 2 * i, cells[i]);
        }
        size = 9;
      } else if (channel == 2 || channel == 3) {
        error = channel == 2 ? read_battery_voltage(h.battery_protocol, h.config.battery_id, word)
                             : read_battery_current(h.battery_protocol, h.config.battery_id, word);
        put16(1, word);
        size = 3;
      } else if (channel == 4 || channel == 5) {
        error = channel == 4
                    ? read_battery_soc(h.battery_protocol, h.config.battery_id, byte)
                    : read_battery_cell_count(h.battery_protocol, h.config.battery_id, byte);
        output[1] = byte;
        size = 2;
      }
      output[0] = error == Error::ok;
      if (error != Error::ok) {
        size = 0;
      }
      return error;
    }
    case device_ping:
      if (channel == 0 && argument < 3) {
        return h.motors[argument].ping();
      }
      if (channel == 1 && argument > 0 && argument < 254 &&
          (h.diagnostic() || h.config.ax_ids[argument] || h.config.mx_ids[argument])) {
        return h.servo_protocol.ping(static_cast<std::uint8_t>(argument));
      }
      if (channel == 2 && (h.config.battery_enabled || h.diagnostic())) {
        return h.battery_protocol.ping(h.config.battery_id);
      }
      return Error::invalid_config;
    case pca_control:
      if (!h.config.pca_enabled) {
        return Error::invalid_config;
      }
      if (argument == 0) {
        return h.pca.full_off();
      }
      if (argument == 1) {
        return h.pca.sleep();
      }
      if (argument == 2) {
        return h.pca.wakeup();
      }
      if (argument == 3) {
        const auto error = h.pca.initialize(values[0], values[1]);
        h.pca_ready = error == Error::ok;
        return error;
      }
      return Error::invalid_argument;
    case pca_allpulse:
      if (!h.config.pca_enabled || argument > 65535) {
        return Error::invalid_config;
      }
      for (std::size_t i = 0; i < 16; ++i) {
        if (!h.config.pca_channels[i] || argument < h.config.pca_calibration[i].minimum_us ||
            argument > h.config.pca_calibration[i].maximum_us) {
          return Error::invalid_config;
        }
      }
      {
        const auto error = h.pca.all_pulse_us(static_cast<std::uint16_t>(argument));
        if (error == Error::ok) {
          for (auto &deadline : h.pca_deadlines) {
            deadline = {true, now};
          }
        }
        return error;
      }
    case wheel_holding:
      if (argument > 1) {
        return Error::invalid_argument;
      }
      h.wheel_holding = argument == 1;
      return h.chassis->stop();
    case application_state: {
      if (!h.application) {
        return Error::invalid_config;
      }
      const auto &app = h.application->state();
      output[0] = static_cast<std::uint8_t>(app.mode);
      output[1] = static_cast<std::uint8_t>(app.phase);
      output[2] = app.strategy;
      output[3] = static_cast<std::uint8_t>(
          (app.communication_enabled ? 1 : 0) | (app.reset_latched ? 2 : 0) |
          (app.reset_requested ? 4 : 0) | (app.motor_enable_requested ? 8 : 0) |
          (app.cord_attached ? 16 : 0) | (app.side_right ? 32 : 0) |
          (app.start_action_skipped ? 64 : 0));
      put16(4, app.input_known);
      put16(6, app.input_pressed);
      put16(8, app.switch_known);
      put16(10, app.switches_pressed);
      wire::write_u32(output.data() + 12, app.match_elapsed_ms);
      wire::write_u32(output.data() + 16, app.dropped_events);
      output[20] = static_cast<std::uint8_t>(app.error);
      output[21] = static_cast<std::uint8_t>(app.stop_error);
      output[22] = static_cast<std::uint8_t>(app.stop_reason);
      output[23] = 0;
      size = 24;
      return Error::ok;
    }
    case application_event: {
      if (!h.application) {
        return Error::invalid_config;
      }
      std::fill_n(output.begin(), 9, 0);
      application::Event event{};
      if (h.application->pop_event(event)) {
        output[0] = 1;
        output[1] = static_cast<std::uint8_t>(event.button);
        output[2] = static_cast<std::uint8_t>(event.callback);
        output[3] = static_cast<std::uint8_t>(event.action);
        output[4] = static_cast<std::uint8_t>(event.state);
        wire::write_u32(output.data() + 5, event.at_ms);
      }
      size = 9;
      return Error::ok;
    }
    case match_arm:
      return h.application ? h.application->arm_match(now) : Error::invalid_config;
    case match_disarm:
      return h.application ? h.application->disarm_match(now) : Error::invalid_config;
    case application_mode:
      return h.application && argument <= 1
                 ? h.application->set_mode(static_cast<application::Mode>(argument))
                 : Error::invalid_argument;
    case communication:
      if (!h.application || argument > 1) {
        return Error::invalid_argument;
      }
      return h.application->set_communication(argument == 1, now);
    case diagnostic_text: {
      const auto append = [&](const char *text) {
        for (std::size_t i = 0; text[i] != 0 && size < 96; ++i) {
          output[size++] = static_cast<std::uint8_t>(text[i]);
        }
      };
      const auto number = [&](std::uint32_t value) {
        std::array<char, 10> digits{};
        std::size_t count = 0;
        do {
          digits[count++] = static_cast<char>('0' + value % 10);
          value /= 10;
        } while (value != 0);
        while (count != 0 && size < 96) {
          output[size++] = static_cast<std::uint8_t>(digits[--count]);
        }
      };
      append("fw=0.1.0 mode=");
      append(h.diagnostic() ? "DIAGNOSTIC" : "OPERATIONAL");
      append(" profile=");
      number(static_cast<std::uint8_t>(h.config.hardware_profile));
      append(" armed=");
      number(state.armed);
      append(" estop=");
      number(state.estop);
      append(" fault=");
      number(state.fault);
      append(" error=");
      number(static_cast<std::uint8_t>(state.last_error));
      append(" pumps=");
      number(h.pumps->state().commanded);
      append(" reset=");
      number(board::reset_cause());
      return Error::ok;
    }
    }
    return Error::unsupported;
  }
  static Error job_request(void *context, std::uint8_t type, std::uint32_t owner,
                           std::span<const std::uint8_t> input, std::span<std::uint8_t> output,
                           std::size_t &size, std::uint32_t now) {
    auto &h = *static_cast<Hardware *>(context);
    if (type == 0x24) {
      return maintenance_request(context, input, output, size, now);
    }
    size = 0;
    if (!h.gripper_engine || output.size() < 24) {
      return Error::unsupported;
    }
    Error error = Error::malformed;
    if (type == 0x21 && input.size() == 22) {
      if (h.gripper_engine->busy()) {
        return Error::lease_conflict;
      }
      const bool motion =
          input[1] == 6 || input[1] == 8 || input[1] == 9 || input[1] == 12 || input[1] == 22;
      const auto &wheels = h.chassis->state();
      if (motion && (!wheels.commanded_known || wheels.requested.vx != 0 ||
                     wheels.requested.vy != 0 || wheels.requested.wz != 0)) {
        return Error::invalid_argument;
      }
      for (auto value : wheels.wheel_commanded) {
        if (motion && value != 0) {
          return Error::invalid_argument;
        }
      }
      gripper::Request request{};
      request.gripper = input[0];
      request.operation = static_cast<gripper::Operation>(input[1]);
      request.value = wire::read_f32(input.data() + 2);
      request.mask = wire::read_u32(input.data() + 6);
      request.timeout_ms = wire::read_u32(input.data() + 10);
      request.argument = wire::read_f32(input.data() + 14);
      request.speed = wire::read_f32(input.data() + 18);
      error = h.begin_job(owner, request, now);
    } else if ((type == 0x22 || type == 0x23) && input.size() == 4) {
      if (h.job_id == 0 || wire::read_u32(input.data()) != h.job_id ||
          h.gripper_engine->state().owner != owner) {
        return Error::lease_conflict;
      }
      error = type == 0x23 ? h.gripper_engine->cancel(owner) : Error::ok;
      if (type == 0x23) {
        h.job_finished = now;
      }
    }
    const auto &state = h.gripper_engine->state();
    output[0] = static_cast<std::uint8_t>(state.phase);
    output[1] = static_cast<std::uint8_t>(state.operation);
    output[2] = state.gripper;
    output[3] = state.step;
    output[4] = state.count;
    output[5] = static_cast<std::uint8_t>(state.completion);
    output[6] = state.commanded_steps;
    output[7] = state.settled_steps;
    output[8] = state.referenced_steps;
    output[9] = state.measured_steps;
    output[10] = static_cast<std::uint8_t>(state.error);
    output[11] = static_cast<std::uint8_t>(state.stop_error);
    wire::write_u32(output.data() + 12, h.job_id);
    wire::write_u32(output.data() + 16, state.owner);
    wire::write_u32(output.data() + 20,
                    (h.gripper_engine->busy() ? now : h.job_finished) - h.job_started);
    size = 24;
    return error;
  }
};
struct Packet {
  wire::Frame frame{};
  std::uint8_t port{};
  bool reset_after_reply{};
};
StaticQueue_t commands_storage{}, replies_storage{};
alignas(Packet) std::array<std::uint8_t, 8 * sizeof(Packet)> commands_buffer{}, replies_buffer{};
QueueHandle_t commands{}, replies{};
std::atomic<std::uint32_t> control_progress{}, comms_progress{};
std::atomic<bool> comms_fault{};
struct ControlArgs {
  Hardware &hardware;
  Controller &controller;
  wire::Dispatcher &dispatcher;
};
void control_task(void *pointer) {
  auto &args = *static_cast<ControlArgs *>(pointer);
  auto &hardware = args.hardware;
  auto &controller = args.controller;
  const auto hw = hardware.initialize(), initialized = controller.initialize(board::millis());
  if (hw != Error::ok || initialized != Error::ok) {
    (void)controller.fail(hw != Error::ok ? hw : initialized);
  }
  if (hardware.application && hardware.application->initialize(board::millis()) != Error::ok) {
    (void)controller.fail(Error::invalid_config);
  }
  board::debug_enable(hardware.config.debug_uart_enabled);
  board::stepper_service(&hardware, Hardware::step_tick);
  auto last = xTaskGetTickCount();
  std::uint32_t feedback_at = 0;
  for (;;) {
    const auto now = board::millis();
    if (hardware.config.estop_input != 255) {
      controller.physical_estop(board::input(hardware.config.estop_input) ==
                                hardware.config.estop_active_high);
    }
    if (board::receive_overflow() || comms_fault.exchange(false) || board::stepper_failed()) {
      (void)controller.fail(Error::output_failure);
    }
    // Expire the previous commands before a job may renew them after a delayed
    // control tick.
    (void)controller.tick(now);
    if (hardware.aux_expired(now)) {
      (void)controller.fail(Error::timeout);
    }
    if (hardware.application) {
      std::uint16_t raw = 0;
      for (std::uint8_t i = 0; i < 13; ++i) {
        if (board::input(i)) {
          raw |= static_cast<std::uint16_t>(1U << i);
        }
      }
      (void)hardware.application->sample_inputs(raw, now);
      (void)hardware.application->tick(now);
    }
    hardware.job_tick(now);
    Packet request{};
    for (unsigned i = 0; i < 8 && xQueueReceive(commands, &request, 0) == pdPASS; ++i) {
      Packet reply{args.dispatcher.dispatch(request.frame, board::millis()), request.port, false};
      reply.reset_after_reply =
          hardware.reset_pending && request.frame.type == 0x24 && request.frame.length == 22 &&
          request.frame.payload[0] ==
              static_cast<std::uint8_t>(maintenance::Operation::board_reset) &&
          reply.frame.length == 1 && reply.frame.payload[0] == static_cast<std::uint8_t>(Error::ok);
      if (xQueueSend(replies, &reply, 0) != pdPASS) {
        (void)controller.fail(Error::output_failure);
      }
    }
    (void)controller.tick(board::millis());
    if (controller.state().armed && elapsed(now, feedback_at, 50)) {
      feedback_at = now;
      if (hardware.poll_feedback(controller.chassis()) != Error::ok) {
        (void)controller.fail(Error::timeout);
      }
    }
    control_progress.store(xTaskGetTickCount(), std::memory_order_release);
    vTaskDelayUntil(&last, pdMS_TO_TICKS(10));
  }
}
void comms_task(void *) {
  std::array<wire::FrameParser, 2> parsers{wire::FrameParser{}, wire::FrameParser{}};
  std::array<std::uint8_t, wire::max_payload + wire::frame_overhead> buffer{};
  auto last = xTaskGetTickCount();
  for (;;) {
    for (std::uint8_t port = 0; port < 2; ++port) {
      for (unsigned i = 0; i < 512; ++i) {
        const auto byte = port == 0 ? board::host_read() : board::debug_read();
        if (!byte) {
          break;
        }
        auto frame = parsers[port].push(*byte, board::millis());
        if (frame) {
          Packet packet{*frame, port, false};
          if (xQueueSend(commands, &packet, 0) != pdPASS) {
            comms_fault.store(true);
          }
        }
      }
    }
    Packet reply{};
    for (unsigned i = 0; i < 8 && xQueueReceive(replies, &reply, 0) == pdPASS; ++i) {
      const auto size = wire::encode_frame(reply.frame, buffer);
      const auto sent =
          size == 0 ? Error::malformed
                    : (reply.port == 0 ? board::host_write(std::span{buffer}.first(size), 20000)
                                       : board::debug_write(std::span{buffer}.first(size), 20000));
      if (sent != Error::ok) {
        comms_fault.store(true);
      } else if (reply.reset_after_reply) {
        board::system_reset();
      }
    }
    comms_progress.store(xTaskGetTickCount(), std::memory_order_release);
    vTaskDelayUntil(&last, pdMS_TO_TICKS(1));
  }
}
void watchdog_task(void *) {
  board::watchdog_start();
  for (;;) {
    const auto now = xTaskGetTickCount();
    if (now - control_progress.load(std::memory_order_acquire) < pdMS_TO_TICKS(100) &&
        now - comms_progress.load(std::memory_order_acquire) < pdMS_TO_TICKS(100)) {
      board::watchdog_feed();
    }
    vTaskDelay(pdMS_TO_TICKS(20));
  }
}
} // namespace
[[noreturn]] void eurobot_run() {
#ifdef EUROBOT_DEPLOYMENT_CONFIG
  static const auto config = make_deployment();
#else
  static const Deployment config{};
#endif
  // Long-lived configuration, device objects and task stacks are in BSS, never
  // the startup MSP.
  static Hardware hardware{config};
  static PumpBank pumps{{config.channels, config.max_active, 250, config.max_pump_on_ms},
                        {&hardware, Hardware::write_outputs}};
  static Chassis chassis{config.chassis, {&hardware, Hardware::write_wheels}};
  hardware.chassis = &chassis;
  static Controller controller{pumps,
                               chassis,
                               {&hardware, Hardware::stop_hook},
                               config.mode != board::DeploymentMode::OPERATIONAL_MODE};
  hardware.controller = &controller;
  hardware.pumps = &pumps;
  static gripper::Engine grippers{config.grippers,
                                  {&hardware, Hardware::job_guard, Hardware::job_validate,
                                   Hardware::job_issue, Hardware::job_feedback,
                                   Hardware::job_stop}};
  hardware.gripper_engine = &grippers;
  static application::Application app{config.application,
                                      {&hardware, Hardware::application_guard,
                                       Hardware::application_stop, Hardware::application_action}};
  hardware.application = &app;
  static wire::Dispatcher dispatcher{controller,
                                     {&hardware, Hardware::actuator, Hardware::sensor_read,
                                      Hardware::job_request, Hardware::command_guard}};
  static ControlArgs args{hardware, controller, dispatcher};
  commands = xQueueCreateStatic(8, sizeof(Packet), commands_buffer.data(), &commands_storage);
  replies = xQueueCreateStatic(8, sizeof(Packet), replies_buffer.data(), &replies_storage);
  if (!commands || !replies) {
    board::fail_safe();
  }
  static StaticTask_t control_tcb, comms_tcb, watchdog_tcb;
  static StackType_t control_stack[2048], comms_stack[1024], watchdog_stack[256];
  if (!xTaskCreateStatic(control_task, "control", 2048, &args, 4, control_stack, &control_tcb) ||
      !xTaskCreateStatic(comms_task, "comms", 1024, nullptr, 3, comms_stack, &comms_tcb) ||
      !xTaskCreateStatic(watchdog_task, "watchdog", 256, nullptr, 2, watchdog_stack,
                         &watchdog_tcb)) {
    board::fail_safe();
  }
  vTaskStartScheduler();
  board::fail_safe();
}
