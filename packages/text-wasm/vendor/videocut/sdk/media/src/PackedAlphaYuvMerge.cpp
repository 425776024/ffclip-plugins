#include "videocut/media/PackedAlphaYuvMerge.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <new>
#include <string>
#include <utility>

namespace videocut::media {
namespace {

constexpr std::size_t kMaximumExactlyRepresentableFloatInteger = 1U << 24U;

bool Reject(std::string message, std::string &error) {
  error = std::move(message);
  return false;
}

bool CheckedMultiply(const std::size_t lhs, const std::size_t rhs,
                     std::size_t &product) noexcept {
  product = 0U;
  if (rhs != 0U && lhs > std::numeric_limits<std::size_t>::max() / rhs)
    return false;
  product = lhs * rhs;
  return true;
}

bool ValidatePlane(const PackedAlphaYuvPlaneView &plane, const char *name,
                   std::string &error) {
  if (!plane.bytes)
    return Reject(std::string(name) + " plane bytes are null", error);
  if (plane.logicalWidth == 0U || plane.logicalHeight == 0U)
    return Reject(std::string(name) + " plane dimensions are zero", error);
  if (plane.rowBytes < plane.logicalWidth)
    return Reject(std::string(name) + " plane stride is smaller than width",
                  error);
  // Strides participate in float32 normalized-coordinate calculations.
  if (plane.rowBytes > kMaximumExactlyRepresentableFloatInteger ||
      static_cast<std::size_t>(static_cast<float>(plane.rowBytes)) !=
          plane.rowBytes) {
    return Reject(std::string(name) +
                      " plane stride is not exactly representable as float32",
                  error);
  }
  if (plane.logicalHeight > kMaximumExactlyRepresentableFloatInteger ||
      static_cast<std::uint32_t>(static_cast<float>(plane.logicalHeight)) !=
          plane.logicalHeight) {
    return Reject(std::string(name) +
                      " plane height is not exactly representable as float32",
                  error);
  }
  std::size_t requiredBytes = 0U;
  if (!CheckedMultiply(plane.rowBytes, plane.logicalHeight, requiredBytes))
    return Reject(std::string(name) + " plane byte size overflows", error);
  if (plane.byteSize != requiredBytes)
    return Reject(std::string(name) +
                      " plane byte size does not exactly match stride*height",
                  error);
  return true;
}

float SampleLinearClamp(const PackedAlphaYuvPlaneView &plane,
                        const float normalizedX,
                        const float normalizedY) noexcept {
  // Normalized linear sampling places texel centers at (i+0.5)/size.
  // Clamp each footprint index to match clamp-to-edge sampling.
  const float sourceX = normalizedX * static_cast<float>(plane.rowBytes) - 0.5F;
  const float sourceY =
      normalizedY * static_cast<float>(plane.logicalHeight) - 0.5F;
  const float floorX = std::floor(sourceX);
  const float floorY = std::floor(sourceY);
  const float fractionX = sourceX - floorX;
  const float fractionY = sourceY - floorY;

  const auto clampX = [&plane](const float value) noexcept {
    return static_cast<std::size_t>(
        std::clamp(value, 0.0F, static_cast<float>(plane.rowBytes - 1U)));
  };
  const auto clampY = [&plane](const float value) noexcept {
    return static_cast<std::size_t>(
        std::clamp(value, 0.0F, static_cast<float>(plane.logicalHeight - 1U)));
  };
  const std::size_t x0 = clampX(floorX);
  const std::size_t x1 = clampX(floorX + 1.0F);
  const std::size_t y0 = clampY(floorY);
  const std::size_t y1 = clampY(floorY + 1.0F);
  const auto value = [&plane](const std::size_t x,
                              const std::size_t y) noexcept {
    return static_cast<float>(plane.bytes[y * plane.rowBytes + x]) / 255.0F;
  };
  const float top = value(x0, y0) + (value(x1, y0) - value(x0, y0)) * fractionX;
  const float bottom =
      value(x0, y1) + (value(x1, y1) - value(x0, y1)) * fractionX;
  return top + (bottom - top) * fractionY;
}

struct Rgb final {
  float red{0.0F};
  float green{0.0F};
  float blue{0.0F};
};

Rgb ConvertYuv(const PackedAlphaYuvConversion &conversion, const float y,
               const float u, const float v) noexcept {
  const float shiftedY = y - conversion.offsets[0];
  const float shiftedU = u - conversion.offsets[1];
  const float shiftedV = v - conversion.offsets[2];
  const auto &matrix = conversion.matrix;
  return {
      (matrix[0] * shiftedY + matrix[3] * shiftedU) + matrix[6] * shiftedV,
      (matrix[1] * shiftedY + matrix[4] * shiftedU) + matrix[7] * shiftedV,
      (matrix[2] * shiftedY + matrix[5] * shiftedU) + matrix[8] * shiftedV,
  };
}

std::uint8_t QuantizeUnorm8(const float value) noexcept {
  const float encoded = std::clamp(value, 0.0F, 1.0F) * 255.0F;
  return static_cast<std::uint8_t>(std::lround(encoded));
}

} // namespace

bool ConvertPlanarYuv420pRgba8(const PlanarYuvRgbaRequest &request,
                              PlanarYuvRgbaResult &output,
                              std::string &error) {
  output = {};
  error.clear();
  if (request.layout != PlanarYuvRgbaLayout::Opaque &&
      request.layout != PlanarYuvRgbaLayout::AlphaAndColor) {
    return Reject("planar YUV output layout is invalid", error);
  }
  const bool packed = request.layout == PlanarYuvRgbaLayout::AlphaAndColor;
  if (!ValidatePlane(request.y, "Y", error) ||
      !ValidatePlane(request.u, "U", error) ||
      !ValidatePlane(request.v, "V", error)) {
    return false;
  }
  if ((request.y.logicalWidth & 1U) != 0U ||
      (request.y.logicalHeight & 1U) != 0U) {
    return Reject("packed-alpha YUV dimensions must have even width and height",
                  error);
  }
  if (request.u.logicalWidth != request.y.logicalWidth / 2U ||
      request.v.logicalWidth != request.u.logicalWidth ||
      request.u.logicalHeight != request.y.logicalHeight / 2U ||
      request.v.logicalHeight != request.u.logicalHeight) {
    return Reject("packed-alpha YUV planes are not planar 4:2:0", error);
  }
  // Both chroma planes share the same normalized horizontal range.
  if (request.u.rowBytes != request.v.rowBytes)
    return Reject("packed-alpha U/V strides must match", error);
  for (const float value : request.conversion.matrix) {
    if (!std::isfinite(value))
      return Reject("packed-alpha YUV matrix is not finite", error);
  }
  for (const float value : request.conversion.offsets) {
    if (!std::isfinite(value))
      return Reject("packed-alpha YUV offsets are not finite", error);
  }

  const std::uint32_t outputWidth =
      packed ? request.y.logicalWidth / 2U : request.y.logicalWidth;
  const std::uint32_t outputHeight = request.y.logicalHeight;
  std::size_t outputRowBytes = 0U;
  if (!CheckedMultiply(static_cast<std::size_t>(outputWidth), 4U,
                       outputRowBytes)) {
    return Reject("packed-alpha output row size overflows", error);
  }
  std::size_t outputByteSize = 0U;
  if (!CheckedMultiply(outputRowBytes, static_cast<std::size_t>(outputHeight),
                       outputByteSize)) {
    return Reject("packed-alpha output byte size overflows", error);
  }

  std::vector<std::uint8_t> rgba8;
  try {
    rgba8.resize(outputByteSize);
  } catch (const std::bad_alloc &) {
    return Reject("packed-alpha output allocation failed", error);
  } catch (...) {
    return Reject("packed-alpha output allocation threw", error);
  }

  const float yStride = static_cast<float>(request.y.rowBytes);
  const float yWidth = static_cast<float>(request.y.logicalWidth);
  const float yPixelWidth = 1.0F / yStride;
  const float yRange = packed ? ((yWidth / yStride) / 2.0F) - yPixelWidth
                              : (yWidth / yStride) - yPixelWidth;
  const float uvStride = static_cast<float>(request.u.rowBytes);
  const float uvWidth = static_cast<float>(request.u.logicalWidth);
  const float uvPixelWidth = 1.0F / uvStride;
  const float uvRange = packed ? ((uvWidth / uvStride) / 2.0F) - uvPixelWidth
                               : (uvWidth / uvStride) - uvPixelWidth;

  for (std::uint32_t y = 0U; y < outputHeight; ++y) {
    const float rowCenter =
        (static_cast<float>(y) + 0.5F) / static_cast<float>(outputHeight);
    const float texCoordY = request.flipY ? 1.0F - rowCenter : rowCenter;
    auto *outputRow =
        rgba8.data() + static_cast<std::size_t>(y) * outputRowBytes;
    for (std::uint32_t x = 0U; x < outputWidth; ++x) {
      const float texCoordX =
          (static_cast<float>(x) + 0.5F) / static_cast<float>(outputWidth);
      const float yLowX = texCoordX * yRange;
      const float uvLowX = texCoordX * uvRange;
      float yRgbX = yLowX;
      float uvRgbX = uvLowX;
      float yAlphaX = yLowX;
      float uvAlphaX = uvLowX;
      if (packed) {
        const float yHighX = ((texCoordX * yRange) + yRange) + yPixelWidth;
        const float uvHighX = ((texCoordX * uvRange) + uvRange) + uvPixelWidth;
        yRgbX = request.alphaFirst ? yHighX : yLowX;
        uvRgbX = request.alphaFirst ? uvHighX : uvLowX;
        yAlphaX = request.alphaFirst ? yLowX : yHighX;
        uvAlphaX = request.alphaFirst ? uvLowX : uvHighX;
      }

      const Rgb rgb = ConvertYuv(
          request.conversion, SampleLinearClamp(request.y, yRgbX, texCoordY),
          SampleLinearClamp(request.u, uvRgbX, texCoordY),
          SampleLinearClamp(request.v, uvRgbX, texCoordY));
      float alpha = 1.0F;
      if (packed) {
        const Rgb alphaColor = ConvertYuv(
            request.conversion, SampleLinearClamp(request.y, yAlphaX, texCoordY),
            SampleLinearClamp(request.u, uvAlphaX, texCoordY),
            SampleLinearClamp(request.v, uvAlphaX, texCoordY));
        // Keep the float average before the final UNORM8 write. The color
        // region's RGB is never multiplied by this recovered alpha.
        alpha = ((alphaColor.red + alphaColor.green) + alphaColor.blue) / 3.0F;
      }
      if (!std::isfinite(rgb.red) || !std::isfinite(rgb.green) ||
          !std::isfinite(rgb.blue) || !std::isfinite(alpha)) {
        return Reject("packed-alpha YUV conversion produced non-finite color",
                      error);
      }

      auto *pixel = outputRow + static_cast<std::size_t>(x) * 4U;
      pixel[0] = QuantizeUnorm8(rgb.red);
      pixel[1] = QuantizeUnorm8(rgb.green);
      pixel[2] = QuantizeUnorm8(rgb.blue);
      pixel[3] = QuantizeUnorm8(alpha);
    }
  }

  output.rgba8 = std::move(rgba8);
  output.width = outputWidth;
  output.height = outputHeight;
  output.rowBytes = outputRowBytes;
  return true;
}

bool MergePackedAlphaYuv420p(const PackedAlphaYuvMergeRequest &request,
                            PackedAlphaYuvMergeResult &output,
                            std::string &error) {
  PlanarYuvRgbaRequest planar;
  planar.y = request.y;
  planar.u = request.u;
  planar.v = request.v;
  planar.conversion = request.conversion;
  planar.layout = PlanarYuvRgbaLayout::AlphaAndColor;
  planar.alphaFirst = request.alphaFirst;
  planar.flipY = request.flipY;
  return ConvertPlanarYuv420pRgba8(planar, output, error);
}

} // namespace videocut::media
