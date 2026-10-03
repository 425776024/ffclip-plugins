#pragma once

#include "text/NativeCommandSubmission.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace videocut::skia_runtime::internal {

inline constexpr std::uint32_t kQtTextGaussianMetalImplementationVersion = 1U;

enum class QtTextGaussianAxis : std::uint32_t {
  Horizontal = 1U,
  Vertical = 2U,
};

// Versioned raw boundary for the captured LumiGaussianBlur X/Y Metal passes.
// Rows are logical top-left first and channels are independent RGBA8Unorm
// bytes. No color conversion or alpha premultiplication is permitted here.
struct QtTextGaussianRawRgba8ImageView final {
  const std::uint8_t *pixels{nullptr};
  std::size_t byteSize{0U};
  int width{0};
  int height{0};
  std::size_t rowBytes{0U};
  // Optional borrowed RGBA8 Metal texture in logical bottom-left orientation.
  // When supplied, the matching target and ordered producer queue are required.
  void *nativeTexture{nullptr};
};

struct QtTextGaussianTextureTarget final {
  void *texture{nullptr};
  void *commandQueue{nullptr};
  NativeCommandSubmission submit;
};

struct QtTextGaussianMetalRequest final {
  std::uint32_t implementationVersion{
      kQtTextGaussianMetalImplementationVersion};
  QtTextGaussianRawRgba8ImageView source;
  QtTextGaussianAxis axis{QtTextGaussianAxis::Horizontal};
  float sampleCount{0.0F};
  float sigma{0.0F};
  float step{0.0F};
  float gamma{2.2F};
};

class QtTextGaussianMetalRuntime {
public:
  virtual ~QtTextGaussianMetalRuntime() = default;

  QtTextGaussianMetalRuntime(const QtTextGaussianMetalRuntime &) = delete;
  QtTextGaussianMetalRuntime &
  operator=(const QtTextGaussianMetalRuntime &) = delete;

  [[nodiscard]] virtual bool Render(
      const QtTextGaussianMetalRequest &request,
      std::vector<std::uint8_t> &outputPixels, std::string &error,
      const QtTextGaussianTextureTarget *target = nullptr) = 0;

  [[nodiscard]] virtual const char *BackendName() const noexcept = 0;

protected:
  QtTextGaussianMetalRuntime() = default;
};

// Returns null only when the versioned native backend is unavailable. Once a
// runtime exists, an execution failure must be surfaced rather than silently
// switching to a numerically different implementation.
[[nodiscard]] std::unique_ptr<QtTextGaussianMetalRuntime>
CreateQtTextGaussianMetalRuntime(std::string &error);

} // namespace videocut::skia_runtime::internal
