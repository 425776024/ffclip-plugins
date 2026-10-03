#pragma once

#include <cstdint>
#include <cstring>

namespace videocut::base {

// Decode IEEE 754 binary16 bits, preserving signed zero and NaN payload bits.
[[nodiscard]] inline float Float16ToFloat32(const std::uint16_t bits) noexcept {
  const std::uint32_t sign = (static_cast<std::uint32_t>(bits) & 0x8000U)
                             << 16U;
  std::int32_t exponent = static_cast<std::int32_t>(
      (static_cast<std::uint32_t>(bits) >> 10U) & 0x1fU);
  std::uint32_t mantissa = static_cast<std::uint32_t>(bits) & 0x03ffU;
  std::uint32_t result = 0U;
  if (exponent == 0) {
    if (mantissa == 0U) {
      result = sign;
    } else {
      exponent = 1;
      while ((mantissa & 0x0400U) == 0U) {
        mantissa <<= 1U;
        --exponent;
      }
      mantissa &= 0x03ffU;
      result = sign | (static_cast<std::uint32_t>(exponent + 112) << 23U) |
               (mantissa << 13U);
    }
  } else if (exponent == 0x1f) {
    result = sign | 0x7f800000U | (mantissa << 13U);
  } else {
    result = sign | (static_cast<std::uint32_t>(exponent + 112) << 23U) |
             (mantissa << 13U);
  }
  float value = 0.0F;
  static_assert(sizeof(value) == sizeof(result));
  std::memcpy(&value, &result, sizeof(value));
  return value;
}

} // namespace videocut::base
