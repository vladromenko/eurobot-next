#include "diagnostic_msgs/msg/diagnostic_array.hpp"
#include "diagnostic_msgs/msg/key_value.hpp"
#include "eurobot_hardware/transport.hpp"
#include "eurobot_interfaces/action/gripper.hpp"
#include "eurobot_interfaces/msg/board_inputs.hpp"
#include "eurobot_interfaces/msg/controller_state.hpp"
#include "eurobot_interfaces/srv/arm.hpp"
#include "eurobot_interfaces/srv/maintenance.hpp"
#include "eurobot_interfaces/srv/set_actuator.hpp"
#include "eurobot_interfaces/srv/set_outputs.hpp"
#include "geometry_msgs/msg/twist_stamped.hpp"
#include "rcl_interfaces/msg/parameter_descriptor.hpp"
#include "rclcpp/message_info.hpp"
#include "rclcpp/rclcpp.hpp"
#include "rclcpp_action/rclcpp_action.hpp"
#include "sensor_msgs/msg/battery_state.hpp"
#include "std_srvs/srv/trigger.hpp"
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <limits>
#include <random>
#include <stdexcept>
#include <string>

namespace eurobot::ros {
using namespace std::chrono_literals;
class Bridge final : public rclcpp::Node {
  using Trigger = std_srvs::srv::Trigger;
  using Arm = eurobot_interfaces::srv::Arm;
  using Maintenance = eurobot_interfaces::srv::Maintenance;
  using Outputs = eurobot_interfaces::srv::SetOutputs;
  using Actuator = eurobot_interfaces::srv::SetActuator;
  using State = eurobot_interfaces::msg::ControllerState;
  using Gripper = eurobot_interfaces::action::Gripper;
  using GripperGoal = rclcpp_action::ServerGoalHandle<Gripper>;

public:
  Bridge() : Node("eurobot_bridge") {
    rcl_interfaces::msg::ParameterDescriptor descriptor{};
    descriptor.read_only = true;
    path_ = declare_parameter<std::string>("port", "/dev/serial/by-id/CONFIGURE_ME", descriptor);
    const auto baud = declare_parameter<std::int64_t>("baud", 115200, descriptor);
    const auto timeout = declare_parameter<std::int64_t>("ack_timeout_ms", 60, descriptor);
    maintenance_enabled_ = declare_parameter<bool>("maintenance_enabled", false, descriptor);
    frame_ = declare_parameter<std::string>("command_frame", "base_link", descriptor);
    const auto current_sign =
        declare_parameter<std::int64_t>("battery_current_sign", 0, descriptor);
    if (current_sign < -1 || current_sign > 1)
      throw std::invalid_argument("battery_current_sign must be -1, 0 (unknown), or +1");
    current_sign_ = static_cast<int>(current_sign);
    if (baud <= 0 || static_cast<std::uint64_t>(baud) > std::numeric_limits<unsigned>::max() ||
        timeout < 5 || timeout > 100 || frame_.empty())
      throw std::invalid_argument("invalid bridge parameters");
    baud_ = static_cast<unsigned>(baud);
    timeout_ = std::chrono::milliseconds(timeout);
    state_pub_ = create_publisher<State>("state", rclcpp::QoS(1).reliable());
    battery_pub_ =
        create_publisher<sensor_msgs::msg::BatteryState>("battery", rclcpp::QoS(1).reliable());
    inputs_pub_ = create_publisher<eurobot_interfaces::msg::BoardInputs>("board_inputs",
                                                                         rclcpp::QoS(1).reliable());
    diagnostics_pub_ = create_publisher<diagnostic_msgs::msg::DiagnosticArray>(
        "diagnostics", rclcpp::QoS(1).reliable());
    twist_sub_ = create_subscription<geometry_msgs::msg::TwistStamped>(
        "cmd_vel", rclcpp::QoS(1).best_effort().durability_volatile(),
        [this](const geometry_msgs::msg::TwistStamped &message, const rclcpp::MessageInfo &info) {
          on_twist(message, info);
        });
    arm_ = create_service<Arm>("arm", [this](const std::shared_ptr<Arm::Request> request,
                                             std::shared_ptr<Arm::Response> response) {
      if (request->maintenance_session && !maintenance_enabled_) {
        response->message = "maintenance sessions are disabled by the launch configuration";
        return;
      }
      const auto publishers = count_publishers(twist_sub_->get_topic_name());
      const auto decision =
          authority_.decide_arm(request->control_token, gate_.armed || maintenance_session_,
                                request->maintenance_session ? 0 : publishers);
      if (decision == ArmDecision::idempotent &&
          request->maintenance_session == maintenance_session_) {
        response->accepted = true;
        response->control_token = authority_.token();
        response->message = maintenance_session_ ? "this disarmed maintenance lease is already held"
                                                 : "this control session is already armed";
        return;
      }
      if (decision != ArmDecision::acquire) {
        response->message = "active/stale token, conflicting publisher or different session mode; "
                            "release before acquiring a new session";
        return;
      }
      if (gripper_reservation_.busy()) {
        response->message = "gripper job must reach a terminal state before a new session";
        return;
      }
      authority_.reset();
      maintenance_session_ = false;
      velocity_ = {};
      have_velocity_ = false;
      motion_started_ = false;
      if (!transport_.connected()) {
        response->message = "serial device is disconnected";
        return;
      }
      if (wire_lease_owned()) {
        const auto released = command(wire::Type::release, {});
        if (!released || *released != 0) {
          response->message = "previous lease release was not confirmed";
          return;
        }
      }
      new_owner();
      maintenance_session_ = request->maintenance_session;
      arm_requested_ = false;
      std::array<std::uint8_t, 4> lease{};
      put_u32(lease, 500);
      const auto claimed = command(wire::Type::claim, lease);
      if (request->maintenance_session) {
        response->accepted = claimed && *claimed == 0 && wire_lease_owned() &&
                             (status_[1] & 1U) == 0 && authority_.grant(owner_);
      } else {
        arm_requested_ = true;
        const auto rearmed =
            claimed && *claimed == 0 ? command(wire::Type::rearm, {}) : std::nullopt;
        response->accepted = rearmed && *rearmed == 0 && gate_.armed && authority_.grant(owner_);
      }
      if (!response->accepted) {
        authority_.reset();
        maintenance_session_ = false;
        arm_requested_ = false;
        gate_.armed = false;
      } else {
        authority_reason_.clear();
        motion_epoch_.reset(now().nanoseconds());
      }
      response->control_token = response->accepted ? authority_.token() : 0;
      response->message =
          response->accepted ? (request->maintenance_session
                                    ? "disarmed maintenance lease acquired; outputs remain disabled"
                                    : "control session armed; keep token and send fresh commands")
                             : "lease/arming rejected or ACK unavailable";
    });
    maintenance_ = create_service<Maintenance>(
        "maintenance", [this](const std::shared_ptr<Maintenance::Request> request,
                              std::shared_ptr<Maintenance::Response> response) {
          MaintenanceRequest operation{request->operation, request->channel, request->argument,
                                       request->secondary, request->values};
          if (!valid_maintenance_request(operation)) {
            response->error_code = 2;
            response->message = "invalid typed maintenance operation/arguments";
            return;
          }
          if (!maintenance_read_only(request->operation)) {
            const bool owned = request->control_token != 0 &&
                               request->control_token == authority_.token() &&
                               request->control_token == owner_ && wire_lease_owned();
            const bool requires_armed =
                request->operation == 22 || request->operation == 26 || request->operation == 33;
            const bool either_mode = request->operation == 23 || request->operation == 25;
            const bool correct_mode =
                either_mode ||
                (requires_armed ? gate_.armed : (maintenance_session_ && (status_[1] & 1U) == 0));
            if (!maintenance_enabled_ || !owned || !correct_mode || gripper_reservation_.busy()) {
              response->error_code = 5;
              response->message = "explicit maintenance configuration, matching owned lease, "
                                  "correct armed/disarmed mode and idle job required";
              return;
            }
          }
          std::array<std::uint8_t, 22> payload{};
          if (encode_maintenance_request(operation, payload) != payload.size()) {
            response->error_code = 2;
            response->message = "invalid maintenance payload";
            return;
          }
          const auto frame = exchange(static_cast<wire::Type>(0x24), payload);
          if (!frame) {
            response->error_code = 9;
            response->message = "maintenance ACK unavailable; outcome unknown";
            return;
          }
          const auto reply = decode_maintenance_reply(
              request->operation, std::span(frame->payload).first(frame->length), request->channel,
              request->argument);
          if (!reply) {
            fault();
            response->error_code = 11;
            response->message = "malformed maintenance ACK";
            return;
          }
          response->error_code = reply->error;
          response->accepted = reply->error == 0;
          response->data.assign(reply->data.begin(), reply->data.begin() + reply->length);
          response->message =
              response->accepted
                  ? "typed operation acknowledged; consult documented response/evidence"
                  : "firmware rejected maintenance operation";
        });
    disarm_ =
        create_service<Trigger>("disarm", [this](const std::shared_ptr<Trigger::Request>,
                                                 std::shared_ptr<Trigger::Response> response) {
          finish_gripper(3, "control session stopped by operator", false);
          authority_.reset();
          maintenance_session_ = false;
          arm_requested_ = false;
          gate_.armed = false;
          have_velocity_ = false;
          const auto result = command(wire::Type::release, {});
          response->success = result && *result == 0;
          response->message = response->success
                                  ? "lease released"
                                  : "release not confirmed; firmware lease timeout remains active";
        });
    estop_ = create_service<Trigger>(
        "emergency_stop", [this](const std::shared_ptr<Trigger::Request>,
                                 std::shared_ptr<Trigger::Response> response) {
          finish_gripper(3, "control session stopped by operator", false);
          authority_.reset();
          maintenance_session_ = false;
          arm_requested_ = false;
          gate_.armed = false;
          have_velocity_ = false;
          const auto result = command(wire::Type::estop, {});
          response->success = result && *result == 0;
          response->message = response->success
                                  ? "firmware emergency stop acknowledged"
                                  : "stop unconfirmed: use hardware emergency isolation";
        });
    outputs_ = create_service<Outputs>(
        "set_outputs", [this](const std::shared_ptr<Outputs::Request> request,
                              std::shared_ptr<Outputs::Response> response) {
          if (!authority_.allows(request->control_token, gate_.armed) ||
              request->control_token != owner_) {
            response->error_code = 5;
            response->message = "active control token required";
            return;
          }
          std::array<std::uint8_t, 16> payload{};
          put_u32(std::span(payload).subspan(0, 4), request->pumps_select);
          put_u32(std::span(payload).subspan(4, 4), request->pumps_values);
          put_u32(std::span(payload).subspan(8, 4), request->valves_select);
          put_u32(std::span(payload).subspan(12, 4), request->valves_values);
          const auto result = command(wire::Type::pumps, payload);
          response->error_code = result.value_or(9);
          response->accepted = result && *result == 0;
          response->message =
              response->accepted
                  ? "requested outputs accepted; consult commanded-known and measured-known masks"
                  : "output request rejected or outcome unknown";
        });
    actuator_ = create_service<Actuator>(
        "set_actuator", [this](const std::shared_ptr<Actuator::Request> request,
                               std::shared_ptr<Actuator::Response> response) {
          if (!authority_.allows(request->control_token, gate_.armed) ||
              request->control_token != owner_) {
            response->error_code = 5;
            response->message = "active control token required";
            return;
          }
          if (!valid_actuator(request->kind, request->id, request->value, request->rate)) {
            response->error_code = 2;
            response->message = "invalid kind, channel, value or rate; see typed interface units";
            return;
          }
          std::array<std::uint8_t, 10> payload{};
          payload[0] = request->kind;
          payload[1] = request->id;
          put_float(std::span(payload).subspan(2, 4), request->value);
          put_float(std::span(payload).subspan(6, 4), request->rate);
          const auto result = command(wire::Type::actuator, payload);
          response->error_code = result.value_or(9);
          response->accepted = result && *result == 0;
          response->message = response->accepted
                                  ? "actuator command acknowledged"
                                  : "actuator rejected, unsupported or outcome unknown";
        });
    gripper_server_ = rclcpp_action::create_server<Gripper>(
        get_node_base_interface(), get_node_clock_interface(), get_node_logging_interface(),
        get_node_waitables_interface(), "gripper",
        [this](const rclcpp_action::GoalUUID &uuid, std::shared_ptr<const Gripper::Goal> goal) {
          return accept_gripper(uuid, *goal);
        },
        [this](const std::shared_ptr<GripperGoal> goal) { return cancel_gripper(goal); },
        [this](const std::shared_ptr<GripperGoal> goal) { start_gripper(goal); });
    timer_ = create_wall_timer(50ms, [this] { tick(); });
  }
  ~Bridge() override {
    // Signal shutdown may invalidate the ROS context before this destructor runs.
    // Release UART ownership regardless; never publish an action result on a dead context.
    if (rclcpp::ok(get_node_base_interface()->get_context())) {
      try {
        finish_gripper(3, "bridge is shutting down; firmware outcome is unconfirmed", false);
      } catch (const std::exception &) {
        // Result delivery is best effort during teardown; the firmware lease remains bounded.
      }
    }
    gripper_goal_.reset();
    gripper_reservation_.reset();
    if (gate_.armed || maintenance_session_)
      static_cast<void>(command(wire::Type::release, {}));
  }

private:
  void new_owner() {
    authority_.reset();
    maintenance_session_ = false;
    motion_epoch_.reset(now().nanoseconds());
    std::random_device random;
    const auto candidate = static_cast<std::uint32_t>(random());
    owner_ = (candidate == 0 || candidate == owner_) ? (owner_ == UINT32_MAX ? 1U : owner_ + 1U)
                                                     : candidate;
    sequence_ = 0;
  }
  bool wire_lease_owned() const {
    return valid_status_ && (status_[1] & 4U) != 0 &&
           get_u32(std::span(status_).subspan(4, 4)) == owner_;
  }
  std::optional<Frame> exchange(wire::Type opcode, std::span<const std::uint8_t> payload) {
    if (!transport_.connected())
      return {};
    if (sequence_ == UINT32_MAX || payload.size() > wire::max_payload) {
      fault();
      return {};
    }
    Frame request{};
    request.type = static_cast<std::uint8_t>(opcode);
    request.sequence = ++sequence_;
    request.owner = owner_;
    request.length = static_cast<std::uint16_t>(payload.size());
    std::copy(payload.begin(), payload.end(), request.payload.begin());
    auto reply = transport_.transact(request, timeout_);
    if (!reply)
      fault();
    return reply;
  }
  std::optional<std::uint8_t> command(wire::Type opcode, std::span<const std::uint8_t> payload) {
    const auto response = exchange(opcode, payload);
    if (!response)
      return {};
    if (response->length != wire::status_size) {
      fault();
      return {};
    }
    std::copy_n(response->payload.begin(), wire::status_size, status_.begin());
    valid_status_ = true;
    gate_.armed = arm_requested_ && (status_[1] & 1U) != 0 &&
                  get_u32(std::span(status_).subspan(4, 4)) == owner_;
    if (!gate_.armed) {
      // A faulted MCU cannot auto-arm later from an unrelated status ACK.
      arm_requested_ = false;
      if (!maintenance_session_ || !wire_lease_owned()) {
        authority_.reset();
        maintenance_session_ = false;
      }
      have_velocity_ = false;
    }
    return status_[0];
  }
  void fault() {
    finish_gripper(9, "serial fault; gripper outcome is unconfirmed", false);
    authority_.reset();
    maintenance_session_ = false;
    sensors_ = SensorSample{};
    arm_requested_ = false;
    gate_.fault();
    have_velocity_ = false;
    valid_status_ = false;
    transport_.close();
  }
  void query_sensors() {
    sensors_ = SensorSample{};
    if (transport_.connected()) {
      const auto reply = exchange(wire::Type::sensors, {});
      if (reply) {
        const auto sample =
            decode_sensors(std::span(reply->payload).first(reply->length), current_sign_);
        if (sample)
          sensors_ = *sample;
        else
          fault();
      }
    }
    publish_sensors();
  }
  void publish_sensors() {
    sensor_msgs::msg::BatteryState battery{};
    battery.header.stamp = now();
    battery.header.frame_id = frame_;
    battery.voltage = sensors_.voltage;
    battery.current = sensors_.current;
    battery.percentage = sensors_.percentage;
    battery.temperature = unknown_measurement;
    battery.charge = unknown_measurement;
    battery.capacity = unknown_measurement;
    battery.design_capacity = unknown_measurement;
    battery.present = sensors_.battery_known;
    battery.cell_voltage.assign(sensors_.cell_voltage.begin(),
                                sensors_.cell_voltage.begin() + sensors_.cells);
    battery.cell_temperature.assign(sensors_.cells, unknown_measurement);
    battery_pub_->publish(battery);
    eurobot_interfaces::msg::BoardInputs inputs{};
    inputs.header = battery.header;
    inputs.levels = sensors_.levels;
    inputs.known_mask = sensors_.known_mask;
    inputs.battery_sample_known = sensors_.battery_known;
    inputs.query_error = sensors_.error;
    inputs.battery_error = sensors_.battery_error;
    inputs_pub_->publish(inputs);
  }
  void on_twist(const geometry_msgs::msg::TwistStamped &message, const rclcpp::MessageInfo &info) {
    const auto &t = message.twist;
    const auto &stamp = message.header.stamp;
    const bool stamp_valid =
        stamp.sec >= 0 && stamp.nanosec < 1000000000U && (stamp.sec != 0 || stamp.nanosec != 0);
    const auto source_stamp_ns =
        static_cast<std::int64_t>(stamp.sec) * 1000000000LL + stamp.nanosec;
    const double source_stamp = static_cast<double>(source_stamp_ns) * 1e-9;
    const double age = now().seconds() - source_stamp;
    const bool planar = t.linear.z == 0 && t.angular.x == 0 && t.angular.y == 0;
    const bool valid = stamp_valid && motion_epoch_.accepts(source_stamp_ns) &&
                       message.header.frame_id == frame_ && std::isfinite(age) && age >= -0.05 &&
                       age <= 0.2 && std::isfinite(t.linear.x) && std::isfinite(t.linear.y) &&
                       std::isfinite(t.angular.z) && planar;
    if (gate_.armed && valid) {
      const std::array<double, 3> input{t.linear.x, t.linear.y, t.angular.z};
      bool representable = true;
      for (auto value : input)
        representable = representable &&
                        std::abs(value) <= static_cast<double>(std::numeric_limits<float>::max());
      if (representable) {
        const auto &gid = info.get_rmw_message_info().publisher_gid;
        if (!authority_.observe_publisher(std::span{gid.data})) {
          finish_gripper(5, "cmd_vel publisher conflict revoked the control session", false);
          authority_reason_ =
              "cmd_vel publisher conflict or unavailable GID; explicit new arm required";
          authority_.reset();
          maintenance_session_ = false;
          arm_requested_ = false;
          gate_.armed = false;
          have_velocity_ = false;
          motion_started_ = false;
          static_cast<void>(command(wire::Type::release, {}));
          return;
        }
        if (gripper_owns_motion()) {
          have_velocity_ = false;
          authority_reason_ = "gripper action holds exclusive motion control";
          return;
        }
        for (unsigned i = 0; i < 3; ++i)
          velocity_[i] = static_cast<float>(input[i]);
        have_velocity_ = true;
        motion_started_ = true;
        velocity_deadline_ = std::chrono::steady_clock::now() +
                             std::chrono::duration_cast<std::chrono::steady_clock::duration>(
                                 std::chrono::duration<double>(std::min(0.2, 0.2 - age)));
      } else {
        have_velocity_ = false;
      }
    } else {
      have_velocity_ = false;
    }
  }
  void tick() {
    const auto clock = std::chrono::steady_clock::now();
    if (!transport_.connected() && clock >= next_reconnect_) {
      next_reconnect_ = clock + 1s;
      authority_.reset();
      maintenance_session_ = false;
      arm_requested_ = false;
      gate_.armed = false;
      have_velocity_ = false;
      valid_status_ = false;
      if (transport_.open(path_, baud_)) {
        new_owner();
      }
    }
    if (transport_.connected()) {
      if (maintenance_session_ && wire_lease_owned() && authority_.token() == owner_)
        static_cast<void>(command(wire::Type::heartbeat, {}));
      else if (gate_.armed) {
        if (gripper_owns_motion())
          static_cast<void>(command(wire::Type::heartbeat, {}));
        else if (have_velocity_ && clock <= velocity_deadline_) {
          const auto heartbeat = command(wire::Type::heartbeat, {});
          if (heartbeat && *heartbeat == 0 && gate_.armed &&
              std::chrono::steady_clock::now() <= velocity_deadline_) {
            std::array<std::uint8_t, 12> payload{};
            for (unsigned i = 0; i < 3; ++i)
              put_float(std::span(payload).subspan(i * 4, 4), velocity_[i]);
            static_cast<void>(command(wire::Type::twist, payload));
          }
        } else if (motion_started_) {
          // A stale publisher stops all outputs and revokes the lease; it never resumes
          // automatically.
          authority_.reset();
          maintenance_session_ = false;
          arm_requested_ = false;
          gate_.armed = false;
          have_velocity_ = false;
          static_cast<void>(command(wire::Type::release, {}));
        } else
          static_cast<void>(command(wire::Type::heartbeat, {}));
      } else
        static_cast<void>(command(wire::Type::status, {}));
    }
    if (gripper_reservation_.busy())
      update_gripper();
    if (clock >= next_sensor_query_) {
      next_sensor_query_ = clock + 500ms;
      query_sensors();
    }
    if (!transport_.connected()) {
      sensors_ = SensorSample{};
      publish_sensors();
    }
    publish();
  }
  static bool operation_uses_motion(std::uint8_t operation) {
    return operation == 6 || operation == 8 || operation == 9 || operation == 12 || operation == 22;
  }
  bool gripper_owns_motion() const {
    return gripper_reservation_.busy() && operation_uses_motion(gripper_request_.operation);
  }
  bool gripper_motion_idle() const {
    if (!valid_status_ || (status_[76] & 4U) == 0 ||
        !std::all_of(velocity_.begin(), velocity_.end(), [](float value) { return value == 0; }))
      return false;
    for (unsigned i = 0; i < 3; ++i)
      if (get_u32(std::span(status_).subspan(40 + 4 * i, 4)) != 0)
        return false;
    return true;
  }
  rclcpp_action::GoalResponse accept_gripper(const rclcpp_action::GoalUUID &uuid,
                                             const Gripper::Goal &goal) {
    GripperRequest request{goal.gripper, goal.operation, goal.value,     goal.argument,
                           goal.speed,   goal.mask,      goal.timeout_ms};
    const bool session = transport_.connected() &&
                         authority_.allows(goal.control_token, gate_.armed) &&
                         goal.control_token == owner_;
    if (!valid_gripper_request(request) ||
        !gripper_reservation_.reserve(session, !operation_uses_motion(request.operation) ||
                                                   gripper_motion_idle()))
      return rclcpp_action::GoalResponse::REJECT;
    reserved_gripper_uuid_ = uuid;
    gripper_request_ = request;
    gripper_token_ = goal.control_token;
    gripper_status_ = {};
    gripper_job_id_ = 0;
    gripper_submit_pending_ = false;
    gripper_cancel_pending_ = false;
    gripper_cancel_sent_ = false;
    gripper_timed_out_ = false;
    gripper_deadline_ =
        std::chrono::steady_clock::now() + std::chrono::milliseconds(goal.timeout_ms);
    if (operation_uses_motion(request.operation)) {
      have_velocity_ = false;
      motion_started_ = false;
      velocity_ = {};
    }
    return rclcpp_action::GoalResponse::ACCEPT_AND_EXECUTE;
  }
  rclcpp_action::CancelResponse cancel_gripper(const std::shared_ptr<GripperGoal> &goal) {
    if (!gripper_reservation_.busy() || !reserved_gripper_uuid_ ||
        goal->get_goal_id() != *reserved_gripper_uuid_)
      return rclcpp_action::CancelResponse::REJECT;
    gripper_cancel_pending_ = true;
    return rclcpp_action::CancelResponse::ACCEPT;
  }
  void start_gripper(const std::shared_ptr<GripperGoal> &goal) {
    if (!reserved_gripper_uuid_ || goal->get_goal_id() != *reserved_gripper_uuid_ ||
        !gripper_reservation_.busy()) {
      auto result = std::make_shared<Gripper::Result>();
      result->error_code = 3;
      result->message = "session ended before firmware submission";
      goal->abort(result);
      return;
    }
    gripper_goal_ = goal;
    gripper_submit_pending_ = true;
  }
  void publish_gripper_feedback() {
    if (!gripper_goal_ || !gripper_goal_->is_active() || !gripper_status_.snapshot_known)
      return;
    auto feedback = std::make_shared<Gripper::Feedback>();
    feedback->job_id = gripper_status_.job_id;
    feedback->phase = static_cast<std::uint8_t>(gripper_status_.phase);
    feedback->operation = gripper_status_.operation;
    feedback->step = gripper_status_.step;
    feedback->step_count = gripper_status_.count;
    feedback->completion_evidence = static_cast<std::uint8_t>(gripper_status_.evidence);
    feedback->commanded_steps = gripper_status_.commanded_steps;
    feedback->settled_steps = gripper_status_.settled_steps;
    feedback->referenced_steps = gripper_status_.referenced_steps;
    feedback->measured_steps = gripper_status_.measured_steps;
    feedback->elapsed_ms = gripper_status_.elapsed_ms;
    feedback->progress = gripper_status_.count == 0 ? 0.0F
                                                    : static_cast<float>(gripper_status_.step) /
                                                          static_cast<float>(gripper_status_.count);
    gripper_goal_->publish_feedback(feedback);
  }
  void finish_gripper(std::uint8_t error, const std::string &message, bool outcome_known) {
    if (!gripper_reservation_.busy() && !gripper_goal_)
      return;
    auto goal = gripper_goal_;
    auto result = std::make_shared<Gripper::Result>();
    result->outcome_known = outcome_known;
    result->error_code = error;
    result->stop_error = gripper_status_.stop_error;
    result->job_id = gripper_status_.job_id;
    result->phase = static_cast<std::uint8_t>(gripper_status_.phase);
    result->completion_evidence = static_cast<std::uint8_t>(gripper_status_.evidence);
    result->commanded_steps = gripper_status_.commanded_steps;
    result->settled_steps = gripper_status_.settled_steps;
    result->referenced_steps = gripper_status_.referenced_steps;
    result->measured_steps = gripper_status_.measured_steps;
    result->elapsed_ms = gripper_status_.elapsed_ms;
    result->message = message;
    result->completed = outcome_known && error == 0 &&
                        gripper_status_.phase == GripperPhase::completed &&
                        gripper_status_.job_error == 0 && gripper_status_.stop_error == 0;
    result->physically_verified = result->completed && gripper_status_.fully_measured();
    const bool cancelled = outcome_known && error == 0 &&
                           gripper_status_.phase == GripperPhase::cancelled &&
                           gripper_status_.stop_error == 0;
    const bool held_motion = gripper_owns_motion();
    gripper_goal_.reset();
    reserved_gripper_uuid_.reset();
    gripper_reservation_.reset();
    gripper_submit_pending_ = false;
    gripper_cancel_pending_ = false;
    gripper_cancel_sent_ = false;
    if (held_motion || gripper_status_.phase == GripperPhase::cancelled) {
      motion_epoch_.reset(now().nanoseconds());
      have_velocity_ = false;
      motion_started_ = false;
      velocity_ = {};
    }
    if (goal && goal->is_active()) {
      if (result->completed)
        goal->succeed(result);
      else if (cancelled && goal->is_canceling())
        goal->canceled(result);
      else
        goal->abort(result);
    }
  }
  void revoke_gripper_session() {
    authority_.reset();
    arm_requested_ = false;
    gate_.armed = false;
    have_velocity_ = false;
    motion_started_ = false;
    if (transport_.connected())
      static_cast<void>(command(wire::Type::release, {}));
  }
  void update_gripper() {
    if (!gripper_goal_)
      return;
    const bool normal_control = transport_.connected() &&
                                authority_.allows(gripper_token_, gate_.armed) &&
                                gripper_token_ == owner_;
    // Query-only recovery: the MCU may fault/disarm a job before our next heartbeat ACK.
    // Retain no actuation authority, no heartbeat renewal and no cancel/submit permission.
    const bool final_query_only = !normal_control && transport_.connected() && !gate_.armed &&
                                  gripper_job_id_ != 0 && gripper_token_ == owner_ &&
                                  wire_lease_owned();
    if (!normal_control && !final_query_only) {
      finish_gripper(3, "control session lost; firmware outcome is unconfirmed", false);
      return;
    }
    const auto now_steady = std::chrono::steady_clock::now();
    if (!final_query_only && now_steady >= gripper_deadline_) {
      gripper_timed_out_ = true;
      gripper_cancel_pending_ = true;
    }
    if (!final_query_only && gripper_submit_pending_ && gripper_cancel_pending_) {
      gripper_status_ = {};
      gripper_status_.phase = GripperPhase::cancelled;
      gripper_status_.snapshot_known = true;
      finish_gripper(gripper_timed_out_ ? 9 : 0, "cancelled before any firmware submission", true);
      return;
    }
    if (!final_query_only && gripper_cancel_sent_ && now_steady >= gripper_cancel_deadline_) {
      revoke_gripper_session();
      finish_gripper(9, "cancellation confirmation deadline exceeded; outcome unconfirmed", false);
      return;
    }
    if (!final_query_only && !gripper_submit_pending_ && !gripper_cancel_pending_ &&
        now_steady < next_gripper_poll_)
      return;
    std::optional<Frame> reply;
    const bool submitting = !final_query_only && gripper_submit_pending_;
    bool sent_cancel = false;
    if (submitting) {
      std::array<std::uint8_t, 22> payload{};
      const auto length = encode_gripper_request(gripper_request_, payload);
      if (length != payload.size()) {
        finish_gripper(2, "invalid gripper request", true);
        return;
      }
      gripper_submit_pending_ = false;
      reply = exchange(static_cast<wire::Type>(0x21), payload);
    } else {
      std::array<std::uint8_t, 4> payload{};
      put_u32(payload, gripper_job_id_);
      const bool cancel = !final_query_only && gripper_cancel_pending_ && !gripper_cancel_sent_;
      sent_cancel = cancel;
      if (cancel) {
        gripper_cancel_sent_ = true;
        gripper_cancel_deadline_ = now_steady + 500ms;
      }
      reply = exchange(static_cast<wire::Type>(cancel ? 0x23 : 0x22), payload);
    }
    next_gripper_poll_ = std::chrono::steady_clock::now() + 50ms;
    if (!reply)
      return; // exchange already faults, aborts and invalidates the session.
    auto snapshot = decode_gripper_reply(std::span(reply->payload).first(reply->length));
    if (!snapshot) {
      fault();
      return;
    }
    const bool identity_matches = snapshot->snapshot_known && snapshot->job_id != 0 &&
                                  snapshot->owner == owner_ &&
                                  (gripper_job_id_ == 0 || snapshot->job_id == gripper_job_id_) &&
                                  snapshot->operation == gripper_request_.operation &&
                                  snapshot->gripper == gripper_request_.gripper;
    if (snapshot->error != 0) {
      // Engine::cancel rejects an already-terminal job with invalid_argument. The
      // matching complete snapshot still proves that job's outcome, not a cancellation.
      if (sent_cancel && snapshot->error == 2 && identity_matches && snapshot->terminal()) {
        snapshot->error = 0;
      } else {
        const auto error = snapshot->error;
        if (submitting)
          finish_gripper(error, "firmware rejected submit; job did not start", true);
        else {
          revoke_gripper_session();
          finish_gripper(error, "firmware rejected a running-job transaction; outcome unconfirmed",
                         false);
        }
        return;
      }
    }
    if (!identity_matches) {
      fault();
      return;
    }
    if (final_query_only && !snapshot->terminal()) {
      revoke_gripper_session();
      finish_gripper(3, "disarmed MCU has no terminal job snapshot; outcome unconfirmed", false);
      return;
    }
    gripper_status_ = *snapshot;
    gripper_job_id_ = snapshot->job_id;
    publish_gripper_feedback();
    if (gripper_status_.terminal()) {
      const auto reported_error =
          gripper_status_.job_error != 0 ? gripper_status_.job_error : gripper_status_.stop_error;
      const auto error =
          gripper_timed_out_
              ? std::uint8_t{9}
              : (reported_error == 0 && gripper_status_.phase == GripperPhase::faulted
                     ? std::uint8_t{8}
                     : reported_error);
      const bool failed =
          gripper_timed_out_ || gripper_status_.phase == GripperPhase::faulted || error != 0;
      if (failed || final_query_only)
        revoke_gripper_session();
      finish_gripper(error, "firmware gripper terminal snapshot received", true);
    }
  }
  void publish() {
    State state{};
    state.header.stamp = now();
    state.header.frame_id = frame_;
    state.connected = transport_.connected();
    state.state_valid = valid_status_;
    state.armed = gate_.armed;
    state.transport_faults = gate_.faults;
    state.transport_status = transport_.error();
    if (valid_status_) {
      const auto read = [this](unsigned offset) {
        return get_u32(std::span(status_).subspan(offset, 4));
      };
      state.estop_latched = (status_[1] & 2U) != 0;
      state.lease_active = (status_[1] & 4U) != 0;
      state.controller_fault = (status_[1] & 8U) != 0;
      state.firmware_error = status_[0];
      state.pump_channels = status_[2];
      state.owner = read(4);
      state.pumps_requested = read(8);
      state.pumps_commanded = read(12);
      state.pumps_commanded_known = read(16);
      state.pumps_measured = read(20);
      state.pumps_known = read(24);
      state.valves_requested = read(28);
      state.valves_commanded = read(32);
      state.valves_commanded_known = read(36);
      state.wheels_measured_known = status_[3];
      for (unsigned i = 0; i < 3; ++i) {
        state.wheel_commanded_ticks_per_minute[i] = std::bit_cast<std::int32_t>(read(40 + 4 * i));
        state.wheel_measured_ticks_per_minute[i] = std::bit_cast<std::int32_t>(read(52 + 4 * i));
        state.pose_x_y_yaw[i] = std::bit_cast<float>(read(64 + 4 * i));
      }
      state.wheels_commanded_known = (status_[76] & 4U) != 0;
      state.pose_known = (status_[76] & 2U) != 0;
      state.controller_error = status_[77];
      state.pump_error = status_[78];
      state.chassis_error = status_[79];
      state.uptime_ms = read(80);
    }
    state_pub_->publish(state);
    diagnostic_msgs::msg::DiagnosticArray diagnostics{};
    diagnostics.header.stamp = state.header.stamp;
    diagnostic_msgs::msg::DiagnosticStatus detail{};
    detail.name = "eurobot/controller";
    detail.hardware_id = path_;
    detail.level = state.connected
                       ? ((state.armed && !state.estop_latched && !state.controller_fault &&
                           state.firmware_error == 0 && state.controller_error == 0 &&
                           state.pump_error == 0 && state.chassis_error == 0)
                              ? 0
                              : 1)
                       : 2;
    detail.message = state.transport_status;
    const auto add = [&detail](const std::string &key, const std::string &value) {
      diagnostic_msgs::msg::KeyValue item{};
      item.key = key;
      item.value = value;
      detail.values.push_back(item);
    };
    add("firmware_error", std::to_string(state.firmware_error));
    add("transport_faults", std::to_string(state.transport_faults));
    add("controller_error", std::to_string(state.controller_error));
    add("pump_error", std::to_string(state.pump_error));
    add("chassis_error", std::to_string(state.chassis_error));
    add("battery_query_error", std::to_string(sensors_.error));
    add("battery_error", std::to_string(sensors_.battery_error));
    add("battery_current_raw_ma",
        sensors_.battery_known ? std::to_string(sensors_.raw_current_ma) : "unknown");
    add("gripper_exclusive_motion", gripper_owns_motion() ? "true" : "false");
    add("gripper_busy", gripper_reservation_.busy() ? "true" : "false");
    add("gripper_job_id", std::to_string(gripper_job_id_));
    add("control_authority", authority_reason_);
    add("control_session",
        maintenance_session_ ? "maintenance_disarmed" : (gate_.armed ? "armed" : "none"));
    add("control_token", std::to_string(authority_.token()));
    add("owner", std::to_string(state.owner));
    add("pump_measured_known", std::to_string(state.pumps_known));
    diagnostics.status.push_back(detail);
    diagnostics_pub_->publish(diagnostics);
  }
  Transport transport_{};
  Gate gate_{};
  Authority authority_{};
  GripperReservation gripper_reservation_{};
  std::optional<rclcpp_action::GoalUUID> reserved_gripper_uuid_{};
  std::shared_ptr<GripperGoal> gripper_goal_{};
  GripperRequest gripper_request_{};
  GripperReply gripper_status_{};
  std::uint32_t gripper_token_{}, gripper_job_id_{};
  bool gripper_submit_pending_{}, gripper_cancel_pending_{}, gripper_cancel_sent_{},
      gripper_timed_out_{};
  std::chrono::steady_clock::time_point gripper_deadline_{}, gripper_cancel_deadline_{},
      next_gripper_poll_{};
  rclcpp_action::Server<Gripper>::SharedPtr gripper_server_{};
  std::string authority_reason_{};
  SensorSample sensors_{};
  int current_sign_{};
  std::string path_, frame_;
  unsigned baud_{};
  std::chrono::milliseconds timeout_{60};
  std::uint32_t owner_{1}, sequence_{0};
  std::array<std::uint8_t, wire::status_size> status_{};
  bool maintenance_enabled_{false}, maintenance_session_{false};
  bool valid_status_{false}, have_velocity_{false}, motion_started_{false}, arm_requested_{false};
  std::array<float, 3> velocity_{};
  MotionEpoch motion_epoch_{};
  std::chrono::steady_clock::time_point velocity_deadline_{}, next_reconnect_{},
      next_sensor_query_{};
  rclcpp::Publisher<State>::SharedPtr state_pub_;
  rclcpp::Publisher<sensor_msgs::msg::BatteryState>::SharedPtr battery_pub_;
  rclcpp::Publisher<eurobot_interfaces::msg::BoardInputs>::SharedPtr inputs_pub_;
  rclcpp::Publisher<diagnostic_msgs::msg::DiagnosticArray>::SharedPtr diagnostics_pub_;
  rclcpp::Subscription<geometry_msgs::msg::TwistStamped>::SharedPtr twist_sub_;
  rclcpp::Service<Arm>::SharedPtr arm_;
  rclcpp::Service<Maintenance>::SharedPtr maintenance_;
  rclcpp::Service<Trigger>::SharedPtr disarm_, estop_;
  rclcpp::Service<Outputs>::SharedPtr outputs_;
  rclcpp::Service<Actuator>::SharedPtr actuator_;
  rclcpp::TimerBase::SharedPtr timer_;
};
} // namespace eurobot::ros
int main(int argc, char **argv) {
  rclcpp::init(argc, argv);
  auto bridge = std::make_shared<eurobot::ros::Bridge>();
  rclcpp::executors::SingleThreadedExecutor executor;
  executor.add_node(bridge);
  executor.spin();
  executor.remove_node(bridge);
  bridge.reset();
  rclcpp::shutdown();
  return 0;
}
