#include "eurobot/core.hpp"
#include <gtest/gtest.h>
using namespace eurobot;
namespace {
Error outputs(void *, std::uint32_t, std::uint32_t) { return Error::ok; }
Error wheels(void *, const std::array<std::int32_t, 3> &) { return Error::ok; }
struct Sensor {
  unsigned calls{};
  bool overflow{};
  Error failure{Error::ok};
};
Error read(void *c, std::span<std::uint8_t> out, std::size_t &count, std::uint32_t now) {
  auto &s = *static_cast<Sensor *>(c);
  ++s.calls;
  count = s.overflow ? 128 : 24;
  if (out.size() >= 24) {
    out[0] = 1;
    wire::write_u32(out.data() + 1, now);
    out[21] = 0xff;
    out[22] = 0x1f;
  }
  return s.failure;
}
} // namespace
TEST(SensorWire, UnconfiguredReadIsExplicitlyUnsupported) {
  PumpBank p{{}, {nullptr, outputs}};
  Chassis ch{{}, {nullptr, wheels}};
  Controller c{p, ch};
  wire::Dispatcher d{c};
  wire::Frame f{};
  f.type = 9;
  f.owner = 42;
  f.sequence = 99;
  auto r = d.dispatch(f, 0);
  EXPECT_EQ(r.type, 0x89);
  EXPECT_EQ(r.owner, 42U);
  EXPECT_EQ(r.sequence, 99U);
  ASSERT_EQ(r.length, 1);
  EXPECT_EQ(r.payload[0], static_cast<unsigned>(Error::unsupported));
}
TEST(SensorWire, ReadOnlyExtensionWorksWithoutArmingAndBoundsResponse) {
  PumpBank p{{}, {nullptr, outputs}};
  Chassis ch{{}, {nullptr, wheels}};
  Controller c{p, ch};
  Sensor s{};
  wire::Dispatcher d{c, {&s, nullptr, read}};
  wire::Frame f{};
  f.type = 9;
  auto r = d.dispatch(f, 123);
  ASSERT_EQ(r.length, 25);
  EXPECT_EQ(r.payload[0], 0);
  EXPECT_EQ(r.payload[1], 1);
  EXPECT_EQ(wire::read_u32(r.payload.data() + 2), 123U);
  EXPECT_EQ(s.calls, 1U);
  EXPECT_FALSE(c.state().armed);
  f.length = 1;
  r = d.dispatch(f, 124);
  EXPECT_EQ(r.payload[0], static_cast<unsigned>(Error::malformed));
  EXPECT_EQ(s.calls, 1U);
  f.length = 0;
  s.overflow = true;
  r = d.dispatch(f, 125);
  EXPECT_EQ(r.length, 1);
  EXPECT_EQ(r.payload[0], static_cast<unsigned>(Error::malformed));
}
TEST(SensorWire, MeasurementFailureNeverTurnsIntoKnownSuccess) {
  PumpBank p{{}, {nullptr, outputs}};
  Chassis ch{{}, {nullptr, wheels}};
  Controller c{p, ch};
  Sensor s{0, false, Error::timeout};
  wire::Dispatcher d{c, {&s, nullptr, read}};
  wire::Frame f{};
  f.type = 9;
  auto r = d.dispatch(f, 0);
  EXPECT_EQ(r.payload[0], static_cast<unsigned>(Error::timeout));
  EXPECT_FALSE(c.state().armed);
}
