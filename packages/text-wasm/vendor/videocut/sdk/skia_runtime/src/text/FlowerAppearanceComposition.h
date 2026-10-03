#pragma once

#include "videocut/text/TextEffectFramePlan.h"

#include <cstddef>
#include <cstdint>
#include <string_view>
#include <vector>

namespace videocut::skia_runtime::internal {

/// Renderer-side view of one typed glyph layer. The authored variant remains
/// the only material representation; this record adds only resolved ordering.
struct OrderedGlyphMaterialLayer final {
  const text::TextGlyphMaterialLayer *layer{nullptr};
  std::string_view layerId;
  std::int32_t zOrder{0};
  text::TextBlendMode blend{text::TextBlendMode::SourceOver};
  std::size_t authoredIndex{0U};
};

struct TextVisualOutsets final {
  float left{0.0F};
  float top{0.0F};
  float right{0.0F};
  float bottom{0.0F};

  void Include(const TextVisualOutsets &other) noexcept;
};

/// Returns the literal material or the editable slot's already-resolved
/// fallback. Semantic replacement is completed before the render document is
/// installed; the renderer never mutates an editable slot.
[[nodiscard]] const text::TextMaterial &
ResolveTextMaterial(const text::TextMaterialBinding &binding) noexcept;

[[nodiscard]] std::vector<OrderedGlyphMaterialLayer>
OrderGlyphMaterialLayers(const text::TextGlyphMaterialStack &stack);

[[nodiscard]] TextVisualOutsets
ResolveGlyphMaterialOutsets(const text::TextGlyphMaterialStack &stack) noexcept;

[[nodiscard]] TextVisualOutsets
ResolveBackdropSourceOutsets(const text::TextBackdropSource &source) noexcept;

[[nodiscard]] TextVisualOutsets ResolvePostEffectOutsets(
    const std::vector<text::TextEffectPostEffectNode> &nodes) noexcept;

} // namespace videocut::skia_runtime::internal
