#include "text/TextRuntimeShader.h"
#include "text/QtTextPostEffectRuntime.h"
#include "text/QtTextPostEffectParameters.h"

#include "text/QtTextDirectionalBlursRuntime.h"
#include "text/QtTextDistortChromaMetalRuntime.h"
#include "text/QtTextDustMetalRuntime.h"
#include "text/QtTextEngineCopyMetalRuntime.h"
#include "text/QtTextGaussianMetalRuntime.h"
#include "text/QtTextRadialBlurMetalRuntime.h"
#include "text/QtTextShakeMetalRuntime.h"
#include "text/QtTextSoftGlowMetalRuntime.h"
#include "text/QtTextTrailRuntime.h"
#include "text/QtTextTurbulenceMetalRuntime.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <limits>
#include <memory>
#include <mutex>
#include <sstream>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace videocut::skia_runtime::internal {
using namespace post_effect;
namespace {

sk_sp<SkShader> RawLinearShader(const sk_sp<SkImage> &image) {
  return image ? image->makeRawShader(SkTileMode::kClamp, SkTileMode::kClamp,
                                      SkSamplingOptions(SkFilterMode::kLinear))
               : sk_sp<SkShader>{};
}

sk_sp<SkShader> RawNearestShader(const sk_sp<SkImage> &image) {
  return image ? image->makeRawShader(SkTileMode::kClamp, SkTileMode::kClamp,
                                      SkSamplingOptions(SkFilterMode::kNearest))
               : sk_sp<SkShader>{};
}

SkImageInfo StageImageInfo(const int width, const int height,
                           const QtTextPostEffectSurfaceSemantics semantics) {
  // SkRuntimeEffect shader outputs use Skia's premultiplied shader-value
  // convention. Marking an RGBA8 render target kUnpremul makes RasterPipeline
  // (and Ganesh where supported) unpremultiply the four output values, which
  // destroys independent data channels whenever RGB>A. RawRgbaData therefore
  // uses a premul *transport tag* while forbidding all color-image operations:
  // every write is kSrc, every child is makeRawShader, and no color space is
  // attached. The focused Raster/Metal probe locks this byte contract.
  (void)semantics;
  return SkImageInfo::Make(width, height, kRGBA_8888_SkColorType,
                           kPremul_SkAlphaType, nullptr);
}

bool ShouldDumpStage(const float progress) {
  const char *root = std::getenv("VIDEOCUT_DUMP_TEXT_POST_EFFECTS");
  if (root == nullptr || root[0] == '\0')
    return false;
  const int frameMilli = static_cast<int>(std::lround(progress * 1000.0F));
  if (const char *frameFilter =
          std::getenv("VIDEOCUT_DUMP_TEXT_POST_EFFECT_FRAME");
      frameFilter != nullptr && frameFilter[0] != '\0' &&
      std::strtol(frameFilter, nullptr, 10) != frameMilli) {
    return false;
  }
  return true;
}

void DumpStage(const sk_sp<SkImage> &image, const float progress,
               const std::string_view stage, SkiaGpuContext *gpuContext,
               const QtTextPostEffectSurfaceSemantics semantics =
                   QtTextPostEffectSurfaceSemantics::ColorPremultiplied) {
  if (!image || !ShouldDumpStage(progress))
    return;
  const char *root = std::getenv("VIDEOCUT_DUMP_TEXT_POST_EFFECTS");
  const int frameMilli = static_cast<int>(std::lround(progress * 1000.0F));
  const auto info = StageImageInfo(image->width(), image->height(), semantics);
  std::string error;
  auto surface = gpuContext ? gpuContext->MakeSurface(info, error)
                            : SkSurfaces::Raster(info);
  if (!surface)
    return;
  SkPaint paint;
  paint.setAntiAlias(false);
  paint.setBlendMode(SkBlendMode::kSrc);
  surface->getCanvas()->clear(SK_ColorTRANSPARENT);
  if (semantics == QtTextPostEffectSurfaceSemantics::RawRgbaData) {
    paint.setShader(
        image->makeRawShader(SkTileMode::kClamp, SkTileMode::kClamp,
                             SkSamplingOptions(SkFilterMode::kNearest)));
    surface->getCanvas()->drawRect(
        SkRect::MakeWH(static_cast<float>(image->width()),
                       static_cast<float>(image->height())),
        paint);
  } else {
    surface->getCanvas()->drawImageRect(
        image.get(),
        SkRect::MakeWH(static_cast<float>(image->width()),
                       static_cast<float>(image->height())),
        SkSamplingOptions(SkFilterMode::kNearest), &paint);
  }
  const std::size_t rowBytes = static_cast<std::size_t>(image->width()) * 4U;
  std::vector<std::uint8_t> pixels(
      rowBytes * static_cast<std::size_t>(image->height()), 0U);
  if (!surface->readPixels(info, pixels.data(), rowBytes, 0, 0))
    return;
  std::error_code directoryError;
  const std::filesystem::path directory(root);
  std::filesystem::create_directories(directory, directoryError);
  if (directoryError)
    return;
  std::ostringstream filename;
  filename << "frame-" << std::setw(3) << std::setfill('0') << frameMilli
           << "-current-qt-contract-" << stage << '-' << image->width() << 'x'
           << image->height() << ".rgba";
  std::ofstream output(directory / filename.str(),
                       std::ios::binary | std::ios::trunc);
  output.write(reinterpret_cast<const char *>(pixels.data()),
               static_cast<std::streamsize>(pixels.size()));
}

class StageRenderer final {
public:
  StageRenderer(SkiaGpuContext *gpuContext, std::string &error)
      : gpuContext_(gpuContext), error_(error) {}

  SkiaGpuContext *GpuContext() const noexcept { return gpuContext_; }

  sk_sp<SkSurface> MakeSurface(const SkImageInfo &info) const {
    return gpuContext_ ? gpuContext_->MakeSurface(info, error_)
                       : SkSurfaces::Raster(info);
  }

  sk_sp<SkImage> Draw(const SkImageInfo &info, sk_sp<SkShader> shader) const {
    if (!shader)
      return {};
    auto surface = MakeSurface(info);
    if (!surface)
      return {};
    SkPaint paint;
    paint.setAntiAlias(false);
    paint.setBlendMode(SkBlendMode::kSrc);
    paint.setShader(std::move(shader));
    surface->getCanvas()->clear(SK_ColorTRANSPARENT);
    surface->getCanvas()->drawRect(
        SkRect::MakeWH(static_cast<float>(info.width()),
                       static_cast<float>(info.height())),
        paint);
    return surface->makeImageSnapshot();
  }

private:
  SkiaGpuContext *gpuContext_{nullptr};
  std::string &error_;
};

const char *
SurfaceSemanticsName(const QtTextPostEffectSurfaceSemantics semantics) {
  switch (semantics) {
  case QtTextPostEffectSurfaceSemantics::ColorPremultiplied:
    return "color_premultiplied";
  case QtTextPostEffectSurfaceSemantics::RawRgbaData:
    return "raw_rgba_data";
  }
  return "unknown";
}

void RecordExecutedPass(QtTextPostEffectExecutionTrace *executionTrace,
                        const char *chain, const char *stage,
                        const std::size_t ordinal, const std::size_t passCount,
                        const SkImageInfo &info,
                        const QtTextPostEffectSurfaceSemantics semantics,
                        const char *executor = "skia-runtime-effect",
                        const std::uint32_t executorVersion = 0U,
                        const char *pixelFormat = "RGBA8Unorm") {
  if (executionTrace) {
    executionTrace->passes.push_back({chain, stage, ordinal, passCount,
                                      info.width(), info.height(), semantics,
                                      executor, executorVersion, pixelFormat});
  }
  if (std::getenv("VIDEOCUT_TRACE_TEXT_QT_POSTFX") != nullptr) {
    std::fprintf(stderr,
                 "[VIDEOCUT_TEXT_QT_POSTFX_PASS] chain=%s pass=%zu/%zu "
                 "stage=%s target=%dx%d semantics=%s executor=%s "
                 "executor_version=%u pixel_format=%s status=executed\n",
                 chain, ordinal, passCount, stage, info.width(), info.height(),
                 SurfaceSemanticsName(semantics), executor, executorVersion,
                 pixelFormat);
  }
}

struct EngineCopyRuntimeHolder final {
  std::once_flag initialization;
  std::mutex renderMutex;
  std::unique_ptr<QtTextEngineCopyMetalRuntime> runtime;
  std::string unavailableReason;
};

EngineCopyRuntimeHolder &EngineCopyRuntime() {
  static EngineCopyRuntimeHolder holder;
  std::call_once(holder.initialization, []() {
    holder.runtime =
        CreateQtTextEngineCopyMetalRuntime(holder.unavailableReason);
    if (!holder.runtime && holder.unavailableReason.empty()) {
      holder.unavailableReason =
          "Qt EngineCopy native Metal v1 returned no runtime";
    }
  });
  return holder;
}

struct DistortChromaMetalRuntimeHolder final {
  std::once_flag initialization;
  std::mutex renderMutex;
  std::unique_ptr<QtTextDistortChromaMetalRuntime> runtime;
  std::string unavailableReason;
};

DistortChromaMetalRuntimeHolder &DistortChromaMetalRuntime() {
  static DistortChromaMetalRuntimeHolder holder;
  std::call_once(holder.initialization, []() {
    holder.runtime =
        CreateQtTextDistortChromaMetalRuntime(holder.unavailableReason);
    if (!holder.runtime && holder.unavailableReason.empty()) {
      holder.unavailableReason =
          "Qt LumiDistortChroma native Metal runtime returned no runtime";
    }
  });
  return holder;
}

struct RadialBlurMetalRuntimeHolder final {
  std::once_flag initialization;
  std::mutex renderMutex;
  std::unique_ptr<QtTextRadialBlurMetalRuntime> runtime;
  std::string unavailableReason;
};

RadialBlurMetalRuntimeHolder &RadialBlurMetalRuntime() {
  static RadialBlurMetalRuntimeHolder holder;
  std::call_once(holder.initialization, []() {
    holder.runtime =
        CreateQtTextRadialBlurMetalRuntime(holder.unavailableReason);
    if (!holder.runtime && holder.unavailableReason.empty()) {
      holder.unavailableReason =
          "Qt LumiRadialBlur native Metal v1 returned no runtime";
    }
  });
  return holder;
}

struct ShakeMetalRuntimeHolder final {
  std::once_flag initialization;
  std::mutex renderMutex;
  std::unique_ptr<QtTextShakeMetalRuntime> runtime;
  std::string unavailableReason;
};

ShakeMetalRuntimeHolder &ShakeMetalRuntime() {
  static ShakeMetalRuntimeHolder holder;
  std::call_once(holder.initialization, []() {
    holder.runtime = CreateQtTextShakeMetalRuntime(holder.unavailableReason);
    if (!holder.runtime && holder.unavailableReason.empty()) {
      holder.unavailableReason =
          "Qt LumiSShake native Metal v1 returned no runtime";
    }
  });
  return holder;
}

struct GaussianMetalRuntimeHolder final {
  std::once_flag initialization;
  std::mutex renderMutex;
  std::unique_ptr<QtTextGaussianMetalRuntime> runtime;
  std::string unavailableReason;
};

GaussianMetalRuntimeHolder &GaussianMetalRuntime() {
  static GaussianMetalRuntimeHolder holder;
  std::call_once(holder.initialization, []() {
    holder.runtime = CreateQtTextGaussianMetalRuntime(holder.unavailableReason);
    if (!holder.runtime && holder.unavailableReason.empty()) {
      holder.unavailableReason =
          "Qt LumiGaussianBlur native Metal v1 returned no runtime";
    }
  });
  return holder;
}

struct SoftGlowMetalRuntimeHolder final {
  std::once_flag initialization;
  std::mutex renderMutex;
  std::unique_ptr<QtTextSoftGlowMetalRuntime> runtime;
  std::string unavailableReason;
};

SoftGlowMetalRuntimeHolder &SoftGlowMetalRuntime() {
  static SoftGlowMetalRuntimeHolder holder;
  std::call_once(holder.initialization, []() {
    holder.runtime = CreateQtTextSoftGlowMetalRuntime(holder.unavailableReason);
    if (!holder.runtime && holder.unavailableReason.empty()) {
      holder.unavailableReason =
          "Qt LumiSoftGlow native Metal v1 returned no runtime";
    }
  });
  return holder;
}

struct TurbulenceMetalRuntimeHolder final {
  std::once_flag initialization;
  std::mutex renderMutex;
  std::unique_ptr<QtTextTurbulenceMetalRuntime> runtime;
  std::string unavailableReason;
};

TurbulenceMetalRuntimeHolder &TurbulenceMetalRuntime() {
  static TurbulenceMetalRuntimeHolder holder;
  std::call_once(holder.initialization, []() {
    holder.runtime =
        CreateQtTextTurbulenceMetalRuntime(holder.unavailableReason);
    if (!holder.runtime && holder.unavailableReason.empty()) {
      holder.unavailableReason =
          "Qt LumiTurbulenceDisplacement native Metal v1 returned no runtime";
    }
  });
  return holder;
}

struct DirectionalBlursMetalRuntimeHolder final {
  std::once_flag initialization;
  std::mutex renderMutex;
  std::unique_ptr<QtTextDirectionalBlursRuntime> runtime;
  std::string unavailableReason;
};

DirectionalBlursMetalRuntimeHolder &DirectionalBlursMetalRuntime() {
  static DirectionalBlursMetalRuntimeHolder holder;
  std::call_once(holder.initialization, []() {
    holder.runtime =
        CreateQtTextDirectionalBlursRuntime(holder.unavailableReason);
    if (!holder.runtime && holder.unavailableReason.empty()) {
      holder.unavailableReason =
          "Qt DirectionalBlurs native Metal v1 returned no runtime";
    }
  });
  return holder;
}

struct DustMetalRuntimeHolder final {
  std::once_flag initialization;
  std::mutex renderMutex;
  std::unique_ptr<QtTextDustMetalRuntime> runtime;
  std::string unavailableReason;
};

DustMetalRuntimeHolder &DustMetalRuntime() {
  static DustMetalRuntimeHolder holder;
  std::call_once(holder.initialization, []() {
    holder.runtime = CreateQtTextDustMetalRuntime(holder.unavailableReason);
    if (!holder.runtime && holder.unavailableReason.empty())
      holder.unavailableReason = "Qt LumiDust native Metal v2 returned no runtime";
  });
  return holder;
}

struct TrailRuntimeEntry final {
  std::unique_ptr<QtTextTrailRuntime> runtime;
  std::string initializationError;
};

struct TrailRuntimeKey final {
  std::uint64_t renderGraphInstanceId{0U};
  std::uint64_t effectNodeInstanceId{0U};

  bool operator==(const TrailRuntimeKey &other) const noexcept {
    return renderGraphInstanceId == other.renderGraphInstanceId &&
           effectNodeInstanceId == other.effectNodeInstanceId;
  }
};

struct TrailRuntimeKeyHash final {
  std::size_t operator()(const TrailRuntimeKey &key) const noexcept {
    std::uint64_t hash = key.renderGraphInstanceId;
    hash ^= key.effectNodeInstanceId + 0x9e3779b97f4a7c15ULL + (hash << 6U) +
            (hash >> 2U);
    if constexpr (sizeof(std::size_t) < sizeof(std::uint64_t))
      hash ^= hash >> 32U;
    return static_cast<std::size_t>(hash);
  }
};

struct TrailRuntimeRegistry final {
  std::mutex mutex;
  std::unordered_map<TrailRuntimeKey, TrailRuntimeEntry, TrailRuntimeKeyHash>
      entries;
};

TrailRuntimeRegistry &TrailRuntimes() {
  static TrailRuntimeRegistry registry;
  return registry;
}

struct PendingDirectionalBlurKey final {
  std::uint64_t renderGraphInstanceId{0U};
  std::uint64_t lifecycleEpoch{0U};
  std::string nodeId;

