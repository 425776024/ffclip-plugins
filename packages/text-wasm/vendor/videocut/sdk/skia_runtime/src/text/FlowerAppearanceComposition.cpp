#include "text/FlowerAppearanceComposition.h"

#include <algorithm>
#include <cmath>
#include <type_traits>

namespace videocut::skia_runtime::internal {

void TextVisualOutsets::Include(const TextVisualOutsets &other) noexcept {
  left = std::max(left, other.left);
  top = std::max(top, other.top);
  right = std::max(right, other.right);
  bottom = std::max(bottom, other.bottom);
}

const text::TextMaterial &
ResolveTextMaterial(const text::TextMaterialBinding &binding) noexcept {
  return std::visit(
      [](const auto &value) -> const text::TextMaterial & {
        using Value = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Value, text::LiteralTextMaterial>)
          return value.material;
        else
          return value.fallback;
      },
      binding);
}

std::vector<OrderedGlyphMaterialLayer>
OrderGlyphMaterialLayers(const text::TextGlyphMaterialStack &stack) {
  std::vector<OrderedGlyphMaterialLayer> result;
  result.reserve(stack.layers.size());
  for (std::size_t index = 0U; index < stack.layers.size(); ++index) {
    const auto &layer = stack.layers[index];
    std::visit(
        [&](const auto &value) {
          result.push_back(
              {&layer, value.layerId, value.zOrder, value.blend, index});
        },
        layer);
  }
  std::stable_sort(result.begin(), result.end(),
                   [](const auto &left, const auto &right) {
                     if (left.zOrder != right.zOrder)
                       return left.zOrder < right.zOrder;
                     return left.authoredIndex < right.authoredIndex;
                   });
  return result;
}

TextVisualOutsets ResolveGlyphMaterialOutsets(
    const text::TextGlyphMaterialStack &stack) noexcept {
  TextVisualOutsets result;
  for (const auto &layer : stack.layers) {
    std::visit(
        [&](const auto &value) {
          using Value = std::decay_t<decltype(value)>;
          if constexpr (std::is_same_v<Value, text::TextFillLayer>) {
            result.left = std::max(result.left, -value.offsetX);
            result.right = std::max(result.right, value.offsetX);
            result.top = std::max(result.top, -value.offsetY);
            result.bottom = std::max(result.bottom, value.offsetY);
          } else if constexpr (std::is_same_v<Value, text::TextStrokeLayer>) {
            const float extent = std::max(0.0F, value.width) * 0.5F +
                                 std::max(0.0F, value.spread) +
                                 std::max(0.0F, value.blurRadius) * 2.0F;
            result.left = std::max(result.left, extent - value.offsetX);
            result.right = std::max(result.right, extent + value.offsetX);
            result.top = std::max(result.top, extent - value.offsetY);
            result.bottom = std::max(result.bottom, extent + value.offsetY);
          } else if constexpr (std::is_same_v<Value,
                                              text::TextShadowLayer>) {
            const float radians = value.thicknessAngleDegrees *
                                  3.14159265358979323846F / 180.0F;
            const float projectionX =
                std::cos(radians) * value.thicknessDistance;
            // Text material coordinates are Y-down. Keep the signed Y
            // convention identical for bounds and painting.
            const float projectionY =
                -std::sin(radians) * value.thicknessDistance;
            const float extent = std::max(0.0F, value.spread) +
                                 std::max(0.0F, value.blurRadius) * 2.0F;
            const float offsetX = value.offsetX + projectionX;
            const float offsetY = value.offsetY + projectionY;
            result.left = std::max(result.left, extent - offsetX);
            result.right = std::max(result.right, extent + offsetX);
            result.top = std::max(result.top, extent - offsetY);
            result.bottom = std::max(result.bottom, extent + offsetY);
            for (const auto &stroke : value.strokes) {
              const float strokeExtent =
                  std::max(0.0F, stroke.width + 2.0F * value.spread) * 0.5F +
                  std::max(0.0F, stroke.spread) +
                  std::max(0.0F, stroke.blurRadius) * 2.0F;
              const float strokeOffsetX = offsetX + stroke.offsetX;
              const float strokeOffsetY = offsetY + stroke.offsetY;
              result.left =
                  std::max(result.left, strokeExtent - strokeOffsetX);
              result.right =
                  std::max(result.right, strokeExtent + strokeOffsetX);
              result.top =
                  std::max(result.top, strokeExtent - strokeOffsetY);
              result.bottom =
                  std::max(result.bottom, strokeExtent + strokeOffsetY);
            }
          } else if constexpr (std::is_same_v<Value, text::TextGlowLayer>) {
            const float extent = std::max(0.0F, value.radius) * 2.0F +
                                 std::max(0.0F, value.spread);
            const float offsetX = value.directionX * value.radius;
            const float offsetY = value.directionY * value.radius;
            result.left = std::max(result.left, extent - offsetX);
            result.right = std::max(result.right, extent + offsetX);
            result.top = std::max(result.top, extent - offsetY);
            result.bottom = std::max(result.bottom, extent + offsetY);
          }
        },
        layer);
  }
  return result;
}

TextVisualOutsets ResolveBackdropSourceOutsets(
    const text::TextBackdropSource &source) noexcept {
  TextVisualOutsets result;
  const auto *rounded = std::get_if<text::RoundedRectBackdrop>(&source);
  if (!rounded)
    return result;

  text::TextGlyphMaterialStack strokes;
  strokes.layers.reserve(rounded->strokes.size());
  for (const auto &stroke : rounded->strokes)
    strokes.layers.emplace_back(stroke);
  result.Include(ResolveGlyphMaterialOutsets(strokes));

  const float tailLength = std::max(0.0F, rounded->tail.length);
  switch (rounded->tail.edge) {
  case text::BubbleTailEdge::Top:
    result.top = std::max(result.top, tailLength);
    break;
  case text::BubbleTailEdge::Bottom:
    result.bottom = std::max(result.bottom, tailLength);
    break;
  case text::BubbleTailEdge::Left:
    result.left = std::max(result.left, tailLength);
    break;
  case text::BubbleTailEdge::Right:
    result.right = std::max(result.right, tailLength);
    break;
  case text::BubbleTailEdge::None:
    break;
  }
  return result;
}

TextVisualOutsets ResolvePostEffectOutsets(
    const std::vector<text::TextEffectPostEffectNode> &nodes) noexcept {
  TextVisualOutsets result;
  for (const auto &node : nodes) {
    const float extent = std::max(0.0F, node.paddingPx);
    result.left = std::max(result.left, extent);
    result.top = std::max(result.top, extent);
    result.right = std::max(result.right, extent);
    result.bottom = std::max(result.bottom, extent);
  }
  return result;
}

} // namespace videocut::skia_runtime::internal
