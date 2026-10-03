#pragma once

#include "videocut/text/TextLayout.h"
#include "videocut/text_composition/TextCompositionDocument.h"
#include "videocut/vector/VectorRenderLane.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace videocut::text_composition {

/// Absolute, stateless clocks for one sample. Source values remain in the
/// canonical media-tick domain; composition and decoration asset clocks use
/// microseconds. No field is relative to a previously rendered frame.
struct DecorationSampleTime final {
  std::int64_t absoluteCompositionUs{0};
  std::int64_t compositionBeginUs{0};
  std::int64_t compositionEndUs{0};
  std::int64_t absoluteSourceTicks{0};
  std::int64_t sourceWindowBeginTicks{0};
  std::int64_t sourceWindowEndTicks{0};
  /// Authoring handles include neutral decoration geometry, not sampled motion.
  bool suppressAnimation{false};
};

struct DecorationRuntimeLimits final {
  std::size_t maximumBindings{128};
  std::size_t maximumLayoutClustersScanned{1'000'000};
  std::size_t maximumInstancesPerBinding{4096};
  std::size_t maximumInstances{16'384};
  std::uint64_t maximumPixelsPerBinding{67'108'864};
  std::uint64_t maximumPixels{134'217'728};
  std::uint64_t maximumWorkingSetBytes{512U * 1024U * 1024U};
  std::uint32_t maximumInstanceDimension{16'384};
};

struct DecorationOrthographicBasis final {
  float xx{1.0F};
  float xy{0.0F};
  float yx{0.0F};
  float yy{1.0F};
};

/// Renderer-neutral source-raster to composition-pixel affine. Every value is
/// derived from authored instance data, admitted asset geometry, and the
/// raster result; no template-specific placement constant belongs here.
struct DecorationRasterAffine final {
  float xx{1.0F};
  float xy{0.0F};
  float yx{0.0F};
  float yy{1.0F};
  float xOffset{0.0F};
  float yOffset{0.0F};
};

struct DecorationInstance final {
  std::string instanceId;
  std::string variantId;
  DecorationAssetReference asset{};
  std::int64_t assetTimeUs{0};
  text::TextUtf8Range targetRange{};
  /// Geometry remains in the authored/local TextLayout space supplied to the
  /// sampler. Output canvas or backend coordinates are never persisted here.
  text::Rect localTargetBounds{};
  float localTranslationX{0.0F};
  float localTranslationY{0.0F};
  float localScaleX{1.0F};
  float localScaleY{1.0F};
  float localRotationDegrees{0.0F};
  float opacity{1.0F};
  /// The final authored-base + ordered-animation affine basis. The legacy
  /// scale/rotation fields remain the readable base decomposition; renderers
  /// consume this basis through ResolveDecorationOrthographicBasis.
  DecorationOrthographicBasis composedBasis{};
  bool hasComposedBasis{false};
  DecorationSamplingPolicy sampling{DecorationSamplingPolicy::Automatic};
  DecorationTransformInherit transformInherit{
      DecorationTransformInherit::Full};
};

DecorationOrthographicBasis
ResolveDecorationOrthographicBasis(const DecorationInstance &instance)
    noexcept;

DecorationRasterAffine ResolveDecorationRasterAffine(
    const DecorationInstance &instance, std::uint32_t sourceWidth,
    std::uint32_t sourceHeight, std::int32_t rasterOriginX,
    std::int32_t rasterOriginY, float rasterToOutputScaleX,
    float rasterToOutputScaleY) noexcept;

struct DecorationRenderItem final {
  std::string decorationId;
  DecorationAssetReference primaryAsset{};
  std::optional<TextResourceReference> fallbackAsset;
  std::string fallbackAssetId;
  std::int64_t assetTimeUs{0};
  std::optional<vector::VectorAssetFrameSample> assetFrameSample;
  bool enabled{false};
  bool visible{false};
  bool fallbackApplied{false};
  bool useFallbackAsset{false};
  bool nativeBackgroundFallback{false};
  VectorDecorationMode mode{VectorDecorationMode::TrackedGrapheme};
  std::int32_t zOrder{0};
  DecorationEffectScope effectScope{
      DecorationEffectScope::InsideGroupBehindText};
  std::vector<DecorationInstance> instances;
  std::vector<Diagnostic> diagnostics;
};

struct DecorationRenderPlan final {
  std::vector<DecorationRenderItem> items;
  std::vector<Diagnostic> diagnostics;
  std::size_t instanceCount{0};
  std::uint64_t estimatedPixels{0};
  std::uint64_t estimatedWorkingSetBytes{0};
  bool textOnlyFastPath{false};
};

/// Pure renderer-neutral sampler over canonical current ranges and portable
/// assets. Invalid bindings become disabled items while later bindings remain
/// independently sampleable.
DecorationRenderPlan
SampleDecorationRenderPlan(const TextCompositionDocument &document,
                           const text::TextLayout &localLayout,
                           const DecorationSampleTime &time,
                           const DecorationRuntimeLimits &limits = {});

} // namespace videocut::text_composition
