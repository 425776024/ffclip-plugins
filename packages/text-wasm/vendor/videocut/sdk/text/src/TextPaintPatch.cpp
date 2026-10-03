#include "videocut/text/TextPaintPatch.h"

namespace videocut::text {

bool TextPaintPatch::Empty() const noexcept {
  return !kind && !color && !stops && !start && !end && !center && !radius &&
         !spread && !texture && !textureFit && !textureScale &&
         !textureRotationDegrees && !textureOffset && !textureFlipX &&
         !textureFlipY && !textureOpacity;
}

void ApplyTextPaintPatch(TextPaint &paint, const TextPaintPatch &patch) {
  if (patch.kind) paint.kind = *patch.kind;
  if (patch.color) paint.color = *patch.color;
  if (patch.stops) paint.stops = *patch.stops;
  if (patch.start) {
    paint.startX = (*patch.start)[0];
    paint.startY = (*patch.start)[1];
  }
  if (patch.end) {
    paint.endX = (*patch.end)[0];
    paint.endY = (*patch.end)[1];
  }
  if (patch.center) {
    paint.centerX = (*patch.center)[0];
    paint.centerY = (*patch.center)[1];
  }
  if (patch.radius) paint.radius = *patch.radius;
  if (patch.spread) paint.spread = *patch.spread;
  if (patch.texture) paint.texture = *patch.texture;
  if (patch.textureFit) paint.textureFit = *patch.textureFit;
  if (patch.textureScale) paint.textureScale = *patch.textureScale;
  if (patch.textureRotationDegrees) {
    paint.textureRotationDegrees = *patch.textureRotationDegrees;
  }
  if (patch.textureOffset) {
    paint.textureOffsetX = (*patch.textureOffset)[0];
    paint.textureOffsetY = (*patch.textureOffset)[1];
  }
  if (patch.textureFlipX) paint.textureFlipX = *patch.textureFlipX;
  if (patch.textureFlipY) paint.textureFlipY = *patch.textureFlipY;
  if (patch.textureOpacity) paint.textureOpacity = *patch.textureOpacity;
}

} // namespace videocut::text
