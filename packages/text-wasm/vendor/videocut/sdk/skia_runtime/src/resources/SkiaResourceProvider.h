#pragma once

#include "internal/skia/SkiaHeaders.h"
#include "videocut/skia_runtime/SkiaRuntimeFactory.h"
#include "videocut/vector/VectorDocument.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

namespace videocut::skia_runtime::internal {

struct AnimatedImageFrameSelectionState;

// Retain immutable decoded/encoded storage for Skia without duplicating bytes.
sk_sp<SkData> RetainImmutableSkData(
    std::shared_ptr<const std::vector<std::uint8_t>> bytes) noexcept;

inline constexpr std::uint32_t kQtFollowerDirectFrameContractVersion = 1U;

/// Exact decoded follower frame before Skottie/Lottie rasterization.
///
/// pixels are the RGBA8 bytes emitted by the recovered packed-YUV merge:
/// top-left rows and associated color.  Callers must preserve that alpha
/// representation; relabeling these bytes as straight alpha is forbidden.
struct QtFollowerDirectFrame final {
  std::uint32_t contractVersion{kQtFollowerDirectFrameContractVersion};
  std::uint32_t width{0U};
  std::uint32_t height{0U};
  std::size_t rowBytes{0U};
  std::shared_ptr<const std::vector<std::uint8_t>> pixels;
  std::string frameIdentity;
  std::uint64_t exactSourceFrameCount{0U};
};

class ImmutableResourceProvider final : public skresources::ResourceProvider {
public:
  explicit ImmutableResourceProvider(
      const std::vector<vector::VectorResource> &resources,
      bool requiredTypefaceClosure = false,
      std::shared_ptr<const RuntimeAnimatedImageSourceFactory>
          animatedImages = {});

  sk_sp<SkData> load(const char resourcePath[],
                     const char resourceName[]) const override;
  sk_sp<skresources::ImageAsset>
  loadImageAsset(const char resourcePath[], const char resourceName[],
                 const char resourceId[]) const override;
  sk_sp<SkTypeface> loadTypeface(const char name[],
                                 const char url[]) const override;

  bool resourceFailureObserved() const noexcept {
    return resourceFailureObserved_->load(std::memory_order_relaxed);
  }

  bool SelectQtByProgressSourceFrame(std::uint32_t contractVersion,
                                     double progress,
                                     std::uint64_t &sourceFrameIndex,
                                     std::string &error) noexcept;
  void ClearSourceFrameSelection() noexcept;

  /// Resolves one packed-YUV resource without crossing Skottie's
  /// ImageAsset or an intermediate SkSurface.  The logical id must identify
  /// an exact immutable packed-alpha resource; ordinary images fail closed.
  bool ResolveQtFollowerDirectFrame(
      const std::string &logicalId, std::int64_t sourceTimeUs,
      std::optional<std::uint64_t> sourceFrameIndex,
      QtFollowerDirectFrame &output, std::string &error) const noexcept;

private:
  struct Entry final {
    std::string logicalId;
    std::string mediaType;
    std::shared_ptr<const std::vector<std::uint8_t>> bytes;
  };

  const Entry *Find(const char resourcePath[], const char resourceName[],
                    const char resourceId[] = nullptr) const;

  std::unordered_map<std::string, Entry> resources_;
  bool requiredTypefaceClosure_{false};
  std::shared_ptr<const RuntimeAnimatedImageSourceFactory> animatedImages_;
  std::shared_ptr<AnimatedImageFrameSelectionState> frameSelection_;
  mutable std::mutex frameAdmissionMutex_;
  mutable std::mutex directFrameMutex_;
  mutable std::unordered_map<std::string,
                             std::shared_ptr<RuntimeAnimatedImageSource>>
      directFrameSources_;
  mutable bool packedSourceObserved_{false};
  mutable bool sourceFrameAdmissionValid_{true};
  mutable std::optional<std::uint64_t> exactSourceFrameCount_;
  // ImageAsset instances may legally outlive the provider that created them.
  std::shared_ptr<std::atomic<bool>> resourceFailureObserved_{
      std::make_shared<std::atomic<bool>>(false)};
};

sk_sp<SkFontMgr> MakeSystemFontManager();

} // namespace videocut::skia_runtime::internal
