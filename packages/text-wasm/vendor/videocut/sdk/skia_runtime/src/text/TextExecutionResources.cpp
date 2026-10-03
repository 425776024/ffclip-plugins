#include "text/TextRenderPipeline.h"
#include "videocut/base/Float16.h"

namespace videocut::skia_runtime::internal::text_lane {


std::uint32_t ReadResidentUint32(
    const std::vector<std::uint8_t> &bytes, const std::size_t offset) noexcept {
  return static_cast<std::uint32_t>(bytes[offset]) |
         (static_cast<std::uint32_t>(bytes[offset + 1U]) << 8U) |
         (static_cast<std::uint32_t>(bytes[offset + 2U]) << 16U) |
         (static_cast<std::uint32_t>(bytes[offset + 3U]) << 24U);
}


std::uint16_t ReadResidentUint16(
    const std::vector<std::uint8_t> &bytes, const std::size_t offset) noexcept {
  return static_cast<std::uint16_t>(bytes[offset]) |
         static_cast<std::uint16_t>(
             static_cast<std::uint16_t>(bytes[offset + 1U]) << 8U);
}


float ReadResidentFloat32(const std::vector<std::uint8_t> &bytes,
                          const std::size_t offset) noexcept {
  const auto bits = ReadResidentUint32(bytes, offset);
  float value = 0.0F;
  static_assert(sizeof(value) == sizeof(bits));
  std::memcpy(&value, &bits, sizeof(value));
  return value;
}


bool DecodeResidentVatMesh(
    const std::shared_ptr<const std::vector<std::uint8_t>> &bytes,
    ResidentVatMesh &output, std::string &error) {
  output = {};
  constexpr std::string_view serializedMagic{"%SerializedFormat%@\n"};
  constexpr std::array<std::uint8_t, 8U> vertexLayoutSignature{
      0x94U, 0x74U, 0x1dU, 0xdaU, 0x0cU, 0x00U, 0x00U, 0x00U};
  constexpr std::array<std::uint8_t, 8U> indexPayloadSignature{
      0x5fU, 0x2cU, 0x28U, 0x6bU, 0x16U, 0x00U, 0x00U, 0x00U};
  constexpr std::size_t vertexStride = 116U;
  constexpr std::array<std::size_t, 11U> requiredFloatOffsets{
      0U, 4U, 8U, 12U, 16U, 20U, 24U, 28U, 32U, 36U, 40U};
  if (!bytes || bytes->size() < 512U ||
      bytes->size() > kMaximumDecodedAssetBytes ||
      bytes->size() < serializedMagic.size() ||
      !std::equal(serializedMagic.begin(), serializedMagic.end(),
                  bytes->begin())) {
    error = "text VAT mesh container is unavailable or malformed";
    return false;
  }
  constexpr std::array<std::uint8_t, 4U> meshSignature{
      static_cast<std::uint8_t>('M'), static_cast<std::uint8_t>('E'),
      static_cast<std::uint8_t>('S'), static_cast<std::uint8_t>('H')};
  const auto meshMarker = std::search(bytes->begin(), bytes->begin() +
                                                          static_cast<std::ptrdiff_t>(
                                                              std::min<std::size_t>(
                                                                  bytes->size(),
                                                                  4096U)),
                                      meshSignature.begin(),
                                      meshSignature.end());
  if (meshMarker == bytes->begin() + static_cast<std::ptrdiff_t>(
                                        std::min<std::size_t>(bytes->size(),
                                                              4096U))) {
    error = "text VAT mesh container has no mesh payload";
    return false;
  }
  const auto indexSignature = FindResidentSignature(
      *bytes, indexPayloadSignature, 0U, bytes->size());
  if (!indexSignature || *indexSignature + 12U > bytes->size()) {
    error = "text VAT mesh index payload is unavailable";
    return false;
  }
  const auto indexCount = ReadResidentUint32(*bytes, *indexSignature + 8U);
  const auto indexOffset = *indexSignature + 12U;
  const std::uint64_t indexBytes = static_cast<std::uint64_t>(indexCount) * 2U;
  if (indexCount == 0U || indexCount % 3U != 0U ||
      indexBytes > bytes->size() - indexOffset) {
    error = "text VAT mesh index payload is invalid";
    return false;
  }
  output.indices.reserve(indexCount);
  std::uint16_t maximumIndex = 0U;
  for (std::uint32_t index = 0U; index < indexCount; ++index) {
    const auto value =
        ReadResidentUint16(*bytes, indexOffset + static_cast<std::size_t>(index) * 2U);
    output.indices.push_back(value);
    maximumIndex = std::max(maximumIndex, value);
  }
  const std::size_t vertexCount = static_cast<std::size_t>(maximumIndex) + 1U;
  if (vertexCount < 3U || vertexCount > 65535U ||
      vertexCount > std::numeric_limits<std::size_t>::max() / vertexStride) {
    error = "text VAT mesh vertex count is invalid";
    return false;
  }
  const auto vertexLayout = FindResidentSignature(
      *bytes, vertexLayoutSignature, serializedMagic.size(), *indexSignature);
  const auto vertexBytes = vertexCount * vertexStride;
  if (!vertexLayout || *vertexLayout < vertexBytes) {
    error = "text VAT mesh vertex layout is unavailable";
    return false;
  }
  const auto vertexOffset = *vertexLayout - vertexBytes;
  if (vertexOffset <= static_cast<std::size_t>(
                          std::distance(bytes->begin(), meshMarker) + 4) ||
      vertexOffset > 64U * 1024U) {
    error = "text VAT mesh vertex payload boundary is invalid";
    return false;
  }
  output.vertices.resize(vertexCount);
  for (std::size_t index = 0U; index < vertexCount; ++index) {
    const auto base = vertexOffset + index * vertexStride;
    std::array<float, requiredFloatOffsets.size()> values{};
    for (std::size_t component = 0U; component < values.size(); ++component) {
      values[component] =
          ReadResidentFloat32(*bytes, base + requiredFloatOffsets[component]);
      if (!std::isfinite(values[component])) {
        error = "text VAT mesh vertex contains a non-finite component";
        return false;
      }
    }
    if (values[3] < -0.001F || values[3] > 1.001F ||
        values[4] < -0.001F || values[4] > 1.001F ||
        values[5] < -0.001F || values[5] > 1.001F ||
        values[6] < -0.001F || values[6] > 1.001F) {
      error = "text VAT mesh texture coordinates are outside the resident ABI";
      return false;
    }
    auto &vertex = output.vertices[index];
    vertex.position = {values[0], values[1], values[2]};
    vertex.uv0 = {values[3], values[4]};
    vertex.uv1 = {values[5], values[6]};
    vertex.uv2 = {values[7], values[8]};
    vertex.uv3 = {values[9], values[10]};
  }
  error.clear();
  return true;
}


bool DecodeResidentFloatTexture(
    const std::shared_ptr<const std::vector<std::uint8_t>> &bytes,
    ResidentFloatTexture &output, std::string &error) {
  output = {};
  if (!bytes || bytes->size() < 64U) {
    error = "text VAT float texture is unavailable";
    return false;
  }
  std::size_t payloadOffset = 0U;
  const auto scanEnd = std::min<std::size_t>(bytes->size() - 12U, 64U * 1024U);
  for (std::size_t offset = 20U; offset <= scanEnd; ++offset) {
    const auto width = ReadResidentUint32(*bytes, offset);
    const auto height = ReadResidentUint32(*bytes, offset + 4U);
    const auto format = ReadResidentUint32(*bytes, offset + 8U);
    if (width == 0U || height == 0U || width > 8192U || height > 8192U ||
        format != 3U) {
      continue;
    }
    const std::uint64_t pixelBytes = static_cast<std::uint64_t>(width) *
                                     static_cast<std::uint64_t>(height) * 8U;
    if (pixelBytes > std::numeric_limits<std::size_t>::max() ||
        offset + 12U + static_cast<std::size_t>(pixelBytes) != bytes->size()) {
      continue;
    }
    output.width = width;
    output.height = height;
    payloadOffset = offset + 12U;
    break;
  }
  if (payloadOffset == 0U) {
    error = "text VAT float texture payload is malformed";
    return false;
  }
  const auto payloadBytes = bytes->size() - payloadOffset;
  auto data = SkData::MakeWithCopy(bytes->data() + payloadOffset, payloadBytes);
  const auto info = SkImageInfo::Make(
      static_cast<int>(output.width), static_cast<int>(output.height),
      kRGBA_F16_SkColorType, kUnpremul_SkAlphaType, nullptr);
  output.image = data ? SkImages::RasterFromData(
                            info, std::move(data),
                            static_cast<std::size_t>(output.width) * 8U)
                      : nullptr;
  if (!output.image) {
    error = "text VAT float texture publication failed";
    return false;
  }
  output.bytes = bytes;
  output.payloadOffset = payloadOffset;
  error.clear();
  return true;
}



std::array<float, 4U>
SampleResidentFloatTexture(const ResidentFloatTexture &texture, const float u,
                           const float v) noexcept {
  std::array<float, 4U> result{};
  if (!texture.bytes || texture.width == 0U || texture.height == 0U)
    return result;
  const auto x = static_cast<std::uint32_t>(std::clamp(
      static_cast<int>(std::floor(u * static_cast<float>(texture.width))), 0,
      static_cast<int>(texture.width) - 1));
  const auto y = static_cast<std::uint32_t>(std::clamp(
      static_cast<int>(std::floor(v * static_cast<float>(texture.height))), 0,
      static_cast<int>(texture.height) - 1));
  const auto offset = texture.payloadOffset +
                      (static_cast<std::size_t>(y) * texture.width + x) * 8U;
  for (std::size_t component = 0U; component < result.size(); ++component) {
    result[component] = base::Float16ToFloat32(
        ReadResidentUint16(*texture.bytes, offset + component * 2U));
  }
  return result;
}


std::optional<ExecutionMaterialViewport> ResolveExecutionMaterialViewport(
    const ExecutionGraphRaster &raster, const PageRenderGroupDomain *pageDomain,
    const SkSize viewportSize, std::string &error) {
  ExecutionMaterialViewport result;
  result.size = SkISize::Make(static_cast<int>(viewportSize.width()),
                             static_cast<int>(viewportSize.height()));
  result.scaleX = pageDomain ? pageDomain->rasterScaleX : 1.0F;
  result.scaleY = pageDomain ? pageDomain->rasterScaleY : 1.0F;
  if (!raster.image || result.size.isEmpty() ||
      !std::isfinite(result.scaleX) || !std::isfinite(result.scaleY) ||
      result.scaleX <= 0.0F || result.scaleY <= 0.0F) {
    error = "text fullscreen material requires a valid source and viewport";
    return std::nullopt;
  }
  result.textOrigin = pageDomain
      ? std::array<float, 2>{pageDomain->deviceTargetBounds.left(),
                             pageDomain->deviceTargetBounds.top()}
      : std::array<float, 2>{static_cast<float>(raster.bounds.left()),
                             static_cast<float>(raster.bounds.top())};
  result.textExtent = {
      static_cast<float>(raster.image->width()) / result.scaleX,
      static_cast<float>(raster.image->height()) / result.scaleY};
  result.localBounds = SkRect::MakeXYWH(
      pageDomain ? -result.textOrigin[0] * result.scaleX : 0.0F,
      pageDomain ? -result.textOrigin[1] * result.scaleY : 0.0F,
      viewportSize.width() * result.scaleX,
      viewportSize.height() * result.scaleY);
  return result;
}


sk_sp<SkPicture> RecordExecutionMaterialViewport(
    const sk_sp<SkImage> &image, const ExecutionMaterialViewport &viewport) {
  // The graph's Page promotion cancels this transform exactly once.
  SkPictureRecorder recorder;
  auto *canvas = recorder.beginRecording(viewport.localBounds);
  canvas->translate(viewport.localBounds.left(), viewport.localBounds.top());
  canvas->scale(viewport.scaleX, viewport.scaleY);
  canvas->drawImage(image, 0.0F, 0.0F,
                   SkSamplingOptions(SkFilterMode::kLinear), nullptr);
  return recorder.finishRecordingAsPicture();
}


std::array<float, 3U> ResidentCross(const std::array<float, 3U> &left,
                                    const std::array<float, 3U> &right) noexcept {
  return {left[1] * right[2] - left[2] * right[1],
          left[2] * right[0] - left[0] * right[2],
          left[0] * right[1] - left[1] * right[0]};
}


sk_sp<SkImage> ExecuteResidentVatRbdMesh(
    const sk_sp<SkImage> &source, SkiaGpuContext *gpuContext,
    const ResidentVatMesh &mesh, const ResidentFloatTexture &positionTexture,
    const ResidentFloatTexture &rotationTexture, const SkIRect &rasterBounds,
    const SkRect &textBounds, const std::array<float, 2U> &textSize,
    const float animationTime, const float textureRotationDegrees,
    const float fade,
    const float frameCount, const float houdiniFps, const float displayFrame,
    const float autoPlayback, const float interframeInterpolation,
    const float animateFirstFrame, const std::array<float, 3U> &boundMinimum,
    const std::array<float, 3U> &boundMaximum,
    const float cameraFovDegrees, const float modelDepthScale,
    std::string &error) {
  if (!source || mesh.vertices.empty() || mesh.indices.empty() ||
      positionTexture.width == 0U || positionTexture.height == 0U ||
      rotationTexture.width == 0U || rotationTexture.height == 0U ||
      rasterBounds.isEmpty() || textBounds.isEmpty() ||
      !textBounds.isFinite() || !std::isfinite(textSize[0]) ||
      !std::isfinite(textSize[1]) || textSize[0] <= 0.0F ||
      textSize[1] <= 0.0F || !std::isfinite(animationTime) ||
      !std::isfinite(textureRotationDegrees) || !std::isfinite(fade) ||
      !std::isfinite(frameCount) ||
      !std::isfinite(houdiniFps) || !std::isfinite(displayFrame) ||
      !std::isfinite(autoPlayback) ||
      !std::isfinite(interframeInterpolation) ||
      !std::isfinite(animateFirstFrame) ||
      !std::all_of(boundMinimum.begin(), boundMinimum.end(),
                   [](const float value) { return std::isfinite(value); }) ||
      !std::all_of(boundMaximum.begin(), boundMaximum.end(),
                   [](const float value) { return std::isfinite(value); }) ||
      !std::isfinite(cameraFovDegrees) || !std::isfinite(modelDepthScale) ||
      frameCount < 1.0F ||
      houdiniFps <= 0.0F || cameraFovDegrees <= 0.0F ||
      cameraFovDegrees >= 180.0F || modelDepthScale <= 0.0F ||
      interframeInterpolation < 0.0F || interframeInterpolation > 1.0F) {
    error = "text VAT mesh execution parameters are invalid";
    return {};
  }
  const auto roundedFrameCount = static_cast<std::uint32_t>(std::lround(frameCount));
  if (roundedFrameCount == 0U ||
      positionTexture.height % roundedFrameCount != 0U ||
      rotationTexture.height % roundedFrameCount != 0U) {
    error = "text VAT texture frame topology is invalid";
    return {};
  }
  const float cycle = animationTime *
                      (houdiniFps / std::max(frameCount - 0.01F, 0.01F));
  const float wrappedCycle = cycle - std::floor(cycle);
  const float continuousFrame =
      autoPlayback > 0.5F ? wrappedCycle * frameCount + 1.0F : displayFrame;
  const float selectedFrame = std::floor(continuousFrame);
  const float interframeWeight =
      interframeInterpolation > 0.5F
          ? continuousFrame - std::floor(continuousFrame)
          : 0.0F;
  float wrappedFrame = std::fmod(selectedFrame - 1.0F, frameCount);
  if (wrappedFrame < 0.0F)
    wrappedFrame += frameCount;
  const auto frameIndex = static_cast<std::uint32_t>(std::clamp(
      static_cast<int>(std::floor(wrappedFrame)), 0,
      static_cast<int>(roundedFrameCount) - 1));
  const auto nextFrameIndex =
      static_cast<std::uint32_t>((frameIndex + 1U) % roundedFrameCount);
  const auto normalizedActiveRatio = [](const float scaled,
                                        const bool ceilingEncoding) {
    const float encoded = ceilingEncoding
                              ? std::ceil(scaled) - scaled
                              : scaled - std::floor(scaled);
    const float ratio = 1.0F - encoded;
    return ratio < 0.0001F || ratio > 0.9999F ? 1.0F : ratio;
  };
  const float activePixelsX =
      normalizedActiveRatio(boundMinimum[2] * 10.0F, true);
  const float activePixelsY =
      normalizedActiveRatio(boundMaximum[0] * -10.0F, false);
  if (activePixelsX <= 0.0F || activePixelsX > 1.0F ||
      activePixelsY <= 0.0F || activePixelsY > 1.0F) {
    error = "text VAT mesh active texture region is invalid";
    return {};
  }
  const float frameV = static_cast<float>(frameIndex) / frameCount *
                       activePixelsY;
  const float nextFrameV = static_cast<float>(nextFrameIndex) / frameCount *
                           activePixelsY;
  const float canvasWidth = static_cast<float>(rasterBounds.width());
  const float canvasHeight = static_cast<float>(rasterBounds.height());
  const float aspect = canvasWidth / canvasHeight;
  const float tangent = std::tan(cameraFovDegrees *
                                 (std::numbers::pi_v<float> / 360.0F));
  const float cameraDepth = 1.0F / tangent;
  // The source vertex shader declares scale_val but never reads it; its
  // captured MVP remains fixed even during the final fade. Shrinking this
  // model would move every baked VAT fragment near the end of the clip.
  const float modelScale =
      std::max(textBounds.width(), textBounds.height()) / canvasHeight;
  const float centerX = textBounds.centerX() - rasterBounds.left();
  const float centerY = textBounds.centerY() - rasterBounds.top();
  const float textAspect = textSize[0] / textSize[1];
  const float textureRadians = textureRotationDegrees *
                               (std::numbers::pi_v<float> / 180.0F);
  const float textureSine = std::sin(textureRadians);
  const float textureCosine = std::cos(textureRadians);

  std::vector<SkPoint> positions(mesh.vertices.size());
  std::vector<SkPoint> textureCoordinates(mesh.vertices.size());
  std::vector<TextVatMeshVertex> gpuVertices(mesh.vertices.size());
  std::vector<bool> visible(mesh.vertices.size(), true);
  for (std::size_t index = 0U; index < mesh.vertices.size(); ++index) {
    const auto &vertex = mesh.vertices[index];
    const float sampleU = vertex.uv1[0] * activePixelsX;
    const float sampleV =
        (1.0F - vertex.uv1[1]) * activePixelsY + frameV;
    const auto translation =
        SampleResidentFloatTexture(positionTexture, sampleU, sampleV);
    const auto rotation =
        SampleResidentFloatTexture(rotationTexture, sampleU, sampleV);
    const auto nextTranslation = SampleResidentFloatTexture(
        positionTexture, sampleU,
        (1.0F - vertex.uv1[1]) * activePixelsY + nextFrameV);
    const auto nextRotation = SampleResidentFloatTexture(
        rotationTexture, sampleU,
        (1.0F - vertex.uv1[1]) * activePixelsY + nextFrameV);
    std::array<float, 4U> sampledTranslation{};
    std::array<float, 4U> sampledRotation{};
    for (std::size_t component = 0U; component < 4U; ++component) {
      sampledTranslation[component] =
          std::lerp(translation[component], nextTranslation[component],
                    interframeWeight);
      sampledRotation[component] =
          std::lerp(rotation[component], nextRotation[component],
                    interframeWeight);
    }
    const std::array<float, 3U> piecePivot{
        vertex.position[0] - vertex.uv2[0],
        vertex.position[1] - vertex.uv3[0],
        vertex.position[2] - (1.0F - vertex.uv3[1])};
    const std::array<float, 3U> quaternion{
        sampledRotation[0], sampledRotation[1], sampledRotation[2]};
    // Preserve the authored Qt shader contract exactly.  This VAT family is
    // bug-compatible with the source GLSL/Metal program and uses q.z as the
    // quaternion product term rather than reconstructing q.w.
    const auto firstCross = ResidentCross(quaternion, piecePivot);
    const std::array<float, 3U> intermediate{
        firstCross[0] + quaternion[2] * piecePivot[0],
        firstCross[1] + quaternion[2] * piecePivot[1],
        firstCross[2] + quaternion[2] * piecePivot[2]};
    const auto secondCross = ResidentCross(quaternion, intermediate);
    const bool useAnimatedPosition =
        animateFirstFrame >= 0.50001F ||
        std::fabs(static_cast<float>(frameIndex) / frameCount - 1.0F) < 1.0F;
    const std::array<float, 3U> transformed =
        useAnimatedPosition
            ? std::array<float, 3U>{
                  -sampledTranslation[0] + piecePivot[0] +
                      secondCross[0] * 2.0F,
                  sampledTranslation[1] + piecePivot[1] +
                      secondCross[1] * 2.0F,
                  sampledTranslation[2] + piecePivot[2] +
                      secondCross[2] * 2.0F}
            : vertex.position;
    const float viewDepth = cameraDepth - transformed[2] * modelDepthScale;
    if (!std::isfinite(viewDepth) || viewDepth <= 0.0001F) {
      visible[index] = false;
      positions[index] = SkPoint::Make(-1.0e6F, -1.0e6F);
    } else {
      const float projectedX = transformed[0] * modelScale /
                               (viewDepth * tangent * aspect);
      const float projectedY = transformed[1] * modelScale /
                               (viewDepth * tangent);
      positions[index] =
          SkPoint::Make(centerX + projectedX * canvasWidth * 0.5F,
                        centerY - projectedY * canvasHeight * 0.5F);
    }
    const float centeredTextureX = vertex.uv0[0] - 0.5F;
    // Resident mesh UVs use the Metal/GL bottom-left texture convention,
    // while Skia image coordinates are top-left. Convert the source-text V
    // coordinate exactly once before applying the authored texture rotation.
    const float centeredTextureY = (1.0F - vertex.uv0[1]) - 0.5F;
    const float textureU = textureCosine * centeredTextureX -
                               textureSine * centeredTextureY +
                           0.5F;
    const float rotatedTextureV = textureSine * centeredTextureX +
                                      textureCosine * centeredTextureY +
                                  0.5F;
    const float textureV =
        (rotatedTextureV - 0.5F) * textAspect + 0.5F;
    textureCoordinates[index] =
        SkPoint::Make(textureU * static_cast<float>(source->width()),
                      textureV * static_cast<float>(source->height()));
    if (visible[index]) {
      const float normalizedX =
          positions[index].x() * 2.0F / canvasWidth - 1.0F;
      const float normalizedY =
          1.0F - positions[index].y() * 2.0F / canvasHeight;
      gpuVertices[index] = TextVatMeshVertex{
          normalizedX * viewDepth, normalizedY * viewDepth,
          viewDepth - 0.01F, viewDepth,
          textureCoordinates[index].x() / static_cast<float>(source->width()),
          textureCoordinates[index].y() /
              static_cast<float>(source->height())};
    }
  }

  std::vector<std::uint16_t> visibleIndices;
  visibleIndices.reserve(mesh.indices.size());
  for (std::size_t index = 0U; index < mesh.indices.size(); index += 3U) {
    const auto first = mesh.indices[index];
    const auto second = mesh.indices[index + 1U];
    const auto third = mesh.indices[index + 2U];
    if (visible[first] && visible[second] && visible[third]) {
      visibleIndices.insert(visibleIndices.end(), {first, second, third});
    }
  }
  if (visibleIndices.empty()) {
    error = "text VAT mesh has no visible triangles";
    return {};
  }
  const auto info = SkImageInfo::Make(
      rasterBounds.width(), rasterBounds.height(), kRGBA_8888_SkColorType,
      kPremul_SkAlphaType, SkColorSpace::MakeSRGB());
  auto surface = gpuContext ? gpuContext->MakeSurface(info, error)
                            : SkSurfaces::Raster(info);
  if (!surface) {
    if (error.empty())
      error = "text VAT mesh target allocation failed";
    return {};
  }
  auto *canvas = surface->getCanvas();
  canvas->clear(SK_ColorTRANSPARENT);
  if (gpuContext) {
    TextVatMeshGpuRequest request;
    request.targetWidth = rasterBounds.width();
    request.targetHeight = rasterBounds.height();
    request.vertices = std::move(gpuVertices);
    request.indices = std::move(visibleIndices);
    request.opacity = std::clamp(fade, 0.0F, 1.0F);
    if (!gpuContext->RenderTextVatMesh(source, request, *surface, error))
      return {};
  } else {
    auto vertices = SkVertices::MakeCopy(
        SkVertices::kTriangles_VertexMode,
        static_cast<int>(positions.size()), positions.data(),
        textureCoordinates.data(), nullptr,
        static_cast<int>(visibleIndices.size()), visibleIndices.data());
    if (!vertices) {
      error = "text VAT mesh vertex publication failed";
      return {};
    }
    SkPaint paint;
    paint.setShader(source->makeRawShader(
        SkTileMode::kDecal, SkTileMode::kDecal,
        SkSamplingOptions(SkFilterMode::kLinear, SkMipmapMode::kNone)));
    paint.setAlphaf(std::clamp(fade, 0.0F, 1.0F));
    canvas->drawVertices(vertices, SkBlendMode::kSrcOver, paint);
  }
  auto image = surface->makeImageSnapshot();
  if (!image)
    error = "text VAT mesh target snapshot failed";
  return image;
}


sk_sp<SkPicture> DepthExecutionGraphPicture(
    const sk_sp<SkPicture> &source, const SkRect &bounds) {
  if (!source)
    return {};
  SkPictureRecorder recorder;
  auto *canvas = recorder.beginRecording(bounds);
  SkPaint paint;
  paint.setColorFilter(
      SkColorFilters::Blend(SK_ColorWHITE, SkBlendMode::kSrcIn));
  canvas->drawPicture(source, nullptr, &paint);
  return recorder.finishRecordingAsPicture();
}


sk_sp<SkPicture> ComposeExecutionGraphInputs(
    const std::string &nodeId, const std::vector<std::string> &inputIds,
    const std::unordered_map<std::string, sk_sp<SkPicture>> &outputs,
    const SkRect &bounds, bool &resolved, std::string &error) {
  resolved = true;
  std::vector<RenderComponent> pictures;
  pictures.reserve(inputIds.size());
  std::size_t order = 0U;
  for (const auto &inputId : inputIds) {
    const auto found = outputs.find(inputId);
    if (found == outputs.end()) {
      error = "text execution graph input was not executed in authored order: " +
              nodeId + " <- " + inputId;
      resolved = false;
      return {};
    }
    if (found->second) {
      pictures.push_back({inputId,
                          text::TextEffectCompositeItemKind::PostEffect,
                          0,
                          text::TextBlendMode::SourceOver,
                          found->second,
                          "execution:" + inputId,
                          order++});
    }
  }
  return ComposePictures(pictures, bounds);
}


DirectLetterExecutionPlan ResolveDirectLetterExecutionPlan(
    const std::vector<text::TextEffectExecutionNodeFramePlan> &nodes,
    const SkRect &pivotBounds) {
  if (nodes.empty())
    return {true, SkMatrix::I(), 1.0F};

  struct SourceState final {
    SkMatrix authoredTransform{SkMatrix::I()};
    float opacity{1.0F};
  };
  std::unordered_map<std::string, SourceState> sourceOutputs;
  std::unordered_set<std::string> referencedOutputs;
  std::vector<std::string> authoredOutputs;
  const auto visit = [&](const auto &self,
                         const std::vector<
                             text::TextEffectExecutionNodeFramePlan> &items,
                         const std::optional<SourceState> &inheritedSource)
      -> bool {
    for (const auto &node : items) {
      if (node.nodeId.empty() || sourceOutputs.contains(node.nodeId) ||
          !ExecutionGraphCapabilityMatchesKind(node.kind, node.capability) ||
          node.inputIds.size() > 1U || !node.resourceIds.empty()) {
        return false;
      }

      std::optional<SourceState> state;
      if (inheritedSource && node.inputIds.empty())
        state = inheritedSource;
      if (!node.inputIds.empty()) {
        const auto &inputId = node.inputIds.front();
        const auto input = sourceOutputs.find(inputId);
        if (input == sourceOutputs.end())
          return false;
        referencedOutputs.insert(inputId);
        state = input->second;
      }

      if (node.active) {
        switch (node.kind) {
        case text::TextEffectExecutionNodeKind::Layout:
        case text::TextEffectExecutionNodeKind::Scene:
          // These are the only control nodes that seed the already evaluated
          // global glyph/component source when no explicit input is present.
          if (!state)
            state = SourceState{};
          break;
        case text::TextEffectExecutionNodeKind::Selector:
        case text::TextEffectExecutionNodeKind::Operator:
          if (!state)
            return false;
          break;
        case text::TextEffectExecutionNodeKind::Composite:
          // Source-over of more than one alias changes edge alpha and is not
          // a direct-source topology.  Composite also requires an authored
          // input rather than the inherited child source.
          if (node.inputIds.size() != 1U || !state)
            return false;
          break;
        case text::TextEffectExecutionNodeKind::State:
        case text::TextEffectExecutionNodeKind::MaterialPass:
        case text::TextEffectExecutionNodeKind::PostEffectPass:
        case text::TextEffectExecutionNodeKind::RenderTarget:
        case text::TextEffectExecutionNodeKind::History:
        case text::TextEffectExecutionNodeKind::MediaInput:
          return false;
        }
      } else if (!state) {
        return false;
      }

      if (!state)
        return false;
      if (node.active &&
          (node.staticAffine.has_value() || !node.parameters.empty())) {
        if (node.kind != text::TextEffectExecutionNodeKind::Scene ||
            node.capability !=
                text::TextEffectExecutionCapability::SceneClone) {
          return false;
        }

        SkMatrix nodeTransform = SkMatrix::I();
        if (node.staticAffine) {
          const auto &affine = *node.staticAffine;
          const float radians = affine.rotationDegrees *
                                (std::numbers::pi_v<float> / 180.0F);
          const float sine = std::sin(radians);
          const float cosine = std::cos(radians);
          const float scaleCosineX = cosine * affine.scaleX;
          const float scaleSineX = sine * affine.scaleX;
          const float scaleCosineY = cosine * affine.scaleY;
          const float scaleSineY = sine * affine.scaleY;
          nodeTransform.setAll(
              scaleCosineX, -scaleSineY,
              affine.translationX + affine.pivotX -
                  scaleCosineX * affine.pivotX +
                  scaleSineY * affine.pivotY,
              scaleSineX, scaleCosineY,
              affine.translationY + affine.pivotY -
                  scaleSineX * affine.pivotX -
                  scaleCosineY * affine.pivotY,
              0.0F, 0.0F, 1.0F);
        }

        float translationX = 0.0F;
        float translationY = 0.0F;
        float scaleX = 1.0F;
        float scaleY = 1.0F;
        float alpha = 1.0F;
        std::unordered_set<text::TextEffectExecutionParameterKind>
            parameterKinds;
        for (const auto &parameter : node.parameters) {
          if (parameter.nodeId != node.nodeId || parameter.slot != 0U ||
              parameter.domain !=
                  text::TextEffectExecutionParameterDomain::Page ||
              parameter.stableUnitId ||
              !parameterKinds.insert(parameter.parameter).second) {
            return false;
          }
          switch (parameter.parameter) {
          case text::TextEffectExecutionParameterKind::SceneTranslation:
            if (parameter.valueSpace !=
                    text::TextEffectExecutionParameterSpace::ReferencePixels ||
                parameter.values.size() != 2U) {
              return false;
            }
            translationX = parameter.values[0];
            translationY = parameter.values[1];
            break;
          case text::TextEffectExecutionParameterKind::SceneScale:
            if (parameter.valueSpace !=
                    text::TextEffectExecutionParameterSpace::Unitless ||
                parameter.values.size() != 2U) {
              return false;
            }
            scaleX = parameter.values[0];
            scaleY = parameter.values[1];
            break;
          case text::TextEffectExecutionParameterKind::SceneOpacity:
            if (parameter.valueSpace !=
                    text::TextEffectExecutionParameterSpace::Unitless ||
                parameter.values.size() != 1U) {
              return false;
            }
            alpha = parameter.values[0];
            break;
          default:
            return false;
          }
          RecordTextExecutionParameterConsumption(node, parameter);
        }
        if (!std::isfinite(translationX) || !std::isfinite(translationY) ||
            !std::isfinite(scaleX) || !std::isfinite(scaleY) ||
            !std::isfinite(alpha)) {
          return false;
        }

        SkMatrix dynamicTransform;
        dynamicTransform.setAll(
            scaleX, 0.0F,
            translationX + pivotBounds.centerX() -
                scaleX * pivotBounds.centerX(),
            0.0F, scaleY,
            translationY + pivotBounds.centerY() -
                scaleY * pivotBounds.centerY(),
            0.0F, 0.0F, 1.0F);
        SkMatrix combinedNodeTransform;
        combinedNodeTransform.setConcat(dynamicTransform, nodeTransform);
        SkMatrix combinedSourceTransform;
        combinedSourceTransform.setConcat(combinedNodeTransform,
                                          state->authoredTransform);
        state->authoredTransform = combinedSourceTransform;
        state->opacity *= std::clamp(alpha, 0.0F, 1.0F);
      }

      sourceOutputs.emplace(node.nodeId, *state);
      authoredOutputs.push_back(node.nodeId);
      if (!node.children.empty()) {
        referencedOutputs.insert(node.nodeId);
        if (!self(self, node.children, state))
          return false;
      }
    }
    return true;
  };
  if (!visit(visit, nodes, std::nullopt))
    return {};

  const SourceState *terminal = nullptr;
  for (const auto &nodeId : authoredOutputs) {
    if (referencedOutputs.contains(nodeId))
      continue;
    if (terminal)
      return {};
    terminal = &sourceOutputs.at(nodeId);
  }
  if (!terminal)
    return {};
  return {true, terminal->authoredTransform, terminal->opacity};
}


void AddExecutionGraphParameterIdentity(
    IdentityBuilder &identity,
    const text::TextEffectExecutionParameterSample &parameter) {
  identity.AddString(parameter.nodeId);
  identity.Add(parameter.parameter);
  identity.Add(parameter.domain);
  identity.Add(parameter.valueSpace);
  identity.Add(parameter.slot);
  identity.Add(parameter.stableUnitId.has_value());
  if (parameter.stableUnitId)
    identity.Add(*parameter.stableUnitId);
  identity.Add(parameter.values.size());
  for (const auto value : parameter.values)
    identity.Add(value);
}


void AddExecutionCameraIdentity(
    IdentityBuilder &identity,
    const std::optional<text::TextEffectExecutionCamera> &camera) {
  identity.Add(camera.has_value());
  if (!camera)
    return;
  for (const auto value : camera->worldToClip)
    identity.Add(value);
  for (const auto value : camera->viewport)
    identity.Add(value);
}

} // namespace videocut::skia_runtime::internal::text_lane
