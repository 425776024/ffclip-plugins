#pragma once

#include "text/SkiaGpuContext.h"
#include "text/QtTextGlyphOutlineRuntime.h"

#include <string>

namespace videocut::skia_runtime::internal {

// The path and source bounds use atlas pixels with a top-left origin.
// sourceBounds is the caller-allocated, distance-range-padded glyph region;
// the builder fails instead of silently clipping an outline at that boundary.
struct TextSdfMeshBuildRequest final {
  const SkPath *path{nullptr};
  TextSdfGlyphIdentity glyphIdentity{};
  SkRect sourceBounds{};
  int atlasWidth{0};
  int atlasHeight{0};
  float rasterDistanceRange{0.0F};
  QtTextGlyphOutlineCoordinateSystem pathCoordinateSystem{
      QtTextGlyphOutlineCoordinateSystem::RendererYDown};
};

[[nodiscard]] bool
BuildTextSdfGpuMesh(const TextSdfMeshBuildRequest &request,
                    TextSdfGpuMesh &mesh, std::string &error);

} // namespace videocut::skia_runtime::internal
