#pragma once

#include "text/NativeCommandSubmission.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace videocut::skia_runtime::internal {

inline constexpr std::uint32_t kQtTextLetterImplementationVersion = 1U;
inline constexpr std::size_t kQtTextLetterVertexStride = 108U;
inline constexpr int kQtTextLetterMaxAtlasDimension = 16384;
inline constexpr std::size_t kQtTextLetterMaxAtlasBytes = 256U * 1024U * 1024U;
inline constexpr std::size_t kQtTextLetterMaxVertexCount = 65536U;
inline constexpr std::size_t kQtTextLetterMaxIndexCount = 8U * 1024U * 1024U;
inline constexpr int kQtTextLetterMaxOutputDimension = 16384;
inline constexpr std::size_t kQtTextLetterMaxOutputBytes = 512U * 1024U * 1024U;

// Transport ABI is deliberately Metal-native: row zero is the first row
// copied by a Metal texture-to-buffer blit. No vertical conversion is applied.
struct QtTextLetterRawRg8ImageView final {
  const std::uint8_t *pixels{nullptr};
  std::size_t byteSize{0U};
  int width{0};
  int height{0};
  std::size_t rowBytes{0U};
  // Production atlas: retained RG8 or RGBA8 Metal texture; sampling uses RG.
  // Raw pixels above remain available for explicit captured-packet requests.
  std::shared_ptr<void> nativeTexture;
};

struct QtTextLetterRawBytesView final {
  const std::uint8_t *bytes{nullptr};
  std::size_t byteSize{0U};
};

struct QtTextLetterUniforms final {
  // Column-major, matching Metal float4x4 and Qt's vertex buffer(5).
  float mvp[16]{1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F,
                0.0F, 0.0F, 1.0F, 0.0F, 0.0F, 0.0F, 0.0F, 1.0F};
  float fillColor[4]{1.0F, 1.0F, 1.0F, 1.0F};

  // Qt stores per-letter opacity in aInstanceColor.w. This multiplier is
  // applied to that field before upload; 1.0 preserves captured vertex bytes.
  float opacity{1.0F};
  float offsetInfo[2]{0.0F, 0.0F};
  std::int32_t multiInstanceColor{1};
  float extraSmooth{0.0F};
  float minimumSdf{0.05F};
  float extraWidth{0.0F};
  float textureFlip[4]{0.0F, 0.0F, 0.0F, 0.0F};
};

struct QtTextLetterRenderRequest final {
  std::uint32_t implementationVersion{kQtTextLetterImplementationVersion};
  QtTextLetterRawRg8ImageView atlas;
  QtTextLetterRawBytesView vertices;
  std::size_t vertexCount{0U};
  std::size_t vertexStride{kQtTextLetterVertexStride};
  const std::uint16_t *indices{nullptr};
  std::size_t indexCount{0U};
  QtTextLetterUniforms uniforms;
  int outputWidth{0};
  int outputHeight{0};
};

// Isolated native implementation of Qt TextPro base-fill TEXT_LETTER_MAT.
// outputPixels is tightly packed RGBA8Unorm in Metal transport row order.
class QtTextLetterMetalRuntime {
public:
  virtual ~QtTextLetterMetalRuntime() = default;
  QtTextLetterMetalRuntime(const QtTextLetterMetalRuntime &) = delete;
  QtTextLetterMetalRuntime &
  operator=(const QtTextLetterMetalRuntime &) = delete;

  [[nodiscard]] virtual bool Render(const QtTextLetterRenderRequest &request,
                                    std::vector<std::uint8_t> &outputPixels,
                                    std::string &error) = 0;
  // Retained RGBA8 Metal texture in transport row order. A supplied queue
  // orders production and consumption without a CPU wait. With no queue, or
  // when diagnostic pixels are requested, the same draw completes before return.
  [[nodiscard]] virtual bool RenderTexture(
      const QtTextLetterRenderRequest &request,
      std::shared_ptr<void> &texture, std::string &error,
      std::vector<std::uint8_t> *diagnosticPixels = nullptr,
      void *commandQueue = nullptr,
      const NativeCommandSubmission &submit = {}) = 0;
  [[nodiscard]] virtual const char *BackendName() const noexcept = 0;

protected:
  QtTextLetterMetalRuntime() = default;
};

[[nodiscard]] std::unique_ptr<QtTextLetterMetalRuntime>
CreateQtTextLetterMetalRuntime(std::string &error);

} // namespace videocut::skia_runtime::internal
