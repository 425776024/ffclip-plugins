#pragma once

#include "internal/skia/SkiaHeaders.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>

namespace videocut::skia_runtime::internal {

inline constexpr std::size_t kTextGradientLutSampleCount = 256U;
inline constexpr std::size_t kTextGradientLutChannelCount = 4U;

struct TextGradientLutStop final {
  float offset{0.0F};
  SkColor4f color{0.0F, 0.0F, 0.0F, 1.0F};
};

/// Immutable CPU staging data plus the real RGBA8 image consumed by Ganesh.
/// The image is intentionally unpremultiplied: the raw image child performs
/// filtering in the same RGBA domain as a Qt RGBA8Unorm texture, and the
/// runtime shader premultiplies only the filtered result.
struct TextGradientLut256 final {
  std::array<std::uint8_t,
             kTextGradientLutSampleCount * kTextGradientLutChannelCount>
      rgba{};
  sk_sp<SkImage> image;
};

struct TextGradientPaintFrame final {
  SkRect bounds{SkRect::MakeEmpty()};
  // TextPro character gradients normalize against the glyph SDF quad and
  // then scale that coordinate around 0.5 before evaluating the gradient.
  // A value greater than one is equivalently represented by expanding the
  // authored gradient geometry around the paint-frame centre.
  float coordinateScale{1.0F};

  [[nodiscard]] SkPoint Resolve(const SkPoint normalizedPoint) const noexcept;
};

/// Builds TextPro's 256x1 intermediate gradient texture. Authored channels are
/// clamped and truncated to RGBA8 first. Every i/255 sample then folds each
/// adjacent stop into the preceding integer result using two float32
/// multiplies, one add, and signed truncation. Equal offsets retain authored
/// order.
[[nodiscard]] std::optional<TextGradientLut256>
BuildTextGradientLut256(std::span<const TextGradientLutStop> authoredStops,
                        std::string &error);

/// Coordinates are ordinary paint/local coordinates. t=0 and t=1 address the
/// centers of LUT texels 0 and 255 respectively; values outside that interval
/// clamp to the endpoint texels.
[[nodiscard]] sk_sp<SkShader>
MakeTextLinearGradientLutShader(const TextGradientLut256 &lut, SkPoint start,
                                SkPoint end, std::string &error);

/// Convenience overload for normalized points in a paint-coordinate frame.
[[nodiscard]] sk_sp<SkShader> MakeTextLinearGradientLutShader(
    const TextGradientLut256 &lut, const TextGradientPaintFrame &frame,
    SkPoint normalizedStart, SkPoint normalizedEnd, std::string &error);

/// Generic centered radial mapping: t=distance(point, center)/radius. This is
/// intentionally isolated from Qt focal/radius-mode policy until that policy
/// is independently evidenced; it shares the exact LUT sampling contract.
[[nodiscard]] sk_sp<SkShader>
MakeTextRadialGradientLutShader(const TextGradientLut256 &lut, SkPoint center,
                                float radius, std::string &error);

/// Convenience overload matching the current text-paint convention where a
/// normalized radius is relative to max(frame.width, frame.height).
[[nodiscard]] sk_sp<SkShader> MakeTextRadialGradientLutShader(
    const TextGradientLut256 &lut, const TextGradientPaintFrame &frame,
    SkPoint normalizedCenter, float normalizedRadius, std::string &error);

} // namespace videocut::skia_runtime::internal