  bool operator==(const PendingDirectionalBlurKey &other) const noexcept {
    return renderGraphInstanceId == other.renderGraphInstanceId &&
           lifecycleEpoch == other.lifecycleEpoch && nodeId == other.nodeId;
  }
};

struct PendingDirectionalBlurKeyHash final {
  std::size_t operator()(const PendingDirectionalBlurKey &key) const noexcept {
    std::size_t hash = std::hash<std::uint64_t>{}(key.renderGraphInstanceId);
    hash ^= std::hash<std::uint64_t>{}(key.lifecycleEpoch) +
            0x9e3779b97f4a7c15ULL + (hash << 6U) + (hash >> 2U);
    hash ^= std::hash<std::string>{}(key.nodeId) + 0x9e3779b97f4a7c15ULL +
            (hash << 6U) + (hash >> 2U);
    return hash;
  }
};

struct PendingDirectionalBlur final {
  sk_sp<SkPicture> source;
  text::TextEffectPostEffectNode effect;
  double progress{0.0};
};

struct PendingDirectionalBlurRegistry final {
  std::mutex mutex;
  std::unordered_map<PendingDirectionalBlurKey, PendingDirectionalBlur,
                     PendingDirectionalBlurKeyHash>
      entries;
};

PendingDirectionalBlurRegistry &PendingDirectionalBlurs() {
  static PendingDirectionalBlurRegistry registry;
  return registry;
}

bool StagePendingDirectionalBlur(
    const sk_sp<SkPicture> &source,
    const text::TextEffectPostEffectNode &effect, const double progress,
    const QtTextPostEffectStateContext *stateContext) {
  if (!source || !stateContext ||
      stateContext->renderGraphInstanceId == 0U || effect.nodeId.empty()) {
    return false;
  }
  PendingDirectionalBlurKey key{stateContext->renderGraphInstanceId,
                                stateContext->lifecycleEpoch, effect.nodeId};
  auto &registry = PendingDirectionalBlurs();
  std::lock_guard<std::mutex> lock(registry.mutex);
  registry.entries.insert_or_assign(
      std::move(key), PendingDirectionalBlur{source, effect, progress});
  return true;
}

bool TakePendingDirectionalBlur(
    const text::TextEffectPostEffectNode &alphaOutlineEffect,
    const QtTextPostEffectStateContext *stateContext,
    PendingDirectionalBlur &pending) {
  if (!stateContext || stateContext->renderGraphInstanceId == 0U)
    return false;
  auto &registry = PendingDirectionalBlurs();
  std::lock_guard<std::mutex> lock(registry.mutex);
  auto match = registry.entries.end();
  for (const auto &inputId : alphaOutlineEffect.inputIds) {
    const PendingDirectionalBlurKey key{stateContext->renderGraphInstanceId,
                                        stateContext->lifecycleEpoch, inputId};
    const auto found = registry.entries.find(key);
    if (found == registry.entries.end())
      continue;
    if (match != registry.entries.end())
      return false;
    match = found;
  }
  if (match == registry.entries.end())
    return false;
  pending = std::move(match->second);
  registry.entries.erase(match);
  return true;
}

std::uint64_t
TrailNodeIdentity(const QtTextPostEffectStateContext &context) noexcept {
  std::uint64_t hash = context.effectNodeInstanceId;
  hash ^= context.renderGroupInstanceId + 0x9e3779b97f4a7c15ULL + (hash << 6U) +
          (hash >> 2U);
  return hash == 0U ? 1U : hash;
}

enum class EngineCopyStatus : unsigned char {
  Executed,
  NativeUnavailable,
  NativeExecutionFailed,
};

struct EngineCopyImageResult final {
  EngineCopyStatus status{EngineCopyStatus::NativeUnavailable};
  sk_sp<SkImage> image;
  std::string diagnostic;
  std::string executor;
  std::uint32_t executorVersion{0U};
};

struct ShakeImageResult final {
  EngineCopyStatus status{EngineCopyStatus::NativeUnavailable};
  sk_sp<SkImage> outputImage;
  std::string diagnostic;
  std::string executor;
  std::uint32_t executorVersion{0U};
};

struct RadialBlurImageResult final {
  EngineCopyStatus status{EngineCopyStatus::NativeUnavailable};
  sk_sp<SkImage> outputImage;
  std::string diagnostic;
  std::string executor;
  std::uint32_t executorVersion{0U};
};

struct DistortChromaImageResult final {
  EngineCopyStatus status{EngineCopyStatus::NativeUnavailable};
  sk_sp<SkImage> lensImage;
  sk_sp<SkImage> blurX1Image;
  sk_sp<SkImage> blurY1Image;
  sk_sp<SkImage> blurX2Image;
  sk_sp<SkImage> blurY2Image;
  sk_sp<SkImage> outputImage;
  std::string diagnostic;
  std::string executor;
  std::uint32_t executorVersion{0U};
};

struct TurbulenceImageResult final {
  EngineCopyStatus status{EngineCopyStatus::NativeUnavailable};
  sk_sp<SkImage> noiseImage;
  sk_sp<SkImage> outputImage;
  std::string diagnostic;
  std::string executor;
  std::uint32_t executorVersion{0U};
};

struct TrailImageResult final {
  EngineCopyStatus status{EngineCopyStatus::NativeUnavailable};
  sk_sp<SkImage> outputImage;
  QtTextTrailRenderResult execution;
  std::string diagnostic;
  std::string executor;
  std::uint32_t executorVersion{0U};
};

bool ReadRawRgba8TopLeft(const sk_sp<SkImage> &image,
                         const StageRenderer &renderer,
                         std::vector<std::uint8_t> &pixels,
                         std::string &error) {
  pixels.clear();
  if (!image) {
    error = "Qt EngineCopy source image is null";
    return false;
  }
  const auto info =
      StageImageInfo(image->width(), image->height(),
                     QtTextPostEffectSurfaceSemantics::RawRgbaData);
  auto surface = renderer.MakeSurface(info);
  auto shader = RawNearestShader(image);
  if (!surface || !shader) {
    error = "Qt EngineCopy could not materialize its raw source surface";
    return false;
  }
  SkPaint paint;
  paint.setAntiAlias(false);
  paint.setBlendMode(SkBlendMode::kSrc);
  paint.setShader(std::move(shader));
  surface->getCanvas()->clear(SK_ColorTRANSPARENT);
  surface->getCanvas()->drawRect(
      SkRect::MakeWH(static_cast<float>(image->width()),
                     static_cast<float>(image->height())),
      paint);
  const std::size_t rowBytes = static_cast<std::size_t>(image->width()) * 4U;
  pixels.resize(rowBytes * static_cast<std::size_t>(image->height()));
  if (!surface->readPixels(info, pixels.data(), rowBytes, 0, 0)) {
    pixels.clear();
    error = "Qt EngineCopy could not read its top-left raw source bytes";
    return false;
  }
  error.clear();
  return true;
}

sk_sp<SkImage> RawRgba8TopLeftImage(std::vector<std::uint8_t> pixels,
                                    const int width, const int height) {
  const std::size_t rowBytes = static_cast<std::size_t>(width) * 4U;
  if (width <= 0 || height <= 0 ||
      pixels.size() != rowBytes * static_cast<std::size_t>(height)) {
    return {};
  }
  auto data = SkData::MakeWithCopy(pixels.data(), pixels.size());
  return data ? SkImages::RasterFromData(
                    StageImageInfo(
                        width, height,
                        QtTextPostEffectSurfaceSemantics::RawRgbaData),
                    std::move(data), rowBytes)
              : sk_sp<SkImage>{};
}

DistortChromaImageResult ExecuteNativeDistortChroma(
    const sk_sp<SkImage> &source, const QtDistortChromaContract &contract,
    const StageRenderer &renderer, const bool captureIntermediates) {
  DistortChromaImageResult result;
  auto *gpuContext = renderer.GpuContext();
  std::vector<std::uint8_t> sourcePixels;
  if (!gpuContext && !ReadRawRgba8TopLeft(source, renderer, sourcePixels, result.diagnostic)) {
    result.status = EngineCopyStatus::NativeExecutionFailed;
    return result;
  }

  QtTextDistortChromaRenderRequest request;
  request.source = {sourcePixels.data(), sourcePixels.size(), contract.width,
                    contract.height,
                    static_cast<std::size_t>(contract.width) * 4U};
  request.lensWidth = contract.lensWidth;
  request.lensHeight = contract.lensHeight;
  request.blurSteps = contract.blurSteps;
  request.blurStrideFirst = contract.blurStrideFirst;
  request.blurStrideSecond = contract.blurStrideSecond;
  request.blurAngleDegrees = contract.blurAngleDegrees;
  request.blurPerpendicularAngleDegrees =
      contract.blurPerpendicularAngleDegrees;
  request.rotateWarpDirectionDegrees =
      contract.rotateWarpDirectionDegrees;
  request.amountRelX = contract.amountRelX;
  request.amountRelY = contract.amountRelY;
  request.wrapModeX = contract.wrapModeX;
  request.wrapModeY = contract.wrapModeY;
  request.chromaSteps = contract.chromaSteps;
  request.warpRed = contract.warpRed;
  request.warpBlue = contract.warpBlue;
  request.warpAmount = contract.warpAmount;
  request.color1 = contract.color1;
  request.color2 = contract.color2;
  request.color3 = contract.color3;
  request.colorMix = contract.colorMix;
  request.captureIntermediates = captureIntermediates;

  auto &holder = DistortChromaMetalRuntime();
  QtTextDistortChromaRenderResult execution;
  {
    std::lock_guard<std::mutex> lock(holder.renderMutex);
    if (!holder.runtime) {
      result.status = EngineCopyStatus::NativeUnavailable;
      result.diagnostic = holder.unavailableReason;
      return result;
    }
    result.executor = holder.runtime->BackendName();
    if (gpuContext) {
      result.outputImage = gpuContext->RenderQtTextRawPass(
          source, contract.width, contract.height,
          [&](void *input, void *output, void *queue,
              const NativeCommandSubmission &submit, std::string &failure) {
            request.source.nativeTexture = input;
            const NativeRgba8TextureTarget target{output, queue, submit};
            return holder.runtime->Render(request, execution, failure, &target);
          }, result.diagnostic);
    } else if (!holder.runtime->Render(request, execution, result.diagnostic)) {
      result.status = EngineCopyStatus::NativeExecutionFailed;
      return result;
    }
  }

  if (!gpuContext) result.outputImage = RawRgba8TopLeftImage(
      std::move(execution.outputPixels), contract.width, contract.height);
  if (!result.outputImage) {
    result.status = EngineCopyStatus::NativeExecutionFailed;
    result.diagnostic =
        "Qt LumiDistortChroma could not publish its top-left raw output";
    return result;
  }
  if (captureIntermediates) {
    result.lensImage = RawRgba8TopLeftImage(
        std::move(execution.lensPixels), contract.lensWidth,
        contract.lensHeight);
    result.blurX1Image = RawRgba8TopLeftImage(
        std::move(execution.blurX1Pixels), contract.lensWidth,
        contract.lensHeight);
    result.blurY1Image = RawRgba8TopLeftImage(
        std::move(execution.blurY1Pixels), contract.lensWidth,
        contract.lensHeight);
    result.blurX2Image = RawRgba8TopLeftImage(
        std::move(execution.blurX2Pixels), contract.lensWidth,
        contract.lensHeight);
    result.blurY2Image = RawRgba8TopLeftImage(
        std::move(execution.blurY2Pixels), contract.lensWidth,
        contract.lensHeight);
    if (!result.lensImage || !result.blurX1Image || !result.blurY1Image ||
        !result.blurX2Image || !result.blurY2Image) {
      result.status = EngineCopyStatus::NativeExecutionFailed;
      result.diagnostic =
          "Qt LumiDistortChroma intermediate probe publication failed";
      return result;
    }
  }
  result.status = EngineCopyStatus::Executed;
  result.executorVersion = kQtTextDistortChromaExecutorVersion;
  result.diagnostic.clear();
  return result;
}

RadialBlurImageResult
ExecuteNativeRadialBlur(const sk_sp<SkImage> &source,
                        const QtRadialBlurContract &contract,
                        const StageRenderer &renderer) {
  RadialBlurImageResult result;
  auto *gpuContext = renderer.GpuContext();
  std::vector<std::uint8_t> sourcePixels;
  if (!gpuContext &&
      !ReadRawRgba8TopLeft(source, renderer, sourcePixels, result.diagnostic)) {
    result.status = EngineCopyStatus::NativeExecutionFailed;
    return result;
  }
  QtTextRadialBlurRenderRequest request;
  request.implementationId = kQtTextRadialBlurImplementationId;
  request.implementationVersion = contract.implementationVersion;
  request.sourceContractDigest = kQtTextRadialBlurSourceContractDigest;
  request.source = {sourcePixels.data(), sourcePixels.size(), contract.width,
                    contract.height,
                    static_cast<std::size_t>(contract.width) * 4U};
  request.intensity = contract.intensity;
  request.blurType = contract.blurType;
  request.center = contract.center;
  request.quality = contract.quality;
  request.weightDecay = contract.weightDecay;
  request.dither = contract.dither;
  request.borderType = contract.borderType;
  request.blurAlpha = contract.blurAlpha;
  request.inverseGammaCorrection = contract.inverseGammaCorrection;
  request.gamma = contract.gamma;
  request.lightIntensity = contract.lightIntensity;
  request.lightTransferMode = contract.lightTransferMode;

  auto &holder = RadialBlurMetalRuntime();
  std::vector<std::uint8_t> outputPixels;
  {
    std::lock_guard<std::mutex> lock(holder.renderMutex);
    if (!holder.runtime) {
      result.status = EngineCopyStatus::NativeUnavailable;
      result.diagnostic = holder.unavailableReason;
      return result;
    }
    result.executor = gpuContext ? "qt-lumi-radial-blur-metal-texture-v1"
                                 : holder.runtime->BackendName();
    if (gpuContext) {
      result.outputImage = gpuContext->RenderQtTextRawPass(
          source, contract.width, contract.height,
          [&](void *input, void *output, void *queue, const NativeCommandSubmission &submit, std::string &failure) {
            request.source.nativeTexture = input;
            const QtTextRadialBlurTextureTarget target{output, queue, submit};
            return holder.runtime->Render(request, outputPixels, failure, &target);
          }, result.diagnostic);
    } else if (holder.runtime->Render(request, outputPixels, result.diagnostic)) {
      result.outputImage = RawRgba8TopLeftImage(
          std::move(outputPixels), contract.width, contract.height);
    }
  }
  if (!result.outputImage) {
    result.status = EngineCopyStatus::NativeExecutionFailed;
    if (result.diagnostic.empty())
      result.diagnostic = "Qt LumiRadialBlur could not publish its raw output image";
    return result;
  }
  result.status = EngineCopyStatus::Executed;
  result.executorVersion = kQtTextRadialBlurImplementationVersion;
  result.diagnostic.clear();
  return result;
}

ShakeImageResult ExecuteNativeShake(const sk_sp<SkImage> &source,
                                    const QtShakeContract &contract,
                                    const StageRenderer &renderer) {
  ShakeImageResult result;
  auto *gpuContext = renderer.GpuContext();
  std::vector<std::uint8_t> sourcePixels;
  if (!gpuContext && !ReadRawRgba8TopLeft(source, renderer, sourcePixels, result.diagnostic)) {
    result.status = EngineCopyStatus::NativeExecutionFailed;
    return result;
  }
  QtTextShakeRenderRequest request;
  request.implementationId = kQtTextShakeImplementationId;
  request.implementationVersion = contract.implementationVersion;
  request.sourceContractDigest = kQtTextShakeSourceContractDigest;
  request.source = {sourcePixels.data(), sourcePixels.size(), contract.width,
                    contract.height,
                    static_cast<std::size_t>(contract.width) * 4U};
  request.uvMatrices = contract.uvMatrices;
  request.fillModeX = contract.fillModeX;
  request.fillModeY = contract.fillModeY;

  auto &holder = ShakeMetalRuntime();
  std::vector<std::uint8_t> outputPixels;
  {
    std::lock_guard<std::mutex> lock(holder.renderMutex);
    if (!holder.runtime) {
      result.status = EngineCopyStatus::NativeUnavailable;
      result.diagnostic = holder.unavailableReason;
      return result;
    }
    result.executor = gpuContext ? "qt-lumi-s-shake-metal-texture-v1"
                                 : holder.runtime->BackendName();
    if (gpuContext) {
      result.outputImage = gpuContext->RenderQtTextRawPass(
          source, contract.width, contract.height, [&](void *input, void *output, void *queue, const NativeCommandSubmission &submit, std::string &error) {
            request.source.nativeTexture = input;
            const QtTextShakeTextureTarget target{output, queue, submit};
            return holder.runtime->Render(request, outputPixels, error, &target);
          }, result.diagnostic);
      result.status = result.outputImage ? EngineCopyStatus::Executed
                                        : EngineCopyStatus::NativeExecutionFailed;
      result.executorVersion = kQtTextShakeImplementationVersion;
      return result;
    }
    if (!holder.runtime->Render(request, outputPixels, result.diagnostic)) {
      result.status = EngineCopyStatus::NativeExecutionFailed;
      return result;
    }
  }
  result.outputImage = RawRgba8TopLeftImage(std::move(outputPixels),
                                            contract.width, contract.height);
  if (!result.outputImage) {
    result.status = EngineCopyStatus::NativeExecutionFailed;
    result.diagnostic =
        "Qt LumiSShake could not publish its top-left raw output image";
    return result;
  }
  result.status = EngineCopyStatus::Executed;
  result.executorVersion = kQtTextShakeImplementationVersion;
  result.diagnostic.clear();
  return result;
}

TrailImageResult
ExecuteNativeTrail(const sk_sp<SkImage> &source,
                   const QtTrailContract &contract,
                   const StageRenderer &renderer,
                   const QtTextPostEffectStateContext *stateContext) {
  TrailImageResult result;
  auto *gpuContext = renderer.GpuContext();
  std::vector<std::uint8_t> sourcePixels;
  if (!gpuContext &&
      !ReadRawRgba8TopLeft(source, renderer, sourcePixels, result.diagnostic)) {
    result.status = EngineCopyStatus::NativeExecutionFailed;
    return result;
  }
  if (!stateContext || stateContext->contractVersion != 1U ||
      stateContext->renderGraphInstanceId == 0U ||
      stateContext->lifecycleEpoch == 0U ||
      stateContext->effectNodeInstanceId == 0U) {
    result.status = EngineCopyStatus::NativeExecutionFailed;
    result.diagnostic =
        "Qt Trail stable render-graph state identity is unavailable";
    return result;
  }
  const auto effectNodeInstanceId = TrailNodeIdentity(*stateContext);
  const TrailRuntimeKey instanceKey{stateContext->renderGraphInstanceId,
                                    effectNodeInstanceId};
  QtTextTrailRenderRequest request;
  request.identity.renderGraphInstanceId = stateContext->renderGraphInstanceId;
  request.identity.effectNodeInstanceId = effectNodeInstanceId;
  request.identity.implementationId = kQtTextTrailImplementationId;
  request.identity.implementationVersion = contract.implementationVersion;
  request.identity.stateSchemaVersion = contract.stateSchemaVersion;
  request.identity.sourceContractDigest = kQtTextTrailSourceContractDigest;
  request.source = {sourcePixels.data(), sourcePixels.size(), contract.width,
                    contract.height,
                    static_cast<std::size_t>(contract.width) * 4U};
  request.parameters.blur = contract.blur;
  request.parameters.weaken = contract.weaken;
  request.parameters.hintProfile = contract.hintProfile;
  request.parameters.hintHue = contract.hintHue;
  request.parameters.hintOffset = contract.hintOffset;
  request.parameters.hintMin = contract.hintMin;
  request.parameters.hintMax = contract.hintMax;
  request.parameters.baseEnabled = contract.baseEnabled;
  request.parameters.baseHint = contract.baseHint;

  auto &registry = TrailRuntimes();
  {
    std::lock_guard<std::mutex> lock(registry.mutex);
    auto [entryIterator, inserted] = registry.entries.try_emplace(instanceKey);
    auto &entry = entryIterator->second;
    if (inserted || (!entry.runtime && entry.initializationError.empty())) {
      entry.runtime = CreateQtTextTrailRuntime(entry.initializationError);
      if (!entry.runtime && entry.initializationError.empty()) {
        entry.initializationError =
            "Qt LumiTrail native Metal v1 returned no runtime";
      }
    }
    request.identity.lifecycleEpoch = stateContext->lifecycleEpoch;
    if (!entry.runtime) {
      result.status = EngineCopyStatus::NativeUnavailable;
      result.diagnostic = entry.initializationError;
      return result;
    }
    result.executor = gpuContext
        ? "qt-lumi-trail-metal-texture-rgba32f-history-v1"
        : entry.runtime->BackendName();
    result.executorVersion = kQtTextTrailImplementationVersion;
    if (gpuContext) {
      result.outputImage = gpuContext->RenderQtTextRawPass(
          source, contract.width, contract.height,
          [&](void *input, void *output, void *queue, const NativeCommandSubmission &submit, std::string &failure) {
            request.source.nativeTexture = input;
            request.target = {output, queue, submit};
            return entry.runtime->Render(request, result.execution, failure);
          }, result.diagnostic);
    } else if (entry.runtime->Render(request, result.execution, result.diagnostic)) {
      result.outputImage = RawRgba8TopLeftImage(
          std::move(result.execution.outputPixels), contract.width, contract.height);
    }
  }
  if (!result.outputImage) {
    result.status = EngineCopyStatus::NativeExecutionFailed;
    if (result.diagnostic.empty())
      result.diagnostic = "Qt Trail could not publish its raw output image";
    return result;
  }
  result.status = EngineCopyStatus::Executed;
  result.diagnostic.clear();
  return result;
}

EngineCopyImageResult ExecuteNativeEngineCopy(const sk_sp<SkImage> &source,
                                              const int outputWidth,
                                              const int outputHeight,
                                              const StageRenderer &renderer) {
  EngineCopyImageResult result;
  auto &holder = EngineCopyRuntime();
  if (!holder.runtime) {
    result.status = EngineCopyStatus::NativeUnavailable;
    result.diagnostic = holder.unavailableReason;
    return result;
  }

  if (auto *gpuContext = renderer.GpuContext()) {
    std::lock_guard<std::mutex> lock(holder.renderMutex);
    result.image = gpuContext->RenderQtTextEngineCopy(
        *holder.runtime, source, outputWidth, outputHeight, result.diagnostic);
    result.status = result.image ? EngineCopyStatus::Executed
                                 : EngineCopyStatus::NativeExecutionFailed;
    result.executor = holder.runtime->BackendName();
    result.executorVersion = kQtTextEngineCopyImplementationVersion;
    return result;
  }

  std::vector<std::uint8_t> sourcePixels;
  if (!ReadRawRgba8TopLeft(source, renderer, sourcePixels, result.diagnostic)) {
    result.status = EngineCopyStatus::NativeExecutionFailed;
    return result;
  }
  QtTextEngineCopyRequest request;
  request.implementationVersion = kQtTextEngineCopyImplementationVersion;
  request.source = {sourcePixels.data(), sourcePixels.size(), source->width(),
                    source->height(),
                    static_cast<std::size_t>(source->width()) * 4U};
  request.outputWidth = outputWidth;
  request.outputHeight = outputHeight;
  std::vector<std::uint8_t> outputPixels;
  {
    std::lock_guard<std::mutex> lock(holder.renderMutex);
    if (!holder.runtime->Render(request, outputPixels, result.diagnostic)) {
      result.status = EngineCopyStatus::NativeExecutionFailed;
      return result;
    }
  }
  result.image =
      RawRgba8TopLeftImage(std::move(outputPixels), outputWidth, outputHeight);
  if (!result.image) {
    result.status = EngineCopyStatus::NativeExecutionFailed;
    result.diagnostic =
        "Qt EngineCopy could not publish its top-left raw output image";
    return result;
  }
  result.status = EngineCopyStatus::Executed;
  result.executor = holder.runtime->BackendName();
  result.executorVersion = kQtTextEngineCopyImplementationVersion;
  result.diagnostic.clear();
  return result;
}

EngineCopyImageResult ExecuteGaussianEngineCopy(
    const sk_sp<SkImage> &source, const SkImageInfo &outputInfo,
    const std::array<float, 2> &inputSize,
    const std::array<float, 2> &outputSize, const StageRenderer &renderer,
    const TextRuntimeProgram &fallbackProgram, const char *stage) {
  auto result = ExecuteNativeEngineCopy(source, outputInfo.width(),
                                        outputInfo.height(), renderer);
  if (result.status == EngineCopyStatus::Executed) {
    if (std::getenv("VIDEOCUT_TRACE_TEXT_QT_POSTFX") != nullptr) {
      std::fprintf(stderr,
                   "[VIDEOCUT_TEXT_QT_ENGINE_COPY] stage=%s version=%u "
                   "backend=%s source=%dx%d target=%dx%d "
                   "transport=%s status=executed\n",
                   stage, result.executorVersion, result.executor.c_str(),
                   source->width(), source->height(), outputInfo.width(),
                   outputInfo.height(), renderer.GpuContext()
                       ? "gpu-resident-bottom-left" : "cpu-top-left");
    }
    return result;
  }
  if (result.status == EngineCopyStatus::NativeExecutionFailed) {
    std::fprintf(stderr,
                 "[VIDEOCUT_TEXT_QT_ENGINE_COPY_FAILURE] stage=%s version=%u "
                 "reason=%s fallback=forbidden\n",
                 stage, kQtTextEngineCopyImplementationVersion,
                 result.diagnostic.c_str());
    return result;
  }

  // The portable shader backend is allowed only when the native executor is
  // unavailable. It never masks a failed native command.
  if (std::getenv("VIDEOCUT_TRACE_TEXT_QT_POSTFX") != nullptr) {
    std::fprintf(stderr,
                 "[VIDEOCUT_TEXT_QT_ENGINE_COPY_FALLBACK] stage=%s "
                 "native_version=%u backend=skia-runtime-effect reason=%s\n",
                 stage, kQtTextEngineCopyImplementationVersion,
                 result.diagnostic.c_str());
  }
  if (!fallbackProgram.effect) {
    result.status = EngineCopyStatus::NativeExecutionFailed;
    result.diagnostic +=
        "; Skia fallback program is unavailable: " + fallbackProgram.error;
    return result;
  }
  SkRuntimeEffectBuilder builder(fallbackProgram.effect);
  builder.child("inputTexture") = RawLinearShader(source);
  builder.uniform("inputSize") = inputSize;
  builder.uniform("outputSize") = outputSize;
  result.image = renderer.Draw(outputInfo, builder.makeShader());
  if (!result.image) {
    result.status = EngineCopyStatus::NativeExecutionFailed;
    result.diagnostic += "; Skia fallback draw failed";
    return result;
  }
  result.status = EngineCopyStatus::Executed;
  result.executor = "skia-runtime-effect-fallback";
  result.executorVersion = 0U;
  return result;
}

EngineCopyImageResult
ExecuteGaussianAxis(const sk_sp<SkImage> &source, const QtTextGaussianAxis axis,
                    const float sampleCount, const float sigma,
                    const float step, const float gamma,
                    const SkImageInfo &outputInfo,
                    const StageRenderer &renderer,
                    const TextRuntimeProgram &fallbackProgram, const char *stage) {
  EngineCopyImageResult result;
  auto &holder = GaussianMetalRuntime();
  if (holder.runtime) {
    auto *gpuContext = renderer.GpuContext();
    std::vector<std::uint8_t> sourcePixels;
    if (!gpuContext &&
        !ReadRawRgba8TopLeft(source, renderer, sourcePixels,
                            result.diagnostic)) {
      result.status = EngineCopyStatus::NativeExecutionFailed;
      return result;
    }
    QtTextGaussianMetalRequest request;
    request.implementationVersion = kQtTextGaussianMetalImplementationVersion;
    request.source = {sourcePixels.data(), sourcePixels.size(), source->width(),
                      source->height(),
                      static_cast<std::size_t>(source->width()) * 4U};
    request.axis = axis;
    request.sampleCount = sampleCount;
    request.sigma = sigma;
    request.step = step;
    request.gamma = gamma;
    std::vector<std::uint8_t> outputPixels;
    {
      std::lock_guard<std::mutex> lock(holder.renderMutex);
      if (gpuContext) {
        result.image = gpuContext->RenderQtTextRawPass(
            source, outputInfo.width(), outputInfo.height(),
            [&](void *input, void *output, void *queue, const NativeCommandSubmission &submit, std::string &failure) {
              request.source.nativeTexture = input;
              const QtTextGaussianTextureTarget target{output, queue, submit};
              return holder.runtime->Render(request, outputPixels, failure, &target);
            }, result.diagnostic);
      } else if (holder.runtime->Render(request, outputPixels, result.diagnostic)) {
        result.image = RawRgba8TopLeftImage(
            std::move(outputPixels), outputInfo.width(), outputInfo.height());
      }
    }
    if (!result.image) {
      result.status = EngineCopyStatus::NativeExecutionFailed;
      if (result.diagnostic.empty())
        result.diagnostic = "Qt Gaussian could not publish its raw output image";
      std::fprintf(stderr,
                   "[VIDEOCUT_TEXT_QT_GAUSSIAN_FAILURE] stage=%s version=%u "
                   "reason=%s fallback=forbidden\n",
                   stage, kQtTextGaussianMetalImplementationVersion,
                   result.diagnostic.c_str());
      return result;
    }
    result.status = EngineCopyStatus::Executed;
    result.executor = gpuContext ? "qt-lumi-gaussian-metal-texture-v1"
                                 : holder.runtime->BackendName();
    result.executorVersion = kQtTextGaussianMetalImplementationVersion;
    if (std::getenv("VIDEOCUT_TRACE_TEXT_QT_POSTFX") != nullptr) {
      std::fprintf(stderr,
                   "[VIDEOCUT_TEXT_QT_GAUSSIAN] stage=%s version=%u "
                   "backend=%s axis=%s source=%dx%d sample=%.9g "
                   "sigma=%.9g step=%.9g gamma=%.9g "
                   "transport=%s status=executed\n",
                   stage, result.executorVersion, result.executor.c_str(),
                   axis == QtTextGaussianAxis::Horizontal ? "x" : "y",
                   source->width(), source->height(), sampleCount, sigma, step,
                   gamma, gpuContext ? "gpu-resident-bottom-left" : "cpu-top-left");
    }
    result.diagnostic.clear();
    return result;
  }

  if (std::getenv("VIDEOCUT_TRACE_TEXT_QT_POSTFX") != nullptr) {
    std::fprintf(stderr,
                 "[VIDEOCUT_TEXT_QT_GAUSSIAN_FALLBACK] stage=%s "
                 "native_version=%u backend=skia-runtime-effect reason=%s\n",
                 stage, kQtTextGaussianMetalImplementationVersion,
                 holder.unavailableReason.c_str());
  }
  if (!fallbackProgram.effect) {
    result.status = EngineCopyStatus::NativeExecutionFailed;
    result.diagnostic =
        holder.unavailableReason +
        "; Skia fallback program is unavailable: " + fallbackProgram.error;
    return result;
  }
  const std::array<float, 2> textureSize{
      static_cast<float>(outputInfo.width()),
      static_cast<float>(outputInfo.height())};
  const std::array<float, 2> sampleStep =
      axis == QtTextGaussianAxis::Horizontal ? std::array<float, 2>{step, 0.0F}
                                             : std::array<float, 2>{0.0F, step};
  SkRuntimeEffectBuilder builder(fallbackProgram.effect);
  builder.child("inputTexture") = RawLinearShader(source);
  builder.uniform("textureSize") = textureSize;
  builder.uniform("sampleCount") = sampleCount;
  builder.uniform("sigma") = sigma;
  builder.uniform("sampleStep") = sampleStep;
  builder.uniform("gamma") = gamma;
  result.image = renderer.Draw(outputInfo, builder.makeShader());
  if (!result.image) {
    result.status = EngineCopyStatus::NativeExecutionFailed;
    result.diagnostic =
        holder.unavailableReason + "; Skia Gaussian fallback draw failed";
    return result;
  }
  result.status = EngineCopyStatus::Executed;
  result.executor = "skia-runtime-effect-fallback";
  result.executorVersion = 0U;
  return result;
}

TurbulenceImageResult
ExecuteNativeTurbulence(const sk_sp<SkImage> &source,
                        const QtTurbulenceContract &contract,
                        const StageRenderer &renderer,
                        const bool captureNoise) {
  TurbulenceImageResult result;
  auto &holder = TurbulenceMetalRuntime();
  if (!holder.runtime) {
    result.status = EngineCopyStatus::NativeUnavailable;
    result.diagnostic = holder.unavailableReason;
    return result;
  }
  auto *gpuContext = renderer.GpuContext();
  std::vector<std::uint8_t> sourcePixels;
  if (!gpuContext &&
      !ReadRawRgba8TopLeft(source, renderer, sourcePixels, result.diagnostic)) {
    result.status = EngineCopyStatus::NativeExecutionFailed;
    return result;
  }
  QtTextTurbulenceMetalRequest request;
  request.implementationVersion = kQtTextTurbulenceMetalImplementationVersion;
  request.source = {sourcePixels.data(), sourcePixels.size(), source->width(),
                    source->height(),
                    static_cast<std::size_t>(source->width()) * 4U};
  request.noiseWidth = contract.noiseWidth;
  request.noiseHeight = contract.noiseHeight;
  request.cycle = contract.cycle;
  request.offsetX = contract.offsetX;
  request.offsetY = contract.offsetY;
  request.quantity = contract.quantity;
  request.complexity = contract.complexity;
  request.evolution = contract.evolution;
  request.type = contract.type;
  request.contrast = contract.contrast;
  request.range = contract.range;
  request.motionTileType = contract.motionTileType;
  std::vector<std::uint8_t> noisePixels;
  std::vector<std::uint8_t> outputPixels;
  {
    std::lock_guard<std::mutex> lock(holder.renderMutex);
    if (gpuContext) {
      result.outputImage = gpuContext->RenderQtTextRawPass(
          source, source->width(), source->height(),
          [&](void *input, void *output, void *queue, const NativeCommandSubmission &submit, std::string &failure) {
            request.source.nativeTexture = input;
            const QtTextTurbulenceTextureTarget target{output, queue, submit, captureNoise};
            return holder.runtime->Render(request, noisePixels, outputPixels,
                                          failure, &target);
          }, result.diagnostic);
    } else if (holder.runtime->Render(request, noisePixels, outputPixels,
                                      result.diagnostic)) {
      result.outputImage = RawRgba8TopLeftImage(std::move(outputPixels),
                                               source->width(), source->height());
    }
  }
  if (!gpuContext || captureNoise)
    result.noiseImage = RawRgba8TopLeftImage(
        std::move(noisePixels), contract.noiseWidth, contract.noiseHeight);
  if (!result.outputImage || ((!gpuContext || captureNoise) && !result.noiseImage)) {
    result.status = EngineCopyStatus::NativeExecutionFailed;
    if (result.diagnostic.empty())
      result.diagnostic = "Qt Turbulence could not publish its raw output images";
    std::fprintf(stderr,
                 "[VIDEOCUT_TEXT_QT_TURBULENCE_FAILURE] version=%u reason=%s "
                 "fallback=forbidden\n",
                 kQtTextTurbulenceMetalImplementationVersion,
                 result.diagnostic.c_str());
    return result;
  }
  result.status = EngineCopyStatus::Executed;
  result.executor = gpuContext ? "qt-lumi-turbulence-metal-texture-v1"
                               : holder.runtime->BackendName();
  result.executorVersion = kQtTextTurbulenceMetalImplementationVersion;
  if (std::getenv("VIDEOCUT_TRACE_TEXT_QT_POSTFX") != nullptr) {
    std::fprintf(stderr,
                 "[VIDEOCUT_TEXT_QT_TURBULENCE] version=%u backend=%s "
                 "source=%dx%d noise=%dx%d cycle=%.9g quantity=%.9g "
                 "complexity=%.9g evolution=%.9g contrast=%.9g "
                 "transport=%s status=executed\n",
                 result.executorVersion, result.executor.c_str(),
                 source->width(), source->height(), contract.noiseWidth,
                 contract.noiseHeight, contract.cycle, contract.quantity,
                 contract.complexity, contract.evolution, contract.contrast,
                 gpuContext ? "gpu-resident-bottom-left" : "cpu-top-left");
  }
  result.diagnostic.clear();
  return result;
}

sk_sp<SkImage> MaterializeSource(const sk_sp<SkPicture> &source,
                                 const SkIRect &rasterBounds,
                                 const SkImageInfo &info,
                                 const StageRenderer &renderer) {
  auto surface = renderer.MakeSurface(info);
  if (!surface)
    return {};
  auto *canvas = surface->getCanvas();
  canvas->clear(SK_ColorTRANSPARENT);
  canvas->translate(-static_cast<float>(rasterBounds.left()),
                    -static_cast<float>(rasterBounds.top()));
  canvas->drawPicture(source.get());
  return surface->makeImageSnapshot();
}

sk_sp<SkPicture> PictureFromImage(const sk_sp<SkImage> &image,
                                  const SkIRect &rasterBounds,
                                  const SkRect &recordingBounds) {
  if (!image)
    return {};
  SkPictureRecorder recorder;
  auto *canvas = recorder.beginRecording(recordingBounds);
  canvas->drawImageRect(
      image.get(),
      SkRect::MakeXYWH(static_cast<float>(rasterBounds.left()),
                       static_cast<float>(rasterBounds.top()),
                       static_cast<float>(image->width()),
                       static_cast<float>(image->height())),
      SkSamplingOptions(SkFilterMode::kNearest), nullptr);
  return recorder.finishRecordingAsPicture();
}

bool ProgramsAvailable(std::initializer_list<const TextRuntimeProgram *> programs,
                       const char *label) {
  for (const auto *program : programs) {
    if (program && program->effect)
      continue;
    if (std::getenv("VIDEOCUT_TRACE_TEXT_QT_POSTFX") != nullptr) {
      std::fprintf(stderr,
                   "[VIDEOCUT_TEXT_QT_POSTFX_UNAVAILABLE] stage=%s "
                   "error=%s\n",
                   label, program ? program->error.c_str() : "missing");
    }
    return false;
  }
  return true;
}

} // namespace

