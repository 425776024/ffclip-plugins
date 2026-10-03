#include "resources/SkiaResourceProvider.h"

#include "resources/QtPackedAlphaYuvMergeRuntime.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <mutex>
#include <new>
#include <string>
#include <utility>
#include <vector>

namespace videocut::skia_runtime::internal {

struct AnimatedImageFrameSelectionState final {
  std::mutex mutex;
  std::optional<std::uint64_t> sourceFrameIndex;
};

namespace {

std::string Join(const char path[], const char name[]) {
  std::string result = path ? path : "";
  if (!result.empty() && result.back() != '/')
    result.push_back('/');
  if (name)
    result.append(name);
  return result;
}

void RegisterImageCodecs() {
  static std::once_flag once;
  std::call_once(once, [] {
    SkCodecs::Register(SkPngDecoder::Decoder());
    SkCodecs::Register(SkJpegDecoder::Decoder());
    SkCodecs::Register(SkWebpDecoder::Decoder());
  });
}

bool SecondsToMicroseconds(const float seconds,
                           std::int64_t &microseconds) noexcept {
  if (!std::isfinite(seconds))
    return false;
  const double scaled = static_cast<double>(seconds) * 1'000'000.0;
  constexpr double kMinimum =
      static_cast<double>(std::numeric_limits<std::int64_t>::min());
  constexpr double kMaximum =
      static_cast<double>(std::numeric_limits<std::int64_t>::max());
  // Reject the double endpoints too: INT64_MAX rounds to 2^63 as a double,
  // which is outside llround's representable result even though it compares
  // equal to static_cast<double>(INT64_MAX).
  if (!std::isfinite(scaled) || scaled <= kMinimum || scaled >= kMaximum)
    return false;
  microseconds = static_cast<std::int64_t>(std::llround(scaled));
  return true;
}

void ReleaseSharedBytes(const void *, void *context) {
  delete static_cast<
      std::shared_ptr<const std::vector<std::uint8_t>> *>(context);
}

void DumpQtFollowerDirectFrame(
    const std::string &logicalId,
    const std::optional<std::uint64_t> sourceFrameIndex,
    const std::int64_t sourceTimeUs,
    const QtPackedAlphaYuvMergeOutput &merged) noexcept {
  const char *root = std::getenv("VIDEOCUT_DUMP_QT_FOLLOWER_DIRECT_FRAME");
  if (root == nullptr || root[0] == '\0' || merged.pixels.empty())
    return;
  try {
    std::filesystem::path directory(root);
    std::filesystem::create_directories(directory);
    std::string safeId = logicalId;
    std::replace_if(safeId.begin(), safeId.end(),
                    [](const char value) {
                      return !(value >= 'a' && value <= 'z') &&
                             !(value >= 'A' && value <= 'Z') &&
                             !(value >= '0' && value <= '9') && value != '-' &&
                             value != '_';
                    },
                    '-');
    const auto coordinate = sourceFrameIndex
                                ? "frame-" + std::to_string(*sourceFrameIndex)
                                : "time-" + std::to_string(sourceTimeUs);
    const auto path = directory /
                      (safeId + "-" + coordinate + "-" +
                       std::to_string(merged.width) + "x" +
                       std::to_string(merged.height) + ".rgba");
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char *>(merged.pixels.data()),
                 static_cast<std::streamsize>(merged.pixels.size()));
  } catch (...) {
    // Debug capture is observational and must never change rendering.
  }
}

class QtPackedAlphaMp4ImageAsset final : public skresources::ImageAsset {
public:
  QtPackedAlphaMp4ImageAsset(
      std::shared_ptr<RuntimeAnimatedImageSource> source,
      std::shared_ptr<std::atomic<bool>> resourceFailureObserved,
      std::unique_ptr<QtPackedAlphaYuvMergeRuntime> packedYuvMerge,
      std::shared_ptr<AnimatedImageFrameSelectionState> frameSelection)
      : source_(std::move(source)),
        resourceFailureObserved_(std::move(resourceFailureObserved)),
        packedYuvMerge_(std::move(packedYuvMerge)),
        frameSelection_(std::move(frameSelection)) {}

  bool isMultiFrame() override { return true; }

