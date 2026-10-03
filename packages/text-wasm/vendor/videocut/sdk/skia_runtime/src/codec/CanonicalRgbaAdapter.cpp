#include "codec/CanonicalRgbaAdapter.h"

#include <algorithm>
#include <limits>

namespace videocut::skia_runtime::internal {
namespace {

bool RequiredExtent(const std::uint32_t height, const std::size_t rowBytes,
                    const std::size_t visibleRowBytes,
                    std::size_t &extent) noexcept {
  extent = 0U;
  if (height == 0U || rowBytes < visibleRowBytes)
    return false;
  const std::size_t precedingRows = static_cast<std::size_t>(height - 1U);
  if (precedingRows != 0U &&
      rowBytes > (std::numeric_limits<std::size_t>::max() - visibleRowBytes) /
                     precedingRows) {
    return false;
  }
  extent = precedingRows * rowBytes + visibleRowBytes;
  return true;
}

bool CheckedMultiply(const std::size_t left, const std::size_t right,
                     std::size_t &product) noexcept {
  product = 0U;
  if (right != 0U &&
      left > std::numeric_limits<std::size_t>::max() / right) {
    return false;
  }
  product = left * right;
  return true;
}

bool IsCanceled(const std::function<bool()> &cancel) noexcept {
  if (!cancel)
    return false;
  try {
    return cancel();
  } catch (...) {
    return true;
  }
}

std::uint8_t Unassociate(const std::uint8_t channel,
                        const std::uint8_t alpha) noexcept {
  if (alpha == 0U)
    return 0U;
  const std::uint32_t value =
      (static_cast<std::uint32_t>(channel) * 255U + alpha / 2U) / alpha;
  return static_cast<std::uint8_t>(std::min(value, 255U));
}

} // namespace

bool ConvertAssociatedRgba8ToCanonicalStraight(
    const AssociatedRgba8View &source,
    const StraightRgba8WriteView &destination,
    const std::function<bool()> &cancel, std::string &error) noexcept {
  error.clear();
  if (!source.bytes || !destination.bytes || source.width == 0U ||
      source.height == 0U) {
    error = "associated RGBA8 bridge has an invalid canvas";
    return false;
  }
  std::size_t visibleRowBytes = 0U;
  if (!CheckedMultiply(static_cast<std::size_t>(source.width), 4U,
                       visibleRowBytes)) {
    error = "associated RGBA8 bridge row size overflows";
    return false;
  }
  std::size_t sourceExtent = 0U;
  std::size_t destinationExtent = 0U;
  if (!RequiredExtent(source.height, source.rowBytes, visibleRowBytes,
                      sourceExtent) ||
      !RequiredExtent(source.height, destination.rowBytes, visibleRowBytes,
                      destinationExtent) ||
      source.byteSize < sourceExtent ||
      destination.byteSize < destinationExtent) {
    error = "associated RGBA8 bridge extent is not canonical";
    return false;
  }

  for (std::uint32_t row = 0U; row < source.height; ++row) {
    if (IsCanceled(cancel)) {
      error = "canceled";
      return false;
    }
    const auto *sourceRow =
        source.bytes + static_cast<std::size_t>(row) * source.rowBytes;
    auto *destinationRow =
        destination.bytes + static_cast<std::size_t>(row) * destination.rowBytes;
    for (std::uint32_t column = 0U; column < source.width; ++column) {
      const std::size_t offset = static_cast<std::size_t>(column) * 4U;
      const std::uint8_t alpha = sourceRow[offset + 3U];
      destinationRow[offset + 0U] =
          Unassociate(sourceRow[offset + 0U], alpha);
      destinationRow[offset + 1U] =
          Unassociate(sourceRow[offset + 1U], alpha);
      destinationRow[offset + 2U] =
          Unassociate(sourceRow[offset + 2U], alpha);
      destinationRow[offset + 3U] = alpha;
    }
  }
  return true;
}

} // namespace videocut::skia_runtime::internal
