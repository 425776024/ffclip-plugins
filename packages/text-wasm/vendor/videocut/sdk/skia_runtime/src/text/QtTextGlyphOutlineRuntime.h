#pragma once

#include "internal/skia/SkiaHeaders.h"

#include <cstdint>
#include <string>

namespace videocut::skia_runtime::internal {

// Versioned CPU-side companion to the TextPro Metal SDF contract. Version 2
// preserves CoreText's native y-up coordinates. The recovered TextPro chain
// feeds those coordinates directly into FUN_0096a55c/FUN_0096ac2e; reflecting
// the path into renderer-y-down space first changes binary32 operation order
// even when a later vertex transform reflects it back.
inline constexpr std::uint32_t kQtTextGlyphOutlineImplementationVersion = 1U;

enum class QtTextGlyphOutlineCoordinateSystem : std::uint8_t {
  RendererYDown = 0U,
  CoreTextYUp = 1U,
};

struct QtTextGlyphOutlineRequest final {
  std::uint32_t implementationVersion{kQtTextGlyphOutlineImplementationVersion};
  const SkTypeface *typeface{nullptr};
  SkGlyphID glyph{0U};
  float pointSize{0.0F};
};

struct QtTextGlyphOutlineResult final {
  std::uint32_t implementationVersion{kQtTextGlyphOutlineImplementationVersion};
  QtTextGlyphOutlineCoordinateSystem coordinateSystem{
      QtTextGlyphOutlineCoordinateSystem::CoreTextYUp};
  SkPath path;
};

// Returns false when the platform/typeface cannot provide the native outline;
// callers may then use their platform-neutral outline implementation.
[[nodiscard]] bool
BuildQtTextGlyphOutlinePath(const QtTextGlyphOutlineRequest &request,
                            QtTextGlyphOutlineResult &result,
                            std::string &error);

} // namespace videocut::skia_runtime::internal
