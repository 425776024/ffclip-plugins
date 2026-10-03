#include "videocut/text_composition/TextCompositionSnapshot.h"

#include "CanonicalTextIdentity.h"
#include "TextSamplingSupport.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <string>
#include <unordered_set>
#include <utility>
#include <variant>

namespace videocut::text_composition {
namespace {

void Add(std::vector<Diagnostic> &diagnostics, std::string code,
         std::string subject, std::string message,
         const DiagnosticSeverity severity = DiagnosticSeverity::Error) {
  diagnostics.push_back({std::move(code), severity, "snapshot",
                         std::move(subject), std::move(message)});
}

text::Rect Union(const text::Rect &left, const text::Rect &right) noexcept {
  if (left.width <= 0.0F || left.height <= 0.0F)
    return right;
  if (right.width <= 0.0F || right.height <= 0.0F)
    return left;
  const auto minimumX = std::min(left.x, right.x);
  const auto minimumY = std::min(left.y, right.y);
  const auto maximumX =
      std::max(left.x + left.width, right.x + right.width);
  const auto maximumY =
      std::max(left.y + left.height, right.y + right.height);
  return {minimumX, minimumY, maximumX - minimumX, maximumY - minimumY};
}

text::Rect Expand(const text::Rect &value, const float left, const float top,
                  const float right, const float bottom) noexcept {
  return {value.x - std::max(0.0F, left),
          value.y - std::max(0.0F, top),
          value.width + std::max(0.0F, left) + std::max(0.0F, right),
          value.height + std::max(0.0F, top) + std::max(0.0F, bottom)};
}

text::Rect TransformEnvelope(const text::Rect &value,
                             const text::TextBackdropTransform &transform)
    noexcept {
  const float scaledWidth = value.width * std::fabs(transform.scaleX);
  const float scaledHeight = value.height * std::fabs(transform.scaleY);
  const float radians =
      transform.rotationDegrees * 3.14159265358979323846F / 180.0F;
  const float cosine = std::fabs(std::cos(radians));
  const float sine = std::fabs(std::sin(radians));
  const float width = scaledWidth * cosine + scaledHeight * sine;
  const float height = scaledWidth * sine + scaledHeight * cosine;
  const float centerX = value.x + value.width * 0.5F + transform.offsetX;
  const float centerY = value.y + value.height * 0.5F + transform.offsetY;
  return {centerX - width * 0.5F, centerY - height * 0.5F, width, height};
}

void ExpandForMaterialLayer(const text::TextGlyphMaterialLayer &layer,
                            text::Rect &extent) noexcept {
  std::visit(
      [&](const auto &typed) {
        using Layer = std::decay_t<decltype(typed)>;
        if constexpr (std::is_same_v<Layer, text::TextFillLayer>) {
          extent = Expand(extent, -typed.offsetX, -typed.offsetY,
                          typed.offsetX, typed.offsetY);
        } else if constexpr (std::is_same_v<Layer,
                                            text::TextStrokeLayer>) {
          const auto radius = std::max(0.0F, typed.width * 0.5F) +
                              std::max(0.0F, typed.blurRadius) * 2.0F +
                              std::max(0.0F, typed.spread);
          extent = Expand(extent, radius - typed.offsetX,
                          radius - typed.offsetY, radius + typed.offsetX,
                          radius + typed.offsetY);
        } else if constexpr (std::is_same_v<Layer,
                                            text::TextShadowLayer>) {
          const float radians = typed.thicknessAngleDegrees *
                                3.14159265358979323846F / 180.0F;
          const float projectionX =
              std::cos(radians) * typed.thicknessDistance;
          const float projectionY =
              -std::sin(radians) * typed.thicknessDistance;
          const auto radius = std::max(0.0F, typed.blurRadius) * 2.0F +
                              std::max(0.0F, typed.spread);
          const auto offsetX = typed.offsetX + projectionX;
          const auto offsetY = typed.offsetY + projectionY;
          const auto base = extent;
          extent = Expand(extent, radius - offsetX, radius - offsetY,
                          radius + offsetX, radius + offsetY);
          for (const auto &stroke : typed.strokes) {
            const auto strokeRadius =
                std::max(0.0F, stroke.width + 2.0F * typed.spread) * 0.5F +
                std::max(0.0F, stroke.spread) +
                std::max(0.0F, stroke.blurRadius) * 2.0F;
            const auto strokeX = offsetX + stroke.offsetX;
            const auto strokeY = offsetY + stroke.offsetY;
            extent = Union(extent, Expand(base, strokeRadius - strokeX,
                                          strokeRadius - strokeY,
                                          strokeRadius + strokeX,
                                          strokeRadius + strokeY));
          }
        } else {
          const auto radius = std::max(0.0F, typed.radius) * 2.0F +
                              std::max(0.0F, typed.spread);
          const auto offsetX = typed.directionX * typed.radius;
          const auto offsetY = typed.directionY * typed.radius;
          extent = Expand(extent, radius - offsetX, radius - offsetY,
                          radius + offsetX, radius + offsetY);
        }
      },
      layer);
}

text::Rect BackdropExtent(const text::Rect &base,
                          const text::TextBackdropLayer &layer) noexcept {
  auto extent = Expand(base, layer.padding.left, layer.padding.top,
                       layer.padding.right, layer.padding.bottom);
  extent = Expand(extent, layer.expand.left, layer.expand.top,
                  layer.expand.right, layer.expand.bottom);
  extent = Expand(extent, layer.sourceOutsets.left, layer.sourceOutsets.top,
                  layer.sourceOutsets.right, layer.sourceOutsets.bottom);
  std::visit(
      [&](const auto &source) {
        using Source = std::decay_t<decltype(source)>;
        if constexpr (std::is_same_v<Source, text::RoundedRectBackdrop>) {
          if (source.authoredWidth && *source.authoredWidth > extent.width) {
            const auto difference = *source.authoredWidth - extent.width;
            extent = Expand(extent, difference * 0.5F, 0.0F,
                            difference * 0.5F, 0.0F);
          }
          if (source.authoredHeight && *source.authoredHeight > extent.height) {
            const auto difference = *source.authoredHeight - extent.height;
            extent = Expand(extent, 0.0F, difference * 0.5F, 0.0F,
                            difference * 0.5F);
          }
          switch (source.tail.edge) {
          case text::BubbleTailEdge::Top:
            extent = Expand(extent, 0.0F, source.tail.length, 0.0F, 0.0F);
            break;
          case text::BubbleTailEdge::Bottom:
            extent = Expand(extent, 0.0F, 0.0F, 0.0F, source.tail.length);
            break;
          case text::BubbleTailEdge::Left:
            extent = Expand(extent, source.tail.length, 0.0F, 0.0F, 0.0F);
            break;
          case text::BubbleTailEdge::Right:
            extent = Expand(extent, 0.0F, 0.0F, source.tail.length, 0.0F);
            break;
          case text::BubbleTailEdge::None:
            break;
          }
          for (const auto &stroke : source.strokes) {
            const auto radius = std::max(0.0F, stroke.width * 0.5F) +
                                std::max(0.0F, stroke.blurRadius) +
                                std::max(0.0F, stroke.spread);
            extent = Expand(extent, radius - stroke.offsetX,
                            radius - stroke.offsetY,
                            radius + stroke.offsetX,
                            radius + stroke.offsetY);
          }
        } else if constexpr (std::is_same_v<Source,
                                            text::NineSliceBackdrop>) {
          extent = Expand(extent, source.contentInsets.left,
                          source.contentInsets.top, source.contentInsets.right,
                          source.contentInsets.bottom);
          if (source.minimumContentWidth > extent.width) {
            const auto difference = source.minimumContentWidth - extent.width;
            extent = Expand(extent, difference * 0.5F, 0.0F,
                            difference * 0.5F, 0.0F);
          }
          if (source.minimumContentHeight > extent.height) {
            const auto difference =
                source.minimumContentHeight - extent.height;
            extent = Expand(extent, 0.0F, difference * 0.5F, 0.0F,
                            difference * 0.5F);
          }
        } else if constexpr (std::is_same_v<Source,
                                            text::AnimatedBackdrop>) {
          extent = Expand(extent, source.contentInsets.left,
                          source.contentInsets.top, source.contentInsets.right,
                          source.contentInsets.bottom);
        }
      },
      layer.source);
  return TransformEnvelope(extent, layer.transform);
}

TextVisualExtentPlan ResolveVisualExtent(
    const TextCompositionDocument &document) noexcept {
  TextVisualExtentPlan plan;
  const auto &layout = document.presentation.authoredLayoutFrame;
  plan.layoutBounds = {layout.x, layout.y, std::max(0.0F, layout.width),
                       std::max(0.0F, layout.height)};
  plan.controlBounds = plan.layoutBounds;
  plan.inkBounds = plan.layoutBounds;
  auto visual = plan.inkBounds;
  for (const auto &paragraph : document.content)
    for (const auto &run : paragraph.runs)
      for (const auto &layer : run.style.materials.layers) {
        auto layerExtent = plan.inkBounds;
        ExpandForMaterialLayer(layer, layerExtent);
        visual = Union(visual, layerExtent);
      }

  // Every backdrop starts from the same text geometry. Their authored order
  // controls compositing only; it must not recursively inflate later layers.
  for (const auto &layer : document.presentation.appearance.backdrops.layers) {
    if (layer.enabled)
      visual = Union(visual, BackdropExtent(plan.inkBounds, layer));
  }
  const auto animation = text::ResolveTextAnimationEnvelope(
      document.animations, plan.layoutBounds.width, plan.layoutBounds.height);
  visual = Expand(visual, animation.left + animation.effectPaddingPx,
                  animation.top + animation.effectPaddingPx,
                  animation.right + animation.effectPaddingPx,
                  animation.bottom + animation.effectPaddingPx);
  for (const auto &decoration : document.decorations) {
    if (!decoration.enabled)
      continue;
    float maximumWidth = decoration.asset.intrinsicWidth;
    float maximumHeight = decoration.asset.intrinsicHeight;
    if (decoration.instancePattern) {
      for (const auto &variant : decoration.instancePattern->assetVariants) {
        maximumWidth = std::max(maximumWidth, variant.asset.intrinsicWidth);
        maximumHeight = std::max(maximumHeight, variant.asset.intrinsicHeight);
      }
    }
    const auto halfWidth =
        maximumWidth * std::fabs(decoration.localTransform.scaleX) * 0.5F;
    const auto halfHeight =
        maximumHeight * std::fabs(decoration.localTransform.scaleY) * 0.5F;
    visual = Expand(
        visual,
        std::fabs(decoration.localTransform.offsetX) + halfWidth +
            decoration.padding.left,
        std::fabs(decoration.localTransform.offsetY) + halfHeight +
            decoration.padding.top,
        std::fabs(decoration.localTransform.offsetX) + halfWidth +
            decoration.padding.right,
        std::fabs(decoration.localTransform.offsetY) + halfHeight +
            decoration.padding.bottom);
  }
  const auto &policy = document.presentation.appearance.visualExtent;
  if (!policy.allowControlOverflow)
    visual = plan.controlBounds;
  const auto clampDimension = [](const float maximum, const bool horizontal,
                                 text::Rect &rect) {
    const float current = horizontal ? rect.width : rect.height;
    if (maximum <= 0.0F || current <= maximum)
      return;
    if (horizontal) {
      rect.x += (rect.width - maximum) * 0.5F;
      rect.width = maximum;
    } else {
      rect.y += (rect.height - maximum) * 0.5F;
      rect.height = maximum;
    }
  };
  if (policy.maximumExtentWidth)
    clampDimension(*policy.maximumExtentWidth, true, visual);
  if (policy.maximumExtentHeight)
    clampDimension(*policy.maximumExtentHeight, false, visual);
  plan.visualExtent = visual;
  plan.conservativeEnvelope = visual;
  return plan;
}

bool HasAnimatedBackdrop(const text::TextBackdropStack &stack) noexcept {
  return std::any_of(stack.layers.begin(), stack.layers.end(),
                     [](const auto &layer) {
                       return layer.enabled &&
                              (std::holds_alternative<text::AnimatedBackdrop>(
                                   layer.source) ||
                               !layer.animationClips.empty());
                     });
}

bool HasAnimatedPostEffect(const text::TextAnimationStack &stack) noexcept {
  return std::any_of(stack.layers.begin(), stack.layers.end(),
                     [](const auto &layer) {
                       return layer.enabled && !layer.postEffects.empty();
                     });
}

bool HasAnimatedDecoration(const text::TextAnimationStack &stack) noexcept {
  return std::any_of(stack.layers.begin(), stack.layers.end(),
                     [](const auto &layer) {
                       return layer.enabled && !layer.decorations.empty();
                     });
}

} // namespace

TextCompositionSnapshotResult BuildTextCompositionSnapshot(
    const TextCompositionDocument &document,
    const TextCompositionSnapshotSample &sample,
    const TextCompositionLimits &limits) {
  TextCompositionSnapshotResult result;
  const auto validation = ValidateTextCompositionDocument(document, limits);
  result.diagnostics = validation.diagnostics;
  if (!validation.valid)
    return result;
  if (sample.compositionEndUs <= sample.compositionBeginUs ||
      sample.sourceWindowEndTicks <= sample.sourceWindowBeginTicks) {
    Add(result.diagnostics, "text_composition.snapshot.sample_invalid", {},
        "composition and source windows must be non-empty half-open ranges");
    return result;
  }
  result.valid = true;
  if (sample.absoluteCompositionUs < sample.compositionBeginUs ||
      sample.absoluteCompositionUs >= sample.compositionEndUs ||
      sample.sourceLocalTicks < sample.sourceWindowBeginTicks ||
      sample.sourceLocalTicks >= sample.sourceWindowEndTicks) {
    result.emptyAtSample = true;
    return result;
  }

  const auto localCompositionUs = detail::SaturatingDifference(
      sample.absoluteCompositionUs, sample.compositionBeginUs);
  TextCompositionSnapshot snapshot;
  snapshot.resolvedText.referenceCanvas =
      document.presentation.referenceCanvas;
  snapshot.resolvedText.layoutBox =
      document.presentation.authoredLayoutFrame;
  snapshot.resolvedText.writingMode = document.presentation.writingMode;
  snapshot.resolvedText.slots = document.contentSlots;
  snapshot.resolvedText.paragraphs = document.content;
  snapshot.appearance = document.presentation.appearance;
  snapshot.animations = document.animations;
  snapshot.timedSpans = detail::ResolveTimedSpans(
      document, sample.sourceWindowBeginTicks, sample.sourceWindowEndTicks);
  snapshot.resources = document.resources;
  snapshot.visualExtentPlan = ResolveVisualExtent(document);

  snapshot.contentIdentity = detail::MakeIdentity(
      "videocut.text.snapshot.content", [&](auto &writer) {
        detail::EncodeContentIdentity(writer, document);
      });
  snapshot.resourceIdentity = detail::MakeIdentity(
      "videocut.text.snapshot.resources", [&](auto &writer) {
        detail::EncodeResourceIdentity(writer, document);
      });
  snapshot.layoutIdentity = detail::MakeIdentity(
      "videocut.text.snapshot.layout", [&](auto &writer) {
        writer.String(snapshot.contentIdentity);
        detail::EncodeLayoutIdentity(writer, document);
      });
  snapshot.glyphMaterialIdentity = detail::MakeIdentity(
      "videocut.text.snapshot.glyph-material", [&](auto &writer) {
        writer.String(snapshot.layoutIdentity);
        writer.String(snapshot.resourceIdentity);
        detail::EncodeGlyphIdentity(writer, document);
      });
  snapshot.backdropIdentity = detail::MakeIdentity(
      "videocut.text.snapshot.backdrop", [&](auto &writer) {
        writer.String(snapshot.layoutIdentity);
        writer.String(snapshot.resourceIdentity);
        detail::EncodeBackdropIdentity(writer, document);
        if (HasAnimatedBackdrop(document.presentation.appearance.backdrops)) {
          writer.Signed(localCompositionUs);
          writer.Signed(sample.sourceLocalTicks);
        }
      });
  snapshot.animationIdentity = detail::MakeIdentity(
      "videocut.text.snapshot.animation", [&](auto &writer) {
        detail::Encode(writer, document.animations);
        writer.Count(snapshot.timedSpans.size());
        for (const auto &span : snapshot.timedSpans) {
          writer.String(span.spanId);
          writer.Enumeration(span.semantic);
          writer.String(span.paragraphId);
          writer.String(span.runId);
          writer.Unsigned(span.utf8Begin);
          writer.Unsigned(span.utf8End);
          writer.Signed(span.startOffsetUs);
          writer.Signed(span.endOffsetUs);
          writer.Enumeration(span.progressMode);
          writer.Signed(span.transitionEndOffsetUs);
        }
        if (!document.animations.layers.empty() ||
            !document.animations.executionGraph.nodes.empty())
          writer.Signed(localCompositionUs);
      });
  snapshot.executionGraphIdentity = detail::MakeIdentity(
      "videocut.text.snapshot.execution-graph", [&](auto &writer) {
        detail::Encode(writer, document.animations.executionGraph);
        if (!document.animations.executionGraph.nodes.empty()) {
          writer.String(snapshot.resourceIdentity);
          writer.Signed(localCompositionUs);
          writer.Signed(sample.sourceLocalTicks);
        }
      });
  snapshot.decorationIdentity = detail::MakeIdentity(
      "videocut.text.snapshot.decorations", [&](auto &writer) {
        detail::EncodeDecorationIdentity(writer, document);
        for (const auto &layer : document.animations.layers) {
          if (layer.decorations.empty())
            continue;
          writer.String(layer.layerId);
          writer.Boolean(layer.enabled);
          detail::Encode(writer, layer.timeDriver);
          writer.Boolean(layer.layerTrack.has_value());
          if (layer.layerTrack)
            detail::Encode(writer, *layer.layerTrack);
          writer.Count(layer.decorations.size());
          for (const auto &decoration : layer.decorations)
            detail::Encode(writer, decoration);
        }
        if (HasAnimatedDecoration(document.animations)) {
          writer.Count(snapshot.timedSpans.size());
          for (const auto &span : snapshot.timedSpans) {
            writer.String(span.spanId);
            writer.Signed(span.startOffsetUs);
            writer.Signed(span.endOffsetUs);
            writer.Signed(span.transitionEndOffsetUs);
          }
        }
        if (!document.decorations.empty()) {
          writer.Signed(localCompositionUs);
          writer.Signed(sample.sourceLocalTicks);
        }
      });
  snapshot.postEffectIdentity = detail::MakeIdentity(
      "videocut.text.snapshot.post-effects", [&](auto &writer) {
        for (const auto &layer : document.animations.layers) {
          if (layer.postEffects.empty())
            continue;
          writer.String(layer.layerId);
          writer.Boolean(layer.enabled);
          detail::Encode(writer, layer.target);
          writer.Enumeration(layer.combineMode);
          detail::Encode(writer, layer.timeDriver);
          writer.Boolean(layer.layerTrack.has_value());
          if (layer.layerTrack)
            detail::Encode(writer, *layer.layerTrack);
          writer.Boolean(layer.renderGroup.has_value());
          if (layer.renderGroup)
            detail::Encode(writer, *layer.renderGroup);
          writer.Count(layer.postEffects.size());
          for (const auto &effect : layer.postEffects)
            detail::Encode(writer, effect);
          writer.Boolean(layer.requiresTimedText);
        }
        if (HasAnimatedPostEffect(document.animations)) {
          writer.Signed(localCompositionUs);
          writer.Count(snapshot.timedSpans.size());
          for (const auto &span : snapshot.timedSpans) {
            writer.String(span.spanId);
            writer.Signed(span.startOffsetUs);
            writer.Signed(span.endOffsetUs);
            writer.Signed(span.transitionEndOffsetUs);
          }
        }
      });
  snapshot.compositeIdentity = detail::MakeIdentity(
      "videocut.text.snapshot.composite", [&](auto &writer) {
        writer.String(snapshot.glyphMaterialIdentity);
        writer.String(snapshot.backdropIdentity);
        writer.String(snapshot.animationIdentity);
        writer.String(snapshot.decorationIdentity);
        writer.String(snapshot.postEffectIdentity);
        writer.String(snapshot.executionGraphIdentity);
        writer.Enumeration(document.effectPolicy);
        writer.Float(document.presentation.appearance.globalAlpha);
      });

  snapshot.resourceKey.digest = snapshot.resourceIdentity;
  snapshot.layoutKey.digest = snapshot.layoutIdentity;
  snapshot.glyphKey.digest = snapshot.glyphMaterialIdentity;
  snapshot.backdropKey.digest = snapshot.backdropIdentity;
  snapshot.postEffectKey.digest = snapshot.postEffectIdentity;
  snapshot.compositeKey.digest = snapshot.compositeIdentity;

  std::unordered_set<std::size_t> disabledIndexes(
      validation.disabledDecorationIndexes.begin(),
      validation.disabledDecorationIndexes.end());
  snapshot.disabledDecorationIds = validation.disabledDecorationIds;
  for (std::size_t index = 0U; index < document.decorations.size(); ++index) {
    if (disabledIndexes.count(index) == 0U &&
        document.decorations[index].enabled)
      snapshot.decorations.push_back(document.decorations[index]);
  }
  snapshot.decorationFreeFastPath = snapshot.decorations.empty();
  result.snapshot = std::move(snapshot);
  return result;
}

text::TextAnimationStack BuildTextLaneAnimations(
    const text::TextAnimationStack &source,
    const std::vector<VectorDecorationBinding> &compositionDecorations) {
  auto animations = source;
  std::unordered_set<std::string> externalIds;
  for (const auto &binding : compositionDecorations)
    externalIds.insert(binding.decorationId);
  for (auto &layer : animations.layers) {
    auto &decorations = layer.decorations;
    decorations.erase(
        std::remove_if(decorations.begin(), decorations.end(),
                       [&](const auto &decoration) {
                         return externalIds.count(decoration.decorationId) != 0;
                       }),
        decorations.end());
  }
  return animations;
}

} // namespace videocut::text_composition
