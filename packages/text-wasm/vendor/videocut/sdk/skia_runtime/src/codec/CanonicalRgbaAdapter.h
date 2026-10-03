#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace videocut::skia_runtime::internal {

struct AssociatedRgba8View final {
  const std::uint8_t *bytes{nullptr};
  std::size_t byteSize{0U};
  std::uint32_t width{0U};
  std::uint32_t height{0U};
  std::size_t rowBytes{0U};
};

struct StraightRgba8WriteView final {
  std::uint8_t *bytes{nullptr};
  std::size_t byteSize{0U};
  std::size_t rowBytes{0U};
};

/// Converts associated RGBA8 into the canonical straight-alpha RGBA8 canvas
/// published by vector render lanes. The conversion is exact for alpha and
/// uses rounded integer unassociation for color; malformed extents fail
/// closed and cancellation never publishes a partially converted frame.
[[nodiscard]] bool ConvertAssociatedRgba8ToCanonicalStraight(
    const AssociatedRgba8View &source,
    const StraightRgba8WriteView &destination,
    const std::function<bool()> &cancel, std::string &error) noexcept;

} // namespace videocut::skia_runtime::internal
