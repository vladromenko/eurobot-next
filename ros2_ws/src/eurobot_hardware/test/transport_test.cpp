#include "eurobot_hardware/transport.hpp"
#include <array>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <iostream>
#include <limits>
#include <poll.h>
#include <thread>
#include <unistd.h>
#ifdef __APPLE__
#include <util.h>
#else
#include <pty.h>
#endif
using namespace eurobot::ros;
using namespace std::chrono_literals;
static void check(bool value, const char *what) {
  if (!value) {
    std::cerr << what << '\n';
    std::exit(1);
  }
}
struct Pty {
  int master{-1}, slave{-1};
  std::array<char, 128> path{};
  Pty() { check(openpty(&master, &slave, path.data(), nullptr, nullptr) == 0, "openpty"); }
  ~Pty() {
    if (master >= 0)
      ::close(master);
    if (slave >= 0)
      ::close(slave);
  }
};
static Frame request() {
  Frame frame{};
  frame.type = 8;
  frame.owner = 0x12345678;
  frame.sequence = 7;
  return frame;
}
static void serve(int master, bool respond, bool wrong_first = false) {
  FrameParser parser;
  bool done = false;
  const auto end = std::chrono::steady_clock::now() + 500ms;
  while (!done && std::chrono::steady_clock::now() < end) {
    pollfd fd{master, POLLIN, 0};
    if (poll(&fd, 1, 20) > 0) {
      std::array<std::uint8_t, 256> bytes{};
      const auto count = ::read(master, bytes.data(), bytes.size());
      for (ssize_t i = 0; i < count; ++i) {
        auto frame = parser.push(bytes[static_cast<std::size_t>(i)], monotonic_ms());
        if (frame) {
          done = true;
          if (respond) {
            frame->type |= 0x80;
            frame->length = 84;
            std::array<std::uint8_t, 160> output{};
            if (wrong_first) {
              const auto send = [&] {
                const auto size = encode_frame(*frame, output);
                check(::write(master, output.data(), size) == static_cast<ssize_t>(size),
                      "write unmatched ACK");
              };
              ++frame->sequence;
              send();
              --frame->sequence;
              ++frame->owner;
              send();
              --frame->owner;
              ++frame->type;
              send();
              --frame->type;
              const auto size = encode_frame(*frame, output);
              output[size - 1] ^= 0x80;
              check(::write(master, output.data(), size) == static_cast<ssize_t>(size),
                    "write corrupt CRC");
            }
            auto length = encode_frame(*frame, output);
            // Fragment ACK across reads, testing incremental parser.
            check(::write(master, output.data(), 5) == 5, "write first ACK fragment");
            std::this_thread::sleep_for(2ms);
            check(::write(master, output.data() + 5, length - 5) ==
                      static_cast<ssize_t>(length - 5),
                  "write ACK remainder");
          }
        }
      }
    }
  }
}
int main() {
  {
    MotionEpoch epoch;
    constexpr std::int64_t stop = 1000000000;
    epoch.reset(stop);
    check(!epoch.accepts(stop - 1) && !epoch.accepts(stop + 40000000) &&
              !epoch.accepts(stop + MotionEpoch::permitted_future_ns),
          "cancel/arm excludes old source stamps including the full permitted future skew");
    check(epoch.accepts(stop + MotionEpoch::permitted_future_ns + 1),
          "fresh local-clock commands work after the bounded 50ms quarantine");
    check(epoch.accepts(stop + 60000000 + 40000000),
          "fresh positive-offset source clock is usable after the new epoch");
    epoch.reset(std::numeric_limits<std::int64_t>::max() - 1);
    check(!epoch.accepts(std::numeric_limits<std::int64_t>::max()),
          "motion epoch future-skew addition saturates without signed overflow");
  }
  {
    MaintenanceRequest operation{6, 2, 0x12345678, 0x90abcdef, {1.25F, -2.5F, 0.75F}};
    std::array<std::uint8_t, 22> payload{};
    check(encode_maintenance_request(operation, payload) == 22 && payload[0] == 6 &&
              payload[1] == 2 && get_u32(std::span(payload).subspan(2, 4)) == 0x12345678 &&
              get_u32(std::span(payload).subspan(6, 4)) == 0x90abcdef &&
              std::bit_cast<float>(get_u32(std::span(payload).subspan(14, 4))) == -2.5F,
          "typed maintenance payload offsets and pose units");
    check(maintenance_read_only(0) && maintenance_read_only(3) && maintenance_read_only(27) &&
              !maintenance_read_only(1) && !maintenance_read_only(22),
          "board diagnostics are separate from privileged mutations");
    operation.operation = 8;
    operation.argument = 2;
    check(!valid_maintenance_request(operation), "servo LED requires Boolean argument");
    operation.operation = 12;
    operation.argument = 254;
    check(!valid_maintenance_request(operation), "servo address excludes reserved IDs");
    operation.operation = 20;
    operation.argument = 65536;
    check(!valid_maintenance_request(operation), "BMS calibration value is bounded to uint16");
    operation.operation = 6;
    operation.values[0] = std::numeric_limits<float>::infinity();
    check(!valid_maintenance_request(operation), "pose and maintenance floats must be finite");
    std::array<std::uint8_t, 17> info{};
    info[1] = 1;
    put_u32(std::span(info).subspan(13, 4), 42);
    auto decoded = decode_maintenance_reply(0, info);
    check(decoded && decoded->error == 0 && decoded->length == 16 &&
              get_u32(std::span(decoded->data).subspan(12, 4)) == 42,
          "board-info response preserves reset flags and owner bytes");
    check(!decode_maintenance_reply(0, std::span(info).first(16)), "truncated board info rejected");
    const std::array<std::uint8_t, 14> reset_readback{0, 7};
    const auto reset = decode_maintenance_reply(5, reset_readback);
    check(reset && reset->length == 13 && reset->data[0] == 7,
          "wheel counter reset requires exact verified readback, not an empty command ACK");
    const std::array<std::uint8_t, 10> event{};
    const std::array<std::uint8_t, 25> snapshot{};
    check(decode_maintenance_reply(28, event) && decode_maintenance_reply(21, snapshot),
          "application snapshot24 and queued event9 have exact payloads");
    const std::array<std::uint8_t, 4> battery_voltage{};
    check(decode_maintenance_reply(30, battery_voltage, 2) &&
              !decode_maintenance_reply(30, battery_voltage, 1),
          "battery value query length depends on explicit selected quantity");
    const std::array<std::uint8_t, 3> stepper_status{};
    const std::array<std::uint8_t, 5> stepper_parameter{};
    check(decode_maintenance_reply(34, stepper_status, 0, 0) &&
              decode_maintenance_reply(34, stepper_parameter, 0, 1) &&
              !decode_maintenance_reply(34, stepper_status, 0, 1),
          "stepper status is uint16 and named get-parameter response is uint32");
    const std::array<std::uint8_t, 8> deployment_info{0, 1, 0, 1, 0, 0, 4, 0};
    MaintenanceRequest info_request{37, 0, 0, 0, {}};
    check(maintenance_read_only(37) && valid_maintenance_request(info_request) &&
              decode_maintenance_reply(37, deployment_info),
          "diagnostic deployment is read only");
    info_request.channel = 1;
    check(!valid_maintenance_request(info_request) &&
              !decode_maintenance_reply(37, std::span(deployment_info).first(7)),
          "deployment query rejects extra fields/truncation");
    MaintenanceRequest probe{36, 98, 0, 0, {}};
    check(maintenance_read_only(36) && valid_maintenance_request(probe),
          "servo discovery is read-only without control token");
    check(encode_maintenance_request(probe, payload) == 22 && payload[0] == 36 && payload[1] == 98,
          "servo probe emits exact named unicast request");
    for (auto id : {0U, 254U, 255U}) {
      probe.channel = static_cast<std::uint8_t>(id);
      check(!valid_maintenance_request(probe), "servo probe forbids reserved/broadcast IDs");
    }
    probe.channel = 98;
    probe.argument = 1;
    check(!valid_maintenance_request(probe), "servo probe does not expose raw register argument");
    std::array<std::uint8_t, 13> discovery{0, 12, 0, 34, 98, 0, 0, 255, 3, 0x34, 0x12, 1, 2};
    check(decode_maintenance_reply(36, discovery, 98) &&
              !decode_maintenance_reply(36, std::span(discovery).first(12), 98),
          "servo probe has exact twelve-byte raw body");
    check(!decode_maintenance_reply(36, discovery, 99),
          "servo probe actual ID must match selected channel");
    discovery[11] = 2;
    check(!decode_maintenance_reply(36, discovery, 98), "servo probe validates torque boolean");
    discovery[11] = 1;
    discovery[12] = 3;
    check(!decode_maintenance_reply(36, discovery, 98),
          "servo probe validates status return level");
    const std::array<std::uint8_t, 2> malformed_probe_nack{4, 0};
    check(!decode_maintenance_reply(36, malformed_probe_nack, 98),
          "servo probe NACK cannot include success body");
    const std::array<std::uint8_t, 1> ok{0}, nack{5};
    check(decode_maintenance_reply(2, ok) && !decode_maintenance_reply(3, ok),
          "empty mutation ACK cannot masquerade as wheel feedback");
    decoded = decode_maintenance_reply(3, nack);
    check(decoded && decoded->error == 5 && decoded->length == 0,
          "maintenance NACK never becomes known measurement");
  }
  {
    GripperReservation reservation{};
    check(!reservation.reserve(false, true) && !reservation.reserve(true, false),
          "gripper requires an active session and idle commanded chassis");
    check(reservation.reserve(true, true) && reservation.busy() && !reservation.reserve(true, true),
          "one accepted gripper goal reserves the mechanism before firmware submission");
    reservation.reset();
    check(!reservation.busy() && reservation.reserve(true, true),
          "terminal state releases the gripper reservation");
    GripperRequest goal{1, 6, -0.25F, 250.0F, 0, 2, 3000};
    std::array<std::uint8_t, 22> payload{};
    check(encode_gripper_request(goal, payload) == 22 && payload[0] == 1 && payload[1] == 6 &&
              std::bit_cast<float>(get_u32(std::span(payload).subspan(2, 4))) == -0.25F &&
              get_u32(std::span(payload).subspan(6, 4)) == 2 &&
              get_u32(std::span(payload).subspan(10, 4)) == 3000 &&
              std::bit_cast<float>(get_u32(std::span(payload).subspan(14, 4))) == 250.0F &&
              get_u32(std::span(payload).subspan(18, 4)) == 0,
          "gripper submit uses the locked 22-byte offsets including timed motion arguments");
    check(encode_gripper_request(goal, std::span(payload).first(21)) == 0,
          "gripper encoder never truncates a command");
    goal.mask = 3;
    check(!valid_gripper_request(goal), "timed motion has exactly three supported axes");
    goal.mask = 0;
    goal.argument = 0;
    check(!valid_gripper_request(goal), "timed motion requires a positive bounded duration");
    goal = {0, 8, 0.1F, -0.2F, 0.15F, 0, 5000};
    check(valid_gripper_request(goal), "2026 relative motion carries dx, dy and positive speed");
    goal.operation = 12;
    check(valid_gripper_request(goal), "2026 polling relative move uses the same units");
    goal.speed = 0;
    check(!valid_gripper_request(goal), "relative move cannot have zero speed");
    goal = {0, 9, 0, 0, 0, 2, 100};
    check(valid_gripper_request(goal), "wheel calibration takes a wheel index and profile limits");
    goal.mask = 3;
    check(!valid_gripper_request(goal), "wheel calibration rejects a fourth wheel");
    goal = {0, 22, 0, 0, 0, 15, 60000};
    check(valid_gripper_request(goal), "2026 capture and inclusive timeout/mask limits");
    goal.timeout_ms = 60001;
    check(!valid_gripper_request(goal), "unbounded gripper deadline rejected");
    goal.timeout_ms = 99;
    check(!valid_gripper_request(goal), "too short gripper deadline rejected");
    goal.timeout_ms = 1000;
    goal.value = std::numeric_limits<float>::quiet_NaN();
    check(!valid_gripper_request(goal), "nonfinite gripper values rejected");
    goal.value = 0;
    goal.operation = 255;
    check(!valid_gripper_request(goal), "unknown gripper operation rejected");
    goal.operation = 20;
    check(valid_gripper_request(goal), "2025 compatibility is explicit rather than a default");
    std::array<std::uint8_t, 25> reply{};
    reply[1] = 2;
    reply[2] = 22;
    reply[3] = 1;
    reply[4] = 2;
    reply[5] = 2;
    reply[6] = 2;
    reply[7] = 0;
    reply[8] = 2;
    put_u32(std::span(reply).subspan(13, 4), 123);
    put_u32(std::span(reply).subspan(17, 4), 42);
    put_u32(std::span(reply).subspan(21, 4), 750);
    auto snapshot = decode_gripper_reply(reply);
    check(snapshot && snapshot->snapshot_known && snapshot->terminal() &&
              !snapshot->fully_measured() && snapshot->job_id == 123 && snapshot->owner == 42 &&
              snapshot->elapsed_ms == 750,
          "settled terminal ACK is known command completion, never a measured grasp");
    reply[0] = 2;
    snapshot = decode_gripper_reply(reply);
    check(snapshot && snapshot->error == 2 && snapshot->snapshot_known &&
              snapshot->phase == GripperPhase::completed && snapshot->job_id == 123 &&
              snapshot->owner == 42 && snapshot->settled_steps == 2,
          "late-cancel NACK preserves the validated completed job identity and evidence");
    reply[4] = 1;
    snapshot = decode_gripper_reply(reply);
    check(snapshot && snapshot->error == 2 && !snapshot->snapshot_known,
          "malformed NACK body never proves terminal completion");
    reply[4] = 2;
    put_u32(std::span(reply).subspan(17, 4), 43);
    snapshot = decode_gripper_reply(reply);
    check(snapshot && snapshot->snapshot_known && snapshot->owner == 43,
          "decoder preserves foreign owner for the bridge's strict identity comparison");
    put_u32(std::span(reply).subspan(17, 4), 42);
    reply[0] = 0;
    reply[6] = 4;
    reply[8] = 0;
    reply[10] = 2;
    snapshot = decode_gripper_reply(reply);
    check(snapshot && snapshot->fully_measured(), "all steps require measured completion evidence");
    reply[12] = 8;
    snapshot = decode_gripper_reply(reply);
    check(snapshot && !snapshot->fully_measured(), "failed stop cannot be physically verified");
    reply[12] = 0;
    reply[1] = 3;
    snapshot = decode_gripper_reply(reply);
    check(snapshot && snapshot->terminal() && !snapshot->fully_measured(),
          "cancelled is a separate terminal state");
    reply[10] = 3;
    check(!decode_gripper_reply(reply), "impossible per-step evidence count rejected");
    reply[10] = 2;
    reply[6] = 5;
    check(!decode_gripper_reply(reply), "unknown completion evidence rejected");
    reply[6] = 2;
    check(!decode_gripper_reply(std::span(reply).first(24)), "truncated job status rejected");
    const std::array<std::uint8_t, 1> nack{5};
    const auto rejected = decode_gripper_reply(nack);
    check(rejected && rejected->error == 5 && !rejected->snapshot_known,
          "one-byte envelope rejection does not manufacture a job snapshot");
    const std::array<std::uint8_t, 1> short_success{0};
    check(!decode_gripper_reply(short_success), "short success cannot confirm a job");
    reply = {};
    reply[1] = 2;
    check(!decode_gripper_reply(reply), "empty completed snapshot cannot confirm success");
  }
  {
    Authority authority{};
    check(authority.decide_arm(0, false) == ArmDecision::acquire &&
              authority.decide_arm(9, false) == ArmDecision::reject,
          "only zero token acquires a new ROS session");
    check(authority.decide_arm(0, false, 2) == ArmDecision::reject,
          "multiple discovered publishers block acquiring authority");
    check(!authority.grant(0) && authority.grant(42) && !authority.grant(43),
          "ROS authority has one nonzero session token");
    check(authority.decide_arm(42, true) == ArmDecision::idempotent &&
              authority.decide_arm(0, true) == ArmDecision::reject &&
              authority.decide_arm(43, true) == ArmDecision::reject,
          "armed session rejects competing acquire/other token");
    check(authority.allows(42, true) && !authority.allows(43, true) && !authority.allows(42, false),
          "actuator/output token and armed gate");
    std::array<std::uint8_t, 24> first{};
    first[3] = 5;
    auto second = first;
    second[3] = 6;
    check(authority.observe_publisher(first) && authority.observe_publisher(first),
          "first publisher is latched and repeat messages accepted");
    check(!authority.observe_publisher(second) && authority.token() == 0 &&
              !authority.allows(42, true),
          "competing publisher revokes service and motion authority");
    check(!authority.observe_publisher(second),
          "different publisher cannot acquire after conflict without explicit arm");
    check(authority.grant(77) && authority.observe_publisher(second),
          "explicit new session can bind a different publisher");
    authority.reset();
    check(!authority.allows(77, true) && authority.decide_arm(77, false) == ArmDecision::reject,
          "disarm/reconnect invalidates previous token");
    check(authority.grant(88), "next session grant");
    std::array<std::uint8_t, 24> unavailable{};
    check(!authority.observe_publisher(unavailable) && authority.token() == 0,
          "unavailable publisher identity cannot receive motion authority");
    check(authority.grant(99) && !authority.observe_publisher(std::span<const std::uint8_t>{}),
          "empty publisher identity rejected");
  }

  {
    std::array<std::uint8_t, 25> payload{};
    payload[1] = 1;
    payload[18] = 75;
    payload[19] = 2;
    const auto word = [&payload](unsigned at, std::uint16_t value) {
      payload[at] = static_cast<std::uint8_t>(value);
      payload[at + 1] = static_cast<std::uint8_t>(value >> 8);
    };
    put_u32(std::span(payload).subspan(2, 4), 24000);
    word(6, 4000);
    word(8, 4100);
    word(14, 8100);
    word(16, 2000);
    word(20, 0xABCD);
    word(22, 0x00F0);
    const auto unknown_sign = decode_sensors(payload);
    check(unknown_sign && unknown_sign->battery_known &&
              std::abs(unknown_sign->voltage - 8.1F) < 0.0001F &&
              std::abs(unknown_sign->percentage - 0.75F) < 0.0001F,
          "battery milli-units and SOC fraction");
    check(std::isnan(unknown_sign->current) && unknown_sign->raw_current_ma == 2000,
          "unsigned current direction remains unknown");
    check(unknown_sign->levels == 0xABCD && unknown_sign->known_mask == 0xF0 &&
              std::isnan(unknown_sign->cell_voltage[2]),
          "GPIO known mask and unused cells remain unknown");
    const auto discharge = decode_sensors(payload, -1);
    check(discharge && std::abs(discharge->current + 2.0F) < 0.0001F, "reviewed discharge sign");
    payload[1] = 0;
    const auto absent = decode_sensors(payload, -1);
    check(absent && !absent->battery_known && std::isnan(absent->voltage) &&
              std::isnan(absent->current) && absent->known_mask == 0xF0,
          "no cached battery after invalidation; GPIO independent");
    payload[1] = 1;
    payload[24] = 9;
    const auto failed = decode_sensors(payload);
    check(failed && !failed->battery_known && std::isnan(failed->percentage),
          "BMS failure invalidates all battery fields");
    payload[24] = 0;
    payload[18] = 101;
    const auto malformed = decode_sensors(payload);
    check(malformed && !malformed->battery_known && malformed->battery_error == 11,
          "invalid SOC cannot become measurement");
    payload[18] = 75;
    payload[19] = 5;
    const auto too_many_cells = decode_sensors(payload);
    check(too_many_cells && !too_many_cells->battery_known && too_many_cells->battery_error == 11,
          "unsupported cell count remains unknown");
    payload[19] = 0;
    const auto zero_cells = decode_sensors(payload);
    check(zero_cells && !zero_cells->battery_known, "zero cell count remains unknown");
    payload[0] = 10;
    const auto unsupported = decode_sensors(payload);
    check(unsupported && unsupported->error == 10 && !unsupported->battery_known &&
              unsupported->known_mask == 0,
          "unsupported sensor query is a valid unknown result");
    const std::array<std::uint8_t, 1> nack{10};
    const auto short_nack = decode_sensors(nack);
    check(short_nack && short_nack->error == 10 && !short_nack->battery_known,
          "short unsupported NACK does not become link failure");
    check(!decode_sensors(std::span(payload).first(24)) && !decode_sensors(payload, 2),
          "sensor reply length and sign validation");
  }
  check(valid_actuator(12, 3, -100, 0) && !valid_actuator(12, 3, 0.5F, 0) &&
            !valid_actuator(12, 3, 10, 100),
        "profile relative stepper uses configured speed/ramp");
  check(valid_actuator(13, 3, 1, 0) && !valid_actuator(13, 3, 2, 0), "DC brake Boolean");
  check(valid_actuator(14, 7, 0.5F, 1000) && !valid_actuator(14, 7, 1.01F, 1000) &&
            !valid_actuator(14, 7, 0.5F, 1000.5F),
        "PWM duty and integer frequency bounds");
  check(valid_actuator(15, 7, 0, 0) && !valid_actuator(15, 8, 0, 0), "PWM disable route bounds");
  check(valid_actuator(11, 3, 1, 500) && !valid_actuator(11, 3, 2, 500) &&
            !valid_actuator(11, 3, 1, 0.5F),
        "stepper homing direction and integer step rate");
  check(valid_actuator(1, 1, 1.2F, 0), "AX radians");
  check(valid_actuator(2, 1, 3.0F, 0), "MX radians");
  check(valid_actuator(3, 15, 90, 0) && !valid_actuator(3, 16, 90, 0), "PCA channel limits");
  check(valid_actuator(4, 7, 90, 0) && !valid_actuator(4, 8, 90, 0), "PWM channel limits");
  check(valid_actuator(5, 3, -100, 1000) && !valid_actuator(5, 3, 1.5F, 1000) &&
            !valid_actuator(5, 3, 100, 0),
        "stepper integer steps and positive rate");
  check(valid_actuator(6, 0, -1, 0) && !valid_actuator(6, 0, 1.1F, 0), "signed DC duty");
  check(valid_actuator(7, 1, 1, 0) && valid_actuator(8, 1, 0, 0) && !valid_actuator(7, 1, 2, 0),
        "torque Boolean");
  check(valid_actuator(9, 59, 16777215, 0) && !valid_actuator(9, 59, 16777216, 0) &&
            !valid_actuator(9, 60, 0, 0),
        "packed RGB bounds");
  check(valid_actuator(10, 0, 0, 0) && !valid_actuator(10, 0, 1, 0),
        "relative odometry reset only");
  check(!valid_actuator(1, 1, std::numeric_limits<float>::quiet_NaN(), 0) &&
            !valid_actuator(0, 0, 0, 0),
        "nonfinite/unknown actuator rejected");
  {
    Pty p;
    Transport transport;
    check(transport.open(p.path.data(), 115200), "open transport");
    Transport competing;
    check(!competing.open(p.path.data(), 115200), "exclusive serial ownership");
    std::thread peer(serve, p.master, true, true);
    const auto reply = transport.transact(request(), 100ms);
    peer.join();
    check(reply && reply->sequence == 7 && reply->length == 84,
          "correlated fragmented ACK; wrong sequence/owner/type and CRC ignored");
  }
  {
    Pty p;
    Transport transport;
    check(transport.open(p.path.data(), 115200), "open timeout transport");
    std::thread peer(serve, p.master, false, false);
    const auto start = std::chrono::steady_clock::now();
    check(!transport.transact(request(), 40ms), "missing ACK");
    peer.join();
    check(std::chrono::steady_clock::now() - start < 200ms, "bounded timeout");
    check(!transport.connected(), "timeout closes port");
    Gate gate;
    gate.armed = true;
    gate.fault();
    check(!gate.armed && gate.faults == 1, "failure latches disarmed");
    check(transport.open(p.path.data(), 115200), "reconnect serial");
    check(!gate.armed, "reconnect does not rearm");
  }
  {
    Pty p;
    Transport transport;
    check(transport.open(p.path.data(), 115200), "open unplug transport");
    ::close(p.master);
    p.master = -1;
    check(!transport.transact(request(), 40ms), "unplug rejects transaction");
    check(!transport.connected(), "unplug closes serial");
  }
  {
    Pty p;
    Transport transport;
    check(!transport.open(p.path.data(), 12345), "unsupported baud rejected");
  }
  std::cout << "serial PTY integration: PASS\n";
}
