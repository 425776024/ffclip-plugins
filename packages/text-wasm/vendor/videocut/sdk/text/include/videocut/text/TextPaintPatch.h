#pragma once

#include "videocut/text/RichTextDocument.h"

#include <array>
#include <optional>
#include <vector>

namespace videocut::text {

/// Field-level authored edit over an existing TextPaint. Absent fields are
/// intentionally retained byte-for-byte; texture source identity is replaced
/// atomically as one TextureReference value.
struct TextPaintPatch final {
  std::optional<PaintKind> kind;
  std::optional<Color> color;
  std::optional<std::vector<GradientStop>> stops;
  std::optional<std::array<float, 2U>> start;
  std::optional<std::array<float, 2U>> end;
  std::optional<std::array<float, 2U>> center;
  std::optional<float> radius;
  std::optional<PaintSpread> spread;
  std::optional<TextureReference> texture;
  std::optional<TextureFit> textureFit;
  std::optional<float> textureScale;
  std::optional<float> textureRotationDegrees;
  std::optional<std::array<float, 2U>> textureOffset;
  std::optional<bool> textureFlipX;
  std::optional<bool> textureFlipY;
  std::optional<float> textureOpacity;

  [[nodiscard]] bool Empty() const noexcept;
};

void ApplyTextPaintPatch(TextPaint &paint, const TextPaintPatch &patch);

} // namespace videocut::text
