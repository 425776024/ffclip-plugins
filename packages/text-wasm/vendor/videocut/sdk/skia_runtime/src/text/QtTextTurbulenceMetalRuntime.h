#pragma once

#include "text/NativeCommandSubmission.h"

#include <cstddef>
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace videocut::skia_runtime::internal {

// Internal material ABI, shared by the native shader and the graph executor.
// Matrix rows map logical top-left pixels to/from the authored sprite UV.
struct alignas(16) QtTextMaterialTurbulenceUniforms final {
  std::array<float, 2> textureSize{};
  std::array<float, 2> viewportSize{};
  std::array<float, 2> scale{};
  std::array<float, 2> offset{};
  float rotation{0};
  float evolution{0};
  float complexity{1};
  float brightness{0};
  float contrast{0};
  float range{0};
  float subImpact{0};
  float subScale{100};
  float subRotation{0};
  float spriteMode{0};
  float cycle{0};
  float padding0{0};
  std::array<float, 2> subOffset{};
  std::array<float, 2> padding1{};
  std::array<float, 4> spriteToOutput0{};
  std::array<float, 4> spriteToOutput1{};
  std::array<float, 4> outputToSprite0{};
  std::array<float, 4> outputToSprite1{};
};
static_assert(sizeof(QtTextMaterialTurbulenceUniforms) == 160U);
static_assert(offsetof(QtTextMaterialTurbulenceUniforms, subOffset) == 80U);
static_assert(offsetof(QtTextMaterialTurbulenceUniforms, spriteToOutput0) == 96U);

// Borrows bottom-left RGBA8 textures and the Ganesh command queue. Commits one
// pass without waiting or transferring pixels to the CPU.
[[nodiscard]] bool RenderQtTextMaterialTurbulence(
    const QtTextMaterialTurbulenceUniforms &uniforms, void *sourceTexture,
    void *outputTexture, void *commandQueue, std::string &error,
    const NativeCommandSubmission &submit = {});

inline constexpr std::uint32_t kQtTextTurbulenceMetalImplementationVersion =
    1U;

struct QtTextTurbulenceRawRgba8ImageView final {
  const std::uint8_t *pixels{nullptr};
  std::size_t byteSize{0U};
  int width{0};
  int height{0};
  std::size_t rowBytes{0U};
  void *nativeTexture{nullptr};
};

// Borrowed bottom-left RGBA8 textures and the ordered producer queue. Normal
// GPU execution returns no CPU buffers; diagnostics may request only noise.
struct QtTextTurbulenceTextureTarget final {
  void *texture{nullptr};
  void *commandQueue{nullptr};
  NativeCommandSubmission submit;
  bool captureNoisePixels{false};
};

// Closed v1 ABI for the captured LumiTurbulenceDisplacement noise and
// displacement passes. CPU rows are top-left first; both outputs are tightly
// packed independent RGBA8Unorm bytes.
struct QtTextTurbulenceMetalRequest final {
  std::uint32_t implementationVersion{
      kQtTextTurbulenceMetalImplementationVersion};
  QtTextTurbulenceRawRgba8ImageView source;
  int noiseWidth{0};
  int noiseHeight{0};
  float cycle{300.0F};
  float offsetX{0.0F};
  float offsetY{0.0F};
  float quantity{2.0F};
  float complexity{2.0F};
  float evolution{0.0F};
  float type{0.0F};
  float contrast{0.0F};
  float range{0.3F};
  float motionTileType{0.0F};
};

class QtTextTurbulenceMetalRuntime {
public:
  virtual ~QtTextTurbulenceMetalRuntime() = default;

  QtTextTurbulenceMetalRuntime(const QtTextTurbulenceMetalRuntime &) = delete;
  QtTextTurbulenceMetalRuntime &
  operator=(const QtTextTurbulenceMetalRuntime &) = delete;

  [[nodiscard]] virtual bool Render(
      const QtTextTurbulenceMetalRequest &request,
      std::vector<std::uint8_t> &noisePixels,
      std::vector<std::uint8_t> &outputPixels, std::string &error,
      const QtTextTurbulenceTextureTarget *target = nullptr) = 0;

  [[nodiscard]] virtual const char *BackendName() const noexcept = 0;

protected:
  QtTextTurbulenceMetalRuntime() = default;
};

[[nodiscard]] std::unique_ptr<QtTextTurbulenceMetalRuntime>
CreateQtTextTurbulenceMetalRuntime(std::string &error);

} // namespace videocut::skia_runtime::internal
