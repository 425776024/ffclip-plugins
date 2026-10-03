#pragma once

#include "internal/skia/SkiaHeaders.h"
#include "videocut/text/TextEffectFramePlan.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <optional>

namespace videocut::skia_runtime::internal {

/// A linear glyph/Page/post chain can use the established native post-effect
/// pipeline. Every explicit post must match the sampled canonical DAG; scene
/// transforms, resources, state, branches and material programs stay explicit.
[[nodiscard]] inline bool IsLinearTextPostEffectExecutionGraph(
    const text::TextEffectFramePlan &plan) noexcept {
  using Kind = text::TextEffectExecutionNodeKind;
  using Capability = text::TextEffectExecutionCapability;
  if (plan.postEffectNodes.empty() || !plan.backdropPasses.empty() ||
      !plan.decorationPasses.empty() || !plan.executionParameters.empty())
    return false;
  const auto &nodes = plan.executionGraph.nodes;
  std::size_t posts = 0U;
  unsigned materials = 0U, targets = 0U;
  for (std::size_t index = 0U; index < nodes.size(); ++index) {
    const auto &node = nodes[index];
    if (!node.active || node.nodeId.empty() || !node.children.empty() ||
        !node.resourceIds.empty() || !node.parameters.empty() ||
        !node.stateId.empty() || !node.historyId.empty() || node.physicsSpec ||
        node.collisionSpec || node.camera || node.staticAffine ||
        (index == 0U ? !node.inputIds.empty()
                     : node.inputIds.size() != 1U ||
                           node.inputIds.front() != nodes[index - 1U].nodeId))
      return false;
    if (node.kind == Kind::PostEffectPass) {
      // This path samples post parameters on the owning animation layer's
      // clock. An authored node clock must retain the explicit executor even
      // when its sampled values happen to coincide with that owner.
      if (node.hasAuthoredTimeDriver || materials != 1U || targets != 1U ||
          posts >= plan.postEffectNodes.size())
        return false;
      const auto &post = plan.postEffectNodes[posts];
      if (node.nodeId != post.nodeId || node.postEffectKind != post.kind ||
          !post.renderGroup || post.renderGroup->spec.mode != text::TextRenderGroupMode::Page ||
          post.layerId != plan.postEffectNodes.front().layerId ||
          (posts == 0U ? !post.inputIds.empty()
                       : post.inputIds.size() != 1U ||
                             post.inputIds.front() != plan.postEffectNodes[posts - 1U].nodeId))
        return false;
      ++posts;
      continue;
    }
    if (posts != 0U || node.postEffectKind)
      return false;
    switch (node.kind) {
    case Kind::Layout:
      if (index != 0U || node.capability != Capability::LayoutGlyphRun)
        return false;
      break;
    case Kind::Selector:
      if (node.capability != Capability::SelectorBase) return false;
      break;
    case Kind::Operator:
      if (node.capability != Capability::OperatorProgram) return false;
      break;
    case Kind::Scene:
      if (node.capability != Capability::ScenePrefab &&
          node.capability != Capability::SceneMesh) return false;
      break;
    case Kind::MaterialPass:
      if (node.capability != Capability::MaterialColorPass || ++materials != 1U)
        return false;
      break;
    case Kind::RenderTarget:
      if (node.capability != Capability::RenderTargetExpanded ||
          materials != 1U || ++targets != 1U) return false;
      break;
    default:
      return false;
    }
  }
  return !nodes.empty() && nodes.front().kind == Kind::Layout &&
         posts == plan.postEffectNodes.size();
}

/// Source UV kernels divide a unit direction by the output viewport before
/// sampling a possibly differently sized attachment. Keep that division out
/// of the model/reference transform and reflect Y exactly once for Skia.
[[nodiscard]] inline std::optional<SkVector> ResolveTextOutputTexelStep(
    const SkVector axis, const float stride, const SkSize viewport,
    const SkSize attachment) noexcept {
  const float length = std::hypot(axis.x(), axis.y());
  if (!std::isfinite(length) || length <= 0.0F || !std::isfinite(stride) ||
      stride < 0.0F || !std::isfinite(viewport.width()) ||
      !std::isfinite(viewport.height()) || viewport.width() <= 0.0F ||
      viewport.height() <= 0.0F || !std::isfinite(attachment.width()) ||
      !std::isfinite(attachment.height()) || attachment.width() <= 0.0F ||
      attachment.height() <= 0.0F)
    return std::nullopt;
  const auto step = SkVector::Make(
      stride * axis.x() / length / viewport.width() * attachment.width(),
      -stride * axis.y() / length / viewport.height() * attachment.height());
  return std::isfinite(step.x()) && std::isfinite(step.y())
             ? std::optional<SkVector>{step} : std::nullopt;
}

/// Effect Programs evaluate unit geometry and ordered transforms in a centred,
/// Y-up source-design domain. Paragraph layout remains in the reference-canvas
/// top-left/Y-down domain; only this bridge crosses that internal boundary.
struct TextEffectCoordinateBridge final {
  float sourceToReferenceScale{1.0F};
  SkPoint referenceCenter{SkPoint::Make(0.0F, 0.0F)};
};

[[nodiscard]] inline std::optional<TextEffectCoordinateBridge>
ResolveTextEffectCoordinateBridge(
    const float referenceWidth, const float referenceHeight,
    const float sourceDesignWidth, const float sourceDesignHeight) noexcept {
  if (!std::isfinite(referenceWidth) || !std::isfinite(referenceHeight) ||
      !std::isfinite(sourceDesignWidth) ||
      !std::isfinite(sourceDesignHeight) || referenceWidth <= 0.0F ||
      referenceHeight <= 0.0F || sourceDesignWidth <= 0.0F ||
      sourceDesignHeight <= 0.0F) {
    return std::nullopt;
  }
  const float scale = referenceWidth / sourceDesignWidth;
  if (!std::isfinite(scale) || scale <= 0.0F)
    return std::nullopt;
  TextEffectCoordinateBridge result;
  result.sourceToReferenceScale = scale;
  result.referenceCenter =
      SkPoint::Make(referenceWidth * 0.5F, referenceHeight * 0.5F);
  return result;
}

/// Maps one reference-canvas point into Effect Program's centred, Y-up
/// source-design domain.
[[nodiscard]] inline SkPoint ReferencePointToTextEffectSource(
    const SkPoint point, const TextEffectCoordinateBridge &bridge) noexcept {
  const float inverseScale = 1.0F / bridge.sourceToReferenceScale;
  return SkPoint::Make(
      (point.x() - bridge.referenceCenter.x()) * inverseScale,
      (bridge.referenceCenter.y() - point.y()) * inverseScale);
}

/// Converts a source-design Effect Program distance into reference-canvas
/// units.
[[nodiscard]] inline float TextEffectDistanceToReference(
    const float value, const TextEffectCoordinateBridge &bridge) noexcept {
  return value * bridge.sourceToReferenceScale;
}

/// Adds one program-facing unit rect to the PositionType::Offset scope.
/// Callers must pass every rect in one common coordinate domain.
inline void IncludeTextAnimationLetterRect(SkRect &scope,
                                           const SkRect &letterRect) noexcept {
  if (!letterRect.isEmpty())
    scope.join(letterRect);
}

/// Expands one row's uniform tight vertical half-extent.
inline void
IncludeTextAnimationUniformTightVerticalExtent(
    float &maximumHalfHeight, const SkRect &tightLetterRect,
    const float rowCenterY) noexcept {
  if (tightLetterRect.isEmpty() || !std::isfinite(rowCenterY))
    return;
  const float halfHeight =
      std::max(std::fabs(tightLetterRect.top() - rowCenterY),
               std::fabs(tightLetterRect.bottom() - rowCenterY));
  if (std::isfinite(halfHeight))
    maximumHalfHeight = std::max(maximumHalfHeight, halfHeight);
}

/// Converts a normalized TextBoundsOffset selector result into the coordinate
/// domain of `letterScope`.
[[nodiscard]] inline SkVector
ResolveTextBoundsPositionOffset(const float normalizedX,
                                const float normalizedY,
                                const SkRect &letterScope) noexcept {
  return SkVector::Make(normalizedX * letterScope.width() * 0.5F,
                        -normalizedY * letterScope.height() * 0.5F);
}

/// Resolves a DistanceFromCenter translation against immutable unit geometry.
[[nodiscard]] inline SkVector ResolveTextAnimationDistanceOffset(
    const SkPoint letterInitialPosition, const SkRect &selectorUnitBounds,
    const SkPoint center, const double distance,
    const bool useLetterInitialPosition) noexcept {
  const double targetX =
      useLetterInitialPosition
          ? static_cast<double>(letterInitialPosition.x())
          : static_cast<double>(selectorUnitBounds.centerX());
  const double targetY =
      useLetterInitialPosition
          ? static_cast<double>(letterInitialPosition.y())
          : static_cast<double>(selectorUnitBounds.centerY());
  const double offsetY = (targetY - static_cast<double>(center.y())) * distance;
  return SkVector::Make(
      static_cast<float>((targetX - static_cast<double>(center.x())) *
                         distance),
      static_cast<float>(offsetY));
}

} // namespace videocut::skia_runtime::internal
