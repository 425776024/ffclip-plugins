#pragma once

#include "text/NativeCommandSubmission.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace videocut::skia_runtime::internal {

inline constexpr std::uint32_t kQtTextEngineCopyImplementationVersion = 1U;

// Public CPU ABI for Qt EngineCopy. Rows are always logical top-left first and
// channels are four independent RGBA8Unorm bytes. In particular, the runtime
// must not premultiply, unpremultiply, color-convert, or clamp RGB against A.
struct QtTextEngineCopyRawRgba8ImageView final {
  const std::uint8_t *pixels{nullptr};
  std::size_t byteSize{0U};
  int width{0};
  int height{0};
  std::size_t rowBytes{0U};
};

struct QtTextEngineCopyRequest final {
  std::uint32_t implementationVersion{kQtTextEngineCopyImplementationVersion};
  QtTextEngineCopyRawRgba8ImageView source;
  int outputWidth{0};
  int outputHeight{0};
};

// Native implementation of the captured Qt/AmazingEngine EngineCopy pass.
// The Apple backend owns an RGBA8Unorm Metal pipeline whose fragment ABI is
// texture2d<float>.sample with normalized linear clamp sampling. outputPixels
// is tightly packed and follows the same top-left CPU ABI as source.
class QtTextEngineCopyMetalRuntime {
public:
  virtual ~QtTextEngineCopyMetalRuntime() = default;

  QtTextEngineCopyMetalRuntime(const QtTextEngineCopyMetalRuntime &) = delete;
  QtTextEngineCopyMetalRuntime &
  operator=(const QtTextEngineCopyMetalRuntime &) = delete;

  [[nodiscard]] virtual bool Render(const QtTextEngineCopyRequest &request,
                                    std::vector<std::uint8_t> &outputPixels,
                                    std::string &error) = 0;

  // RGBA8 Metal textures use the captured bottom-left transport origin.
  // The caller submits producers and consumers on this same queue; encoding
  // retains resources until completion without CPU staging or synchronization.
  [[nodiscard]] virtual bool RenderTextures(void *sourceTexture,
                                           void *outputTexture,
                                           void *commandQueue,
                                           std::string &error,
                                           const NativeCommandSubmission &submit = {}) = 0;

  [[nodiscard]] virtual const char *BackendName() const noexcept = 0;

protected:
  QtTextEngineCopyMetalRuntime() = default;
};

// Returns null only when the versioned native backend is explicitly
// unavailable (non-Apple, non-Metal build, no Metal product device, or native
// pipeline creation failure). A runtime returned successfully treats a later
// Render failure as an execution error, not as permission to silently switch
// numerical implementations.
[[nodiscard]] std::unique_ptr<QtTextEngineCopyMetalRuntime>
CreateQtTextEngineCopyMetalRuntime(std::string &error);

} // namespace videocut::skia_runtime::internal
