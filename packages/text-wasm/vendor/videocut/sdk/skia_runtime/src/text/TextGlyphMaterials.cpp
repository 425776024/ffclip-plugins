#include "text/TextRenderPipeline.h"
#include "text/TextSdfPathTransform.h"

namespace videocut::skia_runtime::internal::text_lane {


std::string GlyphLayerId(const text::TextGlyphMaterialLayer &layer) {
  return std::visit([](const auto &value) { return value.layerId; }, layer);
}


text::TextBlendMode
GlyphLayerBlend(const text::TextGlyphMaterialLayer &layer) {
  return std::visit([](const auto &value) { return value.blend; }, layer);
}


std::vector<ResolvedGlyphMaterialPass> ResolveGlyphMaterialPasses(
    const text::ResolvedRichTextView &view,
    const text::TextEffectFramePlan &framePlan) {
  std::vector<ResolvedGlyphMaterialPass> result;
  std::size_t order = 0U;
  const auto appendPass = [&result, &order](ResolvedGlyphMaterialPass &pass) {
    if (const auto *shadow =
            std::get_if<text::TextShadowLayer>(&pass.material)) {
      const float radians = shadow->thicknessAngleDegrees *
                            3.14159265358979323846F / 180.0F;
      // A shadow's effect-style strokes are authored front-to-back inside
      // the shadow mask.  The glyph material target is painted
      // back-to-front, so preserve that nested topology by resolving the
      // child strokes in reverse authored order before the parent fill.
      for (std::size_t reverseIndex = shadow->strokes.size();
           reverseIndex > 0U; --reverseIndex) {
        const auto nestedIndex = reverseIndex - 1U;
        auto nestedStroke = shadow->strokes[nestedIndex];
        nestedStroke.zOrder = shadow->zOrder;
        nestedStroke.offsetX +=
            shadow->offsetX +
            std::cos(radians) * shadow->thicknessDistance;
        nestedStroke.offsetY +=
            shadow->offsetY -
            std::sin(radians) * shadow->thicknessDistance;
        // Qt draws the child outline before the shadow's softened fill.
        // Parent expansion sets its inner edge; parent blur is not a
        // child-outline uniform (TEXT_LETTER_MAT u_extraSmooth stays 0).
        nestedStroke.width += 2.0F * shadow->spread;
        // Inherit the resolved pass order, including animated overrides.
        auto nested = pass;
        nested.passId += ":nested-stroke:" +
                         std::to_string(nestedIndex);
        nested.layerId = nestedStroke.layerId;
        nested.material = std::move(nestedStroke);
        nested.authoredOrder = order++;
        nested.nestedShadowStroke = true;
        result.push_back(std::move(nested));
      }
    }
    pass.authoredOrder = order++;
    result.push_back(std::move(pass));
  };
  for (const auto &paragraph : view.paragraphs) {
    for (const auto &run : paragraph.runs) {
      for (const auto &ordered : OrderGlyphMaterialLayers(run.style.materials)) {
        if (!ordered.layer)
          continue;
        ResolvedGlyphMaterialPass pass;
        pass.passId = "authored:" + paragraph.paragraphId + ":" + run.runId +
                      ":" + std::string(ordered.layerId) + ":" +
                      std::to_string(ordered.authoredIndex);
        pass.layerId = ordered.layerId;
        pass.paragraphId = paragraph.paragraphId;
        pass.runId = run.runId;
        pass.zOrder = ordered.zOrder;
        pass.blend = ordered.blend;
        pass.material = *ordered.layer;
        if (auto *stroke =
                std::get_if<text::TextStrokeLayer>(&pass.material);
            stroke && !stroke->signedStartWidth &&
            stroke->innerRingWidth <= 0.0F &&
            stroke->spread <= 0.0F &&
            ordered.authoredIndex + 1U < run.style.materials.layers.size()) {
          const auto *next = std::get_if<text::TextStrokeLayer>(
              &run.style.materials.layers[ordered.authoredIndex + 1U]);
          constexpr float kCoordinateTolerance = 1.0e-3F;
          if (next && next->spread <= 0.0F &&
              next->width <= stroke->width &&
              std::fabs(next->offsetX - stroke->offsetX) <=
                  kCoordinateTolerance &&
              std::fabs(next->offsetY - stroke->offsetY) <=
                  kCoordinateTolerance) {
            stroke->innerRingWidth = next->width;
          }
        }
        appendPass(pass);
      }
    }
  }

  for (const auto &sampled : framePlan.glyphMaterialPasses) {
    if (sampled.combineMode == text::TextPropertyCombineMode::Replace &&
        !sampled.layerId.empty()) {
      result.erase(
          std::remove_if(result.begin(), result.end(), [&](const auto &pass) {
            return pass.layerId == sampled.layerId;
          }),
          result.end());
    }
    ResolvedGlyphMaterialPass sampledPass{
        sampled.passId,       sampled.layerId,
        {},                   {},
        sampled.zOrder,       sampled.blend,
        sampled.combineMode,  sampled.material,
        sampled.stableUnitIds, 0U};
    appendPass(sampledPass);
  }
  std::stable_sort(result.begin(), result.end(), [](const auto &left,
                                                     const auto &right) {
    if (left.zOrder != right.zOrder)
      return left.zOrder < right.zOrder;
    return left.authoredOrder < right.authoredOrder;
  });
  return result;
}


bool PassTargetsUnit(const ResolvedGlyphMaterialPass &pass,
                     const UnitGeometry &unit) {
  if (!pass.paragraphId.empty() &&
      pass.paragraphId != unit.binding.paragraphId)
    return false;
  if (!pass.runId.empty() && pass.runId != unit.binding.runId)
    return false;
  return pass.stableUnitIds.empty() ||
         std::find(pass.stableUnitIds.begin(), pass.stableUnitIds.end(),
                   unit.binding.stableUnitId) != pass.stableUnitIds.end();
}


const text::TextGlyphMaterialLayer *FindAuthoredGlyphLayer(
    const text::RichTextRun &run, const std::string &layerId) noexcept {
  const auto found = std::find_if(
      run.style.materials.layers.begin(), run.style.materials.layers.end(),
      [&](const auto &layer) { return GlyphLayerId(layer) == layerId; });
  return found == run.style.materials.layers.end() ? nullptr : &*found;
}


void ApplyLayerGeometry(const text::TextGlyphMaterialLayer &layer,
                        SkPaint &paint) {
  // The only filters installed below are Blur and Offset; both preserve an
  // entirely transparent input. Skia cannot prove this for an arbitrary
  // image filter and therefore still rasterizes zero-opacity glyph passes.
  // Keep the pass and its validated resources, but let the zero-alpha paint
  // take Skia's ordinary no-draw path instead of allocating a blur surface.
  if (paint.getAlphaf() == 0.0F && !paint.getColorFilter() &&
      !paint.getImageFilter() &&
      GlyphLayerBlend(layer) == text::TextBlendMode::SourceOver &&
      paint.asBlendMode() == SkBlendMode::kSrcOver)
    return;
  std::visit(
      [&](const auto &value) {
        using Value = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Value, text::TextFillLayer>) {
          if (value.offsetX != 0.0F || value.offsetY != 0.0F)
            paint.setImageFilter(SkImageFilters::Offset(
                value.offsetX, value.offsetY, nullptr));
        } else if constexpr (std::is_same_v<Value, text::TextStrokeLayer>) {
          paint.setStyle(SkPaint::kStroke_Style);
          paint.setStrokeJoin(SkPaint::kRound_Join);
          paint.setStrokeCap(SkPaint::kRound_Cap);
          paint.setStrokeWidth(std::max(0.0F, value.width) +
                               std::max(0.0F, value.spread) * 2.0F);
          sk_sp<SkImageFilter> filter;
          if (value.blurRadius > 0.0F) {
            filter = SkImageFilters::Blur(value.blurRadius, value.blurRadius,
                                          SkTileMode::kDecal, nullptr);
          }
          if (value.offsetX != 0.0F || value.offsetY != 0.0F) {
            filter = SkImageFilters::Offset(value.offsetX, value.offsetY,
                                            std::move(filter));
          }
          paint.setImageFilter(std::move(filter));
        } else if constexpr (std::is_same_v<Value, text::TextShadowLayer>) {
          const float radians = value.thicknessAngleDegrees *
                                3.14159265358979323846F / 180.0F;
          const float offsetX =
              value.offsetX + std::cos(radians) * value.thicknessDistance;
          const float offsetY =
              value.offsetY - std::sin(radians) * value.thicknessDistance;
          sk_sp<SkImageFilter> filter;
          const float blur =
              std::max(0.0F, value.blurRadius + value.spread);
          if (blur > 0.0F) {
            filter = SkImageFilters::Blur(blur, blur, SkTileMode::kDecal,
                                          nullptr);
          }
          if (offsetX != 0.0F || offsetY != 0.0F) {
            filter = SkImageFilters::Offset(offsetX, offsetY,
                                            std::move(filter));
          }
          paint.setImageFilter(std::move(filter));
        } else if constexpr (std::is_same_v<Value, text::TextGlowLayer>) {
          sk_sp<SkImageFilter> filter;
          const float blur = std::max(0.0F, value.radius + value.spread);
          if (blur > 0.0F) {
            filter = SkImageFilters::Blur(blur, blur, SkTileMode::kDecal,
                                          nullptr);
          }
          if (value.directionX != 0.0F || value.directionY != 0.0F) {
            filter = SkImageFilters::Offset(value.directionX * value.radius,
                                            value.directionY * value.radius,
                                            std::move(filter));
          }
          paint.setImageFilter(std::move(filter));
        }
      },
      layer);
}


const text::TextMaterialBinding &
GlyphLayerMaterial(const text::TextGlyphMaterialLayer &layer) {
  return std::visit(
      [](const auto &value) -> const text::TextMaterialBinding & {
        return value.material;
      },
      layer);
}


text::TextMaterialBinding &
MutableGlyphLayerMaterial(text::TextGlyphMaterialLayer &layer) {
  return std::visit(
      [](auto &value) -> text::TextMaterialBinding & { return value.material; },
      layer);
}


std::optional<float>
FactorTextProRenderGroupLayerOpacity(text::TextGlyphMaterialLayer &layer) {
  auto &binding = MutableGlyphLayerMaterial(layer);
  auto *material = std::visit(
      [](auto &value) -> text::TextMaterial * {
        using Value = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Value, text::LiteralTextMaterial>)
          return &value.material;
        else
          return &value.fallback;
      },
      binding);
  return std::visit(
      [](auto &value) -> std::optional<float> {
        using Material = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Material,
                                     text::SolidTextMaterial>) {
          if (value.color.alpha >= 0.999999F)
            return std::nullopt;
          const auto opacity =
              std::clamp(value.color.alpha, 0.0F, 1.0F);
          value.color.alpha = 1.0F;
          return opacity;
        } else if constexpr (std::is_same_v<Material,
                                            text::TextureTextMaterial>) {
          if (value.opacity >= 0.999999F)
            return std::nullopt;
          const auto opacity = std::clamp(value.opacity, 0.0F, 1.0F);
          value.opacity = 1.0F;
          return opacity;
        } else {
          // The Qt style reader folds a layer's RenderGroup alpha into every
          // gradient-stop alpha.  TEXT_LETTER_MAT itself receives the
          // normalized stops, writes that layer to RGBA8, then the following
          // RenderGroup draw applies the common alpha.  Factor only a common
          // sub-unity maximum; gradients whose authored alpha domain reaches
          // one retain their complete per-stop alpha curve.
          if (value.stops.empty())
            return std::nullopt;
          float commonOpacity = 0.0F;
          for (const auto &stop : value.stops)
            commonOpacity = std::max(
                commonOpacity, std::clamp(stop.color.alpha, 0.0F, 1.0F));
          if (commonOpacity >= 0.999999F)
            return std::nullopt;
          if (commonOpacity > 0.0F) {
            for (auto &stop : value.stops) {
              stop.color.alpha = std::clamp(
                  stop.color.alpha / commonOpacity, 0.0F, 1.0F);
            }
          } else {
            for (auto &stop : value.stops)
              stop.color.alpha = 1.0F;
          }
          return commonOpacity;
        }
      },
      *material);
}


