#pragma once

#include <cstdint>
#include <string>
#include <vector>

namespace videocut::skia_runtime {

struct RgbaAnimationFrame final {
  std::vector<std::uint8_t> rgba;
  std::uint32_t durationMs{0U};
};

/// Encodes one tightly packed, straight-alpha RGBA frame without animation
/// frame minimization. This is used by native visual-fidelity evidence so a
/// comparison never observes WebP sub-frame disposal or coalescing semantics.
[[nodiscard]] bool EncodeRgbaPng(std::uint32_t width, std::uint32_t height,
                                 const std::vector<std::uint8_t> &rgba,
                                 std::vector<std::uint8_t> &output,
                                 std::string &error);

/// Encodes one bounded, transparent, infinitely looping RGBA animation using
/// the libwebp artifact pinned into the product Skia runtime.
[[nodiscard]] bool
EncodeAnimatedWebp(std::uint32_t width, std::uint32_t height,
                   const std::vector<RgbaAnimationFrame> &frames,
                   std::vector<std::uint8_t> &output, std::string &error);

} // namespace videocut::skia_runtime
