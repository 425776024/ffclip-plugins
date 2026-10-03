#pragma once

#include "text/NativeCommandSubmission.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace videocut::skia_runtime::internal {

inline constexpr std::uint32_t kQtTextSoftGlowMetalImplementationVersion = 1U;
inline constexpr std::uint32_t kQtTextDeepGlowMetalImplementationVersion = 1U;

// Versioned boundary for the captured LumiSoftGlow five-pass Metal graph.
// Rows are logical top-left first and channels are independent RGBA8Unorm
// bytes. Every native pass materializes an RGBA8Unorm quantization boundary.
struct QtTextSoftGlowRawRgba8ImageView final {
  const std::uint8_t *pixels{nullptr};
  std::size_t byteSize{0U};
  int width{0};
  int height{0};
  std::size_t rowBytes{0U};
  // Borrowed native bottom-left RGBA8 texture, retained by committed commands.
  // When present, pixels/byteSize/rowBytes are not used.
  void *nativeTexture{nullptr};
};

struct QtTextSoftGlowMetalTextureTarget final {
  void *texture{nullptr};
  void *commandQueue{nullptr};
  NativeCommandSubmission submit;
};

struct QtTextSoftGlowMetalRequest final {
  std::uint32_t implementationVersion{
      kQtTextSoftGlowMetalImplementationVersion};
  QtTextSoftGlowRawRgba8ImageView source;
  int glowWidth{0};
  int glowHeight{0};
  std::int32_t thresholdType{0};
  float thresholdLow{0.1F};
  float thresholdHigh{1.0F};
  float thresholdSmooth{0.0F};
  float grayScale{0.0F};
  float sampleCount{0.0F};
  float sigmaX{0.0F};
  float sigmaY{0.0F};
  float stepX{0.0F};
  float stepY{0.0F};
  float exposure{1.0F};
  std::array<float, 3> glowColor{1.0F, 1.0F, 1.0F};
  std::int32_t displayGlow{0};
};

// Versioned request for the captured LumiDeepGlow graph.  The graph owns one
// full-resolution preprocess target, two shared scaled blur targets, and two
// full-resolution composite targets.  Per-layer arrays are bounded by the
// serializer's eight GlowIter components; no source-language or effect-name dispatch is
// performed by the native backend.
struct QtTextDeepGlowMetalRequest final {
  std::uint32_t implementationVersion{
      kQtTextDeepGlowMetalImplementationVersion};
  QtTextSoftGlowRawRgba8ImageView source;
  int blurWidth{0};
  int blurHeight{0};
  int glowIterations{0};
  int postprocessIteration{0}; // one-based serialized composite attachment

  float glowFromAlpha{1.0F};
  std::int32_t chromaticAberration{0};
  float redOffset{0.0F};
  float greenOffset{0.0F};
  float blueOffset{0.0F};
  std::int32_t gammaCorrect{1};
  float authoredGamma{2.2F};

  float blurGamma{2.2F};
  float stepsInt{1.0F};
  std::array<float, 2> aspect{1.0F, 1.0F};
  float rotateDegrees{0.0F};
  float ratio{1.0F};
  std::array<float, 8> sampledSteps{};
  std::array<float, 8> strides{};
  std::array<float, 8> opacities{};
  float exposure{1.0F};
  std::int32_t compositeBlendMode{0};

  std::int32_t postprocessBlendMode{0};
  std::int32_t tint{1};
  std::int32_t tintMode{1};
  std::array<float, 3> tintColor{1.0F, 0.0F, 0.0F};
  float tintMix{1.0F};
  float sourceOpacity{0.0F};
};

struct QtTextSoftGlowCapturedImage final {
  std::vector<std::uint8_t> pixels;
  int width{0};
  int height{0};
};

// Optional diagnostic receipt at the four materialization boundaries before
// the final blend.  It is populated only when explicitly requested by the
// caller; product renders keep every intermediate and output on the GPU.
struct QtTextSoftGlowIntermediateCapture final {
  QtTextSoftGlowCapturedImage threshold;
  QtTextSoftGlowCapturedImage copy;
  QtTextSoftGlowCapturedImage horizontal;
  QtTextSoftGlowCapturedImage vertical;
};

class QtTextSoftGlowMetalRuntime {
public:
  virtual ~QtTextSoftGlowMetalRuntime() = default;

  QtTextSoftGlowMetalRuntime(const QtTextSoftGlowMetalRuntime &) = delete;
  QtTextSoftGlowMetalRuntime &
  operator=(const QtTextSoftGlowMetalRuntime &) = delete;

  [[nodiscard]] virtual bool
  Render(const QtTextSoftGlowMetalRequest &request,
         std::vector<std::uint8_t> &outputPixels, std::string &error,
         QtTextSoftGlowIntermediateCapture *intermediates = nullptr,
         const QtTextSoftGlowMetalTextureTarget *target = nullptr) = 0;

  [[nodiscard]] virtual bool
  RenderDeepGlow(const QtTextDeepGlowMetalRequest &request,
                 std::vector<std::uint8_t> &outputPixels,
                 std::string &error,
                 const QtTextSoftGlowMetalTextureTarget *target = nullptr) = 0;

  [[nodiscard]] virtual const char *BackendName() const noexcept = 0;

protected:
  QtTextSoftGlowMetalRuntime() = default;
};

[[nodiscard]] std::unique_ptr<QtTextSoftGlowMetalRuntime>
CreateQtTextSoftGlowMetalRuntime(std::string &error);

} // namespace videocut::skia_runtime::internal