const text::TextMaterialCoordinates *GlyphMaterialCoordinates(
    const text::TextGlyphMaterialLayer &layer) noexcept {
  const auto &material = ResolveTextMaterial(GlyphLayerMaterial(layer));
  return std::visit(
      [](const auto &value) -> const text::TextMaterialCoordinates * {
        using Value = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Value, text::SolidTextMaterial>)
          return nullptr;
        else
          return &value.coordinates;
      },
      material);
}


TextSdfCoordinateDomain SdfCoordinateDomain(
    const text::TextGlyphMaterialLayer &layer) noexcept {
  const auto *coordinates = GlyphMaterialCoordinates(layer);
  return coordinates &&
                 coordinates->coordinateSpace !=
                     text::PaintCoordinateSpace::Grapheme
             ? TextSdfCoordinateDomain::TextRect
             : TextSdfCoordinateDomain::GlyphRect;
}


SkRect SdfTextCoordinateBounds(const text::TextGlyphMaterialLayer &layer,
                               const ResolvedLayout &layout,
                               const ParagraphLayout &paragraph) noexcept {
  const auto *coordinates = GlyphMaterialCoordinates(layer);
  return coordinates &&
                 coordinates->coordinateSpace ==
                     text::PaintCoordinateSpace::LayoutBox
             ? paragraph.materialLayoutBounds
             : paragraph.materialTextBounds;
}


void ClearLegacySdfCoordinateOutset(
    text::TextGlyphMaterialLayer &layer) noexcept {
  auto &binding = MutableGlyphLayerMaterial(layer);
  text::TextMaterial *material = nullptr;
  if (auto *literal = std::get_if<text::LiteralTextMaterial>(&binding))
    material = &literal->material;
  else if (auto *slot = std::get_if<text::EditableTextStyleSlot>(&binding))
    material = &slot->fallback;
  if (!material)
    return;
  std::visit(
      [](auto &value) {
        using Value = std::decay_t<decltype(value)>;
        if constexpr (!std::is_same_v<Value, text::SolidTextMaterial>)
          value.coordinates.coordinateOutset = 0.0F;
      },
      *material);
}


bool ResolveSdfMaterialImages(
    const text::TextGlyphMaterialLayer &layer,
    const std::size_t materialUnitIndex, const std::int64_t localTimeUs,
    TextVisualAssetStore &assets, sk_sp<SkImage> &texture,
    sk_sp<SkImage> &replacementMask, std::vector<std::string> &assetSamples,
    std::string &error) {
  texture.reset();
  replacementMask.reset();
  const auto &binding = GlyphLayerMaterial(layer);
  const auto &material = ResolveTextMaterial(binding);
  if (const auto *textureMaterial =
          std::get_if<text::TextureTextMaterial>(&material)) {
    std::string sample;
    texture = ResolveTextureMaterialFrame(*textureMaterial, materialUnitIndex,
                                          localTimeUs, assets, sample, error);
    if (!texture)
      return false;
    assetSamples.push_back(std::move(sample));
  }
  if (const auto *slot = std::get_if<text::EditableTextStyleSlot>(&binding);
      slot && slot->replacementMask) {
    std::string sample;
    replacementMask = ResolveVisualTextureFrame(
        *slot->replacementMask, localTimeUs, assets, sample, error);
    if (!replacementMask)
      return false;
    assetSamples.push_back("mask:" + sample);
  }
  return true;
}


TextSdfGpuRect ToTextSdfRect(const SkRect &rect,
                             const float originX,
                             const float originY) noexcept {
  return {rect.left() - originX, rect.top() - originY, rect.width(),
          rect.height()};
}


namespace {

const UnitGeometry *FindSdfUnitForOutline(
    const std::vector<const UnitGeometry *> &unitsByStableId,
    const CapturedTextGlyphOutline &outline) noexcept {
  auto found = std::lower_bound(
      unitsByStableId.begin(), unitsByStableId.end(), outline.stableUnitId,
      [](const UnitGeometry *unit, const std::uint64_t stableUnitId) {
        return unit->binding.stableUnitId < stableUnitId;
      });
  for (; found != unitsByStableId.end() &&
         (*found)->binding.stableUnitId == outline.stableUnitId; ++found) {
    const auto *unit = *found;
    if (unit->binding.paragraphId == outline.paragraphId &&
        unit->binding.runId == outline.runId &&
        outline.runUtf8Cluster >= unit->binding.utf8Begin &&
        outline.runUtf8Cluster < unit->binding.utf8End) {
      return unit;
    }
  }
  return nullptr;
}

} // namespace


bool BuildGlyphPassParagraph(
    const text::RichTextParagraph &paragraph,
    const ResolvedGlyphMaterialPass &pass, const ResolvedLayout &layout,
    const text::TextEffectFramePlan &framePlan,
    std::optional<GlyphPassLayoutState> &layoutState,
    const FontContext &fonts,
    const std::int64_t localTimeUs, TextVisualAssetStore &assets,
    std::unique_ptr<Paragraph> &output, std::vector<std::string> &assetSamples,
    std::string &error, const bool captureOnly) {
  // The caller owns this cache for one recording call with an unchanged
  // layout and frame plan. Resolve lazily to preserve first-use error order.
  if (!layoutState) {
    std::vector<TextEffectLayoutUnitBinding> bindings;
    bindings.reserve(layout.units.size());
    for (const auto &unit : layout.units)
      bindings.push_back(unit.binding);
    ResolvedTextEffectLayoutMutations mutations;
    if (!mutations.Resolve(framePlan, bindings, error))
      return false;
    GlyphPassLayoutState state;
    state.mutations = std::move(mutations);
    for (std::size_t index = 0U; index < layout.units.size(); ++index) {
      const auto &unit = layout.units[index].binding;
      state.units.try_emplace(
          GlyphPassLayoutState::UnitKey{unit.paragraphId, unit.runId,
                                       unit.utf8Begin, unit.utf8End},
          GlyphPassLayoutState::Entry{index, std::nullopt});
    }
    layoutState = std::move(state);
  } else {
    error.clear();
  }
  const auto &mutations = layoutState->mutations;

  auto builder = ParagraphBuilder::make(
      MakeParagraphStyle(paragraph.style,
                         framePlan.sourceCreationComponent !=
                             text::TextSourceCreationComponent::SdfText),
                                        fonts.collection, fonts.unicode);
  if (!builder) {
    error = "SkParagraph material-pass builder creation failed";
    return false;
  }
  for (const auto &run : paragraph.runs) {
    const auto offsets = GraphemeOffsets(
        *fonts.unicode, run.utf8Text,
        run.locale.empty() ? paragraph.style.locale : run.locale);
    for (std::size_t index = 1U; index < offsets.size(); ++index) {
      const auto begin = offsets[index - 1U];
      const auto end = offsets[index];
      const auto found = layoutState->units.find(
          {paragraph.paragraphId, run.runId, begin, end});
      const UnitGeometry *unit = nullptr;
      const ResolvedTextEffectLayoutMutation *mutation = nullptr;
      if (found != layoutState->units.end()) {
        auto &entry = found->second;
        unit = &layout.units[entry.unitIndex];
        if (!entry.mutationIndex) {
          const auto *resolved = mutations.Find(
              paragraph.paragraphId, run.runId, begin, end);
          entry.mutationIndex =
              resolved ? static_cast<std::size_t>(resolved - mutations.data())
                       : mutations.size();
        }
        if (*entry.mutationIndex < mutations.size())
          mutation = &mutations[*entry.mutationIndex];
      }
      auto style = MakeBaseTextStyle(
          run.style, fonts, mutation, layout.shapingScale,
          run.locale.empty() ? paragraph.style.locale : run.locale,
          paragraph.style.lineHeight);
      style.setColor(SK_ColorTRANSPARENT);
      if (unit && PassTargetsUnit(pass, *unit)) {
        const auto *materialLayer =
            pass.paragraphId.empty()
                ? &pass.material
                : FindAuthoredGlyphLayer(run, pass.layerId);
        if (materialLayer) {
          SkPaint paint;
          std::string assetSample;
          if (captureOnly) {
            paint.setAntiAlias(true);
            paint.setColor(SK_ColorWHITE);
          } else {
            const auto coordinateBounds =
                unit->authoredBounds.isEmpty() ? layout.inkBounds
                                               : unit->authoredBounds;
            if (!ConfigureTextMaterialPaint(
                    GlyphLayerMaterial(*materialLayer), layout.logicalBounds,
                    layout.inkBounds, coordinateBounds, localTimeUs, assets,
                    paint, assetSample, error,
                    static_cast<std::size_t>(unit - layout.units.data()))) {
              return false;
            }
            ApplyLayerGeometry(*materialLayer, paint);
          }
          style.setForegroundPaint(std::move(paint));
          if (!assetSample.empty())
            assetSamples.push_back(std::move(assetSample));
        }
      }
      builder->pushStyle(style);
      if (mutation && mutation->replacementCodepoint &&
          *mutation->replacementCodepoint != 0U) {
        char replacement[SkUTF::kMaxBytesInUTF8Sequence]{};
        const auto byteCount = SkUTF::ToUTF8(
            static_cast<SkUnichar>(*mutation->replacementCodepoint),
            replacement);
        if (byteCount == 0U) {
          error = "text effect replacement codepoint is not encodable";
          return false;
        }
        builder->addText(replacement, byteCount);
      } else {
        builder->addText(run.utf8Text.data() + begin, end - begin);
      }
      builder->pop();
    }
  }
  output = builder->Build();
  if (!output) {
    error = "SkParagraph material-pass construction failed";
    return false;
  }
  const auto paragraphLayout = std::find_if(
      layout.paragraphs.begin(), layout.paragraphs.end(), [&](const auto &p) {
        return p.source && p.source->paragraphId == paragraph.paragraphId;
      });
  if (paragraphLayout == layout.paragraphs.end()) {
    error = "material pass lost its measured paragraph";
    return false;
  }
  fonts.LayoutParagraph(*output, paragraphLayout->width);
  return true;
}


void ApplyUnitTransforms(SkCanvas *canvas,
                         const text::TextEffectUnitFramePlan *plan) {
  if (!canvas || !plan)
    return;
  const auto transform = text::ComposeTextEffectTransforms(plan->transforms);
  if (transform)
    canvas->concat(SkM44::ColMajor(transform->columnMajor.data()));
}


int BeginUnitPaint(SkCanvas *canvas) {
  const int restoreCount = canvas->getSaveCount();
  canvas->save();
  return restoreCount;
}


SkRect SdfLineBoundsForUnit(const ResolvedLayout &layout,
                            const UnitGeometry &unit) noexcept {
  const auto lineIndex = VisualLineForUnit(layout, unit);
  if (lineIndex < layout.publicLayout.authoredLines.size()) {
    const auto line = ToSkRect(
        layout.publicLayout.authoredLines[lineIndex].bounds);
    if (!line.isEmpty())
      return line;
  }
  return unit.layoutBounds.isEmpty() ? unit.authoredBounds : unit.layoutBounds;
}