  sk_sp<SkImage> getFrame(const float seconds) override {
    std::lock_guard<std::mutex> lock(mutex_);
    return ResolveLocked(seconds);
  }

  FrameData getFrameData(const float seconds) override {
    std::lock_guard<std::mutex> lock(mutex_);
    return {
        ResolveLocked(seconds),
        // Qt's recovered shader uses normalized linear sampling with explicit
        // one-texel seam guards. v1 freezes linear base-level sampling here.
        SkSamplingOptions(SkFilterMode::kLinear, SkMipmapMode::kNone),
        SkMatrix::I(),
        SizeFit::kNone,
    };
  }

private:
  void Fail() noexcept {
    if (resourceFailureObserved_)
      resourceFailureObserved_->store(true, std::memory_order_relaxed);
  }

  sk_sp<SkImage> ResolveLocked(const float seconds) {
    std::optional<std::uint64_t> sourceFrameIndex;
    if (frameSelection_) {
      std::lock_guard<std::mutex> selectionLock(frameSelection_->mutex);
      sourceFrameIndex = frameSelection_->sourceFrameIndex;
    }
    if (sourceFrameIndex) {
      if (!source_) {
        Fail();
        return nullptr;
      }
      return ResolvePackedYuvLocked(0, sourceFrameIndex);
    }
    std::int64_t sourceTimeUs = 0;
    if (!source_ || !SecondsToMicroseconds(seconds, sourceTimeUs)) {
      Fail();
      return nullptr;
    }

    return ResolvePackedYuvLocked(sourceTimeUs, std::nullopt);
  }

  sk_sp<SkImage> MakeRasterImage(
      std::shared_ptr<const std::vector<std::uint8_t>> output,
      const std::uint32_t width, const std::uint32_t height,
      const std::size_t rowBytes, const SkAlphaType alphaType) {
    if (!output || output->empty() || width == 0U || height == 0U ||
        width > static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
        height >
            static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
      Fail();
      return nullptr;
    }
    auto data = RetainImmutableSkData(std::move(output));
    if (!data) {
      Fail();
      return nullptr;
    }
    const auto info =
        SkImageInfo::Make(static_cast<int>(width), static_cast<int>(height),
                          kRGBA_8888_SkColorType, alphaType,
                          SkColorSpace::MakeSRGB());
    sk_sp<SkImage> image;
    try {
      image = SkImages::RasterFromData(info, std::move(data), rowBytes);
    } catch (...) {
      Fail();
      return nullptr;
    }
    if (!image)
      Fail();
    return image;
  }

