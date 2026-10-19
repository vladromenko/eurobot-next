#include "eurobot/core.hpp"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
using namespace eurobot;
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
bool near(float a, float b, float tolerance = 1e-5F) { return std::fabs(a - b) < tolerance; }
struct Outputs {
  std::uint32_t pumps{}, valves{}, pump_writes{}, wheel_writes{};
  std::array<std::int32_t, 3> wheels{};
  bool pump_fail{}, wheel_fail{};
  static Error pump(void *context, std::uint32_t p, std::uint32_t v) {
    auto &self = *static_cast<Outputs *>(context);
    ++self.pump_writes;
    if (self.pump_fail) {
      return Error::output_failure;
    }
    self.pumps = p;
    self.valves = v;
    return Error::ok;
  }
  static Error wheel(void *context, const std::array<std::int32_t, 3> &w) {
    auto &self = *static_cast<Outputs *>(context);
    ++self.wheel_writes;
    if (self.wheel_fail) {
      return Error::output_failure;
    }
    self.wheels = w;
    return Error::ok;
  }
};
ChassisConfig test_chassis_config() {
  ChassisConfig config{};
  config.max_linear_mps = 1;
  config.max_angular_radps = 2;
  config.max_accel_mps2 = 2;
  config.max_angular_accel_radps2 = 4;
  config.max_wheel_tpm = 10000000;
  config.max_encoder_delta = 100000;
  return config;
}
struct Fixture {
  Outputs outputs{};
  PumpBank pumps{{8, 8, 250, 0}, {&outputs, Outputs::pump}};
  Chassis chassis{test_chassis_config(), {&outputs, Outputs::wheel}};
  Controller controller{pumps, chassis};
  Fixture() { CHECK(controller.initialize(0) == Error::ok); }
  void arm(std::uint32_t owner = 42, std::uint32_t seq = 1, std::uint32_t now = 0,
           std::uint32_t lease = 1000) {
    CHECK(controller.claim(owner, seq, lease, now) == Error::ok);
    CHECK(controller.rearm(owner, seq + 1, now) == Error::ok);
  }
};
void pump_masks() {
  for (const auto count : {8U, 16U, 32U}) {
    Outputs io{};
    PumpBank bank{{static_cast<std::uint8_t>(count), static_cast<std::uint8_t>(count), 250, 0},
                  {&io, Outputs::pump}};
    CHECK(bank.initialize(0) == Error::ok);
    const auto mask = channel_mask(static_cast<std::uint8_t>(count));
    const auto before = io.pump_writes;
    CHECK(bank.set(mask, mask, mask, mask, 1) == Error::ok);
    CHECK(io.pump_writes == before + 1);
    CHECK(io.pumps == mask);
    CHECK(io.valves == mask);
    CHECK(bank.state().commanded == mask);
    CHECK(bank.state().measured_known == 0);
    CHECK(bank.set(0x55U, 0, 0xaaU, 0, 2) == Error::ok);
    CHECK(io.pumps == (mask & ~0x55U));
    CHECK(io.valves == (mask & ~0xaaU));
    CHECK(bank.stop() == Error::ok);
    CHECK(io.pumps == 0);
    CHECK(io.valves == 0);
  }
  Outputs io{};
  PumpBank limited{{8, 2, 250, 0}, {&io, Outputs::pump}};
  CHECK(limited.initialize(0) == Error::ok);
  CHECK(limited.set(3, 3, 0, 0, 0) == Error::ok);
  const auto writes = io.pump_writes;
  CHECK(limited.set(4, 4, 0, 0, 1) == Error::invalid_argument);
  CHECK(limited.set(0x100U, 0, 0, 0, 1) == Error::invalid_argument);
  CHECK(limited.set(1, 2, 0, 0, 1) == Error::invalid_argument);
  CHECK(limited.set(0, 0, 0, 1, 1) == Error::invalid_argument);
  CHECK(limited.state().requested == 3);
  CHECK(io.pump_writes == writes);
  CHECK(limited.measure(1, 1) == Error::ok);
  CHECK(limited.state().measured_known == 1);
  CHECK(limited.measure(1, 2) == Error::invalid_argument);
  CHECK(limited.state().measured_on == 1);
  for (auto channels : {0U, 33U, 255U}) {
    PumpBank invalid{{static_cast<std::uint8_t>(channels), 0, 250, 0}, {&io, Outputs::pump}};
    CHECK(!invalid.valid());
    CHECK(invalid.initialize(0) == Error::invalid_config);
  }
}
void pump_timeout_and_failure() {
  Outputs io{};
  PumpBank bank{{8, 8, 250, 100}, {&io, Outputs::pump}};
  CHECK(bank.initialize(0) == Error::ok);
  CHECK(bank.set(1, 1, 0, 0, 0xfffffff0U) == Error::ok);
  CHECK(bank.tick(0x20U) == Error::ok);
  CHECK(bank.set(1, 1, 0, 0, 0x30U) == Error::ok);
  CHECK(bank.tick(0x54U) == Error::timeout);
  CHECK(io.pumps == 0); // Dwell survives renewal and millis wrap.
  CHECK(bank.set(0, 0, 1, 1, 100) == Error::ok);
  CHECK(bank.tick(349) == Error::ok);
  CHECK(bank.tick(350) == Error::timeout);
  CHECK(io.valves == 0);
  io.pump_fail = true;
  CHECK(bank.set(1, 1, 0, 0, 400) == Error::output_failure);
  CHECK(bank.state().requested == 1);
  CHECK(bank.state().commanded_known == 0);
  CHECK(bank.state().measured_known == 0);
  CHECK(bank.set(1, 1, 0, 0, 401) == Error::not_armed);
  CHECK(bank.stop() == Error::output_failure);
  CHECK(bank.state().commanded_known == 0);
  io.pump_fail = false;
  CHECK(bank.initialize(402) == Error::ok);
  CHECK(bank.state().commanded == 0);
  PumpBank independent{{8, 8, 250, 0}, {&io, Outputs::pump}};
  CHECK(independent.initialize(0) == Error::ok);
  CHECK(independent.set(3, 3, 1, 1, 0) == Error::ok);
  CHECK(independent.set(2, 2, 1, 1, 200) == Error::ok);
  CHECK(independent.tick(249) == Error::ok);
  CHECK(independent.tick(250) == Error::timeout); // Channel1 cannot renew unrelated channel0.
  CHECK(io.pumps == 0 && io.valves == 0);
  CHECK(independent.set(1, 1, 1, 1, 300) == Error::ok);
  CHECK(independent.set(1, 1, 0, 0, 500) == Error::ok);
  CHECK(independent.tick(550) == Error::timeout); // Pump refresh cannot renew its valve.
}
void lease_and_estop() {
  Fixture f;
  CHECK(!f.controller.state().armed);
  CHECK(f.outputs.pumps == 0);
  CHECK(f.controller.pumps(42, 1, 1, 1, 0, 0, 0) == Error::lease_expired);
  CHECK(f.controller.claim(0, 1, 1000, 0) == Error::invalid_argument);
  CHECK(f.controller.claim(42, 1, 0x80000000U, 0) == Error::invalid_argument);
  f.arm();
  CHECK(f.controller.claim(43, 10, 1000, 0) == Error::lease_conflict);
  CHECK(f.controller.pumps(43, 3, 1, 1, 0, 0, 0) == Error::lease_conflict);
  CHECK(f.controller.pumps(42, 3, 1, 1, 0, 0, 0) == Error::ok);
  CHECK(f.controller.pumps(42, 3, 1, 0, 0, 0, 1) == Error::stale_sequence);
  CHECK(f.outputs.pumps == 1);
  CHECK(f.controller.emergency_stop() == Error::ok);
  CHECK(f.controller.state().estop);
  CHECK(!f.controller.state().armed);
  CHECK(f.outputs.pumps == 0);
  CHECK(f.controller.pumps(42, 4, 1, 1, 0, 0, 2) == Error::estop);
  f.controller.physical_estop(true);
  CHECK(f.controller.rearm(42, 5, 3) == Error::estop);
  f.controller.physical_estop(false);
  CHECK(f.controller.rearm(42, 6, 4) == Error::ok);
  CHECK(f.controller.state().armed);
  CHECK(f.outputs.pumps == 0); // Rearm never restores old requests.
  CHECK(f.controller.release(42, 7, 5) == Error::ok);
  CHECK(!f.controller.state().armed);
  CHECK(f.controller.state().owner == 0);
  CHECK(f.controller.claim(42, 1, 1000, 6) == Error::stale_sequence);
  CHECK(f.controller.claim(43, 1, 1000, 6) == Error::ok);
}
void lease_wrap_and_loss() {
  Fixture f;
  f.arm(42, 0xfffffffeU, 0xfffffff0U, 100);
  CHECK(f.controller.pumps(42, 0, 1, 1, 0, 0, 0xfffffff1U) == Error::ok);
  CHECK(f.controller.heartbeat(42, 0x80000000U, 0xfffffff2U) == Error::stale_sequence);
  CHECK(f.controller.heartbeat(42, 1, 0x20U) == Error::ok);
  CHECK(f.controller.tick(0x40U) == Error::ok);
  CHECK(f.controller.tick(0x84U) == Error::lease_expired);
  CHECK(!f.controller.state().armed);
  CHECK(f.outputs.pumps == 0);
  CHECK(f.controller.state().owner == 0);
  CHECK(f.controller.claim(42, 0xfffffffeU, 100, 0x85U) == Error::stale_sequence);
  CHECK(f.controller.rearm(42, 2, 0x85U) == Error::lease_expired);
  CHECK(f.controller.claim(43, 1, 100, 0x85U) == Error::ok);
  CHECK(f.controller.pumps(43, 2, 1, 1, 0, 0, 0x85U) == Error::not_armed);
}
void controller_safety() {
  Fixture f;
  f.arm();
  CHECK(f.controller.pumps(42, 3, 1, 1, 0, 0, 0) == Error::ok);
  for (std::uint32_t now = 50; now <= 250; now += 50) {
    CHECK(f.controller.heartbeat(42, now, now) == Error::ok);
    const auto status = f.controller.tick(now);
    CHECK(status == (now == 250 ? Error::timeout : Error::ok));
  }
  CHECK(!f.controller.state().armed);
  CHECK(f.outputs.pumps == 0);
  CHECK(f.controller.rearm(42, 251, 251) == Error::ok);
  f.outputs.pump_fail = true;
  CHECK(f.controller.pumps(42, 252, 1, 1, 0, 0, 252) == Error::output_failure);
  CHECK(!f.controller.state().armed);
  CHECK(f.controller.state().fault);
  CHECK(f.pumps.state().commanded_known == 0);
  CHECK(f.pumps.state().measured_known == 0);
  CHECK(f.chassis.state().wheel_commanded == (std::array<std::int32_t, 3>{}));
  f.outputs.pump_fail = false;
  CHECK(f.controller.rearm(42, 253, 253) == Error::ok);
  CHECK(f.controller.velocity(42, 254, {0.1F, 0, 0}, 253) == Error::ok);
  f.outputs.wheel_fail = true;
  CHECK(f.controller.tick(260) == Error::output_failure);
  CHECK(!f.controller.state().armed);
  CHECK(!f.chassis.state().commanded_known);
  CHECK(f.outputs.pumps == 0);
}

