#pragma once

#include "text/NativeCommandSubmission.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace videocut::skia_runtime::internal {

inline constexpr const char *kQtTextShakeImplementationId =
    "videocut.text-post.qt-lumi-s-shake";
inline constexpr std::uint32_t kQtTextShakeImplementationVersion = 1U;
inline constexpr const char *kQtTextShakeSourceContractDigest =
    "sha256-tree-v1:"
    "0e4b70f2e75db73ba565f85209f6bc6ebeeea834f7ef5ca2a8b952a1ee235fce";

struct QtTextShakeRawRgba8ImageView final {
  const std::uint8_t *pixels{nullptr};
  std::size_t byteSize{0U};
  int width{0};
  int height{0};
  std::size_t rowBytes{0U};
  void *nativeTexture{nullptr};
};

/// Closed non-motion-blur LumiSShake draw. Matrices use the same column-major
/// ABI as AmazingEngine's three float4x4 vertex uniforms.
struct QtTextShakeRenderRequest final {
  std::string implementationId{kQtTextShakeImplementationId};
  std::uint32_t implementationVersion{kQtTextShakeImplementationVersion};
  std::string sourceContractDigest{kQtTextShakeSourceContractDigest};
  QtTextShakeRawRgba8ImageView source;
  std::array<std::array<float, 16>, 3> uvMatrices{};
  int fillModeX{0};
  int fillModeY{0};
};

// Bottom-left RGBA8 textures on the caller's ordered Ganesh queue.
struct QtTextShakeTextureTarget final {
  void *texture{nullptr};
  void *commandQueue{nullptr};
  NativeCommandSubmission submit;
};

class QtTextShakeMetalRuntime {
public:
  virtual ~QtTextShakeMetalRuntime() = default;

  QtTextShakeMetalRuntime(const QtTextShakeMetalRuntime &) = delete;
  QtTextShakeMetalRuntime &operator=(const QtTextShakeMetalRuntime &) = delete;

  [[nodiscard]] virtual bool Render(
      const QtTextShakeRenderRequest &request,
      std::vector<std::uint8_t> &outputPixels, std::string &error,
      const QtTextShakeTextureTarget *target = nullptr) = 0;

  [[nodiscard]] virtual const char *BackendName() const noexcept = 0;

protected:
  QtTextShakeMetalRuntime() = default;
};

[[nodiscard]] std::unique_ptr<QtTextShakeMetalRuntime>
CreateQtTextShakeMetalRuntime(std::string &error);

} // namespace videocut::skia_runtime::internal