  sk_sp<SkImage> ResolvePackedYuvLocked(
      const std::int64_t sourceTimeUs,
      const std::optional<std::uint64_t> sourceFrameIndex) {
    if (!packedYuvMerge_) {
      Fail();
      return nullptr;
    }

    RuntimePackedYuvFrame packedFrame;
    std::string error;
    bool frameResolved = false;
    try {
      frameResolved = sourceFrameIndex
                          ? source_->GetPackedYuvFrameBySourceIndex(
                                *sourceFrameIndex, packedFrame, error)
                          : source_->GetPackedYuvFrame(sourceTimeUs,
                                                        packedFrame, error);
    } catch (...) {
      Fail();
      return nullptr;
    }
    if (!frameResolved ||
        packedFrame.contractVersion !=
            kRuntimePackedYuvFrameContractVersion ||
        !packedFrame.decodedFrame || packedFrame.frameIdentity.empty()) {
      Fail();
      return nullptr;
    }

    const auto &description = packedFrame.decodedFrame.desc();
    if (description.format != frame::PixelFormat::Yuv420p ||
        description.data_type != frame::DataType::U8 ||
        description.plane_count != 3U ||
        description.rotation != frame::Rotation::R0 ||
        description.width < 4U || (description.width & 3U) != 0U ||
        description.height < 2U || (description.height & 1U) != 0U ||
        description.width / 2U >
            static_cast<std::uint32_t>(std::numeric_limits<int>::max()) ||
        description.height >
            static_cast<std::uint32_t>(std::numeric_limits<int>::max())) {
      Fail();
      return nullptr;
    }
    if (packedWidth_ == 0U) {
      packedWidth_ = description.width;
      packedHeight_ = description.height;
    } else if (description.width != packedWidth_ ||
               description.height != packedHeight_) {
      Fail();
      return nullptr;
    }
    if (cachedImage_ &&
        cachedFrameIdentity_ == packedFrame.frameIdentity) {
      return cachedImage_;
    }

    QtPackedAlphaYuvMergeOutput merged;
    bool rendered = false;
    try {
      rendered = packedYuvMerge_->Render(
          packedFrame.decodedFrame, QtPackedAlphaYuvMergeLimits{}, merged,
          error);
    } catch (...) {
      Fail();
      return nullptr;
    }
    const std::uint32_t expectedWidth = description.width / 2U;
    if (!rendered || merged.width != expectedWidth ||
        merged.height != description.height ||
        merged.width > std::numeric_limits<std::uint32_t>::max() / 4U) {
      Fail();
      return nullptr;
    }
    const std::size_t outputRowBytes =
        static_cast<std::size_t>(merged.width) * 4U;
    if (merged.height > std::numeric_limits<std::size_t>::max() /
                            outputRowBytes ||
        merged.pixels.size() != outputRowBytes * merged.height) {
      Fail();
      return nullptr;
    }

    std::shared_ptr<const std::vector<std::uint8_t>> output;
    try {
      output = std::make_shared<const std::vector<std::uint8_t>>(
          std::move(merged.pixels));
    } catch (...) {
      Fail();
      return nullptr;
    }
    // The right half of the packed source is already associated color. The
    // recovered merge shader copies it verbatim and Skia must therefore see a
    // premultiplied boundary; multiplying by reconstructed alpha again is the
    // dark/blur regression this v2 contract removes.
    auto image = MakeRasterImage(std::move(output), expectedWidth,
                                 description.height, outputRowBytes,
                                 kPremul_SkAlphaType);
    if (!image)
      return nullptr;
    cachedFrameIdentity_ = std::move(packedFrame.frameIdentity);
    cachedImage_ = image;
    return image;
  }

  std::shared_ptr<RuntimeAnimatedImageSource> source_;
  std::shared_ptr<std::atomic<bool>> resourceFailureObserved_;
  std::unique_ptr<QtPackedAlphaYuvMergeRuntime> packedYuvMerge_;
  std::shared_ptr<AnimatedImageFrameSelectionState> frameSelection_;
  std::mutex mutex_;
  std::uint32_t packedWidth_{0};
  std::uint32_t packedHeight_{0};
  std::string cachedFrameIdentity_;
  sk_sp<SkImage> cachedImage_;
};

} // namespace

sk_sp<SkData> RetainImmutableSkData(
    std::shared_ptr<const std::vector<std::uint8_t>> bytes) noexcept {
  if (!bytes || bytes->empty())
    return nullptr;
  auto owner = std::unique_ptr<std::shared_ptr<const std::vector<std::uint8_t>>>(
      new (std::nothrow) std::shared_ptr<const std::vector<std::uint8_t>>(
          std::move(bytes)));
  if (!owner)
    return nullptr;
  try {
    auto data = SkData::MakeWithProc((*owner)->data(), (*owner)->size(),
                                   &ReleaseSharedBytes, owner.get());
    if (data)
      owner.release();
    return data;
  } catch (...) {
    return nullptr;
  }
}

ImmutableResourceProvider::ImmutableResourceProvider(
    const std::vector<vector::VectorResource> &resources,
    const bool requiredTypefaceClosure,
    std::shared_ptr<const RuntimeAnimatedImageSourceFactory> animatedImages)
    : requiredTypefaceClosure_(requiredTypefaceClosure),
      animatedImages_(std::move(animatedImages)),
      frameSelection_(std::make_shared<AnimatedImageFrameSelectionState>()) {
  RegisterImageCodecs();
  resources_.reserve(resources.size());
  for (const auto &resource : resources) {
    if (resource.bytes) {
      resources_.emplace(resource.logicalName,
                         Entry{resource.logicalName, resource.mediaType,
                               resource.bytes});
    }
  }
}