void ReleaseExactQtTextPostEffectState(
    const std::uint64_t renderGraphInstanceId) noexcept {
  if (renderGraphInstanceId == 0U)
    return;
  {
    auto &registry = TrailRuntimes();
    std::lock_guard<std::mutex> lock(registry.mutex);
    for (auto iterator = registry.entries.begin();
         iterator != registry.entries.end();) {
      if (iterator->first.renderGraphInstanceId == renderGraphInstanceId) {
        iterator = registry.entries.erase(iterator);
      } else {
        ++iterator;
      }
    }
  }
  {
    auto &registry = PendingDirectionalBlurs();
    std::lock_guard<std::mutex> lock(registry.mutex);
    for (auto iterator = registry.entries.begin();
         iterator != registry.entries.end();) {
      if (iterator->first.renderGraphInstanceId == renderGraphInstanceId) {
        iterator = registry.entries.erase(iterator);
      } else {
        ++iterator;
      }
    }
  }
}

sk_sp<SkPicture> ApplyExactQtTextDirectionalBlursChainPicture(
    const sk_sp<SkPicture> &source,
    const text::TextEffectPostEffectNode &directionalEffect,
    const text::TextEffectPostEffectNode &alphaOutlineEffect,
    const double progress,
    const SkRect &recordingBounds, SkiaGpuContext *gpuContext,
    std::string *diagnostic) {
  return ApplyExactQtTextDirectionalBlursChainPicture(
      source, directionalEffect, alphaOutlineEffect, progress, progress,
      recordingBounds, gpuContext, diagnostic);
}

sk_sp<SkPicture> ApplyExactQtTextDirectionalBlursChainPicture(
    const sk_sp<SkPicture> &source,
    const text::TextEffectPostEffectNode &directionalEffect,
    const text::TextEffectPostEffectNode &alphaOutlineEffect,
    const double directionalProgress, const double alphaOutlineProgress,
    const SkRect &recordingBounds, SkiaGpuContext *gpuContext,
    std::string *diagnostic) {
  const auto fail = [&](std::string message) -> sk_sp<SkPicture> {
    if (diagnostic)
      *diagnostic = std::move(message);
    return {};
  };
  if (diagnostic)
    diagnostic->clear();
  if (!source || recordingBounds.isEmpty())
    return fail("Qt DirectionalBlurs source Page is empty");
  SkIRect rasterBounds;
  recordingBounds.roundOut(&rasterBounds);
  const int width = rasterBounds.width();
  const int height = rasterBounds.height();
  QtTextDirectionalBlursRenderRequest request;
  request.contract.identity.implementationId =
      kQtTextDirectionalBlursImplementationId;
  request.contract.identity.implementationVersion =
      kQtTextDirectionalBlursImplementationVersion;
  request.contract.identity.contractSchemaVersion =
      kQtTextDirectionalBlursContractSchemaVersion;
  request.contract.identity.sourceContractDigest =
      kQtTextDirectionalBlursSourceContractDigest;
  request.contract.pageWidth = width;
  request.contract.pageHeight = height;
  request.contract.directional.blurIntensity =
      SampleParameterValue(
          directionalEffect, "blurIntensity", directionalProgress,
          SampleParameterValue(directionalEffect, "radius", directionalProgress,
                               directionalEffect.amount));
  request.contract.directional.angleDegrees =
      SampleParameterValue(directionalEffect, "angle", directionalProgress, 0.0);
  request.contract.directional.directionNum =
      static_cast<std::int32_t>(std::llround(SampleParameterValue(
          directionalEffect, "directionNum", directionalProgress, 4.0)));
  request.contract.directional.exposure =
      SampleParameterValue(directionalEffect, "exposure", directionalProgress, 1.0);
  request.contract.directional.quality =
      SampleParameterValue(directionalEffect, "quality", directionalProgress, 0.5);
  request.contract.directional.spaceDither =
      SampleParameterValue(directionalEffect, "spaceDither", directionalProgress, 0.0);
  request.contract.directional.borderType =
      static_cast<QtTextDirectionalBlursBorderType>(
          std::llround(SampleParameterValue(directionalEffect, "borderType",
                                            directionalProgress, 0.0)));
  request.contract.directional.blendMode =
      static_cast<QtTextDirectionalBlursBlendMode>(std::llround(
          SampleParameterValue(directionalEffect, "blendMode", directionalProgress, 2.0)));
  request.contract.alphaOutline.offsetX =
      SampleParameterValue(alphaOutlineEffect, "offsetX", alphaOutlineProgress, 0.0);
  request.contract.alphaOutline.offsetY =
      SampleParameterValue(alphaOutlineEffect, "offsetY", alphaOutlineProgress, 0.0);
  request.contract.alphaOutline.size =
      SampleParameterValue(alphaOutlineEffect, "size", alphaOutlineProgress, 1.0);
  request.contract.alphaOutline.scaleX =
      SampleParameterValue(alphaOutlineEffect, "scaleX", alphaOutlineProgress, 1.0);
  request.contract.alphaOutline.scaleY =
      SampleParameterValue(alphaOutlineEffect, "scaleY", alphaOutlineProgress, 1.0);
  const auto outlineColor = VectorParameter(alphaOutlineEffect, "outlineColor",
                                            {0.2F, 0.2F, 0.2F, 1.0F});
  for (std::size_t channel = 0U; channel < outlineColor.size(); ++channel) {
    request.contract.alphaOutline.outlineColor[channel] =
        static_cast<double>(outlineColor[channel]);
  }
  request.contract.alphaOutline.intensity =
      SampleParameterValue(alphaOutlineEffect, "intensity", alphaOutlineProgress, 1.0);

  const auto fullInfo = StageImageInfo(
      width, height, QtTextPostEffectSurfaceSemantics::RawRgbaData);
  std::string surfaceError;
  const StageRenderer renderer(gpuContext, surfaceError);
  auto sourceImage =
      MaterializeSource(source, rasterBounds, fullInfo, renderer);
  if (!sourceImage) {
    return fail(surfaceError.empty()
                    ? "Qt DirectionalBlurs could not materialize Page"
                    : std::move(surfaceError));
  }
  std::vector<std::uint8_t> sourcePixels;
  std::string executionError;
  if (!gpuContext && !ReadRawRgba8TopLeft(sourceImage, renderer, sourcePixels,
                           executionError)) {
    return fail(std::move(executionError));
  }
  request.source = {sourcePixels.data(), sourcePixels.size(), width, height,
                    static_cast<std::size_t>(width) * 4U};

  auto &holder = DirectionalBlursMetalRuntime();
  QtTextDirectionalBlursRenderResult rendered;
  std::string backend;
  sk_sp<SkImage> outputImage;
  {
    std::lock_guard<std::mutex> lock(holder.renderMutex);
    if (!holder.runtime)
      return fail(holder.unavailableReason);
    backend = holder.runtime->BackendName();
    if (gpuContext) {
      outputImage = gpuContext->RenderQtTextRawPass(
          sourceImage, width, height,
          [&](void *input, void *output, void *queue,
              const NativeCommandSubmission &submit, std::string &failure) {
            request.source.nativeTexture = input;
            const NativeRgba8TextureTarget target{output, queue, submit};
            return holder.runtime->Render(request, rendered, failure, &target);
          }, executionError);
      if (!outputImage) return fail(std::move(executionError));
    } else if (!holder.runtime->Render(request, rendered, executionError))
      return fail(std::move(executionError));
  }
  if (!gpuContext) outputImage = RawRgba8TopLeftImage(std::move(rendered.outputPixels),
                                          rendered.width, rendered.height);
  if (!outputImage)
    return fail("Qt DirectionalBlurs could not publish its output Page");
  if (std::getenv("VIDEOCUT_TRACE_TEXT_QT_POSTFX") != nullptr) {
    std::fprintf(
        stderr,
        "[VIDEOCUT_TEXT_QT_DIRECTIONAL_BLURS] page=%dx%d downsample=%dx%d "
        "passes=%zu blur=%.17g angle=%.17g alpha_outline=%.17g "
        "executor=%s\n",
        width, height, rendered.contract.downsampleWidth,
        rendered.contract.downsampleHeight, rendered.contract.passes.size(),
        request.contract.directional.blurIntensity,
        request.contract.directional.angleDegrees,
        request.contract.alphaOutline.intensity, backend.c_str());
  }
  if (diagnostic)
    diagnostic->clear();
  return PictureFromImage(outputImage, rasterBounds, recordingBounds);
}

sk_sp<SkPicture> ApplyExactQtTextDirectionalBlurPicture(
    const sk_sp<SkPicture> &source,
    const text::TextEffectPostEffectNode &directionalEffect, const double progress,
    const SkRect &recordingBounds, SkiaGpuContext *gpuContext,
    std::string *diagnostic) {
  const auto fail = [&](std::string message) -> sk_sp<SkPicture> {
    if (diagnostic)
      *diagnostic = std::move(message);
    return {};
  };
  if (diagnostic)
    diagnostic->clear();
  if (!source || recordingBounds.isEmpty())
    return fail("Qt standalone DirectionalBlur source Page is empty");
  SkIRect rasterBounds;
  recordingBounds.roundOut(&rasterBounds);
  const int width = rasterBounds.width();
  const int height = rasterBounds.height();
  if (width <= 0 || height <= 0)
    return fail("Qt standalone DirectionalBlur Page dimensions are invalid");

  QtTextDirectionalBlurStandaloneRenderRequest request;
  request.contract.identity.implementationId =
      kQtTextDirectionalBlurStandaloneImplementationId;
  request.contract.identity.implementationVersion =
      kQtTextDirectionalBlurStandaloneImplementationVersion;
  request.contract.identity.contractSchemaVersion =
      kQtTextDirectionalBlurStandaloneContractSchemaVersion;
  request.contract.identity.sourceContractDigest =
      kQtTextDirectionalBlurStandaloneSourceTreeDigestA;
  request.contract.pageWidth = width;
  request.contract.pageHeight = height;
  request.contract.directional.blurIntensity =
      SampleParameterValue(
          directionalEffect, "blurIntensity", progress,
          SampleParameterValue(directionalEffect, "radius", progress,
                               directionalEffect.amount));
  request.contract.directional.angleDegrees =
      SampleParameterValue(directionalEffect, "angle", progress, 0.0);
  request.contract.directional.directionNum =
      static_cast<std::int32_t>(std::floor(std::clamp(
          SampleParameterValue(directionalEffect, "directionNum", progress,
                               1.0),
          1.0, 4.0)));
  request.contract.directional.exposure =
      SampleParameterValue(directionalEffect, "exposure", progress, 1.0);
  request.contract.directional.quality =
      SampleParameterValue(directionalEffect, "quality", progress, 0.5);
  request.contract.directional.spaceDither =
      SampleParameterValue(directionalEffect, "spaceDither", progress, 0.0);
  request.contract.directional.borderType =
      static_cast<QtTextDirectionalBlursBorderType>(
          std::llround(SampleParameterValue(directionalEffect, "borderType",
                                            progress, 0.0)));
  request.contract.directional.blendMode =
      static_cast<QtTextDirectionalBlursBlendMode>(
          std::llround(SampleParameterValue(directionalEffect, "blendMode",
                                            progress, 2.0)));

  const auto fullInfo = StageImageInfo(
      width, height, QtTextPostEffectSurfaceSemantics::RawRgbaData);
  std::string surfaceError;
  const StageRenderer renderer(gpuContext, surfaceError);
  auto sourceImage =
      MaterializeSource(source, rasterBounds, fullInfo, renderer);
  if (!sourceImage) {
    return fail(surfaceError.empty()
                    ? "Qt standalone DirectionalBlur could not materialize Page"
                    : std::move(surfaceError));
  }
  std::vector<std::uint8_t> sourcePixels;
  std::string executionError;
  if (!gpuContext && !ReadRawRgba8TopLeft(sourceImage, renderer, sourcePixels,
                           executionError)) {
    return fail(std::move(executionError));
  }
  request.source = {sourcePixels.data(), sourcePixels.size(), width, height,
                    static_cast<std::size_t>(width) * 4U};

  auto &holder = DirectionalBlursMetalRuntime();
  QtTextDirectionalBlurStandaloneRenderResult rendered;
  std::string backend;
  sk_sp<SkImage> outputImage;
  {
    std::lock_guard<std::mutex> lock(holder.renderMutex);
    if (!holder.runtime)
      return fail(holder.unavailableReason);
    backend = holder.runtime->BackendName();
    if (gpuContext) {
      outputImage = gpuContext->RenderQtTextRawPass(
          sourceImage, width, height,
          [&](void *input, void *output, void *queue,
              const NativeCommandSubmission &submit, std::string &failure) {
            request.source.nativeTexture = input;
            const NativeRgba8TextureTarget target{output, queue, submit};
            return holder.runtime->RenderStandalone(request, rendered, failure, &target);
          }, executionError);
      if (!outputImage) return fail(std::move(executionError));
    } else if (!holder.runtime->RenderStandalone(request, rendered, executionError))
      return fail(std::move(executionError));
  }
  if (!gpuContext) outputImage = RawRgba8TopLeftImage(std::move(rendered.outputPixels),
                                          rendered.width, rendered.height);
  if (!outputImage)
    return fail("Qt standalone DirectionalBlur could not publish OutputTex");
  if (std::getenv("VIDEOCUT_TRACE_TEXT_QT_POSTFX") != nullptr) {
    std::fprintf(
        stderr,
        "[VIDEOCUT_TEXT_QT_DIRECTIONAL_BLUR] page=%dx%d downsample=%dx%d "
        "passes=%zu blur=%.17g angle=%.17g executor=%s\n",
        width, height, rendered.contract.downsampleWidth,
        rendered.contract.downsampleHeight, rendered.contract.passes.size(),
        request.contract.directional.blurIntensity,
        request.contract.directional.angleDegrees, backend.c_str());
  }
  if (diagnostic)
    diagnostic->clear();
  return PictureFromImage(outputImage, rasterBounds, recordingBounds);
}

