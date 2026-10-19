#include "eurobot/gripper.hpp"
#include <algorithm>
#include <cmath>
#include <limits>
namespace eurobot::gripper {
namespace {
bool finite(float value) noexcept { return std::isfinite(value); }
bool positive(float value) noexcept { return finite(value) && value > 0; }
bool interval(std::uint32_t value) noexcept { return value > 0 && value < 0x80000000U; }
bool servo_binding(ServoBinding binding, float degrees) noexcept {
  return (binding.device == Device::ax || binding.device == Device::mx) && binding.id > 0 &&
         binding.id < 254 && finite(binding.minimum_deg) && finite(binding.maximum_deg) &&
         binding.minimum_deg >= 0 && binding.maximum_deg > binding.minimum_deg &&
         binding.maximum_deg <= (binding.device == Device::ax ? 300 : 360) && finite(degrees) &&
         degrees >= binding.minimum_deg && degrees <= binding.maximum_deg;
}
ServoBinding ax(std::uint8_t id) noexcept { return {Device::ax, id, 0, 300}; }
Error step_shape(const Step &step) noexcept {
  if (!interval(step.timeout_ms) || step.settle_ms >= step.timeout_ms || step.count > 4 ||
      !finite(step.tolerance) || !finite(step.alternate_current_ma)) {
    return Error::invalid_config;
  }
  if (step.enable_before_settle && step.action != Action::stepper_current) {
    return Error::invalid_config;
  }
  switch (step.action) {
  case Action::servo_group:
    if (step.count == 0 || step.required != Evidence::measured || !positive(step.tolerance)) {
      return Error::invalid_config;
    }
    for (std::size_t i = 0; i < step.count; ++i) {
      const auto &target = step.targets[i];
      if ((target.device != Device::ax && target.device != Device::mx) || target.id == 0 ||
          target.id >= 254 || !finite(target.value) || target.value < 0 ||
          target.value > (target.device == Device::ax ? 300 : 360) || !positive(target.rate) ||
          target.rate > 1023 || std::trunc(target.rate) != target.rate) {
        return Error::invalid_config;
      }
      for (std::size_t j = 0; j < i; ++j) {
        if (step.targets[j].id == target.id) {
          return Error::invalid_config;
        }
      }
    }
    break;
  case Action::pca_group:
    if (step.count == 0 || step.required != Evidence::settled || step.settle_ms == 0) {
      return Error::invalid_config;
    }
    for (std::size_t i = 0; i < step.count; ++i) {
      const auto &target = step.targets[i];
      if (target.device != Device::pca || target.id >= 16 || !finite(target.value) ||
          target.rate != 0) {
        return Error::invalid_config;
      }
      for (std::size_t j = 0; j < i; ++j) {
        if (step.targets[j].id == target.id) {
          return Error::invalid_config;
        }
      }
    }
    break;
  case Action::outputs:
    if (step.count != 0 || step.required != Evidence::commanded ||
        (step.pump_values & ~step.pump_select) != 0 ||
        (step.valve_values & ~step.valve_select) != 0) {
      return Error::invalid_config;
    }
    break;
  case Action::stepper_current:
  case Action::stepper_move:
  case Action::stepper_disable:
    if (step.count != 1 || step.targets[0].device != Device::stepper || step.targets[0].id >= 4) {
      return Error::invalid_config;
    }
    if (step.action == Action::stepper_current &&
        (!finite(step.targets[0].value) || step.targets[0].value < 31.25F ||
         step.targets[0].value > 4000 ||
         (step.alternate_current_ma != 0 &&
          (step.alternate_current_ma < 31.25F || step.alternate_current_ma > 4000)))) {
      return Error::invalid_config;
    }
    if (step.action == Action::stepper_move &&
        (step.required != Evidence::referenced_steps || !positive(step.targets[0].rate) ||
         std::trunc(step.targets[0].rate) != step.targets[0].rate)) {
      return Error::invalid_config;
    }
    if (step.action != Action::stepper_move && step.required != Evidence::commanded) {
      return Error::invalid_config;
    }
    break;
  case Action::wait_input:
    if (step.input_select == 0 || (step.input_values & ~step.input_select) != 0 ||
        step.required != Evidence::measured) {
      return Error::invalid_config;
    }
    break;
  case Action::delay:
    if (step.count != 0 || step.settle_ms == 0 || step.required != Evidence::settled) {
      return Error::invalid_config;
    }
    break;
  case Action::timed_drive:
    if (step.required != Evidence::settled || step.drive.axis > 2 || step.drive.duration_ms == 0 ||
        step.drive.duration_ms >= step.timeout_ms || !finite(step.drive.velocity.vx) ||
        !finite(step.drive.velocity.vy) || !finite(step.drive.velocity.wz)) {
      return Error::invalid_config;
    }
    break;
  case Action::relative_drive:
    if (step.required != Evidence::measured || step.drive.axis > 1 ||
        !finite(step.drive.displacement_m) || !positive(step.drive.maximum_speed_mps) ||
        !positive(step.drive.tolerance_m) || !positive(step.drive.minimum_speed_mps) ||
        step.drive.minimum_speed_mps > step.drive.maximum_speed_mps ||
        !positive(step.drive.acceleration_mps2) || !positive(step.drive.heading_kp) ||
        !positive(step.drive.lateral_kp) || !positive(step.drive.maximum_angular_radps)) {
      return Error::invalid_config;
    }
    break;
  case Action::wheel_calibration:
    if (step.required != Evidence::measured || step.drive.axis > 2 ||
        step.drive.wheel_goal_ticks <= 0 || step.drive.wheel_tpm <= 0 ||
        step.drive.wheel_stop_margin < 0 ||
        step.drive.wheel_stop_margin >= step.drive.wheel_goal_ticks) {
      return Error::invalid_config;
    }
    break;
  case Action::chassis_stop:
    if (step.count != 0 || step.required != Evidence::commanded) {
      return Error::invalid_config;
    }
    break;
  default:
    return Error::unsupported;
  }
  return Error::ok;
}
bool feedback_action(Action action) noexcept {
  return action == Action::servo_group || action == Action::stepper_move ||
         action == Action::wait_input || action == Action::timed_drive ||
         action == Action::relative_drive || action == Action::wheel_calibration;
}
} // namespace
Config legacy_2026_reference(std::uint8_t index) noexcept {
  Config config{};
  if (index > 1) {
    return config;
  }
  // Final source SyncWrite lists {254,112}; only112 is an individual target.
  // Same Git change moved98 from main_1 to cursor. Explicitly disable the old
  // non-unicast slot, without inventing a second motor ID or broadcasting writes.
  config.main = index == 0 ? std::array<ServoBinding, 2>{ax(unassigned), ax(112)}
                           : std::array<ServoBinding, 2>{ax(1), ax(2)};
  config.main_active_mask = index == 0 ? 0x02 : 0x03;
  config.side = index == 0 ? std::array<ServoBinding, 2>{ax(7), ax(8)}
                           : std::array<ServoBinding, 2>{ax(16), ax(25)};
  config.cursor = ax(98);
  config.pinion = ax(13);
  config.main_up_deg = 65;
  config.main_down_deg = 149;
  config.main_cursor_deg = 99;
  config.side_closed_deg = {54, 155};
  config.side_open_deg = {index == 0 ? 124.0F : 123.0F, 86};
  config.shift_closed_deg =
      index == 0 ? std::array<float, 2>{102, 15} : std::array<float, 2>{103, 18};
  config.shift_open_deg =
      index == 0 ? std::array<float, 2>{17, 112} : std::array<float, 2>{16, 112};
  config.block_channels = index == 0 ? std::array<std::uint8_t, 4>{1, 2, 3, 4}
                                     : std::array<std::uint8_t, 4>{7, 8, 9, 10};
  config.shift_channels =
      index == 0 ? std::array<std::uint8_t, 2>{5, 6} : std::array<std::uint8_t, 2>{11, 12};
  config.cursor_horizontal_deg = 198;
  config.cursor_vertical_deg = 80;
  config.pinion_start_deg = 148;
  config.pinion_deg_per_mm = 3.979F;
  config.main_speed_raw = 150;
  config.side_speed_raw = 900;
  config.cursor_speed_raw = 600;
  config.pinion_speed_raw = 800;
  config.angle_tolerance_deg = 10;
  config.block_zero_deg = 0;
  config.block_rotated_deg = 180;
  config.lift_stepper = 0;
  config.lift_steps_per_mm = 25;
  config.lift_up_current_ma = 1100;
  config.lift_down_current_ma = 320;
  // Lift bounds/speed, pump mappings and obsolete HL timed-drive parameters
  // need actual deployment review.
  config.operation_timeout_ms = 60000;
  config.drive_reference.tolerance_m = 0.003F;
  config.drive_reference.minimum_speed_mps = 0.002F;
  config.drive_reference.acceleration_mps2 = 0.2F;
  config.drive_reference.heading_kp = 2.2F;
  config.drive_reference.lateral_kp = 2;
  config.drive_reference.maximum_angular_radps = 0.4F;
  config.drive_reference.wheel_goal_ticks = 28 * 4096;
  config.drive_reference.wheel_tpm = 400000;
  config.drive_reference.wheel_stop_margin = 50;
  return config;
}
Config legacy_2025_reference() noexcept {
  Config config{};
  config.main = {ax(103), ax(102)};
  config.main_up_deg = 0;
  config.angle_tolerance_deg = 10;
  // The 2025 task did not specify MovingSpeed; no replacement speed is invented
  // here.
  return config;
}
Engine::Engine(std::span<const Config> configs, Sinks sinks) noexcept : sinks_(sinks) {
  if (configs.size() <= configs_.size()) {
    config_count_ = configs.size();
    std::copy(configs.begin(), configs.end(), configs_.begin());
  }
}
Error Engine::append(Step step) noexcept {
  const auto shape = step_shape(step);
  if (shape != Error::ok) {
    return shape;
  }
  if (state_.count >= maximum_steps) {
    return Error::invalid_config;
  }
  steps_[state_.count++] = step;
  return Error::ok;
}
Error Engine::make_plan(Request request) noexcept {
  const auto &config = configs_[request.gripper];
  Error result = Error::ok;
  const auto add = [this, &result](Step step) {
    if (result == Error::ok) {
      result = append(step);
    }
  };
  const auto pause = [&add](std::uint32_t duration) {
    Step step{};
    step.action = Action::delay;
    step.required = Evidence::settled;
    step.settle_ms = duration;
    step.timeout_ms = duration + 1000;
    add(step);
  };
  const auto servos = [&add, &config, &result](std::span<const ServoBinding> bindings,
                                               std::span<const float> angles, float speed,
                                               std::uint32_t settling) {
    if (bindings.size() != angles.size() || bindings.empty() || bindings.size() > 4) {
      result = Error::invalid_config;
      return;
    }
    Step step{};
    step.action = Action::servo_group;
    step.count = static_cast<std::uint8_t>(bindings.size());
    step.required = Evidence::measured;
    step.settle_ms = settling;
    step.timeout_ms = config.servo_timeout_ms;
    step.tolerance = config.angle_tolerance_deg;
    for (std::size_t i = 0; i < bindings.size(); ++i) {
      if (!servo_binding(bindings[i], angles[i])) {
        result = Error::invalid_config;
      }
      step.targets[i] = {bindings[i].device, bindings[i].id, angles[i], speed, 0};
    }
    add(step);
  };
  const auto main_servos = [&servos, &config, &result](float angle, std::uint32_t settling) {
    if (config.main_active_mask == 0 || (config.main_active_mask & ~0x03U) != 0) {
      result = Error::invalid_config;
      return;
    }
    std::array<ServoBinding, 2> selected{};
    const std::array<float, 2> angles{angle, angle};
    std::size_t count = 0;
    for (std::size_t i = 0; i < config.main.size(); ++i) {
      if ((config.main_active_mask & (1U << i)) != 0)
        selected[count++] = config.main[i];
    }
    servos(std::span{selected}.first(count), std::span{angles}.first(count), config.main_speed_raw,
           settling);
  };
  const auto main = [&main_servos, &config](float state, std::uint32_t settling) {
    const auto angle = state == 0 ? config.main_up_deg
                                  : (state == 1 ? config.main_down_deg : config.main_cursor_deg);
    main_servos(angle, settling);
  };
  const auto side = [&servos, &config](bool closed, std::uint32_t settling) {
    servos(config.side, closed ? config.side_closed_deg : config.side_open_deg,
           config.side_speed_raw, settling);
  };
  const auto pca = [&add, &result](std::span<const std::uint8_t> ids, std::span<const float> angles,
                                   std::uint32_t settling) {
    if (ids.size() != angles.size() || ids.empty() || ids.size() > 4) {
      result = Error::invalid_config;
      return;
    }
    Step step{};
    step.action = Action::pca_group;
    step.required = Evidence::settled;
    step.count = static_cast<std::uint8_t>(ids.size());
    step.settle_ms = settling;
    step.timeout_ms = settling + 1500;
    for (std::size_t i = 0; i < ids.size(); ++i) {
      step.targets[i] = {Device::pca, ids[i], angles[i], 0, 0};
    }
    add(step);
  };
  const auto shift = [&pca, &config](bool spread) {
    pca(config.shift_channels, spread ? config.shift_open_deg : config.shift_closed_deg, 200);
  };
  const auto rotate = [&pca, &pause, &config](std::uint32_t mask) {
    for (std::size_t i = 0; i < 4; ++i) {
      const std::array<std::uint8_t, 1> channel{config.block_channels[i]};
      const std::array<float, 1> angle{(mask & (1U << (3 - i))) != 0 ? config.block_rotated_deg
                                                                     : config.block_zero_deg};
      pca(channel, angle, 20);
    }
    pause(500);
  };
  const auto outputs = [&add, &result, &config](bool pump, std::uint32_t values,
                                                std::uint32_t settling) {
    if ((values & ~0xfU) != 0) {
      result = Error::invalid_argument;
      return;
    }
    const auto &channels = pump ? config.pump_channels : config.valve_channels;
    std::uint32_t select = 0, on = 0;
    for (std::size_t i = 0; i < channels.size(); ++i) {
      if (channels[i] >= 32) {
        result = Error::invalid_config;
        return;
      }
      const auto bit = 1U << channels[i];
      if ((select & bit) != 0) {
        result = Error::invalid_config;
        return;
      }
      select |= bit;
      if ((values & (1U << i)) != 0) {
        on |= bit;
      }
    }
    Step step{};
    step.action = Action::outputs;
    step.settle_ms = settling;
    if (pump) {
      step.pump_select = select;
      step.pump_values = on;
    } else {
      step.valve_select = select;
      step.valve_values = on;
    }
    add(step);
  };
  const auto timed = [&add, &result, &config](float speed, std::uint32_t duration,
                                              std::uint8_t axis, std::uint32_t settling) {
    if (!finite(speed) || duration == 0 || duration >= 60000 || axis > 2) {
      result = Error::invalid_argument;
      return;
    }
    Step step{};
    step.action = Action::timed_drive;
    step.required = Evidence::settled;
    step.drive = config.drive_reference;
    step.drive.axis = axis;
    step.drive.duration_ms = duration;
    step.drive.velocity =
        axis == 0 ? Twist{speed, 0, 0} : (axis == 1 ? Twist{0, speed, 0} : Twist{0, 0, speed});
    step.timeout_ms = std::max(duration, settling) + 1500;
    step.settle_ms = settling;
    add(step);
  };
  const auto stop_base = [&add]() {
    Step step{};
    step.action = Action::chassis_stop;
    add(step);
  };
  switch (request.operation) {
  case Operation::cursor: {
    if (request.value != 0 && request.value != 1) {
      return Error::invalid_argument;
    }
    const std::array<ServoBinding, 1> binding{config.cursor};
    const std::array<float, 1> angle{request.value == 1 ? config.cursor_horizontal_deg
                                                        : config.cursor_vertical_deg};
    servos(binding, angle, config.cursor_speed_raw, 50);
    break;
  }
  case Operation::side_grips:
    if (request.value != 0 && request.value != 1) {
      return Error::invalid_argument;
    }
    side(request.value == 1, 100);
    break;
  case Operation::rotate_main:
    if (request.value != 0 && request.value != 1 && request.value != 2) {
      return Error::invalid_argument;
    }
    main(request.value, 200);
    break;
  case Operation::pumps:
    outputs(true, request.mask, 50);
    break;
  case Operation::valves:
    outputs(false, request.mask, 50);
    break;
  case Operation::blocks_shift:
    if (request.value != 0 && request.value != 1) {
      return Error::invalid_argument;
    }
    shift(request.value == 1);
    break;
  case Operation::blocks_rotate:
  case Operation::blocks_rotate_only:
    if ((request.mask & ~0xfU) != 0) {
      return Error::invalid_argument;
    }
    if (request.operation == Operation::blocks_rotate) {
      shift(true);
    }
    rotate(request.mask);
    if (request.operation == Operation::blocks_rotate) {
      shift(false);
    }
    break;
  case Operation::lift_height: {
    if (config.lift_stepper >= 4 || !positive(config.lift_steps_per_mm) ||
        !positive(config.lift_speed_steps_s) || !finite(config.minimum_height_mm) ||
        !finite(config.maximum_height_mm) || config.minimum_height_mm >= config.maximum_height_mm ||
        request.value < config.minimum_height_mm || request.value > config.maximum_height_mm ||
        config.minimum_lift_steps >= config.maximum_lift_steps) {
      return Error::invalid_config;
    }
    const auto position = static_cast<double>(request.value) * config.lift_steps_per_mm;
    if (!std::isfinite(position) || position < std::numeric_limits<std::int32_t>::min() ||
        position > std::numeric_limits<std::int32_t>::max()) {
      return Error::invalid_argument;
    }
    const auto steps = static_cast<std::int64_t>(std::llround(position));
    if (steps < config.minimum_lift_steps || steps > config.maximum_lift_steps) {
      return Error::invalid_argument;
    }
    Step current{};
    current.action = Action::stepper_current;
    current.count = 1;
    current.targets[0] = {Device::stepper, config.lift_stepper, config.lift_up_current_ma, 0,
                          steps};
    current.alternate_current_ma = config.lift_down_current_ma;
    current.enable_before_settle = true;
    add(current);
    pause(10);
    Step movement{};
    movement.action = Action::stepper_move;
    movement.count = 1;
    movement.required = Evidence::referenced_steps;
    movement.timeout_ms = config.lift_timeout_ms;
    movement.targets[0] = {Device::stepper, config.lift_stepper, 0, config.lift_speed_steps_s,
                           steps};
    movement.tolerance = 0;
    add(movement);
    if (config.disable_lift_after_move) {
      Step disable{};
      disable.action = Action::stepper_disable;
      disable.count = 1;
      disable.targets[0] = {Device::stepper, config.lift_stepper, 0, 0, steps};
      add(disable);
    }
    current.alternate_current_ma = 0;
    current.enable_before_settle = false;
    add(current);
    pause(10);
    break;
  }
  case Operation::slide: {
    if (!positive(config.pinion_deg_per_mm)) {
      return Error::invalid_config;
    }
    const auto degrees = config.pinion_start_deg + request.value * config.pinion_deg_per_mm;
    const std::array<ServoBinding, 1> binding{config.pinion};
    const std::array<float, 1> angle{degrees};
    servos(binding, angle, config.pinion_speed_raw, 50);
    break;
  }
  case Operation::timed_move:
    if (request.mask > 2 || !finite(request.argument) || request.argument < 1 ||
        request.argument >= 60000 || std::trunc(request.argument) != request.argument) {
      return Error::invalid_argument;
    }
    timed(request.value, static_cast<std::uint32_t>(request.argument),
          static_cast<std::uint8_t>(request.mask), 0);
    stop_base();
    pause(50);
    break;
  case Operation::relative_move:
  case Operation::relative_move_poll: {
    if (!positive(request.speed)) {
      return Error::invalid_argument;
    }
    for (const auto axis : {1U, 0U}) {
      Step step{};
      step.action = Action::relative_drive;
      step.required = Evidence::measured;
      step.drive = config.drive_reference;
      step.drive.axis = static_cast<std::uint8_t>(axis);
      step.drive.displacement_m = axis == 1 ? request.argument : request.value;
      step.drive.maximum_speed_mps = request.speed;
      const auto estimate =
          std::fabs(static_cast<double>(step.drive.displacement_m)) / request.speed * 3500 + 3000;
      if (!std::isfinite(estimate) || estimate > 60000) {
        return Error::invalid_argument;
      }
      step.timeout_ms = static_cast<std::uint32_t>(std::ceil(estimate));
      add(step);
      stop_base();
      pause(80);
    }
    pause(100);
    break;
  }
  case Operation::wheel_calibration: {
    if (request.mask > 2) {
      return Error::invalid_argument;
    }
    stop_base();
    pause(20);
    Step step{};
    step.action = Action::wheel_calibration;
    step.required = Evidence::measured;
    step.drive = config.drive_reference;
    step.drive.axis = static_cast<std::uint8_t>(request.mask);
    step.timeout_ms = 60000;
    add(step);
    stop_base();
    pause(20);
    break;
  }
  case Operation::legacy_2025_reset:
  case Operation::legacy_2025_cycle: {
    if (request.operation == Operation::legacy_2025_cycle) {
      for (const auto angle : {100.0F, 50.0F}) {
        main_servos(angle, 0);
        pause(1000);
      }
    }
    main_servos(0, 0);
    break;
  }
  case Operation::capture:
  case Operation::release: {
    if (config.composite != Composite::base_strategy_2026) {
      return Error::unsupported;
    }
    if (request.operation == Operation::capture) {
      if (!positive(config.capture_forward_mps) || !positive(config.capture_backward_mps) ||
          config.capture_axis > 1 || config.capture_forward_ms == 0 ||
          config.capture_backward_ms == 0) {
        return Error::invalid_config;
      }
      side(false, 300);
      main(1, 300);
      side(true, 300);
      side(false, 300);
      outputs(false, 0xf, 300);
      outputs(true, 0xf, 300);
      timed(config.capture_forward_mps, config.capture_forward_ms, config.capture_axis, 2500);
      side(true, 300);
      main(0, 300);
      timed(-config.capture_backward_mps, config.capture_backward_ms, config.capture_axis, 2500);
      // SendImage is a separate high-level ROS/camera operation, never a
      // fictitious MCU action.
      side(false, 300);
    } else {
      main(1, 300);
      outputs(true, 0, 300);
      outputs(false, 0, 300);
      outputs(false, 0xf, 300);
      main(0, 300);
      shift(true);
      rotate(0);
      shift(false);
      pause(4500);
    }
    break;
  }
  case Operation::prepare: {
    const std::array<ServoBinding, 1> cursor_binding{config.cursor};
    const std::array<float, 1> cursor_angle{config.cursor_vertical_deg};
    servos(cursor_binding, cursor_angle, config.cursor_speed_raw, 100);
    side(true, 500);
    rotate(0);
    shift(false);
    outputs(true, 0, 50);
    outputs(false, 0, 50);
    main(0, 200);
    const std::array<ServoBinding, 1> pinion_binding{config.pinion};
    const std::array<float, 1> pinion_angle{config.pinion_start_deg};
    servos(pinion_binding, pinion_angle, config.pinion_speed_raw, 100);
    break;
  }
  default:
    return Error::unsupported;
  }
  return result;
}
void Engine::restore(State previous) noexcept {
  // Rebuild the immutable accepted recipe rather than reserving another 64-step
  // buffer in SRAM.
  if (has_plan_) {
    state_ = {};
    (void)make_plan(accepted_request_);
  }
  state_ = previous;
}
Error Engine::start(std::uint32_t owner, Request request, std::uint32_t now) noexcept {
  if (busy()) {
    return Error::lease_conflict;
  }
  if (config_count_ == 0 || request.gripper >= config_count_ || !sinks_.guard || !sinks_.validate ||
      !sinks_.issue || !sinks_.feedback || !sinks_.stop) {
    return Error::invalid_config;
  }
  if (owner == 0 || !finite(request.value) || !finite(request.argument) || !finite(request.speed) ||
      request.timeout_ms < 100 || request.timeout_ms > 60000) {
    return Error::invalid_argument;
  }
  const auto &config = configs_[request.gripper];
  if (!config.reviewed || !interval(config.operation_timeout_ms) ||
      request.timeout_ms > config.operation_timeout_ms) {
    return Error::invalid_config;
  }
  const auto guard = sinks_.guard(sinks_.context, owner, now);
  if (guard != Error::ok) {
    return guard;
  }
  const auto previous = state_;
  state_ = {};
  state_.owner = owner;
  state_.operation = request.operation;
  state_.gripper = request.gripper;
  const auto planned = make_plan(request);
  if (planned != Error::ok || state_.count == 0) {
    const auto rejected = planned == Error::ok ? Error::invalid_config : planned;
    restore(previous);
    return rejected;
  }
  for (std::size_t i = 0; i < state_.count; ++i) {
    const auto validated = sinks_.validate(sinks_.context, steps_[i]);
    if (validated != Error::ok) {
      restore(previous);
      return validated;
    }
  }
  accepted_request_ = request;
  has_plan_ = true;
  state_.phase = Phase::running;
  operation_timeout_ = request.timeout_ms;
  operation_started_ = now;
  step_started_ = now;
  issued_ = false;
  return Error::ok;
}
void Engine::advance(Evidence evidence, std::uint32_t now) noexcept {
  switch (evidence) {
  case Evidence::commanded:
    ++state_.commanded_steps;
    break;
  case Evidence::settled:
    ++state_.settled_steps;
    break;
  case Evidence::referenced_steps:
    ++state_.referenced_steps;
    break;
  case Evidence::measured:
    ++state_.measured_steps;
    break;
  case Evidence::unknown:
    break;
  }
  ++state_.step;
  issued_ = false;
  step_started_ = now;
  if (state_.step == state_.count) {
    state_.phase = Phase::completed;
    state_.completion = state_.commanded_steps != 0
                            ? Evidence::commanded
                            : (state_.settled_steps != 0
                                   ? Evidence::settled
                                   : (state_.referenced_steps != 0 ? Evidence::referenced_steps
                                                                   : Evidence::measured));
  }
}
Error Engine::terminate(Phase phase, Error reason) noexcept {
  // Publish terminal state before callback: a runtime stop may itself abort the
  // engine.
  state_.phase = phase;
  state_.error = reason;
  state_.completion = Evidence::unknown;
  issued_ = false;
  state_.stop_error = sinks_.stop ? sinks_.stop(sinks_.context, reason) : Error::invalid_config;
  if (state_.stop_error != Error::ok) {
    state_.phase = Phase::faulted;
    return state_.stop_error;
  }
  return reason;
}
Error Engine::tick(std::uint32_t now) noexcept {
  if (!busy()) {
    return Error::ok;
  }
  const auto guard = sinks_.guard(sinks_.context, state_.owner, now);
  if (guard != Error::ok) {
    return terminate(Phase::faulted, guard);
  }
  if (elapsed(now, operation_started_, operation_timeout_)) {
    return terminate(Phase::faulted, Error::timeout);
  }
  const auto &step = steps_[state_.step];
  if (!issued_) {
    step_started_ = now;
    if (step.action != Action::delay) {
      const auto issued = sinks_.issue(sinks_.context, step, now);
      if (issued != Error::ok) {
        return terminate(Phase::faulted, issued);
      }
    }
    issued_ = true;
  }
  if (elapsed(now, step_started_, step.timeout_ms)) {
    return terminate(Phase::faulted, Error::timeout);
  }
  if (feedback_action(step.action)) {
    Completion completion{};
    const auto polled = sinks_.feedback(sinks_.context, step, completion, now);
    if (polled != Error::ok) {
      return terminate(Phase::faulted, polled);
    }
    if (completion.known && completion.done && completion.evidence == step.required &&
        elapsed(now, step_started_, step.settle_ms)) {
      advance(completion.evidence, now);
    }
  } else if (elapsed(now, step_started_, step.settle_ms)) {
    advance(step.required, now);
  }
  return Error::ok;
}
Error Engine::cancel(std::uint32_t owner) noexcept {
  if (!busy()) {
    return Error::invalid_argument;
  }
  if (owner == 0 || owner != state_.owner) {
    return Error::lease_conflict;
  }
  return terminate(Phase::cancelled, Error::ok);
}
Error Engine::abort(Error reason) noexcept {
  if (!busy()) {
    return Error::ok;
  }
  return terminate(Phase::faulted, reason == Error::ok ? Error::output_failure : reason);
}
} // namespace eurobot::gripper
