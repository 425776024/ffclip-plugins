#pragma once

#include "text/NativeCommandSubmission.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace videocut::skia_runtime::internal {

// Digest of the complete Qt AmazingFeature source tree that owns the
// LumiDistortChroma native graph and its three fixed shaders.
inline constexpr const char *kQtTextDistortChromaSourceContractDigest =
    "sha256-tree-v1:"
    "b4d3088f21e9d63a7a61ab8c90c352111fd0757a3dbec1ffd9ae61f7178e17bc";
inline constexpr std::uint32_t kQtTextDistortChromaExecutorVersion = 1U;

struct QtTextDistortChromaRawRgba8ImageView final {
  const std::uint8_t *pixels{nullptr};
  std::size_t byteSize{0U};
  int width{0};
  int height{0};
  std::size_t rowBytes{0U};
  // Borrowed bottom-left RGBA8 texture for the explicit native target path.
  void *nativeTexture{nullptr};
};

/// Values after the native DistortChroma contract has mapped authored parameters. The
/// input and every returned image use tightly packed, top-left-first RGBA8.
struct QtTextDistortChromaRenderRequest final {
  QtTextDistortChromaRawRgba8ImageView source;
  int lensWidth{0};
  int lensHeight{0};
  int blurSteps{1};
  float blurStrideFirst{0.5F};
  float blurStrideSecond{2.0F};
  float blurAngleDegrees{0.0F};
  float blurPerpendicularAngleDegrees{90.0F};
  float rotateWarpDirectionDegrees{0.0F};
  float amountRelX{1.0F};
  float amountRelY{1.0F};
  int wrapModeX{0};
  int wrapModeY{0};
  int chromaSteps{8};
  float warpRed{0.5F};
  float warpBlue{0.5F};
  float warpAmount{0.0F};
  std::array<float, 3> color1{1.0F, 0.0F, 0.0F};
  std::array<float, 3> color2{0.0F, 1.0F, 0.0F};
  std::array<float, 3> color3{0.0F, 0.0F, 1.0F};
  float colorMix{1.0F};
  bool captureIntermediates{false};
};

struct QtTextDistortChromaRenderResult final {
  std::vector<std::uint8_t> lensPixels;
  std::vector<std::uint8_t> blurX1Pixels;
  std::vector<std::uint8_t> blurY1Pixels;
  std::vector<std::uint8_t> blurX2Pixels;
  std::vector<std::uint8_t> blurY2Pixels;
  std::vector<std::uint8_t> outputPixels;
};

class QtTextDistortChromaMetalRuntime {
public:
  virtual ~QtTextDistortChromaMetalRuntime() = default;

  QtTextDistortChromaMetalRuntime(const QtTextDistortChromaMetalRuntime &) =
      delete;
  QtTextDistortChromaMetalRuntime &
  operator=(const QtTextDistortChromaMetalRuntime &) = delete;

  [[nodiscard]] virtual bool
  Render(const QtTextDistortChromaRenderRequest &request,
         QtTextDistortChromaRenderResult &result, std::string &error,
         const NativeRgba8TextureTarget *target = nullptr) = 0;

  [[nodiscard]] virtual const char *BackendName() const noexcept = 0;

protected:
  QtTextDistortChromaMetalRuntime() = default;
};

[[nodiscard]] std::unique_ptr<QtTextDistortChromaMetalRuntime>
CreateQtTextDistortChromaMetalRuntime(std::string &error);

} // namespace videocut::skia_runtime::internal
