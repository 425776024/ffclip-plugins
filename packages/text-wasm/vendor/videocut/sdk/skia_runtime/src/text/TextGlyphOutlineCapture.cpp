#include "text/TextGlyphOutlineCapture.h"

#include "modules/skparagraph/include/Paragraph.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace videocut::skia_runtime::internal {
namespace {

constexpr float kTextProSdfCanonicalEm = 150.0F;
constexpr float kTextProSdfNominalPointSize = 14.0F;

bool IsFinite(const SkPoint point) noexcept {
  return std::isfinite(point.x()) && std::isfinite(point.y());
}

bool WouldExceed(const std::size_t current, const std::size_t additional,
                 const std::size_t limit) noexcept {
  return current > limit || additional > limit - current;
}

bool ValidateAndOrderUnits(
    const TextGlyphOutlineCaptureRequest &request,
    std::vector<const TextGlyphOutlineCaptureUnit *> &ordered,
    std::string &error) {
  ordered.clear();
  ordered.reserve(request.units.size());
  for (const auto &unit : request.units) {
    if (unit.stableUnitId == 0U || unit.paragraphId.empty() ||
        unit.runId.empty() ||
        unit.paragraphUtf8Begin >= unit.paragraphUtf8End ||
        unit.runUtf8Begin >= unit.runUtf8End ||
        unit.paragraphUtf8End - unit.paragraphUtf8Begin !=
            unit.runUtf8End - unit.runUtf8Begin) {
      error = "shaped glyph capture has an invalid unit identity";
      return false;
    }
    ordered.push_back(&unit);
  }
  std::sort(ordered.begin(), ordered.end(), [](const auto *left,
                                                const auto *right) {
    if (left->paragraphUtf8Begin != right->paragraphUtf8Begin)
      return left->paragraphUtf8Begin < right->paragraphUtf8Begin;
    return left->paragraphUtf8End < right->paragraphUtf8End;
  });
  for (std::size_t index = 1U; index < ordered.size(); ++index) {
    // A capture request may cover more than one paragraph.  Ranges from
    // different paragraphs are independent coordinate domains and must not be
    // rejected merely because their local UTF-8 offsets restart at zero.
    if (ordered[index - 1U]->paragraphId == ordered[index]->paragraphId &&
        ordered[index - 1U]->paragraphUtf8End >
            ordered[index]->paragraphUtf8Begin) {
      error = "shaped glyph capture unit identities overlap or span paragraphs";
      return false;
    }
  }
  return true;
}

const TextGlyphOutlineCaptureUnit *FindUnit(
    const std::vector<const TextGlyphOutlineCaptureUnit *> &ordered,
    const std::size_t paragraphUtf8Cluster) noexcept {
  const auto upper = std::upper_bound(
      ordered.begin(), ordered.end(), paragraphUtf8Cluster,
      [](const std::size_t cluster,
         const TextGlyphOutlineCaptureUnit *candidate) {
        return cluster < candidate->paragraphUtf8Begin;
      });
  if (upper == ordered.begin())
    return nullptr;
  const auto *candidate = *std::prev(upper);
  return paragraphUtf8Cluster >= candidate->paragraphUtf8Begin &&
                 paragraphUtf8Cluster < candidate->paragraphUtf8End
             ? candidate
             : nullptr;
}

} // namespace