void auxiliary_safety() {
  struct Auxiliary {
    unsigned stops{};
    bool fail{};
    static Error stop(void *context) {
      auto &state = *static_cast<Auxiliary *>(context);
      ++state.stops;
      return state.fail ? Error::output_failure : Error::ok;
    }
  } auxiliary;
  Outputs outputs{};
  PumpBank pumps{{8, 8, 250, 0}, {&outputs, Outputs::pump}};
  Chassis chassis{test_chassis_config(), {&outputs, Outputs::wheel}};
  Controller controller{pumps, chassis, {&auxiliary, Auxiliary::stop}};
  CHECK(controller.initialize(0) == Error::ok);
  CHECK(auxiliary.stops == 1);
  CHECK(controller.claim(42, 1, 1000, 0) == Error::ok);
  CHECK(controller.rearm(42, 2, 0) == Error::ok);
  CHECK(auxiliary.stops == 2);
  controller.physical_estop(true);
  CHECK(auxiliary.stops == 3);
  controller.physical_estop(true);
  CHECK(auxiliary.stops == 3); // Polling a maintained input does not flood buses.
  CHECK(controller.initialize(1) == Error::ok);
  CHECK(controller.state().estop);
  CHECK(controller.claim(43, 1, 1000, 1) == Error::ok);
  CHECK(controller.rearm(43, 2, 1) == Error::estop);
  controller.physical_estop(false);
  auxiliary.fail = true;
  CHECK(controller.rearm(43, 3, 2) == Error::output_failure);
  CHECK(!controller.state().armed);
  auxiliary.fail = false;
  CHECK(controller.rearm(43, 4, 3) == Error::ok);
  CHECK(controller.release(43, 5, 4) == Error::ok);
  CHECK(auxiliary.stops >= 7);
}
void kinematics() {
  for (const auto profile : {legacy_2025_geometry(), legacy_2026_geometry()}) {
    auto ideal = profile;
    ideal.odometry_scale = 1;
    ideal.odometry_x_scale = 1;
    ideal.odometry_y_scale = 1;
    ideal.angular_odometry_scale = 1;
    ideal.wheel_odometry_scale = {1, 1, 1};
    OmniKinematics k{ideal};
    CHECK(k.valid());
    for (const auto value :
         {Twist{0.2F, 0, 0}, Twist{0, 0.3F, 0}, Twist{0, 0, 1.2F}, Twist{0.1F, -0.2F, -0.3F}}) {
      const auto wheels = k.wheel_tpm(value);
      CHECK(wheels.has_value());
      const auto decoded = k.body_speed(*wheels);
      CHECK(decoded.has_value());
      CHECK(near(decoded->vx, value.vx));
      CHECK(near(decoded->vy, value.vy));
      CHECK(near(decoded->wz, value.wz));
    }
    CHECK(!k.wheel_tpm({std::numeric_limits<float>::quiet_NaN(), 0, 0}));
    CHECK(!k.wheel_tpm({std::numeric_limits<float>::max(), 0, 0}));
    const auto rotate = *k.wheel_tpm({0, 0, 1});
    CHECK((rotate[0] < 0) == (profile.layout == OmniLayout::legacy_2026));
    CHECK(rotate[0] == rotate[1] && rotate[1] == rotate[2]);
  }
  OmniKinematics calibrated{legacy_2026_geometry()};
  const auto actual = *calibrated.body_delta({1000, 1000, 1000});
  const auto config = legacy_2026_geometry();
  const auto factor = 0.988F * 2 * 3.14159265358979323846F * config.wheel_radius_m / (28 * 4096);
  CHECK(near(actual.vx, 1000 * factor * (1.003F - 0.997F) / (2 * 0.8660254F) * 0.988F));
  CHECK(near(actual.vy, 1000 * factor * (-1.003F + 2 * 0.984F - 0.997F) / 3));
  CHECK(!OmniKinematics{OmniConfig{}}.valid());
}
void chassis_limits_and_feedback() {
  Outputs io{};
  auto config = test_chassis_config();
  config.max_wheel_tpm = 2000000;
  Chassis chassis{config, {&io, Outputs::wheel}};
  CHECK(chassis.initialize(0) == Error::ok);
  CHECK(chassis.set({10, 10, 10}, 0) == Error::ok);
  CHECK(std::hypot(chassis.state().requested.vx, chassis.state().requested.vy) <= 1.000001F);
  for (std::uint32_t now = 10; now <= 200; now += 10) {
    const auto before = chassis.state().commanded;
    CHECK(chassis.tick(now) == Error::ok);
    const auto current = chassis.state().commanded;
    CHECK(std::hypot(current.vx - before.vx, current.vy - before.vy) <= 0.020001F);
    CHECK(std::fabs(current.wz - before.wz) <= 0.040001F);
    for (auto wheel : io.wheels) {
      CHECK(std::abs(wheel) <= config.max_wheel_tpm);
    }
  }
  CHECK(chassis.set({std::numeric_limits<float>::infinity(), 0, 0}, 200) ==
        Error::invalid_argument);
  CHECK(chassis.tick(250) == Error::timeout);
  CHECK(io.wheels == (std::array<std::int32_t, 3>{}));
  CHECK(chassis.initialize(300) == Error::ok);
  CHECK(chassis.tick(401) == Error::timeout);
  CHECK(chassis.set({}, 402) == Error::not_armed);
  CHECK(chassis.initialize(403) == Error::ok);
  CHECK(chassis.reset_pose() == Error::ok);
  CHECK(chassis.feedback({100, 200, -300}, {10, 20, -30}) == Error::ok);
  CHECK(chassis.state().pose_known);
  CHECK(chassis.state().measured_known == 7);
  CHECK(chassis.feedback({100, 200, -300}, {10, 20, -30}, 3) == Error::ok);
  CHECK(!chassis.state().pose_known);
  CHECK(chassis.state().measured_known == 3);
  CHECK(chassis.feedback({100, 200, -300}, {10, 20, -30}) == Error::ok);
  CHECK(!chassis.state().pose_known); // Lost odometry cannot silently become trustworthy again.
  CHECK(chassis.reset_pose({1, 2, 50}) == Error::ok);
  CHECK(std::fabs(chassis.state().pose.yaw) <= 3.1416F);
  CHECK(chassis.feedback({0, 0, 0}, {std::numeric_limits<std::int32_t>::min(), 0, 0}) ==
        Error::invalid_argument);
  CHECK(chassis.state().measured_known == 0);
  Chassis invalid{ChassisConfig{}, {&io, Outputs::wheel}};
  CHECK(!invalid.valid());
}
void pid() {
  Pid pid{{2, 1, 0, -1, 1, -10, 10}};
  CHECK(pid.valid());
  for (unsigned i = 0; i < 1000; ++i) {
    CHECK(*pid.update(100, 0, 0.01F) == 1);
  }
  CHECK(pid.integral() == 0);
  CHECK(*pid.update(0, 0, 0.01F) == 0);
  CHECK(*pid.update(0.1F, 0, 1) > 0);
  CHECK(pid.integral() > 0);
  CHECK(*pid.update(-100, 0, 0.01F) == -1);
  CHECK(pid.integral() > 0); // No windup in opposite saturation.
  pid.reset();
  CHECK(pid.integral() == 0);
  CHECK(!pid.update(1, 0, 0));
  CHECK(!pid.update(1, 0, -1));
  CHECK(!pid.update(std::numeric_limits<float>::quiet_NaN(), 0, 1));
  CHECK(!pid.update(std::numeric_limits<float>::max(), -std::numeric_limits<float>::max(), 1));
  CHECK(!pid.update(std::numeric_limits<float>::max(), 0, std::numeric_limits<float>::max()));
  Pid derivative{{0, 0, 1, -100, 100, 0, 0}};
  CHECK(*derivative.update(10, 2, 1) == 0);
  CHECK(*derivative.update(20, 3, 1) == -1); // Derivative on measurement avoids setpoint kick.
  CHECK(!Pid{PidConfig{}}.valid());
}
std::optional<wire::Frame> parse(wire::FrameParser &parser, std::span<const std::uint8_t> bytes,
                                 std::uint32_t now = 0) {
  std::optional<wire::Frame> result;
  for (const auto byte : bytes) {
    auto frame = parser.push(byte, now);
    if (frame) {
      result = frame;
    }
  }
  return result;
}
void protocol() {
  const std::array<std::uint8_t, 9> golden{'1', '2', '3', '4', '5', '6', '7', '8', '9'};
  CHECK(wire::crc16(golden) == 0x29b1U);
  wire::Frame input{};
  input.type = 5;
  input.owner = 0x12345678;
  input.sequence = 0xfffffffe;
  input.length = 128;
  for (unsigned i = 0; i < 128; ++i) {
    input.payload[i] = static_cast<std::uint8_t>(i);
  }
  input.payload[11] = 0xa5;
  input.payload[12] = 0x5a;
  std::array<std::uint8_t, 144> bytes{};
  CHECK(wire::encode_frame(input, bytes) == 144);
  CHECK(bytes[8] == 0x78 && bytes[11] == 0x12);
  wire::FrameParser parser;
  CHECK(!parser.push(0xa5, 0));
  CHECK(!parser.push(0xa5, 1)); // Repeated first magic resynchronizes.
  const auto output = parse(parser, std::span{bytes}.subspan(1), 1);
  CHECK(output.has_value());
  CHECK(output->owner == input.owner);
  CHECK(output->sequence == input.sequence);
  CHECK(output->length == 128);
  CHECK(output->payload == input.payload);
  auto corrupt = bytes;
  corrupt[140] ^= 1;
  CHECK(!parse(parser, corrupt, 2));
  CHECK(parser.rejected() == 1);
  CHECK(parse(parser, bytes, 3).has_value());
  CHECK(!parse(parser, std::span{bytes}.first(8), 4));
  CHECK(parse(parser, bytes, 104).has_value());
  CHECK(parser.rejected() == 2);
  auto oversized = bytes;
  oversized[12] = 129;
  CHECK(!parse(parser, std::span{oversized}.first(14), 105));
  CHECK(parse(parser, bytes, 106).has_value());
  std::array<std::uint8_t, 8> short_buffer{};
  CHECK(wire::encode_frame(input, short_buffer) == 0);
  input.length = 129;
  CHECK(wire::encode_frame(input, bytes) == 0);
  // Long random noise cannot overrun bounded storage; a clean frame is recovered after timeout.
  std::uint32_t random = 1234;
  for (unsigned i = 0; i < 100000; ++i) {
    random ^= random << 13;
    random ^= random >> 17;
    random ^= random << 5;
    const auto ignored = parser.push(static_cast<std::uint8_t>(random), i / 1000 + 200);
    (void)ignored;
  }
  CHECK(parse(parser, bytes, 1000).has_value());
}
void dispatcher() {
  Fixture f;
  wire::Dispatcher dispatcher{f.controller};
  wire::Frame request{};
  request.type = 1;
  request.owner = 42;
  request.sequence = 1;
  request.length = 4;
  wire::write_u32(request.payload.data(), 1000);
  auto response = dispatcher.dispatch(request, 0);
  CHECK(response.type == 0x81);
  CHECK(response.length == 84);
  CHECK(response.payload[0] == 0);
  request.type = 2;
  request.length = 0;
  ++request.sequence;
  response = dispatcher.dispatch(request, 0);
  CHECK(response.payload[0] == 0);
  CHECK((response.payload[1] & 1) != 0);
  request.type = 4;
  request.length = 16;
  ++request.sequence;
  request.payload = {};
  wire::write_u32(request.payload.data(), 255);
  wire::write_u32(request.payload.data() + 4, 0xaa);
  response = dispatcher.dispatch(request, 1);
  CHECK(response.payload[0] == 0);
  CHECK(wire::read_u32(response.payload.data() + 8) == 0xaa);
  CHECK(wire::read_u32(response.payload.data() + 12) == 0xaa);
  CHECK(wire::read_u32(response.payload.data() + 16) == 255);
  CHECK(wire::read_u32(response.payload.data() + 24) == 0); // Output acceptance is not measurement.
  request.length = 15;
  ++request.sequence;
  response = dispatcher.dispatch(request, 2);
  CHECK(response.payload[0] == static_cast<unsigned>(Error::malformed));
  request.type = 5;
  request.length = 12;
  ++request.sequence;
  wire::write_f32(request.payload.data(), std::numeric_limits<float>::quiet_NaN());
  response = dispatcher.dispatch(request, 3);
  CHECK(response.payload[0] == static_cast<unsigned>(Error::invalid_argument));
  request.type = 0x20;
  request.length = 0;
  ++request.sequence;
  response = dispatcher.dispatch(request, 4);
  CHECK(response.payload[0] == static_cast<unsigned>(Error::unsupported));
  request.type = 6;
  request.owner = 999;
  request.sequence = 0;
  response = dispatcher.dispatch(request, 5);
  CHECK(response.payload[0] == 0);
  CHECK(f.outputs.pumps == 0);
  CHECK((response.payload[1] & 2) != 0);
  CHECK((response.payload[1] & 1) == 0);
  request.type = 8;
  response = dispatcher.dispatch(request, 1000);
  CHECK(response.payload[0] == 0);
  CHECK(wire::read_u32(response.payload.data() + 4) == 0);
  CHECK(wire::read_u32(response.payload.data() + 80) == 1000);
}
} // namespace
int main() {
  pump_masks();
  pump_timeout_and_failure();
  lease_and_estop();
  lease_wrap_and_loss();
  controller_safety();
  auxiliary_safety();
  kinematics();
  chassis_limits_and_feedback();
  pid();
  protocol();
  dispatcher();
  std::printf("core: %u checks passed\n", checks);
}