const ImmutableResourceProvider::Entry *
ImmutableResourceProvider::Find(const char resourcePath[],
                                const char resourceName[],
                                const char resourceId[]) const {
  const std::string joined = Join(resourcePath, resourceName);
  const std::string candidates[] = {
      joined,
      resourceName ? resourceName : "",
      resourceId ? resourceId : "",
  };
  for (const auto &candidate : candidates) {
    if (candidate.empty())
      continue;
    const auto found = resources_.find(candidate);
    if (found != resources_.end())
      return &found->second;
  }
  return nullptr;
}

sk_sp<SkData> ImmutableResourceProvider::load(const char resourcePath[],
                                              const char resourceName[]) const {
  const Entry *entry = Find(resourcePath, resourceName);
  if (!entry || !entry->bytes) {
    resourceFailureObserved_->store(true, std::memory_order_relaxed);
    return nullptr;
  }
  auto data = SkData::MakeWithCopy(entry->bytes->data(), entry->bytes->size());
  if (!data) {
    resourceFailureObserved_->store(true, std::memory_order_relaxed);
  }
  return data;
}

sk_sp<skresources::ImageAsset>
ImmutableResourceProvider::loadImageAsset(const char resourcePath[],
                                          const char resourceName[],
                                          const char resourceId[]) const {
  const Entry *entry = Find(resourcePath, resourceName, resourceId);
  if (!entry || !entry->bytes) {
    resourceFailureObserved_->store(true, std::memory_order_relaxed);
    return nullptr;
  }
  const bool packed =
      entry->mediaType == kQtPackedAlphaMp4ResourceMediaType;
  if (packed) {
    if (entry->bytes->empty() || !animatedImages_) {
      resourceFailureObserved_->store(true, std::memory_order_relaxed);
      return nullptr;
    }
    RuntimeAnimatedImageOpenRequest request;
    request.contractVersion = kRuntimePackedYuvFrameContractVersion;
    request.logicalId = entry->logicalId;
    request.mediaType = entry->mediaType;
    request.capability = std::string(kQtPackedAlphaMp4Capability);
    request.bytes = entry->bytes;
    std::string error;
    std::shared_ptr<RuntimeAnimatedImageSource> source;
    try {
      source = animatedImages_->Create(request, error);
    } catch (...) {
      resourceFailureObserved_->store(true, std::memory_order_relaxed);
      return nullptr;
    }
    if (!source) {
      resourceFailureObserved_->store(true, std::memory_order_relaxed);
      return nullptr;
    }
    {
      std::lock_guard<std::mutex> lock(frameAdmissionMutex_);
      packedSourceObserved_ = true;
      const auto frameCount = source->ExactSourceFrameCount();
      if (!frameCount || *frameCount == 0U ||
          *frameCount > 9'007'199'254'740'991ULL ||
          (exactSourceFrameCount_ &&
           *exactSourceFrameCount_ != *frameCount)) {
        sourceFrameAdmissionValid_ = false;
        exactSourceFrameCount_.reset();
      } else {
        exactSourceFrameCount_ = *frameCount;
      }
    }
    std::unique_ptr<QtPackedAlphaYuvMergeRuntime> packedYuvMerge;
    try {
      packedYuvMerge = CreateQtPackedAlphaYuvMergeRuntime(error);
    } catch (...) {
      resourceFailureObserved_->store(true, std::memory_order_relaxed);
      return nullptr;
    }
    if (!packedYuvMerge) {
      resourceFailureObserved_->store(true, std::memory_order_relaxed);
      return nullptr;
    }
    auto *asset = new (std::nothrow) QtPackedAlphaMp4ImageAsset(
        std::move(source), resourceFailureObserved_,
        std::move(packedYuvMerge), frameSelection_);
    if (!asset) {
      resourceFailureObserved_->store(true, std::memory_order_relaxed);
      return nullptr;
    }
    return sk_sp<skresources::ImageAsset>(asset);
  }
  RegisterImageCodecs();
  auto image = skresources::MultiFrameImageAsset::Make(
      SkData::MakeWithCopy(entry->bytes->data(), entry->bytes->size()),
      skresources::ImageDecodeStrategy::kLazyDecode);
  if (!image) {
    resourceFailureObserved_->store(true, std::memory_order_relaxed);
  }
  return image;
}

