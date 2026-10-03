#include "text/TextRenderPipeline.h"

namespace videocut::skia_runtime::internal::text_lane {


std::optional<SkRect> MapEffectBounds(
    const SkRect &source, const text::TextEffectMatrix4x4 &transform) {
  const auto matrix = SkM44::ColMajor(transform.columnMajor.data());
  float minimumX = std::numeric_limits<float>::infinity();
  float minimumY = std::numeric_limits<float>::infinity();
  float maximumX = -std::numeric_limits<float>::infinity();
  float maximumY = -std::numeric_limits<float>::infinity();
  const std::array<SkPoint, 4> corners{
      SkPoint::Make(source.left(), source.top()),
      SkPoint::Make(source.right(), source.top()),
      SkPoint::Make(source.right(), source.bottom()),
      SkPoint::Make(source.left(), source.bottom())};
  for (const auto &corner : corners) {
    const auto mapped = matrix.map(corner.x(), corner.y(), 0.0F, 1.0F);
    if (!std::isfinite(mapped.x) || !std::isfinite(mapped.y) ||
        !std::isfinite(mapped.w) || std::fabs(mapped.w) < 1.0e-6F) {
      return std::nullopt;
    }
    const float x = mapped.x / mapped.w;
    const float y = mapped.y / mapped.w;
    minimumX = std::min(minimumX, x);
    minimumY = std::min(minimumY, y);
    maximumX = std::max(maximumX, x);
    maximumY = std::max(maximumY, y);
  }
  return SkRect::MakeLTRB(minimumX, minimumY, maximumX, maximumY);
}


SkRect MapUnitFrameBounds(const UnitGeometry &unit,
                          const text::TextEffectUnitFramePlan *plan) {
  if (!plan || plan->transforms.empty())
    return unit.authoredBounds;
  const auto composed = text::ComposeTextEffectTransforms(plan->transforms);
  if (!composed)
    return unit.authoredBounds;
  auto bounds = MapEffectBounds(unit.authoredBounds, *composed)
                    .value_or(unit.authoredBounds);
  if (plan->sdfBlurRadius && *plan->sdfBlurRadius > 0.0F) {
    const float radius = std::clamp(*plan->sdfBlurRadius, 0.0F, 512.0F);
    bounds.outset(radius + 2.0F, radius + 2.0F);
  }
  return bounds;
}


TextVisualOutsets ResolveAllGlyphOutsets(
    const text::ResolvedRichTextView &view,
    const text::TextEffectFramePlan &framePlan) {
  TextVisualOutsets result;
  for (const auto &paragraph : view.paragraphs) {
    for (const auto &run : paragraph.runs) {
      result.Include(ResolveGlyphMaterialOutsets(run.style.materials));
      const auto includeDecoration = [&](const text::TextDecorationLine &line) {
        if (!line.enabled)
          return;
        const float extent =
            std::fabs(line.offset) +
            std::max(1.0F, line.thickness) *
                (line.style == text::TextDecorationLineStyle::Solid ||
                         line.style == text::TextDecorationLineStyle::Dotted ||
                         line.style == text::TextDecorationLineStyle::Dashed
                     ? 0.5F
                     : 1.5F);
        result.Include({extent, extent, extent, extent});
      };
      includeDecoration(run.style.decoration.underline);
      includeDecoration(run.style.decoration.strikeThrough);
    }
  }
  for (const auto &pass : framePlan.glyphMaterialPasses) {
    text::TextGlyphMaterialStack stack;
    stack.layers = {pass.material};
    result.Include(ResolveGlyphMaterialOutsets(stack));
  }
  return result;
}


text::Rect ResolveAuthoredControlBounds(
    const text::TextRenderDocument &document, const ResolvedLayout &layout,
    const SkRect &writerLetterBounds) {
  const text::TextEffectFramePlan neutralPlan;
  auto extent = layout.inkBounds;
  // SDF material distances live in the source atlas domain; its authored
  // Letter rectangles already include the drawable margin. Applying those
  // distances again as reference pixels would greatly inflate the handles.
  const auto outsets = document.appearance.sdfMaterial.enabled
      ? TextVisualOutsets{}
      : ResolveAllGlyphOutsets(document.text, neutralPlan);
  const auto &box = document.text.layoutBox;
  extent.fLeft -= std::max(outsets.left, box.padding.left +
      box.fitMeasurementOutsets.left * layout.shapingScale);
  extent.fTop -= std::max(outsets.top, box.padding.top +
      box.fitMeasurementOutsets.top * layout.shapingScale);
  extent.fRight += std::max(outsets.right, box.padding.right +
      box.fitMeasurementOutsets.right * layout.shapingScale);
  extent.fBottom += std::max(outsets.bottom, box.padding.bottom +
      box.fitMeasurementOutsets.bottom * layout.shapingScale);
  // TextPro's authored Letter rectangles include each glyph's atlas margin;
  // tight ink alone excludes flower artwork drawn in that surrounding area.
  extent.join(ToSkRect(layout.publicLayout.authoredLetterBounds));
  // The native SDF writer uses nominal glyph metrics and its fitted origin,
  // whereas shaping exposes tight outlines. Reuse the actual pre-animation
  // Letter union recorded for drawing so the handles enclose the same quads.
  if (writerLetterBounds.isFinite())
    extent.join(writerLetterBounds);
  text::TextRenderRequest neutralRequest;
  neutralRequest.suppressAnimation = true;
  for (const auto &pass :
       ResolveBackdropPasses(document, layout, neutralRequest, neutralPlan)) {
    extent.join(TransformBackdropBounds(
        ExpandByOutsets(pass.bounds, ResolvedBackdropOutsets(pass)),
        pass.transform, pass.bounds));
  }
  return text::IncludeTextControlExtent(
      layout.publicLayout.authoredControlBounds,
      {extent.x(), extent.y(), extent.width(), extent.height()});
}


SkRect ResolveRecordingBounds(const text::TextRenderDocument &document,
                              const ResolvedLayout &layout,
                              const text::TextRenderRequest &request,
                              const TextRenderFramePlan &renderPlan) {
  const auto &framePlan = renderPlan.value();
  SkRect result = layout.inkBounds;
  const auto plannedBounds =
      ToSkRect(framePlan.bounds.expandedRenderTargetBounds);
  if (!plannedBounds.isEmpty())
    result.join(plannedBounds);
  for (const auto &unit : layout.units) {
    result.join(MapUnitFrameBounds(
        unit,
        renderPlan.FindUnit(unit.binding.stableUnitId)));
  }
  const auto glyphOutsets = ResolveAllGlyphOutsets(document.text, framePlan);
  result.fLeft -= glyphOutsets.left;
  result.fTop -= glyphOutsets.top;
  result.fRight += glyphOutsets.right;
  result.fBottom += glyphOutsets.bottom;

  for (const auto &pass :
       ResolveBackdropPasses(document, layout, request, framePlan)) {
    const auto sourceVisualBounds = ExpandByOutsets(
        pass.bounds, ResolvedBackdropOutsets(pass));
    result.join(TransformBackdropBounds(sourceVisualBounds, pass.transform,
                                        pass.bounds));
  }
  for (const auto &pass : framePlan.decorationPasses) {
    const auto sourceBounds = ExpandByOutsets(
        ToSkRect(pass.bounds), ToVisualOutsets(pass.sourceOutsets));
    result.join(MapEffectBounds(sourceBounds, pass.transform.localToText)
                    .value_or(sourceBounds));
  }
  const auto postOutsets = ResolvePostEffectOutsets(framePlan.postEffectNodes);
  result.fLeft -= postOutsets.left;
  result.fTop -= postOutsets.top;
  result.fRight += postOutsets.right;
  result.fBottom += postOutsets.bottom;
  const float external = std::max(0.0F, request.externalEffectOutset);
  result.outset(external + 2.0F, external + 2.0F);
  if (result.isEmpty())
    result = layout.logicalBounds;
  return result;
}


void ApplyPresentationTransform(SkCanvas *canvas,
                                const text::ResolvedRichTextView &view,
                                const text::TextRenderRequest &request,
                                const SkRect &logicalBounds) {
  const float referenceWidth =
      std::max(1.0F, view.referenceCanvas.width);
  const float referenceHeight =
      std::max(1.0F, view.referenceCanvas.height);
  const float referenceScaleX =
      static_cast<float>(request.outputWidth) / referenceWidth;
  const float referenceScaleY =
      static_cast<float>(request.outputHeight) / referenceHeight;
  float scale = 1.0F;
  if (view.referenceCanvas.scalePolicy == text::CanvasScalePolicy::Fit)
    scale = std::min(referenceScaleX, referenceScaleY);
  else if (view.referenceCanvas.scalePolicy == text::CanvasScalePolicy::Fill)
    scale = std::max(referenceScaleX, referenceScaleY);
  const float offsetX =
      (static_cast<float>(request.outputWidth) - referenceWidth * scale) * 0.5F;
  const float offsetY =
      (static_cast<float>(request.outputHeight) - referenceHeight * scale) *
      0.5F;
  const float centerX = offsetX + logicalBounds.centerX() * scale;
  const float centerY = offsetY + logicalBounds.centerY() * scale;
  const float transformedWidth =
      logicalBounds.width() * scale * request.presentation.scaleX;
  const float transformedHeight =
      logicalBounds.height() * scale * request.presentation.scaleY;
  canvas->translate(request.presentation.positionX *
                        (request.outputWidth + transformedWidth) * 0.5F,
                    request.presentation.positionY *
                        (request.outputHeight + transformedHeight) * 0.5F);
  canvas->translate(centerX, centerY);
  canvas->rotate(request.presentation.rotationDegrees);
  canvas->scale(request.presentation.scaleX *
                    (request.presentation.flipHorizontal ? -1.0F : 1.0F),
                request.presentation.scaleY *
                    (request.presentation.flipVertical ? -1.0F : 1.0F));
  canvas->translate(-centerX, -centerY);
  canvas->translate(offsetX, offsetY);
  canvas->scale(scale, scale);
}


SkMatrix ResolvePresentationMatrix(const text::ResolvedRichTextView &view,
                                   const text::TextRenderRequest &request,
                                   const SkRect &logicalBounds) {
  SkPictureRecorder recorder;
  auto *canvas = recorder.beginRecording(
      SkRect::MakeWH(static_cast<float>(request.outputWidth),
                     static_cast<float>(request.outputHeight)));
  ApplyPresentationTransform(canvas, view, request, logicalBounds);
  const auto result = canvas->getTotalMatrix();
  static_cast<void>(recorder.finishRecordingAsPicture());
  return result;
}


SkPoint AuthoredPathPoint(const text::TextPathPoint &point,
                          const text::LayoutBox &layoutBox) noexcept {
  return SkPoint::Make(layoutBox.x + point.x * layoutBox.width,
                       layoutBox.y + point.y * layoutBox.height);
}


PathGeometry BuildPathGeometry(const text::TextPath &authored,
                               const text::LayoutBox &layoutBox,
                               const float sourceLeft,
                               const float sourceRight,
                               const float sourceBaseline) {
  PathGeometry result;
  if (!authored.enabled)
    return result;
  SkPathBuilder builder;
  for (const auto &command : authored.commands) {
    const auto end = AuthoredPathPoint(command.end, layoutBox);
    switch (command.kind) {
    case text::TextPathCommandKind::MoveTo:
      builder.moveTo(end);
      break;
    case text::TextPathCommandKind::LineTo:
      builder.lineTo(end);
      break;
    case text::TextPathCommandKind::QuadraticTo:
      builder.quadTo(AuthoredPathPoint(command.control1, layoutBox), end);
      break;
    case text::TextPathCommandKind::CubicTo:
      builder.cubicTo(AuthoredPathPoint(command.control1, layoutBox),
                      AuthoredPathPoint(command.control2, layoutBox), end);
      break;
    case text::TextPathCommandKind::Close:
      builder.close();
      break;
    }
  }
  result.path = builder.detach();
  result.measure = std::make_unique<SkPathMeasure>(result.path, false, 4.0F);
  result.length = result.measure->getLength();
  const float sourceWidth = std::max(1.0F, sourceRight - sourceLeft);
  if (!std::isfinite(result.length) || result.length <= 0.5F) {
    result.measure.reset();
    return result;
  }
  result.enabled = true;
  result.overflow = authored.overflow;
  result.loop = authored.loop;
  result.rotateToTangent = authored.rotateToTangent;
  result.keepUpright = authored.keepUpright;
  result.sourceLeft = sourceLeft;
  result.sourceRight = sourceRight;
  result.sourceBaseline = sourceBaseline;
  result.startDistance = authored.startOffset * result.length;
  result.baselineOffset = authored.baselineOffset;
  result.horizontalScale =
      authored.overflow == text::TextPathOverflow::ScaleToFit
          ? std::clamp(result.length / sourceWidth, 0.01F, 100.0F)
          : 1.0F;
  return result;
}


bool SamplePath(const PathGeometry &path, const float sourceX,
                PathSample &output) {
  if (!path.enabled || !path.measure || path.length <= 0.0F)
    return false;
  float distance =
      path.startDistance + (sourceX - path.sourceLeft) * path.horizontalScale;
  float extrapolation = 0.0F;
  if (path.loop) {
    distance = std::fmod(distance, path.length);
    if (distance < 0.0F)
      distance += path.length;
  } else if (distance < 0.0F || distance > path.length) {
    if (path.overflow != text::TextPathOverflow::Visible)
      return false;
    extrapolation = distance < 0.0F ? distance : distance - path.length;
    distance = std::clamp(distance, 0.0F, path.length);
  }
  SkPoint position;
  SkVector tangent;
  if (!path.measure->getPosTan(distance, &position, &tangent))
    return false;
  const float magnitude = std::hypot(tangent.x(), tangent.y());
  if (!std::isfinite(magnitude) || magnitude <= 0.0001F)
    return false;
  tangent.scale(1.0F / magnitude);
  position.offset(tangent.x() * extrapolation, tangent.y() * extrapolation);
  float angle = path.rotateToTangent
                    ? std::atan2(tangent.y(), tangent.x()) * 180.0F /
                          SK_ScalarPI
                    : 0.0F;
  if (path.rotateToTangent && path.keepUpright &&
      (angle > 90.0F || angle < -90.0F)) {
    angle += angle > 90.0F ? -180.0F : 180.0F;
    tangent = SkVector::Make(-tangent.x(), -tangent.y());
  }
  output = {position, tangent, angle};
  return true;
}


bool MapPathPoint(const PathGeometry &path, const float sourceX,
                  const float sourceY, SkPoint &output) {
  PathSample sample;
  if (!SamplePath(path, sourceX, sample))
    return false;
  const float radians = sample.angleDegrees * SK_ScalarPI / 180.0F;
  const SkVector normal =
      path.rotateToTangent
          ? SkVector::Make(-std::sin(radians), std::cos(radians))
          : SkVector::Make(0.0F, 1.0F);
  const float normalDistance =
      sourceY - path.sourceBaseline + path.baselineOffset;
  output = SkPoint::Make(sample.position.x() + normal.x() * normalDistance,
                         sample.position.y() + normal.y() * normalDistance);
  return true;
}


bool DrawPictureOnPath(SkCanvas *canvas, const SkPicture &picture,
                       const SkRect &sourceInk, const SkRect &sourceBounds,
                       const PathGeometry &path) {
  if (!canvas || !path.enabled || sourceInk.isEmpty() || sourceBounds.isEmpty())
    return false;
  const float left = sourceInk.left();
  const float width = std::max(1.0F, sourceInk.width());
  const int slices =
      std::clamp(static_cast<int>(std::ceil(width / 2.0F)), 32, 2048);
  const int vertexCount = (slices + 1) * 2;
  std::vector<SkPoint> positions(static_cast<std::size_t>(vertexCount));
  std::vector<SkPoint> textureCoordinates(
      static_cast<std::size_t>(vertexCount));
  std::vector<bool> valid(static_cast<std::size_t>(slices + 1), false);
  const float sliceWidth = width / static_cast<float>(slices);
  for (int index = 0; index <= slices; ++index) {
    const float x = index == slices
                        ? sourceInk.right()
                        : left + sliceWidth * static_cast<float>(index);
    const auto topIndex = static_cast<std::size_t>(index * 2);
    const auto bottomIndex = topIndex + 1U;
    textureCoordinates[topIndex] = SkPoint::Make(x, sourceBounds.top());
    textureCoordinates[bottomIndex] =
        SkPoint::Make(x, sourceBounds.bottom());
    valid[static_cast<std::size_t>(index)] =
        MapPathPoint(path, x, sourceBounds.top(), positions[topIndex]) &&
        MapPathPoint(path, x, sourceBounds.bottom(), positions[bottomIndex]);
  }
  std::vector<std::uint16_t> indices;
  indices.reserve(static_cast<std::size_t>(slices) * 6U);
  for (int index = 0; index < slices; ++index) {
    if (!valid[static_cast<std::size_t>(index)] ||
        !valid[static_cast<std::size_t>(index + 1)]) {
      continue;
    }
    const auto topLeft = static_cast<std::uint16_t>(index * 2);
    const auto bottomLeft = static_cast<std::uint16_t>(topLeft + 1U);
    const auto topRight = static_cast<std::uint16_t>(topLeft + 2U);
    const auto bottomRight = static_cast<std::uint16_t>(topLeft + 3U);
    const auto leftNormal = positions[bottomLeft] - positions[topLeft];
    const auto rightNormal = positions[bottomRight] - positions[topRight];
    if (SkPoint::DotProduct(leftNormal, rightNormal) <= 0.0F)
      continue;
    indices.insert(indices.end(), {topLeft, bottomLeft, topRight, topRight,
                                   bottomLeft, bottomRight});
  }
  if (indices.empty())
    return true;
  auto vertices =
      SkVertices::MakeCopy(SkVertices::kTriangles_VertexMode, vertexCount,
                           positions.data(), textureCoordinates.data(), nullptr,
                           static_cast<int>(indices.size()), indices.data());
  if (!vertices)
    return false;
  SkPaint paint;
  paint.setShader(picture.makeShader(SkTileMode::kDecal, SkTileMode::kDecal,
                                     SkFilterMode::kLinear, nullptr,
                                     &sourceBounds));
  canvas->drawVertices(vertices, SkBlendMode::kSrcOver, paint);
  return true;
}


SkRect MapPathRect(const SkMatrix &matrix, const SkRect &source,
                   const PathGeometry &path) {
  if (!path.enabled)
    return matrix.mapRect(source);
  constexpr int kSamples = 33;
  std::vector<SkPoint> points;
  points.reserve(kSamples * 2U);
  for (int index = 0; index < kSamples; ++index) {
    const float fraction =
        static_cast<float>(index) / static_cast<float>(kSamples - 1);
    const float x = source.left() + source.width() * fraction;
    SkPoint top;
    SkPoint bottom;
    if (MapPathPoint(path, x, source.top(), top))
      points.push_back(top);
    if (MapPathPoint(path, x, source.bottom(), bottom))
      points.push_back(bottom);
  }
  if (points.empty())
    return SkRect::MakeEmpty();
  matrix.mapPoints(points);
  float left = points.front().x();
  float top = points.front().y();
  float right = left;
  float bottom = top;
  for (const auto &point : points) {
    left = std::min(left, point.x());
    top = std::min(top, point.y());
    right = std::max(right, point.x());
    bottom = std::max(bottom, point.y());
  }
  return SkRect::MakeLTRB(left, top, right, bottom);
}


float MapPathY(const SkMatrix &matrix, const float x, const float y,
               const PathGeometry &path) {
  SkPoint point;
  if (!MapPathPoint(path, x, y, point))
    point = SkPoint::Make(x, y);
  matrix.mapPoints({&point, 1U});
  return point.y();
}


float BendOffsetY(const BendGeometry &bend, const float x) noexcept {
  if (!bend.enabled || std::fabs(bend.amount) < 0.0001F || bend.width <= 0.0F)
    return 0.0F;
  const float normalized =
      std::clamp(((x - bend.left) / bend.width) * 2.0F - 1.0F, -1.0F,
                 1.0F);
  return -bend.amount * bend.height * 0.25F *
         (1.0F - normalized * normalized);
}


SkRect MapBentRect(const SkMatrix &matrix, const SkRect &source,
                   const BendGeometry &bend) {
  std::array<SkPoint, 10> points{};
  const std::array<float, 5> xValues{
      source.left(), source.left() + source.width() * 0.25F,
      source.centerX(), source.left() + source.width() * 0.75F,
      source.right()};
  for (std::size_t index = 0U; index < xValues.size(); ++index) {
    const float offset = BendOffsetY(bend, xValues[index]);
    points[index * 2U] =
        SkPoint::Make(xValues[index], source.top() + offset);
    points[index * 2U + 1U] =
        SkPoint::Make(xValues[index], source.bottom() + offset);
  }
  matrix.mapPoints(points);
  float left = points.front().x();
  float top = points.front().y();
  float right = left;
  float bottom = top;
  for (const auto &point : points) {
    left = std::min(left, point.x());
    top = std::min(top, point.y());
    right = std::max(right, point.x());
    bottom = std::max(bottom, point.y());
  }
  return SkRect::MakeLTRB(left, top, right, bottom);
}


float MapBentY(const SkMatrix &matrix, const float x, const float y,
               const BendGeometry &bend) {
  SkPoint point = SkPoint::Make(x, y + BendOffsetY(bend, x));
  matrix.mapPoints({&point, 1U});
  return point.y();
}


SkRect MapDeformedRect(const SkMatrix &matrix, const SkRect &source,
                       const BendGeometry &bend,
                       const PathGeometry &path) {
  return path.enabled ? MapPathRect(matrix, source, path)
                      : MapBentRect(matrix, source, bend);
}


float MapDeformedY(const SkMatrix &matrix, const float x, const float y,
                   const BendGeometry &bend, const PathGeometry &path) {
  return path.enabled ? MapPathY(matrix, x, y, path)
                      : MapBentY(matrix, x, y, bend);
}


void MapLayoutToPresentation(text::TextLayout &layout,
                             const SkMatrix &matrix,
                             const SkRect &authoredVisual,
                             const SkRect &liveLetterBounds,
                             const BendGeometry &bend,
                             const PathGeometry &path,
                             const std::uint32_t outputWidth,
                             const std::uint32_t outputHeight) {
  const auto mapRect = [&](const SkMatrix &transform,
                           const text::Rect &source) {
    const auto mapped =
        MapDeformedRect(transform, ToSkRect(source), bend, path);
    return text::Rect{mapped.x(), mapped.y(), mapped.width(), mapped.height()};
  };
  const auto sourceLogical = layout.authoredLogicalBounds;
  const auto sourceInk = layout.authoredInkBounds;
  const auto sourceLetter = layout.authoredLetterBounds;
  const auto sourceControl = layout.authoredControlBounds;
  const auto authoredVisualRect = text::Rect{
      authoredVisual.x(), authoredVisual.y(), authoredVisual.width(),
      authoredVisual.height()};
  layout.outputWidth = outputWidth;
  layout.outputHeight = outputHeight;
  layout.authoredLogicalBounds =
      mapRect(SkMatrix::I(), sourceLogical);
  layout.authoredInkBounds = mapRect(SkMatrix::I(), sourceInk);
  layout.authoredLetterBounds = mapRect(SkMatrix::I(), sourceLetter);
  layout.authoredControlBounds = mapRect(SkMatrix::I(), sourceControl);
  layout.authoredVisualExtent =
      mapRect(SkMatrix::I(), authoredVisualRect);
  layout.authoredConservativeEnvelope = layout.authoredVisualExtent;
  layout.logicalBounds = mapRect(matrix, sourceLogical);
  layout.inkBounds = mapRect(matrix, sourceInk);
  layout.letterBounds = mapRect(
      matrix, liveLetterBounds.isEmpty()
                  ? sourceLetter
                  : text::Rect{liveLetterBounds.x(), liveLetterBounds.y(),
                               liveLetterBounds.width(),
                               liveLetterBounds.height()});
  layout.controlBounds = mapRect(matrix, sourceControl);
  layout.visualExtent = mapRect(matrix, authoredVisualRect);
  layout.conservativeEnvelope = layout.visualExtent;
  layout.lines = layout.authoredLines;
  layout.baselines.clear();
  for (auto &line : layout.lines) {
    line.baseline = MapDeformedY(matrix, line.bounds.x, line.baseline, bend,
                                 path);
    line.bounds = mapRect(matrix, line.bounds);
    layout.baselines.push_back(line.baseline);
  }
  layout.clusters = layout.authoredClusters;
  for (auto &cluster : layout.clusters) {
    for (auto &box : cluster.boxes) {
      box.bounds = mapRect(matrix, box.bounds);
      box.utf8BeginCaret = mapRect(matrix, box.utf8BeginCaret);
      box.utf8EndCaret = mapRect(matrix, box.utf8EndCaret);
    }
  }
}


bool IsFinitePresentation(const text::TextRenderRequest &request) noexcept {
  const auto &presentation = request.presentation;
  return std::isfinite(presentation.positionX) &&
         std::isfinite(presentation.positionY) &&
         std::isfinite(presentation.scaleX) &&
         std::isfinite(presentation.scaleY) &&
         std::isfinite(presentation.rotationDegrees) &&
         std::isfinite(presentation.opacity) &&
         presentation.scaleX != 0.0F && presentation.scaleY != 0.0F &&
         presentation.opacity >= 0.0F && presentation.opacity <= 1.0F &&
         std::isfinite(request.externalEffectOutset) &&
         request.externalEffectOutset >= 0.0F && request.durationUs >= 0;
}

} // namespace videocut::skia_runtime::internal::text_lane