float StoreTextProBinary32(const float value) noexcept {
  volatile float stored = value;
  return stored;
}


float MultiplyTextProBinary32(const float left, const float right) noexcept {
  return StoreTextProBinary32(StoreTextProBinary32(left) *
                              StoreTextProBinary32(right));
}


float AddTextProBinary32(const float left, const float right) noexcept {
  return StoreTextProBinary32(StoreTextProBinary32(left) +
                              StoreTextProBinary32(right));
}


float SubtractTextProBinary32(const float left, const float right) noexcept {
  return StoreTextProBinary32(StoreTextProBinary32(left) -
                              StoreTextProBinary32(right));
}


float DivideTextProBinary32(const float numerator,
                            const float denominator) noexcept {
  return StoreTextProBinary32(StoreTextProBinary32(numerator) /
                              StoreTextProBinary32(denominator));
}


// Validate and freeze the typed packet before it is attached to a component.
// This is the current-contract replacement for the old selector-driven page
// builder: all glyph selection/atlas generation has already happened in the
// FramePlan/SDF pass, so this adapter only admits immutable pixel data.
std::optional<QtTextNativePageSource> BuildQtTextNativePageSource(
    std::vector<DeferredQtTextNativeLetterBatch> batches,
    const float referenceWidth, const float referenceHeight) {
  if (!std::isfinite(referenceWidth) || referenceWidth <= 0.0F ||
      !std::isfinite(referenceHeight) || referenceHeight <= 0.0F ||
      batches.empty()) {
    return std::nullopt;
  }
  QtTextNativePageSource source;
  source.batches.reserve(batches.size());
  for (auto &batch : batches) {
    if (!batch.atlasTexture || batch.atlasWidth <= 0 || batch.atlasHeight <= 0 ||
        batch.atlasWidth > 4096 || batch.atlasHeight > 4096 ||
        batch.atlasRowBytes < static_cast<std::size_t>(batch.atlasWidth) * 2U ||
        (!batch.atlasRg8MetalRows.empty() &&
         batch.atlasRg8MetalRows.size() !=
             batch.atlasRowBytes * static_cast<std::size_t>(batch.atlasHeight)) ||
        batch.vertices.empty() || batch.indices.empty() ||
        batch.vertices.size() % 4U != 0U || batch.indices.size() % 6U != 0U ||
        batch.unitStableIds.size() != batch.vertices.size() / 4U ||
        !batch.fixedLetterBounds.isFinite() ||
        (batch.fixedLetterBounds.isEmpty() && !batch.vertices.empty()) ||
        !std::isfinite(batch.referenceWidth) ||
        !std::isfinite(batch.referenceHeight) || batch.referenceWidth <= 0.0F ||
        batch.referenceHeight <= 0.0F ||
        !std::all_of(std::begin(batch.materialColor),
                     std::end(batch.materialColor), [](const float value) {
                       return std::isfinite(value) && value >= 0.0F &&
                              value <= 1.0F;
                     }) ||
        std::fabs(batch.referenceWidth - referenceWidth) > 1.0e-3F ||
        std::fabs(batch.referenceHeight - referenceHeight) > 1.0e-3F) {
      return std::nullopt;
    }
    for (const auto index : batch.indices) {
      if (static_cast<std::size_t>(index) >= batch.vertices.size())
        return std::nullopt;
    }
    for (const auto stableUnitId : batch.unitStableIds) {
      if (stableUnitId == 0U)
        return std::nullopt;
    }
    source.batches.push_back(std::move(batch));
  }
  return source;
}


TextProLetterMatrixF32
ToTextProLetterMatrixBinary32(const SkM44 &matrix) noexcept {
  float rowMajor[16];
  matrix.getRowMajor(rowMajor);
  TextProLetterMatrixF32 result;
  for (std::size_t row = 0U; row < 4U; ++row) {
    for (std::size_t column = 0U; column < 4U; ++column) {
      result.values[column * 4U + row] =
          StoreTextProBinary32(rowMajor[row * 4U + column]);
    }
  }
  return result;
}


