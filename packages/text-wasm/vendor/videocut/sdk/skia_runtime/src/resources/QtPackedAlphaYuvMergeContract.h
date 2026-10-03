#pragma once

#include "videocut/media/PackedAlphaYuvMerge.h"

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace videocut::skia_runtime::internal {

inline constexpr std::uint32_t kQtPackedAlphaYuvMergeImplementationVersion = 1U;
inline constexpr std::uint32_t kQtPackedAlphaYuvMergeContractSchemaVersion = 1U;
inline constexpr char kQtPackedAlphaYuvMergeImplementationId[] =
    "videocut.qt-video-anim-seq-packed-alpha-yuv420p-reference";

// These are the exact runtime MSL sources selected by diagnostics[5]/draws[1]
// in the captured VideoAnimSeq render result under
// work/text-bubble-flower-presets/diagnostics. They are intentionally part of
// the contract identity: this helper is a golden reference for that draw, not
// a generic YUV420 converter.
inline constexpr char kQtPackedAlphaYuvMergeVertexSourceSha256[] =
    "aebca5455d91c590a3697cee515bb1f53c92f5cfe290bf759fc5ede165c0d86f";
inline constexpr char kQtPackedAlphaYuvMergeFragmentSourceSha256[] =
    "39cd92f756456c93ee2e469825ab47d0adbbd9e6c1865578efb818d7e05d0de6";

// Captured float32 colorConversionMatrix columns. The shader first subtracts
// 16/255 from Y and 0.5 from U/V, then multiplies this fixed limited-range
// BT.601 matrix. MP4 color metadata is deliberately outside this contract.
inline constexpr float kQtPackedAlphaYuvMergeYScale = 1.1640000343322754F;
inline constexpr float kQtPackedAlphaYuvMergeRedFromV = 1.5959999561309814F;
inline constexpr float kQtPackedAlphaYuvMergeGreenFromU = -0.3919999897480011F;
inline constexpr float kQtPackedAlphaYuvMergeGreenFromV = -0.8130000233650208F;
inline constexpr float kQtPackedAlphaYuvMergeBlueFromU = 2.0169999599456787F;
inline constexpr float kQtPackedAlphaYuvMergeYOffset =
    0.062745101749897003173828125F;
inline constexpr float kQtPackedAlphaYuvMergeUvOffset = 0.5F;

enum class QtPackedAlphaYuvMergeRowOrder : std::uint32_t {
  // The captured merge pass renders through originY=height,height=-height.
  // Its texture is then consumed by the VideoAnimSeq/Sprite path with exactly
  // one v=1-v operation. This API exposes the final logical image after that
  // complete direction chain: output row zero is the visible top row and maps
  // to input-plane row zero. It does not expose raw render-target storage.
  FinalTopLeftAfterNegativeViewportAndSpriteVFlip = 1U,
};

enum class QtPackedAlphaYuvMergeAlphaAssociation : std::uint32_t {
  // The packed MP4 right half already contains associated RGB. The merge pass
  // copies that RGB without multiplying by the recovered alpha.
  Associated = 1U,
};

enum class QtPackedAlphaYuvMergePixelFormat : std::uint32_t {
  Rgba8Unorm = 70U,
};

using QtPackedAlphaYuvPlaneView = media::PackedAlphaYuvPlaneView;

struct QtPackedAlphaYuvMergeIdentity final {
  std::string implementationId{kQtPackedAlphaYuvMergeImplementationId};
  std::uint32_t implementationVersion{
      kQtPackedAlphaYuvMergeImplementationVersion};
  std::uint32_t contractSchemaVersion{
      kQtPackedAlphaYuvMergeContractSchemaVersion};
  std::string vertexSourceSha256{kQtPackedAlphaYuvMergeVertexSourceSha256};
  std::string fragmentSourceSha256{kQtPackedAlphaYuvMergeFragmentSourceSha256};
};

struct QtPackedAlphaYuvMergeRequest final {
  QtPackedAlphaYuvMergeIdentity identity;
  QtPackedAlphaYuvPlaneView y;
  QtPackedAlphaYuvPlaneView u;
  QtPackedAlphaYuvPlaneView v;
  QtPackedAlphaYuvMergeRowOrder outputRowOrder{
      QtPackedAlphaYuvMergeRowOrder::
          FinalTopLeftAfterNegativeViewportAndSpriteVFlip};
};

struct QtPackedAlphaYuvMergeTrace final {
  std::string vertexSourceSha256{kQtPackedAlphaYuvMergeVertexSourceSha256};
  std::string fragmentSourceSha256{kQtPackedAlphaYuvMergeFragmentSourceSha256};
  float yPixelWidth{0.0F};
  float yRange{0.0F};
  float uvPixelWidth{0.0F};
  float uvRange{0.0F};
  bool normalizedCoordinates{true};
  bool linearMinFilter{true};
  bool linearMagFilter{true};
  bool clampToEdgeS{true};
  bool clampToEdgeT{true};
  bool blendingEnabled{false};
  bool innerTextureFlip{false};
  bool alphaIsLeftHalf{true};
  bool associatedRgbIsRightHalf{true};
  bool halvesUseSameY{true};
  bool negativeHeightMergeViewport{true};
  bool downstreamSpriteFlipsVOnce{true};
  bool rgbMultipliedByRecoveredAlpha{false};
  bool alphaAveragedBeforeUnorm8{true};
  std::uint32_t fragmentTextureCount{3U};
};

struct QtPackedAlphaYuvMergeResult final {
  std::vector<std::uint8_t> rgba8;
  std::uint32_t width{0U};
  std::uint32_t height{0U};
  std::size_t rowBytes{0U};
  QtPackedAlphaYuvMergeRowOrder rowOrder{
      QtPackedAlphaYuvMergeRowOrder::
          FinalTopLeftAfterNegativeViewportAndSpriteVFlip};
  QtPackedAlphaYuvMergeAlphaAssociation alphaAssociation{
      QtPackedAlphaYuvMergeAlphaAssociation::Associated};
  QtPackedAlphaYuvMergePixelFormat outputFormat{
      QtPackedAlphaYuvMergePixelFormat::Rgba8Unorm};
  QtPackedAlphaYuvMergeTrace trace;
};

// CPU reference for the captured Qt/Amazing VideoAnimSeq packed-alpha pass.
// The Y plane has packed logical dimensions 2W by H. U and V are W by H/2;
// each plane may have row padding represented by rowBytes. Y width and H
// must be even. An individual packed half may have odd width.
//
// Sampling replays the captured normalized linear/clamp coordinates exactly:
//   yRange  = yWidth/yStride/2 - 1/yStride
//   uvRange = uvWidth/uvStride/2 - 1/uvStride
// Alpha comes from the left half; associated RGB comes from the right half.
// Unknown identities and malformed dimensions, capacities, or strides fail
// closed. On failure result is reset and error contains the reason.
[[nodiscard]] bool MergeQtPackedAlphaYuv420pReference(
    const QtPackedAlphaYuvMergeRequest &request,
    QtPackedAlphaYuvMergeResult &result, std::string &error);

} // namespace videocut::skia_runtime::internal
