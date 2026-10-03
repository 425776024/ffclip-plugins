#pragma once

#include "text/NativeCommandSubmission.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace videocut::skia_runtime::internal {

inline constexpr const char *kQtTextRadialBlurImplementationId =
    "videocut.text-post.qt-lumi-radial-blur";
inline constexpr std::uint32_t kQtTextRadialBlurImplementationVersion = 1U;
inline constexpr const char *kQtTextRadialBlurSourceContractDigest =
    "sha256-tree-v1:"
    "23808e876b3385048ec8cb6e20fa7469183176d22b4190dda91d42039a868960";

struct QtTextRadialBlurRawRgba8ImageView final {
  const std::uint8_t *pixels{nullptr};
  std::size_t byteSize{0U};
  int width{0};
  int height{0};
  std::size_t rowBytes{0U};
  void *nativeTexture{nullptr};
};

/// Borrowed bottom-left RGBA8 textures on the caller's ordered Metal queue.
struct QtTextRadialBlurTextureTarget final {
  void *texture{nullptr};
  void *commandQueue{nullptr};
  NativeCommandSubmission submit;
};

/// One source-closed LumiRadialBlur RGBA8 pass. Values are already mapped by
/// the authored RadialBlur contract (for example amount/200 and quality*100).
struct QtTextRadialBlurRenderRequest final {
  std::string implementationId{kQtTextRadialBlurImplementationId};
  std::uint32_t implementationVersion{kQtTextRadialBlurImplementationVersion};
  std::string sourceContractDigest{kQtTextRadialBlurSourceContractDigest};
  QtTextRadialBlurRawRgba8ImageView source;
  float intensity{0.0F};
  int blurType{0};
  std::array<float, 2> center{0.5F, 0.5F};
  float quality{20.0F};
  float weightDecay{1.0F};
  float dither{0.0F};
  int borderType{0};
  int blurAlpha{1};
  int inverseGammaCorrection{0};
  float gamma{2.2F};
  float lightIntensity{1.0F};
  float lightTransferMode{0.0F};
};

class QtTextRadialBlurMetalRuntime {
public:
  virtual ~QtTextRadialBlurMetalRuntime() = default;

  QtTextRadialBlurMetalRuntime(const QtTextRadialBlurMetalRuntime &) = delete;
  QtTextRadialBlurMetalRuntime &
  operator=(const QtTextRadialBlurMetalRuntime &) = delete;

  [[nodiscard]] virtual bool
  Render(const QtTextRadialBlurRenderRequest &request,
         std::vector<std::uint8_t> &outputPixels, std::string &error,
         const QtTextRadialBlurTextureTarget *target = nullptr) = 0;

  [[nodiscard]] virtual const char *BackendName() const noexcept = 0;

protected:
  QtTextRadialBlurMetalRuntime() = default;
};

[[nodiscard]] std::unique_ptr<QtTextRadialBlurMetalRuntime>
CreateQtTextRadialBlurMetalRuntime(std::string &error);

} // namespace videocut::skia_runtime::internal