bool RenderSdfParagraphMaterial(
    Paragraph &paragraph, const ResolvedGlyphMaterialPass &pass,
    const ResolvedLayout &layout, const ParagraphLayout &placement,
    const TextRenderFramePlan &renderPlan,
    const text::TextLayerAppearance &appearance,
    const float sourceToReferenceScale, const float referenceWidth,
    const float referenceHeight, const std::int64_t localTimeUs,
    const SkM44 &authoredToSurface,
    SkiaGpuContext &gpuContext, SkSurface &surface,
    TextVisualAssetStore &assets, std::vector<std::string> &assetSamples,
    QtTextNativePageSource *nativePageSource,
    SkRect &authoredMaterialBounds,
    std::string &error) {
  const auto &framePlan = renderPlan.value();
  TextGlyphOutlineCaptureRequest capture;
  capture.paragraph = &paragraph;
  capture.paragraphX = placement.x;
  capture.paragraphY = placement.y;
  capture.writingTransform = layout.writingTransform;
  std::vector<const UnitGeometry *> capturedUnits;
  for (const auto unitIndex : placement.unitIndexes) {
    const auto &unit = layout.units[unitIndex];
    if (!PassTargetsUnit(pass, unit))
      continue;
    capture.units.push_back(
        {unit.binding.stableUnitId, unit.binding.paragraphId,
         unit.binding.runId, unit.paragraphUtf8Begin, unit.paragraphUtf8End,
         unit.binding.utf8Begin, unit.binding.utf8End});
    capturedUnits.push_back(&unit);
  }
  std::vector<CapturedTextGlyphOutline> outlines;
  if (!CaptureTextGlyphOutlines(capture, outlines, error))
    return false;

  // The native Letter writer consumes the visitor's paragraph-local glyph
  // position directly.  Recover it before the capture bridge adds paragraph
  // placement and maps through reference space: adding then subtracting a
  // large placement changes the first glyph by an observable float32 ULP.
  // shapedRunIndex/glyphIndexInShapedRun are the stable association emitted
  // by CaptureTextGlyphOutlines, so this does not use geometry rebinding.
  std::vector<std::vector<SkPoint>> nativeGlyphPositions;
  auto nextOutline = outlines.begin();
  bool rasterGlyphFailed = false;
  paragraph.visit(
      [&](const int, const Paragraph::VisitorInfo *info) {
        if (!info)
          return;
        const auto runIndex = nativeGlyphPositions.size();
        std::vector<SkPoint> positions;
        if (info->count > 0 && info->positions &&
            std::isfinite(info->origin.x()) &&
            std::isfinite(info->origin.y())) {
          positions.reserve(static_cast<std::size_t>(info->count));
          for (int index = 0; index < info->count; ++index) {
            positions.push_back(SkPoint::Make(
                AddTextProBinary32(info->origin.x(),
                                   info->positions[index].x()),
                AddTextProBinary32(info->origin.y(),
                                   info->positions[index].y())));
            if (nextOutline != outlines.end() &&
                nextOutline->shapedRunIndex == runIndex &&
                nextOutline->glyphIndexInShapedRun ==
                    static_cast<std::size_t>(index)) {
              ++nextOutline;
              continue;
            }
            // Color/bitmap fonts have no vector outline. Keep their native
            // GPU glyph contribution instead of silently dropping them from
            // an otherwise SDF paragraph. Reuse the authored material and
            // per-unit transforms, opacity, and writing coordinates.
            if (!info->glyphs || !info->utf8Starts ||
                info->font.getSize() <= 0.0F ||
                info->font.getBounds(info->glyphs[index], nullptr).isEmpty())
              continue;
            const UnitGeometry *unit = nullptr;
            const auto cluster = info->utf8Starts[index];
            for (const auto *candidate : capturedUnits) {
              if (cluster >= candidate->paragraphUtf8Begin &&
                  cluster < candidate->paragraphUtf8End) {
                unit = candidate;
                break;
              }
            }
            if (!unit) continue;
            const auto *unitPlan =
                renderPlan.FindUnit(unit->binding.stableUnitId);
            SkPaint paint;
            std::string assetSample;
            if (!ConfigureTextMaterialPaint(
                    GlyphLayerMaterial(pass.material), layout.logicalBounds,
                    layout.inkBounds, unit->authoredBounds, localTimeUs,
                    assets, paint, assetSample, error,
                    static_cast<std::size_t>(unit - layout.units.data()))) {
              rasterGlyphFailed = true;
              continue;
            }
            ApplyLayerGeometry(pass.material, paint);
            if (unitPlan && unitPlan->opacity)
              paint.setAlphaf(paint.getAlphaf() *
                             std::clamp(*unitPlan->opacity, 0.0F, 1.0F));
            if (!assetSample.empty()) assetSamples.push_back(assetSample);
            auto *canvas = surface.getCanvas();
            const auto saved = BeginUnitPaint(canvas);
            canvas->concat(authoredToSurface);
            ApplyUnitTransforms(canvas, unitPlan);
            canvas->concat(layout.writingTransform);
            const auto origin = SkPoint::Make(
                placement.x + info->origin.x(), placement.y + info->origin.y());
            canvas->drawGlyphs({info->glyphs + index, 1U},
                               {info->positions + index, 1U}, origin,
                               info->font, paint);
            canvas->restoreToCount(saved);
            if (nativePageSource)
              nativePageSource->containsRasterGlyphs = true;
          }
        }
        nativeGlyphPositions.push_back(std::move(positions));
      });
  if (rasterGlyphFailed) return false;

  const float rasterDistanceRange =
      appearance.sdfMaterial.rasterDistanceRange;
  const float materialDistanceRange =
      appearance.sdfMaterial.distanceRange;
  if (!std::isfinite(rasterDistanceRange) || rasterDistanceRange <= 0.0F ||
      !std::isfinite(materialDistanceRange) ||
      materialDistanceRange <= 0.0F) {
    error = "text SDF appearance produced an invalid projected range";
    return false;
  }
  constexpr float kTextProSdfCanonicalEm = 150.0F;
  constexpr float kTextProLetterCoordinateExtent = 576.0F;
  constexpr int kTextProSdfAtlasCellGuard = 1;
  constexpr int kMaximumTextSdfAtlasDimension = 4096;
  constexpr std::size_t kMaximumTextSdfAtlasPixels = 16U * 1024U * 1024U;
  struct SharedAtlasCell final {
    int width{0};
    int height{0};
    float coordinateWidth{0.0F};
    float coordinateHeight{0.0F};
    int x{0};
    int y{0};
    SkPath generationOutline;
    SkRect generationInkBounds{SkRect::MakeEmpty()};
    float generationAtlasScale{1.0F};
    QtTextGlyphOutlineCoordinateSystem pathCoordinateSystem{
        QtTextGlyphOutlineCoordinateSystem::RendererYDown};
    TextSdfGlyphIdentity identity;
  };
  struct SharedAtlasDraw final {
    const CapturedTextGlyphOutline *outline{nullptr};
    const UnitGeometry *unit{nullptr};
    const text::TextEffectUnitFramePlan *unitPlan{nullptr};
    std::size_t unitIndex{0U};
    std::size_t cellIndex{0U};
    std::uint32_t materialIndex{0U};
    float opacity{1.0F};
    float authoredAtlasScale{1.0F};
    SkRect letterLocalRect{SkRect::MakeEmpty()};
    SkRect materialLocalRect{SkRect::MakeEmpty()};
  };
  std::vector<SharedAtlasCell> cells;
  std::vector<SharedAtlasDraw> draws;
  std::unordered_map<std::string, std::size_t> cellByGlyph;
  std::size_t totalCellPixels = 0U;
  // Capture already selected this pass's units. Keep equal IDs in placement
  // order so rebinding still chooses the first matching range.
  if (!outlines.empty()) {
    std::stable_sort(capturedUnits.begin(), capturedUnits.end(),
                     [](const auto *left, const auto *right) {
                       return left->binding.stableUnitId <
                              right->binding.stableUnitId;
                     });
  }
  for (const auto &outline : outlines) {
    const auto *unit = FindSdfUnitForOutline(capturedUnits, outline);
    if (!unit) {
      error = "shaped glyph could not be rebound to its text unit";
      return false;
    }
    const auto unitIndex =
        static_cast<std::size_t>(unit - layout.units.data());
    const auto *unitPlan =
        renderPlan.FindUnit(unit->binding.stableUnitId);
    // TextPro clamps a sampled negative absolute font size to zero and omits
    // that Letter before SDF packing.  Shaping the clamped zero through
    // SkParagraph produces a one-pixel placeholder which must not enter the
    // atlas, Page fixed bounds, or post-effect source domain.
    if (unitPlan && unitPlan->absoluteFontSize &&
        *unitPlan->absoluteFontSize <= 0.0F) {
      continue;
    }
    const float opacity =
        unitPlan && unitPlan->opacity
            ? std::max(0.0F, *unitPlan->opacity)
            : 1.0F;

    if (!std::isfinite(outline.shapedFontSize) ||
        outline.shapedFontSize <= 0.0F ||
        outline.nominalRecordPath.isEmpty() ||
        outline.nominalRecordBounds.isEmpty()) {
      continue;
    }
    const float authoredAtlasScale =
        kTextProSdfCanonicalEm / outline.shapedFontSize;
    const float generationAtlasScale = outline.nominalAtlasScale;
    const float canonicalInkWidth =
        outline.nominalRecordBounds.width() * generationAtlasScale;
    const float canonicalInkHeight =
        outline.nominalRecordBounds.height() * generationAtlasScale;
    const float coordinateWidth =
        (canonicalInkWidth + rasterDistanceRange) + rasterDistanceRange;
    const float coordinateHeight =
        (canonicalInkHeight + rasterDistanceRange) + rasterDistanceRange;
    const int atlasWidth =
        std::max(1, static_cast<int>(std::ceil(coordinateWidth)));
    const int atlasHeight =
        std::max(1, static_cast<int>(std::ceil(coordinateHeight)));
    const auto cellPixels = static_cast<std::size_t>(atlasWidth) *
                            static_cast<std::size_t>(atlasHeight);
    if (atlasWidth <= 0 || atlasHeight <= 0 ||
        atlasWidth > kMaximumTextSdfAtlasDimension ||
        atlasHeight > kMaximumTextSdfAtlasDimension ||
        cellPixels > kMaximumTextSdfAtlasPixels) {
      error = "text SDF glyph exceeds the shared atlas budget";
      return false;
    }

    IdentityBuilder cellIdentity;
    cellIdentity.AddString(outline.paragraphId);
    cellIdentity.AddString(outline.runId);
    cellIdentity.Add(outline.glyphId);
    cellIdentity.Add(outline.shapedFontSize);
    cellIdentity.Add(outline.nominalAtlasScale);
    cellIdentity.Add(outline.nominalCoordinateSystem);
    cellIdentity.Add(atlasWidth);
    cellIdentity.Add(atlasHeight);
    const auto cellKey = cellIdentity.Finish();
    auto foundCell = cellByGlyph.find(cellKey);
    std::size_t cellIndex = 0U;
    if (foundCell == cellByGlyph.end()) {
      totalCellPixels += cellPixels;
      if (totalCellPixels > kMaximumTextSdfAtlasPixels) {
        error = "text SDF glyph set exceeds the shared atlas budget";
        return false;
      }
      cellIndex = cells.size();
      cells.push_back(
          {atlasWidth,
           atlasHeight,
           coordinateWidth,
           coordinateHeight,
           0,
           0,
           outline.nominalRecordPath,
           outline.nominalRecordBounds,
           generationAtlasScale,
           outline.nominalCoordinateSystem,
           {outline.stableUnitId,
            outline.paragraphId,
            outline.runId,
            outline.paragraphUtf8Cluster,
            outline.runUtf8Cluster,
            outline.glyphIndexInCluster,
            static_cast<std::uint16_t>(outline.glyphId)}});
      cellByGlyph.emplace(cellKey, cellIndex);
    } else {
      cellIndex = foundCell->second;
    }

    if (draws.size() > std::numeric_limits<std::uint32_t>::max()) {
      error = "text SDF material index exceeds the native packet range";
      return false;
    }
    draws.push_back(
        {&outline, unit, unitPlan, unitIndex, cellIndex,
         static_cast<std::uint32_t>(draws.size()), opacity,
         authoredAtlasScale});
  }

  if (cells.empty())
    return true;

  const auto nextPowerOfTwo = [](const int value) {
    int result = 1;
    while (result < value && result < kMaximumTextSdfAtlasDimension)
      result *= 2;
    return result;
  };
  int maximumCellWidth = 1;
  double totalArea = 0.0;
  for (const auto &cell : cells) {
    maximumCellWidth = std::max(maximumCellWidth, cell.width);
    totalArea += static_cast<double>(cell.width) * cell.height;
  }
  int minimumAtlasWidth = nextPowerOfTwo(std::max(
      512, std::max(maximumCellWidth,
                    static_cast<int>(std::ceil(std::sqrt(totalArea))))));
  minimumAtlasWidth =
      std::min(minimumAtlasWidth, kMaximumTextSdfAtlasDimension);
  const auto packCells = [&](const int width) {
    int x = 0;
    int y = 0;
    int rowHeight = 0;
    for (auto &cell : cells) {
      if (cell.width > width)
        return -1;
      if (x > 0 && x + cell.width > width) {
        y += rowHeight;
        x = 0;
        rowHeight = 0;
      }
      if (y + cell.height > kMaximumTextSdfAtlasDimension)
        return -1;
      cell.x = x;
      cell.y = y;
      x += cell.width + kTextProSdfAtlasCellGuard;
      rowHeight = std::max(rowHeight, cell.height);
    }
    return y + rowHeight;
  };
  int atlasWidth = -1;
  int atlasHeight = -1;
  std::size_t bestArea = std::numeric_limits<std::size_t>::max();
  for (int candidateWidth = minimumAtlasWidth;;) {
    const int packedHeight = packCells(candidateWidth);
    if (packedHeight > 0) {
      const int candidateHeight =
          nextPowerOfTwo(std::max(512, packedHeight));
      const auto candidateArea = static_cast<std::size_t>(candidateWidth) *
                                 static_cast<std::size_t>(candidateHeight);
      if (candidateArea < bestArea ||
          (candidateArea == bestArea && candidateWidth > atlasWidth)) {
        atlasWidth = candidateWidth;
        atlasHeight = candidateHeight;
        bestArea = candidateArea;
      }
    }
    if (candidateWidth >= kMaximumTextSdfAtlasDimension)
      break;
    candidateWidth = std::min(candidateWidth * 2,
                              kMaximumTextSdfAtlasDimension);
  }
  if (atlasWidth <= 0 || atlasHeight <= 0 ||
      bestArea > kMaximumTextSdfAtlasPixels || packCells(atlasWidth) <= 0) {
    error = "text SDF glyphs do not fit the shared atlas";
    return false;
  }

  TextSdfGpuMesh sharedMesh;
  sharedMesh.targetWidth = atlasWidth;
  sharedMesh.targetHeight = atlasHeight;
  sharedMesh.outputX = 0;
  sharedMesh.outputY = 0;
  sharedMesh.outputWidth = atlasWidth;
  sharedMesh.outputHeight = atlasHeight;
  sharedMesh.glyphIdentity = cells.front().identity;
  const float generationPadding = std::floor(rasterDistanceRange);
  for (const auto &cell : cells) {
    float translateX =
        rasterDistanceRange + static_cast<float>(cell.x);
    translateX -=
        cell.generationInkBounds.left() * cell.generationAtlasScale;
    float translateY = static_cast<float>(cell.y);
    translateY -=
        cell.generationInkBounds.top() * cell.generationAtlasScale;
    translateY += rasterDistanceRange;
    if (std::getenv("VIDEOCUT_TRACE_TEXT_SDF_ATLAS_TRANSFORM") != nullptr) {
      std::fprintf(
          stderr,
          "[VIDEOCUT_TEXT_SDF_ATLAS_TRANSFORM] cell=[%d %d %d %d] "
          "ink=[%a %a %a %a] scale=%a padding=%a translate=[%a %a] "
          "raster_range=%a\n",
          cell.x, cell.y, cell.width, cell.height,
          static_cast<double>(cell.generationInkBounds.left()),
          static_cast<double>(cell.generationInkBounds.top()),
          static_cast<double>(cell.generationInkBounds.right()),
          static_cast<double>(cell.generationInkBounds.bottom()),
          static_cast<double>(cell.generationAtlasScale),
          static_cast<double>(generationPadding),
          static_cast<double>(translateX), static_cast<double>(translateY),
          static_cast<double>(rasterDistanceRange));
    }
    const auto atlasPath = ScaleTranslateTextSdfPathBinary32(
        cell.generationOutline, cell.generationAtlasScale,
        cell.generationAtlasScale, translateX, translateY);
    TextSdfGpuMesh cellMesh;
    const auto cellBounds = SkRect::MakeXYWH(
        static_cast<float>(cell.x), static_cast<float>(cell.y),
        cell.coordinateWidth, cell.coordinateHeight);
    if (!BuildTextSdfGpuMesh(
            {&atlasPath, cell.identity, cellBounds, atlasWidth, atlasHeight,
             rasterDistanceRange, cell.pathCoordinateSystem},
            cellMesh, error)) {
      return false;
    }
    sharedMesh.distanceVertices.insert(sharedMesh.distanceVertices.end(),
                                       cellMesh.distanceVertices.begin(),
                                       cellMesh.distanceVertices.end());
    sharedMesh.shapeVertices.insert(sharedMesh.shapeVertices.end(),
                                    cellMesh.shapeVertices.begin(),
                                    cellMesh.shapeVertices.end());
  }

  SkMatrix referenceToParagraph;
  if (!layout.writingTransform.invert(&referenceToParagraph)) {
    error = "text SDF writing transform is not invertible";
    return false;
  }

  constexpr float kTextProSdfNominalPointSize = 14.0F;
  constexpr float kTextProSdfMinimumPadding = 30.0F;
  // Standalone SDFText and legacy Text write the same native Letter extent
  // around different source-space origins. Preserve the creation component in
  // canonical state and apply its writer ABI here, before Letter rects are
  // constructed; the normal authored transform remains component-agnostic.
  constexpr float kSdfTextWriterOriginOffsetY = 1.5162F;
  const float writerOriginOffsetY =
      framePlan.sourceCreationComponent ==
              text::TextSourceCreationComponent::SdfText
          ? kSdfTextWriterOriginOffsetY
          : 0.0F;
  for (auto &draw : draws) {
    const auto &outline = *draw.outline;
    const auto &unit = *draw.unit;
    const auto glyphInParagraph =
        referenceToParagraph.mapPoint(outline.glyphOrigin);
    float glyphPositionX = SubtractTextProBinary32(
        glyphInParagraph.x(), placement.x);
    float glyphPositionY = SubtractTextProBinary32(
        glyphInParagraph.y(), placement.y);
    if (outline.shapedRunIndex < nativeGlyphPositions.size() &&
        outline.glyphIndexInShapedRun <
            nativeGlyphPositions[outline.shapedRunIndex].size()) {
      const auto native =
          nativeGlyphPositions[outline.shapedRunIndex]
                              [outline.glyphIndexInShapedRun];
      glyphPositionX = native.x();
      glyphPositionY = native.y();
    }
    const float nominalLeft = outline.nominalRecordBounds.left();
    const float nominalRight = outline.nominalRecordBounds.right();
    float nominalTop = outline.nominalRecordBounds.top();
    float nominalBottom = outline.nominalRecordBounds.bottom();
    if (outline.nominalCoordinateSystem ==
        QtTextGlyphOutlineCoordinateSystem::CoreTextYUp) {
      nominalTop = StoreTextProBinary32(-outline.nominalRecordBounds.bottom());
      nominalBottom = StoreTextProBinary32(-outline.nominalRecordBounds.top());
    }
    if (!(nominalRight > nominalLeft && nominalBottom > nominalTop)) {
      error = "text SDF nominal Letter bounds are invalid";
      return false;
    }
    const float metricHeight =
        std::max(1.0F, StoreTextProBinary32(outline.shapedFontSize));
    const float letterScale = DivideTextProBinary32(
        metricHeight, kTextProSdfNominalPointSize);
    const float rawX = AddTextProBinary32(
        glyphPositionX,
        MultiplyTextProBinary32(letterScale, nominalLeft));
    const float rawY = AddTextProBinary32(
        glyphPositionY,
        MultiplyTextProBinary32(letterScale, nominalTop));
    const float rawWidth = MultiplyTextProBinary32(
        letterScale, SubtractTextProBinary32(nominalRight, nominalLeft));
    const float rawHeight = MultiplyTextProBinary32(
        letterScale, SubtractTextProBinary32(nominalBottom, nominalTop));
    const float writerAtlasScale = DivideTextProBinary32(
        kTextProSdfCanonicalEm, metricHeight);
    const float writerPadding = DivideTextProBinary32(
        kTextProSdfMinimumPadding, writerAtlasScale);
    const float localLeft = AddTextProBinary32(
        AddTextProBinary32(placement.x, layout.fitWriterOffsetX),
        SubtractTextProBinary32(rawX, writerPadding));
    const float writerPlacementY =
        AddTextProBinary32(placement.y, writerOriginOffsetY);
    const float localTop = AddTextProBinary32(
        AddTextProBinary32(writerPlacementY,
                           SubtractTextProBinary32(rawY, writerPadding)),
        unit.visualReflowOffsetY);
    const float paddingTwice =
        AddTextProBinary32(writerPadding, writerPadding);
    const float localWidth = AddTextProBinary32(paddingTwice, rawWidth);
    const float localHeight = AddTextProBinary32(paddingTwice, rawHeight);
    draw.letterLocalRect = SkRect::MakeLTRB(
        localLeft, localTop, AddTextProBinary32(localLeft, localWidth),
        AddTextProBinary32(localTop, localHeight));
    const float materialPaddingDelta = DivideTextProBinary32(
        SubtractTextProBinary32(rasterDistanceRange,
                                kTextProSdfMinimumPadding),
        writerAtlasScale);
    draw.materialLocalRect = SkRect::MakeLTRB(
        SubtractTextProBinary32(draw.letterLocalRect.left(),
                                materialPaddingDelta),
        SubtractTextProBinary32(draw.letterLocalRect.top(),
                                materialPaddingDelta),
        AddTextProBinary32(draw.letterLocalRect.right(),
                           materialPaddingDelta),
        AddTextProBinary32(draw.letterLocalRect.bottom(),
                           materialPaddingDelta));
    authoredMaterialBounds.join(
        MapRect(layout.writingTransform, draw.materialLocalRect));
    if (std::getenv("VIDEOCUT_TRACE_QT_TEXT_LETTER_NATIVE") != nullptr) {
      std::fprintf(
          stderr,
          "[VIDEOCUT_QT_TEXT_LETTER_NATIVE] stage=typed_reference_writer "
          "paragraph=%s run=%s metric=%a position=[%a %a] nominal=[%a %a "
          "%a %a] scale=%a nominal_atlas_scale=%a padding=%a "
          "placement=[%a %a] reflow=%a "
          "rect=[%a %a %a %a]\n",
          outline.paragraphId.c_str(), outline.runId.c_str(),
          static_cast<double>(metricHeight),
          static_cast<double>(glyphPositionX),
          static_cast<double>(glyphPositionY),
          static_cast<double>(nominalLeft), static_cast<double>(nominalTop),
          static_cast<double>(nominalRight),
          static_cast<double>(nominalBottom),
          static_cast<double>(letterScale),
          static_cast<double>(outline.nominalAtlasScale),
          static_cast<double>(writerPadding),
          static_cast<double>(placement.x),
          static_cast<double>(placement.y),
          static_cast<double>(unit.visualReflowOffsetY),
          static_cast<double>(draw.letterLocalRect.left()),
          static_cast<double>(draw.letterLocalRect.top()),
          static_cast<double>(draw.letterLocalRect.right()),
          static_cast<double>(draw.letterLocalRect.bottom()));
    }
  }

  // The verified TextPro base-fill Page path does not use the generic SDF
  // material program.  It transports the shared RG8 atlas into the captured
  // Letter shader, whose hardware-linear sample and hardware fwidth are
  // observable at glyph edges.  Keep the typed pass/frame-plan as the source
  // of geometry and animation, and use the native packet as its thin pixel
  // executor whenever the material contract is the base solid fill.
  const auto *fillLayer = std::get_if<text::TextFillLayer>(&pass.material);
  const text::SolidTextMaterial *solidMaterial = nullptr;
  const bool hasPageRenderGroup =
      nativePageSource != nullptr &&
      std::any_of(framePlan.postEffectNodes.begin(),
                  framePlan.postEffectNodes.end(), [](const auto &node) {
                    return node.renderGroup.has_value();
                  });
  // The same native Letter packet is also the no-post-effect Bubble source.
  // In that case it is replayed once at presentation resolution by the final
  // compositor; keeping the source deferred avoids an intermediate imageRect
  // resample while preserving the regular component contract.
  const bool nativeLetterSourceRequested =
      nativePageSource != nullptr &&
      (framePlan.postEffectNodes.empty() || hasPageRenderGroup);
  // The captured base-fill Letter packet maps a glyph cell onto the writer's
  // fixed 30-pixel inset.  A package may request a wider raster distance
  // range for strokes and shadows; mapping that wider cell onto the fixed
  // Letter quad compresses the zero contour and makes only the innermost fill
  // visibly smaller.  The generic material lane below maps the full raster
  // range through materialLocalRect and is the correct executor in that case.
  constexpr float kNativeLetterRasterRangeTolerance = 0.0001F;
  const bool nativeLetterRasterContract =
      std::fabs(rasterDistanceRange - kTextProSdfMinimumPadding) <=
      kNativeLetterRasterRangeTolerance;
  bool nativeSolidEligible = nativeLetterSourceRequested &&
                             nativeLetterRasterContract &&
                             fillLayer != nullptr;
  const auto nativeRendered = [&]() -> bool {
    DeferredQtTextNativeBatchCheckpoint checkpoint{
        nativePageSource,
        nativePageSource ? nativePageSource->batches.size() : 0U,
        false};
    if (nativeSolidEligible) {
    const auto &binding = fillLayer->material;
    if (const auto *slot =
            std::get_if<text::EditableTextStyleSlot>(&binding);
        slot && slot->replacementMask) {
      nativeSolidEligible = false;
    } else {
      solidMaterial =
          std::get_if<text::SolidTextMaterial>(&ResolveTextMaterial(binding));
      nativeSolidEligible = solidMaterial != nullptr;
    }
  }
  for (const auto &draw : draws) {
    if (!nativeSolidEligible)
      break;
    if (draw.unitPlan && draw.unitPlan->sdfBlurRadius &&
        *draw.unitPlan->sdfBlurRadius > 0.001F) {
      nativeSolidEligible = false;
    }
  }

  float presentation[16];
  authoredToSurface.getRowMajor(presentation);
  if (nativeSolidEligible) {
    constexpr float kAffineTolerance = 0.000001F;
    nativeSolidEligible =
        std::fabs(presentation[8]) <= kAffineTolerance &&
        std::fabs(presentation[9]) <= kAffineTolerance &&
        std::fabs(presentation[11]) <= kAffineTolerance &&
        std::fabs(presentation[12]) <= kAffineTolerance &&
        std::fabs(presentation[13]) <= kAffineTolerance &&
        std::fabs(presentation[14]) <= kAffineTolerance &&
        std::fabs(presentation[15] - 1.0F) <= kAffineTolerance &&
        std::isfinite(sourceToReferenceScale) &&
        sourceToReferenceScale > 0.0F &&
        std::isfinite(referenceWidth) && referenceWidth > 0.0F &&
        std::isfinite(referenceHeight) && referenceHeight > 0.0F &&
        surface.width() > 0 && surface.height() > 0;
  }

  if (nativeSolidEligible) {
    std::vector<SkIRect> atlasCells;
    atlasCells.reserve(cells.size());
    for (const auto &cell : cells) {
      atlasCells.push_back(SkIRect::MakeXYWH(
          cell.x, cell.y, cell.width, cell.height));
    }
    const std::size_t nativeAtlasRowBytes =
        static_cast<std::size_t>(atlasWidth) * 2U;
    std::vector<std::uint8_t> nativeAtlas;
    const auto diagnosticEnabled = [](const char *name) {
      const char *value = std::getenv(name);
      return value != nullptr && value[0] != '\0';
    };
    const bool diagnosticAtlas =
        diagnosticEnabled("VIDEOCUT_DUMP_QT_TEXT_LETTER_PACKET_DIR") ||
        diagnosticEnabled("VIDEOCUT_DUMP_QT_TEXT_LETTER_PACKET");
    auto atlasTexture = gpuContext.RenderTextSdfAtlas(
        sharedMesh, atlasCells, error, diagnosticAtlas ? &nativeAtlas : nullptr);
    if (!atlasTexture)
      return false;

    std::vector<QtTextTypedLetterVertex> vertices;
    std::vector<std::uint16_t> indices;
    std::vector<std::uint64_t> unitStableIds;
    if (draws.size() >
        static_cast<std::size_t>(
            std::numeric_limits<std::uint16_t>::max() + 1U) /
            4U) {
      error = "TextPro native Letter batch exceeds the uint16 index range";
      return false;
    }
    vertices.reserve(draws.size() * 4U);
    indices.reserve(draws.size() * 6U);
    unitStableIds.reserve(draws.size());
    const float inverseAtlasWidth = DivideTextProBinary32(
        1.0F, static_cast<float>(atlasWidth));
    const float inverseAtlasHeight = DivideTextProBinary32(
        1.0F, static_cast<float>(atlasHeight));
    const float centerX = DivideTextProBinary32(referenceWidth, 2.0F);
    const float centerY = DivideTextProBinary32(referenceHeight, 2.0F);
    const auto colorComponent = [](const float value) {
      return StoreTextProBinary32(std::clamp(value, 0.0F, 1.0F));
    };
    const auto instanceAlphaComponent = [](const float value) {
      // Qt's TextPro Letter packet carries animator alpha as raw float32.
      // Overshoot is intentionally preserved until the RGBA8 target write.
      return StoreTextProBinary32(std::max(0.0F, value));
    };
    const bool usesPerLetterInstanceColor = std::any_of(
        draws.begin(), draws.end(), [](const SharedAtlasDraw &draw) {
          return draw.unitPlan && draw.unitPlan->instanceColor.has_value();
        });
    bool validNativeGeometry = true;
    for (const auto &draw : draws) {
      if (draw.opacity <= 0.0F)
        continue;
      const auto &cell = cells[draw.cellIndex];
      SkM44 unitTransform;
      if (draw.unitPlan && !draw.unitPlan->transforms.empty()) {
        const auto composed =
            text::ComposeTextEffectTransforms(draw.unitPlan->transforms);
        if (!composed) {
          validNativeGeometry = false;
          break;
        }
        unitTransform = SkM44::ColMajor(composed->columnMajor.data());
      }
      const auto letterTransform =
          ToTextProLetterMatrixBinary32(unitTransform);
      auto lineBounds = SdfLineBoundsForUnit(layout, *draw.unit);
      lineBounds.offset(layout.fitWriterOffsetX, 0.0F);
      const auto visualLine = VisualLineForUnit(layout, *draw.unit);
      SkRect tightLineBounds = SkRect::MakeEmpty();
      for (const auto &candidate : layout.units) {
        if (VisualLineForUnit(layout, candidate) == visualLine)
          tightLineBounds.join(candidate.uniformTightAnchorBounds);
      }
      if (tightLineBounds.isEmpty())
        tightLineBounds = lineBounds;
      else
        tightLineBounds.offset(layout.fitWriterOffsetX, 0.0F);
      const std::array<float, 4> qtLineRect{
          DivideTextProBinary32(
              SubtractTextProBinary32(tightLineBounds.left(), centerX),
              sourceToReferenceScale),
          DivideTextProBinary32(
              SubtractTextProBinary32(tightLineBounds.top(), centerY),
              sourceToReferenceScale),
          DivideTextProBinary32(tightLineBounds.width(),
                                sourceToReferenceScale),
          DivideTextProBinary32(tightLineBounds.height(),
                                sourceToReferenceScale),
      };
      if (std::getenv("VIDEOCUT_TRACE_QT_TEXT_LETTER_NATIVE") != nullptr) {
        std::fprintf(
            stderr,
            "[VIDEOCUT_QT_TEXT_LETTER_NATIVE] stage=typed_line_rect "
            "source_scale=%a line=[%a %a %a %a] tight=[%a %a %a %a]\n",
            static_cast<double>(sourceToReferenceScale),
            static_cast<double>(lineBounds.left()),
            static_cast<double>(lineBounds.top()),
            static_cast<double>(lineBounds.width()),
            static_cast<double>(lineBounds.height()),
            static_cast<double>(tightLineBounds.left()),
            static_cast<double>(tightLineBounds.top()),
            static_cast<double>(tightLineBounds.width()),
            static_cast<double>(tightLineBounds.height()));
      }
      const float u0 = MultiplyTextProBinary32(
          static_cast<float>(cell.x), inverseAtlasWidth);
      const float u1 = MultiplyTextProBinary32(
          static_cast<float>(cell.x) + cell.coordinateWidth,
          inverseAtlasWidth);
      const float v0 = MultiplyTextProBinary32(
          static_cast<float>(cell.y), inverseAtlasHeight);
      const float v1 = MultiplyTextProBinary32(
          static_cast<float>(cell.y) + cell.coordinateHeight,
          inverseAtlasHeight);
      const std::array<SkPoint, 4> corners{
          SkPoint::Make(draw.letterLocalRect.left(),
                        draw.letterLocalRect.top()),
          SkPoint::Make(draw.letterLocalRect.right(),
                        draw.letterLocalRect.top()),
          SkPoint::Make(draw.letterLocalRect.right(),
                        draw.letterLocalRect.bottom()),
          SkPoint::Make(draw.letterLocalRect.left(),
                        draw.letterLocalRect.bottom()),
      };
      const std::array<std::array<float, 2>, 4> texcoords{
          std::array<float, 2>{u0, v1}, std::array<float, 2>{u1, v1},
          std::array<float, 2>{u1, v0}, std::array<float, 2>{u0, v0}};
      text::Color instanceTint{1.0F, 1.0F, 1.0F, draw.opacity};
      if (usesPerLetterInstanceColor) {
        // The batch material becomes white when any letter supplies a color.
        // Unselected letters must carry the original material color themselves.
        instanceTint = draw.unitPlan && draw.unitPlan->instanceColor
                           ? *draw.unitPlan->instanceColor
                           : solidMaterial->color;
        instanceTint.alpha = MultiplyTextProBinary32(instanceTint.alpha,
                                                     draw.opacity);
      }

      const auto vertexBase =
          static_cast<std::uint16_t>(vertices.size());
      std::array<QtTextTypedLetterVertex, 4> glyphVertices{};
      for (std::size_t corner = 0U; corner < corners.size(); ++corner) {
        const auto mapped = TransformTextProLetterPointF32(
            letterTransform,
            {corners[corner].x(), corners[corner].y(), 0.0F});
        if (!mapped) {
          validNativeGeometry = false;
          break;
        }
        auto &vertex = glyphVertices[corner];
        vertex.position[0] = DivideTextProBinary32(
            SubtractTextProBinary32(mapped->x, centerX),
            kTextProLetterCoordinateExtent);
        vertex.position[1] = DivideTextProBinary32(
            SubtractTextProBinary32(centerY, mapped->y),
            kTextProLetterCoordinateExtent);
        vertex.position[2] = DivideTextProBinary32(
            mapped->z, kTextProLetterCoordinateExtent);
        vertex.instanceColor[0] = colorComponent(instanceTint.red);
        vertex.instanceColor[1] = colorComponent(instanceTint.green);
        vertex.instanceColor[2] = colorComponent(instanceTint.blue);
        vertex.instanceColor[3] = instanceAlphaComponent(instanceTint.alpha);
        std::copy(qtLineRect.begin(), qtLineRect.end(), vertex.lineRect);
        vertex.sdfTexcoord[0] = texcoords[corner][0];
        vertex.sdfTexcoord[1] = texcoords[corner][1];
        vertex.sdfTexcoordMinMax[0] = u0;
        vertex.sdfTexcoordMinMax[1] = v0;
        vertex.sdfTexcoordMinMax[2] = u1;
        vertex.sdfTexcoordMinMax[3] = v1;
        // TODO(refactor:R2): source smooth-x varies with font size; defer
        // because the base-fill shader does not consume this component.
        vertex.smoothBoldIndex[0] = 0.0125F;
        vertex.smoothBoldIndex[2] =
            static_cast<float>(draw.materialIndex) + 0.000001F;
      }
      if (!validNativeGeometry)
        break;
      vertices.insert(vertices.end(), glyphVertices.begin(),
                      glyphVertices.end());
      // The quad is appended only after the geometry validation above, so the
      // metadata remains exactly aligned with the native vertex packet.
      unitStableIds.push_back(draw.unit->binding.stableUnitId);
      const std::array<std::uint16_t, 6> glyphIndices{
          vertexBase,
          static_cast<std::uint16_t>(vertexBase + 2U),
          static_cast<std::uint16_t>(vertexBase + 1U),
          vertexBase,
          static_cast<std::uint16_t>(vertexBase + 3U),
          static_cast<std::uint16_t>(vertexBase + 2U)};
      indices.insert(indices.end(), glyphIndices.begin(), glyphIndices.end());
    }

    if (validNativeGeometry && !vertices.empty()) {
      const char *nativePacketDirectory =
          std::getenv("VIDEOCUT_DUMP_QT_TEXT_LETTER_PACKET_DIR");
      const char *nativePacketTimeFilter =
          std::getenv("VIDEOCUT_DUMP_QT_TEXT_LETTER_PACKET_TIME_US");
      bool nativePacketTimeMatches = true;
      if (nativePacketTimeFilter != nullptr &&
          nativePacketTimeFilter[0] != '\0') {
        char *end = nullptr;
        const auto requestedTime =
            std::strtoll(nativePacketTimeFilter, &end, 10);
        nativePacketTimeMatches =
            end != nativePacketTimeFilter && end != nullptr && *end == '\0' &&
            requestedTime == localTimeUs;
      }
      static unsigned nativePacketOrdinal = 0U;
      if (nativePacketDirectory != nullptr && nativePacketDirectory[0] != '\0' &&
          nativePacketTimeMatches && nativePacketOrdinal < 2U) {
        const unsigned ordinal = nativePacketOrdinal++;
        const auto writePacket = [&](const char *kind, const void *bytes,
                                     const std::size_t size) {
          char path[4096]{};
          const int length = std::snprintf(
              path, sizeof(path), "%s/qt-text-letter-%s-%u.bin",
              nativePacketDirectory, kind, ordinal);
          if (length <= 0 || static_cast<std::size_t>(length) >= sizeof(path))
            return;
          if (FILE *file = std::fopen(path, "wb")) {
            std::fwrite(bytes, 1U, size, file);
            std::fclose(file);
          }
        };
        writePacket("atlas-rg8", nativeAtlas.data(), nativeAtlas.size());
        writePacket("vertices-stride108", vertices.data(),
                    vertices.size() * sizeof(QtTextTypedLetterVertex));
        writePacket("indices-u16", indices.data(),
                    indices.size() * sizeof(std::uint16_t));
      }
      if (nativePageSource) {
        DeferredQtTextNativeLetterBatch batch;
        batch.atlasTexture = atlasTexture;
        batch.atlasRg8MetalRows = nativeAtlas;
        batch.atlasWidth = atlasWidth;
        batch.atlasHeight = atlasHeight;
        batch.atlasRowBytes = nativeAtlasRowBytes;
        batch.vertices = vertices;
        batch.indices = indices;
        batch.unitStableIds = unitStableIds;
        const text::Color materialColor =
            usesPerLetterInstanceColor
                ? text::Color{1.0F, 1.0F, 1.0F, 1.0F}
                : solidMaterial->color;
        batch.materialColor[0] = colorComponent(materialColor.red);
        batch.materialColor[1] = colorComponent(materialColor.green);
        batch.materialColor[2] = colorComponent(materialColor.blue);
        batch.materialColor[3] = colorComponent(materialColor.alpha);
        batch.referenceWidth = referenceWidth;
        batch.referenceHeight = referenceHeight;
        batch.sampledTimeUs = localTimeUs;
        for (const auto &draw : draws)
          batch.fixedLetterBounds.join(draw.letterLocalRect);
        auto built = BuildQtTextNativePageSource(
            std::vector<DeferredQtTextNativeLetterBatch>{std::move(batch)},
            referenceWidth, referenceHeight);
        if (!built) {
          // Keep the portable path alive when packet validation rejects an
          // optional native batch (for example a device-limit mismatch).
          return false;
        } else {
          auto &builtBatches = built->batches;
          nativePageSource->batches.insert(
              nativePageSource->batches.end(),
              std::make_move_iterator(builtBatches.begin()),
              std::make_move_iterator(builtBatches.end()));
        }
      }
      QtTextLetterRenderRequest request;
      request.atlas = {nativeAtlas.data(), nativeAtlas.size(), atlasWidth,
                       atlasHeight, nativeAtlasRowBytes, atlasTexture};
      request.vertices = {
          reinterpret_cast<const std::uint8_t *>(vertices.data()),
          vertices.size() * sizeof(QtTextTypedLetterVertex)};
      request.vertexCount = vertices.size();
      request.indices = indices.data();
      request.indexCount = indices.size();
      request.outputWidth = surface.width();
      request.outputHeight = surface.height();
      request.uniforms.offsetInfo[1] = -0.7853981852531433F;
      const text::Color materialColor =
          usesPerLetterInstanceColor
              ? text::Color{1.0F, 1.0F, 1.0F, 1.0F}
              : solidMaterial->color;
      request.uniforms.fillColor[0] = colorComponent(materialColor.red);
      request.uniforms.fillColor[1] = colorComponent(materialColor.green);
      request.uniforms.fillColor[2] = colorComponent(materialColor.blue);
      request.uniforms.fillColor[3] = colorComponent(materialColor.alpha);

      const float a = presentation[0];
      const float b = presentation[1];
      const float c = presentation[4];
      const float d = presentation[5];
      const float translateX = presentation[3];
      const float translateY = presentation[7];
      const float xFactor =
          2.0F / static_cast<float>(request.outputWidth);
      const float yFactor =
          2.0F / static_cast<float>(request.outputHeight);
      std::fill(std::begin(request.uniforms.mvp),
                std::end(request.uniforms.mvp), 0.0F);
      request.uniforms.mvp[0] =
          xFactor * a * kTextProLetterCoordinateExtent;
      request.uniforms.mvp[1] =
          c == 0.0F ? 0.0F
                    : -yFactor * c * kTextProLetterCoordinateExtent;
      request.uniforms.mvp[4] =
          b == 0.0F ? 0.0F
                    : -xFactor * b * kTextProLetterCoordinateExtent;
      request.uniforms.mvp[5] =
          yFactor * d * kTextProLetterCoordinateExtent;
      request.uniforms.mvp[10] = -1.0F / 550.0F;
      request.uniforms.mvp[12] =
          xFactor * (a * centerX + b * centerY + translateX) - 1.0F;
      request.uniforms.mvp[13] =
          1.0F - yFactor * (c * centerX + d * centerY + translateY);
      request.uniforms.mvp[14] = -0.8F;
      request.uniforms.mvp[15] = 1.0F;

      struct RuntimeHolder final {
        std::unique_ptr<QtTextLetterMetalRuntime> runtime;
        std::string creationError;
      };
      static RuntimeHolder runtimeHolder = [] {
        RuntimeHolder holder;
        holder.runtime =
            CreateQtTextLetterMetalRuntime(holder.creationError);
        return holder;
      }();
      if (runtimeHolder.runtime) {
        auto image = gpuContext.RenderQtTextLetter(
            *runtimeHolder.runtime, request, nullptr, error);
        if (!image) {
          if (error.empty())
            error = "Qt TextPro native Letter image publication failed";
          return false;
        }
        const auto localBounds = SkRect::MakeWH(
            static_cast<float>(request.outputWidth),
            static_cast<float>(request.outputHeight));
        surface.getCanvas()->drawImageRect(
            image.get(), localBounds,
            SkSamplingOptions(SkFilterMode::kNearest), nullptr);
        TextMaterialExecutionScope::RecordLayer(
            pass.material, appearance.sdfMaterial, pass.nestedShadowStroke,
            "qt-letter-metal");
        if (std::getenv("VIDEOCUT_TRACE_QT_TEXT_LETTER_NATIVE") != nullptr) {
          std::fprintf(
              stderr,
              "[VIDEOCUT_QT_TEXT_LETTER_NATIVE] stage=typed_rendered "
              "glyphs=%zu vertices=%zu indices=%zu atlas=%dx%d "
              "target=%dx%d matrix=[%a %a %a %a %a %a] "
              "source_scale=%a backend=%s\n",
              draws.size(), vertices.size(), indices.size(), atlasWidth,
              atlasHeight, request.outputWidth, request.outputHeight,
              static_cast<double>(a), static_cast<double>(b),
              static_cast<double>(c), static_cast<double>(d),
              static_cast<double>(translateX),
              static_cast<double>(translateY),
              static_cast<double>(sourceToReferenceScale),
              runtimeHolder.runtime->BackendName());
        }
        checkpoint.committed = true;
        return true;
      }
    }
  }
  return false;
  }();
  if (nativeRendered)
    return true;
  // A native Letter runtime/pixel readback failure must not turn the whole
  // text pass into an empty picture.  The generic SDF path below is the
  // compatibility executor and will report its own error if it also cannot
  // render.
  error.clear();

  const auto *outerShadow =
      std::get_if<text::TextShadowLayer>(&pass.material);
  const auto *shadowStroke =
      pass.nestedShadowStroke
          ? std::get_if<text::TextStrokeLayer>(&pass.material)
          : nullptr;
  // A shadow effect-style owns one mask for the complete selected glyph set.
  // Isolate each Letter contribution from the loaded material target, then
  // merge the batch once in authored order.  This keeps a later Letter's
  // transparent texture/soft-mask fringe from replacing coverage already
  // contributed by an earlier Letter at a large-spread overlap.
  const bool uniteShadowMask =
      draws.size() > 1U &&
      ((outerShadow && outerShadow->kind == text::TextShadowKind::Outer &&
        outerShadow->spread > 0.0F) ||
       (shadowStroke &&
        (shadowStroke->spread > 0.0F || shadowStroke->width > 0.0F)));
  struct UnitedShadowGlyphTextures final {
    sk_sp<SkImage> material;
    sk_sp<SkImage> mask;
  };
  std::vector<UnitedShadowGlyphTextures> unitedShadowGlyphs;
  if (uniteShadowMask)
    unitedShadowGlyphs.reserve(draws.size());
  for (const auto &draw : draws) {
    if (draw.opacity <= 0.0F)
      continue;
    const auto &outline = *draw.outline;
    const auto &unit = *draw.unit;
    const auto *unitPlan = draw.unitPlan;
    const auto &cell = cells[draw.cellIndex];

    auto material = pass.material;
    ClearLegacySdfCoordinateOutset(material);
    sk_sp<SkImage> texture;
    sk_sp<SkImage> replacementMask;
    if (!ResolveSdfMaterialImages(
            material, draw.unitIndex, localTimeUs, assets, texture,
            replacementMask, assetSamples, error)) {
      return false;
    }

    SkM44 unitTransform;
    if (unitPlan && !unitPlan->transforms.empty()) {
      const auto composed =
          text::ComposeTextEffectTransforms(unitPlan->transforms);
      if (!composed) {
        error = "text SDF unit transform could not be composed";
        return false;
      }
      unitTransform = SkM44::ColMajor(composed->columnMajor.data());
    }
    const SkM44 localToSurface = authoredToSurface * unitTransform;
    TextSdfMaterialGpuRequest request;
    request.mesh = sharedMesh;
    request.glyphAtlasRect = {
        static_cast<float>(cell.x), static_cast<float>(cell.y),
        cell.coordinateWidth, cell.coordinateHeight};
    request.glyphLocalRect = {
        draw.materialLocalRect.left(), draw.materialLocalRect.top(),
        draw.materialLocalRect.width(), draw.materialLocalRect.height()};
    request.glyphMaterialRect = request.glyphLocalRect;
    request.lineRect = ToTextSdfRect(SdfLineBoundsForUnit(layout, unit),
                                    0.0F, 0.0F);
    request.textRect = ToTextSdfRect(
        SdfTextCoordinateBounds(material, layout, placement), 0.0F, 0.0F);
    request.replacementMaskRect = request.glyphMaterialRect;
    request.coordinateDomain = SdfCoordinateDomain(material);
    request.materialIndex = draw.materialIndex;
    localToSurface.getColMajor(request.localToPresentation.data());
    const float centerX = DivideTextProBinary32(referenceWidth, 2.0F);
    const float centerY = DivideTextProBinary32(referenceHeight, 2.0F);
    const std::array<SkPoint, 4> qtLetterCorners{
        SkPoint::Make(draw.materialLocalRect.left(),
                      draw.materialLocalRect.top()),
        SkPoint::Make(draw.materialLocalRect.right(),
                      draw.materialLocalRect.top()),
        SkPoint::Make(draw.materialLocalRect.right(),
                      draw.materialLocalRect.bottom()),
        SkPoint::Make(draw.materialLocalRect.left(),
                      draw.materialLocalRect.bottom()),
    };
    for (std::size_t corner = 0U; corner < qtLetterCorners.size(); ++corner) {
      request.qtLetterPositions[corner * 4U] = DivideTextProBinary32(
          SubtractTextProBinary32(qtLetterCorners[corner].x(), centerX),
          kTextProLetterCoordinateExtent);
      request.qtLetterPositions[corner * 4U + 1U] = DivideTextProBinary32(
          SubtractTextProBinary32(centerY, qtLetterCorners[corner].y()),
          kTextProLetterCoordinateExtent);
      request.qtLetterPositions[corner * 4U + 2U] = 0.0F;
      request.qtLetterPositions[corner * 4U + 3U] = 1.0F;
    }
    float qtPresentation[16];
    localToSurface.getRowMajor(qtPresentation);
    sk_sp<SkSurface> isolatedShadowSurface;
    SkSurface *materialSurface = &surface;
    if (uniteShadowMask) {
      const auto targetInfo = SkImageInfo::Make(
          surface.width(), surface.height(), kRGBA_8888_SkColorType,
          kPremul_SkAlphaType, SkColorSpace::MakeSRGB());
      isolatedShadowSurface = gpuContext.MakeSurface(targetInfo, error);
      if (!isolatedShadowSurface)
        return false;
      isolatedShadowSurface->getCanvas()->clear(SK_ColorTRANSPARENT);
      materialSurface = isolatedShadowSurface.get();
    }
    const float xFactor =
        2.0F / static_cast<float>(materialSurface->width());
    const float yFactor =
        2.0F / static_cast<float>(materialSurface->height());
    request.qtLetterMvp.fill(0.0F);
    request.qtLetterMvp[0] =
        xFactor * qtPresentation[0] * kTextProLetterCoordinateExtent;
    request.qtLetterMvp[1] =
        qtPresentation[4] == 0.0F
            ? 0.0F
            : -yFactor * qtPresentation[4] *
                  kTextProLetterCoordinateExtent;
    request.qtLetterMvp[4] =
        qtPresentation[1] == 0.0F
            ? 0.0F
            : -xFactor * qtPresentation[1] *
                  kTextProLetterCoordinateExtent;
    request.qtLetterMvp[5] =
        yFactor * qtPresentation[5] * kTextProLetterCoordinateExtent;
    request.qtLetterMvp[10] = -1.0F / 550.0F;
    request.qtLetterMvp[12] =
        xFactor * (qtPresentation[0] * centerX +
                   qtPresentation[1] * centerY + qtPresentation[3]) -
        1.0F;
    request.qtLetterMvp[13] =
        1.0F - yFactor * (qtPresentation[4] * centerX +
                          qtPresentation[5] * centerY + qtPresentation[7]);
    request.qtLetterMvp[14] = -0.8F;
    request.qtLetterMvp[15] = 1.0F;
    std::visit(
        [&](const auto &layer) {
          using Layer = std::decay_t<decltype(layer)>;
          float offsetX = 0.0F;
          float offsetY = 0.0F;
          if constexpr (std::is_same_v<Layer, text::TextFillLayer>) {
            offsetX = layer.offsetX;
            offsetY = layer.offsetY;
            if (layer.normalizedPolarOffset) {
              request.qtLetterPolarOffset = {
                  layer.normalizedPolarOffset->radius,
                  layer.normalizedPolarOffset->angleRadians};
              request.qtLetterPolarOffsetEnabled = true;
            }
          } else if constexpr (std::is_same_v<Layer,
                                               text::TextStrokeLayer>) {
            offsetX = layer.offsetX;
            offsetY = layer.offsetY;
            if (layer.normalizedPolarOffset) {
              request.qtLetterPolarOffset = {
                  layer.normalizedPolarOffset->radius,
                  layer.normalizedPolarOffset->angleRadians};
              request.qtLetterPolarOffsetEnabled = true;
            }
          } else if constexpr (std::is_same_v<Layer,
                                               text::TextShadowLayer>) {
            if (layer.kind != text::TextShadowKind::Inner) {
              offsetX = layer.offsetX;
              offsetY = layer.offsetY;
              if (layer.normalizedPolarOffset) {
                request.qtLetterPolarOffset = {
                    layer.normalizedPolarOffset->radius,
                    layer.normalizedPolarOffset->angleRadians};
                request.qtLetterPolarOffsetEnabled = true;
              }
            } else if (layer.normalizedUvOffset) {
              request.qtInnerShadowUvOffset = {
                  layer.normalizedUvOffset->x,
                  layer.normalizedUvOffset->y};
              request.qtInnerShadowUvOffsetEnabled = true;
            }
          } else if constexpr (std::is_same_v<Layer, text::TextGlowLayer>) {
            offsetX = layer.directionX * layer.radius;
            offsetY = layer.directionY * layer.radius;
            if (layer.normalizedPolarOffset) {
              request.qtLetterPolarOffset = {
                  layer.normalizedPolarOffset->radius,
                  layer.normalizedPolarOffset->angleRadians};
              request.qtLetterPolarOffsetEnabled = true;
            }
          }
          request.qtLetterOffset[0] = DivideTextProBinary32(
              offsetX, kTextProLetterCoordinateExtent);
          request.qtLetterOffset[1] = DivideTextProBinary32(
              -offsetY, kTextProLetterCoordinateExtent);
        },
        pass.material);
    request.material = std::move(material);
    request.texture = std::move(texture);
    request.replacementMask = std::move(replacementMask);
    request.authoredAtlasScale = draw.authoredAtlasScale;
    request.materialCoordinateExtent =
        kTextProSdfCanonicalEm + rasterDistanceRange * 2.0F;
    request.materialDistanceRange = materialDistanceRange;
    request.smoothingScale = appearance.sdfMaterial.smoothingScale;
    request.sdfBlurRadius =
        unitPlan && unitPlan->sdfBlurRadius
            ? std::max(0.0F, *unitPlan->sdfBlurRadius)
            : 0.0F;
    request.presentationOpacity = draw.opacity;
    if (unitPlan && unitPlan->instanceColor)
      request.presentationTint = *unitPlan->instanceColor;
    request.presentationBlend = text::TextBlendMode::SourceOver;
    if (!gpuContext.RenderTextSdfMaterial(request, *materialSurface, error))
      return false;
    TextMaterialExecutionScope::RecordLayer(
        request.material, appearance.sdfMaterial, pass.nestedShadowStroke);
    if (isolatedShadowSurface) {
      const auto pixelInfo = SkImageInfo::Make(
          materialSurface->width(), materialSurface->height(),
          kRGBA_8888_SkColorType, kPremul_SkAlphaType,
          SkColorSpace::MakeSRGB());
      UnitedShadowGlyphTextures glyphTextures;
      glyphTextures.material = isolatedShadowSurface->makeImageSnapshot();
      if (!glyphTextures.material) {
        error = "text SDF shadow material snapshot failed";
        return false;
      }
      auto maskSurface = gpuContext.MakeSurface(pixelInfo, error);
      if (!maskSurface)
        return false;
      maskSurface->getCanvas()->clear(SK_ColorTRANSPARENT);
      auto maskRequest = request;
      std::visit(
          [](auto &layer) {
            layer.material = text::LiteralTextMaterial{
                text::SolidTextMaterial{{1.0F, 1.0F, 1.0F, 1.0F}}};
          },
          maskRequest.material);
      maskRequest.texture.reset();
      maskRequest.presentationTint.red = 1.0F;
      maskRequest.presentationTint.green = 1.0F;
      maskRequest.presentationTint.blue = 1.0F;
      if (!gpuContext.RenderTextSdfMaterial(maskRequest, *maskSurface,
                                            error)) {
        return false;
      }
      glyphTextures.mask = maskSurface->makeImageSnapshot();
      if (!glyphTextures.mask) {
        error = "text SDF shadow mask snapshot failed";
        return false;
      }
      unitedShadowGlyphs.push_back(std::move(glyphTextures));
    }
  }
  if (uniteShadowMask) {
    const bool uniformMaterial = std::holds_alternative<text::SolidTextMaterial>(
        ResolveTextMaterial(GlyphLayerMaterial(pass.material)));
    const auto pixelInfo = SkImageInfo::Make(
        surface.width(), surface.height(), kRGBA_8888_SkColorType,
        kPremul_SkAlphaType, SkColorSpace::MakeSRGB());
    auto coverageSurface = gpuContext.MakeSurface(pixelInfo, error);
    if (!coverageSurface) return false;
    coverageSurface->getCanvas()->clear(SK_ColorTRANSPARENT);
    SkPaint addCoverage;
    addCoverage.setBlendMode(SkBlendMode::kPlus);
    // Saturating addition of nonnegative RGBA8 masks preserves their summed
    // coverage without a probabilistic union or a CPU synchronization point.
    for (const auto &glyph : unitedShadowGlyphs) {
      coverageSurface->getCanvas()->drawImage(
          glyph.mask, 0.0F, 0.0F, SkSamplingOptions(), &addCoverage);
    }
    const auto coverage = coverageSurface->makeImageSnapshot();
    const auto &program = GetTextRuntimeProgram(
        TextRuntimeShader::MaterialUnitedShadow);
    if (!coverage || !program.effect) {
      error = "text SDF united shadow shader unavailable: " + program.error;
      return false;
    }

    // Only nonuniform materials accumulate successive colors. Keep that
    // intermediate in FP16 (the Metal renderable floating-point format),
    // then quantize to RGBA8 once at publication. Uniform selection needs
    // RGBA8 only; no CPU readback is needed for either material kind.
    const auto accumulationInfo = uniformMaterial ? pixelInfo
        : pixelInfo.makeColorType(kRGBA_F16_SkColorType);
    std::array<sk_sp<SkSurface>, 2> accumulation{
        gpuContext.MakeSurface(accumulationInfo, error),
        gpuContext.MakeSurface(accumulationInfo, error)};
    if (!accumulation[0] || !accumulation[1]) return false;
    accumulation[0]->getCanvas()->clear(SK_ColorTRANSPARENT);
    auto unitedImage = accumulation[0]->makeImageSnapshot();
    const auto rawShader = [](const sk_sp<SkImage> &image) {
      return image->makeRawShader(SkTileMode::kClamp, SkTileMode::kClamp,
                                 SkSamplingOptions());
    };
    std::size_t destination = 1U;
    for (std::size_t index = 0U; index < unitedShadowGlyphs.size(); ++index) {
      if (!unitedImage) {
        error = "text SDF united shadow accumulation snapshot failed";
        return false;
      }
      const auto &glyph = unitedShadowGlyphs[uniformMaterial
          ? unitedShadowGlyphs.size() - 1U - index : index];
      SkRuntimeEffectBuilder builder(program.effect);
      builder.child("materialTexture") = rawShader(glyph.material);
      builder.child("maskTexture") = rawShader(glyph.mask);
      builder.child("unitedCoverageTexture") = rawShader(coverage);
      builder.child("previousTexture") = rawShader(unitedImage);
      builder.uniform("uniformMaterial") = uniformMaterial ? 1 : 0;
      auto shader = builder.makeShader();
      if (!shader) {
        error = "text SDF united shadow shader creation failed";
        return false;
      }
      SkPaint compose;
      compose.setBlendMode(SkBlendMode::kSrc);
      compose.setShader(std::move(shader));
      accumulation[destination]->getCanvas()->drawPaint(compose);
      unitedImage = accumulation[destination]->makeImageSnapshot();
      destination = 1U - destination;
    }
    if (!unitedImage) {
      error = "text SDF united shadow publication failed";
      return false;
    }
    if (!uniformMaterial) {
      auto quantized = gpuContext.MakeSurface(pixelInfo, error);
      if (!quantized) return false;
      SkPaint copy;
      copy.setBlendMode(SkBlendMode::kSrc);
      quantized->getCanvas()->drawImage(
          unitedImage, 0.0F, 0.0F, SkSamplingOptions(), &copy);
      unitedImage = quantized->makeImageSnapshot();
      if (!unitedImage) {
        error = "text SDF united shadow quantization failed";
        return false;
      }
    }
    surface.getCanvas()->drawImage(
        unitedImage, 0.0F, 0.0F,
        SkSamplingOptions(SkFilterMode::kNearest, SkMipmapMode::kNone),
        nullptr);
  }
  return true;
}


