#pragma once

#include "internal/skia/SkiaHeaders.h"
#include "text/QtTextGlyphOutlineRuntime.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace videocut::skia_runtime::internal {

// Qt's captured TextPro path uses one float-positioned glyph quad and an RG8
// SDF sampled by the material shader. Keep shaping origins fractional, avoid
// grid-fitting the vector outline, and never request LCD coverage for a mask
// whose antialiasing is resolved by the SDF shader.
inline constexpr SkFont::Edging kQtTextGlyphEdging =
    SkFont::Edging::kAntiAlias;
inline constexpr SkFontHinting kQtTextGlyphHinting = SkFontHinting::kNone;
inline constexpr bool kQtTextGlyphSubpixelPositioning = true;

struct TextGlyphOutlineCaptureUnit final {
  std::uint64_t stableUnitId{0U};
  std::string paragraphId;
  std::string runId;
  std::size_t paragraphUtf8Begin{0U};
  std::size_t paragraphUtf8End{0U};
  std::size_t runUtf8Begin{0U};
  std::size_t runUtf8End{0U};
};

struct TextGlyphOutlineCaptureRequest final {
  skia::textlayout::Paragraph *paragraph{nullptr};
  SkScalar paragraphX{0.0F};
  SkScalar paragraphY{0.0F};
  SkMatrix writingTransform{SkMatrix::I()};
  // Only glyphs whose paragraph-global cluster is covered by one of these
  // identities are captured. Ranges must be disjoint; geometry is never used
  // to recover an authored unit after shaping.
  std::vector<TextGlyphOutlineCaptureUnit> units;
  std::size_t maxGlyphCount{65'536U};
  std::size_t maxPathCount{65'536U};
  std::size_t maxPathPointCount{4U * 1'024U * 1'024U};
  std::size_t maxPathVerbCount{4U * 1'024U * 1'024U};
};

struct CapturedTextGlyphOutline final {
  SkPath path;
  SkRect tightBounds{SkRect::MakeEmpty()};
  // TextPro generates its distance field from a fixed 14-point outline, not
  // by scaling the laid-out path back down. Keep the native record beside the
  // reference-space outline so the SDF adapter can recover the canonical
  // 150-pixel em without losing CoreText binary32 geometry.
  SkPath nominalRecordPath;
  SkRect nominalRecordBounds{SkRect::MakeEmpty()};
  float shapedFontSize{0.0F};
  float nominalAtlasScale{1.0F};
  QtTextGlyphOutlineCoordinateSystem nominalCoordinateSystem{
      QtTextGlyphOutlineCoordinateSystem::RendererYDown};
  SkPoint baselineOrigin{SkPoint::Make(0.0F, 0.0F)};
  SkPoint glyphOrigin{SkPoint::Make(0.0F, 0.0F)};
  std::uint64_t stableUnitId{0U};
  std::string paragraphId;
  std::string runId;
  std::size_t paragraphUtf8Cluster{0U};
  std::size_t runUtf8Cluster{0U};
  std::size_t glyphIndexInCluster{0U};
  std::size_t shapedRunIndex{0U};
  std::size_t glyphIndexInShapedRun{0U};
  SkGlyphID glyphId{0U};
  int lineIndex{0};
};

[[nodiscard]] bool CaptureTextGlyphOutlines(
    const TextGlyphOutlineCaptureRequest &request,
    std::vector<CapturedTextGlyphOutline> &outlines, std::string &error);

} // namespace videocut::skia_runtime::internal
