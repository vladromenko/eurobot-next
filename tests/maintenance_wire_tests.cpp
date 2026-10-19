#include "eurobot/core.hpp"
#include "eurobot/maintenance.hpp"
#include <algorithm>
#include <array>
#include <gtest/gtest.h>
#include <limits>

namespace {
using eurobot::Chassis;
using eurobot::ChassisConfig;
using eurobot::Controller;
using eurobot::Error;
using eurobot::PumpBank;
using eurobot::maintenance::Operation;
namespace wire = eurobot::wire;

Error write_outputs(void *, std::uint32_t, std::uint32_t) { return Error::ok; }
Error write_wheels(void *, const std::array<std::int32_t, 3> &) { return Error::ok; }
ChassisConfig chassis_limits() {
  ChassisConfig config{};
  config.max_linear_mps = config.max_angular_radps = 1;
  config.max_accel_mps2 = config.max_angular_accel_radps2 = 1;
  config.max_wheel_tpm = 10000000;
  config.max_encoder_delta = 1000000;
  return config;
}
struct MaintenanceBackend {
  unsigned calls{}, guards{};
  Error result{Error::ok}, guard_result{Error::ok};
  std::size_t body_size{};
  std::uint32_t seen_owner{}, seen_time{};
  std::uint8_t seen_type{}, guarded_operation{};
  std::array<std::uint8_t, 22> seen_payload{};
  static Error request(void *context, std::uint8_t type, std::uint32_t owner,
                       std::span<const std::uint8_t> input, std::span<std::uint8_t> output,
                       std::size_t &size, std::uint32_t now) {
    auto &self = *static_cast<MaintenanceBackend *>(context);
    ++self.calls;
    self.seen_owner = owner;
    self.seen_time = now;
    self.seen_type = type;
    if (input.size() != self.seen_payload.size()) {
      return Error::malformed;
    }
    std::copy(input.begin(), input.end(), self.seen_payload.begin());
    std::fill_n(output.begin(), std::min(self.body_size, output.size()), 0x5a);
    size = self.body_size;
    return self.result;
  }
  static Error guard(void *context, std::uint8_t type, std::span<const std::uint8_t> input) {
    auto &self = *static_cast<MaintenanceBackend *>(context);
    ++self.guards;
    self.seen_type = type;
    self.guarded_operation = input.empty() ? 255 : input[0];
    return self.guard_result;
  }
};
class MaintenanceWireTest : public testing::Test {
protected:
  PumpBank pumps{{8, 8, 250, 0}, {nullptr, write_outputs}};
  Chassis chassis{chassis_limits(), {nullptr, write_wheels}};
  Controller controller{pumps, chassis};
  MaintenanceBackend backend{};
  wire::Dispatcher dispatcher{
      controller,
      {&backend, nullptr, nullptr, MaintenanceBackend::request, MaintenanceBackend::guard}};
  void SetUp() override { ASSERT_EQ(controller.initialize(0), Error::ok); }
  static wire::Frame request(Operation operation, std::uint32_t sequence = 2,
                             std::uint32_t owner = 42) {
    wire::Frame frame{};
    frame.type = 0x24;
    frame.owner = owner;
    frame.sequence = sequence;
    frame.length = 22;
    frame.payload[0] = static_cast<std::uint8_t>(operation);
    frame.payload[1] = 3;
    wire::write_u32(frame.payload.data() + 2, 0x12345678);
    wire::write_u32(frame.payload.data() + 6, 0xabcdef01);
    wire::write_f32(frame.payload.data() + 10, 1.25F);
    wire::write_f32(frame.payload.data() + 14, -2.5F);
    wire::write_f32(frame.payload.data() + 18, 0.75F);
    return frame;
  }
  static Error error(const wire::Frame &response) {
    return static_cast<Error>(response.payload[0]);
  }
};

TEST_F(MaintenanceWireTest, ReadOnlyQueriesBypassOwnerAndGuardWithoutConsumingSequence) {
  backend.body_size = 16;
  backend.guard_result = Error::lease_conflict;
  auto frame = request(Operation::board_info, UINT32_MAX, 0);
  auto response = dispatcher.dispatch(frame, 1);
  EXPECT_EQ(error(response), Error::ok);
  EXPECT_EQ(response.type, 0xa4);
  EXPECT_EQ(response.owner, 0U);
  EXPECT_EQ(response.sequence, UINT32_MAX);
  EXPECT_EQ(response.length, 17);
  EXPECT_EQ(backend.guards, 0U);
  ASSERT_EQ(controller.claim(42, 1, 500, 2), Error::ok);
  backend.body_size = 13;
  response = dispatcher.dispatch(request(Operation::wheel_path, UINT32_MAX, 99), 3);
  EXPECT_EQ(error(response), Error::ok);
  EXPECT_EQ(backend.seen_owner, 99U);
  EXPECT_EQ(backend.guards, 0U);
  backend.body_size = 0;
  backend.guard_result = Error::ok;
  response = dispatcher.dispatch(request(Operation::clear_reset, 2), 4);
  EXPECT_EQ(error(response), Error::ok);
  EXPECT_EQ(backend.guards, 1U);
  EXPECT_EQ(backend.calls, 3U);
  EXPECT_EQ(backend.seen_time, 4U);
  const auto expected = request(Operation::clear_reset, 2);
  EXPECT_TRUE(std::equal(backend.seen_payload.begin(), backend.seen_payload.end(),
                         expected.payload.begin()));
}

TEST_F(MaintenanceWireTest, DisarmedMutationRequiresClaimAndRejectsArmedController) {
  EXPECT_EQ(error(dispatcher.dispatch(request(Operation::clear_reset), 1)), Error::lease_expired);
  ASSERT_EQ(controller.claim(42, 1, 500, 2), Error::ok);
  EXPECT_EQ(error(dispatcher.dispatch(request(Operation::clear_reset, 2), 3)), Error::ok);
  ASSERT_EQ(controller.rearm(42, 3, 4), Error::ok);
  EXPECT_EQ(error(dispatcher.dispatch(request(Operation::clear_reset, 4), 5)),
            Error::lease_conflict);
  EXPECT_EQ(backend.calls, 1U);
  EXPECT_TRUE(controller.state().armed);
  EXPECT_TRUE(controller.lease_active(5));
}

TEST_F(MaintenanceWireTest, ArmedAndOwnedAccessUseOneLeaseWithFreshSequence) {
  ASSERT_EQ(controller.claim(42, 1, 500, 0), Error::ok);
  EXPECT_EQ(error(dispatcher.dispatch(request(Operation::wheel_holding, 2), 1)), Error::not_armed);
  ASSERT_EQ(controller.rearm(42, 3, 2), Error::ok);
  EXPECT_EQ(error(dispatcher.dispatch(request(Operation::wheel_holding, 4), 3)), Error::ok);
  EXPECT_EQ(error(dispatcher.dispatch(request(Operation::communication, 5), 4)), Error::ok);
  ASSERT_EQ(controller.emergency_stop(), Error::ok);
  EXPECT_FALSE(controller.state().armed);
  EXPECT_EQ(error(dispatcher.dispatch(request(Operation::match_disarm, 6), 5)), Error::ok);
  EXPECT_EQ(error(dispatcher.dispatch(request(Operation::pca_allpulse, 7), 6)), Error::estop);
  EXPECT_EQ(backend.calls, 3U);
}

TEST_F(MaintenanceWireTest, MutationReplayAndWrongOwnerNeverReachBackend) {
  ASSERT_EQ(controller.claim(42, 1, 500, 0), Error::ok);
  auto frame = request(Operation::clear_reset, 2, 77);
  EXPECT_EQ(error(dispatcher.dispatch(frame, 1)), Error::lease_conflict);
  frame.owner = 42;
  EXPECT_EQ(error(dispatcher.dispatch(frame, 2)), Error::ok);
  EXPECT_EQ(error(dispatcher.dispatch(frame, 3)), Error::stale_sequence);
  EXPECT_EQ(backend.calls, 1U);
  EXPECT_EQ(backend.guards, 1U);
}

TEST_F(MaintenanceWireTest, ExactPayloadLengthAndKnownOperationAreRequiredBeforeAuthority) {
  ASSERT_EQ(controller.claim(42, 1, 500, 0), Error::ok);
  for (const auto length : {0U, 21U, 23U, 129U}) {
    auto frame = request(Operation::clear_reset, 2);
    frame.length = static_cast<std::uint16_t>(length);
    EXPECT_EQ(error(dispatcher.dispatch(frame, 1)), Error::malformed);
  }
  auto frame = request(Operation::clear_reset, 2);
  frame.payload[0] = 255;
  EXPECT_EQ(error(dispatcher.dispatch(frame, 2)), Error::unsupported);
  frame.type = 0x25;
  EXPECT_EQ(error(dispatcher.dispatch(frame, 3)), Error::unsupported);
  EXPECT_EQ(backend.calls, 0U);
  EXPECT_EQ(backend.guards, 0U);
  EXPECT_EQ(error(dispatcher.dispatch(request(Operation::clear_reset, 2), 4)), Error::ok);
}

TEST_F(MaintenanceWireTest, ResourceGuardRejectsMutationWithoutLosingLeaseAndConsumesItsSequence) {
  ASSERT_EQ(controller.claim(42, 1, 500, 0), Error::ok);
  backend.guard_result = Error::lease_conflict;
  auto frame = request(Operation::clear_reset, 2);
  auto response = dispatcher.dispatch(frame, 1);
  EXPECT_EQ(error(response), Error::lease_conflict);
  EXPECT_EQ(response.length, 1);
  EXPECT_EQ(backend.calls, 0U);
  EXPECT_EQ(backend.guarded_operation, static_cast<std::uint8_t>(Operation::clear_reset));
  EXPECT_EQ(error(dispatcher.dispatch(frame, 2)), Error::stale_sequence);
  EXPECT_EQ(backend.guards, 1U);
  EXPECT_EQ(controller.heartbeat(42, 3, 3), Error::ok);
  EXPECT_TRUE(controller.lease_active(3));
}

TEST_F(MaintenanceWireTest, ReplySizeIsBoundedAndExtensionFailureIsNotSuccess) {
  backend.body_size = wire::max_payload - 1;
  auto response = dispatcher.dispatch(request(Operation::diagnostic_text, 0, 0), 1);
  EXPECT_EQ(error(response), Error::ok);
  EXPECT_EQ(response.length, wire::max_payload);
  backend.body_size = wire::max_payload;
  response = dispatcher.dispatch(request(Operation::board_info, 0, 0), 2);
  EXPECT_EQ(error(response), Error::malformed);
  EXPECT_EQ(response.length, 1);
  backend.body_size = std::numeric_limits<std::size_t>::max();
  response = dispatcher.dispatch(request(Operation::board_info, 0, 0), 3);
  EXPECT_EQ(error(response), Error::malformed);
  EXPECT_EQ(response.length, 1);
  backend.body_size = 0;
  backend.result = Error::timeout;
  response = dispatcher.dispatch(request(Operation::wheel_ticks, 0, 0), 4);
  EXPECT_EQ(error(response), Error::timeout);
  EXPECT_EQ(response.length, 1);
}

TEST_F(MaintenanceWireTest, MissingExtensionAndExpiredMutationAreRejected) {
  wire::Dispatcher missing{controller};
  EXPECT_EQ(error(missing.dispatch(request(Operation::board_info, 0, 0), 1)), Error::unsupported);
  ASSERT_EQ(controller.claim(42, 1, 500, 2), Error::ok);
  EXPECT_EQ(error(dispatcher.dispatch(request(Operation::clear_reset, 2), 503)),
            Error::lease_expired);
  EXPECT_EQ(backend.calls, 0U);
  EXPECT_FALSE(controller.lease_active(503));
  // An expired controller remains diagnosable and this query cannot renew its old lease.
  EXPECT_EQ(error(dispatcher.dispatch(request(Operation::board_info, 0, 0), 504)), Error::ok);
  EXPECT_EQ(backend.calls, 1U);
  EXPECT_FALSE(controller.lease_active(504));
}
} // namespace

