#include "resources/QtPackedAlphaYuvMergeContract.h"

#include <string>
#include <utility>

namespace videocut::skia_runtime::internal {
namespace {

bool Reject(std::string message, std::string &error) {
  error = std::move(message);
  return false;
}

bool ValidateIdentity(const QtPackedAlphaYuvMergeIdentity &identity,
                      std::string &error) {
  if (identity.implementationId != kQtPackedAlphaYuvMergeImplementationId)
    return Reject("Qt packed-alpha YUV implementation id is unsupported",
                  error);
  if (identity.implementationVersion !=
      kQtPackedAlphaYuvMergeImplementationVersion) {
    return Reject("Qt packed-alpha YUV implementation version is unsupported",
                  error);
  }
  if (identity.contractSchemaVersion !=
      kQtPackedAlphaYuvMergeContractSchemaVersion) {
    return Reject("Qt packed-alpha YUV contract schema is unsupported", error);
  }
  if (identity.vertexSourceSha256 != kQtPackedAlphaYuvMergeVertexSourceSha256) {
    return Reject("Qt packed-alpha YUV vertex shader identity is unsupported",
                  error);
  }
  if (identity.fragmentSourceSha256 !=
      kQtPackedAlphaYuvMergeFragmentSourceSha256) {
    return Reject("Qt packed-alpha YUV fragment shader identity is unsupported",
                  error);
  }
  return true;
}

} // namespace

bool MergeQtPackedAlphaYuv420pReference(
    const QtPackedAlphaYuvMergeRequest &request,
    QtPackedAlphaYuvMergeResult &result, std::string &error) {
  result = {};
  error.clear();
  if (!ValidateIdentity(request.identity, error))
    return false;
  if (request.outputRowOrder !=
      QtPackedAlphaYuvMergeRowOrder::
          FinalTopLeftAfterNegativeViewportAndSpriteVFlip) {
    return Reject("Qt packed-alpha YUV output row order is unsupported", error);
  }

  media::PackedAlphaYuvMergeRequest merge;
  merge.y = request.y;
  merge.u = request.u;
  merge.v = request.v;
  merge.conversion.matrix = {
      kQtPackedAlphaYuvMergeYScale,
      kQtPackedAlphaYuvMergeYScale,
      kQtPackedAlphaYuvMergeYScale,
      0.0F,
      kQtPackedAlphaYuvMergeGreenFromU,
      kQtPackedAlphaYuvMergeBlueFromU,
      kQtPackedAlphaYuvMergeRedFromV,
      kQtPackedAlphaYuvMergeGreenFromV,
      0.0F,
  };
  merge.conversion.offsets = {kQtPackedAlphaYuvMergeYOffset,
                              kQtPackedAlphaYuvMergeUvOffset,
                              kQtPackedAlphaYuvMergeUvOffset};
  media::PackedAlphaYuvMergeResult merged;
  if (!media::MergePackedAlphaYuv420p(merge, merged, error))
    return false;

  result.rgba8 = std::move(merged.rgba8);
  result.width = merged.width;
  result.height = merged.height;
  result.rowBytes = merged.rowBytes;
  result.rowOrder = request.outputRowOrder;
  result.alphaAssociation = QtPackedAlphaYuvMergeAlphaAssociation::Associated;
  const float yStride = static_cast<float>(request.y.rowBytes);
  const float uvStride = static_cast<float>(request.u.rowBytes);
  result.trace.yPixelWidth = 1.0F / yStride;
  result.trace.yRange =
      ((static_cast<float>(request.y.logicalWidth) / yStride) / 2.0F) -
      result.trace.yPixelWidth;
  result.trace.uvPixelWidth = 1.0F / uvStride;
  result.trace.uvRange =
      ((static_cast<float>(request.u.logicalWidth) / uvStride) / 2.0F) -
      result.trace.uvPixelWidth;
  return true;
}

} // namespace videocut::skia_runtime::internal
