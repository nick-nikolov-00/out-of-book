#pragma once

#include <bit>
#include <cstdint>

/*
 * Finiteness tests that survive this project's build flags.
 *
 * Everything here compiles with -ffast-math, which implies -ffinite-math-only:
 * the compiler is then entitled to assume no NaN or infinity ever occurs, and
 * it folds std::isnan to false and std::isfinite to true outright. Guards
 * written with them are dead code, and a NaN that reaches a comparison reads as
 * if it were -inf rather than making the comparison false, so it wins arg-max
 * tests instead of dropping out of them.
 *
 * Inspecting the bit pattern cannot be folded away, so these keep working. The
 * better fix where it is available is not to produce the value at all — an
 * empty optional says "no estimate" without relying on any of this.
 */
namespace floatbits {

inline bool isFinite(double v) {
  constexpr uint64_t EXPONENT = 0x7FF0000000000000ULL;
  return (std::bit_cast<uint64_t>(v) & EXPONENT) != EXPONENT;
}

inline bool isNaN(double v) {
  constexpr uint64_t EXPONENT = 0x7FF0000000000000ULL;
  constexpr uint64_t MANTISSA = 0x000FFFFFFFFFFFFFULL;
  const uint64_t bits = std::bit_cast<uint64_t>(v);

  return (bits & EXPONENT) == EXPONENT && (bits & MANTISSA) != 0;
}

} // namespace floatbits