bool ImmutableResourceProvider::SelectQtByProgressSourceFrame(
    const std::uint32_t contractVersion, const double progress,
    std::uint64_t &sourceFrameIndex, std::string &error) noexcept {
  sourceFrameIndex = 0U;
  if (contractVersion !=
          vector::kVectorAssetSamplingQtVideoAnimSeqByProgress ||
      !std::isfinite(progress) || progress < 0.0 || progress > 1.0) {
    error = "Qt ByProgress source-frame request contract is invalid";
    ClearSourceFrameSelection();
    return false;
  }
  std::uint64_t frameCount = 0U;
  {
    std::lock_guard<std::mutex> lock(frameAdmissionMutex_);
    if (!packedSourceObserved_ || !sourceFrameAdmissionValid_ ||
        !exactSourceFrameCount_) {
      error = "Qt ByProgress requires one exact packed source-frame "
              "admission";
      ClearSourceFrameSelection();
      return false;
    }
    frameCount = *exactSourceFrameCount_;
  }
  if (!vector::ResolveQtByProgressSourceFrame(
          contractVersion, progress, frameCount, sourceFrameIndex)) {
    error = "Qt ByProgress source-frame coordinate is invalid";
    ClearSourceFrameSelection();
    return false;
  }
  {
    std::lock_guard<std::mutex> lock(frameSelection_->mutex);
    frameSelection_->sourceFrameIndex = sourceFrameIndex;
  }
  error.clear();
  return true;
}

void ImmutableResourceProvider::ClearSourceFrameSelection() noexcept {
  if (!frameSelection_)
    return;
  std::lock_guard<std::mutex> lock(frameSelection_->mutex);
  frameSelection_->sourceFrameIndex.reset();
}