bool ExecutionGraphRequiresExplicitRasterExecutor(
    const std::vector<text::TextEffectExecutionNodeFramePlan> &nodes,
    const std::vector<std::string> *externallyCompositedResourceIds) {
  const auto externallyComposited =
      [&](const std::string &resourceId) noexcept {
        return externallyCompositedResourceIds != nullptr &&
               std::find(externallyCompositedResourceIds->begin(),
                         externallyCompositedResourceIds->end(), resourceId) !=
                   externallyCompositedResourceIds->end();
      };
  for (const auto &node : nodes) {
    if (ExecutionGraphRequiresExplicitRasterExecutor(
            node.children, externallyCompositedResourceIds)) {
      return true;
    }
    switch (node.kind) {
    case text::TextEffectExecutionNodeKind::History:
    case text::TextEffectExecutionNodeKind::State:
      return true;
    case text::TextEffectExecutionNodeKind::MediaInput:
      if (node.capability ==
              text::TextEffectExecutionCapability::MediaParticle ||
          node.capability ==
              text::TextEffectExecutionCapability::MediaMesh ||
          ((node.capability ==
                text::TextEffectExecutionCapability::MediaImage ||
            node.capability ==
                text::TextEffectExecutionCapability::MediaImageSequence ||
            node.capability ==
                text::TextEffectExecutionCapability::MediaVideo) &&
           (node.resourceIds.empty() ||
            std::any_of(node.resourceIds.begin(), node.resourceIds.end(),
                        [&](const auto &resourceId) {
                          return !externallyComposited(resourceId);
                        })))) {
        return true;
      }
      break;
    case text::TextEffectExecutionNodeKind::Scene:
      return true;
    case text::TextEffectExecutionNodeKind::MaterialPass:
      if (node.capability ==
              text::TextEffectExecutionCapability::MaterialDepthPass ||
          node.capability >=
              text::TextEffectExecutionCapability::MaterialAlphaModulate ||
          node.staticAffine || !node.parameters.empty()) {
        return true;
      }
      break;
    case text::TextEffectExecutionNodeKind::PostEffectPass:
    case text::TextEffectExecutionNodeKind::RenderTarget:
    case text::TextEffectExecutionNodeKind::Composite:
    case text::TextEffectExecutionNodeKind::Selector:
    case text::TextEffectExecutionNodeKind::Operator:
      return true;
    case text::TextEffectExecutionNodeKind::Layout:
      if (node.capability !=
          text::TextEffectExecutionCapability::LayoutGlyphRun) {
        return true;
      }
      break;
    }
  }
  return false;
}

} // namespace videocut::skia_runtime::internal::text_lane