sk_sp<SkPicture> ApplyExactQtTextPostEffectPicture(
    const sk_sp<SkPicture> &source, const text::TextEffectPostEffectNode &effect,
    const double progress, const SkRect &recordingBounds,
    SkiaGpuContext *gpuContext,
    QtTextPostEffectExecutionTrace *executionTrace,
    const QtTextPostEffectStateContext *stateContext) {
  if (executionTrace)
    *executionTrace = {};
  if (!source || recordingBounds.isEmpty())
    return {};
  SkIRect rasterBounds;
  recordingBounds.roundOut(&rasterBounds);
  const int width = rasterBounds.width();
  const int height = rasterBounds.height();
  if (width <= 0 || height <= 0 ||
      static_cast<std::size_t>(width) * static_cast<std::size_t>(height) >
          16U * 1024U * 1024U) {
    return {};
  }
  if (executionTrace) {
    executionTrace->pageWidth = width;
    executionTrace->pageHeight = height;
  }
  const auto fullInfo = StageImageInfo(
      width, height, QtTextPostEffectSurfaceSemantics::ColorPremultiplied);
  std::string surfaceError;
  const StageRenderer renderer(gpuContext, surfaceError);
  auto sourceImage =
      MaterializeSource(source, rasterBounds, fullInfo, renderer);
  if (!sourceImage)
    return {};
  const auto kind = effect.kind;
  if (kind == text::TextPostEffectKind::GaussianBlur ||
      kind == text::TextPostEffectKind::SoftGlow) {
    // Qt passes the Dust D3 texture object directly to the first downstream
    // draw. Keep this diagnostic boundary explicit so the exact-D3 injection
    // probe can prove whether picture materialization changed any raw byte
    // before attributing a residual to GaussianBlur or SoftGlow.
    DumpStage(sourceImage, progress, "source-materialized", gpuContext,
              QtTextPostEffectSurfaceSemantics::RawRgbaData);
  }
  const std::array<float, 2> fullSize{static_cast<float>(width),
                                      static_cast<float>(height)};

  if (kind == text::TextPostEffectKind::OpticsCompensation &&
      FindParameter(effect, "fov")) {
    const auto &distortProgram =
        GetTextRuntimeProgram(TextRuntimeShader::PostOpticsCompensationDistort);
    const auto &antiAliasingProgram = GetTextRuntimeProgram(
        TextRuntimeShader::PostOpticsCompensationAntiAliasing);
    const bool antiAliasing =
        SampleParameter(effect, "antiAliasing", progress, 1.0F) >= 0.5F;
    if (!ProgramsAvailable(
            antiAliasing
                ? std::initializer_list<const TextRuntimeProgram *>{
                      &distortProgram, &antiAliasingProgram}
                : std::initializer_list<const TextRuntimeProgram *>{
                      &distortProgram},
            "optics-compensation")) {
      return {};
    }
    const auto center =
        VectorParameter(effect, "center", {0.5F, 0.5F, 0.0F, 0.0F});
    const float orientation =
        SampleParameter(effect, "fovOrientation", progress, 0.0F);
    float orientationSize = static_cast<float>(width);
    if (orientation >= 1.5F) {
      orientationSize = std::hypot(static_cast<float>(width),
                                   static_cast<float>(height));
    } else if (orientation >= 0.5F) {
      orientationSize = static_cast<float>(height);
    }
    const std::array<float, 2> fovScale{
        static_cast<float>(width) / orientationSize,
        static_cast<float>(height) / orientationSize};
    const float fovIntensity = std::clamp(
        SampleParameter(effect, "fov", progress, 0.0F) / 180.0F, 0.0F,
        1.0F);
    const bool inverse = SampleParameter(
                             effect, "inverseLensDistortion", progress, 0.0F) >=
                         0.5F;
    float firstDistortion = 0.0F;
    float secondDistortion = 0.0F;
    if (!inverse) {
      if (fovIntensity <= 0.5F) {
        secondDistortion =
            22.759F * std::pow(fovIntensity, 3.167F) +
            0.431F * fovIntensity;
      } else if (fovIntensity <= 0.9F) {
        const float delta = fovIntensity - 0.5F;
        secondDistortion = std::pow(
            1.6583124F + 9.085F * delta +
                1914.57F * std::pow(delta, 6.15F),
            2.0F);
      } else {
        firstDistortion =
            std::pow(67.0F * (fovIntensity - 0.9F), 4.0F);
        secondDistortion = std::pow(
            1.6583124F + 9.085F * 0.4F +
                1914.57F * std::pow(0.4F, 6.15F),
            2.0F);
        if (std::abs(fovIntensity - 1.0F) < 0.00001F)
          firstDistortion = 5000.0F;
      }
    } else {
      constexpr float x2 = 150.0F / 180.0F;
      constexpr float x3 = 178.0F / 180.0F;
      constexpr float x4 = 179.9F / 180.0F;
      firstDistortion = 14.77F * std::pow(fovIntensity, 10.0F) +
                        2.7F * std::pow(fovIntensity, 3.0F);
      secondDistortion = 3.82F * std::pow(fovIntensity, 2.54F) +
                         0.529F * fovIntensity;
      if (fovIntensity > x2) {
        if (fovIntensity < x3) {
          firstDistortion =
              218.48F * std::pow(fovIntensity, 73.79F) - 54.266F +
              69.83F * fovIntensity;
        } else if (fovIntensity < x4) {
          firstDistortion =
              std::pow(658.399F * (fovIntensity - x3), 4.0F) + 110.58F;
        } else {
          firstDistortion =
              std::pow(33162.0F * (fovIntensity - x4), 4.0F) + 2443.4F;
        }
      }
    }
    SkRuntimeEffectBuilder distortBuilder(distortProgram.effect);
    distortBuilder.child("inputTexture") = RawLinearShader(sourceImage);
    distortBuilder.uniform("textureSize") = fullSize;
    distortBuilder.uniform("center") =
        std::array<float, 2>{center[0], center[1]};
    distortBuilder.uniform("fovScale") = fovScale;
    distortBuilder.uniform("distortParameters") =
        std::array<float, 2>{firstDistortion, secondDistortion};
    distortBuilder.uniform("inverseLensDistortion") = inverse ? 1.0F : 0.0F;
    distortBuilder.uniform("fillBorders") =
        SampleParameter(effect, "fillBorders", progress, 0.0F);
    auto distortedImage = renderer.Draw(fullInfo, distortBuilder.makeShader());
    if (!distortedImage)
      return {};
    const std::size_t passCount = antiAliasing ? 2U : 1U;
    RecordExecutedPass(
        executionTrace, "optics_compensation",
        "optics_compensation_distort", 1U, passCount, fullInfo,
        QtTextPostEffectSurfaceSemantics::ColorPremultiplied);
    if (!antiAliasing)
      return PictureFromImage(distortedImage, rasterBounds, recordingBounds);
    SkRuntimeEffectBuilder antiAliasingBuilder(antiAliasingProgram.effect);
    antiAliasingBuilder.child("inputTexture") =
        RawLinearShader(distortedImage);
    antiAliasingBuilder.uniform("textureSize") = fullSize;
    auto outputImage =
        renderer.Draw(fullInfo, antiAliasingBuilder.makeShader());
    if (!outputImage)
      return {};
    RecordExecutedPass(
        executionTrace, "optics_compensation",
        "optics_compensation_anti_aliasing", 2U, 2U, fullInfo,
        QtTextPostEffectSurfaceSemantics::ColorPremultiplied);
    return PictureFromImage(outputImage, rasterBounds, recordingBounds);
  }

  if (kind == text::TextPostEffectKind::Projection &&
      FindParameter(effect, "backAlpha")) {
    const auto &program =
        GetTextRuntimeProgram(TextRuntimeShader::PostProjectionCopyScale);
    if (!ProgramsAvailable({&program}, "projection"))
      return {};
    const float scale =
        SampleParameter(effect, "scale", progress, 1.0F);
    const double shakeTime = static_cast<double>(
        SampleParameter(effect, "shakeTime", progress, 0.0F)) * 10.0;
    const double cycleProgress = shakeTime - std::floor(shakeTime);
    const double currentFrame = cycleProgress * 150.0;
    double shakeValue = 0.0;
    constexpr std::array<std::array<double, 2>, 5> shakeKeyframes{{
        {0.0, 0.0}, {50.0, 0.04}, {87.0, 0.02}, {125.0, 0.04},
        {150.0, 0.04}}};
    for (std::size_t index = 1U; index < shakeKeyframes.size(); ++index) {
      if (currentFrame <= shakeKeyframes[index][0]) {
        const double segmentProgress =
            (currentFrame - shakeKeyframes[index - 1U][0]) /
            (shakeKeyframes[index][0] - shakeKeyframes[index - 1U][0]);
        shakeValue =
            shakeKeyframes[index - 1U][1] * (1.0 - segmentProgress) +
            shakeKeyframes[index][1] * segmentProgress;
        break;
      }
    }
    const double randomSeed = stateContext
                                  ? static_cast<double>(
                                        stateContext->randomSeed & 0xffffffU)
                                  : 0.0;
    const auto random01 = [](const double first,
                             const double second) noexcept {
      const double value =
          std::sin(first * 12.9898 + second * 78.233) * 43758.5453;
      return value - std::floor(value);
    };
    const double randomX =
        (random01(randomSeed, std::floor(shakeTime * 9.2)) * 2.0 - 1.0) *
            0.4 +
        0.2;
    const double randomY =
        (random01(randomSeed + 1.0, std::floor(shakeTime * 9.2)) * 2.0 -
         1.0) *
            0.4 +
        0.2;
    const double interpolation =
        shakeTime * 9.2 - std::floor(shakeTime * 9.2);
    const std::array<float, 2> shake{
        static_cast<float>(randomX * interpolation * shakeValue * 0.45 *
                           static_cast<double>(width) /
                           static_cast<double>(height)),
        static_cast<float>(randomY * interpolation * shakeValue * 0.45)};
    SkRuntimeEffectBuilder builder(program.effect);
    builder.child("inputTexture") = RawLinearShader(sourceImage);
    builder.uniform("textureSize") = fullSize;
    builder.uniform("inverseScale") = 1.0F / scale;
    builder.uniform("backAlpha") =
        SampleParameter(effect, "backAlpha", progress, 1.0F);
    builder.uniform("mixWithBlack") =
        SampleParameter(effect, "mixWithBlack", progress, 0.0F);
    builder.uniform("offsetY") =
        SampleParameter(effect, "offsetY_0", progress, 0.0F);
    builder.uniform("shake") = shake;
    builder.uniform("shakeScale") =
        SampleParameter(effect, "shakeScale", progress, 1.0F);
    builder.uniform("textExpandRatio") =
        std::array<float, 2>{1.000001F, 1.000001F};
    auto outputImage = renderer.Draw(fullInfo, builder.makeShader());
    if (!outputImage)
      return {};
    RecordExecutedPass(executionTrace, "projection", "projection", 1U, 1U,
                       fullInfo,
                       QtTextPostEffectSurfaceSemantics::ColorPremultiplied);
    return PictureFromImage(outputImage, rasterBounds, recordingBounds);
  }

  if (kind == text::TextPostEffectKind::SimpleChoker &&
      FindParameter(effect, "chokeMatte")) {
    const auto &downscaleProgram =
        GetTextRuntimeProgram(TextRuntimeShader::PostSimpleChokerDownscale);
    const auto &matteProgram =
        GetTextRuntimeProgram(TextRuntimeShader::PostSimpleChokerMatte);
    const auto &blendProgram =
        GetTextRuntimeProgram(TextRuntimeShader::PostSimpleChokerBlend);
    if (!ProgramsAvailable({&downscaleProgram, &matteProgram, &blendProgram},
                           "simple-choker")) {
      return {};
    }
    const float quality =
        SampleParameter(effect, "quality", progress, 0.5F);
    const int downsampleWidth =
        std::max(1, static_cast<int>(static_cast<float>(width) * quality));
    const int downsampleHeight =
        std::max(1, static_cast<int>(static_cast<float>(height) * quality));
    const auto downsampleInfo = StageImageInfo(
        downsampleWidth, downsampleHeight,
        QtTextPostEffectSurfaceSemantics::ColorPremultiplied);
    const auto matteInfo = StageImageInfo(
        downsampleWidth, downsampleHeight,
        QtTextPostEffectSurfaceSemantics::RawRgbaData);
    const std::array<float, 2> downsampleSize{
        static_cast<float>(downsampleWidth),
        static_cast<float>(downsampleHeight)};
    const auto keyColor = VectorParameter(
        effect, "simpleColorKey", {1.0F, 0.0F, 0.0F, 1.0F});

    SkRuntimeEffectBuilder downscaleBuilder(downscaleProgram.effect);
    downscaleBuilder.child("inputTexture") = RawLinearShader(sourceImage);
    downscaleBuilder.uniform("inputSize") = fullSize;
    downscaleBuilder.uniform("outputSize") = downsampleSize;
    downscaleBuilder.uniform("enableColorKey") =
        SampleParameter(effect, "enableColorKey", progress, 0.0F);
    downscaleBuilder.uniform("colorTolerance") =
        SampleParameter(effect, "colorTolerance", progress, 0.1F);
    downscaleBuilder.uniform("keyColor") = keyColor;
    auto downsampleImage =
        renderer.Draw(downsampleInfo, downscaleBuilder.makeShader());
    if (!downsampleImage)
      return {};
    RecordExecutedPass(
        executionTrace, "simple_choker", "simple_choker_downscale", 1U, 3U,
        downsampleInfo,
        QtTextPostEffectSurfaceSemantics::ColorPremultiplied);

    SkRuntimeEffectBuilder matteBuilder(matteProgram.effect);
    matteBuilder.child("inputTexture") = RawLinearShader(downsampleImage);
    matteBuilder.uniform("textureSize") = downsampleSize;
    matteBuilder.uniform("chokeAmount") =
        SampleParameter(effect, "chokeMatte", progress, 0.0F) * 0.5F *
        quality;
    auto matteImage = renderer.Draw(matteInfo, matteBuilder.makeShader());
    if (!matteImage)
      return {};
    RecordExecutedPass(executionTrace, "simple_choker",
                       "simple_choker_matte", 2U, 3U, matteInfo,
                       QtTextPostEffectSurfaceSemantics::RawRgbaData);

    SkRuntimeEffectBuilder blendBuilder(blendProgram.effect);
    blendBuilder.child("inputTexture") = RawLinearShader(sourceImage);
    blendBuilder.child("matteTexture") = RawLinearShader(matteImage);
    blendBuilder.uniform("inputSize") = fullSize;
    blendBuilder.uniform("matteSize") = downsampleSize;
    blendBuilder.uniform("view") =
        SampleParameter(effect, "view", progress, 0.0F);
    auto outputImage = renderer.Draw(fullInfo, blendBuilder.makeShader());
    if (!outputImage)
      return {};
    RecordExecutedPass(
        executionTrace, "simple_choker", "simple_choker_blend", 3U, 3U,
        fullInfo, QtTextPostEffectSurfaceSemantics::ColorPremultiplied);
    return PictureFromImage(outputImage, rasterBounds, recordingBounds);
  }

  if ((kind >= text::TextPostEffectKind::SimpleChoker &&
       kind <= text::TextPostEffectKind::PerspectiveEchoTrail) ||
      kind == text::TextPostEffectKind::PulseEnvelope) {
    const auto &program =
        GetTextRuntimeProgram(TextRuntimeShader::PostSemanticPostEffect);
    if (!ProgramsAvailable({&program}, "semantic-post-effect"))
      return {};
    float mode = 0.0F;
    const char *chain = "simple_choker";
    std::array<float, 4> p0{};
    std::array<float, 4> p1{};
    std::array<float, 4> p2{};
    switch (kind) {
    case text::TextPostEffectKind::SimpleChoker: {
      p0 = {SampleParameter(effect, "radius", progress, 6.0F),
            SampleParameter(effect, "amount", progress, effect.amount),
            SampleParameter(effect, "softness", progress, 0.02F),
            SampleParameter(effect, "threshold", progress, 0.5F)};
      break;
    }
    case text::TextPostEffectKind::CCLens: {
      mode = 1.0F;
      chain = "cc_lens";
      const auto center =
          VectorParameter(effect, "center", {0.5F, 0.5F, 0.0F, 0.0F});
      const bool qtRadiusContract = FindParameter(effect, "radius") != nullptr;
      p0 = {center[0], center[1],
            SampleParameter(effect, qtRadiusContract ? "radius" : "size",
                            progress, 1.0F),
            qtRadiusContract
                ? SampleParameter(effect, "convergence", progress, 0.0F)
                : SampleParameter(
                      effect, "distortion", progress,
                      FindParameter(effect, "convergence") ? 0.0F
                                                             : effect.amount)};
      p1 = {SampleParameter(effect, "convergence", progress, 0.0F),
            SampleParameter(effect, "fieldOfView", progress, 0.0F),
            SampleParameter(effect, "invert", progress, 0.0F),
            SampleParameter(effect, "orientation", progress, 0.0F)};
      p2 = {0.0F, 0.0F, 0.0F, qtRadiusContract ? 1.0F : 0.0F};
      break;
    }
    case text::TextPostEffectKind::OpticsCompensation: {
      mode = 2.0F;
      chain = "optics_compensation";
      const auto center =
          VectorParameter(effect, "center", {0.5F, 0.5F, 0.0F, 0.0F});
      const float reverse = SampleParameter(
          effect, "reverseLensDistortion", progress, 1.0F);
      p0 = {center[0], center[1],
            SampleParameter(effect, "fieldOfView", progress, effect.amount),
            reverse};
      p1 = {SampleParameter(effect, "viewMode", progress, 0.0F),
            SampleParameter(effect, "optimalPixels", progress, 0.0F),
            SampleParameter(effect, "resize", progress, 0.0F), 0.0F};
      break;
    }
    case text::TextPostEffectKind::Projection: {
      mode = 3.0F;
      chain = "projection";
      const auto center =
          VectorParameter(effect, "center", {0.5F, 0.5F, 0.0F, 0.0F});
      auto scale =
          VectorParameter(effect, "scale", {1.0F, 1.0F, 0.0F, 0.0F});
      if (const auto *parameter = FindParameter(effect, "scale");
          parameter && parameter->values.size() == 1U) {
        scale[1] = scale[0];
      }
      const auto vanishingPoint = VectorParameter(
          effect, "vanishingPoint", {0.5F, 0.5F, 0.0F, 0.0F});
      p0 = {SampleParameter(effect, "rotationX", progress, 0.0F),
            SampleParameter(effect, "rotationY", progress, 0.0F),
            SampleParameter(effect, "rotationZ", progress, 0.0F),
            SampleParameter(effect, "perspective", progress, effect.amount)};
      p1 = {center[0], center[1], scale[0], scale[1]};
      p2 = {vanishingPoint[0], vanishingPoint[1],
            SampleParameter(effect, "fieldOfView", progress, 0.0F),
            SampleParameter(effect, "focalLength", progress, 1.0F)};
      break;
    }
    case text::TextPostEffectKind::DynamicSignalGlitch:
      mode = 4.0F;
      chain = "dynamic_signal_glitch";
      if (FindParameter(effect, "initialGlitchStrength")) {
        p0 = {SampleParameter(effect, "intensity", progress, 1.0F),
              SampleParameter(effect, "initialGlitchStrength", progress,
                              0.5F),
              SampleParameter(effect, "initialFlashFrequency", progress,
                              10.0F),
              SampleParameter(effect, "initialColorOffset", progress,
                              10.0F)};
        p1 = {SampleParameter(effect, "initialDistortionAmount", progress,
                              5.0F),
              static_cast<float>(width) / static_cast<float>(height), 0.0F,
              0.0F};
        p2 = {0.0F, 1.0F, 0.0F, 0.0F};
      } else {
        p0 = {SampleParameter(effect, "amount", progress,
                              effect.amount * static_cast<float>(width)),
              SampleParameter(effect, "blockSize", progress, 8.0F),
              SampleParameter(effect, "chromaShift", progress, 0.0F),
              SampleParameter(
                  effect, "seed", progress,
                  stateContext
                      ? static_cast<float>(stateContext->randomSeed &
                                           0xffffffU)
                      : 0.0F)};
        p1 = {SampleParameter(effect, "frequency", progress, 12.0F),
              SampleParameter(effect, "phase", progress, 0.0F),
              SampleParameter(effect, "scanline", progress, 0.0F),
              SampleParameter(effect, "speed", progress, 1.0F)};
        p2 = {SampleParameter(effect, "intensity", progress, 1.0F), 0.0F,
              0.0F, 0.0F};
      }
      break;
    case text::TextPostEffectKind::ElectricPulseMotionBlur:
      mode = 5.0F;
      chain = "electric_pulse_motion_blur";
      if (FindParameter(effect, "maxBlurStrength")) {
        p0 = {SampleParameter(effect, "intensity", progress, 1.0F),
              SampleParameter(effect, "maxBlurStrength", progress, 0.5F),
              SampleParameter(effect, "minBlurStrength", progress, 0.0F),
              SampleParameter(effect, "syncCycle", progress, 3.0F)};
        p2 = {0.0F, 1.0F, 0.0F, 0.0F};
      } else {
        p0 = {SampleParameter(effect, "angle", progress, 0.0F),
              SampleParameter(effect, "radius", progress,
                              effect.amount * 16.0F),
              SampleParameter(effect, "samples", progress, 8.0F),
              SampleParameter(effect, "pulse", progress, 0.5F)};
        p1 = {SampleParameter(effect, "phase", progress, 0.0F),
              SampleParameter(effect, "exposure", progress, 1.0F),
              SampleParameter(effect, "speed", progress, 1.0F),
              SampleParameter(effect, "intensity", progress, 1.0F)};
        p2 = {SampleParameter(
                  effect, "amount", progress,
                  FindParameter(effect, "radius") ? 1.0F : effect.amount),
              0.0F, 0.0F, 0.0F};
      }
      break;
    case text::TextPostEffectKind::PulseEnvelope:
      mode = 5.0F;
      chain = "pulse_envelope";
      p0 = {SampleParameter(effect, "intensity", progress, effect.amount),
            SampleParameter(effect, "maxBlurStrength", progress, 0.5F),
            SampleParameter(effect, "minBlurStrength", progress, 0.0F),
            SampleParameter(effect, "syncCycle", progress, 3.0F)};
      p2 = {0.0F, 1.0F, 0.0F, 0.0F};
      break;
    case text::TextPostEffectKind::MorphologicalOutline: {
      mode = 6.0F;
      chain = "morphological_outline";
      const auto color = VectorParameter(
          effect, "outlineColor", {1.0F, 1.0F, 1.0F, 1.0F});
      p0 = {SampleParameter(effect, "intensity", progress, effect.amount),
            SampleParameter(effect, "ratio", progress,
                            static_cast<float>(width) /
                                static_cast<float>(height)),
            SampleParameter(effect, "size", progress, 1.0F),
            SampleParameter(effect, "scaleX", progress, 1.0F)};
      p1 = {SampleParameter(effect, "scaleY", progress, 1.0F),
            SampleParameter(effect, "offsetX", progress, 0.0F),
            SampleParameter(effect, "offsetY", progress, 0.0F), color[0]};
      p2 = {color[1], color[2], color[3], 0.0F};
      break;
    }
    case text::TextPostEffectKind::AlternatingSegmentMask: {
      mode = 7.0F;
      chain = "alternating_segment_mask";
      const auto color = VectorParameter(
          effect, "segmentColor", {1.0F, 1.0F, 1.0F, 1.0F});
      const auto displacement = VectorParameter(
          effect, "displacement", {0.0F, 0.0F, 0.0F, 0.0F});
      p0 = {SampleParameter(effect, "intensity", progress, effect.amount),
            SampleParameter(effect, "segmentWidth", progress, 0.1F),
            SampleParameter(effect, "rotationAngle", progress, 0.0F),
            displacement[0]};
      p1 = color;
      p2 = {displacement[1], 0.0F, 0.0F, 0.0F};
      break;
    }
    case text::TextPostEffectKind::CenterSplitDisplacement:
      mode = 8.0F;
      chain = "center_split_displacement";
      p0 = {SampleParameter(effect, "intensity", progress, effect.amount),
            SampleParameter(effect, "ratio", progress,
                            static_cast<float>(width) /
                                static_cast<float>(height)),
            SampleParameter(effect, "pointA", progress, 25.0F),
            SampleParameter(effect, "pointB", progress, 75.0F)};
      p1 = {SampleParameter(effect, "split", progress, 0.0F), 0.0F, 0.0F,
            0.0F};
      break;
    case text::TextPostEffectKind::CutAndDrop:
      mode = 9.0F;
      chain = "cut_and_drop";
      p0 = {SampleParameter(effect, "intensity", progress, effect.amount),
            SampleParameter(effect, "ratio", progress,
                            static_cast<float>(width) /
                                static_cast<float>(height)),
            SampleParameter(effect, "knifeProgress", progress, 0.0F),
            SampleParameter(effect, "dropStrand", progress, 0.0F)};
      p1 = {SampleParameter(effect, "slant", progress, 0.0F), 0.0F, 0.0F,
            0.0F};
      break;
    case text::TextPostEffectKind::BlockGlitchDotMatrix:
      mode = 10.0F;
      chain = "block_glitch_dot_matrix";
      p0 = {SampleParameter(effect, "intensity", progress, effect.amount),
            SampleParameter(effect, "ratio", progress,
                            static_cast<float>(width) /
                                static_cast<float>(height)),
            SampleParameter(effect, "glitchAmount", progress, 0.0F),
            SampleParameter(effect, "blockScale", progress, 0.0F)};
      p1 = {SampleParameter(effect, "jitterSpeed", progress, 0.0F), 0.0F,
            0.0F, 0.0F};
      break;
    case text::TextPostEffectKind::ProceduralFlameOutline: {
      mode = 11.0F;
      chain = "procedural_flame_outline";
      const auto color =
          VectorParameter(effect, "flameColor", {1.0F, 0.5F, 0.0F, 1.0F});
      p0 = {SampleParameter(effect, "intensity", progress, effect.amount),
            SampleParameter(effect, "ratio", progress,
                            static_cast<float>(width) /
                                static_cast<float>(height)),
            SampleParameter(effect, "loopTime", progress,
                            static_cast<float>(progress)),
            SampleParameter(effect, "flameSpeed", progress, 1.0F)};
      p1 = {SampleParameter(effect, "noiseScale", progress, 1.0F), color[0],
            color[1], color[2]};
      p2 = {color[3], 0.0F, 0.0F, 0.0F};
      break;
    }
    case text::TextPostEffectKind::CylindricalScroll:
      mode = 12.0F;
      chain = "cylindrical_scroll";
      p0 = {SampleParameter(effect, "intensity", progress, effect.amount),
            SampleParameter(effect, "ratio", progress,
                            static_cast<float>(width) /
                                static_cast<float>(height)),
            SampleParameter(effect, "scrollProgress", progress,
                            static_cast<float>(progress)),
            SampleParameter(effect, "itemCount", progress, 1.0F)};
      p1 = {SampleParameter(effect, "tunnelRadius", progress, 1.0F),
            SampleParameter(effect, "fov", progress, 1.0F),
            SampleParameter(effect, "pauseStrength", progress, 0.0F), 0.0F};
      break;
    case text::TextPostEffectKind::SplitSqueezeWave:
      mode = 13.0F;
      chain = "split_squeeze_wave";
      p0 = {SampleParameter(effect, "intensity", progress, effect.amount),
            SampleParameter(effect, "ratio", progress,
                            static_cast<float>(width) /
                                static_cast<float>(height)),
            SampleParameter(effect, "waveHeight", progress, 0.0F),
            SampleParameter(effect, "waveFreq", progress, 1.0F)};
      break;
    case text::TextPostEffectKind::PerspectiveEchoTrail: {
      mode = 14.0F;
      chain = "perspective_echo_trail";
      const auto color = VectorParameter(
          effect, "outlineColor", {1.0F, 1.0F, 1.0F, 1.0F});
      p0 = {SampleParameter(effect, "intensity", progress, effect.amount),
            SampleParameter(effect, "ratio", progress,
                            static_cast<float>(width) /
                                static_cast<float>(height)),
            SampleParameter(effect, "echoCount", progress, 8.0F),
            SampleParameter(effect, "echoSpacing", progress, 0.05F)};
      p1 = {SampleParameter(effect, "perspective", progress, 0.5F),
            SampleParameter(effect, "curveTension", progress, 1.0F), color[0],
            color[1]};
      p2 = {color[2], color[3], 0.0F, 0.0F};
      break;
    }
    default:
      return {};
    }
    if (!std::all_of(p0.begin(), p0.end(), [](const float value) {
          return std::isfinite(value);
        }) ||
        !std::all_of(p1.begin(), p1.end(), [](const float value) {
          return std::isfinite(value);
        }) ||
        !std::all_of(p2.begin(), p2.end(), [](const float value) {
          return std::isfinite(value);
        })) {
      return {};
    }
    SkRuntimeEffectBuilder builder(program.effect);
    builder.child("inputTexture") = RawLinearShader(sourceImage);
    builder.uniform("textureSize") = fullSize;
    builder.uniform("mode") = mode;
    builder.uniform("progress") = static_cast<float>(progress);
    builder.uniform("p0") = p0;
    builder.uniform("p1") = p1;
    builder.uniform("p2") = p2;
    auto outputImage = renderer.Draw(fullInfo, builder.makeShader());
    if (!outputImage)
      return {};
    RecordExecutedPass(executionTrace, chain, chain, 1U, 1U, fullInfo,
                       QtTextPostEffectSurfaceSemantics::ColorPremultiplied,
                       gpuContext ? "skia-metal-semantic"
                                  : "skia-raster-semantic",
                       1U);
    return PictureFromImage(outputImage, rasterBounds, recordingBounds);
  }

  if (kind == text::TextPostEffectKind::DirectionalBlur) {
    const auto contract =
        ResolveQtDirectionalBlurContract(effect, progress, width, height);
    if (!contract.exactPathSupported)
      return {};
    if (contract.requiresAlphaOutlineFusion) {
      if (!StagePendingDirectionalBlur(source, effect, progress, stateContext))
        return {};
      return source;
    }
    std::string diagnostic;
    auto output = ApplyExactQtTextDirectionalBlurPicture(
        source, effect, progress, recordingBounds, gpuContext, &diagnostic);
    if (!output)
      return {};
    RecordExecutedPass(
        executionTrace, "directional_blur", "directional_blur_copy", 1U, 2U,
        StageImageInfo(contract.blurWidth, contract.blurHeight,
                       QtTextPostEffectSurfaceSemantics::RawRgbaData),
        QtTextPostEffectSurfaceSemantics::RawRgbaData,
        "qt-directional-blurs-metal", 1U);
    RecordExecutedPass(
        executionTrace, "directional_blur", "directional_blur", 2U, 2U,
        StageImageInfo(width, height,
                       QtTextPostEffectSurfaceSemantics::RawRgbaData),
        QtTextPostEffectSurfaceSemantics::RawRgbaData,
        "qt-directional-blurs-metal", 1U);
    return output;
  }

  if (kind == text::TextPostEffectKind::Dust) {
    if (!stateContext || stateContext->presentationWidth <= 0 ||
        stateContext->presentationHeight <= 0)
      return {};
    const auto contract = ResolveQtDustContract(
        effect, progress, width, height, stateContext->presentationWidth,
        stateContext->presentationHeight);
    if (!contract.exactPathSupported)
      return {};
    std::vector<std::uint8_t> sourcePixels;
    std::string diagnostic;
    if (!gpuContext && !ReadRawRgba8TopLeft(sourceImage, renderer, sourcePixels, diagnostic))
      return {};
    QtTextDustRenderRequest request;
    request.source = {sourcePixels.data(), sourcePixels.size(), width, height,
                      static_cast<std::size_t>(width) * 4U};
    const auto copyNoise = [](const QtDustNoiseContract &sourceNoise) {
      QtTextDustNoiseParameters target;
      target.brightness = sourceNoise.brightness;
      target.contrast = sourceNoise.contrast;
      target.quantity = sourceNoise.quantity;
      target.complexity = sourceNoise.complexity;
      target.evolutionDegrees = sourceNoise.evolutionDegrees;
      target.cycle = sourceNoise.cycle;
      target.offsetX = sourceNoise.offsetX;
      target.offsetY = sourceNoise.offsetY;
      target.rotateDegrees = sourceNoise.rotateDegrees;
      target.type = sourceNoise.type;
      target.subImpact = sourceNoise.subImpact;
      target.subScale = sourceNoise.subScale;
      target.subRotateDegrees = sourceNoise.subRotateDegrees;
      target.subOffsetX = sourceNoise.subOffsetX;
      target.subOffsetY = sourceNoise.subOffsetY;
      target.pictureScale = sourceNoise.pictureScale;
      return target;
    };
    request.parameters.noise = copyNoise(contract.noise);
    request.parameters.maskNoise = copyNoise(contract.maskNoise);
    request.parameters.distortionIntensity = contract.distortionIntensity;
    request.parameters.gravity = contract.gravity;
    request.parameters.gravityRotationDegrees =
        contract.gravityRotationDegrees;
    request.parameters.maskType =
        static_cast<QtTextDustMaskType>(contract.maskType);
    request.parameters.maskFeather = contract.maskFeather;
    request.parameters.maskLineRotationRadians =
        contract.maskLineRotationRadians;
    request.implementationVersion = contract.implementationVersion;
    request.progressPercent = contract.progressPercent;
    request.noiseTextureWidth = contract.noiseWidth;
    request.noiseTextureHeight = contract.noiseHeight;
    auto &holder = DustMetalRuntime();
    std::vector<std::uint8_t> outputPixels;
    sk_sp<SkImage> outputImage;
    {
      std::lock_guard<std::mutex> lock(holder.renderMutex);
      if (!holder.runtime) return {};
      if (gpuContext) {
        outputImage = gpuContext->RenderQtTextRawPass(
            sourceImage, width, height,
            [&](void *input, void *output, void *queue,
                const NativeCommandSubmission &submit, std::string &failure) {
              request.source.nativeTexture = input;
              const NativeRgba8TextureTarget target{output, queue, submit};
              return holder.runtime->Render(request, outputPixels, failure, &target);
            }, diagnostic);
      } else if (!holder.runtime->Render(request, outputPixels, diagnostic)) {
        return {};
      }
    }
    if (!gpuContext) outputImage =
        RawRgba8TopLeftImage(std::move(outputPixels), width, height);
    if (!outputImage)
      return {};
    constexpr std::array<const char *, 3> kStages{
        "dust_noise", "dust_mask_noise", "dust_particles"};
    for (std::size_t index = 0; index < kStages.size(); ++index) {
      const bool particle = index == 2U;
      RecordExecutedPass(
          executionTrace, "dust", kStages[index], index + 1U, kStages.size(),
          StageImageInfo(particle ? width : contract.noiseWidth,
                         particle ? height : contract.noiseHeight,
                         QtTextPostEffectSurfaceSemantics::RawRgbaData),
          QtTextPostEffectSurfaceSemantics::RawRgbaData,
          "qt-dust-metal", contract.implementationVersion);
    }
    return PictureFromImage(outputImage, rasterBounds, recordingBounds);
  }

  if (kind == text::TextPostEffectKind::DeepGlow) {
    DumpStage(sourceImage, static_cast<float>(progress), "deep-glow-source",
              gpuContext,
              QtTextPostEffectSurfaceSemantics::RawRgbaData);
    const auto contract =
        ResolveQtTypedDeepGlowContract(effect, progress, width, height);
    if (!contract.exactPathSupported)
      return {};
    const auto rawFullInfo = StageImageInfo(
        width, height, QtTextPostEffectSurfaceSemantics::RawRgbaData);
    const auto blurInfo = StageImageInfo(
        contract.blurWidth, contract.blurHeight,
        QtTextPostEffectSurfaceSemantics::RawRgbaData);
    const std::size_t passCount =
        2U + static_cast<std::size_t>(contract.glowIterations) * 4U;

    auto &deepGlowMetal = SoftGlowMetalRuntime();
    if (deepGlowMetal.runtime) {
      std::vector<std::uint8_t> sourcePixels;
      std::string nativeError;
      if (!gpuContext &&
          !ReadRawRgba8TopLeft(sourceImage, renderer, sourcePixels,
                              nativeError)) {
        std::fprintf(stderr,
                     "[VIDEOCUT_TEXT_QT_DEEP_GLOW_FAILURE] version=%u "
                     "reason=%s fallback=forbidden\n",
                     kQtTextDeepGlowMetalImplementationVersion,
                     nativeError.c_str());
        return {};
      }
      const float ratio = std::clamp(
          SampleParameter(effect, "ratio", progress, 1.0F), 0.0F, 2.0F);
      const float gammaCorrect =
          SampleParameter(effect, "gammaCorrect", progress, 1.0F);
      const float authoredBlendMode =
          SampleParameter(effect, "blendMode", progress, 1.0F);
      QtTextDeepGlowMetalRequest request;
      request.source = {sourcePixels.data(), sourcePixels.size(), width, height,
                        static_cast<std::size_t>(width) * 4U};
      request.blurWidth = contract.blurWidth;
      request.blurHeight = contract.blurHeight;
      request.glowIterations = contract.glowIterations;
      request.postprocessIteration = contract.postprocessIteration;
      request.glowFromAlpha = contract.glowFromAlpha;
      request.chromaticAberration =
          contract.chromaticAberrationEnabled ? 1 : 0;
      request.redOffset = contract.chromaticOffsets[0] * 0.05F;
      request.greenOffset = contract.chromaticOffsets[1] * 0.05F;
      request.blueOffset = contract.chromaticOffsets[2] * 0.05F;
      request.gammaCorrect = gammaCorrect >= 0.5F ? 1 : 0;
      request.authoredGamma = contract.authoredGamma;
      request.blurGamma = contract.gamma;
      request.stepsInt = contract.stepsInt;
      request.aspect = {
          static_cast<float>(width) /
              static_cast<float>(std::max(width, height)),
          static_cast<float>(height) /
              static_cast<float>(std::max(width, height))};
      request.rotateDegrees =
          SampleParameter(effect, "rotate", progress, 0.0F);
      request.ratio = ratio;
      request.sampledSteps = contract.sampledSteps;
      request.strides = contract.strides;
      request.opacities = contract.opacities;
      request.exposure = contract.exposure;
      request.compositeBlendMode = authoredBlendMode >= 0.5F ? 0 : 1;
      request.postprocessBlendMode = 0;
      request.tint = contract.tintEnabled ? 1 : 0;
      request.tintMode = contract.tintMode;
      request.tintColor = contract.tintColor;
      request.tintMix = contract.tintMix;
      request.sourceOpacity = contract.sourceOpacity;

      std::vector<std::uint8_t> outputPixels;
      sk_sp<SkImage> outputImage;
      std::string executor;
      {
        std::lock_guard<std::mutex> lock(deepGlowMetal.renderMutex);
        executor = gpuContext ? "qt-lumi-glow-metal-texture-v1"
                              : deepGlowMetal.runtime->BackendName();
        if (gpuContext) {
          outputImage = gpuContext->RenderQtTextRawPass(
              sourceImage, width, height,
              [&](void *input, void *output, void *queue, const NativeCommandSubmission &submit, std::string &failure) {
                request.source.nativeTexture = input;
                const QtTextSoftGlowMetalTextureTarget target{output, queue, submit};
                return deepGlowMetal.runtime->RenderDeepGlow(
                    request, outputPixels, failure, &target);
              }, nativeError);
        } else if (deepGlowMetal.runtime->RenderDeepGlow(
                       request, outputPixels, nativeError)) {
          outputImage = RawRgba8TopLeftImage(std::move(outputPixels), width, height);
        }
        if (!outputImage) {
          std::fprintf(stderr,
                       "[VIDEOCUT_TEXT_QT_DEEP_GLOW_FAILURE] version=%u "
                       "reason=%s fallback=forbidden\n",
                       kQtTextDeepGlowMetalImplementationVersion,
                       nativeError.c_str());
          return {};
        }
      }
      std::size_t ordinal = 1U;
      RecordExecutedPass(executionTrace, "deep_glow",
                         "deep_glow_preprocess", ordinal++, passCount,
                         rawFullInfo,
                         QtTextPostEffectSurfaceSemantics::RawRgbaData,
                         executor.c_str(),
                         kQtTextDeepGlowMetalImplementationVersion);
      for (int index = 0; index < contract.glowIterations; ++index) {
        RecordExecutedPass(executionTrace, "deep_glow",
                           "deep_glow_downscale", ordinal++, passCount,
                           blurInfo,
                           QtTextPostEffectSurfaceSemantics::RawRgbaData,
                           executor.c_str(),
                           kQtTextDeepGlowMetalImplementationVersion);
        RecordExecutedPass(executionTrace, "deep_glow", "deep_glow_x",
                           ordinal++, passCount, blurInfo,
                           QtTextPostEffectSurfaceSemantics::RawRgbaData,
                           executor.c_str(),
                           kQtTextDeepGlowMetalImplementationVersion);
        RecordExecutedPass(executionTrace, "deep_glow", "deep_glow_y",
                           ordinal++, passCount, blurInfo,
                           QtTextPostEffectSurfaceSemantics::RawRgbaData,
                           executor.c_str(),
                           kQtTextDeepGlowMetalImplementationVersion);
        RecordExecutedPass(executionTrace, "deep_glow",
                           "deep_glow_composite", ordinal++, passCount,
                           rawFullInfo,
                           QtTextPostEffectSurfaceSemantics::RawRgbaData,
                           executor.c_str(),
                           kQtTextDeepGlowMetalImplementationVersion);
      }
      RecordExecutedPass(executionTrace, "deep_glow",
                         "deep_glow_postprocess", ordinal, passCount,
                         rawFullInfo,
                         QtTextPostEffectSurfaceSemantics::RawRgbaData,
                         executor.c_str(),
                         kQtTextDeepGlowMetalImplementationVersion);
      DumpStage(outputImage, static_cast<float>(progress),
                "deep-glow-postprocess", gpuContext,
                QtTextPostEffectSurfaceSemantics::RawRgbaData);
      return PictureFromImage(outputImage, rasterBounds, recordingBounds);
    }

    const auto &preprocessProgram =
        GetTextRuntimeProgram(TextRuntimeShader::PostDeepGlowPreprocess);
    const auto &downscaleProgram =
        GetTextRuntimeProgram(TextRuntimeShader::PostDeepGlowDownscale);
    const auto &blurProgram =
        GetTextRuntimeProgram(TextRuntimeShader::PostDeepGlowBlur);
    const auto &compositeProgram =
        GetTextRuntimeProgram(TextRuntimeShader::PostDeepGlowComposite);
    const auto &postprocessProgram =
        GetTextRuntimeProgram(TextRuntimeShader::PostDeepGlowPostprocess);
    if (!ProgramsAvailable({&preprocessProgram, &downscaleProgram, &blurProgram,
                            &compositeProgram, &postprocessProgram},
                           "deep-glow"))
      return {};
    const std::array<float, 2> blurSize{
        static_cast<float>(contract.blurWidth),
        static_cast<float>(contract.blurHeight)};
    std::size_t ordinal = 1U;

    const float gammaCorrect =
        SampleParameter(effect, "gammaCorrect", progress, 1.0F);
    SkRuntimeEffectBuilder preprocess(preprocessProgram.effect);
    preprocess.child("inputTexture") = RawLinearShader(sourceImage);
    preprocess.uniform("sourceSize") = fullSize;
    preprocess.uniform("glowFromAlpha") =
        SampleParameter(effect, "glowFromAlpha", progress, 1.0F);
    preprocess.uniform("chromaticAberration") =
        SampleParameter(effect, "ca", progress, 0.0F) >= 0.5F ? 1.0F
                                                               : 0.0F;
    preprocess.uniform("redOffset") =
        SampleParameter(effect, "redOffset", progress, 0.01F) * 0.05F;
    preprocess.uniform("greenOffset") =
        SampleParameter(effect, "greenOffset", progress, -0.01F) * 0.05F;
    preprocess.uniform("blueOffset") =
        SampleParameter(effect, "blueOffset", progress, 0.0F) * 0.05F;
    preprocess.uniform("gammaCorrect") =
        gammaCorrect >= 0.5F ? 1.0F : 0.0F;
    preprocess.uniform("gammaValue") = contract.authoredGamma;
    auto preprocessedImage =
        renderer.Draw(rawFullInfo, preprocess.makeShader());
    if (!preprocessedImage)
      return {};
    RecordExecutedPass(executionTrace, "deep_glow", "deep_glow_preprocess",
                       ordinal++, passCount, rawFullInfo,
                       QtTextPostEffectSurfaceSemantics::RawRgbaData);
    DumpStage(preprocessedImage, static_cast<float>(progress),
              "deep-glow-preprocess", gpuContext,
              QtTextPostEffectSurfaceSemantics::RawRgbaData);

    const float ratio = std::clamp(
        SampleParameter(effect, "ratio", progress, 1.0F), 0.0F, 2.0F);
    const float rotate =
        SampleParameter(effect, "rotate", progress, 0.0F);
    const float authoredBlendMode =
        SampleParameter(effect, "blendMode", progress, 1.0F);
    const float compositeBlendMode =
        authoredBlendMode >= 0.5F ? 0.0F : 1.0F;
    const std::array<float, 2> aspect{
        static_cast<float>(width) / static_cast<float>(std::max(width, height)),
        static_cast<float>(height) /
            static_cast<float>(std::max(width, height))};

    sk_sp<SkImage> iterationInput = preprocessedImage;
    std::array<sk_sp<SkImage>, 8> compositeImages{};
    for (int index = 0; index < contract.glowIterations; ++index) {
      SkRuntimeEffectBuilder downscale(downscaleProgram.effect);
      downscale.child("inputTexture") = RawLinearShader(iterationInput);
      downscale.uniform("inputSize") = fullSize;
      downscale.uniform("outputSize") = blurSize;
      auto downscaledImage = renderer.Draw(blurInfo, downscale.makeShader());
      if (!downscaledImage)
        return {};
      RecordExecutedPass(executionTrace, "deep_glow",
                         "deep_glow_downscale", ordinal++, passCount,
                         blurInfo,
                         QtTextPostEffectSurfaceSemantics::RawRgbaData);

      const auto renderBlur = [&](const sk_sp<SkImage> &input,
                                  const float angle) -> sk_sp<SkImage> {
        SkRuntimeEffectBuilder blur(blurProgram.effect);
        blur.child("inputTexture") = RawLinearShader(input);
        blur.uniform("textureSize") = blurSize;
        blur.uniform("gammaValue") = contract.gamma;
        blur.uniform("stepsInt") = contract.stepsInt;
        blur.uniform("angle") = angle;
        blur.uniform("aspect") = aspect;
        blur.uniform("rotate") = rotate;
        blur.uniform("steps") =
            contract.sampledSteps[index] *
            (angle < 45.0F ? ratio : (2.0F - ratio));
        blur.uniform("stride") = contract.strides[index];
        blur.uniform("sigma") = 4.0F;
        return renderer.Draw(blurInfo, blur.makeShader());
      };
      auto horizontalImage = renderBlur(downscaledImage, 0.0F);
      if (!horizontalImage)
        return {};
      RecordExecutedPass(executionTrace, "deep_glow", "deep_glow_x",
                         ordinal++, passCount, blurInfo,
                         QtTextPostEffectSurfaceSemantics::RawRgbaData);
      auto verticalImage = renderBlur(horizontalImage, 90.0F);
      if (!verticalImage)
        return {};
      RecordExecutedPass(executionTrace, "deep_glow", "deep_glow_y",
                         ordinal++, passCount, blurInfo,
                         QtTextPostEffectSurfaceSemantics::RawRgbaData);

      SkRuntimeEffectBuilder composite(compositeProgram.effect);
      composite.child("inputTexture") = RawLinearShader(iterationInput);
      composite.child("blurTexture") = RawLinearShader(verticalImage);
      composite.uniform("inputSize") = fullSize;
      composite.uniform("blurSize") = blurSize;
      composite.uniform("blendMode") = compositeBlendMode;
      composite.uniform("opacity") = contract.opacities[index];
      composite.uniform("gammaValue") = contract.gamma;
      composite.uniform("composite") = 1.0F;
      composite.uniform("multiplier") = contract.exposure;
      compositeImages[index] =
          renderer.Draw(rawFullInfo, composite.makeShader());
      if (!compositeImages[index])
        return {};
      RecordExecutedPass(executionTrace, "deep_glow",
                         "deep_glow_composite", ordinal++, passCount,
                         rawFullInfo,
                         QtTextPostEffectSurfaceSemantics::RawRgbaData);
      iterationInput = compositeImages[index];
    }

    const int postprocessIndex = contract.postprocessIteration - 1;
    if (postprocessIndex < 0 ||
        postprocessIndex >= static_cast<int>(compositeImages.size()) ||
        !compositeImages[postprocessIndex])
      return {};
    const auto tintColor =
        VectorParameter(effect, "tintColor", {1.0F, 0.0F, 0.0F, 1.0F});
    SkRuntimeEffectBuilder postprocess(postprocessProgram.effect);
    postprocess.child("glowTexture") =
        RawLinearShader(compositeImages[postprocessIndex]);
    postprocess.child("sourceTexture") = RawLinearShader(sourceImage);
    postprocess.uniform("textureSize") = fullSize;
    postprocess.uniform("blendMode") = 0.0F;
    postprocess.uniform("tint") =
        SampleParameter(effect, "tint", progress, 1.0F) >= 0.5F ? 1.0F
                                                                 : 0.0F;
    postprocess.uniform("tintMode") =
        SampleParameter(effect, "tintMode", progress, 1.0F);
    postprocess.uniform("tintColor") =
        std::array<float, 3>{tintColor[0], tintColor[1], tintColor[2]};
    postprocess.uniform("tintMix") =
        SampleParameter(effect, "tintMix", progress, 1.0F);
    postprocess.uniform("sourceOpacity") =
        SampleParameter(effect, "sourceOpacity", progress, 0.0F);
    auto outputImage =
        renderer.Draw(rawFullInfo, postprocess.makeShader());
    if (!outputImage)
      return {};
    RecordExecutedPass(executionTrace, "deep_glow",
                       "deep_glow_postprocess", ordinal, passCount,
                       rawFullInfo,
                       QtTextPostEffectSurfaceSemantics::RawRgbaData);
    DumpStage(outputImage, static_cast<float>(progress),
              "deep-glow-postprocess", gpuContext,
              QtTextPostEffectSurfaceSemantics::RawRgbaData);
    if (std::getenv("VIDEOCUT_TRACE_TEXT_DEEP_GLOW") != nullptr) {
      std::fprintf(stderr,
                   "[VIDEOCUT_TEXT_DEEP_GLOW_EXACT] progress=%.9g "
                   "target=%dx%d blur=%dx%d iterations=%d "
                   "postprocess_iteration=%d passes=%zu\n",
                   progress, width, height, contract.blurWidth,
                   contract.blurHeight, contract.glowIterations,
                   postprocessIndex + 1, passCount);
    }
    return PictureFromImage(outputImage, rasterBounds, recordingBounds);
  }

  if (kind == text::TextPostEffectKind::SGlow) {
    const auto contract =
        ResolveQtSGlowContract(effect, progress, width, height);
    const auto &thresholdProgram =
        GetTextRuntimeProgram(TextRuntimeShader::PostSGlowThreshold);
    const auto &gaussianProgram =
        GetTextRuntimeProgram(TextRuntimeShader::PostSGlowGaussian);
    const auto &compositeProgram =
        GetTextRuntimeProgram(TextRuntimeShader::PostSGlowComposite);
    if (!contract.exactPathSupported ||
        !ProgramsAvailable(
            {&thresholdProgram, &gaussianProgram, &compositeProgram}, "s-glow"))
      return {};
    const float quality = std::clamp(
        SampleParameter(effect, "quality", progress, 0.5F), 0.01F, 1.0F);
    constexpr float kMaximumEffectWidth = 720.0F;
    const float cappedScale =
        std::min(1.0F, kMaximumEffectWidth / static_cast<float>(width));
    const int blurWidth = std::max(
        1, static_cast<int>(std::floor(static_cast<float>(width) *
                                       cappedScale * quality)));
    const int blurHeight = std::max(
        1, static_cast<int>(std::floor(static_cast<float>(height) *
                                       cappedScale * quality)));
    const auto rawFullInfo = StageImageInfo(
        width, height, QtTextPostEffectSurfaceSemantics::RawRgbaData);
    const auto blurInfo = StageImageInfo(
        blurWidth, blurHeight, QtTextPostEffectSurfaceSemantics::RawRgbaData);
    const std::array<float, 2> blurSize{static_cast<float>(blurWidth),
                                        static_cast<float>(blurHeight)};
    constexpr std::size_t kPassCount = 6U;
    std::size_t ordinal = 1U;

    SkRuntimeEffectBuilder threshold(thresholdProgram.effect);
    threshold.child("inputTexture") = RawLinearShader(sourceImage);
    threshold.uniform("sourceSize") = fullSize;
    threshold.uniform("targetSize") = blurSize;
    threshold.uniform("glowFromAlpha") = std::clamp(
        SampleParameter(effect, "glowFromAlpha", progress, 0.0F), 0.0F,
        1.0F);
    threshold.uniform("threshold") = std::clamp(
        SampleParameter(effect, "threshold", progress, 0.5F), 0.0F,
        0.999999F);
    const auto thresholdAddColor = VectorParameter(
        effect, "thresholdAddColor", {0.0F, 0.0F, 0.0F, 1.0F});
    threshold.uniform("thresholdAddColor") = std::array<float, 3>{
        thresholdAddColor[0], thresholdAddColor[1], thresholdAddColor[2]};
    threshold.uniform("useAlphaThreshold") =
        contract.useAlphaThreshold >= 0.5F ? 1.0F : 0.0F;
    auto thresholdImage = renderer.Draw(blurInfo, threshold.makeShader());
    if (!thresholdImage)
      return {};
    DumpStage(sourceImage, progress, "s-glow-source", gpuContext,
              QtTextPostEffectSurfaceSemantics::RawRgbaData);
    DumpStage(thresholdImage, progress, "s-glow-threshold", gpuContext,
              QtTextPostEffectSurfaceSemantics::RawRgbaData);
    RecordExecutedPass(executionTrace, "s_glow", "s_glow_threshold",
                       ordinal++, kPassCount, blurInfo,
                       QtTextPostEffectSurfaceSemantics::RawRgbaData);

    const float glowWidth =
        std::fabs(SampleParameter(effect, "glowWidth", progress, 0.1F));
    const float baseRadius = glowWidth * static_cast<float>(blurWidth);
    const float widthX =
        std::fabs(SampleParameter(effect, "widthX", progress, 1.0F));
    const float widthY =
        std::fabs(SampleParameter(effect, "widthY", progress, 1.0F));
    const std::array<float, 4> channelWidths{
        std::fabs(SampleParameter(effect, "widthRed", progress, 1.0F)),
        std::fabs(SampleParameter(effect, "widthGreen", progress, 1.2F)),
        std::fabs(SampleParameter(effect, "widthBlue", progress, 1.4F)),
        1.0F};
    const auto radii = [&](const float axisWidth) {
      return std::array<float, 4>{baseRadius * axisWidth * channelWidths[0],
                                  baseRadius * axisWidth * channelWidths[1],
                                  baseRadius * axisWidth * channelWidths[2],
                                  baseRadius};
    };
    const auto sigmaParameters = [](const std::array<float, 4> &radius) {
      std::array<float, 4> sigma{};
      for (std::size_t channel = 0; channel < sigma.size(); ++channel)
        sigma[channel] = radius[channel] * radius[channel] / 2.88F;
      return sigma;
    };
    const auto horizontalRadius = radii(widthX);
    const auto verticalRadius = radii(widthY);
    const auto horizontalSigma = sigmaParameters(horizontalRadius);
    const auto verticalSigma = sigmaParameters(verticalRadius);
    const float edgeMode =
        SampleParameter(effect, "edgeMode", progress, 1.0F) >= 0.5F ? 1.0F
                                                                    : 0.0F;
    const float dither =
        SampleParameter(effect, "dither", progress, 0.0F);
    constexpr float kMaximumRadius = 60.0F;

    const auto renderGaussian =
        [&](const sk_sp<SkImage> &input, const bool vertical,
            const bool secondPair, const std::array<float, 4> &radius,
            const std::array<float, 4> &sigma) -> sk_sp<SkImage> {
      SkRuntimeEffectBuilder gaussian(gaussianProgram.effect);
      gaussian.child("inputTexture") = RawLinearShader(input);
      gaussian.uniform("screenSize") = blurSize;
      gaussian.uniform("axis") =
          vertical ? std::array<float, 2>{0.0F, 1.0F}
                   : std::array<float, 2>{1.0F, 0.0F};
      gaussian.uniform("edgeMode") = edgeMode;
      gaussian.uniform("dither") = dither;
      gaussian.uniform("colorRadius") = radius;
      gaussian.uniform("maxRadius") = kMaximumRadius;
      gaussian.uniform("colorSigma") = sigma;
      gaussian.uniform("packedInput") = vertical ? 1.0F : 0.0F;
      gaussian.uniform("secondPair") = secondPair ? 1.0F : 0.0F;
      gaussian.uniform("hashPlus") = vertical ? 0.223F : 0.199F;
      gaussian.uniform("hashMinus") = vertical ? 0.569F : 0.677F;
      return renderer.Draw(blurInfo, gaussian.makeShader());
    };

    const auto horizontalFirst = renderGaussian(
        thresholdImage, false, false, horizontalRadius, horizontalSigma);
    if (!horizontalFirst)
      return {};
    RecordExecutedPass(executionTrace, "s_glow",
                       "s_glow_horizontal_first", ordinal++, kPassCount,
                       blurInfo,
                       QtTextPostEffectSurfaceSemantics::RawRgbaData);
    const auto horizontalSecond = renderGaussian(
        thresholdImage, false, true, horizontalRadius, horizontalSigma);
    if (!horizontalSecond)
      return {};
    RecordExecutedPass(executionTrace, "s_glow",
                       "s_glow_horizontal_second", ordinal++, kPassCount,
                       blurInfo,
                       QtTextPostEffectSurfaceSemantics::RawRgbaData);
    const auto verticalFirst = renderGaussian(
        horizontalFirst, true, false, verticalRadius, verticalSigma);
    if (!verticalFirst)
      return {};
    RecordExecutedPass(executionTrace, "s_glow", "s_glow_vertical_first",
                       ordinal++, kPassCount, blurInfo,
                       QtTextPostEffectSurfaceSemantics::RawRgbaData);
    const auto verticalSecond = renderGaussian(
        horizontalSecond, true, true, verticalRadius, verticalSigma);
    if (!verticalSecond)
      return {};
    DumpStage(horizontalFirst, progress, "s-glow-horizontal-first", gpuContext,
              QtTextPostEffectSurfaceSemantics::RawRgbaData);
    DumpStage(horizontalSecond, progress, "s-glow-horizontal-second",
              gpuContext, QtTextPostEffectSurfaceSemantics::RawRgbaData);
    DumpStage(verticalFirst, progress, "s-glow-vertical-first", gpuContext,
              QtTextPostEffectSurfaceSemantics::RawRgbaData);
    DumpStage(verticalSecond, progress, "s-glow-vertical-second", gpuContext,
              QtTextPostEffectSurfaceSemantics::RawRgbaData);
    RecordExecutedPass(executionTrace, "s_glow", "s_glow_vertical_second",
                       ordinal++, kPassCount, blurInfo,
                       QtTextPostEffectSurfaceSemantics::RawRgbaData);

    SkRuntimeEffectBuilder composite(compositeProgram.effect);
    composite.child("inputTexture") = RawLinearShader(sourceImage);
    composite.child("blurTexture1") = RawLinearShader(verticalFirst);
    composite.child("blurTexture2") = RawLinearShader(verticalSecond);
    composite.uniform("sourceSize") = fullSize;
    composite.uniform("blurSize") = blurSize;
    composite.uniform("sourceOpacity") = std::clamp(
        SampleParameter(effect, "sourceOpacity", progress, 1.0F), 0.0F,
        1.0F);
    composite.uniform("backgroundBrightness") =
        std::max(0.0F, contract.bgBrightness);
    const auto glowColor =
        VectorParameter(effect, "glowColor", {1.0F, 1.0F, 1.0F, 1.0F});
    composite.uniform("glowColor") =
        std::array<float, 3>{glowColor[0], glowColor[1], glowColor[2]};
    composite.uniform("brightness") = std::max(
        0.0F, SampleParameter(effect, "brightness", progress, 2.0F));
    composite.uniform("glowUnderSource") = std::clamp(
        SampleParameter(effect, "glowUnderSource", progress, 0.0F), 0.0F,
        1.0F);
    composite.uniform("lightBackground") =
        std::clamp(contract.lightBackground, 0.0F, 1.0F);
    composite.uniform("combine") = static_cast<float>(std::clamp(
        static_cast<int>(std::lround(
            SampleParameter(effect, "combine", progress, 0.0F))),
        0, 4));
    auto outputImage = renderer.Draw(rawFullInfo, composite.makeShader());
    if (!outputImage)
      return {};
    DumpStage(outputImage, progress, "s-glow-composite", gpuContext,
              QtTextPostEffectSurfaceSemantics::RawRgbaData);
    RecordExecutedPass(executionTrace, "s_glow", "s_glow_composite",
                       ordinal, kPassCount, rawFullInfo,
                       QtTextPostEffectSurfaceSemantics::RawRgbaData);
    if (std::getenv("VIDEOCUT_TRACE_TEXT_SGLOW") != nullptr) {
      std::fprintf(stderr,
                   "[VIDEOCUT_TEXT_SGLOW_EXACT] progress=%.9g "
                   "target=%dx%d intermediate=%dx%d passes=%zu\n",
                   progress, width, height, blurWidth, blurHeight,
                   kPassCount);
    }
    return PictureFromImage(outputImage, rasterBounds, recordingBounds);
  }

  if (kind == text::TextPostEffectKind::ChromaticAberration) {
    const auto contract =
        ResolveQtChromaticAberrationContract(effect, progress, width, height);
    const auto &program =
        GetTextRuntimeProgram(TextRuntimeShader::PostChromaticAberration);
    if (!contract.exactPathSupported ||
        !ProgramsAvailable({&program}, "chromatic-aberration"))
      return {};
    SkRuntimeEffectBuilder builder(program.effect);
    builder.child("inputTexture") = RawLinearShader(sourceImage);
    builder.uniform("textureSize") = fullSize;
    builder.uniform("authoredOffset") =
        std::array<float, 2>{contract.offsetX, contract.offsetY};
    const auto rawInfo = StageImageInfo(
        width, height, QtTextPostEffectSurfaceSemantics::RawRgbaData);
    auto outputImage = renderer.Draw(rawInfo, builder.makeShader());
    if (!outputImage)
      return {};
    RecordExecutedPass(executionTrace, "chromatic_aberration",
                       "chromatic_aberration", 1U, 1U, rawInfo,
                       QtTextPostEffectSurfaceSemantics::RawRgbaData);
    return PictureFromImage(outputImage, rasterBounds, recordingBounds);
  }

  if (kind == text::TextPostEffectKind::WaveWarp) {
    const auto contract =
        ResolveQtWaveWarpContract(effect, progress, width, height);
    const auto &program =
        GetTextRuntimeProgram(TextRuntimeShader::PostWaveWarp);
    if (!contract.exactPathSupported ||
        !ProgramsAvailable({&program}, "wave-warp"))
      return {};
    SkRuntimeEffectBuilder builder(program.effect);
    builder.child("inputTexture") = RawLinearShader(sourceImage);
    builder.uniform("effectBounds") =
        std::array<float, 4>{0.0F, 0.0F, static_cast<float>(width),
                             static_cast<float>(height)};
    builder.uniform("amplitude") = contract.amplitude[0];
    builder.uniform("wavelength") = contract.wavelength;
    builder.uniform("phase") = contract.phaseRadians;
    builder.uniform("waveDirection") =
        std::array<float, 2>{std::cos(contract.angleRadians),
                             std::sin(contract.angleRadians)};
    builder.uniform("waveType") = contract.waveType;
    builder.uniform("fixedType") = contract.fixedType;
    builder.uniform("antiAliasing") = contract.antiAliasing;
    const auto rawInfo = StageImageInfo(
        width, height, QtTextPostEffectSurfaceSemantics::RawRgbaData);
    auto outputImage = renderer.Draw(rawInfo, builder.makeShader());
    if (!outputImage)
      return {};
    RecordExecutedPass(executionTrace, "wave_warp", "wave_warp", 1U, 1U,
                       rawInfo, QtTextPostEffectSurfaceSemantics::RawRgbaData);
    return PictureFromImage(outputImage, rasterBounds, recordingBounds);
  }

  if (kind == text::TextPostEffectKind::MultiShadow) {
    if (!stateContext || stateContext->renderGroupExpandRatioX <= 0.0F ||
        stateContext->renderGroupExpandRatioY <= 0.0F)
      return {};
    const auto contract = ResolveQtMultiShadowContract(
        effect, progress, width, height, stateContext->renderGroupExpandRatioX,
        stateContext->renderGroupExpandRatioY);
    const auto &program =
        GetTextRuntimeProgram(TextRuntimeShader::PostMultiShadow);
    if (!contract.exactPathSupported ||
        !ProgramsAvailable({&program}, "multi-shadow"))
      return {};
    SkRuntimeEffectBuilder builder(program.effect);
    builder.child("inputTexture") = RawLinearShader(sourceImage);
    builder.uniform("textureSize") = fullSize;
    builder.uniform("renderGroupExpandRatio") =
        contract.renderGroupExpandRatio;
    builder.uniform("layerCount") = contract.layerCount;
    builder.uniform("originalAlpha") = contract.originalAlpha;
    for (std::size_t index = 0; index < contract.layers.size(); ++index) {
      const auto &layer = contract.layers[index];
      const std::string transformName = "transform" + std::to_string(index);
      const std::string colorName = "color" + std::to_string(index);
      builder.uniform(transformName.c_str()) =
          std::array<float, 4>{layer.positionX, layer.positionY, layer.scale,
                               layer.rotationRadians};
      auto color = layer.color;
      color[3] *= layer.alpha;
      builder.uniform(colorName.c_str()) = color;
    }
    const auto rawInfo = StageImageInfo(
        width, height, QtTextPostEffectSurfaceSemantics::RawRgbaData);
    auto outputImage = renderer.Draw(rawInfo, builder.makeShader());
    if (!outputImage)
      return {};
    RecordExecutedPass(executionTrace, "multi_shadow", "multi_shadow", 1U,
                       1U, rawInfo,
                       QtTextPostEffectSurfaceSemantics::RawRgbaData);
    return PictureFromImage(outputImage, rasterBounds, recordingBounds);
  }

  if (kind == text::TextPostEffectKind::DistortChroma) {
    const auto contract =
        ResolveQtDistortChromaContract(effect, progress, width, height);
    if (!contract.exactPathSupported) {
      if (std::getenv("VIDEOCUT_TRACE_TEXT_QT_POSTFX") != nullptr) {
        std::fprintf(stderr,
                     "[VIDEOCUT_TEXT_QT_POSTFX_FAIL_CLOSED] "
                     "kind=distort_chroma reason=%s\n",
                     contract.unsupportedReason.c_str());
      }
      return {};
    }
    const bool captureIntermediates = ShouldDumpStage(progress);
    if (captureIntermediates) {
      DumpStage(sourceImage, progress, "distort-source", gpuContext,
                QtTextPostEffectSurfaceSemantics::RawRgbaData);
    }
    auto native = ExecuteNativeDistortChroma(
        sourceImage, contract, renderer, captureIntermediates);
    if (native.status != EngineCopyStatus::Executed || !native.outputImage) {
      if (std::getenv("VIDEOCUT_TRACE_TEXT_QT_POSTFX") != nullptr) {
        std::fprintf(stderr,
                     "[VIDEOCUT_TEXT_QT_POSTFX_FAIL_CLOSED] "
                     "kind=distort_chroma reason=%s\n",
                     native.diagnostic.c_str());
      }
      return {};
    }

    constexpr std::array<const char *, 6> kStages{
        "distort_lens",    "distort_blur_x1", "distort_blur_y1",
        "distort_blur_x2", "distort_blur_y2", "distort_output",
    };
    for (std::size_t index = 0; index < kStages.size(); ++index) {
      const bool outputPass = index + 1U == kStages.size();
      RecordExecutedPass(
          executionTrace, "distort_chroma", kStages[index], index + 1U,
          kStages.size(),
          StageImageInfo(outputPass ? width : contract.lensWidth,
                         outputPass ? height : contract.lensHeight,
                         QtTextPostEffectSurfaceSemantics::RawRgbaData),
          QtTextPostEffectSurfaceSemantics::RawRgbaData,
          native.executor.c_str(), native.executorVersion);
    }
    if (captureIntermediates) {
      DumpStage(native.lensImage, progress, "distort-lens", gpuContext,
                QtTextPostEffectSurfaceSemantics::RawRgbaData);
      DumpStage(native.blurX1Image, progress, "distort-blur-x1", gpuContext,
                QtTextPostEffectSurfaceSemantics::RawRgbaData);
      DumpStage(native.blurY1Image, progress, "distort-blur-y1", gpuContext,
                QtTextPostEffectSurfaceSemantics::RawRgbaData);
      DumpStage(native.blurX2Image, progress, "distort-blur-x2", gpuContext,
                QtTextPostEffectSurfaceSemantics::RawRgbaData);
      DumpStage(native.blurY2Image, progress, "distort-blur-y2", gpuContext,
                QtTextPostEffectSurfaceSemantics::RawRgbaData);
    }
    DumpStage(native.outputImage, progress, "distort-output", gpuContext,
              QtTextPostEffectSurfaceSemantics::RawRgbaData);
    if (std::getenv("VIDEOCUT_TRACE_TEXT_QT_POSTFX") != nullptr) {
      std::fprintf(
          stderr,
          "[VIDEOCUT_TEXT_QT_DISTORT_CHROMA] backend=%s target=%dx%d "
          "lens=%dx%d blur_steps=%d strides=%.9g,%.9g amount=%.9g "
          "warp=%.9g,%.9g chroma_steps=%d wrap=%d,%d status=executed\n",
          native.executor.c_str(), width, height, contract.lensWidth,
          contract.lensHeight, contract.blurSteps, contract.blurStrideFirst,
          contract.blurStrideSecond, contract.warpAmount, contract.warpRed,
          contract.warpBlue, contract.chromaSteps, contract.wrapModeX,
          contract.wrapModeY);
    }
    return PictureFromImage(native.outputImage, rasterBounds, recordingBounds);
  }

  if (kind == text::TextPostEffectKind::RadialBlur) {
    DumpStage(sourceImage, static_cast<float>(progress), "radial-blur-source",
              gpuContext,
              QtTextPostEffectSurfaceSemantics::RawRgbaData);
    const auto contract =
        ResolveQtRadialBlurContract(effect, progress, width, height);
    if (!contract.exactPathSupported) {
      if (std::getenv("VIDEOCUT_TRACE_TEXT_QT_POSTFX") != nullptr) {
        std::fprintf(stderr,
                     "[VIDEOCUT_TEXT_QT_POSTFX_FAIL_CLOSED] "
                     "kind=radial_blur reason=%s\n",
                     contract.unsupportedReason.c_str());
      }
      return {};
    }
    auto native = ExecuteNativeRadialBlur(sourceImage, contract, renderer);
    if (native.status != EngineCopyStatus::Executed || !native.outputImage) {
      if (std::getenv("VIDEOCUT_TRACE_TEXT_QT_POSTFX") != nullptr) {
        std::fprintf(stderr,
                     "[VIDEOCUT_TEXT_QT_POSTFX_FAIL_CLOSED] "
                     "kind=radial_blur reason=%s\n",
                     native.diagnostic.c_str());
      }
      return {};
    }
    RecordExecutedPass(
        executionTrace, "radial_blur", "radial_blur", 1U, 1U,
        StageImageInfo(width, height,
                       QtTextPostEffectSurfaceSemantics::RawRgbaData),
        QtTextPostEffectSurfaceSemantics::RawRgbaData, native.executor.c_str(),
        native.executorVersion);
    DumpStage(native.outputImage, progress, "radial-blur", gpuContext,
              QtTextPostEffectSurfaceSemantics::RawRgbaData);
    if (std::getenv("VIDEOCUT_TRACE_TEXT_QT_POSTFX") != nullptr) {
      std::fprintf(stderr,
                   "[VIDEOCUT_TEXT_QT_RADIAL_BLUR] version=%u backend=%s "
                   "target=%dx%d type=%d intensity=%.9g quality=%.9g "
                   "center=%.9g,%.9g dither=%.9g status=executed\n",
                   contract.implementationVersion, native.executor.c_str(),
                   width, height, contract.blurType, contract.intensity,
                   contract.quality, contract.center[0], contract.center[1],
                   contract.dither);
    }
    return PictureFromImage(native.outputImage, rasterBounds, recordingBounds);
  }

  if (kind == text::TextPostEffectKind::Shake) {
    DumpStage(sourceImage, static_cast<float>(progress), "shake-source",
              gpuContext,
              QtTextPostEffectSurfaceSemantics::RawRgbaData);
    const auto contract =
        ResolveQtShakeContract(
            effect, progress,
            stateContext ? stateContext->effectTimeSeconds : progress, width,
            height);
    if (!contract.exactPathSupported) {
      if (std::getenv("VIDEOCUT_TRACE_TEXT_QT_POSTFX") != nullptr) {
        std::fprintf(stderr,
                     "[VIDEOCUT_TEXT_QT_POSTFX_FAIL_CLOSED] kind=shake "
                     "reason=%s\n",
                     contract.unsupportedReason.c_str());
      }
      return {};
    }
    auto native = ExecuteNativeShake(sourceImage, contract, renderer);
    if (native.status != EngineCopyStatus::Executed || !native.outputImage) {
      if (std::getenv("VIDEOCUT_TRACE_TEXT_QT_POSTFX") != nullptr) {
        std::fprintf(stderr,
                     "[VIDEOCUT_TEXT_QT_POSTFX_FAIL_CLOSED] kind=shake "
                     "reason=%s\n",
                     native.diagnostic.c_str());
      }
      return {};
    }
    RecordExecutedPass(
        executionTrace, "shake", "shake", 1U, 1U,
        StageImageInfo(width, height,
                       QtTextPostEffectSurfaceSemantics::RawRgbaData),
        QtTextPostEffectSurfaceSemantics::RawRgbaData, native.executor.c_str(),
        native.executorVersion);
    DumpStage(native.outputImage, progress, "shake", gpuContext,
              QtTextPostEffectSurfaceSemantics::RawRgbaData);
    if (std::getenv("VIDEOCUT_TRACE_TEXT_QT_POSTFX") != nullptr) {
      std::fprintf(stderr,
                   "[VIDEOCUT_TEXT_QT_SHAKE] version=%u backend=%s "
                   "target=%dx%d fill=%d,%d wave_time=%.12g "
                   "tx=%.9g ty=%.9g status=executed\n",
                   contract.implementationVersion, native.executor.c_str(),
                   width, height, contract.fillModeX, contract.fillModeY,
                   contract.waveTimeSeconds, contract.uvMatrices[0][12],
                   contract.uvMatrices[0][13]);
    }
    return PictureFromImage(native.outputImage, rasterBounds, recordingBounds);
  }

  if (kind == text::TextPostEffectKind::Trail) {
    DumpStage(sourceImage, progress, "trail-source", gpuContext,
              QtTextPostEffectSurfaceSemantics::RawRgbaData);
    const auto contract =
        ResolveQtTrailContract(effect, progress, width, height);
    if (!contract.exactPathSupported) {
      if (std::getenv("VIDEOCUT_TRACE_TEXT_QT_POSTFX") != nullptr) {
        std::fprintf(stderr,
                     "[VIDEOCUT_TEXT_QT_POSTFX_FAIL_CLOSED] kind=trail "
                     "reason=%s\n",
                     contract.unsupportedReason.c_str());
      }
      // Trail is stateful: falling through to translated current-Page copies
      // would silently replace its temporal contract. Preserve Page instead.
      return source;
    }
    auto native =
        ExecuteNativeTrail(sourceImage, contract, renderer, stateContext);
    if (native.status != EngineCopyStatus::Executed || !native.outputImage) {
      if (std::getenv("VIDEOCUT_TRACE_TEXT_QT_POSTFX") != nullptr) {
        std::fprintf(stderr,
                     "[VIDEOCUT_TEXT_QT_POSTFX_FAIL_CLOSED] kind=trail "
                     "reason=%s\n",
                     native.diagnostic.c_str());
      }
      return source;
    }
    constexpr std::array<const char *, 5> kStages{
        "trail_blur_x", "trail_blur_y", "trail_time_a", "trail_time_b_commit",
        "trail_hint"};
    for (std::size_t index = 0; index < kStages.size(); ++index) {
      const bool floatHistory = index == 2U || index == 3U;
      RecordExecutedPass(executionTrace, "trail", kStages[index], index + 1U,
                         kStages.size(), fullInfo,
                         QtTextPostEffectSurfaceSemantics::RawRgbaData,
                         native.executor.c_str(), native.executorVersion,
                         floatHistory ? "RGBA32Float" : "RGBA8Unorm");
    }
    DumpStage(native.outputImage, progress, "trail-hint", gpuContext,
              QtTextPostEffectSurfaceSemantics::RawRgbaData);
    if (std::getenv("VIDEOCUT_TRACE_TEXT_QT_POSTFX") != nullptr) {
      std::fprintf(
          stderr,
          "[VIDEOCUT_TEXT_QT_POSTFX] kind=trail target=%dx%d passes=5 "
          "sample_ordinal=%llu resource_generation=%llu reset_reason=%u "
          "blur=%.9g samples=%d step=[%.9g %.9g] sigma=[%.9g %.9g] "
          "decay_step=%.9g executor=%s\n",
          width, height,
          static_cast<unsigned long long>(
              native.execution.committedSampleOrdinal),
          static_cast<unsigned long long>(native.execution.resourceGeneration),
          static_cast<unsigned>(native.execution.resetReason), contract.blur,
          contract.blurSamples, contract.stepX, contract.stepY, contract.sigmaX,
          contract.sigmaY, contract.decayStep, native.executor.c_str());
    }
    return PictureFromImage(native.outputImage, rasterBounds, recordingBounds);
  }

  if (kind == text::TextPostEffectKind::RadianceGlow) {
    const auto contract =
        ResolveQtRadianceGlowContract(effect, progress, width, height);
    if (!contract.exactPathSupported) {
      if (std::getenv("VIDEOCUT_TRACE_TEXT_QT_POSTFX") != nullptr) {
        std::fprintf(stderr,
                     "[VIDEOCUT_TEXT_QT_POSTFX_FALLBACK] kind=radiance "
                     "reason=%s\n",
                     contract.unsupportedReason.c_str());
      }
      return {};
    }
    const auto &thresholdProgram =
        GetTextRuntimeProgram(TextRuntimeShader::PostRadianceThreshold);
    const auto &blitProgram =
        GetTextRuntimeProgram(TextRuntimeShader::PostLinearBlit);
    const auto &blurProgram =
        GetTextRuntimeProgram(TextRuntimeShader::PostRadianceDirectionalBlur);
    const auto &blendProgram =
        GetTextRuntimeProgram(TextRuntimeShader::PostRadianceBlend);
    if (!ProgramsAvailable(
            {&thresholdProgram, &blitProgram, &blurProgram, &blendProgram},
            "radiance")) {
      return {};
    }
    const std::size_t passCount =
        static_cast<std::size_t>(contract.directionCount + contract.erodeIterations) + 3U;
    const auto blurInfo = StageImageInfo(
        contract.blurWidth, contract.blurHeight,
        QtTextPostEffectSurfaceSemantics::ColorPremultiplied);
    const std::array<float, 2> blurSize{
        static_cast<float>(contract.blurWidth),
        static_cast<float>(contract.blurHeight)};

    SkRuntimeEffectBuilder thresholdBuilder(thresholdProgram.effect);
    thresholdBuilder.child("inputTexture") = RawLinearShader(sourceImage);
    thresholdBuilder.uniform("textureSize") = fullSize;
    thresholdBuilder.uniform("thresholdType") = contract.thresholdType;
    thresholdBuilder.uniform("thresholdLow") = contract.thresholdLow;
    thresholdBuilder.uniform("thresholdHigh") = contract.thresholdHigh;
    thresholdBuilder.uniform("thresholdSmooth") = contract.thresholdSmooth;
    thresholdBuilder.uniform("grayScale") = contract.grayScale;
    auto thresholdImage =
        renderer.Draw(fullInfo, thresholdBuilder.makeShader());
    if (!thresholdImage)
      return {};
    RecordExecutedPass(executionTrace, "radiance", "radiance_threshold", 1U,
                       passCount, fullInfo,
                       QtTextPostEffectSurfaceSemantics::ColorPremultiplied);
    DumpStage(thresholdImage, progress, "radiance-threshold", gpuContext);

    SkRuntimeEffectBuilder blitBuilder(blitProgram.effect);
    blitBuilder.child("inputTexture") = RawLinearShader(thresholdImage);
    blitBuilder.uniform("inputSize") = fullSize;
    blitBuilder.uniform("outputSize") = blurSize;
    auto downsampleImage = renderer.Draw(blurInfo, blitBuilder.makeShader());
    if (!downsampleImage)
      return {};
    RecordExecutedPass(executionTrace, "radiance", "radiance_downsample", 2U,
                       passCount, blurInfo,
                       QtTextPostEffectSurfaceSemantics::ColorPremultiplied);
    DumpStage(downsampleImage, progress, "radiance-downsample", gpuContext);

    if (contract.erodeIterations > 0) {
      const auto &erodeProgram =
          GetTextRuntimeProgram(TextRuntimeShader::PostRadianceErode);
      if (!ProgramsAvailable({&erodeProgram}, "radiance-erode"))
        return {};
      for (int iteration = 0; iteration < contract.erodeIterations; ++iteration) {
        SkRuntimeEffectBuilder erodeBuilder(erodeProgram.effect);
        erodeBuilder.child("inputTexture") = RawLinearShader(downsampleImage);
        erodeBuilder.uniform("textureSize") = blurSize;
        erodeBuilder.uniform("erodeStep") =
            std::array<float, 2>{contract.erodeStepX, contract.erodeStepY};
        auto erodedImage = renderer.Draw(blurInfo, erodeBuilder.makeShader());
        if (!erodedImage)
          return {};
        downsampleImage = std::move(erodedImage);
        RecordExecutedPass(executionTrace, "radiance", "radiance_erode",
                           static_cast<std::size_t>(iteration) + 3U,
                           passCount, blurInfo,
                           QtTextPostEffectSurfaceSemantics::ColorPremultiplied);
        DumpStage(downsampleImage, progress,
                  "radiance-erode-" + std::to_string(iteration + 1), gpuContext);
      }
    }

    std::array<sk_sp<SkImage>, 4> blurImages;
    for (int direction = 0; direction < contract.directionCount; ++direction) {
      SkRuntimeEffectBuilder blurBuilder(blurProgram.effect);
      blurBuilder.child("inputTexture") = RawLinearShader(downsampleImage);
      blurBuilder.uniform("textureSize") = blurSize;
      blurBuilder.uniform("sampleCount") = contract.sampleCount;
      blurBuilder.uniform("sigma") = contract.sigma;
      blurBuilder.uniform("spaceDither") =
          direction == 0 ? contract.spaceDither : 0.0F;
      blurBuilder.uniform("sampleStep") =
          contract.directionSteps[static_cast<std::size_t>(direction)];
      blurBuilder.uniform("borderType") =
          direction == 0 ? contract.borderType : 0.0F;
      blurBuilder.uniform("exposure") =
          direction == 0 ? contract.exposure : 0.0F;
      blurImages[static_cast<std::size_t>(direction)] =
          renderer.Draw(blurInfo, blurBuilder.makeShader());
      if (!blurImages[static_cast<std::size_t>(direction)])
        return {};
      RecordExecutedPass(
          executionTrace, "radiance", "radiance_directional_blur",
          static_cast<std::size_t>(direction + contract.erodeIterations) + 3U,
          passCount, blurInfo,
          QtTextPostEffectSurfaceSemantics::ColorPremultiplied);
      DumpStage(blurImages[static_cast<std::size_t>(direction)], progress,
                "radiance-directional-blur-" + std::to_string(direction + 1),
                gpuContext);
    }

    SkRuntimeEffectBuilder blendBuilder(blendProgram.effect);
    for (std::size_t direction = 0; direction < blurImages.size(); ++direction) {
      const auto sourceDirection = std::min(
          direction, static_cast<std::size_t>(contract.directionCount - 1));
      blendBuilder.child("glowTexture" + std::to_string(direction + 1U)) =
          RawLinearShader(blurImages[sourceDirection]);
    }
    blendBuilder.child("inputTexture") = RawLinearShader(sourceImage);
    blendBuilder.uniform("textureSize") = fullSize;
    blendBuilder.uniform("glowSize") = blurSize;
    blendBuilder.uniform("exposure") = contract.exposure;
    blendBuilder.uniform("directionCount") =
        static_cast<float>(contract.directionCount);
    blendBuilder.uniform("glowColor") = contract.glowColor;
    blendBuilder.uniform("displayGlow") = contract.displayGlow;
    auto outputImage = renderer.Draw(fullInfo, blendBuilder.makeShader());
    if (!outputImage)
      return {};
    RecordExecutedPass(executionTrace, "radiance", "radiance_blend",
                       passCount, passCount, fullInfo,
                       QtTextPostEffectSurfaceSemantics::ColorPremultiplied);
    DumpStage(outputImage, progress, "radiance-blend", gpuContext);
    if (std::getenv("VIDEOCUT_TRACE_TEXT_QT_POSTFX") != nullptr) {
      std::fprintf(stderr,
                   "[VIDEOCUT_TEXT_QT_POSTFX] kind=radiance target=%dx%d "
                   "passes=%zu directions=%d sample=%.9g sigma=%.9g "
                   "step=[%.9g %.9g]\n",
                   width, height, passCount, contract.directionCount,
                   contract.sampleCount, contract.sigma, contract.stepX,
                   contract.stepY);
    }
    return PictureFromImage(outputImage, rasterBounds, recordingBounds);
  }

  if (kind == text::TextPostEffectKind::AlphaOutline) {
    PendingDirectionalBlur pendingDirectional;
    if (TakePendingDirectionalBlur(effect, stateContext,
                                   pendingDirectional)) {
      std::string diagnostic;
      auto output = ApplyExactQtTextDirectionalBlursChainPicture(
          pendingDirectional.source, pendingDirectional.effect, effect,
          pendingDirectional.progress, progress, recordingBounds, gpuContext,
          &diagnostic);
      if (!output)
        return {};
      RecordExecutedPass(
          executionTrace, "alpha_outline", "alpha_outline", 1U, 1U,
          StageImageInfo(width, height,
                         QtTextPostEffectSurfaceSemantics::ColorPremultiplied),
          QtTextPostEffectSurfaceSemantics::ColorPremultiplied,
          "qt-directional-blurs-metal",
          kQtTextDirectionalBlursImplementationVersion);
      return output;
    }
    const auto &program =
        GetTextRuntimeProgram(TextRuntimeShader::PostAlphaOutline);
    if (!ProgramsAvailable({&program}, "alpha-outline"))
      return {};
    const auto contract =
        ResolveQtAlphaOutlineContract(effect, progress, width, height);
    SkRuntimeEffectBuilder builder(program.effect);
    builder.child("inputTexture") = RawLinearShader(sourceImage);
    builder.uniform("textureSize") = fullSize;
    builder.uniform("offsetX") = contract.offsetX;
    builder.uniform("offsetY") = contract.offsetY;
    builder.uniform("ratio") = contract.ratio;
    builder.uniform("size") = contract.size;
    builder.uniform("scaleX") = contract.scaleX;
    builder.uniform("scaleY") = contract.scaleY;
    builder.uniform("outlineColor") = contract.outlineColor;
    builder.uniform("intensity") = contract.intensity;
    auto outputImage = renderer.Draw(fullInfo, builder.makeShader());
    if (!outputImage)
      return {};
    RecordExecutedPass(executionTrace, "alpha_outline", "alpha_outline", 1U, 1U,
                       fullInfo,
                       QtTextPostEffectSurfaceSemantics::ColorPremultiplied);
    DumpStage(outputImage, progress, "alpha-outline", gpuContext);
    if (std::getenv("VIDEOCUT_TRACE_TEXT_QT_POSTFX") != nullptr) {
      std::fprintf(stderr,
                   "[VIDEOCUT_TEXT_QT_POSTFX] kind=alpha-outline target=%dx%d "
                   "passes=1 ratio=%.9g size=%.9g intensity=%.9g\n",
                   width, height, contract.ratio, contract.size,
                   contract.intensity);
    }
    return PictureFromImage(outputImage, rasterBounds, recordingBounds);
  }

  if (kind == text::TextPostEffectKind::TurbulenceDisplacement) {
    const auto contract =
        ResolveQtTurbulenceContract(effect, progress, width, height);
    if (!contract.exactPathSupported) {
      if (std::getenv("VIDEOCUT_TRACE_TEXT_QT_POSTFX") != nullptr) {
        std::fprintf(stderr,
                     "[VIDEOCUT_TEXT_QT_POSTFX_FALLBACK] kind=turbulence "
                     "reason=%s\n",
                     contract.unsupportedReason.c_str());
      }
      return {};
    }
    auto native = ExecuteNativeTurbulence(sourceImage, contract, renderer,
                                          ShouldDumpStage(progress));
    if (native.status == EngineCopyStatus::Executed && native.outputImage) {
      RecordExecutedPass(
          executionTrace, "turbulence", "turbulence_noise", 1U, 2U,
          StageImageInfo(contract.noiseWidth, contract.noiseHeight,
                         QtTextPostEffectSurfaceSemantics::RawRgbaData),
          QtTextPostEffectSurfaceSemantics::RawRgbaData,
          native.executor.c_str(), native.executorVersion);
      DumpStage(native.noiseImage, progress, "turbulence-noise", gpuContext,
                QtTextPostEffectSurfaceSemantics::RawRgbaData);
      RecordExecutedPass(executionTrace, "turbulence",
                         "turbulence_displacement", 2U, 2U, fullInfo,
                         QtTextPostEffectSurfaceSemantics::ColorPremultiplied,
                         native.executor.c_str(), native.executorVersion);
      DumpStage(native.outputImage, progress, "turbulence-displacement",
                gpuContext);
      return PictureFromImage(native.outputImage, rasterBounds,
                              recordingBounds);
    }
    if (native.status == EngineCopyStatus::NativeExecutionFailed) {
      return {};
    }
    if (std::getenv("VIDEOCUT_TRACE_TEXT_QT_POSTFX") != nullptr) {
      std::fprintf(stderr,
                   "[VIDEOCUT_TEXT_QT_TURBULENCE_FALLBACK] "
                   "native_version=%u backend=skia-runtime-effect reason=%s\n",
                   kQtTextTurbulenceMetalImplementationVersion,
                   native.diagnostic.c_str());
    }
    const auto &noiseProgram =
        GetTextRuntimeProgram(TextRuntimeShader::PostTurbulenceNoise);
    const auto &displacementProgram =
        GetTextRuntimeProgram(TextRuntimeShader::PostTurbulenceDisplacement);
    if (!ProgramsAvailable({&noiseProgram, &displacementProgram},
                           "turbulence")) {
      return {};
    }
    const auto noiseInfo =
        StageImageInfo(contract.noiseWidth, contract.noiseHeight,
                       QtTextPostEffectSurfaceSemantics::RawRgbaData);
    const std::array<float, 2> noiseSize{
        static_cast<float>(contract.noiseWidth),
        static_cast<float>(contract.noiseHeight)};
    SkRuntimeEffectBuilder noiseBuilder(noiseProgram.effect);
    noiseBuilder.uniform("screenSize") = noiseSize;
    noiseBuilder.uniform("cycle") = contract.cycle;
    noiseBuilder.uniform("authoredOffset") =
        std::array<float, 2>{contract.offsetX, contract.offsetY};
    noiseBuilder.uniform("authoredRotate") = 0.0F;
    noiseBuilder.uniform("authoredScale") =
        std::array<float, 2>{contract.quantity, contract.quantity};
    noiseBuilder.uniform("turbulenceType") = contract.type;
    noiseBuilder.uniform("complexity") = contract.complexity;
    noiseBuilder.uniform("evolution") = contract.evolution;
    // These are the captured, immutable defaults from noise.material.
    noiseBuilder.uniform("subImpact") = 0.60000002384185791F;
    noiseBuilder.uniform("subScale") = 56.0F;
    noiseBuilder.uniform("subRotate") = 0.0F;
    noiseBuilder.uniform("subOffset") = std::array<float, 2>{0.0F, 0.0F};
    auto noiseImage = renderer.Draw(noiseInfo, noiseBuilder.makeShader());
    if (!noiseImage || noiseImage->alphaType() != kPremul_SkAlphaType)
      return {};
    RecordExecutedPass(executionTrace, "turbulence", "turbulence_noise", 1U, 2U,
                       noiseInfo,
                       QtTextPostEffectSurfaceSemantics::RawRgbaData);
    DumpStage(noiseImage, progress, "turbulence-noise", gpuContext,
              QtTextPostEffectSurfaceSemantics::RawRgbaData);

    SkRuntimeEffectBuilder displacementBuilder(displacementProgram.effect);
    displacementBuilder.child("noiseTexture") = RawNearestShader(noiseImage);
    displacementBuilder.child("inputTexture") = RawLinearShader(sourceImage);
    displacementBuilder.uniform("noiseSize") = noiseSize;
    displacementBuilder.uniform("textureSize") = fullSize;
    displacementBuilder.uniform("brightness") = 0.0F;
    displacementBuilder.uniform("contrast") = contract.contrast;
    displacementBuilder.uniform("quantity") = contract.quantity;
    displacementBuilder.uniform("range") = contract.range;
    displacementBuilder.uniform("turbulenceType") = contract.type;
    displacementBuilder.uniform("motionTileType") = contract.motionTileType;
    auto outputImage =
        renderer.Draw(fullInfo, displacementBuilder.makeShader());
    if (!outputImage)
      return {};
    RecordExecutedPass(executionTrace, "turbulence", "turbulence_displacement",
                       2U, 2U, fullInfo,
                       QtTextPostEffectSurfaceSemantics::ColorPremultiplied);
    DumpStage(outputImage, progress, "turbulence-displacement", gpuContext);
    if (std::getenv("VIDEOCUT_TRACE_TEXT_QT_POSTFX") != nullptr) {
      std::fprintf(stderr,
                   "[VIDEOCUT_TEXT_QT_POSTFX] kind=turbulence target=%dx%d "
                   "noise=%dx%d passes=2 noise_rt=raw_rgba8_unorm "
                   "skia_alpha_tag=premul-data-transport write=kSrc "
                   "sampling=manual-float-linear(raw-nearest) evolution=%.9g "
                   "contrast=%.9g\n",
                   width, height, contract.noiseWidth, contract.noiseHeight,
                   contract.evolution, contract.contrast);
    }
    return PictureFromImage(outputImage, rasterBounds, recordingBounds);
  }

  if (kind == text::TextPostEffectKind::GodRay) {
    const auto contract =
        ResolveQtGodRayContract(effect, progress, width, height);
    if (!contract.exactPathSupported) {
      if (std::getenv("VIDEOCUT_TRACE_TEXT_QT_POSTFX") != nullptr) {
        std::fprintf(stderr,
                     "[VIDEOCUT_TEXT_QT_POSTFX_FALLBACK] kind=god-ray "
                     "reason=%s\n",
                     contract.unsupportedReason.c_str());
      }
      return {};
    }
    const auto &thresholdProgram =
        GetTextRuntimeProgram(TextRuntimeShader::PostGodRayThreshold);
    const auto &gaussianProgram =
        GetTextRuntimeProgram(TextRuntimeShader::PostGodRayGaussian);
    const auto &compositeProgram =
        GetTextRuntimeProgram(TextRuntimeShader::PostGodRayComposite);
    const bool blurActive =
        contract.sampleCount > 0.00000999999974737875163555145263671875F;
    if (blurActive) {
      if (!ProgramsAvailable(
              {&thresholdProgram, &gaussianProgram, &compositeProgram},
              "god-ray")) {
        return {};
      }
    } else if (!ProgramsAvailable({&thresholdProgram, &compositeProgram},
                                  "god-ray")) {
      return {};
    }
    const auto rayInfo =
        StageImageInfo(contract.rayWidth, contract.rayHeight,
                       QtTextPostEffectSurfaceSemantics::ColorPremultiplied);
    const std::array<float, 2> raySize{static_cast<float>(contract.rayWidth),
                                       static_cast<float>(contract.rayHeight)};
    const std::size_t passCount = blurActive ? 4U : 2U;

    SkRuntimeEffectBuilder thresholdBuilder(thresholdProgram.effect);
    thresholdBuilder.child("inputTexture") = RawLinearShader(sourceImage);
    thresholdBuilder.uniform("inputSize") = fullSize;
    thresholdBuilder.uniform("outputSize") = raySize;
    thresholdBuilder.uniform("useAlphaThreshold") = contract.useAlphaThreshold;
    thresholdBuilder.uniform("colorType") = contract.colorType;
    thresholdBuilder.uniform("threshold") = contract.threshold;
    auto rayImage = renderer.Draw(rayInfo, thresholdBuilder.makeShader());
    if (!rayImage)
      return {};
    RecordExecutedPass(executionTrace, "god_ray", "god_ray_threshold", 1U,
                       passCount, rayInfo,
                       QtTextPostEffectSurfaceSemantics::ColorPremultiplied);
    DumpStage(rayImage, progress, "god-ray-threshold", gpuContext);

    std::size_t nextOrdinal = 2U;
    if (blurActive) {
      SkRuntimeEffectBuilder horizontalBuilder(gaussianProgram.effect);
      horizontalBuilder.child("inputTexture") = RawLinearShader(rayImage);
      horizontalBuilder.uniform("textureSize") = raySize;
      horizontalBuilder.uniform("sampleCount") = contract.sampleCount;
      horizontalBuilder.uniform("sigma") = contract.sigmaX;
      horizontalBuilder.uniform("sampleStep") =
          std::array<float, 2>{contract.dx, 0.0F};
      auto horizontalImage =
          renderer.Draw(rayInfo, horizontalBuilder.makeShader());
      if (!horizontalImage)
        return {};
      RecordExecutedPass(executionTrace, "god_ray", "god_ray_gaussian_x",
                         nextOrdinal++, passCount, rayInfo,
                         QtTextPostEffectSurfaceSemantics::ColorPremultiplied);
      DumpStage(horizontalImage, progress, "god-ray-gaussian-x", gpuContext);

      SkRuntimeEffectBuilder verticalBuilder(gaussianProgram.effect);
      verticalBuilder.child("inputTexture") = RawLinearShader(horizontalImage);
      verticalBuilder.uniform("textureSize") = raySize;
      verticalBuilder.uniform("sampleCount") = contract.sampleCount;
      verticalBuilder.uniform("sigma") = contract.sigmaY;
      verticalBuilder.uniform("sampleStep") =
          std::array<float, 2>{0.0F, contract.dy};
      rayImage = renderer.Draw(rayInfo, verticalBuilder.makeShader());
      if (!rayImage)
        return {};
      RecordExecutedPass(executionTrace, "god_ray", "god_ray_gaussian_y",
                         nextOrdinal++, passCount, rayInfo,
                         QtTextPostEffectSurfaceSemantics::ColorPremultiplied);
      DumpStage(rayImage, progress, "god-ray-gaussian-y", gpuContext);
    }

    SkRuntimeEffectBuilder compositeBuilder(compositeProgram.effect);
    compositeBuilder.child("rayTexture") = RawLinearShader(rayImage);
    compositeBuilder.child("inputTexture") = RawLinearShader(sourceImage);
    compositeBuilder.uniform("raySize") = raySize;
    compositeBuilder.uniform("textureSize") = fullSize;
    compositeBuilder.uniform("center") = contract.center;
    compositeBuilder.uniform("useAngle") = contract.useAngle;
    compositeBuilder.uniform("minAngle") = contract.minAngle;
    compositeBuilder.uniform("maxAngle") = contract.maxAngle;
    compositeBuilder.uniform("intensity") = contract.intensity;
    compositeBuilder.uniform("brightness") = contract.brightness;
    compositeBuilder.uniform("quality") = contract.quality;
    compositeBuilder.uniform("weightDecay") = contract.weightDecay;
    compositeBuilder.uniform("normalizationSample") =
        contract.normalizationSample;
    compositeBuilder.uniform("sampleScale") = contract.sampleScale;
    compositeBuilder.uniform("sampleBias") = contract.sampleBias;
    compositeBuilder.uniform("colorDecay") = contract.colorDecay;
    compositeBuilder.uniform("borderType") = contract.borderType;
    compositeBuilder.uniform("blendMode") = contract.blendMode;
    compositeBuilder.uniform("lightColor") = contract.lightColor;
    compositeBuilder.uniform("displayRayOnly") = contract.displayRayOnly;
    compositeBuilder.uniform("inverseGammaCorrection") =
        contract.inverseGammaCorrection;
    compositeBuilder.uniform("gamma") = contract.gamma;
    compositeBuilder.uniform("grayscaleCorrection") =
        contract.grayscaleCorrection;
    compositeBuilder.uniform("dither") = contract.dither;
    compositeBuilder.uniform("noiseIntensity") = contract.noiseIntensity;
    auto outputImage = renderer.Draw(fullInfo, compositeBuilder.makeShader());
    if (!outputImage)
      return {};
    RecordExecutedPass(executionTrace, "god_ray", "god_ray_composite",
                       nextOrdinal, passCount, fullInfo,
                       QtTextPostEffectSurfaceSemantics::ColorPremultiplied);
    DumpStage(outputImage, progress, "god-ray-composite", gpuContext);
    if (std::getenv("VIDEOCUT_TRACE_TEXT_QT_POSTFX") != nullptr) {
      std::fprintf(stderr,
                   "[VIDEOCUT_TEXT_QT_POSTFX] kind=god-ray target=%dx%d "
                   "ray=%dx%d passes=%zu threshold=%.9g sample=%.9g "
                   "sigma=[%.9g %.9g] center=[%.9g %.9g] brightness=%.9g\n",
                   width, height, contract.rayWidth, contract.rayHeight,
                   passCount, contract.threshold, contract.sampleCount,
                   contract.sigmaX, contract.sigmaY, contract.center[0],
                   contract.center[1], contract.brightness);
    }
    return PictureFromImage(outputImage, rasterBounds, recordingBounds);
  }

  if (kind == text::TextPostEffectKind::GaussianBlur) {
    const auto contract =
        ResolveQtGaussianBlurContract(effect, progress, width, height);
    if (!contract.exactPathSupported) {
      if (std::getenv("VIDEOCUT_TRACE_TEXT_QT_POSTFX") != nullptr) {
        std::fprintf(stderr,
                     "[VIDEOCUT_TEXT_QT_POSTFX_FALLBACK] "
                     "kind=gaussian-blur reason=%s\n",
                     contract.unsupportedReason.c_str());
      }
      return {};
    }
    const auto &blitProgram =
        GetTextRuntimeProgram(TextRuntimeShader::PostLinearBlit);
    const auto &gaussianProgram =
        GetTextRuntimeProgram(TextRuntimeShader::PostGaussianBlurSeparable);
    if (!ProgramsAvailable({&gaussianProgram}, "gaussian-blur")) {
      return {};
    }
    const auto rawFullInfo = StageImageInfo(
        width, height, QtTextPostEffectSurfaceSemantics::RawRgbaData);
    if (!contract.active) {
      auto copy = ExecuteGaussianEngineCopy(sourceImage, rawFullInfo, fullSize,
                                            fullSize, renderer, blitProgram,
                                            "gaussian_copy");
      if (copy.status != EngineCopyStatus::Executed || !copy.image)
        return {};
      auto outputImage = std::move(copy.image);
      RecordExecutedPass(executionTrace, "gaussian_blur", "gaussian_copy", 1U,
                         1U, rawFullInfo,
                         QtTextPostEffectSurfaceSemantics::RawRgbaData,
                         copy.executor.c_str(), copy.executorVersion);
      DumpStage(outputImage, progress, "gaussian-copy", gpuContext,
                QtTextPostEffectSurfaceSemantics::RawRgbaData);
      return PictureFromImage(outputImage, rasterBounds, recordingBounds);
    }

    const auto blurInfo =
        StageImageInfo(contract.blurWidth, contract.blurHeight,
                       QtTextPostEffectSurfaceSemantics::RawRgbaData);
    const std::array<float, 2> blurSize{
        static_cast<float>(contract.blurWidth),
        static_cast<float>(contract.blurHeight)};
    auto downsample =
        ExecuteGaussianEngineCopy(sourceImage, blurInfo, fullSize, blurSize,
                                  renderer, blitProgram, "gaussian_downsample");
    if (downsample.status != EngineCopyStatus::Executed || !downsample.image)
      return {};
    auto downsampleImage = std::move(downsample.image);
    RecordExecutedPass(executionTrace, "gaussian_blur", "gaussian_downsample",
                       1U, 4U, blurInfo,
                       QtTextPostEffectSurfaceSemantics::RawRgbaData,
                       downsample.executor.c_str(), downsample.executorVersion);
    DumpStage(downsampleImage, progress, "gaussian-downsample", gpuContext,
              QtTextPostEffectSurfaceSemantics::RawRgbaData);

    auto horizontal = ExecuteGaussianAxis(
        downsampleImage, QtTextGaussianAxis::Horizontal, contract.sampleCount,
        contract.sigmaX, contract.stepX, contract.gamma, blurInfo, renderer,
        gaussianProgram, "gaussian_x");
    if (horizontal.status != EngineCopyStatus::Executed || !horizontal.image)
      return {};
    auto horizontalImage = std::move(horizontal.image);
    RecordExecutedPass(executionTrace, "gaussian_blur", "gaussian_x", 2U, 4U,
                       blurInfo, QtTextPostEffectSurfaceSemantics::RawRgbaData,
                       horizontal.executor.c_str(), horizontal.executorVersion);
    DumpStage(horizontalImage, progress, "gaussian-x", gpuContext,
              QtTextPostEffectSurfaceSemantics::RawRgbaData);

    auto vertical = ExecuteGaussianAxis(
        horizontalImage, QtTextGaussianAxis::Vertical, contract.sampleCount,
        contract.sigmaY, contract.stepY, contract.gamma, blurInfo, renderer,
        gaussianProgram, "gaussian_y");
    if (vertical.status != EngineCopyStatus::Executed || !vertical.image)
      return {};
    auto verticalImage = std::move(vertical.image);
    RecordExecutedPass(executionTrace, "gaussian_blur", "gaussian_y", 3U, 4U,
                       blurInfo, QtTextPostEffectSurfaceSemantics::RawRgbaData,
                       vertical.executor.c_str(), vertical.executorVersion);
    DumpStage(verticalImage, progress, "gaussian-y", gpuContext,
              QtTextPostEffectSurfaceSemantics::RawRgbaData);

    auto upsample = ExecuteGaussianEngineCopy(verticalImage, rawFullInfo,
                                              blurSize, fullSize, renderer,
                                              blitProgram, "gaussian_upsample");
    if (upsample.status != EngineCopyStatus::Executed || !upsample.image)
      return {};
    auto outputImage = std::move(upsample.image);
    RecordExecutedPass(executionTrace, "gaussian_blur", "gaussian_upsample", 4U,
                       4U, rawFullInfo,
                       QtTextPostEffectSurfaceSemantics::RawRgbaData,
                       upsample.executor.c_str(), upsample.executorVersion);
    DumpStage(outputImage, progress, "gaussian-upsample", gpuContext,
              QtTextPostEffectSurfaceSemantics::RawRgbaData);
    return PictureFromImage(outputImage, rasterBounds, recordingBounds);
  }

  if (kind == text::TextPostEffectKind::SoftGlow) {
    const auto contract =
        ResolveQtSoftGlowContract(effect, progress, width, height);
    const char *chain = "soft_glow";
    if (!contract.exactPathSupported) {
      if (std::getenv("VIDEOCUT_TRACE_TEXT_QT_POSTFX") != nullptr) {
        std::fprintf(stderr,
                     "[VIDEOCUT_TEXT_QT_POSTFX_FALLBACK] "
                     "kind=soft-glow reason=%s\n",
                     contract.unsupportedReason.c_str());
      }
      return {};
    }
    const auto rawFullInfo = StageImageInfo(
        width, height, QtTextPostEffectSurfaceSemantics::RawRgbaData);
    const auto glowInfo =
        StageImageInfo(contract.glowWidth, contract.glowHeight,
                       QtTextPostEffectSurfaceSemantics::RawRgbaData);
    const std::array<float, 2> glowSize{
        static_cast<float>(contract.glowWidth),
        static_cast<float>(contract.glowHeight)};

    auto &softGlowMetal = SoftGlowMetalRuntime();
    if (softGlowMetal.runtime) {
      std::vector<std::uint8_t> sourcePixels;
      std::string nativeError;
      if (!gpuContext &&
          !ReadRawRgba8TopLeft(sourceImage, renderer, sourcePixels,
                              nativeError)) {
        std::fprintf(stderr,
                     "[VIDEOCUT_TEXT_QT_SOFT_GLOW_FAILURE] version=%u "
                     "reason=%s fallback=forbidden\n",
                     kQtTextSoftGlowMetalImplementationVersion,
                     nativeError.c_str());
        return {};
      }

      // Keep the native five-pass path observable at the same quantized
      // boundary as the Qt runtime capture. This is gated by DumpStage's
      // explicit diagnostic environment and is inert in product renders.
      DumpStage(sourceImage, progress, "soft-glow-source", gpuContext,
                QtTextPostEffectSurfaceSemantics::RawRgbaData);

      QtTextSoftGlowMetalRequest request;
      request.implementationVersion =
          kQtTextSoftGlowMetalImplementationVersion;
      request.source = {sourcePixels.data(), sourcePixels.size(), width, height,
                        static_cast<std::size_t>(width) * 4U};
      request.glowWidth = contract.glowWidth;
      request.glowHeight = contract.glowHeight;
      request.thresholdType = static_cast<std::int32_t>(contract.thresholdType);
      request.thresholdLow = contract.thresholdLow;
      request.thresholdHigh = contract.thresholdHigh;
      request.thresholdSmooth = contract.thresholdSmooth;
      request.grayScale = contract.grayScale;
      request.sampleCount = contract.sampleCount;
      request.sigmaX = contract.sigmaX;
      request.sigmaY = contract.sigmaY;
      request.stepX = contract.stepX;
      request.stepY = contract.stepY;
      request.exposure = contract.shaderExposure;
      request.glowColor = contract.glowColor;
      request.displayGlow =
          static_cast<std::int32_t>(contract.displayGlow);

      std::vector<std::uint8_t> outputPixels;
      sk_sp<SkImage> outputImage;
      QtTextSoftGlowIntermediateCapture intermediateCapture;
      auto *intermediateCaptureOutput =
          ShouldDumpStage(progress) ? &intermediateCapture : nullptr;
      std::string executor;
      {
        std::lock_guard<std::mutex> lock(softGlowMetal.renderMutex);
        executor = gpuContext ? "qt-lumi-glow-metal-texture-v1"
                              : softGlowMetal.runtime->BackendName();
        if (gpuContext) {
          outputImage = gpuContext->RenderQtTextRawPass(
              sourceImage, width, height,
              [&](void *input, void *output, void *queue, const NativeCommandSubmission &submit, std::string &failure) {
                request.source.nativeTexture = input;
                const QtTextSoftGlowMetalTextureTarget target{output, queue, submit};
                return softGlowMetal.runtime->Render(
                    request, outputPixels, failure, intermediateCaptureOutput,
                    &target);
              }, nativeError);
        } else if (softGlowMetal.runtime->Render(
                       request, outputPixels, nativeError, intermediateCaptureOutput)) {
          outputImage = RawRgba8TopLeftImage(std::move(outputPixels), width, height);
        }
        if (!outputImage) {
          std::fprintf(stderr,
                       "[VIDEOCUT_TEXT_QT_SOFT_GLOW_FAILURE] version=%u "
                       "reason=%s fallback=forbidden\n",
                       kQtTextSoftGlowMetalImplementationVersion,
                       nativeError.c_str());
          return {};
        }
      }
      if (intermediateCaptureOutput != nullptr) {
        const auto dumpCaptured = [&](QtTextSoftGlowCapturedImage &captured,
                                      const std::string_view stage) {
          auto image = RawRgba8TopLeftImage(std::move(captured.pixels),
                                            captured.width, captured.height);
          DumpStage(image, progress, stage, gpuContext,
                    QtTextPostEffectSurfaceSemantics::RawRgbaData);
        };
        dumpCaptured(intermediateCapture.threshold, "soft-glow-threshold");
        dumpCaptured(intermediateCapture.copy, "soft-glow-copy");
        dumpCaptured(intermediateCapture.horizontal, "soft-glow-x");
        dumpCaptured(intermediateCapture.vertical, "soft-glow-y");
      }
      RecordExecutedPass(executionTrace, chain, "soft_glow_threshold", 1U, 5U,
                         rawFullInfo,
                         QtTextPostEffectSurfaceSemantics::RawRgbaData,
                         executor.c_str(),
                         kQtTextSoftGlowMetalImplementationVersion);
      RecordExecutedPass(executionTrace, chain, "soft_glow_copy", 2U, 5U,
                         glowInfo,
                         QtTextPostEffectSurfaceSemantics::RawRgbaData,
                         executor.c_str(),
                         kQtTextSoftGlowMetalImplementationVersion);
      RecordExecutedPass(executionTrace, chain, "soft_glow_x", 3U, 5U,
                         glowInfo,
                         QtTextPostEffectSurfaceSemantics::RawRgbaData,
                         executor.c_str(),
                         kQtTextSoftGlowMetalImplementationVersion);
      RecordExecutedPass(executionTrace, chain, "soft_glow_y", 4U, 5U,
                         glowInfo,
                         QtTextPostEffectSurfaceSemantics::RawRgbaData,
                         executor.c_str(),
                         kQtTextSoftGlowMetalImplementationVersion);
      RecordExecutedPass(executionTrace, chain, "soft_glow_blend", 5U, 5U,
                         rawFullInfo,
                         QtTextPostEffectSurfaceSemantics::RawRgbaData,
                         executor.c_str(),
                         kQtTextSoftGlowMetalImplementationVersion);
      DumpStage(outputImage, progress, "soft-glow-blend", gpuContext,
                QtTextPostEffectSurfaceSemantics::RawRgbaData);
      if (std::getenv("VIDEOCUT_TRACE_TEXT_QT_POSTFX") != nullptr) {
        std::fprintf(stderr,
                     "[VIDEOCUT_TEXT_QT_SOFT_GLOW] version=%u backend=%s "
                     "page=%dx%d glow=%dx%d sample=%.9g exposure=%.9g "
                     "cpu_abi=top-left status=executed\n",
                     kQtTextSoftGlowMetalImplementationVersion,
                     executor.c_str(), width, height, contract.glowWidth,
                     contract.glowHeight, contract.sampleCount,
                     contract.shaderExposure);
      }
      return PictureFromImage(outputImage, rasterBounds, recordingBounds);
    }

    if (std::getenv("VIDEOCUT_TRACE_TEXT_QT_POSTFX") != nullptr) {
      std::fprintf(stderr,
                   "[VIDEOCUT_TEXT_QT_SOFT_GLOW_FALLBACK] native_version=%u "
                   "backend=skia-runtime-effect reason=%s\n",
                   kQtTextSoftGlowMetalImplementationVersion,
                   softGlowMetal.unavailableReason.c_str());
    }
    const auto &thresholdProgram =
        GetTextRuntimeProgram(TextRuntimeShader::PostRadianceThreshold);
    const auto &blitProgram =
        GetTextRuntimeProgram(TextRuntimeShader::PostLinearBlit);
    const auto &gaussianProgram =
        GetTextRuntimeProgram(TextRuntimeShader::PostSoftGlowSeparable);
    const auto &blendProgram =
        GetTextRuntimeProgram(TextRuntimeShader::PostSoftGlowBlend);
    if (!ProgramsAvailable(
            {&thresholdProgram, &blitProgram, &gaussianProgram, &blendProgram},
            "soft-glow")) {
      return {};
    }

    SkRuntimeEffectBuilder thresholdBuilder(thresholdProgram.effect);
    thresholdBuilder.child("inputTexture") = RawLinearShader(sourceImage);
    thresholdBuilder.uniform("textureSize") = fullSize;
    thresholdBuilder.uniform("thresholdType") = contract.thresholdType;
    thresholdBuilder.uniform("thresholdLow") = contract.thresholdLow;
    thresholdBuilder.uniform("thresholdHigh") = contract.thresholdHigh;
    thresholdBuilder.uniform("thresholdSmooth") = contract.thresholdSmooth;
    thresholdBuilder.uniform("grayScale") = contract.grayScale;
    auto thresholdImage =
        renderer.Draw(rawFullInfo, thresholdBuilder.makeShader());
    if (!thresholdImage)
      return {};
    RecordExecutedPass(executionTrace, chain, "soft_glow_threshold", 1U,
                       5U, rawFullInfo,
                       QtTextPostEffectSurfaceSemantics::RawRgbaData);
    DumpStage(thresholdImage, progress, "soft-glow-threshold", gpuContext,
              QtTextPostEffectSurfaceSemantics::RawRgbaData);

    auto copy =
        ExecuteGaussianEngineCopy(thresholdImage, glowInfo, fullSize, glowSize,
                                  renderer, blitProgram, "soft_glow_copy");
    if (copy.status != EngineCopyStatus::Executed || !copy.image)
      return {};
    auto copyImage = std::move(copy.image);
    RecordExecutedPass(executionTrace, chain, "soft_glow_copy", 2U, 5U,
                       glowInfo, QtTextPostEffectSurfaceSemantics::RawRgbaData,
                       copy.executor.c_str(), copy.executorVersion);
    DumpStage(copyImage, progress, "soft-glow-copy", gpuContext,
              QtTextPostEffectSurfaceSemantics::RawRgbaData);

    SkRuntimeEffectBuilder horizontalBuilder(gaussianProgram.effect);
    horizontalBuilder.child("inputTexture") = RawLinearShader(copyImage);
    horizontalBuilder.uniform("textureSize") = glowSize;
    horizontalBuilder.uniform("sampleCount") = contract.sampleCount;
    horizontalBuilder.uniform("sigma") = contract.sigmaX;
    horizontalBuilder.uniform("sampleStep") =
        std::array<float, 2>{contract.stepX, 0.0F};
    horizontalBuilder.uniform("exposure") = 1.0F;
    auto horizontalImage =
        renderer.Draw(glowInfo, horizontalBuilder.makeShader());
    if (!horizontalImage)
      return {};
    RecordExecutedPass(executionTrace, chain, "soft_glow_x", 3U, 5U,
                       glowInfo, QtTextPostEffectSurfaceSemantics::RawRgbaData);
    DumpStage(horizontalImage, progress, "soft-glow-x", gpuContext,
              QtTextPostEffectSurfaceSemantics::RawRgbaData);

    SkRuntimeEffectBuilder verticalBuilder(gaussianProgram.effect);
    verticalBuilder.child("inputTexture") = RawLinearShader(horizontalImage);
    verticalBuilder.uniform("textureSize") = glowSize;
    verticalBuilder.uniform("sampleCount") = contract.sampleCount;
    verticalBuilder.uniform("sigma") = contract.sigmaY;
    verticalBuilder.uniform("sampleStep") =
        std::array<float, 2>{0.0F, contract.stepY};
    verticalBuilder.uniform("exposure") = contract.shaderExposure;
    auto verticalImage = renderer.Draw(glowInfo, verticalBuilder.makeShader());
    if (!verticalImage)
      return {};
    RecordExecutedPass(executionTrace, chain, "soft_glow_y", 4U, 5U,
                       glowInfo, QtTextPostEffectSurfaceSemantics::RawRgbaData);
    DumpStage(verticalImage, progress, "soft-glow-y", gpuContext,
              QtTextPostEffectSurfaceSemantics::RawRgbaData);

    SkRuntimeEffectBuilder blendBuilder(blendProgram.effect);
    blendBuilder.child("glowTexture") = RawLinearShader(verticalImage);
    blendBuilder.child("inputTexture") = RawLinearShader(sourceImage);
    blendBuilder.uniform("textureSize") = fullSize;
    blendBuilder.uniform("glowSize") = glowSize;
    blendBuilder.uniform("exposure") = contract.shaderExposure;
    blendBuilder.uniform("glowColor") = contract.glowColor;
    blendBuilder.uniform("displayGlow") = contract.displayGlow;
    auto outputImage = renderer.Draw(rawFullInfo, blendBuilder.makeShader());
    if (!outputImage)
      return {};
    RecordExecutedPass(executionTrace, chain, "soft_glow_blend", 5U, 5U,
                       rawFullInfo,
                       QtTextPostEffectSurfaceSemantics::RawRgbaData);
    DumpStage(outputImage, progress, "soft-glow-blend", gpuContext,
              QtTextPostEffectSurfaceSemantics::RawRgbaData);
    return PictureFromImage(outputImage, rasterBounds, recordingBounds);
  }

  if (kind == text::TextPostEffectKind::LinearWipe) {
    const auto contract =
        ResolveQtLinearWipeContract(effect, progress, width, height);
    if (!contract.exactPathSupported) {
      if (std::getenv("VIDEOCUT_TRACE_TEXT_QT_POSTFX") != nullptr) {
        std::fprintf(stderr,
                     "[VIDEOCUT_TEXT_QT_POSTFX_FALLBACK] kind=linear-wipe "
                     "reason=%s\n",
                     contract.unsupportedReason.c_str());
      }
      return {};
    }
    const auto &program =
        GetTextRuntimeProgram(TextRuntimeShader::PostLinearWipe);
    if (!ProgramsAvailable({&program}, "linear-wipe"))
      return {};
    SkRuntimeEffectBuilder builder(program.effect);
    builder.child("inputTexture") = RawLinearShader(sourceImage);
    builder.uniform("textureSize") = fullSize;
    builder.uniform("mappedProgress") = contract.mappedProgress;
    builder.uniform("rotationRadians") =
        contract.rotationDegrees * 0.01745329251994329576923690768489F;
    builder.uniform("feather") = contract.feather;
    auto outputImage = renderer.Draw(fullInfo, builder.makeShader());
    if (!outputImage)
      return {};
    RecordExecutedPass(executionTrace, "linear_wipe", "linear_wipe", 1U, 1U,
                       fullInfo,
                       QtTextPostEffectSurfaceSemantics::ColorPremultiplied);
    DumpStage(outputImage, progress, "linear-wipe", gpuContext);
    if (std::getenv("VIDEOCUT_TRACE_TEXT_QT_POSTFX") != nullptr) {
      std::fprintf(stderr,
                   "[VIDEOCUT_TEXT_QT_POSTFX] kind=linear-wipe target=%dx%d "
                   "passes=1 progress=%.9g rotation=%.9g feather=%.9g\n",
                   width, height, contract.mappedProgress,
                   contract.rotationDegrees, contract.feather);
    }
    return PictureFromImage(outputImage, rasterBounds, recordingBounds);
  }
  return {};
}

} // namespace videocut::skia_runtime::internal
