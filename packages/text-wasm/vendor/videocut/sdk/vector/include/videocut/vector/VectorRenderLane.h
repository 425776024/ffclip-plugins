#pragma once

#include "videocut/frame/VideoFrame.h"
#include "videocut/vector/VectorCustomization.h"
#include "videocut/vector/VectorDocument.h"

#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

namespace videocut::vector {

enum class VectorRasterDelivery : std::uint8_t {
  ResolvedFrame = 0,
  DeferredCompositeRaster = 1,
};

struct VectorRenderOptions final {
  std::uint32_t maximumWidth{8192};
  std::uint32_t maximumHeight{8192};
  std::uint64_t maximumPixels{33'554'432};
  std::size_t maximumSurfaceBytes{256U * 1024U * 1024U};
  std::size_t maximumWorkingBytes{512U * 1024U * 1024U};
  std::size_t maximumCacheBytes{256U * 1024U * 1024U};
  std::uint32_t maximumCachedFrames{32};
  std::string rendererProfile{"videocut-skia-vector-v1"};
};

inline constexpr std::uint32_t kVectorAssetSamplingTimeAbi = 1U;
inline constexpr std::uint32_t
    kVectorAssetSamplingQtVideoAnimSeqByProgress = 1U;

/// Resolves the authored ByProgress coordinate to the decoder's zero-based
/// display-frame index using Qt VideoAnimSeq fixed-seek publication:
/// logical frame zero publishes source
/// frame zero, while every positive logical seek publishes the preceding
/// display frame.
inline bool ResolveQtByProgressSourceFrame(
    const std::uint32_t contractVersion, const double progress,
    const std::uint64_t frameCount,
    std::uint64_t &sourceFrameIndex) noexcept {
  sourceFrameIndex = 0U;
  if (contractVersion != kVectorAssetSamplingQtVideoAnimSeqByProgress ||
      !std::isfinite(progress) || progress < 0.0 || progress > 1.0 ||
      frameCount == 0U) {
    return false;
  }
  const double coordinate =
      progress * (static_cast<double>(frameCount) - 0.01);
  if (!std::isfinite(coordinate) || coordinate < 0.0)
    return false;
  const auto logicalFrame =
      static_cast<std::uint64_t>(std::floor(coordinate));
  if (logicalFrame >= frameCount)
    return false;
  sourceFrameIndex = logicalFrame > 0U ? logicalFrame - 1U : logicalFrame;
  return true;
}

/// Request for Qt VideoAnimSeq ByProgress sampling. The vector
/// runtime resolves progress against the admitted exact source-frame count.
/// This is deliberately not a timestamp and must never be approximated through
/// duration/fps. ResolveQtByProgressSourceFrame preserves Qt's fixed-seek
/// published frame behavior.
struct VectorAssetFrameSample final {
  std::uint32_t contractVersion{kVectorAssetSamplingQtVideoAnimSeqByProgress};
  double progress{0.0};
};

struct VectorRenderRequest final {
  std::int64_t clipLocalTimeUs{0};
  /// Optional direct clock in the vector asset's source-time domain.
  ///
  /// When present, this value bypasses VectorTimeMapping completely: rate,
  /// direction, looping, and hold policies have already been resolved by the
  /// composition runtime. Animated assets require a value in the half-open
  /// interval [0, asset duration); static assets accept only zero.
  /// clipLocalTimeUs remains the publication timestamp and is intentionally
  /// independent from this asset clock.
  std::optional<std::int64_t> assetTimeUs;
  /// Optional exact footage-frame sampling inside a Lottie document. Assets
  /// sampled by source time leave this absent and use assetTimeUs above.
  std::optional<VectorAssetFrameSample> assetFrameSample;
  std::uint32_t outputWidth{0};
  std::uint32_t outputHeight{0};
  VectorViewportPolicy viewport{};
  RenderQuality quality{RenderQuality::PreviewActive};
  VectorRasterDelivery delivery{VectorRasterDelivery::ResolvedFrame};
  /// SVG can publish directly to the consuming GPU domain. Lottie retains
  /// straight-alpha rasterization where its low-coverage pixel ABI requires it.
  bool gpuConsumer{false};
  std::int32_t gpuDeviceIndex{-1};
  std::uint64_t gpuDeviceGeneration{1};
  /// Hint for supersampled Lottie ink when the GPU will later apply a
  /// presentation scale. Live transform drags keep this at 1.0 and reuse the
  /// cached ink; settle/preview raises it with |scale| so enlarged stickers
  /// do not upsample a soft bitmap. The lane never raises the deferred 1×
  /// fallback with this hint — budget failures still try plain 2× first.
  float presentationScale{1.0F};
  /// Minimum supersample scale for deferred Lottie ink. 0 keeps the lane
  /// default (2). Preview live/settle use 4 so drag samples share the
  /// rest-frame cache key.
  int minimumSupersampleScale{0};
  /// Item-local sampled field values. This is deliberately separate from
  /// VectorDocument::overrides, which remain the authored static baseline.
  VectorRuntimeOverrideSet runtimeOverrides;
  /// Optional owner-local enter/loop/exit envelope. This is request state,
  /// not a second durable animation owner.
  std::optional<VectorAnimationPhaseEnvelope> animationPhases;
  /// Publication deadline. A frame that finishes after this fence is never
  /// cached or published, even when the underlying Skia call is synchronous.
  std::optional<std::chrono::steady_clock::time_point> deadline;
  CancelCheck cancel;
};

struct VectorRenderResult final {
  VectorStatusCode status{VectorStatusCode::Failed};
  frame::VideoFrame image;
  std::int32_t originX{0};
  std::int32_t originY{0};
  /// Requested-output pixels represented by one raster pixel. Values below
  /// one keep a supersampled vector raster alive until the GPU compositor
  /// performs the final reconstruction filter.
  float rasterToOutputScaleX{1.0F};
  float rasterToOutputScaleY{1.0F};
  Rect logicalBounds{};
  /// Stable authored local box, independent from animation ink and shared
  /// canvas placement. This is the direct-manipulation owner/control truth.
  Rect authoredLocalBounds{};
  bool authoredLocalBoundsValid{false};
  Rect inkBounds{};
  /// True only when inkBounds was measured from rendered coverage. Logical
  /// layout bounds must never be exposed as a fabricated visual-ink result.
  bool inkBoundsValid{false};
  VectorAnimationPhase sampledPhase{VectorAnimationPhase::Intrinsic};
  std::int64_t sampledPhaseLocalUs{0};
  std::int64_t sampledSourceUs{0};
  std::optional<std::uint64_t> sampledSourceFrameIndex;
  std::uint64_t documentGeneration{0};
  std::vector<Diagnostic> diagnostics;

  explicit operator bool() const noexcept {
    return status == VectorStatusCode::Ok && static_cast<bool>(image);
  }
};

class VectorRenderLane {
public:
  virtual ~VectorRenderLane() = default;
  virtual bool ReplaceDocument(VectorDocument document, std::string &error) = 0;
  virtual std::uint64_t generation() const noexcept = 0;
  virtual VectorRenderResult Render(const VectorRenderRequest &request) = 0;
  virtual void PurgeCache() noexcept = 0;
};

using VectorRenderLaneHolder = std::shared_ptr<VectorRenderLane>;

} // namespace videocut::vector
