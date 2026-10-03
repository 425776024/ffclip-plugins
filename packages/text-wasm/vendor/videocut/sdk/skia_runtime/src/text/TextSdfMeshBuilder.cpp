#include "text/TextSdfMeshBuilder.h"
#include "text/TextSdfMeshPositionContract.h"
#include "text/TextSdfPathTransform.h"
#include "include/pathops/SkPathOps.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace videocut::skia_runtime::internal {
namespace {

constexpr int kMaximumAtlasExtent = 32 * 1024;
constexpr std::size_t kMaximumPathVerbs = 128U * 1024U;
constexpr std::size_t kMaximumOutlinePrimitives = 512U * 1024U;
constexpr std::size_t kMaximumTriangleVertices = 3U * 512U * 1024U;
constexpr float kMinimumDistanceRange = 0.25F;
constexpr float kMaximumDistanceMetric = 65'536.0F;
constexpr float kContainmentTolerance = 1.0e-4F;

enum class SdfOutlinePrimitiveKind : std::uint8_t {
  Line,
  Quadratic,
};

constexpr float kTextSdfLineCanonicalHalfExtent = 0.001F;
constexpr float kTextSdfLineOriginEpsilon = 0.000001F;

struct SdfOutlinePrimitive final {
  SdfOutlinePrimitiveKind kind{SdfOutlinePrimitiveKind::Line};
  SkPoint start;
  SkPoint control;
  SkPoint end;
  SkVector canonicalX{0.0F, 0.0F};
  SkVector canonicalY{0.0F, 0.0F};
  SkPoint canonicalOffset{0.0F, 0.0F};
  SkVector qtCanonicalRowX{0.0F, 0.0F};
  SkVector qtCanonicalRowY{0.0F, 0.0F};
  SkPoint qtCanonicalOrigin{0.0F, 0.0F};
  float canonicalLimitBegin{0.0F};
  float canonicalLimitEnd{0.0F};
  float inverseCanonicalScale{1.0F};
};

[[nodiscard]] SkPoint Midpoint(const SkPoint left,
                               const SkPoint right) noexcept {
#if defined(__clang__)
#pragma clang fp contract(off)
#endif
  const float x = left.x() + right.x();
  const float y = left.y() + right.y();
  return {x * 0.5F, y * 0.5F};
}

void AppendLinePrimitive(const SkPoint start, const SkPoint end,
                         std::vector<SdfOutlinePrimitive> &primitives) {
#if defined(__clang__)
#pragma clang fp contract(off)
#endif
  if (start == end)
    return;

  const float deltaX = end.x() - start.x();
  const float deltaY = end.y() - start.y();
  const float midpointSumX = end.x() + start.x();
  const float midpointSumY = end.y() + start.y();
  const float midpointX = midpointSumX * 0.5F;
  const float midpointY = midpointSumY * 0.5F;
  const float deltaSquaredX = deltaX * deltaX;
  const float deltaSquaredY = deltaY * deltaY;
  const float lengthSquared = deltaSquaredY + deltaSquaredX;
  const float length = std::sqrt(lengthSquared);
  if (!(length > 0.0F) || !std::isfinite(length))
    return;
  const float directionX = deltaX / length;
  const float directionY = deltaY / length;
  const float epsilonLength = kTextSdfLineOriginEpsilon * length;
  const float originYOffset = directionX * epsilonLength;
  const float originY = originYOffset + midpointY;
  const float originXOffset = directionY * epsilonLength;
  const float originX = midpointX - originXOffset;
  const float halfLength = length * 0.5F;
  const float inverseCanonicalScale =
      halfLength / kTextSdfLineCanonicalHalfExtent;

  SdfOutlinePrimitive primitive;
  primitive.start = start;
  primitive.control = Midpoint(start, end);
  primitive.end = end;
  primitive.qtCanonicalRowX = {directionX, directionY};
  primitive.qtCanonicalRowY = {-directionY, directionX};
  primitive.qtCanonicalOrigin = {originX, originY};
  primitive.canonicalLimitBegin = -kTextSdfLineCanonicalHalfExtent;
  primitive.canonicalLimitEnd = kTextSdfLineCanonicalHalfExtent;
  primitive.inverseCanonicalScale = inverseCanonicalScale;
  primitives.push_back(primitive);
}

void AppendQuadraticPrimitive(
    const SkPoint start, const SkPoint control, const SkPoint end,
    std::vector<SdfOutlinePrimitive> &primitives) {
#if defined(__clang__)
#pragma clang fp contract(off)
#endif
  constexpr float kMinimumArmBalance = 0.1F;
  constexpr float kMinimumNonCollinearity = 0.01F;
  const SkVector startArm{control.x() - start.x(),
                          control.y() - start.y()};
  const SkVector endArm{end.x() - control.x(), end.y() - control.y()};
  const float startArmLengthSquaredX = startArm.x() * startArm.x();
  const float startArmLengthSquaredY = startArm.y() * startArm.y();
  const float startArmLengthSquared =
      startArmLengthSquaredX + startArmLengthSquaredY;
  const float endArmLengthSquaredX = endArm.x() * endArm.x();
  const float endArmLengthSquaredY = endArm.y() * endArm.y();
  const float endArmLengthSquared =
      endArmLengthSquaredX + endArmLengthSquaredY;
  const float startArmLength = std::sqrt(startArmLengthSquared);
  const float endArmLength = std::sqrt(endArmLengthSquared);
  const float armLengthSum = startArmLength + endArmLength;
  if (!(armLengthSum > 0.0F) || !std::isfinite(armLengthSum)) {
    AppendLinePrimitive(start, end, primitives);
    return;
  }

  const float startArmRatio = startArmLength / armLengthSum;
  const float armBalance = std::min(startArmRatio, 1.0F - startArmRatio);
  if (!(armBalance >= kMinimumArmBalance)) {
    AppendLinePrimitive(start, end, primitives);
    return;
  }

  const SkVector normalizedStartArm{startArm.x() / startArmLength,
                                    startArm.y() / startArmLength};
  const SkVector normalizedEndArm{endArm.x() / endArmLength,
                                  endArm.y() / endArmLength};
  const float normalizedDotX = normalizedStartArm.x() * normalizedEndArm.x();
  const float normalizedDotY = normalizedStartArm.y() * normalizedEndArm.y();
  const float normalizedDot = normalizedDotX + normalizedDotY;
  if (!std::isfinite(normalizedDot) ||
      1.0F - std::abs(normalizedDot) < kMinimumNonCollinearity) {
    AppendLinePrimitive(start, control, primitives);
    AppendLinePrimitive(control, end, primitives);
    return;
  }

  float midpointAxisX = end.x() + start.x();
  float midpointAxisY = end.y() + start.y();
  midpointAxisX *= 0.5F;
  midpointAxisY *= 0.5F;
  midpointAxisX -= control.x();
  midpointAxisY -= control.y();
  const SkVector midpointAxis{midpointAxisX, midpointAxisY};
  const float midpointAxisLengthSquaredX =
      midpointAxis.x() * midpointAxis.x();
  const float midpointAxisLengthSquaredY =
      midpointAxis.y() * midpointAxis.y();
  const float midpointAxisLengthSquared =
      midpointAxisLengthSquaredX + midpointAxisLengthSquaredY;
  const float midpointAxisLength = std::sqrt(midpointAxisLengthSquared);
  if (!(midpointAxisLength > 1.0e-6F) ||
      !std::isfinite(midpointAxisLength)) {
    AppendLinePrimitive(start, end, primitives);
    return;
  }

  const SkVector axis{midpointAxis.x() / midpointAxisLength,
                      midpointAxis.y() / midpointAxisLength};
  const float startDotX = normalizedStartArm.x() * axis.x();
  const float startDotY = normalizedStartArm.y() * axis.y();
  const float startDot = startDotX + startDotY;
  const float startCrossX = normalizedStartArm.x() * axis.y();
  const float startCrossY = normalizedStartArm.y() * axis.x();
  const float startCross = startCrossX - startCrossY;
  const float endDotX = normalizedEndArm.x() * axis.x();
  const float endDotY = normalizedEndArm.y() * axis.y();
  const float endDot = endDotX + endDotY;
  const float endCrossX = normalizedEndArm.x() * axis.y();
  const float endCrossY = normalizedEndArm.y() * axis.x();
  const float endCross = endCrossX - endCrossY;
  if (std::abs(startCross) <= 1.0e-6F ||
      std::abs(endCross) <= 1.0e-6F || !std::isfinite(startCross) ||
      !std::isfinite(endCross)) {
    AppendLinePrimitive(start, control, primitives);
    AppendLinePrimitive(control, end, primitives);
    return;
  }

  const float startRatio = startDot / startCross;
  const float endRatio = endDot / endCross;
  const float canonicalStart = startRatio * 0.5F;
  const float canonicalEnd = endRatio * 0.5F;
  const float endpointDeltaX = end.x() - start.x();
  const float endpointDeltaY = end.y() - start.y();
  const float endpointCrossX = endpointDeltaX * axis.y();
  const float endpointCrossY = endpointDeltaY * axis.x();
  const float endpointCross = endpointCrossX - endpointCrossY;
  const float canonicalSpan = canonicalEnd - canonicalStart;
  const float inverseCanonicalScale = endpointCross / canonicalSpan;
  if (!(inverseCanonicalScale > 0.0F) ||
      !std::isfinite(inverseCanonicalScale) ||
      !std::isfinite(canonicalStart) || !std::isfinite(canonicalEnd)) {
    AppendLinePrimitive(start, control, primitives);
    AppendLinePrimitive(control, end, primitives);
    return;
  }

  const float canonicalScale = 1.0F / inverseCanonicalScale;
  const SkVector perpendicular{axis.y(), -axis.x()};
  const float canonicalStartSquared = canonicalStart * canonicalStart;
  const float scaledCanonicalSquare =
      canonicalStartSquared * inverseCanonicalScale;
  float qtCanonicalOriginX = axis.x() * scaledCanonicalSquare;
  qtCanonicalOriginX = start.x() - qtCanonicalOriginX;
  float qtCanonicalOriginY = axis.y() * scaledCanonicalSquare;
  qtCanonicalOriginY = start.y() - qtCanonicalOriginY;
  const float scaledCanonicalStart = canonicalStart * inverseCanonicalScale;
  const float qtCanonicalOriginXTerm = axis.y() * scaledCanonicalStart;
  qtCanonicalOriginX -= qtCanonicalOriginXTerm;
  const float qtCanonicalOriginYTerm = axis.x() * scaledCanonicalStart;
  qtCanonicalOriginY = qtCanonicalOriginYTerm + qtCanonicalOriginY;

  SdfOutlinePrimitive primitive;
  primitive.kind = SdfOutlinePrimitiveKind::Quadratic;
  primitive.start = start;
  primitive.control = control;
  primitive.end = end;
  primitive.canonicalX = perpendicular * canonicalScale;
  primitive.canonicalY = axis * canonicalScale;
  primitive.canonicalOffset = {canonicalStart, canonicalStartSquared};
  primitive.qtCanonicalRowX = {axis.y(), -axis.x()};
  primitive.qtCanonicalRowY = axis;
  primitive.qtCanonicalOrigin = {qtCanonicalOriginX, qtCanonicalOriginY};
  primitive.canonicalLimitBegin = std::min(canonicalStart, canonicalEnd);
  primitive.canonicalLimitEnd = std::max(canonicalStart, canonicalEnd);
  primitive.inverseCanonicalScale = inverseCanonicalScale;
  primitives.push_back(primitive);
}

[[nodiscard]] SkPoint
QtCanonicalPrimitivePosition(const SkPoint point,
                             const SdfOutlinePrimitive &primitive) {
#if defined(__clang__)
#pragma clang fp contract(off)
#endif
  const float reciprocalScale = 1.0F / primitive.inverseCanonicalScale;
  const float deltaX = point.x() - primitive.qtCanonicalOrigin.x();
  const float deltaY = point.y() - primitive.qtCanonicalOrigin.y();
  const float rowXProductX = primitive.qtCanonicalRowX.x() * deltaX;
  const float rowXProductY = primitive.qtCanonicalRowX.y() * deltaY;
  const float rowYProductX = primitive.qtCanonicalRowY.x() * deltaX;
  const float rowYProductY = primitive.qtCanonicalRowY.y() * deltaY;
  const float rowX = rowXProductY + rowXProductX;
  const float rowY = rowYProductY + rowYProductX;
  return {rowX * reciprocalScale, rowY * reciprocalScale};
}

[[nodiscard]] SkPoint EvaluateCubic(const SkPoint start,
                                    const SkPoint control1,
                                    const SkPoint control2,
                                    const SkPoint end,
                                    const float parameter) {
#if defined(__clang__)
#pragma clang fp contract(off)
#endif
  const float inverse = 1.0F - parameter;
  const auto component = [&](const float p0, const float p1, const float p2,
                             const float p3) {
    float value = p0 * inverse;
    value *= inverse;
    value *= inverse;
    float term = p1 * 3.0F;
    term *= parameter;
    term *= inverse;
    term *= inverse;
    value = term + value;
    term = p2 * 3.0F;
    term *= parameter;
    term *= parameter;
    term *= inverse;
    value = term + value;
    term = p3 * parameter;
    term *= parameter;
    term *= parameter;
    return term + value;
  };
  return {component(start.x(), control1.x(), control2.x(), end.x()),
          component(start.y(), control1.y(), control2.y(), end.y())};
}

[[nodiscard]] SkVector EvaluateCubicTangent(
    const SkPoint start, const SkPoint control1, const SkPoint control2,
    const SkPoint end, const float parameter) {
#if defined(__clang__)
#pragma clang fp contract(off)
#endif
  const float inverse = 1.0F - parameter;
  float startFactor = -3.0F * inverse;
  startFactor *= inverse;
  const float threeParameter = parameter * 3.0F;
  const float control1Factor = 1.0F - threeParameter;
  const float control2Factor = (2.0F - threeParameter) * parameter;
  const auto component = [&](const float p0, const float p1, const float p2,
                             const float p3) {
    float value = p0 * startFactor;
    float term = p1 * 3.0F;
    term *= inverse;
    term *= control1Factor;
    value = term + value;
    term = p2 * 3.0F;
    term *= control2Factor;
    value = term + value;
    term = p3 * 3.0F;
    term *= parameter;
    term *= parameter;
    return term + value;
  };
  return {component(start.x(), control1.x(), control2.x(), end.x()),
          component(start.y(), control1.y(), control2.y(), end.y())};
}

[[nodiscard]] float
QtQuadraticTangentAngle(const SkVector tangent) noexcept {
  if (tangent.x() != 0.0F &&
      std::abs(tangent.y() / tangent.x()) < 30.0F) {
    return std::atan(tangent.y() / tangent.x());
  }
  return 1.5707964F;
}

void AppendQtCubicQuadratics(
    const SkPoint start, const SkPoint control1, const SkPoint control2,
    const SkPoint end, std::vector<SdfOutlinePrimitive> &primitives) {
#if defined(__clang__)
#pragma clang fp contract(off)
#endif
  constexpr float kTangentIntersectionThreshold = 0.08722222F;
  for (int half = 0; half < 2; ++half) {
    const float beginParameter = static_cast<float>(half) * 0.5F;
    const float endParameter = static_cast<float>(half + 1) * 0.5F;
    const auto begin =
        EvaluateCubic(start, control1, control2, end, beginParameter);
    const auto finish =
        EvaluateCubic(start, control1, control2, end, endParameter);
    const auto beginTangent =
        EvaluateCubicTangent(start, control1, control2, end, beginParameter);
    const auto endTangent =
        EvaluateCubicTangent(start, control1, control2, end, endParameter);
    SkPoint quadraticControl = Midpoint(begin, finish);
    if (std::abs(QtQuadraticTangentAngle(beginTangent) -
                 QtQuadraticTangentAngle(endTangent)) >=
        kTangentIntersectionThreshold) {
      float endTangentParameter = 0.0F;
      bool hasIntersection = false;
      if (std::abs(beginTangent.x()) <= 0.001F) {
        if (endTangent.x() != 0.0F) {
          endTangentParameter =
              (begin.x() - finish.x()) / endTangent.x();
          hasIntersection = std::isfinite(endTangentParameter);
        }
      } else {
        float scaledDeltaX = finish.x() - begin.x();
        scaledDeltaX *= beginTangent.y();
        scaledDeltaX /= beginTangent.x();
        float scaledEndTangentX = endTangent.x() * beginTangent.y();
        scaledEndTangentX /= beginTangent.x();
        const float numerator = (begin.y() - finish.y()) + scaledDeltaX;
        const float denominator = endTangent.y() - scaledEndTangentX;
        if (denominator != 0.0F) {
          endTangentParameter = numerator / denominator;
          hasIntersection = std::isfinite(endTangentParameter);
        }
      }
      if (hasIntersection) {
        const SkPoint intersection{
            endTangent.x() * endTangentParameter + finish.x(),
            endTangent.y() * endTangentParameter + finish.y()};
        if (std::isfinite(intersection.x()) &&
            std::isfinite(intersection.y())) {
          quadraticControl = intersection;
        }
      }
    }
    AppendQuadraticPrimitive(begin, quadraticControl, finish, primitives);
  }
}

[[nodiscard]] std::vector<SdfOutlinePrimitive>
BuildGlyphOutlinePrimitives(const SkPath &path) {
  constexpr int kConicSubdivisionPower = 4;
  std::vector<SdfOutlinePrimitive> primitives;
  primitives.reserve(
      std::min<std::size_t>(path.countVerbs() * 2U, 4096U));
  SkPath::Iter iterator(path, true);
  SkPoint points[4];
  while (true) {
    const auto verb = iterator.next(points);
    switch (verb) {
    case SkPath::kMove_Verb:
      break;
    case SkPath::kLine_Verb:
      AppendLinePrimitive(points[0], points[1], primitives);
      break;
    case SkPath::kQuad_Verb:
      AppendQuadraticPrimitive(points[0], points[1], points[2], primitives);
      break;
    case SkPath::kConic_Verb: {
      std::array<SkPoint, 1 + 2 * (1 << kConicSubdivisionPower)> quadratics;
      const int count = SkPath::ConvertConicToQuads(
          points[0], points[1], points[2], iterator.conicWeight(),
          quadratics.data(), kConicSubdivisionPower);
      for (int index = 0; index < count; ++index) {
        const auto offset = static_cast<std::size_t>(index) * 2U;
        AppendQuadraticPrimitive(quadratics[offset], quadratics[offset + 1U],
                                 quadratics[offset + 2U], primitives);
      }
      break;
    }
    case SkPath::kCubic_Verb:
      AppendQtCubicQuadratics(points[0], points[1], points[2], points[3],
                              primitives);
      break;
    case SkPath::kClose_Verb:
      break;
    case SkPath::kDone_Verb:
      return primitives;
    }
  }
}

struct TextSdfMeshPlacement final {
  int targetWidth{0};
  int targetHeight{0};
  float anchorX{0.0F};
  float anchorY{0.0F};
  float pathOriginX{0.0F};
  float pathOriginY{0.0F};
  int outputX{0};
  int outputY{0};
  int outputWidth{0};
  int outputHeight{0};
};

[[nodiscard]] TextSdfGpuMesh BuildNativeTextSdfMesh(
    const int width, const int height, const float distanceLimit,
    const SkRect pathBounds,
    const QtTextGlyphOutlineCoordinateSystem pathCoordinateSystem,
    const std::vector<SdfOutlinePrimitive> &primitives,
    const TextSdfMeshPlacement *placement = nullptr) {
  TextSdfGpuMesh mesh;
  const auto nextPowerOfTwo = [](const int value) {
    int result = 1;
    while (result < value && result < kMaximumAtlasExtent)
      result *= 2;
    return result;
  };
  if (placement != nullptr) {
    mesh.targetWidth = placement->targetWidth;
    mesh.targetHeight = placement->targetHeight;
    mesh.outputX = placement->outputX;
    mesh.outputY = placement->outputY;
    mesh.outputWidth = placement->outputWidth;
    mesh.outputHeight = placement->outputHeight;
  } else {
    mesh.targetWidth = nextPowerOfTwo(std::max(512, width));
    mesh.targetHeight = nextPowerOfTwo(std::max(512, height));
    mesh.outputX = 0;
    mesh.outputY = 0;
    mesh.outputWidth = width;
    mesh.outputHeight = height;
  }

  const float targetWidth = static_cast<float>(mesh.targetWidth);
  const float targetHeight = static_cast<float>(mesh.targetHeight);
  const bool coreTextYUp =
      pathCoordinateSystem == QtTextGlyphOutlineCoordinateSystem::CoreTextYUp;
  const float anchorX =
      placement != nullptr ? placement->anchorX : distanceLimit;
  const float anchorY =
      placement != nullptr ? placement->anchorY : distanceLimit;
  const float positionOffsetX =
      placement != nullptr && coreTextYUp
          ? anchorX - placement->pathOriginX
          : anchorX - pathBounds.left();
  const float positionOffsetY =
      coreTextYUp ? anchorY - (placement != nullptr
                                   ? placement->pathOriginY
                                   : pathBounds.top())
                  : pathBounds.bottom() + anchorY;
  const auto qtPixelPosition = [&](const SkPoint point) {
    return SkPoint{point.x() + positionOffsetX,
                   coreTextYUp ? point.y() + positionOffsetY
                               : positionOffsetY - point.y()};
  };
  const auto qtPosition = [&](const SkPoint point) {
    const auto pixels = qtPixelPosition(point);
    return SkPoint{pixels.x() / targetWidth, pixels.y() / targetHeight};
  };
  const auto appendDistanceVertex =
      [&](const SkPoint normalizedPosition, const SkPoint parabola,
          const float limitBegin, const float limitEnd,
          const float distanceScale) {
        mesh.distanceVertices.push_back(
            {normalizedPosition.x(), normalizedPosition.y(), parabola.x(),
             parabola.y(), limitBegin, limitEnd, distanceScale,
             distanceLimit});
      };
  const auto appendShapeVertex = [&](const SkPoint point,
                                     const float parabolaX,
                                     const float parabolaY) {
    const auto position = qtPosition(point);
    mesh.shapeVertices.push_back(
        {position.x(), position.y(), parabolaX, parabolaY});
  };

  mesh.distanceVertices.reserve(primitives.size() * 6U);
  mesh.shapeVertices.reserve(primitives.size() * 6U);
  SkPoint contourAnchor{0.0F, 0.0F};
  SkPoint previousEnd{0.0F, 0.0F};
  bool hasContour = false;
  for (const auto &primitive : primitives) {
    const bool beginsContour =
        !hasContour || primitive.start != previousEnd;
    if (beginsContour)
      contourAnchor = primitive.start;
    hasContour = true;

    if (primitive.kind == SdfOutlinePrimitiveKind::Quadratic) {
      const SkPoint startControlMidpoint =
          Midpoint(primitive.start, primitive.control);
      const SkPoint controlEndMidpoint =
          Midpoint(primitive.control, primitive.end);
      const float minimumX =
          std::min({primitive.start.x(), startControlMidpoint.x(),
                    controlEndMidpoint.x(), primitive.end.x()}) -
          distanceLimit;
      const float maximumX =
          std::max({primitive.start.x(), startControlMidpoint.x(),
                    controlEndMidpoint.x(), primitive.end.x()}) +
          distanceLimit;
      const float minimumY =
          std::min({primitive.start.y(), startControlMidpoint.y(),
                    controlEndMidpoint.y(), primitive.end.y()}) -
          distanceLimit;
      const float maximumY =
          std::max({primitive.start.y(), startControlMidpoint.y(),
                    controlEndMidpoint.y(), primitive.end.y()}) +
          distanceLimit;
      const std::array<SkPoint, 6> originalCorners =
          coreTextYUp
              ? std::array<SkPoint, 6>{SkPoint{minimumX, minimumY},
                                       SkPoint{maximumX, minimumY},
                                       SkPoint{maximumX, maximumY},
                                       SkPoint{minimumX, minimumY},
                                       SkPoint{maximumX, maximumY},
                                       SkPoint{minimumX, maximumY}}
              : std::array<SkPoint, 6>{
                    SkPoint{minimumX, maximumY},
                    SkPoint{maximumX, maximumY},
                    SkPoint{maximumX, minimumY},
                    SkPoint{minimumX, maximumY},
                    SkPoint{maximumX, minimumY},
                    SkPoint{minimumX, minimumY}};
      const float qtLimitBegin =
          coreTextYUp ? primitive.canonicalLimitBegin
                      : -primitive.canonicalLimitEnd;
      const float qtLimitEnd =
          coreTextYUp ? primitive.canonicalLimitEnd
                      : -primitive.canonicalLimitBegin;
      for (const auto point : originalCorners) {
        SkPoint canonicalPosition;
        if (coreTextYUp) {
          canonicalPosition =
              QtCanonicalPrimitivePosition(point, primitive);
        } else {
          const SkVector delta{point.x() - primitive.start.x(),
                               point.y() - primitive.start.y()};
          const float canonicalX =
              SkPoint::DotProduct(delta, primitive.canonicalX) +
              primitive.canonicalOffset.x();
          const float canonicalY =
              SkPoint::DotProduct(delta, primitive.canonicalY) +
              primitive.canonicalOffset.y();
          canonicalPosition = {-canonicalX, canonicalY};
        }
        appendDistanceVertex(qtPosition(point), canonicalPosition,
                             qtLimitBegin, qtLimitEnd,
                             primitive.inverseCanonicalScale);
      }
    } else if (coreTextYUp) {
      const float minimumX =
          std::min(primitive.start.x(), primitive.end.x()) - distanceLimit;
      const float maximumX =
          std::max(primitive.start.x(), primitive.end.x()) + distanceLimit;
      const float minimumY =
          std::min(primitive.start.y(), primitive.end.y()) - distanceLimit;
      const float maximumY =
          std::max(primitive.start.y(), primitive.end.y()) + distanceLimit;
      const std::array<SkPoint, 6> originalCorners{
          SkPoint{minimumX, minimumY}, SkPoint{maximumX, minimumY},
          SkPoint{maximumX, maximumY}, SkPoint{minimumX, minimumY},
          SkPoint{maximumX, maximumY}, SkPoint{minimumX, maximumY}};
      for (const auto point : originalCorners) {
        appendDistanceVertex(
            qtPosition(point),
            QtCanonicalPrimitivePosition(point, primitive),
            primitive.canonicalLimitBegin, primitive.canonicalLimitEnd,
            primitive.inverseCanonicalScale);
      }
    } else {
      const auto startPixels = qtPixelPosition(primitive.start);
      const auto endPixels = qtPixelPosition(primitive.end);
      const SkVector pixelSegment{endPixels.x() - startPixels.x(),
                                  endPixels.y() - startPixels.y()};
      const float pixelLength = pixelSegment.length();
      if (!(pixelLength > 0.0F) || !std::isfinite(pixelLength)) {
        previousEnd = primitive.end;
        continue;
      }
      const SkVector direction{pixelSegment.x() / pixelLength,
                               pixelSegment.y() / pixelLength};
      const SkVector perpendicular{-direction.y(), direction.x()};
      const SkPoint midpoint = Midpoint(startPixels, endPixels);
      const float halfPixelLength = pixelLength * 0.5F;
      const float distanceScale =
          halfPixelLength / kTextSdfLineCanonicalHalfExtent;
      const float canonicalScale = 1.0F / distanceScale;
      const float minimumX =
          std::min(startPixels.x(), endPixels.x()) - distanceLimit;
      const float maximumX =
          std::max(startPixels.x(), endPixels.x()) + distanceLimit;
      const float minimumY =
          std::min(startPixels.y(), endPixels.y()) - distanceLimit;
      const float maximumY =
          std::max(startPixels.y(), endPixels.y()) + distanceLimit;
      const std::array<SkPoint, 6> qtCorners{
          SkPoint{minimumX, minimumY}, SkPoint{maximumX, minimumY},
          SkPoint{maximumX, maximumY}, SkPoint{minimumX, minimumY},
          SkPoint{maximumX, maximumY}, SkPoint{minimumX, maximumY}};
      for (const auto point : qtCorners) {
        const SkVector delta{point.x() - midpoint.x(),
                             point.y() - midpoint.y()};
        const SkPoint parabola{
            SkPoint::DotProduct(delta, direction) * canonicalScale,
            SkPoint::DotProduct(delta, perpendicular) * canonicalScale};
        const SkPoint normalized{point.x() / targetWidth,
                                 point.y() / targetHeight};
        appendDistanceVertex(normalized, parabola,
                             -kTextSdfLineCanonicalHalfExtent,
                             kTextSdfLineCanonicalHalfExtent,
                             distanceScale);
      }
    }

    appendShapeVertex(contourAnchor, 0.0F, 1.0F);
    appendShapeVertex(primitive.start, 0.0F, 1.0F);
    appendShapeVertex(primitive.end, 0.0F, 1.0F);
    if (primitive.kind == SdfOutlinePrimitiveKind::Quadratic) {
      appendShapeVertex(primitive.start, -1.0F, 1.0F);
      appendShapeVertex(primitive.control, 0.0F, -1.0F);
      appendShapeVertex(primitive.end, 1.0F, 1.0F);
    }
    previousEnd = primitive.end;
  }
  return mesh;
}

[[nodiscard]] bool FiniteRect(const SkRect &rect) noexcept {
  return std::isfinite(rect.left()) && std::isfinite(rect.top()) &&
         std::isfinite(rect.right()) && std::isfinite(rect.bottom());
}

[[nodiscard]] bool
ValidateRequest(const TextSdfMeshBuildRequest &request,
                std::string &error) {
  if (request.glyphIdentity.stableUnitId == 0U ||
      request.glyphIdentity.paragraphId.empty() ||
      request.glyphIdentity.runId.empty()) {
    error = "text SDF mesh request has no shaped glyph identity";
    return false;
  }
  if (request.path == nullptr || request.path->isEmpty()) {
    error = "text SDF mesh request has no outline";
    return false;
  }
  if (!request.path->isFinite()) {
    error = "text SDF mesh request has a non-finite outline";
    return false;
  }
  if (request.path->getFillType() != SkPathFillType::kWinding) {
    error = "text SDF mesh request requires a finite non-zero winding fill";
    return false;
  }
  if (request.path->countVerbs() == 0U ||
      request.path->countVerbs() > kMaximumPathVerbs) {
    error = "text SDF outline exceeds the path verb budget";
    return false;
  }
  if (request.atlasWidth <= 0 || request.atlasHeight <= 0 ||
      request.atlasWidth > kMaximumAtlasExtent ||
      request.atlasHeight > kMaximumAtlasExtent) {
    error = "text SDF mesh request has invalid atlas dimensions";
    return false;
  }
  if (!FiniteRect(request.sourceBounds) ||
      !(request.sourceBounds.left() < request.sourceBounds.right()) ||
      !(request.sourceBounds.top() < request.sourceBounds.bottom()) ||
      request.sourceBounds.left() < 0.0F ||
      request.sourceBounds.top() < 0.0F ||
      request.sourceBounds.right() > static_cast<float>(request.atlasWidth) ||
      request.sourceBounds.bottom() >
          static_cast<float>(request.atlasHeight)) {
    error = "text SDF mesh request has invalid top-left source bounds";
    return false;
  }
  if (!std::isfinite(request.rasterDistanceRange) ||
      request.rasterDistanceRange < kMinimumDistanceRange ||
      request.rasterDistanceRange > kMaximumDistanceMetric) {
    error = "text SDF mesh request has invalid raster distance range";
    return false;
  }
  if (request.pathCoordinateSystem !=
          QtTextGlyphOutlineCoordinateSystem::RendererYDown &&
      request.pathCoordinateSystem !=
          QtTextGlyphOutlineCoordinateSystem::CoreTextYUp) {
    error = "text SDF mesh request has an invalid outline coordinate system";
    return false;
  }

  const SkRect pathBounds = request.path->computeTightBounds();
  if (!FiniteRect(pathBounds) || !(pathBounds.left() < pathBounds.right()) ||
      !(pathBounds.top() < pathBounds.bottom())) {
    error = "text SDF mesh request has degenerate outline bounds";
    return false;
  }
  const float storedRange = std::floor(request.rasterDistanceRange);
  if (pathBounds.left() - request.sourceBounds.left() +
              kContainmentTolerance <
          storedRange ||
      pathBounds.top() - request.sourceBounds.top() +
              kContainmentTolerance <
          storedRange ||
      request.sourceBounds.right() - pathBounds.right() +
              kContainmentTolerance <
          storedRange ||
      request.sourceBounds.bottom() - pathBounds.bottom() +
              kContainmentTolerance <
          storedRange) {
    error =
        "text SDF source bounds do not preserve the raster distance range";
    return false;
  }
  return true;
}

[[nodiscard]] bool ValidateMesh(const TextSdfGpuMesh &mesh,
                                std::string &error) {
  if (mesh.targetWidth <= 0 || mesh.targetHeight <= 0 ||
      mesh.targetWidth > kMaximumAtlasExtent ||
      mesh.targetHeight > kMaximumAtlasExtent || mesh.outputX < 0 ||
      mesh.outputY < 0 || mesh.outputWidth <= 0 || mesh.outputHeight <= 0 ||
      mesh.outputX + mesh.outputWidth > mesh.targetWidth ||
      mesh.outputY + mesh.outputHeight > mesh.targetHeight) {
    error = "text SDF exact mesh has invalid target or output bounds";
    return false;
  }
  if (mesh.distanceVertices.empty() || mesh.shapeVertices.empty() ||
      mesh.distanceVertices.size() % 3U != 0U ||
      mesh.shapeVertices.size() % 3U != 0U ||
      mesh.distanceVertices.size() > kMaximumTriangleVertices ||
      mesh.shapeVertices.size() > kMaximumTriangleVertices) {
    error = "text SDF exact mesh violates the triangle budget";
    return false;
  }
  // Distance quads cover the quadratic control hull, which can extend by a
  // fraction of a pixel beyond the tight outline bounds used to pack a glyph
  // cell. Metal clips that bounded coverage at the atlas viewport; shape
  // vertices and every non-position field retain their strict contract.
  for (std::size_t index = 0U; index < mesh.distanceVertices.size(); ++index) {
    const auto &vertex = mesh.distanceVertices[index];
    const auto reject = [&](const std::string_view field, const float value) {
      error = "text SDF exact distance vertex is outside the Metal contract: " +
              std::string(field) + "=" + std::to_string(value) +
              ", vertex=" + std::to_string(index);
      return false;
    };
    if (!IsTextSdfDistanceAtlasPosition(vertex.positionX, mesh.targetWidth))
      return reject("position_x", vertex.positionX);
    if (!IsTextSdfDistanceAtlasPosition(vertex.positionY, mesh.targetHeight))
      return reject("position_y", vertex.positionY);
    if (!std::isfinite(vertex.parabolaX))
      return reject("parabola_x", vertex.parabolaX);
    if (!std::isfinite(vertex.parabolaY))
      return reject("parabola_y", vertex.parabolaY);
    if (!std::isfinite(vertex.limitBegin))
      return reject("limit_begin", vertex.limitBegin);
    if (!std::isfinite(vertex.limitEnd))
      return reject("limit_end", vertex.limitEnd);
    if (!std::isfinite(vertex.distanceScale) ||
        !(vertex.distanceScale > 0.0F))
      return reject("distance_scale", vertex.distanceScale);
    if (!std::isfinite(vertex.distanceLimit) ||
        !(vertex.distanceLimit > 0.0F))
      return reject("distance_limit", vertex.distanceLimit);
  }
  for (std::size_t index = 0U; index < mesh.shapeVertices.size(); ++index) {
    const auto &vertex = mesh.shapeVertices[index];
    const auto reject = [&](const std::string_view field, const float value) {
      error = "text SDF exact shape vertex is outside the Metal contract: " +
              std::string(field) + "=" + std::to_string(value) +
              ", vertex=" + std::to_string(index);
      return false;
    };
    if (!IsTextSdfNormalizedAtlasPosition(vertex.positionX))
      return reject("position_x", vertex.positionX);
    if (!IsTextSdfNormalizedAtlasPosition(vertex.positionY))
      return reject("position_y", vertex.positionY);
    if (!std::isfinite(vertex.parabolaX))
      return reject("parabola_x", vertex.parabolaX);
    if (!std::isfinite(vertex.parabolaY))
      return reject("parabola_y", vertex.parabolaY);
  }
  return true;
}

} // namespace

