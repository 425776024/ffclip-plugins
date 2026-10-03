#include "text/QtTextGlyphOutlineRuntime.h"

#include "include/ports/SkTypeface_mac.h"

#include <CoreGraphics/CoreGraphics.h>
#include <CoreText/CoreText.h>

#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace videocut::skia_runtime::internal {
namespace {

struct RawPathElement final {
  std::uint32_t type{0U};
  std::vector<CGPoint> points;
};

struct PathSink final {
  SkPathBuilder builder;
  bool started{false};
  CGPoint current{CGPointZero};
  std::vector<RawPathElement> rawElements;
};

bool SamePoint(const CGPoint left, const CGPoint right) {
  return left.x == right.x && left.y == right.y;
}

void BeginIfNeeded(PathSink &sink) {
  if (sink.started)
    return;
  sink.builder.moveTo(static_cast<float>(sink.current.x),
                      static_cast<float>(sink.current.y));
  sink.started = true;
}

void AppendPathElement(void *context, const CGPathElement *element) {
  auto &sink = *static_cast<PathSink *>(context);
  const CGPoint *points = element->points;
  std::size_t pointCount = 0U;
  switch (element->type) {
  case kCGPathElementMoveToPoint:
  case kCGPathElementAddLineToPoint:
    pointCount = 1U;
    break;
  case kCGPathElementAddQuadCurveToPoint:
    pointCount = 2U;
    break;
  case kCGPathElementAddCurveToPoint:
    pointCount = 3U;
    break;
  case kCGPathElementCloseSubpath:
    break;
  }
  RawPathElement raw;
  raw.type = static_cast<std::uint32_t>(element->type);
  if (pointCount > 0U)
    raw.points.assign(points, points + pointCount);
  sink.rawElements.push_back(std::move(raw));
  switch (element->type) {
  case kCGPathElementMoveToPoint:
    sink.started = false;
    sink.current = points[0];
    return;
  case kCGPathElementAddLineToPoint:
    if (!SamePoint(sink.current, points[0])) {
      BeginIfNeeded(sink);
      sink.current = points[0];
      sink.builder.lineTo(static_cast<float>(points[0].x),
                          static_cast<float>(points[0].y));
    }
    return;
  case kCGPathElementAddQuadCurveToPoint:
    if (!SamePoint(sink.current, points[0]) ||
        !SamePoint(sink.current, points[1])) {
      BeginIfNeeded(sink);
      sink.current = points[1];
      sink.builder.quadTo(
          static_cast<float>(points[0].x), static_cast<float>(points[0].y),
          static_cast<float>(points[1].x), static_cast<float>(points[1].y));
    }
    return;
  case kCGPathElementAddCurveToPoint:
    if (!SamePoint(sink.current, points[0]) ||
        !SamePoint(sink.current, points[1]) ||
        !SamePoint(sink.current, points[2])) {
      BeginIfNeeded(sink);
      sink.current = points[2];
      sink.builder.cubicTo(
          static_cast<float>(points[0].x), static_cast<float>(points[0].y),
          static_cast<float>(points[1].x), static_cast<float>(points[1].y),
          static_cast<float>(points[2].x), static_cast<float>(points[2].y));
    }
    return;
  case kCGPathElementCloseSubpath:
    if (sink.started)
      sink.builder.close();
    return;
  }
}

void DumpCoreTextPathDiagnostic(const QtTextGlyphOutlineRequest &request,
                                const PathSink &sink) {
  const char *directory =
      std::getenv("VIDEOCUT_DUMP_TEXT_CORETEXT_PATH_DIR");
  if (directory == nullptr || directory[0] == '\0')
    return;
  static std::atomic<std::uint32_t> ordinal{0U};
  const std::uint32_t current = ordinal.fetch_add(1U);
  const std::string path =
      std::string(directory) + "/text-coretext-path-" +
      std::to_string(current) + "-glyph-" +
      std::to_string(static_cast<unsigned>(request.glyph)) + ".bin";
  std::FILE *file = std::fopen(path.c_str(), "wb");
  if (file == nullptr)
    return;
  const std::uint32_t magic = 0x56435450U; // "VCTP"
  const std::uint32_t version = 1U;
  const std::uint32_t glyph = request.glyph;
  const std::uint32_t elementCount =
      static_cast<std::uint32_t>(sink.rawElements.size());
  std::fwrite(&magic, sizeof(magic), 1U, file);
  std::fwrite(&version, sizeof(version), 1U, file);
  std::fwrite(&glyph, sizeof(glyph), 1U, file);
  std::fwrite(&request.pointSize, sizeof(request.pointSize), 1U, file);
  std::fwrite(&elementCount, sizeof(elementCount), 1U, file);
  for (const auto &element : sink.rawElements) {
    const std::uint32_t pointCount =
        static_cast<std::uint32_t>(element.points.size());
    std::fwrite(&element.type, sizeof(element.type), 1U, file);
    std::fwrite(&pointCount, sizeof(pointCount), 1U, file);
    for (const auto point : element.points) {
      const double x = static_cast<double>(point.x);
      const double y = static_cast<double>(point.y);
      const float x32 = static_cast<float>(point.x);
      const float y32 = static_cast<float>(point.y);
      std::fwrite(&x, sizeof(x), 1U, file);
      std::fwrite(&y, sizeof(y), 1U, file);
      std::fwrite(&x32, sizeof(x32), 1U, file);
      std::fwrite(&y32, sizeof(y32), 1U, file);
    }
  }
  std::fclose(file);
}

} // namespace

bool BuildQtTextGlyphOutlinePath(const QtTextGlyphOutlineRequest &request,
                                 QtTextGlyphOutlineResult &result,
                                 std::string &error) {
  result = QtTextGlyphOutlineResult{};
  if (request.implementationVersion !=
      kQtTextGlyphOutlineImplementationVersion) {
    error = "unsupported Qt TextPro glyph-outline implementation version";
    return false;
  }
  if (request.typeface == nullptr || !std::isfinite(request.pointSize) ||
      request.pointSize <= 0.0F) {
    error = "Qt TextPro glyph-outline request is invalid";
    return false;
  }
  CTFontRef source = SkTypeface_GetCTFontRef(request.typeface);
  if (source == nullptr) {
    error = "concrete SkTypeface has no CoreText font handle";
    return false;
  }
  CTFontRef font = CTFontCreateCopyWithAttributes(
      source, static_cast<CGFloat>(request.pointSize), nullptr, nullptr);
  if (font == nullptr) {
    error = "CoreText point-size font creation failed";
    return false;
  }
  CGPathRef coreTextPath = CTFontCreatePathForGlyph(
      font, static_cast<CGGlyph>(request.glyph), nullptr);
  CFRelease(font);
  if (coreTextPath == nullptr || CGPathIsEmpty(coreTextPath)) {
    if (coreTextPath != nullptr)
      CGPathRelease(coreTextPath);
    error = "CoreText glyph outline is empty";
    return false;
  }
  PathSink sink;
  CGPathApply(coreTextPath, &sink, AppendPathElement);
  CGPathRelease(coreTextPath);
  DumpCoreTextPathDiagnostic(request, sink);
  result.path = sink.builder.detach();
  if (result.path.isEmpty()) {
    error = "CoreText glyph outline conversion produced an empty path";
    return false;
  }
  result.implementationVersion = request.implementationVersion;
  result.coordinateSystem = QtTextGlyphOutlineCoordinateSystem::CoreTextYUp;
  error.clear();
  return true;
}

} // namespace videocut::skia_runtime::internal
