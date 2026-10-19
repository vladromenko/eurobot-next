#pragma once
#include <array>
#include <cstdint>

// Main-board connector poster supplied by the owner, checked against final2026
// board_*_defines.h. Connector numbers are one-based; software indices are zero-based.
namespace eurobot::board::connectors {
enum class Port : std::uint8_t { a, b, c, d, e };
struct Pin {
  Port port;
  std::uint8_t bit;
  constexpr bool operator==(const Pin &) const = default;
};
inline constexpr std::array<Pin, 28> gpio{
    {{Port::d, 12}, {Port::d, 13}, {Port::d, 15}, {Port::d, 14}, {Port::a, 8}, {Port::c, 11},
     {Port::d, 1},  {Port::d, 0},  {Port::b, 9},  {Port::b, 8},  {Port::d, 4}, {Port::d, 3},
     {Port::d, 7},  {Port::b, 3},  {Port::b, 5},  {Port::b, 4},  {Port::b, 6}, {Port::b, 7},
     {Port::e, 1},  {Port::e, 0},  {Port::e, 2},  {Port::e, 3},  {Port::e, 5}, {Port::e, 4},
     {Port::e, 6},  {Port::c, 13}, {Port::c, 15}, {Port::c, 14}}};
// GPIO9/10 are labelled Input Only in the supplied poster.
inline constexpr std::array<std::uint8_t, 13> input_gpio_numbers{27, 25, 23, 21, 28, 26, 24,
                                                                 22, 20, 18, 16, 14, 12};
inline constexpr auto inputs = [] {
  std::array<Pin, input_gpio_numbers.size()> result{};
  for (std::size_t i = 0; i < result.size(); ++i)
    result[i] = gpio[input_gpio_numbers[i] - 1];
  return result;
}();
struct Pwm {
  Pin pin;
  std::uint8_t timer;
  std::uint8_t channel; // Hardware channel 1..4, not CCR array index.
};
inline constexpr std::array<Pwm, 8> pwm{{{{Port::d, 15}, 4, 4},
                                         {{Port::d, 14}, 4, 3},
                                         {{Port::d, 13}, 4, 2},
                                         {{Port::d, 12}, 4, 1},
                                         {{Port::a, 0}, 5, 1},
                                         {{Port::a, 1}, 5, 2},
                                         {{Port::a, 2}, 5, 3},
                                         {{Port::a, 3}, 5, 4}}};
inline constexpr std::array<std::uint8_t, 20> power_register_bits{
    20, 21, 18, 19, 22, 23, 10, 11, 14, 15, 12, 13, 6, 7, 4, 5, 2, 3, 1, 0};
inline constexpr std::array<unsigned, 4> stepper_reset_bits{8, 9, 17, 16};
struct Stepper {
  Pin chip_select;
  Pin direction;
  Pin step;
};
// Output1 is at the bottom of the supplied drawing; Output4 is at the top.
inline constexpr std::array<Stepper, 4> steppers{{{{Port::e, 7}, {Port::e, 8}, {Port::e, 9}},
                                                  {{Port::d, 10}, {Port::e, 10}, {Port::e, 11}},
                                                  {{Port::d, 11}, {Port::e, 12}, {Port::e, 13}},
                                                  {{Port::c, 9}, {Port::e, 15}, {Port::e, 14}}}};
inline constexpr Pin host_tx{Port::a, 9}, host_rx{Port::a, 10};
inline constexpr Pin debug_tx{Port::c, 12}, debug_rx{Port::d, 2};
// Prescribed NEW installation. These routes do not identify the old robot's harness.
inline constexpr std::array<std::uint8_t, 8> installation_pumps{1, 3, 5, 7, 9, 11, 13, 15};
inline constexpr std::array<std::uint8_t, 8> installation_valves{2, 4, 6, 8, 10, 12, 14, 16};
} // namespace eurobot::board::connectors
