#include "text/SkiaGpuContext.h"

namespace videocut::skia_runtime::internal {

namespace {

class UnsupportedSkiaGpuContext final : public SkiaGpuContext {
public:
  PublishedFrame PublishFrame(SkSurface &, std::int64_t, std::uint64_t,
      bool, const std::function<bool()> &) override {
    return {{}, "native Skia GPU frame publication is unavailable"};
  }
  bool WaitForSubmittedWork(std::string &error) override {
    error = "native Skia GPU completion is unavailable";
    return false;
  }
  sk_sp<SkImage> RenderQtTextRawPass(
      const sk_sp<SkImage> &, int, int, const NativeRgba8Pass &,
      std::string &error) override {
    error = "native Qt raw texture rendering is unavailable";
    return {};
  }

  sk_sp<SkImage> RenderQtTextEngineCopy(
      QtTextEngineCopyMetalRuntime &, const sk_sp<SkImage> &, int, int,
      std::string &error) override {
    error = "native Qt EngineCopy texture rendering is unavailable";
    return {};
  }

  sk_sp<SkImage> RenderQtTextLetter(
      QtTextLetterMetalRuntime &, const QtTextLetterRenderRequest &,
      sk_sp<SkColorSpace>, std::string &error,
      std::vector<std::uint8_t> *) override {
    error = "native Qt Letter texture rendering is unavailable";
    return {};
  }

  sk_sp<SkSurface> MakeSurface(const SkImageInfo &, std::string &error) override {
    error = "native Skia GPU context is unavailable";
    return {};
  }

  std::shared_ptr<void> RenderTextSdfAtlas(
      const TextSdfGpuMesh &, const std::vector<SkIRect> &,
      std::string &error, std::vector<std::uint8_t> *) override {
    error = "native text SDF rendering is unavailable";
    return {};
  }

  bool RenderTextSdfMaterial(const TextSdfMaterialGpuRequest &, SkSurface &,
                             std::string &error) override {
    error = "native text SDF material rendering is unavailable";
    return false;
  }

  bool CompositeQtTextRenderGroup(
      const sk_sp<SkImage> &, SkSurface &,
      const QtTextRenderGroupCompositeRequest &,
      std::string &error) override {
    error = "native Qt text RenderGroup composite is unavailable";
    return false;
  }

  bool CompositeQtTextFollower(
      const sk_sp<SkImage> &, SkSurface &,
      const QtTextFollowerCompositeRequest &,
      std::string &error) override {
    error = "native Qt text follower composite is unavailable";
    return false;
  }

  bool RenderTextVatMesh(const sk_sp<SkImage> &,
                         const TextVatMeshGpuRequest &, SkSurface &,
                         std::string &error) override {
    error = "native text VAT mesh rendering is unavailable";
    return false;
  }
};

} // namespace

std::unique_ptr<SkiaGpuContext> CreateSkiaGpuContext(
    std::string &error, std::int32_t, std::uint64_t) {
  error.clear();
  return {};
}

bool NativeTextGpuRequired() noexcept { return false; }

} // namespace videocut::skia_runtime::internal
