#include "eurobot/core.hpp"
#include <gtest/gtest.h>
using namespace eurobot;
namespace {
Error outputs(void *, std::uint32_t, std::uint32_t) { return Error::ok; }
Error wheels(void *, const std::array<std::int32_t, 3> &) { return Error::ok; }
ChassisConfig limits() {
  ChassisConfig c{};
  c.max_linear_mps = c.max_angular_radps = 1;
  c.max_accel_mps2 = c.max_angular_accel_radps2 = 1;
  c.max_wheel_tpm = 10000000;
  c.max_encoder_delta = 1000000;
  return c;
}
struct Jobs {
  unsigned calls{}, commands{};
  bool busy{}, overflow{};
  static Error request(void *context, std::uint8_t, std::uint32_t owner,
                       std::span<const std::uint8_t>, std::span<std::uint8_t> output,
                       std::size_t &size, std::uint32_t now) {
    auto &self = *static_cast<Jobs *>(context);
    ++self.calls;
    size = self.overflow ? wire::max_payload : 24;
    output[0] = 1;
    wire::write_u32(output.data() + 16, owner);
    wire::write_u32(output.data() + 20, now);
    return Error::ok;
  }
  static Error gate(void *context, std::uint8_t, std::span<const std::uint8_t>) {
    return static_cast<Jobs *>(context)->busy ? Error::lease_conflict : Error::ok;
  }
  static Error actuator(void *context, std::span<const std::uint8_t>, std::uint32_t) {
    ++static_cast<Jobs *>(context)->commands;
    return Error::ok;
  }
};
struct Fixture : testing::Test {
  PumpBank pumps{{8, 8, 250, 0}, {nullptr, outputs}};
  Chassis chassis{limits(), {nullptr, wheels}};
  Controller controller{pumps, chassis};
  Jobs jobs{};
  wire::Dispatcher dispatch{controller,
                            {&jobs, Jobs::actuator, nullptr, Jobs::request, Jobs::gate}};
  void SetUp() override {
    ASSERT_EQ(controller.initialize(0), Error::ok);
    ASSERT_EQ(controller.claim(42, 1, 500, 0), Error::ok);
  }
  wire::Frame frame(std::uint8_t type, std::uint32_t seq) {
    wire::Frame f{};
    f.type = type;
    f.owner = 42;
    f.sequence = seq;
    return f;
  }
};
TEST_F(Fixture, SubmitRequiresArmWhileQueryStillRequiresOwnerAndFreshSequence) {
  auto f = frame(0x21, 2);
  auto r = dispatch.dispatch(f, 1);
  ASSERT_EQ(r.length, 1);
  EXPECT_EQ(r.payload[0], static_cast<unsigned>(Error::not_armed));
  EXPECT_EQ(jobs.calls, 0U);
  f = frame(0x22, 3);
  f.owner = 43;
  r = dispatch.dispatch(f, 2);
  EXPECT_EQ(r.payload[0], static_cast<unsigned>(Error::lease_conflict));
  EXPECT_EQ(jobs.calls, 0U);
  f.owner = 42;
  r = dispatch.dispatch(f, 2);
  ASSERT_EQ(r.length, 25);
  EXPECT_EQ(r.type, 0xa2);
  EXPECT_EQ(wire::read_u32(r.payload.data() + 17), 42U);
  r = dispatch.dispatch(f, 3);
  EXPECT_EQ(r.payload[0], static_cast<unsigned>(Error::stale_sequence));
  EXPECT_EQ(jobs.calls, 1U);
}
TEST_F(Fixture, BusyOperationRejectsOverlappingControlButPermitsHeartbeatAndEmergencyStop) {
  ASSERT_EQ(controller.rearm(42, 2, 0), Error::ok);
  jobs.busy = true;
  auto f = frame(0x20, 3);
  f.length = 10;
  auto r = dispatch.dispatch(f, 1);
  EXPECT_EQ(r.payload[0], static_cast<unsigned>(Error::lease_conflict));
  EXPECT_EQ(jobs.commands, 0U);
  f = frame(4, 4);
  f.length = 16;
  wire::write_u32(f.payload.data(), 1);
  wire::write_u32(f.payload.data() + 4, 1);
  r = dispatch.dispatch(f, 2);
  EXPECT_EQ(r.payload[0], static_cast<unsigned>(Error::lease_conflict));
  EXPECT_EQ(pumps.state().commanded, 0U);
  f = frame(3, 5);
  EXPECT_EQ(dispatch.dispatch(f, 3).payload[0], 0);
  f = frame(6, 6);
  EXPECT_EQ(dispatch.dispatch(f, 4).payload[0], 0);
  EXPECT_TRUE(controller.state().estop);
  EXPECT_FALSE(controller.state().armed);
}
TEST_F(Fixture, ExtensionCannotOverflowFrameAndExpiredLeaseNeverInvokesIt) {
  jobs.overflow = true;
  auto f = frame(0x22, 2);
  auto r = dispatch.dispatch(f, 1);
  EXPECT_EQ(r.length, 1);
  EXPECT_EQ(r.payload[0], static_cast<unsigned>(Error::malformed));
  f = frame(0x22, 3);
  r = dispatch.dispatch(f, 501);
  EXPECT_NE(r.payload[0], 0);
  EXPECT_EQ(jobs.calls, 1U);
}
} // namespace