bool CaptureTextGlyphOutlines(
    const TextGlyphOutlineCaptureRequest &request,
    std::vector<CapturedTextGlyphOutline> &outlines, std::string &error) {
  outlines.clear();
  error.clear();
  if (!request.paragraph) {
    error = "shaped glyph capture requires a laid-out paragraph";
    return false;
  }
  if (!std::isfinite(request.paragraphX) ||
      !std::isfinite(request.paragraphY) ||
      !request.writingTransform.isFinite()) {
    error = "shaped glyph capture placement is non-finite";
    return false;
  }
  if (request.maxGlyphCount == 0U || request.maxPathCount == 0U ||
      request.maxPathPointCount == 0U || request.maxPathVerbCount == 0U) {
    error = "shaped glyph capture budgets must be non-zero";
    return false;
  }
  // SkParagraph keeps its unresolved-glyph sentinel for an empty paragraph
  // even after layout. There is no glyph capture work in that valid state.
  if (request.units.empty())
    return true;
  if (request.paragraph->unresolvedGlyphs() < 0) {
    error = "shaped glyph capture paragraph has not been laid out";
    return false;
  }

  std::vector<const TextGlyphOutlineCaptureUnit *> orderedUnits;
  if (!ValidateAndOrderUnits(request, orderedUnits, error))
    return false;

  std::size_t glyphCount = 0U;
  std::size_t pointCount = 0U;
  std::size_t verbCount = 0U;
  std::size_t shapedRunIndex = 0U;
  bool failed = false;
  std::map<std::pair<std::uint64_t, std::size_t>, std::size_t>
      nextGlyphIndexInCluster;
  request.paragraph->visit(
      [&](const int lineIndex,
          const skia::textlayout::Paragraph::VisitorInfo *info) {
        if (failed || !info)
          return;
        const std::size_t thisRunIndex = shapedRunIndex++;
        if (!info->glyphs || !info->positions || !info->utf8Starts ||
            !IsFinite(info->origin) ||
            info->font.getEdging() != kQtTextGlyphEdging ||
            info->font.getHinting() != kQtTextGlyphHinting ||
            info->font.isSubpixel() != kQtTextGlyphSubpixelPositioning) {
          error = "shaped glyph run violates the frozen outline sampling contract";
          failed = true;
          return;
        }
        // Effect Programs intentionally use a zero absolute font size as a
        // hidden Letter. It has no TextPro surface and must not manufacture a
        // degenerate one-pixel outline/atlas entry.
        if (!std::isfinite(info->font.getSize()) ||
            info->font.getSize() <= 0.0F) {
          return;
        }
        if (info->count < 0 ||
            WouldExceed(glyphCount, static_cast<std::size_t>(info->count),
                        request.maxGlyphCount)) {
          error = "shaped glyph capture exceeded its glyph budget";
          failed = true;
          return;
        }
        glyphCount += static_cast<std::size_t>(info->count);

        SkFont nominalFont = info->font;
        nominalFont.setSize(kTextProSdfNominalPointSize);
        nominalFont.setSubpixel(false);
        nominalFont.setHinting(kQtTextGlyphHinting);
        nominalFont.setEdging(kQtTextGlyphEdging);
        SkFontMetrics nominalMetrics{};
        nominalFont.getMetrics(&nominalMetrics);
        const float nominalAscent = std::max(0.0F, -nominalMetrics.fAscent);
        const float nominalDescent = std::max(0.0F, nominalMetrics.fDescent);
        const float rawNominalHeight = nominalAscent + nominalDescent;
        const float nominalNormalization =
            rawNominalHeight > 0.0F
                ? kTextProSdfNominalPointSize / rawNominalHeight
                : 1.0F;
        const float normalizedNominalAscent =
            nominalAscent * nominalNormalization;
        const float normalizedNominalDescent =
            nominalNormalization * nominalDescent;
        const float normalizedNominalHeight =
            normalizedNominalAscent + normalizedNominalDescent;
        const float nominalAtlasScale =
            kTextProSdfCanonicalEm /
            std::max(1.0F, normalizedNominalHeight);
        if (std::getenv("VIDEOCUT_TRACE_QT_TEXT_GLYPH_METRICS") != nullptr) {
          std::fprintf(
              stderr,
              "[VIDEOCUT_QT_TEXT_GLYPH_METRICS] point_size=%a ascent=%a "
              "descent=%a raw_height=%a normalization=%a "
              "normalized_height=%a atlas_scale=%a\n",
              static_cast<double>(kTextProSdfNominalPointSize),
              static_cast<double>(nominalAscent),
              static_cast<double>(nominalDescent),
              static_cast<double>(rawNominalHeight),
              static_cast<double>(nominalNormalization),
              static_cast<double>(normalizedNominalHeight),
              static_cast<double>(nominalAtlasScale));
        }

        const auto baselineInParagraph =
            SkPoint::Make(request.paragraphX + info->origin.x(),
                          request.paragraphY + info->origin.y());
        const SkPoint baselineInReference =
            request.writingTransform.mapPoint(baselineInParagraph);
        if (!IsFinite(baselineInReference)) {
          error = "shaped glyph run has a non-finite baseline";
          failed = true;
          return;
        }

        for (int glyphIndex = 0; glyphIndex < info->count; ++glyphIndex) {
          const auto paragraphCluster =
              static_cast<std::size_t>(info->utf8Starts[glyphIndex]);
          const auto *unit = FindUnit(orderedUnits, paragraphCluster);
          if (!unit)
            continue;
          const auto position = info->positions[glyphIndex];
          if (!IsFinite(position)) {
            error = "shaped glyph has a non-finite baseline-relative origin";
            failed = true;
            continue;
          }

          const SkGlyphID glyph = info->glyphs[glyphIndex];
          const SkRect metricBounds = info->font.getBounds(glyph, nullptr);
          if (!metricBounds.isFinite()) {
            error = "shaped glyph has non-finite font bounds";
            failed = true;
            return;
          }
          auto sourcePath = info->font.getPath(glyph);
          if (!sourcePath || sourcePath->isEmpty()) {
            // Some fallback/color glyphs expose metrics without a vector
            // outline.  They have no SDF contribution; omit that glyph and
            // let the regular SkParagraph/material path provide the fallback.
            continue;
          }
          if (!sourcePath->isFinite()) {
            error = "shaped glyph outline is non-finite";
            failed = true;
            return;
          }

          std::optional<SkPath> nominalRecordPath;
          auto nominalCoordinateSystem =
              QtTextGlyphOutlineCoordinateSystem::RendererYDown;
          QtTextGlyphOutlineResult nativeNominalOutline;
          std::string nativeOutlineError;
          if (BuildQtTextGlyphOutlinePath(
                  {kQtTextGlyphOutlineImplementationVersion,
                   nominalFont.getTypeface(), glyph,
                   kTextProSdfNominalPointSize},
                  nativeNominalOutline, nativeOutlineError)) {
            nominalCoordinateSystem = nativeNominalOutline.coordinateSystem;
            nominalRecordPath = std::move(nativeNominalOutline.path);
          } else {
            nominalRecordPath = nominalFont.getPath(glyph);
          }
          if (!nominalRecordPath || nominalRecordPath->isEmpty() ||
              !nominalRecordPath->isFinite()) {
            // Keep the capture usable when the platform font cannot expose a
            // nominal outline.  The caller can continue with its source
            // picture instead of dropping the whole text pass.
            continue;
          }
          const SkRect nominalRecordBounds =
              nominalRecordPath->computeTightBounds();
          if (!nominalRecordBounds.isFinite() ||
              nominalRecordBounds.isEmpty() ||
              !std::isfinite(nominalAtlasScale) ||
              nominalAtlasScale <= 0.0F) {
            error = "shaped glyph has invalid canonical nominal metrics";
            failed = true;
            return;
          }

          const auto sourcePointCount =
              static_cast<std::size_t>(sourcePath->countPoints());
          const auto sourceVerbCount =
              static_cast<std::size_t>(sourcePath->countVerbs());
          if (WouldExceed(pointCount, sourcePointCount,
                          request.maxPathPointCount) ||
              WouldExceed(verbCount, sourceVerbCount,
                          request.maxPathVerbCount)) {
            error = "shaped glyph outline exceeded its path or verb budget";
            failed = true;
            return;
          }

          const auto glyphInParagraph = SkPoint::Make(
              baselineInParagraph.x() + position.x(),
              baselineInParagraph.y() + position.y());
          SkMatrix glyphToParagraph;
          glyphToParagraph.setTranslate(glyphInParagraph.x(),
                                        glyphInParagraph.y());
          const auto glyphToReference =
              SkMatrix::Concat(request.writingTransform, glyphToParagraph);
          auto transformed = sourcePath->tryMakeTransform(glyphToReference);
          if (!transformed || !transformed->isFinite() ||
              transformed->isEmpty()) {
            error = "shaped glyph outline transform failed";
            failed = true;
            return;
          }
          const auto transformedPointCount =
              static_cast<std::size_t>(transformed->countPoints());
          const auto transformedVerbCount =
              static_cast<std::size_t>(transformed->countVerbs());
          if (WouldExceed(pointCount, transformedPointCount,
                          request.maxPathPointCount) ||
              WouldExceed(verbCount, transformedVerbCount,
                          request.maxPathVerbCount) ||
              outlines.size() >= request.maxPathCount) {
            error = "transformed glyph outlines exceeded their capture budget";
            failed = true;
            return;
          }
          const SkRect tightBounds = transformed->computeTightBounds();
          if (!tightBounds.isFinite() || tightBounds.isEmpty()) {
            error = "transformed glyph outline has invalid tight bounds";
            failed = true;
            return;
          }

          const SkPoint glyphInReference =
              request.writingTransform.mapPoint(glyphInParagraph);
          if (!IsFinite(glyphInReference)) {
            error = "shaped glyph produced a non-finite reference origin";
            failed = true;
            return;
          }
          const auto clusterKey =
              std::make_pair(unit->stableUnitId, paragraphCluster);
          const auto glyphIndexInCluster =
              nextGlyphIndexInCluster[clusterKey]++;

          CapturedTextGlyphOutline captured;
          captured.path = std::move(*transformed);
          captured.tightBounds = tightBounds;
          captured.nominalRecordPath = std::move(*nominalRecordPath);
          captured.nominalRecordBounds = nominalRecordBounds;
          captured.shapedFontSize = info->font.getSize();
          captured.nominalAtlasScale = nominalAtlasScale;
          captured.nominalCoordinateSystem = nominalCoordinateSystem;
          captured.baselineOrigin = baselineInReference;
          captured.glyphOrigin = glyphInReference;
          captured.stableUnitId = unit->stableUnitId;
          captured.paragraphId = unit->paragraphId;
          captured.runId = unit->runId;
          captured.paragraphUtf8Cluster = paragraphCluster;
          captured.runUtf8Cluster =
              unit->runUtf8Begin +
              (paragraphCluster - unit->paragraphUtf8Begin);
          captured.glyphIndexInCluster = glyphIndexInCluster;
          captured.shapedRunIndex = thisRunIndex;
          captured.glyphIndexInShapedRun =
              static_cast<std::size_t>(glyphIndex);
          captured.glyphId = glyph;
          captured.lineIndex = lineIndex;
          pointCount += transformedPointCount;
          verbCount += transformedVerbCount;
          outlines.push_back(std::move(captured));
        }
      });

  if (failed) {
    outlines.clear();
    return false;
  }
  return true;
}

} // namespace videocut::skia_runtime::internal