bool BuildTextSdfGpuMesh(const TextSdfMeshBuildRequest &request,
                         TextSdfGpuMesh &mesh, std::string &error) {
  mesh = {};
  error.clear();
  if (!ValidateRequest(request, error))
    return false;

  // The typed request currently carries an already atlas-positioned path.
  // Rebuild the verb stream through the recovered scalar transform so this
  // boundary cannot silently switch back to SkMatrix/FMA path mapping.
  // Font outlines may contain overlapping stroke components. Their internal
  // seams are not glyph boundaries and must not contribute distance ramps.
  // Normalize once when building the cached atlas, before emitting GPU meshes.
  const auto silhouette = Simplify(*request.path);
  if (!silhouette || !silhouette->isFinite()) {
    error = "text SDF glyph silhouette could not be resolved";
    return false;
  }
  // PathOps may return even-odd contours. The Metal stencil accumulates
  // non-zero winding, so orient nested counters before emitting triangles;
  // merely relabelling the fill type fills holes such as the bowl of R.
  const auto windingSilhouette = AsWinding(*silhouette);
  if (!windingSilhouette || !windingSilhouette->isFinite() ||
      windingSilhouette->getFillType() != SkPathFillType::kWinding) {
    error = "text SDF glyph silhouette winding could not be resolved";
    return false;
  }
  const SkPath canonicalPath =
      ScaleTranslateTextSdfPathBinary32(*windingSilhouette, 1.0F, 1.0F, 0.0F, 0.0F);
  auto primitives = BuildGlyphOutlinePrimitives(canonicalPath);
  if (primitives.empty()) {
    error = "text SDF vector outline has no drawable primitives";
    return false;
  }
  if (primitives.size() > kMaximumOutlinePrimitives ||
      primitives.size() > kMaximumTriangleVertices / 6U) {
    error = "text SDF vector outline exceeds the primitive budget";
    return false;
  }

  // The request atlas is the already-packed shared target. sourceBounds is
  // one continuous TextPro cell within it; recover only that cell's discrete
  // storage extent for BuildNativeTextSdfMesh's width/height arguments. The
  // placement output remains the full shared atlas exactly as in 80c9's
  // deferred-native-vector path.
  const int cellWidth = static_cast<int>(
      std::ceil(static_cast<double>(request.sourceBounds.width())));
  const int cellHeight = static_cast<int>(
      std::ceil(static_cast<double>(request.sourceBounds.height())));
  const TextSdfMeshPlacement placement{
      request.atlasWidth,
      request.atlasHeight,
      request.sourceBounds.left() + request.rasterDistanceRange,
      request.sourceBounds.top() + request.rasterDistanceRange,
      request.sourceBounds.left() + request.rasterDistanceRange,
      request.sourceBounds.top() + request.rasterDistanceRange,
      0,
      0,
      request.atlasWidth,
      request.atlasHeight,
  };
  TextSdfGpuMesh result = BuildNativeTextSdfMesh(
      cellWidth, cellHeight, request.rasterDistanceRange,
      canonicalPath.computeTightBounds(),
      request.pathCoordinateSystem, primitives, &placement);
  result.glyphIdentity = request.glyphIdentity;
  if (!ValidateMesh(result, error))
    return false;

  mesh = std::move(result);
  return true;
}

} // namespace videocut::skia_runtime::internal
