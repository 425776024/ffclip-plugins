#include "text/TextRenderPipeline.h"

namespace videocut::skia_runtime::internal::text_lane {


int TextEffectUnitType(const text::RichTextRun *run,
                       const UnitGeometry &unit, SkUnicode &unicode) {
  if (!run || unit.binding.utf8Begin >= unit.binding.utf8End ||
      unit.binding.utf8End > run->utf8Text.size()) {
    return 0;
  }
  bool hasCodepoint = false;
  bool hardBreak = false;
  bool whitespace = true;
  const auto *begin = run->utf8Text.data() + unit.binding.utf8Begin;
  const auto length = unit.binding.utf8End - unit.binding.utf8Begin;
  unicode.forEachCodepoint(
      begin, static_cast<std::int32_t>(length),
      [&](const SkUnichar codepoint, std::size_t, std::size_t, std::size_t) {
        hasCodepoint = true;
        hardBreak = hardBreak || unicode.isHardBreak(codepoint);
        whitespace = whitespace && unicode.isWhitespace(codepoint);
      });
  if (hardBreak)
    return 2;
  return hasCodepoint && whitespace ? 1 : 0;
}


std::uint32_t TextEffectUnitUnicodeCodepoint(
    const text::RichTextRun *run, const UnitGeometry &unit) noexcept {
  if (!run || unit.binding.utf8Begin >= unit.binding.utf8End ||
      unit.binding.utf8End > run->utf8Text.size()) {
    return 0U;
  }
  const char *cursor = run->utf8Text.data() + unit.binding.utf8Begin;
  const char *const end = run->utf8Text.data() + unit.binding.utf8End;
  if (SkUTF::CountUTF8(cursor, static_cast<std::size_t>(end - cursor)) != 1)
    return 0U;
  const auto codepoint = SkUTF::NextUTF8(&cursor, end);
  if (codepoint <= 0 || cursor != end)
    return 0U;
  return static_cast<std::uint32_t>(codepoint);
}


text::Color RepresentativeMaterialColor(
    const text::TextMaterialBinding &binding) {
  return std::visit(
      [](const auto &material) -> text::Color {
        using Material = std::decay_t<decltype(material)>;
        if constexpr (std::is_same_v<Material, text::SolidTextMaterial>) {
          return material.color;
        } else if constexpr (std::is_same_v<
                                 Material, text::LinearGradientTextMaterial> ||
                             std::is_same_v<
                                 Material, text::RadialGradientTextMaterial>) {
          return material.stops.empty()
                     ? text::Color{1.0F, 1.0F, 1.0F, 1.0F}
                     : material.stops.front().color;
        } else {
          if (material.underlayColor)
            return *material.underlayColor;
          if (!material.underlayGradient.empty())
            return material.underlayGradient.front().color;
          return {1.0F, 1.0F, 1.0F,
                  std::clamp(material.opacity, 0.0F, 1.0F)};
        }
      },
      ResolveTextMaterial(binding));
}


text::Color RepresentativeRunColor(const text::RichTextRun *run) {
  if (!run)
    return {1.0F, 1.0F, 1.0F, 1.0F};
  const auto fill = std::find_if(
      run->style.materials.layers.begin(), run->style.materials.layers.end(),
      [](const auto &layer) {
        return std::holds_alternative<text::TextFillLayer>(layer);
      });
  if (fill == run->style.materials.layers.end())
    return {1.0F, 1.0F, 1.0F, 1.0F};
  const auto *fillLayer = std::get_if<text::TextFillLayer>(&*fill);
  return fillLayer ? RepresentativeMaterialColor(fillLayer->material)
                   : text::Color{1.0F, 1.0F, 1.0F, 1.0F};
}


TextEffectCoordinateBridge ResolveDocumentTextEffectBridge(
    const text::ResolvedRichTextView &view,
    const text::TextLayerAppearance &appearance) {
  const float sourceWidth = appearance.sdfMaterial.sourceDesignWidth > 0.0F
                                ? appearance.sdfMaterial.sourceDesignWidth
                                : view.referenceCanvas.width;
  const float sourceHeight = appearance.sdfMaterial.sourceDesignHeight > 0.0F
                                 ? appearance.sdfMaterial.sourceDesignHeight
                                 : view.referenceCanvas.height;
  return ResolveTextEffectCoordinateBridge(
             view.referenceCanvas.width, view.referenceCanvas.height,
             sourceWidth, sourceHeight)
      .value_or(TextEffectCoordinateBridge{});
}


text::TextEffectRect ReferenceEffectRect(const SkRect &bounds) noexcept {
  return bounds.isEmpty()
             ? text::TextEffectRect{}
             : text::TextEffectRect{bounds.x(), bounds.y(), bounds.width(),
                                    bounds.height()};
}


text::TextEffectFrameInput BuildTextEffectEvaluationFrame(
    const ResolvedLayout &layout, const double progress,
    const std::int64_t localTimeUs,
    const text::ResolvedRichTextView &view,
    const text::TextLayerAppearance &appearance, SkUnicode &unicode) {
  text::TextEffectFrameInput input;
  input.progress = progress;
  input.timeUs = localTimeUs;
  input.writingMode = view.writingMode;
  input.tightTextRect = ReferenceEffectRect(layout.inkBounds);
  input.textRect = ReferenceEffectRect(layout.logicalBounds);
  if (localTimeUs == 0 &&
      std::getenv("VIDEOCUT_TRACE_TEXT_RENDER_GROUP_TOPOLOGY") != nullptr) {
    for (const auto &paragraph : layout.paragraphs) {
      paragraph.measurement->visit(
          [&](const int line, const Paragraph::VisitorInfo *info) {
            if (!info || info->count <= 0 || !info->glyphs)
              return;
            std::fprintf(stderr,
                         "[VIDEOCUT_TEXT_CANVAS_ADVANCE] line=%d font=%.9g "
                         "glyph=%u width=%.9g advance=%.9g count=%d\n",
                         line, info->font.getSize(), info->glyphs[0],
                         info->font.getWidth(info->glyphs[0]),
                         info->advanceX, info->count);
          });
    }
  }
  SkRect canvasBounds = SkRect::MakeEmpty();
  input.units.reserve(layout.units.size());
  std::unordered_map<std::string, std::string> contentSlotByRun;
  for (const auto &slot : view.slots) {
    for (const auto &runId : slot.runIds)
      contentSlotByRun.emplace(runId, slot.slotId);
  }
  std::unordered_map<std::size_t, std::size_t> nextIndexInRow;
  std::vector<SkRect> tightRowBounds(layout.publicLayout.authoredLines.size(),
                                     SkRect::MakeEmpty());
  std::vector<SkRect> rowBounds(layout.publicLayout.authoredLines.size(),
                                SkRect::MakeEmpty());
  std::size_t nextRenderIndex = 0U;
  for (std::size_t index = 0U; index < layout.units.size(); ++index) {
    const auto &unit = layout.units[index];
    const auto row = VisualLineForUnit(layout, unit);
    if (rowBounds.size() <= row) {
      tightRowBounds.resize(row + 1U, SkRect::MakeEmpty());
      rowBounds.resize(row + 1U, SkRect::MakeEmpty());
    }
    tightRowBounds[row].join(unit.uniformTightAnchorBounds);
    rowBounds[row].join(unit.layoutBounds);
    const auto indexInRow = nextIndexInRow[row]++;
    const auto *run = FindRun(view, unit.binding.paragraphId,
                              unit.binding.runId);
    const int type = TextEffectUnitType(run, unit, unicode);
    // SDFText's canvasRect encloses shaped advances with font-em line cells.
    // SkParagraph's tight boxes include face ascent/descent (1.5 em for the
    // admitted CJK fallback), while logicalBounds includes the wrap width.
    // Neither is the unpadded source canvas consumed by material shaders.
    if (type != 2 && unit.canvasCellFontSize > 0.0F &&
        !unit.localBounds.isEmpty() &&
        unit.paragraphIndex < layout.paragraphs.size()) {
      const auto &paragraph = layout.paragraphs[unit.paragraphIndex];
      auto cell = SkRect::MakeXYWH(
          paragraph.x + unit.localBounds.left(),
          paragraph.y + unit.localBounds.centerY() -
              unit.canvasCellFontSize * 0.5F,
          unit.localBounds.width(), unit.canvasCellFontSize);
      cell = MapRect(layout.writingTransform, cell);
      cell.offset(0.0F, unit.visualReflowOffsetY);
      canvasBounds.join(cell);
    }
    text::TextEffectUnitInput frameUnit;
    frameUnit.stableUnitId = unit.binding.stableUnitId;
    frameUnit.index = index;
    frameUnit.renderIndex = nextRenderIndex;
    frameUnit.type = type;
    frameUnit.unicodeCodepoint = TextEffectUnitUnicodeCodepoint(run, unit);
    frameUnit.row = row;
    frameUnit.indexInRow = indexInRow;
    const auto programBounds = unit.scriptBounds.isEmpty()
                                   ? unit.layoutBounds
                                   : unit.scriptBounds;
    const auto initialPosition = unit.scriptBounds.isEmpty()
                                     ? SkPoint::Make(programBounds.centerX(),
                                                     programBounds.centerY())
                                     : unit.letterInitialPosition;
    frameUnit.programRect = ReferenceEffectRect(programBounds);
    frameUnit.tightRect =
        ReferenceEffectRect(unit.uniformTightAnchorBounds);
    frameUnit.rect = ReferenceEffectRect(unit.layoutBounds);
    frameUnit.initialPositionX = initialPosition.x();
    frameUnit.initialPositionY = initialPosition.y();
    frameUnit.instanceColor = RepresentativeRunColor(run);
    if (const auto slot = contentSlotByRun.find(unit.binding.runId);
        slot != contentSlotByRun.end()) {
      frameUnit.contentSlotId = slot->second;
    }
    frameUnit.paragraphId = unit.binding.paragraphId;
    frameUnit.runId = unit.binding.runId;
    frameUnit.documentUtf8Begin = unit.documentUtf8Begin;
    frameUnit.documentUtf8End = unit.documentUtf8End;
    frameUnit.documentWordUtf8Begin = unit.documentWordUtf8Begin;
    frameUnit.documentWordUtf8End = unit.documentWordUtf8End;
    frameUnit.visualLineIndex = row;
    if (row < layout.publicLayout.authoredLines.size()) {
      frameUnit.baselineY =
          layout.publicLayout.authoredLines[row].baseline;
    }
    if (type == 0) {
      frameUnit.normalUnitIndex = nextRenderIndex;
      ++nextRenderIndex;
    }
    if (run)
      frameUnit.fontSize = run->style.fontSize;
    input.units.push_back(frameUnit);
  }
  input.normalUnitCount = nextRenderIndex;
  input.canvasRect = ReferenceEffectRect(canvasBounds);
  input.tightRowRects.reserve(tightRowBounds.size());
  for (const auto &bounds : tightRowBounds)
    input.tightRowRects.push_back(ReferenceEffectRect(bounds));
  input.rowRects.reserve(rowBounds.size());
  for (const auto &bounds : rowBounds)
    input.rowRects.push_back(ReferenceEffectRect(bounds));
  static_cast<void>(appearance);
  return input;
}


bool BuildTextEffectEvaluationResources(
    const text::TextAnimationStack &animations,
    const text::TextEffectFramePlan &externalPlan,
    const bool suppressAnimation, TextVisualAssetStore &assets,
    std::vector<text::TextEffectEvaluationResource> &resources,
    std::string &error) {
  resources.clear();
  if (suppressAnimation)
    return true;
  std::unordered_map<std::string, std::size_t> resourceById;
  for (const auto &layer : animations.layers) {
    if (!layer.enabled)
      continue;
    for (const auto &decoration : layer.decorations) {
      const auto sampled = std::find_if(
          externalPlan.resources.begin(), externalPlan.resources.end(),
          [&](const auto &candidate) {
            return candidate.resourceId == decoration.decorationId ||
                   candidate.assetId == decoration.assetId;
          });
      std::string digest = sampled == externalPlan.resources.end()
                               ? std::string{}
                               : sampled->digest;
      const auto sampleTimeUs = sampled == externalPlan.resources.end()
                                    ? 0
                                    : sampled->localTimeUs;
      auto *asset =
          assets.ResolveUnqualified(decoration.assetId, digest, error);
      if (!asset)
        return false;
      if (digest.empty())
        digest = asset->reference.digest;
      if (digest.empty() && asset->bytes && !asset->bytes->empty()) {
        digest = videocut::vector::Sha256Digest(asset->bytes->data(),
                                                 asset->bytes->size());
      }
      float intrinsicWidth = asset->intrinsicWidth;
      float intrinsicHeight = asset->intrinsicHeight;
      if ((intrinsicWidth <= 0.0F || intrinsicHeight <= 0.0F) &&
          asset->stream) {
        std::string ignoredIdentity;
        auto frame = assets.SampleRaster(*asset, sampleTimeUs,
                                         ignoredIdentity, error);
        if (!frame)
          return false;
        intrinsicWidth = static_cast<float>(frame->width());
        intrinsicHeight = static_cast<float>(frame->height());
      }
      if (decoration.decorationId.empty() || decoration.assetId.empty() ||
          digest.empty() || !std::isfinite(intrinsicWidth) ||
          !std::isfinite(intrinsicHeight) || intrinsicWidth <= 0.0F ||
          intrinsicHeight <= 0.0F) {
        error = "animated text decoration has no admitted resource metadata";
        return false;
      }
      text::TextEffectEvaluationResource resource{
          decoration.decorationId, decoration.assetId, digest,
          intrinsicWidth, intrinsicHeight, asset->durationUs};
      const auto [found, inserted] =
          resourceById.emplace(resource.resourceId, resources.size());
      if (inserted) {
        resources.push_back(std::move(resource));
      } else {
        const auto &existing = resources[found->second];
        if (existing.assetId != resource.assetId ||
            existing.digest != resource.digest ||
            existing.intrinsicWidth != resource.intrinsicWidth ||
            existing.intrinsicHeight != resource.intrinsicHeight ||
            existing.durationUs != resource.durationUs) {
          error = "animated text decoration resource identity is ambiguous";
          return false;
        }
      }
    }
  }
  std::function<bool(const std::vector<text::TextEffectExecutionNode> &)>
      admitGraphResources;
  admitGraphResources =
      [&](const std::vector<text::TextEffectExecutionNode> &nodes) {
        for (const auto &node : nodes) {
          for (const auto &resourceId : node.resourceIds) {
            const auto existing = resourceById.find(resourceId);
            if (existing != resourceById.end())
              continue;
            const auto sampled = std::find_if(
                externalPlan.resources.begin(), externalPlan.resources.end(),
                [&](const auto &candidate) {
                  // Package animation IR may carry either the logical
                  // resource_id or the normalized resident asset identity.
                  // Both are valid only when they resolve to the same
                  // already-admitted sample record.
                  return candidate.resourceId == resourceId ||
                         candidate.assetId == resourceId;
                });
            if (sampled == externalPlan.resources.end() ||
                sampled->assetId.empty() || sampled->digest.empty()) {
              error = "text execution graph resource has no admitted sample: " +
                      resourceId;
              return false;
            }
            resourceById.emplace(resourceId, resources.size());
            resources.push_back({resourceId, sampled->assetId,
                                 sampled->digest, 0.0F, 0.0F, 0});
          }
          if (!admitGraphResources(node.children))
            return false;
        }
        return true;
      };
  if (!admitGraphResources(animations.executionGraph.nodes))
    return false;
  return true;
}


SkRect ToSkRect(const text::TextEffectRect &rect) {
  return SkRect::MakeXYWH(rect.x, rect.y, rect.width, rect.height);
}


SkRect ToSkRect(const text::Rect &rect) {
  return SkRect::MakeXYWH(rect.x, rect.y, rect.width, rect.height);
}


SkRect ExpandByInsets(const SkRect &source, const text::Insets &insets) {
  return SkRect::MakeLTRB(source.left() - insets.left,
                          source.top() - insets.top,
                          source.right() + insets.right,
                          source.bottom() + insets.bottom);
}


SkRect ExpandByOutsets(const SkRect &source,
                       const TextVisualOutsets &outsets) {
  return SkRect::MakeLTRB(source.left() - outsets.left,
                          source.top() - outsets.top,
                          source.right() + outsets.right,
                          source.bottom() + outsets.bottom);
}

} // namespace videocut::skia_runtime::internal::text_lane
