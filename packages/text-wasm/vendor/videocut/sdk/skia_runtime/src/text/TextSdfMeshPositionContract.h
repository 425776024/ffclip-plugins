#pragma once

#include <cmath>

namespace videocut::skia_runtime::internal {

// Shape geometry is the glyph itself and must remain inside the normalized
// atlas. Distance geometry covers a quadratic control hull around that shape;
// Metal can clip up to one raster pixel of that hull at the atlas viewport.
[[nodiscard]] inline bool
IsTextSdfNormalizedAtlasPosition(const float value) noexcept {
  return std::isfinite(value) && value >= 0.0F && value <= 1.0F;
}

[[nodiscard]] inline bool
IsTextSdfDistanceAtlasPosition(const float value,
                               const int atlasExtent) noexcept {
  if (!std::isfinite(value) || atlasExtent <= 0)
    return false;
  const float rasterGuard = 1.0F / static_cast<float>(atlasExtent);
  return value >= -rasterGuard && value <= 1.0F + rasterGuard;
}

} // namespace videocut::skia_runtime::internal
