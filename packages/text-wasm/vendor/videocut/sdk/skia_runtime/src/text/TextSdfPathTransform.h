#pragma once

#include "include/core/SkPath.h"
#include "include/core/SkPathBuilder.h"

namespace videocut::skia_runtime::internal {

// Both atlas outlines and mesh silhouettes use separate binary32 multiply
// and add operations, including the identity transform's rounding behavior.
[[nodiscard]] inline SkPath ScaleTranslateTextSdfPathBinary32(
    const SkPath &path, const float scaleX, const float scaleY,
    const float translateX, const float translateY) {
#if defined(__clang__)
#pragma clang fp contract(off)
#endif
  const auto transformPoint = [&](const SkPoint point) {
    float x = point.x() * scaleX;
    float y = point.y() * scaleY;
    x += translateX;
    y += translateY;
    return SkPoint{x, y};
  };
  SkPathBuilder builder;
  SkPath::Iter iterator(path, false);
  SkPoint points[4];
  while (true) {
    switch (iterator.next(points)) {
    case SkPath::kMove_Verb:
      builder.moveTo(transformPoint(points[0]));
      break;
    case SkPath::kLine_Verb:
      builder.lineTo(transformPoint(points[1]));
      break;
    case SkPath::kQuad_Verb:
      builder.quadTo(transformPoint(points[1]), transformPoint(points[2]));
      break;
    case SkPath::kConic_Verb:
      builder.conicTo(transformPoint(points[1]), transformPoint(points[2]),
                      iterator.conicWeight());
      break;
    case SkPath::kCubic_Verb:
      builder.cubicTo(transformPoint(points[1]), transformPoint(points[2]),
                      transformPoint(points[3]));
      break;
    case SkPath::kClose_Verb:
      builder.close();
      break;
    case SkPath::kDone_Verb: {
      auto transformed = builder.detach();
      transformed.setFillType(path.getFillType());
      return transformed;
    }
    }
  }
}

} // namespace videocut::skia_runtime::internal
