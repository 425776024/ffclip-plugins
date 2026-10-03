#pragma once

#include "resources/QtPackedAlphaYuvMergeContract.h"
#include "videocut/frame/VideoFrame.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace videocut::skia_runtime::internal {

inline constexpr std::size_t kQtPackedAlphaYuvMergeDefaultOutputByteBudget =
    256U * 1024U * 1024U;
inline constexpr std::size_t kQtPackedAlphaYuvMergeDefaultWorkingSetByteBudget =
    768U * 1024U * 1024U;

struct QtPackedAlphaYuvMergeLimits final {
  std::uint32_t implementationVersion{
      kQtPackedAlphaYuvMergeImplementationVersion};
  std::size_t maximumOutputBytes{kQtPackedAlphaYuvMergeDefaultOutputByteBudget};
  std::size_t maximumWorkingSetBytes{
      kQtPackedAlphaYuvMergeDefaultWorkingSetByteBudget};
};

// Public CPU result ABI. pixels contains tightly packed RGBA8Unorm rows in
// top-left-first order. RGB is already associated with alpha by the source
// packed video and must not be multiplied by alpha again.
struct QtPackedAlphaYuvMergeOutput final {
  std::vector<std::uint8_t> pixels;
  std::uint32_t width{0U};
  std::uint32_t height{0U};

  void Clear() noexcept {
    pixels.clear();
    width = 0U;
    height = 0U;
  }
};

// Versioned native implementation of the captured Qt/AmazingEngine
// VideoAnimSeq packed-alpha pass. V1 accepts only CPU-mappable, unrotated,
// even-sized Yuv420p/U8 frames with three scalar planes. The Y plane stores
// alpha on the left and associated RGB on the right, so its stored width is
// exactly twice the returned width.
class QtPackedAlphaYuvMergeRuntime {
public:
  virtual ~QtPackedAlphaYuvMergeRuntime() = default;

  QtPackedAlphaYuvMergeRuntime(const QtPackedAlphaYuvMergeRuntime &) = delete;
  QtPackedAlphaYuvMergeRuntime &
  operator=(const QtPackedAlphaYuvMergeRuntime &) = delete;

  [[nodiscard]] virtual bool Render(const frame::VideoFrame &source,
                                    const QtPackedAlphaYuvMergeLimits &limits,
                                    QtPackedAlphaYuvMergeOutput &output,
                                    std::string &error) = 0;

  [[nodiscard]] virtual const char *BackendName() const noexcept = 0;

protected:
  QtPackedAlphaYuvMergeRuntime() = default;
};

// Returns null when this build cannot provide the exact Metal v1 backend.
// There is deliberately no CPU numerical fallback under this factory.
[[nodiscard]] std::unique_ptr<QtPackedAlphaYuvMergeRuntime>
CreateQtPackedAlphaYuvMergeRuntime(std::string &error);

} // namespace videocut::skia_runtime::internal