namespace {
TEST_F(MaintenanceWireTest, FailedMutationStopsAllButFailedReadAndValidationDoNot) {
  ASSERT_EQ(controller.claim(42, 1, 500, 0), Error::ok);
  ASSERT_EQ(controller.rearm(42, 2, 0), Error::ok);
  ASSERT_EQ(controller.pumps(42, 3, 1, 1, 0, 0, 0), Error::ok);
  backend.result = Error::timeout;
  EXPECT_EQ(error(dispatcher.dispatch(request(Operation::wheel_ticks, 0, 0), 1)), Error::timeout);
  EXPECT_TRUE(controller.state().armed);
  EXPECT_EQ(pumps.state().commanded, 1U);
  backend.result = Error::invalid_argument;
  EXPECT_EQ(error(dispatcher.dispatch(request(Operation::pca_allpulse, 4), 2)),
            Error::invalid_argument);
  EXPECT_TRUE(controller.state().armed);
  backend.result = Error::output_failure;
  EXPECT_EQ(error(dispatcher.dispatch(request(Operation::pca_allpulse, 5), 3)),
            Error::output_failure);
  EXPECT_FALSE(controller.state().armed);
  EXPECT_TRUE(controller.state().fault);
  EXPECT_EQ(pumps.state().commanded, 0U);
}
} // namespace