bool ImmutableResourceProvider::ResolveQtFollowerDirectFrame(
    const std::string &logicalId, const std::int64_t sourceTimeUs,
    const std::optional<std::uint64_t> sourceFrameIndex,
    QtFollowerDirectFrame &output, std::string &error) const noexcept {
  output = {};
  const auto fail = [&](std::string message) {
    resourceFailureObserved_->store(true, std::memory_order_relaxed);
    error = std::move(message);
    output = {};
    return false;
  };
  if (logicalId.empty())
    return fail("Qt follower direct-frame logical id is empty");
  const auto found = resources_.find(logicalId);
  if (found == resources_.end() ||
      found->second.mediaType != kQtPackedAlphaMp4ResourceMediaType ||
      !found->second.bytes || found->second.bytes->empty() ||
      !animatedImages_) {
    return fail("Qt follower direct-frame requires one exact immutable "
                "packed-YUV resource");
  }

  try {
    // RuntimeAnimatedImageSource implementations are not required to be
    // reentrant.  Keep source creation, exact-index decode and merge ordered
    // just like Qt's VideoAnimSeq instance.
    std::lock_guard<std::mutex> lock(directFrameMutex_);
    auto &source = directFrameSources_[logicalId];
    if (!source) {
      RuntimeAnimatedImageOpenRequest request;
      request.contractVersion = kRuntimePackedYuvFrameContractVersion;
      request.logicalId = found->second.logicalId;
      request.mediaType = found->second.mediaType;
      request.capability = std::string(kQtPackedAlphaMp4Capability);
      request.bytes = found->second.bytes;
      source = animatedImages_->Create(request, error);
      if (!source)
        return fail(error.empty()
                        ? "Qt follower direct-frame source creation failed"
                        : std::move(error));
    }

    const auto frameCount = source->ExactSourceFrameCount();
    if (!frameCount || *frameCount == 0U ||
        *frameCount > 9'007'199'254'740'991ULL) {
      return fail("Qt follower direct-frame requires an exact source-frame "
                  "count");
    }
    if (sourceFrameIndex && *sourceFrameIndex >= *frameCount) {
      return fail("Qt follower direct-frame source index exceeded its exact "
                  "frame count");
    }

    RuntimePackedYuvFrame packedFrame;
    const bool resolved = sourceFrameIndex
                              ? source->GetPackedYuvFrameBySourceIndex(
                                    *sourceFrameIndex, packedFrame, error)
                              : source->GetPackedYuvFrame(
                                    sourceTimeUs, packedFrame, error);
    if (!resolved ||
        packedFrame.contractVersion !=
            kRuntimePackedYuvFrameContractVersion ||
        !packedFrame.decodedFrame || packedFrame.frameIdentity.empty()) {
      return fail(error.empty()
                      ? "Qt follower direct-frame decode failed"
                      : std::move(error));
    }

    const auto &description = packedFrame.decodedFrame.desc();
    if (description.format != frame::PixelFormat::Yuv420p ||
        description.data_type != frame::DataType::U8 ||
        description.plane_count != 3U ||
        description.rotation != frame::Rotation::R0 ||
        description.width < 4U || (description.width & 3U) != 0U ||
        description.height < 2U || (description.height & 1U) != 0U) {
      return fail("Qt follower direct-frame decoded geometry is outside the "
                  "packed-YUV contract");
    }

    auto merge = CreateQtPackedAlphaYuvMergeRuntime(error);
    if (!merge)
      return fail(error.empty()
                      ? "Qt follower direct-frame merge runtime is unavailable"
                      : std::move(error));
    QtPackedAlphaYuvMergeOutput merged;
    if (!merge->Render(packedFrame.decodedFrame,
                       QtPackedAlphaYuvMergeLimits{}, merged, error)) {
      return fail(error.empty()
                      ? "Qt follower direct-frame packed-YUV merge failed"
                      : std::move(error));
    }
    const std::uint32_t expectedWidth = description.width / 2U;
    if (merged.width != expectedWidth ||
        merged.height != description.height ||
        merged.width > std::numeric_limits<std::uint32_t>::max() / 4U) {
      return fail("Qt follower direct-frame merge geometry is invalid");
    }
    const std::size_t rowBytes =
        static_cast<std::size_t>(merged.width) * 4U;
    if (merged.height >
            std::numeric_limits<std::size_t>::max() / rowBytes ||
        merged.pixels.size() != rowBytes * merged.height) {
      return fail("Qt follower direct-frame merge byte extent is invalid");
    }

    DumpQtFollowerDirectFrame(logicalId, sourceFrameIndex, sourceTimeUs,
                              merged);

    output.contractVersion = kQtFollowerDirectFrameContractVersion;
    output.width = merged.width;
    output.height = merged.height;
    output.rowBytes = rowBytes;
    output.pixels =
        std::make_shared<const std::vector<std::uint8_t>>(
            std::move(merged.pixels));
    output.frameIdentity = std::move(packedFrame.frameIdentity);
    output.exactSourceFrameCount = *frameCount;
    error.clear();
    return true;
  } catch (const std::exception &exception) {
    return fail(exception.what());
  } catch (...) {
    return fail("Qt follower direct-frame resolution raised an unknown "
                "exception");
  }
}

sk_sp<SkTypeface>
ImmutableResourceProvider::loadTypeface(const char name[],
                                        const char url[]) const {
  const Entry *entry = Find("", url, name);
  if (!entry || !entry->bytes) {
    if (requiredTypefaceClosure_) {
      resourceFailureObserved_->store(true, std::memory_order_relaxed);
    }
    return nullptr;
  }
  std::vector<sk_sp<SkData>> fonts;
  fonts.push_back(
      SkData::MakeWithCopy(entry->bytes->data(), entry->bytes->size()));
  if (!fonts.front()) {
    resourceFailureObserved_->store(true, std::memory_order_relaxed);
    return nullptr;
  }
  auto manager = SkFontMgr_New_Custom_Data(SkSpan(fonts));
  // The immutable resource is the authority. Lottie family strings are
  // legacy aliases and must never redirect selection to a platform font.
  auto typeface = manager ? manager->makeFromData(fonts.front(), 0) : nullptr;
  if (!typeface) {
    resourceFailureObserved_->store(true, std::memory_order_relaxed);
  }
  return typeface;
}

sk_sp<SkFontMgr> MakeSystemFontManager() {
#if defined(__APPLE__)
  return SkFontMgr_New_CoreText(nullptr);
#elif defined(_WIN32)
  return SkFontMgr_New_DirectWrite();
#else
  return nullptr;
#endif
}

} // namespace videocut::skia_runtime::internal
