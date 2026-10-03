#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace videocut::media {

struct PackedAlphaYuvPlaneView final {
  // Row zero is the decoded top row. rowBytes is also the sampled texture
  // width, so byteSize includes row padding and equals rowBytes*logicalHeight.
  const std::uint8_t *bytes{nullptr};
  std::size_t byteSize{0U};
  std::uint32_t logicalWidth{0U};
  std::uint32_t logicalHeight{0U};
  std::size_t rowBytes{0U};
};

struct PackedAlphaYuvConversion final {
  // RGB = matrix * (YUV - offsets); matrix is column-major.
  std::array<float, 9> matrix{};
  std::array<float, 3> offsets{};
};

struct PackedAlphaYuvMergeRequest final {
  PackedAlphaYuvPlaneView y;
  PackedAlphaYuvPlaneView u;
  PackedAlphaYuvPlaneView v;
  PackedAlphaYuvConversion conversion;
  bool alphaFirst{true};
  bool flipY{false};
};

struct PackedAlphaYuvMergeResult final {
  std::vector<std::uint8_t> rgba8;
  std::uint32_t width{0U};
  std::uint32_t height{0U};
  std::size_t rowBytes{0U};
};

enum class PlanarYuvRgbaLayout : std::uint8_t {
  Opaque,
  AlphaAndColor,
};

struct PlanarYuvRgbaRequest final {
  PackedAlphaYuvPlaneView y;
  PackedAlphaYuvPlaneView u;
  PackedAlphaYuvPlaneView v;
  PackedAlphaYuvConversion conversion;
  PlanarYuvRgbaLayout layout{PlanarYuvRgbaLayout::Opaque};
  bool alphaFirst{true};
  bool flipY{false};
};

using PlanarYuvRgbaResult = PackedAlphaYuvMergeResult;

// Converts even-sized YUV420p using normalized linear/clamp sampling and one
// pixel subtracted from each plane's horizontal sampling range. Opaque uses
// the full plane width and writes alpha=1; AlphaAndColor uses two horizontal
// regions and the converted RGB mean as alpha. No view is retained, no
// transfer function is applied, and RGB is never multiplied by alpha.
// On failure output is reset and error is set.
[[nodiscard]] bool ConvertPlanarYuv420pRgba8(
    const PlanarYuvRgbaRequest &request,
    PlanarYuvRgbaResult &output, std::string &error);

// Synchronously merges two horizontal regions of an owned-by-caller YUV420p
// frame. The physical width and height must be even; each output dimension
// is W/2 by H. Each plane uses normalized linear/clamp sampling with a
// one-pixel inset in its half-width range. Alpha is the converted RGB mean
// before UNORM8 quantization. RGB is copied without multiplying by alpha.
// flipY reverses the normalized vertical sampling coordinate for both halves.
// No input view is retained. On failure output is reset and error is set.
[[nodiscard]] bool MergePackedAlphaYuv420p(
    const PackedAlphaYuvMergeRequest &request,
    PackedAlphaYuvMergeResult &output, std::string &error);

} // namespace videocut::media
