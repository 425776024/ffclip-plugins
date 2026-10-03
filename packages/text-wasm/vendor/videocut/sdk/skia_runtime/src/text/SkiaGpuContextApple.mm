#include "runtime/MetalRuntimeSupport.h"
#include "text/shaders/MetalTextSdf.h"
#include "text/shaders/MetalTextVatMesh.h"

#include "text/shaders/MetalTextSdfMaterial.h"

#include "text/shaders/MetalQtTextRenderGroupCompositeVertex.h"

#include "text/shaders/MetalQtTextRenderGroupCompositeFragment.h"

#include "text/shaders/MetalQtTextFollowerComposite.h"

#include "text/SkiaGpuContext.h"
#include "text/QtTextEngineCopyMetalRuntime.h"
#include "text/QtTextLetterMetalRuntime.h"
#include "text/TextGradientLutShader.h"
#include "text/TextSdfMeshPositionContract.h"

#include "include/gpu/ganesh/SkImageGanesh.h"
#include "include/gpu/ganesh/GrBackendSurface.h"
#include "include/gpu/ganesh/mtl/GrMtlBackendSurface.h"
#include "src/gpu/ganesh/GrDirectContextPriv.h"
#include "src/gpu/ganesh/mtl/GrMtlGpu.h"
#include "src/gpu/ganesh/mtl/GrMtlCommandBuffer.h"
#include "videocut/gpu_execution/GpuExecution.h"

#import <Metal/Metal.h>
#import <simd/simd.h>

#include "text/SkiaMetalCompletion.h"
#include "text/NativeMetalSubmission.h"
#include "videocut/gpu_frame/FrameBufferInterop.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <type_traits>
#include <utility>

namespace videocut::skia_runtime::internal {
namespace {

constexpr const char *kTextSdfMetalSource = shader_sources::MetalTextSdf;

constexpr const char *kTextSdfMaterialMetalSource =
    shader_sources::MetalTextSdfMaterial;

constexpr const char *kQtTextRenderGroupCompositeVertexSource =
    shader_sources::MetalQtTextRenderGroupCompositeVertex;
constexpr const char *kQtTextRenderGroupCompositeFragmentSource =
    shader_sources::MetalQtTextRenderGroupCompositeFragment;
constexpr const char *kQtTextFollowerCompositeMetalSource =
    shader_sources::MetalQtTextFollowerComposite;

constexpr const char *kTextVatMeshMetalSource =
    shader_sources::MetalTextVatMesh;

struct TextSdfMetalState final {
  id<MTLCommandQueue> queue{nil};
  id<MTLRenderPipelineState> distancePipeline{nil};
  id<MTLRenderPipelineState> shapePipeline{nil};
  id<MTLRenderPipelineState> inversePipeline{nil};
  id<MTLRenderPipelineState> materialDistancePipeline{nil};
  id<MTLRenderPipelineState> materialShapePipeline{nil};
  id<MTLRenderPipelineState> materialInversePipeline{nil};
  id<MTLRenderPipelineState> materialRgbaPipeline{nil};
  id<MTLRenderPipelineState> materialBgraPipeline{nil};
  id<MTLRenderPipelineState> materialSourceOverRgbaPipeline{nil};
  id<MTLRenderPipelineState> materialSourceOverBgraPipeline{nil};
  id<MTLDepthStencilState> distanceDepthStencil{nil};
  id<MTLDepthStencilState> shapeIncrementDepthStencil{nil};
  id<MTLDepthStencilState> shapeDecrementDepthStencil{nil};
  id<MTLDepthStencilState> inverseDepthStencil{nil};
  id<MTLDepthStencilState> materialDepthStencil{nil};
  id<MTLSamplerState> distanceSampler{nil};
  id<MTLSamplerState> sourceSampler{nil};
  id<MTLSamplerState> destinationSampler{nil};
  id<MTLSamplerState> gradientSampler{nil};

  ~TextSdfMetalState() {
    [gradientSampler release];
    [destinationSampler release];
    [sourceSampler release];
    [distanceSampler release];
    [materialDepthStencil release];
    [inverseDepthStencil release];
    [shapeDecrementDepthStencil release];
    [shapeIncrementDepthStencil release];
    [distanceDepthStencil release];
    [materialSourceOverBgraPipeline release];
    [materialSourceOverRgbaPipeline release];
    [materialBgraPipeline release];
    [materialRgbaPipeline release];
    [materialInversePipeline release];
    [materialShapePipeline release];
    [materialDistancePipeline release];
    [inversePipeline release];
    [shapePipeline release];
    [distancePipeline release];
    [queue release];
  }
};

static_assert(sizeof(TextSdfDistanceVertex) == 32U);
static_assert(sizeof(TextSdfShapeVertex) == 16U);

struct alignas(16) TextSdfMaterialUniforms final {
  matrix_float4x4 localToPresentation{};
  std::array<simd_float4, 4> qtLetterPositions{};
  matrix_float4x4 qtLetterMvp{};
  simd_float4 qtLetterOffset{};
  simd_float4 qtInnerShadowUvOffset{};
  simd_float4 destinationSize{};
  simd_float4 distanceTextureSize{};
  simd_float4 glyphAtlasRect{};
  simd_float4 glyphLocalRect{};
  simd_float4 glyphMaterialRect{};
  simd_float4 lineRect{};
  simd_float4 textRect{};
  simd_float4 replacementMaskRect{};
  simd_float4 distanceAndOpacity{};
  simd_float4 layerGeometry0{};
  simd_float4 layerGeometry1{};
  simd_float4 layerGeometry2{};
  simd_float4 linearGradient{};
  simd_float4 radialAndCoordinates{};
  simd_float4 materialTransform{};
  simd_float4 textureTransform{};
  simd_float4 textureCoordinateScale{};
  simd_float4 textureAtlas{};
  simd_float4 materialColor{};
  simd_float4 underlayColor{};
  simd_float4 underlayGradient{};
  simd_float4 presentationTint{};
  simd_uint4 modes0{};
  simd_uint4 modes1{};
  simd_uint4 modes2{};
  simd_uint4 modes3{};
  simd_uint4 modes4{};
  std::array<simd_float4, 4> gradientOffsets{};
  std::array<simd_float4, 16> gradientColors{};
};
static_assert(sizeof(TextSdfMaterialUniforms) == 976U);

id<MTLRenderPipelineState> MakeTextSdfPipeline(
    id<MTLDevice> device, id<MTLFunction> vertexFunction,
    id<MTLFunction> fragmentFunction, const bool colorWrites,
    const bool inverseBlend, const MTLPixelFormat colorFormat,
    std::string &error) {
  auto *descriptor = [[MTLRenderPipelineDescriptor alloc] init];
  descriptor.vertexFunction = vertexFunction;
  descriptor.fragmentFunction = fragmentFunction;
  descriptor.rasterSampleCount = 1;
  descriptor.colorAttachments[0].pixelFormat = colorFormat;
  descriptor.colorAttachments[0].writeMask =
      colorWrites ? MTLColorWriteMaskAll : MTLColorWriteMaskNone;
  if (inverseBlend) {
    auto *color = descriptor.colorAttachments[0];
    color.blendingEnabled = YES;
    color.rgbBlendOperation = MTLBlendOperationAdd;
    color.alphaBlendOperation = MTLBlendOperationAdd;
    color.sourceRGBBlendFactor = MTLBlendFactorOneMinusDestinationColor;
    color.destinationRGBBlendFactor = MTLBlendFactorZero;
    color.sourceAlphaBlendFactor = MTLBlendFactorOneMinusDestinationAlpha;
    color.destinationAlphaBlendFactor = MTLBlendFactorZero;
  }
  descriptor.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float_Stencil8;
  descriptor.stencilAttachmentPixelFormat =
      MTLPixelFormatDepth32Float_Stencil8;
  NSError *pipelineError = nil;
  id<MTLRenderPipelineState> pipeline =
      [device newRenderPipelineStateWithDescriptor:descriptor
                                             error:&pipelineError];
  [descriptor release];
  if (pipeline == nil)
    error = MetalErrorMessage(@"text SDF Metal pipeline creation failed",
                              pipelineError);
  return pipeline;
}

id<MTLRenderPipelineState> MakeTextSdfMaterialPipeline(
    id<MTLDevice> device, id<MTLFunction> vertexFunction,
    id<MTLFunction> fragmentFunction, const MTLPixelFormat colorFormat,
    const bool sourceOver, std::string &error) {
  auto *descriptor = [[MTLRenderPipelineDescriptor alloc] init];
  descriptor.vertexFunction = vertexFunction;
  descriptor.fragmentFunction = fragmentFunction;
  descriptor.rasterSampleCount = 1U;
  descriptor.colorAttachments[0].pixelFormat = colorFormat;
  descriptor.colorAttachments[0].writeMask = MTLColorWriteMaskAll;
  auto *color = descriptor.colorAttachments[0];
  color.blendingEnabled = sourceOver ? YES : NO;
  if (sourceOver) {
    color.rgbBlendOperation = MTLBlendOperationAdd;
    color.alphaBlendOperation = MTLBlendOperationAdd;
    color.sourceRGBBlendFactor = MTLBlendFactorOne;
    color.sourceAlphaBlendFactor = MTLBlendFactorOne;
    color.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    color.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
  }
  NSError *pipelineError = nil;
  id<MTLRenderPipelineState> pipeline =
      [device newRenderPipelineStateWithDescriptor:descriptor
                                             error:&pipelineError];
  [descriptor release];
  if (pipeline == nil) {
    error = MetalErrorMessage(
        @"VideoCut text SDF material pipeline creation failed",
        pipelineError);
  }
  return pipeline;
}

MTLStencilDescriptor *MakeStencilDescriptor(
    const MTLCompareFunction compareFunction,
    const MTLStencilOperation stencilFailure,
    const MTLStencilOperation depthFailure,
    const MTLStencilOperation pass, const std::uint32_t readMask,
    const std::uint32_t writeMask) {
  auto *descriptor = [[MTLStencilDescriptor alloc] init];
  descriptor.stencilCompareFunction = compareFunction;
  descriptor.stencilFailureOperation = stencilFailure;
  descriptor.depthFailureOperation = depthFailure;
  descriptor.depthStencilPassOperation = pass;
  descriptor.readMask = readMask;
  descriptor.writeMask = writeMask;
  return descriptor;
}

id<MTLDepthStencilState> MakeDepthStencilState(
    id<MTLDevice> device, const MTLCompareFunction depthCompare,
    const bool depthWrite, MTLStencilDescriptor *front,
    MTLStencilDescriptor *back) {
  auto *descriptor = [[MTLDepthStencilDescriptor alloc] init];
  descriptor.depthCompareFunction = depthCompare;
  descriptor.depthWriteEnabled = depthWrite ? YES : NO;
  descriptor.frontFaceStencil = front;
  descriptor.backFaceStencil = back;
  id<MTLDepthStencilState> state =
      [device newDepthStencilStateWithDescriptor:descriptor];
  [descriptor release];
  return state;
}

std::unique_ptr<TextSdfMetalState>
CreateTextSdfMetalState(id<MTLDevice> device, std::string &error) {
  auto state = std::make_unique<TextSdfMetalState>();
  state->queue = [device newCommandQueue];
  if (state->queue == nil) {
    error = "text SDF Metal command queue creation failed";
    return {};
  }

  NSError *libraryError = nil;
  id<MTLLibrary> library = [device
      newLibraryWithSource:[NSString stringWithUTF8String:kTextSdfMetalSource]
                   options:nil
                     error:&libraryError];
  if (library == nil) {
    error = MetalErrorMessage(@"text SDF Metal shader compilation failed",
                              libraryError);
    return {};
  }
  id<MTLFunction> distanceVertex =
      [library newFunctionWithName:@"textSdfDistanceVertex"];
  id<MTLFunction> materialDistanceVertex =
      [library newFunctionWithName:@"textSdfMaterialDistanceVertex"];
  id<MTLFunction> distanceFragment =
      [library newFunctionWithName:@"textSdfDistanceFragment"];
  id<MTLFunction> shapeVertex =
      [library newFunctionWithName:@"textSdfShapeVertex"];
  id<MTLFunction> materialShapeVertex =
      [library newFunctionWithName:@"textSdfMaterialShapeVertex"];
  id<MTLFunction> shapeFragment =
      [library newFunctionWithName:@"textSdfShapeFragment"];
  id<MTLFunction> inverseVertex =
      [library newFunctionWithName:@"textSdfInverseVertex"];
  id<MTLFunction> inverseFragment =
      [library newFunctionWithName:@"textSdfInverseFragment"];
  if (distanceVertex == nil || materialDistanceVertex == nil ||
      distanceFragment == nil || shapeVertex == nil ||
      materialShapeVertex == nil || shapeFragment == nil ||
      inverseVertex == nil || inverseFragment == nil) {
    error = "text SDF Metal shader entry point is unavailable";
  } else {
    state->distancePipeline = MakeTextSdfPipeline(
        device, distanceVertex, distanceFragment, true, false,
        MTLPixelFormatRGBA8Unorm, error);
    if (state->distancePipeline != nil) {
      state->shapePipeline = MakeTextSdfPipeline(
          device, shapeVertex, shapeFragment, false, false,
          MTLPixelFormatRGBA8Unorm, error);
    }
    if (state->shapePipeline != nil) {
      state->inversePipeline = MakeTextSdfPipeline(
          device, inverseVertex, inverseFragment, true, true,
          MTLPixelFormatRGBA8Unorm, error);
    }
    if (state->inversePipeline != nil) {
      state->materialDistancePipeline = MakeTextSdfPipeline(
          device, materialDistanceVertex, distanceFragment, true, false,
          MTLPixelFormatRG8Unorm, error);
    }
    if (state->materialDistancePipeline != nil) {
      state->materialShapePipeline = MakeTextSdfPipeline(
          device, materialShapeVertex, shapeFragment, false, false,
          MTLPixelFormatRG8Unorm, error);
    }
    if (state->materialShapePipeline != nil) {
      state->materialInversePipeline = MakeTextSdfPipeline(
          device, inverseVertex, inverseFragment, true, true,
          MTLPixelFormatRG8Unorm, error);
    }
  }
  [inverseFragment release];
  [inverseVertex release];
  [shapeFragment release];
  [materialShapeVertex release];
  [shapeVertex release];
  [distanceFragment release];
  [materialDistanceVertex release];
  [distanceVertex release];
  [library release];
  if (state->distancePipeline == nil || state->shapePipeline == nil ||
      state->inversePipeline == nil ||
      state->materialDistancePipeline == nil ||
      state->materialShapePipeline == nil ||
      state->materialInversePipeline == nil) {
    return {};
  }

  NSError *materialLibraryError = nil;
  id<MTLLibrary> materialLibrary = [device
      newLibraryWithSource:
          [NSString stringWithUTF8String:kTextSdfMaterialMetalSource]
                   options:nil
                     error:&materialLibraryError];
  if (materialLibrary == nil) {
    error = MetalErrorMessage(
        @"VideoCut text SDF material shader compilation failed",
        materialLibraryError);
    return {};
  }
  id<MTLFunction> materialVertex =
      [materialLibrary newFunctionWithName:@"videoCutTextSdfMaterialVertex"];
  id<MTLFunction> materialFragment = [materialLibrary
      newFunctionWithName:@"videoCutTextSdfMaterialFragment"];
  if (materialVertex == nil || materialFragment == nil) {
    error = "VideoCut text SDF material shader entry point is unavailable";
  } else {
    state->materialRgbaPipeline = MakeTextSdfMaterialPipeline(
        device, materialVertex, materialFragment, MTLPixelFormatRGBA8Unorm,
        false, error);
    if (state->materialRgbaPipeline != nil) {
      state->materialBgraPipeline = MakeTextSdfMaterialPipeline(
          device, materialVertex, materialFragment, MTLPixelFormatBGRA8Unorm,
          false, error);
    }
    if (state->materialBgraPipeline != nil) {
      state->materialSourceOverRgbaPipeline = MakeTextSdfMaterialPipeline(
          device, materialVertex, materialFragment, MTLPixelFormatRGBA8Unorm,
          true, error);
    }
    if (state->materialSourceOverRgbaPipeline != nil) {
      state->materialSourceOverBgraPipeline = MakeTextSdfMaterialPipeline(
          device, materialVertex, materialFragment, MTLPixelFormatBGRA8Unorm,
          true, error);
    }
  }
  [materialFragment release];
  [materialVertex release];
  [materialLibrary release];
  if (state->materialRgbaPipeline == nil ||
      state->materialBgraPipeline == nil ||
      state->materialSourceOverRgbaPipeline == nil ||
      state->materialSourceOverBgraPipeline == nil) {
    return {};
  }

  auto *keep = MakeStencilDescriptor(
      MTLCompareFunctionAlways, MTLStencilOperationKeep,
      MTLStencilOperationKeep, MTLStencilOperationKeep,
      std::numeric_limits<std::uint32_t>::max(),
      std::numeric_limits<std::uint32_t>::max());
  state->distanceDepthStencil = MakeDepthStencilState(
      device, MTLCompareFunctionLessEqual, true, keep, keep);
  [keep release];

  auto *increment = MakeStencilDescriptor(
      MTLCompareFunctionAlways, MTLStencilOperationKeep,
      MTLStencilOperationIncrementWrap, MTLStencilOperationIncrementWrap, 0U,
      std::numeric_limits<std::uint32_t>::max());
  state->shapeIncrementDepthStencil = MakeDepthStencilState(
      device, MTLCompareFunctionAlways, false, increment, nil);
  [increment release];

  auto *decrement = MakeStencilDescriptor(
      MTLCompareFunctionAlways, MTLStencilOperationKeep,
      MTLStencilOperationDecrementWrap, MTLStencilOperationDecrementWrap, 0U,
      std::numeric_limits<std::uint32_t>::max());
  state->shapeDecrementDepthStencil = MakeDepthStencilState(
      device, MTLCompareFunctionAlways, false, nil, decrement);
  [decrement release];

  auto *inverse = MakeStencilDescriptor(
      MTLCompareFunctionNotEqual, MTLStencilOperationZero,
      MTLStencilOperationZero, MTLStencilOperationZero, 0xffU,
      std::numeric_limits<std::uint32_t>::max());
  state->inverseDepthStencil = MakeDepthStencilState(
      device, MTLCompareFunctionAlways, false, inverse, inverse);
  [inverse release];
  state->materialDepthStencil =
      MakeDepthStencilState(device, MTLCompareFunctionAlways, false, nil, nil);
  if (state->distanceDepthStencil == nil ||
      state->shapeIncrementDepthStencil == nil ||
      state->shapeDecrementDepthStencil == nil ||
      state->inverseDepthStencil == nil ||
      state->materialDepthStencil == nil) {
    error = "text SDF Metal depth/stencil state creation failed";
    return {};
  }

  state->distanceSampler =
      MakeNormalizedClampSampler(device, MTLSamplerMinMagFilterLinear);

  state->sourceSampler =
      MakeNormalizedClampSampler(device, MTLSamplerMinMagFilterLinear);

  state->destinationSampler =
      MakeNormalizedClampSampler(device, MTLSamplerMinMagFilterNearest);
  // Captured TEXT_LETTER_MAT draws that bind a 256x1 RGBA8 gradient LUT use
  // min/mag linear filtering on sampler(1).  Texture materials use their own
  // linear sampler as well; the unused 1x1 gradient slot on texture-only draws
  // must not be mistaken for the active gradient sampler contract.
  state->gradientSampler =
      MakeNormalizedClampSampler(device, MTLSamplerMinMagFilterLinear);
  if (state->distanceSampler == nil || state->sourceSampler == nil ||
      state->destinationSampler == nil || state->gradientSampler == nil) {
    error = "VideoCut text SDF material sampler creation failed";
    return {};
  }
  error.clear();
  return state;
}

constexpr std::size_t kMaximumTextSdfTriangleVertices = 4U * 1024U * 1024U;
constexpr float kMaximumTextSdfMaterialMetric = 65'536.0F;
constexpr int kMaximumMetalTextSdfTextureExtent = 16 * 1024;
constexpr float kDegreesToRadians =
    3.14159265358979323846F / 180.0F;

bool FiniteTextSdfRect(const TextSdfGpuRect &rect,
                       const bool requirePositiveExtent) noexcept {
  return std::isfinite(rect.x) && std::isfinite(rect.y) &&
         std::isfinite(rect.width) && std::isfinite(rect.height) &&
         (!requirePositiveExtent ||
          (rect.width > 0.0F && rect.height > 0.0F));
}

bool ValidTextSdfColor(const text::Color &color) noexcept {
  const std::array<float, 4> channels{color.red, color.green, color.blue,
                                      color.alpha};
  return std::all_of(channels.begin(), channels.end(), [](const float value) {
    return std::isfinite(value) && value >= 0.0F && value <= 1.0F;
  });
}

bool ValidateTextSdfGpuMesh(const TextSdfGpuMesh &mesh, float &distanceRange,
                            std::string &error) {
  distanceRange = 0.0F;
  const auto outputRight = static_cast<std::int64_t>(mesh.outputX) +
                           static_cast<std::int64_t>(mesh.outputWidth);
  const auto outputBottom = static_cast<std::int64_t>(mesh.outputY) +
                            static_cast<std::int64_t>(mesh.outputHeight);
  if (mesh.targetWidth <= 0 || mesh.targetHeight <= 0 ||
      mesh.outputX < 0 || mesh.outputY < 0 || mesh.outputWidth <= 0 ||
      mesh.outputHeight <= 0 || outputRight > mesh.targetWidth ||
      outputBottom > mesh.targetHeight) {
    error = "text SDF mesh has invalid target or output bounds";
    return false;
  }
  if (mesh.distanceVertices.empty() || mesh.shapeVertices.empty() ||
      mesh.distanceVertices.size() % 3U != 0U ||
      mesh.shapeVertices.size() % 3U != 0U ||
      mesh.distanceVertices.size() > kMaximumTextSdfTriangleVertices ||
      mesh.shapeVertices.size() > kMaximumTextSdfTriangleVertices) {
    error = "text SDF mesh has an invalid or unbounded triangle stream";
    return false;
  }
  distanceRange = mesh.distanceVertices.front().distanceLimit;
  if (!std::isfinite(distanceRange) || distanceRange <= 0.0F ||
      distanceRange > kMaximumTextSdfMaterialMetric) {
    error = "text SDF mesh has an invalid signed-distance range";
    return false;
  }
  const float rangeTolerance =
      std::max(0.0001F, std::fabs(distanceRange) * 0.00001F);
  for (const auto &vertex : mesh.distanceVertices) {
    const std::array<float, 8> values{
        vertex.positionX,    vertex.positionY,  vertex.parabolaX,
        vertex.parabolaY,    vertex.limitBegin, vertex.limitEnd,
        vertex.distanceScale, vertex.distanceLimit};
    if (!std::all_of(values.begin(), values.end(), [](const float value) {
          return std::isfinite(value);
        }) ||
        !IsTextSdfDistanceAtlasPosition(vertex.positionX, mesh.targetWidth) ||
        !IsTextSdfDistanceAtlasPosition(vertex.positionY, mesh.targetHeight) ||
        vertex.limitBegin > vertex.limitEnd || vertex.distanceScale <= 0.0F ||
        vertex.distanceLimit <= 0.0F ||
        std::fabs(vertex.distanceLimit - distanceRange) > rangeTolerance) {
      error = "text SDF distance vertex violates the current GPU contract";
      return false;
    }
  }
  for (const auto &vertex : mesh.shapeVertices) {
    const std::array<float, 4> values{vertex.positionX, vertex.positionY,
                                      vertex.parabolaX, vertex.parabolaY};
    if (!std::all_of(values.begin(), values.end(), [](const float value) {
          return std::isfinite(value);
        }) ||
        vertex.positionX < 0.0F || vertex.positionX > 1.0F ||
        vertex.positionY < 0.0F || vertex.positionY > 1.0F) {
      error = "text SDF shape vertex violates the current GPU contract";
      return false;
    }
  }
  return true;
}

bool ResolveTextSdfBlendMode(const text::TextBlendMode blend,
                             std::uint32_t &value) noexcept {
  switch (blend) {
  case text::TextBlendMode::SourceOver:
    value = 0U;
    return true;
  case text::TextBlendMode::Multiply:
    value = 1U;
    return true;
  case text::TextBlendMode::Screen:
    value = 2U;
    return true;
  case text::TextBlendMode::Overlay:
    value = 3U;
    return true;
  case text::TextBlendMode::Add:
    value = 4U;
    return true;
  case text::TextBlendMode::Darken:
    value = 5U;
    return true;
  case text::TextBlendMode::Lighten:
    value = 6U;
    return true;
  }
  return false;
}

bool ResolveTextSdfSpread(const text::PaintSpread spread,
                          std::uint32_t &value) noexcept {
  switch (spread) {
  case text::PaintSpread::Clamp:
    value = 0U;
    return true;
  case text::PaintSpread::Repeat:
    value = 1U;
    return true;
  case text::PaintSpread::Mirror:
    value = 2U;
    return true;
  }
  return false;
}

simd_float4 TextSdfColorVector(const text::Color &color) noexcept {
  return simd_make_float4(color.red, color.green, color.blue, color.alpha);
}

simd_float4 TextSdfRectVector(const TextSdfGpuRect &rect) noexcept {
  return simd_make_float4(rect.x, rect.y, rect.width, rect.height);
}

bool FillTextSdfGradient(
    const std::vector<text::GradientStop> &stops,
    const text::GradientSampling sampling, TextSdfMaterialUniforms &uniforms,
    std::string &error) {
  if (stops.size() < 2U || stops.size() > uniforms.gradientColors.size() ||
      (sampling != text::GradientSampling::Continuous &&
       sampling != text::GradientSampling::Rgba8Lut256)) {
    error = "text SDF material has an invalid gradient contract";
    return false;
  }
  float previous = -1.0F;
  for (std::size_t index = 0U; index < stops.size(); ++index) {
    const auto &stop = stops[index];
    if (!std::isfinite(stop.offset) || stop.offset < 0.0F ||
        stop.offset > 1.0F || stop.offset <= previous ||
        !ValidTextSdfColor(stop.color)) {
      error = "text SDF material gradient stops are invalid";
      return false;
    }
    previous = stop.offset;
    uniforms.gradientOffsets[index / 4U][index % 4U] = stop.offset;
    uniforms.gradientColors[index] = TextSdfColorVector(stop.color);
  }
  uniforms.modes1.z =
      sampling == text::GradientSampling::Rgba8Lut256 ? 1U : 0U;
  uniforms.modes1.w = static_cast<std::uint32_t>(stops.size());
  return true;
}

id<MTLTexture> CreateTextSdfGradientLutTexture(
    id<MTLDevice> device, const TextSdfMaterialUniforms &uniforms,
    std::string &error) {
  if (device == nil || uniforms.modes1.z == 0U || uniforms.modes1.w < 2U ||
      uniforms.modes1.w > uniforms.gradientColors.size()) {
    error = "text SDF gradient LUT texture request is invalid";
    return nil;
  }
  std::vector<TextGradientLutStop> stops;
  stops.reserve(uniforms.modes1.w);
  for (std::uint32_t index = 0U; index < uniforms.modes1.w; ++index) {
    const simd_float4 color = uniforms.gradientColors[index];
    stops.push_back({uniforms.gradientOffsets[index / 4U][index % 4U],
                     {color.x, color.y, color.z, color.w}});
  }
  auto lut = BuildTextGradientLut256(stops, error);
  if (!lut)
    return nil;

  MTLTextureDescriptor *descriptor =
      [MTLTextureDescriptor
          texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                       width:kTextGradientLutSampleCount
                                      height:1U
                                   mipmapped:NO];
  descriptor.storageMode = MTLStorageModeShared;
  descriptor.usage = MTLTextureUsageShaderRead;
  id<MTLTexture> texture = [device newTextureWithDescriptor:descriptor];
  if (texture == nil) {
    error = "text SDF gradient LUT Metal texture allocation failed";
    return nil;
  }
  [texture replaceRegion:MTLRegionMake2D(0U, 0U,
                                         kTextGradientLutSampleCount, 1U)
            mipmapLevel:0U
              withBytes:lut->rgba.data()
            bytesPerRow:kTextGradientLutSampleCount *
                        kTextGradientLutChannelCount];
  return texture;
}

bool FillTextSdfMaterialCoordinates(
    const text::TextMaterialCoordinates &coordinates,
    TextSdfMaterialUniforms &uniforms, std::string &error) {
  if (coordinates.coordinateSpace < text::PaintCoordinateSpace::LayoutBox ||
      coordinates.coordinateSpace > text::PaintCoordinateSpace::Grapheme ||
      !std::isfinite(coordinates.coordinateOutset) ||
      coordinates.coordinateOutset < 0.0F ||
      coordinates.coordinateOutset > kMaximumTextSdfMaterialMetric ||
      !std::isfinite(coordinates.coordinateScale) ||
      coordinates.coordinateScale < 0.01F ||
      coordinates.coordinateScale > 100.0F) {
    error = "text SDF material coordinate transform is invalid";
    return false;
  }
  uniforms.radialAndCoordinates.w = coordinates.coordinateOutset;
  uniforms.materialTransform.x = coordinates.coordinateScale;
  return true;
}

const text::TextMaterial *ResolveTextSdfMaterial(
    const text::TextMaterialBinding &binding, bool &hasReplacementMask,
    const text::TextureReference *&replacementMask, std::string &error) {
  hasReplacementMask = false;
  replacementMask = nullptr;
  if (binding.valueless_by_exception()) {
    error = "text SDF material binding has no active variant";
    return nullptr;
  }
  if (const auto *literal =
          std::get_if<text::LiteralTextMaterial>(&binding)) {
    return &literal->material;
  }
  const auto *slot = std::get_if<text::EditableTextStyleSlot>(&binding);
  if (slot == nullptr || slot->semanticRole.empty() ||
      slot->semanticRole.size() > 256U) {
    error = "text SDF editable material slot is invalid";
    return nullptr;
  }
  if (slot->replacementMask) {
    hasReplacementMask = true;
    replacementMask = &*slot->replacementMask;
  }
  return &slot->fallback;
}

bool FillTextSdfTypedMaterial(
    const text::TextMaterialBinding &binding, const sk_sp<SkImage> &texture,
    const GrSurfaceOrigin textureOrigin,
    const sk_sp<SkImage> &replacementMaskImage,
    const GrSurfaceOrigin replacementMaskOrigin,
    TextSdfMaterialUniforms &uniforms, std::string &error) {
  bool hasReplacementMask = false;
  const text::TextureReference *replacementMask = nullptr;
  const text::TextMaterial *material = ResolveTextSdfMaterial(
      binding, hasReplacementMask, replacementMask, error);
  if (material == nullptr || material->valueless_by_exception()) {
    if (error.empty())
      error = "text SDF material has no active variant";
    return false;
  }

  bool requiresTexture = false;
  bool valid = std::visit(
      [&](const auto &value) {
        using Material = std::decay_t<decltype(value)>;
        if constexpr (std::is_same_v<Material,
                                     text::SolidTextMaterial>) {
          if (!ValidTextSdfColor(value.color)) {
            error = "text SDF solid material color is invalid";
            return false;
          }
          uniforms.modes0.y = 0U;
          uniforms.materialColor = TextSdfColorVector(value.color);
          uniforms.materialTransform.x = 1.0F;
          return true;
        } else if constexpr (std::is_same_v<
                                 Material,
                                 text::LinearGradientTextMaterial>) {
          std::uint32_t spread = 0U;
          if (!ResolveTextSdfSpread(value.spread, spread) ||
              !std::isfinite(value.startX) ||
              !std::isfinite(value.startY) ||
              !std::isfinite(value.endX) || !std::isfinite(value.endY) ||
              std::fabs(value.startX) > 8.0F ||
              std::fabs(value.startY) > 8.0F ||
              std::fabs(value.endX) > 8.0F ||
              std::fabs(value.endY) > 8.0F ||
              (value.startX == value.endX && value.startY == value.endY) ||
              !FillTextSdfMaterialCoordinates(value.coordinates, uniforms,
                                              error) ||
              !FillTextSdfGradient(value.stops, value.sampling, uniforms,
                                   error)) {
            if (error.empty())
              error = "text SDF linear gradient is invalid";
            return false;
          }
          uniforms.modes0.y = 1U;
          uniforms.modes0.w = spread;
          // TEXT_LETTER_MAT rotates vGradientTexcoord in normalized scope
          // coordinates. Projecting normalized endpoints into a non-square
          // pixel rectangle first changes the authored angle and LUT phase.
          const float deltaX = value.endX - value.startX;
          const float deltaY = value.endY - value.startY;
          const float lengthSquared =
              deltaX * deltaX + deltaY * deltaY;
          const float inverseLengthSquared =
              lengthSquared > 1.0e-12F ? 1.0F / lengthSquared : 0.0F;
          uniforms.linearGradient = simd_make_float4(
              value.startX, value.startY, deltaX * inverseLengthSquared,
              deltaY * inverseLengthSquared);
          return true;
        } else if constexpr (std::is_same_v<
                                 Material,
                                 text::RadialGradientTextMaterial>) {
          std::uint32_t spread = 0U;
          if (!ResolveTextSdfSpread(value.spread, spread) ||
              !std::isfinite(value.centerX) ||
              !std::isfinite(value.centerY) ||
              !std::isfinite(value.radius) || value.radius <= 0.0F ||
              std::fabs(value.centerX) > 8.0F ||
              std::fabs(value.centerY) > 8.0F || value.radius > 8.0F ||
              !FillTextSdfMaterialCoordinates(value.coordinates, uniforms,
                                              error) ||
              !FillTextSdfGradient(value.stops, value.sampling, uniforms,
                                   error)) {
            if (error.empty())
              error = "text SDF radial gradient is invalid";
            return false;
          }
          uniforms.modes0.y = 2U;
          uniforms.modes0.w = spread;
          const simd_float4 bounds =
              uniforms.modes0.z ==
                      static_cast<std::uint32_t>(
                          TextSdfCoordinateDomain::GlyphRect)
                  ? uniforms.glyphMaterialRect
                  : uniforms.modes0.z ==
                            static_cast<std::uint32_t>(
                                TextSdfCoordinateDomain::LineRect)
                        ? uniforms.lineRect
                        : uniforms.textRect;
          const float outset = uniforms.radialAndCoordinates.w;
          const float width = std::max(1.0F, bounds.z + outset * 2.0F);
          const float height = std::max(1.0F, bounds.w + outset * 2.0F);
          const float left = bounds.x - outset;
          const float top = bounds.y - outset;
          const float scale = uniforms.materialTransform.x;
          const float scaledCenterX =
              0.5F + (value.centerX - 0.5F) * scale;
          const float scaledCenterY =
              0.5F + (value.centerY - 0.5F) * scale;
          const float centerX = left + scaledCenterX * width;
          const float centerY = top + scaledCenterY * height;
          const float radius =
              value.radius * scale * std::max(width, height);
          const float inverseRadius =
              radius > 1.0e-12F ? 1.0F / radius : 0.0F;
          uniforms.radialAndCoordinates = simd_make_float4(
              centerX, centerY, inverseRadius, outset);
          return true;
        } else {
          requiresTexture = true;
          if (value.texture.assetId.empty() ||
              value.texture.sourceKind < text::TextureSourceKind::Builtin ||
              value.texture.sourceKind >
                  text::TextureSourceKind::ProjectManaged ||
              value.texture.orientation < text::TextureOrientation::Up ||
              value.texture.orientation > text::TextureOrientation::Left ||
              value.fit < text::TextureFit::Cover ||
              value.fit > text::TextureFit::Tile ||
              value.mapping < text::TextureMapping::ScopeBounds ||
              value.mapping > text::TextureMapping::GlyphDistanceField ||
              !std::isfinite(value.scale) || value.scale < 0.01F ||
              value.scale > 100.0F ||
              !std::isfinite(value.rotationDegrees) ||
              std::fabs(value.rotationDegrees) > 3'600.0F ||
              !std::isfinite(value.offsetX) ||
              !std::isfinite(value.offsetY) ||
              std::fabs(value.offsetX) > 8.0F ||
              std::fabs(value.offsetY) > 8.0F ||
              !std::isfinite(value.textureOpacity) ||
              value.textureOpacity < 0.0F || value.textureOpacity > 1.0F ||
              !std::isfinite(value.opacity) || value.opacity < 0.0F ||
              value.opacity > 1.0F || value.atlasColumns == 0U ||
              value.atlasColumns > 64U || value.atlasRows == 0U ||
              value.atlasRows > 64U ||
              !FillTextSdfMaterialCoordinates(value.coordinates, uniforms,
                                              error)) {
            if (error.empty())
              error = "text SDF texture material is invalid";
            return false;
          }
          uniforms.modes0.y = 3U;
          uniforms.modes1.x = static_cast<std::uint32_t>(value.fit);
          uniforms.modes1.y = static_cast<std::uint32_t>(value.mapping);
          uniforms.materialTransform.y = value.scale;
          uniforms.materialTransform.z =
              value.rotationDegrees * kDegreesToRadians;
          uniforms.textureTransform.x = value.offsetX;
          uniforms.textureTransform.y = value.offsetY;
          uniforms.textureAtlas.x =
              static_cast<float>(value.atlasColumns);
          uniforms.textureAtlas.y =
              static_cast<float>(value.atlasRows);
          uniforms.layerGeometry2.w = value.textureOpacity;
          uniforms.distanceAndOpacity.z *= value.opacity;
          uniforms.modes2.x = value.flipX ? 1U : 0U;
          uniforms.modes2.y = value.flipY ? 1U : 0U;
          uniforms.modes2.z =
              static_cast<std::uint32_t>(value.texture.orientation);
          uniforms.modes2.w = value.sourceAlpha ? 1U : 0U;
          if (value.underlayColor) {
            if (!ValidTextSdfColor(*value.underlayColor)) {
              error = "text SDF texture underlay color is invalid";
              return false;
            }
            uniforms.modes3.x |= 1U;
            uniforms.underlayColor =
                TextSdfColorVector(*value.underlayColor);
          }
          if (!value.underlayGradient.empty()) {
            if (!value.underlayColor) {
              const auto &projection = value.underlayGradientProjection;
              std::uint32_t spread = 0U;
              const float deltaX = projection.endX - projection.startX;
              const float deltaY = projection.endY - projection.startY;
              const float lengthSquared =
                  deltaX * deltaX + deltaY * deltaY;
              if (!ResolveTextSdfSpread(projection.spread, spread) ||
                  !std::isfinite(projection.startX) ||
                  !std::isfinite(projection.startY) ||
                  !std::isfinite(projection.endX) ||
                  !std::isfinite(projection.endY) ||
                  !(lengthSquared > 1.0e-12F)) {
                error = "text SDF texture underlay projection is invalid";
                return false;
              }
              if (!FillTextSdfGradient(
                      value.underlayGradient,
                      projection.sampling, uniforms, error)) {
                return false;
              }
              const float inverseLengthSquared = 1.0F / lengthSquared;
              uniforms.underlayGradient = simd_make_float4(
                  projection.startX, projection.startY,
                  deltaX * inverseLengthSquared,
                  deltaY * inverseLengthSquared);
              uniforms.modes3.y = 1U;
              uniforms.modes0.w = spread;
            }
          }
          return true;
        }
      },
      *material);
  if (!valid)
    return false;

  if (hasReplacementMask) {
    if (replacementMask == nullptr || replacementMask->assetId.empty() ||
        replacementMask->orientation < text::TextureOrientation::Up ||
        replacementMask->orientation > text::TextureOrientation::Left) {
      error = "text SDF replacement mask is invalid";
      return false;
    }
    uniforms.modes4.x = 1U;
    uniforms.modes4.z =
        static_cast<std::uint32_t>(replacementMask->orientation);
    uniforms.modes4.y =
        replacementMaskOrigin == kTopLeft_GrSurfaceOrigin ? 1U : 0U;
  }
  if (requiresTexture != static_cast<bool>(texture)) {
    error = requiresTexture
                ? "text SDF material requires one resolved texture"
                : "text SDF material received an unexpected texture";
    return false;
  }
  if (texture && (texture->width() <= 0 || texture->height() <= 0)) {
    error = "text SDF resolved texture has invalid dimensions";
    return false;
  }
  if (texture) {
    uniforms.textureTransform.z = static_cast<float>(texture->width());
    uniforms.textureTransform.w = static_cast<float>(texture->height());
    uniforms.modes4.w =
        textureOrigin == kTopLeft_GrSurfaceOrigin ? 1U : 0U;
  } else {
    uniforms.textureTransform.z = 1.0F;
    uniforms.textureTransform.w = 1.0F;
  }
  if (hasReplacementMask != static_cast<bool>(replacementMaskImage) ||
      (replacementMaskImage &&
       (replacementMaskImage->width() <= 0 ||
        replacementMaskImage->height() <= 0))) {
    error = hasReplacementMask
                ? "text SDF material requires its replacement mask"
                : "text SDF material received an unexpected replacement mask";
    return false;
  }
  return true;
}

bool BuildTextSdfMaterialUniforms(
    const TextSdfMaterialGpuRequest &request, const int destinationWidth,
    const int destinationHeight, const float materialDistanceRange,
    const float rasterDistanceRange,
    const GrSurfaceOrigin textureOrigin,
    const GrSurfaceOrigin replacementMaskOrigin,
    TextSdfMaterialUniforms &uniforms, std::string &error) {
  if (!FiniteTextSdfRect(request.glyphAtlasRect, true) ||
      !FiniteTextSdfRect(request.glyphLocalRect, true) ||
      !FiniteTextSdfRect(request.glyphMaterialRect, true) ||
      !FiniteTextSdfRect(request.lineRect, true) ||
      !FiniteTextSdfRect(request.textRect, true) ||
      request.glyphAtlasRect.x < 0.0F ||
      request.glyphAtlasRect.y < 0.0F ||
      request.glyphAtlasRect.x + request.glyphAtlasRect.width >
          static_cast<float>(request.mesh.targetWidth) ||
      request.glyphAtlasRect.y + request.glyphAtlasRect.height >
          static_cast<float>(request.mesh.targetHeight) ||
      request.glyphAtlasRect.x + 0.001F <
          static_cast<float>(request.mesh.outputX) ||
      request.glyphAtlasRect.y + 0.001F <
          static_cast<float>(request.mesh.outputY) ||
      request.glyphAtlasRect.x + request.glyphAtlasRect.width >
          static_cast<float>(request.mesh.outputX + request.mesh.outputWidth) +
              0.001F ||
      request.glyphAtlasRect.y + request.glyphAtlasRect.height >
          static_cast<float>(request.mesh.outputY + request.mesh.outputHeight) +
              0.001F) {
    error = "text SDF material rectangles do not match the glyph atlas";
    return false;
  }
  if (request.coordinateDomain < TextSdfCoordinateDomain::GlyphRect ||
      request.coordinateDomain > TextSdfCoordinateDomain::TextRect ||
      destinationWidth <= 0 || destinationHeight <= 0 ||
      !std::isfinite(request.materialDistanceRange) ||
      request.materialDistanceRange <= 0.0F ||
      request.materialDistanceRange > kMaximumTextSdfMaterialMetric ||
      !std::isfinite(request.authoredAtlasScale) ||
      request.authoredAtlasScale <= 0.0F ||
      request.authoredAtlasScale > kMaximumTextSdfMaterialMetric ||
      !std::isfinite(request.materialCoordinateExtent) ||
      request.materialCoordinateExtent <= 0.0F ||
      request.materialCoordinateExtent > kMaximumTextSdfMaterialMetric ||
      !std::isfinite(rasterDistanceRange) || rasterDistanceRange <= 0.0F ||
      rasterDistanceRange > kMaximumTextSdfMaterialMetric ||
      !std::isfinite(request.smoothingScale) ||
      request.smoothingScale <= 0.0F || request.smoothingScale > 4.0F ||
      !std::isfinite(request.sdfBlurRadius) || request.sdfBlurRadius < 0.0F ||
      request.sdfBlurRadius > kMaximumTextSdfMaterialMetric ||
      !std::isfinite(request.presentationOpacity) ||
      request.presentationOpacity < 0.0F ||
      request.presentationOpacity > 1.0F ||
      !std::isfinite(request.presentationTint.red) ||
      !std::isfinite(request.presentationTint.green) ||
      !std::isfinite(request.presentationTint.blue) ||
      !std::isfinite(request.presentationTint.alpha) ||
      !std::all_of(request.localToPresentation.begin(),
                   request.localToPresentation.end(), [](const float value) {
                     return std::isfinite(value);
                   })) {
    error = "text SDF material request contains invalid presentation state";
    return false;
  }
  std::uint32_t blend = 0U;
  if (!ResolveTextSdfBlendMode(request.presentationBlend, blend)) {
    error = "text SDF material blend mode is invalid";
    return false;
  }

  uniforms = {};
  std::memcpy(&uniforms.localToPresentation,
              request.localToPresentation.data(),
              sizeof(request.localToPresentation));
  std::memcpy(uniforms.qtLetterPositions.data(),
              request.qtLetterPositions.data(),
              sizeof(request.qtLetterPositions));
  std::memcpy(&uniforms.qtLetterMvp, request.qtLetterMvp.data(),
              sizeof(request.qtLetterMvp));
  uniforms.qtLetterOffset = simd_make_float4(
      request.qtLetterPolarOffsetEnabled ? request.qtLetterPolarOffset[0]
                                         : request.qtLetterOffset[0],
      request.qtLetterPolarOffsetEnabled ? request.qtLetterPolarOffset[1]
                                         : request.qtLetterOffset[1],
      request.qtLetterPolarOffsetEnabled ? 1.0F : 0.0F, 0.0F);
  uniforms.qtInnerShadowUvOffset = simd_make_float4(
      request.qtInnerShadowUvOffset[0], request.qtInnerShadowUvOffset[1],
      request.qtInnerShadowUvOffsetEnabled ? 1.0F : 0.0F, 0.0F);
  uniforms.destinationSize = simd_make_float4(
      static_cast<float>(destinationWidth),
      static_cast<float>(destinationHeight),
      1.0F / static_cast<float>(destinationWidth),
      1.0F / static_cast<float>(destinationHeight));
  uniforms.distanceTextureSize = simd_make_float4(
      static_cast<float>(request.mesh.targetWidth),
      static_cast<float>(request.mesh.targetHeight),
      1.0F / static_cast<float>(request.mesh.targetWidth),
      1.0F / static_cast<float>(request.mesh.targetHeight));
  uniforms.glyphAtlasRect = TextSdfRectVector(request.glyphAtlasRect);
  uniforms.glyphLocalRect = TextSdfRectVector(request.glyphLocalRect);
  uniforms.glyphMaterialRect = TextSdfRectVector(request.glyphMaterialRect);
  uniforms.lineRect = TextSdfRectVector(request.lineRect);
  uniforms.textRect = TextSdfRectVector(request.textRect);
  uniforms.replacementMaskRect =
      TextSdfRectVector(request.replacementMaskRect);
  uniforms.distanceAndOpacity = simd_make_float4(
      materialDistanceRange, request.sdfBlurRadius,
      request.presentationOpacity,
      request.smoothingScale);
  uniforms.presentationTint = simd_make_float4(
      std::clamp(request.presentationTint.red, 0.0F, 1.0F),
      std::clamp(request.presentationTint.green, 0.0F, 1.0F),
      std::clamp(request.presentationTint.blue, 0.0F, 1.0F),
      std::clamp(request.presentationTint.alpha, 0.0F, 1.0F));
  uniforms.materialTransform = simd_make_float4(1.0F, 1.0F, 0.0F, 0.0F);
  // The captured TextPro material coordinate uses the authored 150-unit atlas
  // scale. Nominal/CoreText record scale is only a distance-raster concern;
  // tight glyph rectangles cannot reconstruct this value without changing
  // texture phase and scale.
  uniforms.materialTransform.w =
      request.authoredAtlasScale / request.materialCoordinateExtent;
  uniforms.textureAtlas.z =
      static_cast<float>(request.materialIndex) + 0.000001F;
  // TEXT_LETTER_MAT's u_texcoordScale is applied to the interpolated SDF UV
  // before atlas-grid and authored texture scaling.  Preserve the recovered
  // canonical coordinate extent explicitly instead of reconstructing the
  // phase from fragment-local coordinates.
  uniforms.textureAtlas.w = request.materialCoordinateExtent;
  uniforms.textureCoordinateScale = simd_make_float4(
      uniforms.distanceTextureSize.x / request.materialCoordinateExtent,
      uniforms.distanceTextureSize.y / request.materialCoordinateExtent,
      0.0F, 0.0F);
  uniforms.layerGeometry2.w = 1.0F;
  uniforms.modes0.z = static_cast<std::uint32_t>(request.coordinateDomain);
  uniforms.modes3.z = blend;

  if (request.material.valueless_by_exception()) {
    error = "text SDF glyph material layer has no active variant";
    return false;
  }
  bool validLayer = std::visit(
      [&](const auto &layer) {
        using Layer = std::decay_t<decltype(layer)>;
        std::uint32_t layerBlend = 0U;
        if (layer.layerId.empty() || layer.layerId.size() > 512U ||
            !ResolveTextSdfBlendMode(layer.blend, layerBlend)) {
          error = "text SDF glyph material layer identity is invalid";
          return false;
        }
        if constexpr (std::is_same_v<Layer, text::TextFillLayer>) {
          if (!std::isfinite(layer.offsetX) ||
              !std::isfinite(layer.offsetY)) {
            error = "text SDF fill geometry is invalid";
            return false;
          }
          uniforms.modes0.x = 0U;
        } else if constexpr (std::is_same_v<Layer,
                                            text::TextStrokeLayer>) {
          if (!std::isfinite(layer.width) || layer.width < 0.0F ||
              !std::isfinite(layer.innerRingWidth) ||
              layer.innerRingWidth < 0.0F ||
              layer.innerRingWidth > layer.width ||
              (layer.signedStartWidth &&
               (!std::isfinite(*layer.signedStartWidth) ||
                std::fabs(*layer.signedStartWidth) >
                    kMaximumTextSdfMaterialMetric)) ||
              !std::isfinite(layer.offsetX) ||
              !std::isfinite(layer.offsetY) ||
              !std::isfinite(layer.blurRadius) || layer.blurRadius < 0.0F ||
              !std::isfinite(layer.spread) || layer.spread < 0.0F) {
            error = "text SDF stroke geometry is invalid";
            return false;
          }
          uniforms.modes0.x = 1U;
          uniforms.layerGeometry0 = simd_make_float4(
              layer.offsetX, layer.offsetY, layer.spread, layer.blurRadius);
          uniforms.layerGeometry1.x = layer.width;
          uniforms.layerGeometry1.y = layer.innerRingWidth;
          if (layer.signedStartWidth) {
            uniforms.layerGeometry2.w = *layer.signedStartWidth;
            uniforms.modes3.x |= 2U;
          }
        } else if constexpr (std::is_same_v<Layer,
                                            text::TextShadowLayer>) {
          if (layer.kind < text::TextShadowKind::Outer ||
              layer.kind > text::TextShadowKind::Inner ||
              !std::isfinite(layer.offsetX) ||
              !std::isfinite(layer.offsetY) ||
              !std::isfinite(layer.blurRadius) || layer.blurRadius < 0.0F ||
              !std::isfinite(layer.spread) ||
              (layer.spread < 0.0F &&
               (layer.kind != text::TextShadowKind::Outer ||
                layer.smoothing !=
                    text::TextOuterShadowSmoothingMode::Feather)) ||
              !std::isfinite(layer.thicknessAngleDegrees) ||
              !std::isfinite(layer.thicknessDistance) ||
              layer.thicknessDistance < 0.0F ||
              layer.smoothing < text::TextOuterShadowSmoothingMode::Auto ||
              layer.smoothing >
                  text::TextOuterShadowSmoothingMode::DiffuseRoundMask ||
              !std::isfinite(layer.roundMaskIntensity) ||
              layer.roundMaskIntensity < 0.0F ||
              !std::isfinite(layer.sdfBlurScale) ||
              layer.sdfBlurScale < 0.0F || layer.sdfBlurScale > 16.0F) {
            error = "text SDF shadow geometry is invalid";
            return false;
          }
          uniforms.modes0.x =
              layer.kind == text::TextShadowKind::Inner ? 3U : 2U;
          uniforms.layerGeometry0 = simd_make_float4(
              layer.offsetX, layer.offsetY, layer.spread, layer.blurRadius);
          uniforms.layerGeometry1.z = layer.thicknessDistance;
          uniforms.layerGeometry1.w =
              layer.thicknessAngleDegrees * kDegreesToRadians;
          uniforms.layerGeometry2.z = layer.roundMaskIntensity;
          // xyz retain the captured UV offset/enable ABI; w is otherwise
          // unused by Qt and carries the authored inner smoothing receipt.
          uniforms.qtInnerShadowUvOffset.w = layer.sdfBlurScale;
          uniforms.modes3.w = static_cast<std::uint32_t>(layer.smoothing);
        } else {
          if (!std::isfinite(layer.radius) || layer.radius < 0.0F ||
              !std::isfinite(layer.spread) || layer.spread < 0.0F ||
              !std::isfinite(layer.directionX) ||
              !std::isfinite(layer.directionY) ||
              std::fabs(layer.directionX) > 1.0F ||
              std::fabs(layer.directionY) > 1.0F) {
            error = "text SDF glow geometry is invalid";
            return false;
          }
          uniforms.modes0.x = 4U;
          uniforms.layerGeometry0 = simd_make_float4(
              layer.directionX * layer.radius,
              layer.directionY * layer.radius, layer.spread, layer.radius);
          uniforms.layerGeometry2.x = layer.directionX;
          uniforms.layerGeometry2.y = layer.directionY;
        }
        const std::array<float, 8> metrics{
            uniforms.layerGeometry0.x, uniforms.layerGeometry0.y,
            uniforms.layerGeometry0.z, uniforms.layerGeometry0.w,
            uniforms.layerGeometry1.x, uniforms.layerGeometry1.y,
            uniforms.layerGeometry1.z, uniforms.layerGeometry2.z};
        if (!std::all_of(metrics.begin(), metrics.end(), [](const float value) {
              return std::isfinite(value) &&
                     std::fabs(value) <= kMaximumTextSdfMaterialMetric;
            })) {
          error = "text SDF glyph material metric exceeds GPU bounds";
          return false;
        }
        return FillTextSdfTypedMaterial(
            layer.material, request.texture, textureOrigin,
            request.replacementMask, replacementMaskOrigin, uniforms, error);
      },
      request.material);
  if (validLayer &&
      std::getenv("VIDEOCUT_TRACE_TEXT_SDF_MATERIAL_UNIFORMS") != nullptr) {
    const auto layerId = std::visit(
        [](const auto &layer) -> const std::string & { return layer.layerId; },
        request.material);
    std::fprintf(
        stderr,
        "text-sdf-material-uniforms layer=%s layer_kind=%u material_kind=%u "
        "coordinate_domain=%u blend=%u smoothing_mode=%u "
        "distance_range=%.9g raster_distance_range=%.9g "
        "smoothing_scale=%.9g requested_blur=%.9g opacity=%.9g "
        "material_color=%.9g,%.9g,%.9g,%.9g "
        "presentation_tint=%.9g,%.9g,%.9g,%.9g "
        "qt_letter_offset=%.9g,%.9g,%.9g,%.9g "
        "qt_inner_shadow_uv=%.9g,%.9g,%.9g,%.9g "
        "modes1=%u,%u,%u,%u modes2=%u,%u,%u,%u "
        "modes3=%u,%u,%u,%u modes4=%u,%u,%u,%u "
        "material_transform=%.9g,%.9g,%.9g,%.9g "
        "texture_atlas=%.9g,%.9g,%.9g,%.9g "
        "geometry0=%.9g,%.9g,%.9g,%.9g "
        "geometry1=%.9g,%.9g,%.9g,%.9g "
        "geometry2=%.9g,%.9g,%.9g,%.9g "
        "atlas_rect=%.9g,%.9g,%.9g,%.9g "
        "local_rect=%.9g,%.9g,%.9g,%.9g "
        "local_to_presentation="
        "%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,"
        "%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g "
        "qt_position0=%.9g,%.9g,%.9g,%.9g "
        "qt_mvp=%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,"
        "%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g\n",
        layerId.c_str(), uniforms.modes0.x, uniforms.modes0.y,
        uniforms.modes0.z, uniforms.modes3.z, uniforms.modes3.w,
        materialDistanceRange, rasterDistanceRange,
        request.smoothingScale, request.sdfBlurRadius,
        request.presentationOpacity, uniforms.materialColor.x,
        uniforms.materialColor.y, uniforms.materialColor.z,
        uniforms.materialColor.w, uniforms.presentationTint.x,
        uniforms.presentationTint.y, uniforms.presentationTint.z,
        uniforms.presentationTint.w, uniforms.qtLetterOffset.x,
        uniforms.qtLetterOffset.y, uniforms.qtLetterOffset.z,
        uniforms.qtLetterOffset.w, uniforms.qtInnerShadowUvOffset.x,
        uniforms.qtInnerShadowUvOffset.y, uniforms.qtInnerShadowUvOffset.z,
        uniforms.qtInnerShadowUvOffset.w, uniforms.modes1.x,
        uniforms.modes1.y, uniforms.modes1.z, uniforms.modes1.w,
        uniforms.modes2.x, uniforms.modes2.y, uniforms.modes2.z,
        uniforms.modes2.w, uniforms.modes3.x, uniforms.modes3.y,
        uniforms.modes3.z, uniforms.modes3.w, uniforms.modes4.x,
        uniforms.modes4.y, uniforms.modes4.z, uniforms.modes4.w,
        uniforms.materialTransform.x, uniforms.materialTransform.y,
        uniforms.materialTransform.z, uniforms.materialTransform.w,
        uniforms.textureAtlas.x, uniforms.textureAtlas.y,
        uniforms.textureAtlas.z, uniforms.textureAtlas.w,
        uniforms.layerGeometry0.x,
        uniforms.layerGeometry0.y, uniforms.layerGeometry0.z,
        uniforms.layerGeometry0.w, uniforms.layerGeometry1.x,
        uniforms.layerGeometry1.y, uniforms.layerGeometry1.z,
        uniforms.layerGeometry1.w, uniforms.layerGeometry2.x,
        uniforms.layerGeometry2.y, uniforms.layerGeometry2.z,
        uniforms.layerGeometry2.w, uniforms.glyphAtlasRect.x,
        uniforms.glyphAtlasRect.y, uniforms.glyphAtlasRect.z,
        uniforms.glyphAtlasRect.w, uniforms.glyphLocalRect.x,
        uniforms.glyphLocalRect.y, uniforms.glyphLocalRect.z,
        uniforms.glyphLocalRect.w,
        request.localToPresentation[0], request.localToPresentation[1],
        request.localToPresentation[2], request.localToPresentation[3],
        request.localToPresentation[4], request.localToPresentation[5],
        request.localToPresentation[6], request.localToPresentation[7],
        request.localToPresentation[8], request.localToPresentation[9],
        request.localToPresentation[10], request.localToPresentation[11],
        request.localToPresentation[12], request.localToPresentation[13],
        request.localToPresentation[14], request.localToPresentation[15],
        request.qtLetterPositions[0], request.qtLetterPositions[1],
        request.qtLetterPositions[2], request.qtLetterPositions[3],
        request.qtLetterMvp[0], request.qtLetterMvp[1],
        request.qtLetterMvp[2], request.qtLetterMvp[3],
        request.qtLetterMvp[4], request.qtLetterMvp[5],
        request.qtLetterMvp[6], request.qtLetterMvp[7],
        request.qtLetterMvp[8], request.qtLetterMvp[9],
        request.qtLetterMvp[10], request.qtLetterMvp[11],
        request.qtLetterMvp[12], request.qtLetterMvp[13],
        request.qtLetterMvp[14], request.qtLetterMvp[15]);
  }
  if (validLayer && uniforms.modes0.y == 3U &&
      std::getenv("VIDEOCUT_TRACE_TEXT_SDF_TEXTURE_UNIFORMS") != nullptr) {
    const auto hashBytes = [](const void *data, const std::size_t size) {
      const auto *bytes = static_cast<const std::uint8_t *>(data);
      std::uint64_t hash = 1469598103934665603ULL;
      for (std::size_t index = 0; index < size; ++index) {
        hash ^= bytes[index];
        hash *= 1099511628211ULL;
      }
      return hash;
    };
    const std::size_t distanceByteCount =
        request.mesh.distanceVertices.size() * sizeof(TextSdfDistanceVertex);
    const std::size_t shapeByteCount =
        request.mesh.shapeVertices.size() * sizeof(TextSdfShapeVertex);
    std::fprintf(
        stderr,
        "text-sdf-texture-uniforms target=%ux%u output=%d,%d,%d,%d "
        "atlas_rect=%.9g,%.9g,%.9g,%.9g material_extent=%.9g "
        "local_rect=%.9g,%.9g,%.9g,%.9g "
        "material_rect=%.9g,%.9g,%.9g,%.9g "
        "local_to_presentation="
        "%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,"
        "%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g,%.9g "
        "authored_atlas_scale=%.9g texcoord_scale=%.9g,%.9g "
        "texture_grid=%.9g,%.9g texture_scale=%.9g rotation_rad=%.9g "
        "offset=%.9g,%.9g material_index=%u texture_origin=%u "
        "orientation=%u flip=%u,%u fit=%u mapping=%u "
        "distance_vertices=%zu distance_hash=%016llx "
        "shape_vertices=%zu shape_hash=%016llx\n",
        request.mesh.targetWidth, request.mesh.targetHeight,
        request.mesh.outputX, request.mesh.outputY, request.mesh.outputWidth,
        request.mesh.outputHeight, uniforms.glyphAtlasRect.x,
        uniforms.glyphAtlasRect.y, uniforms.glyphAtlasRect.z,
        uniforms.glyphAtlasRect.w, request.materialCoordinateExtent,
        uniforms.glyphLocalRect.x, uniforms.glyphLocalRect.y,
        uniforms.glyphLocalRect.z, uniforms.glyphLocalRect.w,
        uniforms.glyphMaterialRect.x, uniforms.glyphMaterialRect.y,
        uniforms.glyphMaterialRect.z, uniforms.glyphMaterialRect.w,
        request.localToPresentation[0], request.localToPresentation[1],
        request.localToPresentation[2], request.localToPresentation[3],
        request.localToPresentation[4], request.localToPresentation[5],
        request.localToPresentation[6], request.localToPresentation[7],
        request.localToPresentation[8], request.localToPresentation[9],
        request.localToPresentation[10], request.localToPresentation[11],
        request.localToPresentation[12], request.localToPresentation[13],
        request.localToPresentation[14], request.localToPresentation[15],
        request.authoredAtlasScale, uniforms.textureCoordinateScale.x,
        uniforms.textureCoordinateScale.y,
        uniforms.textureAtlas.x, uniforms.textureAtlas.y,
        uniforms.materialTransform.y, uniforms.materialTransform.z,
        uniforms.textureTransform.x, uniforms.textureTransform.y,
        request.materialIndex, static_cast<unsigned>(textureOrigin),
        uniforms.modes2.z, uniforms.modes2.x, uniforms.modes2.y,
        uniforms.modes1.x, uniforms.modes1.y,
        request.mesh.distanceVertices.size(),
        static_cast<unsigned long long>(hashBytes(
            request.mesh.distanceVertices.data(), distanceByteCount)),
        request.mesh.shapeVertices.size(),
        static_cast<unsigned long long>(hashBytes(
            request.mesh.shapeVertices.data(), shapeByteCount)));
  }
  return validLayer;
}

struct TextSdfEncodingResources final {
  id<MTLTexture> depthTexture{nil};
  id<MTLBuffer> distanceBuffer{nil};
  id<MTLBuffer> shapeBuffer{nil};

  ~TextSdfEncodingResources() {
    [shapeBuffer release];
    [distanceBuffer release];
    [depthTexture release];
  }
};

bool EncodeTextSdfDistance(const TextSdfGpuMesh &mesh,
                           TextSdfMetalState &state, id<MTLDevice> device,
                           id<MTLCommandBuffer> commandBuffer,
                           id<MTLTexture> distanceTexture,
                           const bool topLeftCoordinates,
                           TextSdfEncodingResources &resources,
                           std::string &error) {
  if (commandBuffer == nil || distanceTexture == nil ||
      distanceTexture.width != static_cast<NSUInteger>(mesh.targetWidth) ||
      distanceTexture.height != static_cast<NSUInteger>(mesh.targetHeight) ||
      (topLeftCoordinates
           ? distanceTexture.pixelFormat != MTLPixelFormatRG8Unorm
           : distanceTexture.pixelFormat != MTLPixelFormatRGBA8Unorm)) {
    error = "text SDF distance target does not match its mesh";
    return false;
  }
  MTLTextureDescriptor *depthDescriptor =
      [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:
                                MTLPixelFormatDepth32Float_Stencil8
                                                   width:mesh.targetWidth
                                                  height:mesh.targetHeight
                                               mipmapped:NO];
  depthDescriptor.storageMode = MTLStorageModePrivate;
  depthDescriptor.usage = MTLTextureUsageRenderTarget;
  resources.depthTexture = [device newTextureWithDescriptor:depthDescriptor];
  resources.distanceBuffer = [device
      newBufferWithBytes:mesh.distanceVertices.data()
                    length:mesh.distanceVertices.size() *
                           sizeof(TextSdfDistanceVertex)
                   options:MTLResourceStorageModeShared];
  resources.shapeBuffer = [device
      newBufferWithBytes:mesh.shapeVertices.data()
                    length:mesh.shapeVertices.size() *
                           sizeof(TextSdfShapeVertex)
                   options:MTLResourceStorageModeShared];
  if (resources.depthTexture == nil || resources.distanceBuffer == nil ||
      resources.shapeBuffer == nil) {
    error = "text SDF distance resource allocation failed";
    return false;
  }

  MTLRenderPassDescriptor *pass =
      [MTLRenderPassDescriptor renderPassDescriptor];
  pass.colorAttachments[0].texture = distanceTexture;
  pass.colorAttachments[0].loadAction = MTLLoadActionClear;
  pass.colorAttachments[0].storeAction = MTLStoreActionStore;
  pass.colorAttachments[0].clearColor =
      MTLClearColorMake(0.0, 0.0, 0.0, 0.0);
  pass.depthAttachment.texture = resources.depthTexture;
  pass.depthAttachment.loadAction = MTLLoadActionClear;
  pass.depthAttachment.storeAction = MTLStoreActionDontCare;
  pass.depthAttachment.clearDepth = 1.0;
  pass.stencilAttachment.texture = resources.depthTexture;
  pass.stencilAttachment.loadAction = MTLLoadActionClear;
  pass.stencilAttachment.storeAction = MTLStoreActionDontCare;
  pass.stencilAttachment.clearStencil = 0U;
  id<MTLRenderCommandEncoder> encoder =
      [commandBuffer renderCommandEncoderWithDescriptor:pass];
  if (encoder == nil) {
    error = "text SDF distance encoder creation failed";
    return false;
  }
  const MTLViewport viewport =
      topLeftCoordinates
          ? MTLViewport{0.0, 0.0, static_cast<double>(mesh.targetWidth),
                        static_cast<double>(mesh.targetHeight), 0.0, 1.0}
          : MTLViewport{0.0, static_cast<double>(mesh.targetHeight),
                        static_cast<double>(mesh.targetWidth),
                        -static_cast<double>(mesh.targetHeight), 0.0, 1.0};
  [encoder setViewport:viewport];
  [encoder setScissorRect:MTLScissorRect{
                              0U, 0U,
                              static_cast<NSUInteger>(mesh.targetWidth),
                              static_cast<NSUInteger>(mesh.targetHeight)}];
  [encoder setFrontFacingWinding:MTLWindingCounterClockwise];
  [encoder setStencilReferenceValue:0U];

  [encoder setRenderPipelineState:
               topLeftCoordinates ? state.materialDistancePipeline
                                  : state.distancePipeline];
  [encoder setDepthStencilState:state.distanceDepthStencil];
  [encoder setCullMode:MTLCullModeNone];
  [encoder setVertexBuffer:resources.distanceBuffer offset:0U atIndex:0U];
  [encoder drawPrimitives:MTLPrimitiveTypeTriangle
                vertexStart:0U
                vertexCount:mesh.distanceVertices.size()];

  [encoder setRenderPipelineState:
               topLeftCoordinates ? state.materialShapePipeline
                                  : state.shapePipeline];
  [encoder setVertexBuffer:resources.shapeBuffer offset:0U atIndex:0U];
  [encoder setDepthStencilState:state.shapeIncrementDepthStencil];
  [encoder setCullMode:MTLCullModeBack];
  [encoder drawPrimitives:MTLPrimitiveTypeTriangle
                vertexStart:0U
                vertexCount:mesh.shapeVertices.size()];
  [encoder setDepthStencilState:state.shapeDecrementDepthStencil];
  [encoder setCullMode:MTLCullModeFront];
  [encoder drawPrimitives:MTLPrimitiveTypeTriangle
                vertexStart:0U
                vertexCount:mesh.shapeVertices.size()];

  [encoder setRenderPipelineState:
               topLeftCoordinates ? state.materialInversePipeline
                                  : state.inversePipeline];
  [encoder setDepthStencilState:state.inverseDepthStencil];
  [encoder setCullMode:topLeftCoordinates ? MTLCullModeNone
                                          : MTLCullModeFront];
  [encoder drawPrimitives:MTLPrimitiveTypeTriangle
                vertexStart:0U
                vertexCount:6U];
  [encoder endEncoding];
  return true;
}

// AmazingEngine reuses TextPro's 108-byte Letter record for the canonical
// RenderGroup quad. Only position (float offset 0) and texcoord (offset 11)
// are consumed by the captured _RENDER_GROUP shader; retaining the complete
// stride keeps the stage-in ABI identical to Qt.
struct QtTextRenderGroupCompositeVertex final {
  float values[27]{};
};
static_assert(sizeof(QtTextRenderGroupCompositeVertex) == 108U);

std::array<QtTextRenderGroupCompositeVertex, 4>
QtTextRenderGroupCompositeVertices() {
  std::array<QtTextRenderGroupCompositeVertex, 4> vertices{};
  vertices[0].values[0] = -1.0F;
  vertices[0].values[1] = 1.0F;
  vertices[0].values[11] = 0.0F;
  vertices[0].values[12] = 1.0F;
  vertices[1].values[0] = 1.0F;
  vertices[1].values[1] = 1.0F;
  vertices[1].values[11] = 1.0F;
  vertices[1].values[12] = 1.0F;
  vertices[2].values[0] = 1.0F;
  vertices[2].values[1] = -1.0F;
  vertices[2].values[11] = 1.0F;
  vertices[2].values[12] = 0.0F;
  vertices[3].values[0] = -1.0F;
  vertices[3].values[1] = -1.0F;
  vertices[3].values[11] = 0.0F;
  vertices[3].values[12] = 0.0F;
  return vertices;
}

struct QtTextRenderGroupCompositeMetalState final {
  id<MTLRenderPipelineState> pipeline{nil};
  id<MTLDepthStencilState> depthStencil{nil};
  id<MTLSamplerState> sampler{nil};
  id<MTLBuffer> vertices{nil};
  id<MTLBuffer> indices{nil};
  MTLPixelFormat colorFormat{MTLPixelFormatInvalid};

  ~QtTextRenderGroupCompositeMetalState() {
    [indices release];
    [vertices release];
    [sampler release];
    [depthStencil release];
    [pipeline release];
  }
};

struct QtTextFollowerCompositeVertex final {
  float positionX{0.0F};
  float positionY{0.0F};
  float texcoordX{0.0F};
  float texcoordY{0.0F};
};
static_assert(sizeof(QtTextFollowerCompositeVertex) == 16U);

struct QtTextFollowerCompositeMetalState final {
  id<MTLRenderPipelineState> pipeline{nil};
  id<MTLSamplerState> sampler{nil};
  MTLPixelFormat colorFormat{MTLPixelFormatInvalid};

  ~QtTextFollowerCompositeMetalState() {
    [sampler release];
    [pipeline release];
  }
};

struct TextVatMeshMetalState final {
  id<MTLRenderPipelineState> pipeline{nil};
  id<MTLSamplerState> sampler{nil};
  MTLPixelFormat colorFormat{MTLPixelFormatInvalid};

  ~TextVatMeshMetalState() {
    [sampler release];
    [pipeline release];
  }
};

static_assert(sizeof(TextVatMeshVertex) == 24U);

std::unique_ptr<TextVatMeshMetalState>
CreateTextVatMeshMetalState(id<MTLDevice> device,
                              const MTLPixelFormat colorFormat,
                              std::string &error) {
  auto state = std::make_unique<TextVatMeshMetalState>();
  NSError *libraryError = nil;
  id<MTLLibrary> library = [device
      newLibraryWithSource:
          [NSString stringWithUTF8String:kTextVatMeshMetalSource]
                   options:nil
                     error:&libraryError];
  if (library == nil) {
    error = MetalErrorMessage(@"text VAT mesh shader compilation failed",
                              libraryError);
    return {};
  }
  id<MTLFunction> vertex =
      [library newFunctionWithName:@"textVatMeshVertex"];
  id<MTLFunction> fragment =
      [library newFunctionWithName:@"textVatMeshFragment"];
  if (vertex == nil || fragment == nil) {
    error = "text VAT mesh shader entry point is unavailable";
  } else {
    auto *descriptor = [[MTLRenderPipelineDescriptor alloc] init];
    descriptor.vertexFunction = vertex;
    descriptor.fragmentFunction = fragment;
    descriptor.rasterSampleCount = 1U;
    auto *color = descriptor.colorAttachments[0];
    color.pixelFormat = colorFormat;
    color.blendingEnabled = YES;
    color.rgbBlendOperation = MTLBlendOperationAdd;
    color.alphaBlendOperation = MTLBlendOperationAdd;
    color.sourceRGBBlendFactor = MTLBlendFactorOne;
    color.sourceAlphaBlendFactor = MTLBlendFactorOne;
    color.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    color.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    color.writeMask = MTLColorWriteMaskAll;
    NSError *pipelineError = nil;
    state->pipeline =
        [device newRenderPipelineStateWithDescriptor:descriptor
                                               error:&pipelineError];
    if (state->pipeline == nil) {
      error = MetalErrorMessage(@"text VAT mesh pipeline creation failed",
                                pipelineError);
    }
    [descriptor release];
  }
  [fragment release];
  [vertex release];
  [library release];
  if (state->pipeline == nil)
    return {};
  state->colorFormat = colorFormat;

  state->sampler =
      MakeNormalizedClampSampler(device, MTLSamplerMinMagFilterLinear);
  if (state->sampler == nil) {
    error = "text VAT mesh immutable Metal resource creation failed";
    return {};
  }
  error.clear();
  return state;
}

std::unique_ptr<QtTextFollowerCompositeMetalState>
CreateQtTextFollowerCompositeMetalState(id<MTLDevice> device,
                                          const MTLPixelFormat colorFormat,
                                          std::string &error) {
  auto state = std::make_unique<QtTextFollowerCompositeMetalState>();
  NSError *libraryError = nil;
  id<MTLLibrary> library = [device
      newLibraryWithSource:
          [NSString stringWithUTF8String:
                        kQtTextFollowerCompositeMetalSource]
                   options:nil
                     error:&libraryError];
  if (library == nil) {
    error = MetalErrorMessage(@"Qt follower shader compilation failed",
                              libraryError);
    return {};
  }
  id<MTLFunction> vertex = [library newFunctionWithName:@"main0"];
  id<MTLFunction> fragment =
      [library newFunctionWithName:@"followerFragment"];
  if (vertex == nil || fragment == nil) {
    error = "Qt follower shader entry point is unavailable";
  } else {
    auto *descriptor = [[MTLRenderPipelineDescriptor alloc] init];
    descriptor.vertexFunction = vertex;
    descriptor.fragmentFunction = fragment;
    descriptor.rasterSampleCount = 1U;
    auto *color = descriptor.colorAttachments[0];
    color.pixelFormat = colorFormat;
    color.blendingEnabled = YES;
    color.rgbBlendOperation = MTLBlendOperationAdd;
    color.alphaBlendOperation = MTLBlendOperationAdd;
    color.sourceRGBBlendFactor = MTLBlendFactorOne;
    color.sourceAlphaBlendFactor = MTLBlendFactorOne;
    color.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    color.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    color.writeMask = MTLColorWriteMaskAll;
    NSError *pipelineError = nil;
    state->pipeline =
        [device newRenderPipelineStateWithDescriptor:descriptor
                                               error:&pipelineError];
    if (state->pipeline == nil) {
      error = MetalErrorMessage(@"Qt follower Metal pipeline creation failed",
                                pipelineError);
    }
    [descriptor release];
  }
  [fragment release];
  [vertex release];
  [library release];
  if (state->pipeline == nil)
    return {};
  state->colorFormat = colorFormat;

  state->sampler =
      MakeNormalizedClampSampler(device, MTLSamplerMinMagFilterLinear);
  if (state->sampler == nil) {
    error = "Qt follower Metal sampler creation failed";
    return {};
  }
  error.clear();
  return state;
}

std::unique_ptr<QtTextRenderGroupCompositeMetalState>
CreateQtTextRenderGroupCompositeMetalState(id<MTLDevice> device,
                                             const MTLPixelFormat colorFormat,
                                             std::string &error) {
  auto state = std::make_unique<QtTextRenderGroupCompositeMetalState>();
  NSError *vertexError = nil;
  NSError *fragmentError = nil;
  id<MTLLibrary> vertexLibrary = [device
      newLibraryWithSource:
          [NSString stringWithUTF8String:
                        kQtTextRenderGroupCompositeVertexSource]
                   options:nil
                     error:&vertexError];
  id<MTLLibrary> fragmentLibrary = [device
      newLibraryWithSource:
          [NSString stringWithUTF8String:
                        kQtTextRenderGroupCompositeFragmentSource]
                   options:nil
                     error:&fragmentError];
  if (vertexLibrary == nil || fragmentLibrary == nil) {
    error = vertexLibrary == nil
                ? MetalErrorMessage(
                      @"Qt RenderGroup vertex shader compilation failed",
                      vertexError)
                : MetalErrorMessage(
                      @"Qt RenderGroup fragment shader compilation failed",
                      fragmentError);
    [fragmentLibrary release];
    [vertexLibrary release];
    return {};
  }
  id<MTLFunction> vertex = [vertexLibrary newFunctionWithName:@"main0"];
  id<MTLFunction> fragment =
      [fragmentLibrary newFunctionWithName:@"main0"];
  if (vertex == nil || fragment == nil) {
    error = "Qt RenderGroup shader entry point is unavailable";
  } else {
    auto *layout = [[MTLVertexDescriptor alloc] init];
    layout.attributes[0].format = MTLVertexFormatFloat3;
    layout.attributes[0].offset = 0U;
    layout.attributes[0].bufferIndex = 30U;
    layout.attributes[1].format = MTLVertexFormatFloat4;
    layout.attributes[1].offset = 44U;
    layout.attributes[1].bufferIndex = 30U;
    layout.layouts[30].stride = 108U;
    layout.layouts[30].stepFunction = MTLVertexStepFunctionPerVertex;
    layout.layouts[30].stepRate = 1U;

    auto *descriptor = [[MTLRenderPipelineDescriptor alloc] init];
    descriptor.vertexFunction = vertex;
    descriptor.fragmentFunction = fragment;
    descriptor.vertexDescriptor = layout;
    descriptor.rasterSampleCount = 1U;
    descriptor.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
    auto *color = descriptor.colorAttachments[0];
    color.pixelFormat = colorFormat;
    color.blendingEnabled = YES;
    color.rgbBlendOperation = MTLBlendOperationAdd;
    color.alphaBlendOperation = MTLBlendOperationAdd;
    color.sourceRGBBlendFactor = MTLBlendFactorOne;
    color.sourceAlphaBlendFactor = MTLBlendFactorOne;
    color.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    color.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    color.writeMask = MTLColorWriteMaskAll;
    NSError *pipelineError = nil;
    state->pipeline =
        [device newRenderPipelineStateWithDescriptor:descriptor
                                               error:&pipelineError];
    if (state->pipeline == nil) {
      error = MetalErrorMessage(
          @"Qt RenderGroup Metal pipeline creation failed", pipelineError);
    }
    [descriptor release];
    [layout release];
  }
  [fragment release];
  [vertex release];
  [fragmentLibrary release];
  [vertexLibrary release];
  if (state->pipeline == nil)
    return {};
  state->colorFormat = colorFormat;

  auto *depthDescriptor = [[MTLDepthStencilDescriptor alloc] init];
  depthDescriptor.depthCompareFunction = MTLCompareFunctionAlways;
  depthDescriptor.depthWriteEnabled = NO;
  state->depthStencil =
      [device newDepthStencilStateWithDescriptor:depthDescriptor];
  [depthDescriptor release];

  state->sampler =
      MakeNormalizedClampSampler(device, MTLSamplerMinMagFilterLinear);

  const auto vertices = QtTextRenderGroupCompositeVertices();
  constexpr std::array<std::uint16_t, 6> indices{0U, 2U, 1U,
                                                 0U, 3U, 2U};
  state->vertices =
      [device newBufferWithBytes:vertices.data()
                          length:sizeof(vertices)
                         options:MTLResourceStorageModeShared];
  state->indices =
      [device newBufferWithBytes:indices.data()
                          length:sizeof(indices)
                         options:MTLResourceStorageModeShared];
  if (state->depthStencil == nil || state->sampler == nil ||
      state->vertices == nil || state->indices == nil) {
    error = "Qt RenderGroup immutable Metal resource creation failed";
    return {};
  }
  error.clear();
  return state;
}

class MetalSkiaGpuContext final : public SkiaGpuContext {
public:
  static std::unique_ptr<MetalSkiaGpuContext> Create(
      std::string &error, std::int32_t deviceIndex, std::uint64_t generation) {
    gpu_execution::DeviceRequest request;
    request.preferred = gpu_execution::Backend::Metal;
    request.deviceIndex = deviceIndex;
    request.generation = generation;
    auto executionDevice = gpu_execution::Device::Create(request, error);
    if (!executionDevice) {
      if (error.empty())
        error = "product Metal device is unavailable";
      return {};
    }
    id<MTLDevice> device = (__bridge id<MTLDevice>)(
        reinterpret_cast<void *>(executionDevice->nativeDeviceHandle()));
    if (!device) {
      error = "product Metal device has no native handle";
      return {};
    }
    id<MTLCommandQueue> queue = [device newCommandQueue];
    if (!queue) {
      error = "Metal command queue creation failed";
      return {};
    }

    GrMtlBackendContext backend;
    backend.fDevice.retain((__bridge GrMTLHandle)device);
    // newCommandQueue returns an owned object. sk_cfp adopts that retain;
    // Ganesh takes its own retain when the direct context is created.
    backend.fQueue.reset((__bridge GrMTLHandle)queue);
    auto context = GrDirectContexts::MakeMetal(backend);
    if (!context) {
      error = "Skia Ganesh Metal context creation failed";
      return {};
    }
    context->setResourceCacheLimit(128U * 1024U * 1024U);
    // Keep the exact queue passed to Ganesh. The native final composite is an
    // external Metal write, and queue ordering is the zero-copy synchronization
    // contract between preceding Skia work and subsequent readback/draw work.
    id<MTLCommandQueue> ganeshQueue = [queue retain];
    error.clear();
    return std::unique_ptr<MetalSkiaGpuContext>(
        new MetalSkiaGpuContext(std::move(executionDevice),
                                std::move(context), ganeshQueue));
  }

  ~MetalSkiaGpuContext() override {
    textVatMeshState_.reset();
    qtTextFollowerCompositeState_.reset();
    qtTextRenderGroupCompositeState_.reset();
    textSdfState_.reset();
    if (context_)
      context_->releaseResourcesAndAbandonContext();
    [publicationPipeline_ release];
    [ganeshQueue_ release];
  }

  sk_sp<SkSurface> MakeSurface(const SkImageInfo &info,
                               std::string &error) override {
    if (!context_ || context_->abandoned()) {
      error = "Skia Ganesh Metal context is unavailable";
      return {};
    }
    auto surface = SkSurfaces::RenderTarget(
        context_.get(), skgpu::Budgeted::kNo, info, 0,
        kTopLeft_GrSurfaceOrigin, nullptr);
    if (!surface) {
      error = "Skia Ganesh Metal surface allocation failed: size=" +
              std::to_string(info.width()) + "x" +
              std::to_string(info.height()) + " color_type=" +
              std::to_string(static_cast<int>(info.colorType())) +
              " supported=" +
              (context_->colorTypeSupportedAsSurface(info.colorType())
                   ? std::string("1")
                   : std::string("0")) +
              " max_render_target=" +
              std::to_string(context_->maxRenderTargetSize()) + " oomed=" +
              (context_->oomed() ? std::string("1") : std::string("0"));
      return {};
    }
    error.clear();
    return surface;
  }

  bool WaitForSubmittedWork(std::string &error) override {
    @autoreleasepool {
      if (!context_ || context_->abandoned()) {
        error = "Skia GPU context is unavailable";
        return false;
      }
      FlushGpu();
      if (!SubmitNative([ganeshQueue_ commandBuffer], error, 0U))
        return false;
      return completionTimeline_->Wait(lastSubmission_, error);
    }
  }

  PublishedFrame PublishFrame(
      SkSurface &surface, std::int64_t timestampUs, std::uint64_t generation,
      bool premultiplied, const std::function<bool()> &cancel) override {
    @autoreleasepool {
      PublishedFrame result;
      if (cancel && cancel()) {
        result.error = "canceled";
        return result;
      }
      if (!context_ || context_->abandoned() ||
          surface.recordingContext() != context_.get()) {
        result.error = "GPU publication requires this lane's Ganesh surface";
        return result;
      }
      const auto info = surface.imageInfo();
      const bool f32 = info.colorType() == kRGBA_F16_SkColorType;
      if ((info.colorType() != kRGBA_8888_SkColorType && !f32) ||
          (info.alphaType() != kPremul_SkAlphaType &&
           info.alphaType() != kUnpremul_SkAlphaType)) {
        result.error = "GPU publication requires canonical RGBA8 or RGBAF16";
        return result;
      }
      const auto backend = SkSurfaces::GetBackendTexture(
          &surface, SkSurface::BackendHandleAccess::kFlushRead);
      GrMtlTextureInfo metal;
      if (!GrBackendTextures::GetMtlTextureInfo(backend, &metal) ||
          !metal.fTexture) {
        result.error = "GPU publication could not borrow the Skia texture";
        return result;
      }
      id<MTLTexture> source = (__bridge id<MTLTexture>)metal.fTexture.get();
      if (source.device != ganeshQueue_.device || source.sampleCount != 1U ||
          source.width != static_cast<NSUInteger>(info.width()) ||
          source.height != static_cast<NSUInteger>(info.height()) ||
          source.pixelFormat != (f32 ? MTLPixelFormatRGBA16Float
                                    : MTLPixelFormatRGBA8Unorm)) {
        result.error = "GPU publication source violates its texture contract";
        return result;
      }
      if (!publicationPipeline_) {
        // Transfer only: preserve the exact CPU publication alpha rounding.
        constexpr const char *shader = R"MSL(
#include <metal_stdlib>
using namespace metal;
struct Publication { uint width, height, floating, sourcePremul, outputPremul; };
kernel void publish_rgba(texture2d<float, access::read> input [[texture(0)]],
                         device uchar *output [[buffer(0)]],
                         constant Publication &p [[buffer(1)]],
                         uint2 pos [[thread_position_in_grid]]) {
  if (pos.x >= p.width || pos.y >= p.height) return;
  float4 c = input.read(pos);
  uint offset = pos.y * p.width + pos.x;
  if (p.floating) {
    if (p.sourcePremul != p.outputPremul) {
      c.rgb = p.outputPremul ? c.rgb * c.a :
          (c.a > 0.0f ? c.rgb / c.a : float3(0.0f));
    }
    reinterpret_cast<device float4 *>(output)[offset] = c;
  } else {
    uint4 b = uint4(round(clamp(c, 0.0f, 1.0f) * 255.0f));
    if (p.sourcePremul != p.outputPremul) {
      b.rgb = p.outputPremul ? (b.rgb * b.a + 127u) / 255u :
          (b.a > 0u ? min((b.rgb * 255u + b.a / 2u) / b.a, 255u) : uint3(0u));
    }
    reinterpret_cast<device uchar4 *>(output)[offset] = uchar4(b);
  }
})MSL";
        NSError *nativeError = nil;
        id<MTLLibrary> library = [source.device
            newLibraryWithSource:[NSString stringWithUTF8String:shader]
                         options:nil error:&nativeError];
        id<MTLFunction> function = [library newFunctionWithName:@"publish_rgba"];
        if (function)
          publicationPipeline_ = [source.device
              newComputePipelineStateWithFunction:function error:&nativeError];
        [function release];
        [library release];
        if (!publicationPipeline_) {
          result.error = MetalErrorMessage(@"Skia publication pipeline failed", nativeError);
          return result;
        }
      }
      gpu::ImageLayoutSpec spec;
      spec.width = info.width(); spec.height = info.height(); spec.channels = 4;
      spec.scalarType = f32 ? gpu::ScalarF32 : gpu::ScalarU8;
      spec.pixelFormat = gpu::PixelAbgr;
      spec.channelLayout = gpu::ChannelsInterleaved;
      gpu::ImageBuffer output;
      if (!executionDevice_->AllocateImage(spec, output, result.error))
        return result;
      gpu_execution::NativeBufferView view;
      if (!executionDevice_->DescribeNativeBuffer(output, view)) {
        result.error = "Skia publication output has no native buffer";
        return result;
      }
      FlushGpu();
      id<MTLCommandBuffer> command = [ganeshQueue_ commandBuffer];
      id<MTLComputeCommandEncoder> encoder = [command computeCommandEncoder];
      if (!encoder) {
        result.error = "Skia publication command allocation failed";
        return result;
      }
      struct Parameters { std::uint32_t width, height, floating, sourcePremul, outputPremul; };
      const Parameters parameters{static_cast<std::uint32_t>(info.width()),
          static_cast<std::uint32_t>(info.height()), f32,
          info.alphaType() == kPremul_SkAlphaType, premultiplied};
      [encoder setComputePipelineState:publicationPipeline_];
      [encoder setTexture:source atIndex:0];
      [encoder setBuffer:(id<MTLBuffer>)view.nativeBuffer offset:view.offset atIndex:0];
      [encoder setBytes:&parameters length:sizeof(parameters) atIndex:1];
      [encoder dispatchThreads:MTLSizeMake(info.width(), info.height(), 1)
          threadsPerThreadgroup:MTLSizeMake(16, 16, 1)];
      [encoder endEncoding];
      if (!SubmitNative(command, result.error,
                        source.allocatedSize + output.AvailableBytes(),
                        output.StorageLease()))
        return result;
      auto completion = std::make_shared<SkiaMetalFrameCompletion>(
          completionTimeline_, executionDevice_, lastSubmission_);
      if (!output.AttachCompletionFence(completion)) {
        result.error = "Skia publication could not attach producer completion";
        return result;
      }
      gpu_frame::ImportOptions options;
      options.policy = gpu_frame::ImportPolicy::WrapGpu;
      options.kind = frame::FrameKind::Image;
      options.frame_generation = generation;
      options.device_generation = executionDevice_->generation();
      options.override_timing = true;
      options.timing = {timestampUs, 1, {1, 1'000'000}};
      options.override_presentation = true;
      options.color.primaries = frame::ColorPrimaries::Unknown;
      options.color.transfer = frame::TransferFunction::Srgb;
      options.color.matrix = frame::MatrixCoefficients::Identity;
      options.color.range = frame::ColorRange::Full;
      options.color.alpha = premultiplied ? frame::AlphaMode::Premultiplied
                                         : frame::AlphaMode::Straight;
      options.color.model = frame::ColorModel::Rgb;
      auto published = gpu_frame::PublishImageBuffer(output, options);
      if (!published) result.error = published.error().message();
      else if (cancel && cancel()) result.error = "canceled";
      else result.frame = std::move(published).value();
      return result;
    }
  }

  sk_sp<SkImage> RenderQtTextEngineCopy(
      QtTextEngineCopyMetalRuntime &runtime, const sk_sp<SkImage> &source,
      int width, int height, std::string &error) override {
    return RenderQtTextRawPass(
        source, width, height,
        [&](void *input, void *output, void *queue, const NativeCommandSubmission &submit, std::string &failure) {
          return runtime.RenderTextures(input, output, queue, failure, submit);
        }, error);
  }

  sk_sp<SkImage> RenderQtTextRawPass(
      const sk_sp<SkImage> &source, int width, int height,
      const NativeRgba8Pass &render, std::string &error) override {
    @autoreleasepool {
      if (!context_ || context_->abandoned() || ganeshQueue_ == nil ||
          !source || !render || width <= 0 || height <= 0) {
        error = "Qt raw pass requires a source and an active GPU context";
        return {};
      }
      // Preserve the raw RGBA byte contract: no color transform, alpha
      // conversion or RGB<=A clamp. Bottom-left surfaces replace exactly the
      // CPU upload/readback row flips of the captured EngineCopy ABI.
      const auto sourceInfo = SkImageInfo::Make(
          source->width(), source->height(), kRGBA_8888_SkColorType,
          kPremul_SkAlphaType, nullptr);
      const auto outputInfo = sourceInfo.makeWH(width, height);
      auto output = SkSurfaces::RenderTarget(
          context_.get(), skgpu::Budgeted::kYes, outputInfo, 0,
          kBottomLeft_GrSurfaceOrigin, nullptr);
      if (!output) {
        error = "Qt raw pass GPU surface allocation failed";
        return {};
      }

      // A preceding native pass already produces the required raw texture.
      // Reuse only exact, same-context images: different origins, swizzles,
      // subsets or storage extents still use the byte-preserving draw below.
      GrBackendTexture sourceBackend;
      GrMtlTextureInfo sourceMetal;
      sk_sp<SkImage> textureImage;
      bool reuseSource = false;
      if (source->isTextureBacked() &&
          source->colorType() == kRGBA_8888_SkColorType &&
          source->alphaType() == kPremul_SkAlphaType) {
        textureImage = SkImages::TextureFromImage(
            context_.get(), source.get(), skgpu::Mipmapped::kNo);
        GrSurfaceOrigin origin = kTopLeft_GrSurfaceOrigin;
        if (textureImage &&
            SkImages::GetBackendTextureFromImage(
                textureImage.get(), &sourceBackend, false, &origin) &&
            origin == kBottomLeft_GrSurfaceOrigin &&
            GrBackendTextures::GetMtlTextureInfo(sourceBackend, &sourceMetal) &&
            sourceMetal.fTexture) {
          id<MTLTexture> native =
              (__bridge id<MTLTexture>)sourceMetal.fTexture.get();
          reuseSource = native.device == ganeshQueue_.device &&
              native.textureType == MTLTextureType2D &&
              native.pixelFormat == MTLPixelFormatRGBA8Unorm &&
              native.width == static_cast<NSUInteger>(source->width()) &&
              native.height == static_cast<NSUInteger>(source->height()) &&
              native.sampleCount == 1U &&
              (native.usage & MTLTextureUsageShaderRead);
        }
      }
      sk_sp<SkSurface> input;
      if (!reuseSource) {
        input = SkSurfaces::RenderTarget(
            context_.get(), skgpu::Budgeted::kYes, sourceInfo, 0,
            kBottomLeft_GrSurfaceOrigin, nullptr);
        auto shader = source->makeRawShader(
            SkTileMode::kClamp, SkTileMode::kClamp,
            SkSamplingOptions(SkFilterMode::kNearest));
        if (!input || !shader) {
          error = "Qt raw pass GPU source allocation failed";
          return {};
        }
        SkPaint paint;
        paint.setAntiAlias(false);
        paint.setBlendMode(SkBlendMode::kSrc);
        paint.setShader(std::move(shader));
        input->getCanvas()->drawRect(
            SkRect::MakeWH(static_cast<float>(source->width()),
                          static_cast<float>(source->height())), paint);
        sourceBackend = SkSurfaces::GetBackendTexture(
            input.get(), SkSurface::BackendHandleAccess::kFlushRead);
      }
      const auto outputBackend = SkSurfaces::GetBackendTexture(
          output.get(), SkSurface::BackendHandleAccess::kFlushWrite);
      GrMtlTextureInfo outputMetal;
      if (!GrBackendTextures::GetMtlTextureInfo(sourceBackend, &sourceMetal) ||
          !GrBackendTextures::GetMtlTextureInfo(outputBackend, &outputMetal) ||
          !sourceMetal.fTexture || !outputMetal.fTexture) {
        error = "Qt raw pass could not unwrap its GPU surfaces";
        return {};
      }
      id<MTLTexture> sourceTexture =
          (__bridge id<MTLTexture>)sourceMetal.fTexture.get();
      id<MTLTexture> outputTexture =
          (__bridge id<MTLTexture>)outputMetal.fTexture.get();
      if (sourceTexture.width != static_cast<NSUInteger>(source->width()) ||
          sourceTexture.height != static_cast<NSUInteger>(source->height()) ||
          outputTexture.width != static_cast<NSUInteger>(width) ||
          outputTexture.height != static_cast<NSUInteger>(height)) {
        error = "Qt raw pass GPU allocation does not match its logical extent";
        return {};
      }
      // Ganesh producers, native copy and subsequent Ganesh consumers share
      // one ordered queue. Committed command buffers retain Metal resources.
      FlushGpu();
      if (!render(
              const_cast<void *>(sourceMetal.fTexture.get()),
              const_cast<void *>(outputMetal.fTexture.get()),
              (void *)ganeshQueue_, Submission(), error))
        return {};
      auto image = output->makeImageSnapshot();
      if (!image)
        error = "Qt raw pass GPU image publication failed";
      else
        error.clear();
      return image;
    }
  }

  sk_sp<SkImage> RenderQtTextLetter(
      QtTextLetterMetalRuntime &runtime, const QtTextLetterRenderRequest &request,
      sk_sp<SkColorSpace> colorSpace, std::string &error,
      std::vector<std::uint8_t> *diagnosticPixels) override {
    @autoreleasepool {
      if (!context_ || context_->abandoned() || ganeshQueue_ == nil) {
        error = "Qt Letter requires an active GPU context";
        return {};
      }
      FlushGpu();
      std::shared_ptr<void> texture;
      if (!runtime.RenderTexture(request, texture, error, diagnosticPixels,
                                 (void *)ganeshQueue_, Submission()))
        return {};
      const int width = request.outputWidth;
      const int height = request.outputHeight;
      id<MTLTexture> native = (id<MTLTexture>)texture.get();
      id<MTLDevice> device = (__bridge id<MTLDevice>)(
          reinterpret_cast<void *>(executionDevice_->nativeDeviceHandle()));
      if (!context_ || context_->abandoned() || native == nil ||
          native.device != device || width <= 0 || height <= 0 ||
          native.width != static_cast<NSUInteger>(width) ||
          native.height != static_cast<NSUInteger>(height) ||
          native.pixelFormat != MTLPixelFormatRGBA8Unorm) {
        error = "Qt Letter texture does not match the Ganesh Metal context";
        return {};
      }
      GrMtlTextureInfo info;
      info.fTexture.retain((__bridge GrMTLHandle)native);
      const auto backend = GrBackendTextures::MakeMtl(
          width, height, skgpu::Mipmapped::kNo, info);
      auto image = SkImages::BorrowTextureFrom(
          context_.get(), backend, kBottomLeft_GrSurfaceOrigin,
          kRGBA_8888_SkColorType, kPremul_SkAlphaType, std::move(colorSpace),
          [](void *owner) { delete static_cast<std::shared_ptr<void> *>(owner); },
          new std::shared_ptr<void>(texture));
      if (!image)
        error = "Qt Letter GPU image publication failed";
      else
        error.clear();
      return image;
    }
  }

  bool RenderTextVatMesh(const sk_sp<SkImage> &source,
                         const TextVatMeshGpuRequest &request,
                         SkSurface &destination,
                         std::string &error) override {
    @autoreleasepool {
      constexpr std::size_t kMaximumVatVertexCount = 4U * 1024U * 1024U;
      constexpr std::size_t kMaximumVatIndexCount =
          kMaximumVatVertexCount * 3U;
      if (request.implementationVersion != kTextVatMeshImplementationVersion ||
          !source || source->width() <= 0 || source->height() <= 0 ||
          request.targetWidth <= 0 || request.targetHeight <= 0 ||
          destination.width() != request.targetWidth ||
          destination.height() != request.targetHeight ||
          request.vertices.empty() || request.indices.empty() ||
          request.vertices.size() > kMaximumVatVertexCount ||
          request.indices.size() > kMaximumVatIndexCount ||
          request.indices.size() % 3U != 0U ||
          !std::isfinite(request.opacity) || request.opacity < 0.0F) {
        error = "text VAT mesh request is invalid";
        return false;
      }
      if (!std::all_of(request.vertices.begin(), request.vertices.end(),
                       [](const auto &vertex) {
                         return std::isfinite(vertex.clipX) &&
                                std::isfinite(vertex.clipY) &&
                                std::isfinite(vertex.clipZ) &&
                                std::isfinite(vertex.clipW) &&
                                vertex.clipW > 0.0F &&
                                std::isfinite(vertex.textureU) &&
                                std::isfinite(vertex.textureV);
                       }) ||
          !std::all_of(request.indices.begin(), request.indices.end(),
                       [&](const auto index) {
                         return static_cast<std::size_t>(index) <
                                request.vertices.size();
                       })) {
        error = "text VAT mesh contains an invalid vertex or index";
        return false;
      }
      if (!context_ || context_->abandoned() || ganeshQueue_ == nil) {
        error = "text VAT mesh lost its Ganesh Metal context";
        return false;
      }
      id<MTLDevice> device = (__bridge id<MTLDevice>)(
          reinterpret_cast<void *>(executionDevice_->nativeDeviceHandle()));
      if (device == nil) {
        error = "text VAT mesh Metal device is unavailable";
        return false;
      }
      const auto colorType = destination.imageInfo().colorType();
      const MTLPixelFormat targetColorFormat =
          colorType == kRGBA_F16_SkColorType
              ? MTLPixelFormatRGBA16Float
              : colorType == kBGRA_8888_SkColorType
                    ? MTLPixelFormatBGRA8Unorm
                    : colorType == kRGBA_8888_SkColorType
                          ? MTLPixelFormatRGBA8Unorm
                          : MTLPixelFormatInvalid;
      if (targetColorFormat == MTLPixelFormatInvalid) {
        error = "text VAT mesh destination color type is unsupported";
        return false;
      }
      if (!textVatMeshState_ ||
          textVatMeshState_->colorFormat != targetColorFormat) {
        textVatMeshState_ =
            CreateTextVatMeshMetalState(device, targetColorFormat, error);
        if (!textVatMeshState_)
          return false;
      }

      auto textureImage = SkImages::TextureFromImage(
          context_.get(), source.get(), skgpu::Mipmapped::kNo,
          skgpu::Budgeted::kNo);
      GrBackendTexture sourceBackend;
      GrSurfaceOrigin sourceOrigin = kTopLeft_GrSurfaceOrigin;
      if (!textureImage ||
          !SkImages::GetBackendTextureFromImage(
              textureImage.get(), &sourceBackend, false, &sourceOrigin) ||
          !sourceBackend.isValid()) {
        error = "text VAT mesh source texture is unavailable";
        return false;
      }
      auto targetBackend = SkSurfaces::GetBackendTexture(
          &destination, SkSurface::BackendHandleAccess::kFlushWrite);
      GrMtlTextureInfo sourceInfo;
      GrMtlTextureInfo targetInfo;
      if (!targetBackend.isValid() ||
          !GrBackendTextures::GetMtlTextureInfo(sourceBackend, &sourceInfo) ||
          !GrBackendTextures::GetMtlTextureInfo(targetBackend, &targetInfo) ||
          !sourceInfo.fTexture || !targetInfo.fTexture) {
        error = "text VAT mesh could not unwrap its Metal textures";
        return false;
      }
      id<MTLTexture> sourceTexture =
          (__bridge id<MTLTexture>)sourceInfo.fTexture.get();
      id<MTLTexture> targetTexture =
          (__bridge id<MTLTexture>)targetInfo.fTexture.get();
      if (sourceTexture == nil || targetTexture == nil ||
          (sourceTexture.pixelFormat != MTLPixelFormatRGBA8Unorm &&
           sourceTexture.pixelFormat != MTLPixelFormatBGRA8Unorm &&
           sourceTexture.pixelFormat != MTLPixelFormatRGBA16Float) ||
          targetTexture.pixelFormat != targetColorFormat ||
          sourceTexture.width != static_cast<NSUInteger>(source->width()) ||
          sourceTexture.height != static_cast<NSUInteger>(source->height()) ||
          targetTexture.width !=
              static_cast<NSUInteger>(request.targetWidth) ||
          targetTexture.height !=
              static_cast<NSUInteger>(request.targetHeight)) {
        error = "text VAT mesh Metal texture contract is invalid";
        return false;
      }

      FlushGpu();
      id<MTLBuffer> vertexBuffer = [device
          newBufferWithBytes:request.vertices.data()
                      length:request.vertices.size() * sizeof(TextVatMeshVertex)
                     options:MTLResourceStorageModeShared];
      id<MTLBuffer> indexBuffer = [device
          newBufferWithBytes:request.indices.data()
                      length:request.indices.size() * sizeof(std::uint16_t)
                     options:MTLResourceStorageModeShared];
      id<MTLCommandBuffer> commandBuffer = [ganeshQueue_ commandBuffer];
      const auto releaseResources = [&]() {
        [indexBuffer release];
        [vertexBuffer release];
      };
      if (vertexBuffer == nil || indexBuffer == nil || commandBuffer == nil) {
        releaseResources();
        error = "text VAT mesh per-draw Metal resource allocation failed";
        return false;
      }
      commandBuffer.label = @"VideoCut Text VAT mesh v1";
      MTLRenderPassDescriptor *pass =
          [MTLRenderPassDescriptor renderPassDescriptor];
      pass.colorAttachments[0].texture = targetTexture;
      pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
      pass.colorAttachments[0].storeAction = MTLStoreActionStore;
      id<MTLRenderCommandEncoder> encoder =
          [commandBuffer renderCommandEncoderWithDescriptor:pass];
      if (encoder == nil) {
        releaseResources();
        error = "text VAT mesh Metal encoder creation failed";
        return false;
      }
      [encoder setViewport:MTLViewport{
                               0.0, 0.0,
                               static_cast<double>(request.targetWidth),
                               static_cast<double>(request.targetHeight),
                               0.0, 1.0}];
      [encoder setScissorRect:MTLScissorRect{
                                  0U, 0U,
                                  static_cast<NSUInteger>(request.targetWidth),
                                  static_cast<NSUInteger>(request.targetHeight)}];
      [encoder setCullMode:MTLCullModeNone];
      [encoder setFrontFacingWinding:MTLWindingClockwise];
      [encoder setTriangleFillMode:MTLTriangleFillModeFill];
      [encoder setRenderPipelineState:textVatMeshState_->pipeline];
      [encoder setVertexBuffer:vertexBuffer offset:0U atIndex:0U];
      const float opacity = request.opacity;
      const std::uint32_t flipSourceV =
          sourceOrigin == kTopLeft_GrSurfaceOrigin ? 0U : 1U;
      [encoder setFragmentBytes:&opacity length:sizeof(opacity) atIndex:0U];
      [encoder setFragmentBytes:&flipSourceV
                         length:sizeof(flipSourceV)
                        atIndex:1U];
      [encoder setFragmentTexture:sourceTexture atIndex:0U];
      [encoder setFragmentSamplerState:textVatMeshState_->sampler atIndex:0U];
      [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                          indexCount:request.indices.size()
                           indexType:MTLIndexTypeUInt16
                         indexBuffer:indexBuffer
                   indexBufferOffset:0U];
      [encoder endEncoding];
      if (!SubmitNativeTextureCommand(commandBuffer, Submission(),
          {sourceTexture, targetTexture, vertexBuffer, indexBuffer}, error)) {
        releaseResources();
        return false;
      }
      releaseResources();
      error.clear();
      return true;
    }
  }

  bool CompositeQtTextRenderGroup(
      const sk_sp<SkImage> &pageImage, SkSurface &presentationTarget,
      const QtTextRenderGroupCompositeRequest &request,
      std::string &error) override {
    @autoreleasepool {
      if (request.implementationVersion !=
          kQtTextRenderGroupCompositeImplementationVersion) {
        error = "Qt RenderGroup composite implementation version is unsupported";
        return false;
      }
      if (!pageImage || pageImage->width() <= 0 || pageImage->height() <= 0 ||
          request.presentationWidth <= 0 ||
          request.presentationHeight <= 0 ||
          presentationTarget.width() != request.presentationWidth ||
          presentationTarget.height() != request.presentationHeight ||
          (presentationTarget.imageInfo().colorType() !=
               kRGBA_8888_SkColorType &&
           presentationTarget.imageInfo().colorType() !=
               kRGBA_F16_SkColorType) ||
          !std::isfinite(request.alpha) || request.alpha < 0.0F) {
        error = "Qt RenderGroup composite request is invalid";
        return false;
      }
      if (!std::all_of(request.mvp.begin(), request.mvp.end(),
                       [](const float value) { return std::isfinite(value); }) ||
          !std::all_of(request.customMatrix.begin(),
                       request.customMatrix.end(), [](const float value) {
                         return std::isfinite(value);
                       })) {
        error = "Qt RenderGroup composite matrix contains a non-finite value";
        return false;
      }
      if (!context_ || context_->abandoned() || ganeshQueue_ == nil) {
        error = "Qt RenderGroup composite lost its Ganesh Metal context";
        return false;
      }
      id<MTLDevice> device = (__bridge id<MTLDevice>)(
          reinterpret_cast<void *>(executionDevice_->nativeDeviceHandle()));
      if (device == nil) {
        error = "Qt RenderGroup composite Metal device is unavailable";
        return false;
      }
      const MTLPixelFormat targetColorFormat =
          presentationTarget.imageInfo().colorType() == kRGBA_F16_SkColorType
              ? MTLPixelFormatRGBA16Float
              : MTLPixelFormatRGBA8Unorm;
      if (!qtTextRenderGroupCompositeState_ ||
          qtTextRenderGroupCompositeState_->colorFormat != targetColorFormat) {
        qtTextRenderGroupCompositeState_ =
            CreateQtTextRenderGroupCompositeMetalState(
                device, targetColorFormat, error);
        if (!qtTextRenderGroupCompositeState_)
          return false;
      }

      // A raster Page is uploaded once into this Ganesh context. Texture-backed
      // Pages from native/Skia post passes retain their existing allocation.
      auto textureImage = SkImages::TextureFromImage(
          context_.get(), pageImage.get(), skgpu::Mipmapped::kNo,
          skgpu::Budgeted::kNo);
      if (!textureImage) {
        error = "Qt RenderGroup could not resolve the Page as a Metal texture";
        return false;
      }
      GrBackendTexture sourceBackend;
      GrSurfaceOrigin sourceOrigin = kTopLeft_GrSurfaceOrigin;
      if (!SkImages::GetBackendTextureFromImage(
              textureImage.get(), &sourceBackend, false, &sourceOrigin) ||
          !sourceBackend.isValid()) {
        error = "Qt RenderGroup source backend texture is unavailable";
        return false;
      }
      auto targetBackend = SkSurfaces::GetBackendTexture(
          &presentationTarget,
          SkSurface::BackendHandleAccess::kFlushWrite);
      if (!targetBackend.isValid()) {
        error = "Qt RenderGroup target backend texture is unavailable";
        return false;
      }
      GrMtlTextureInfo sourceInfo;
      GrMtlTextureInfo targetInfo;
      if (!GrBackendTextures::GetMtlTextureInfo(sourceBackend, &sourceInfo) ||
          !GrBackendTextures::GetMtlTextureInfo(targetBackend, &targetInfo) ||
          !sourceInfo.fTexture || !targetInfo.fTexture) {
        error = "Qt RenderGroup could not unwrap its Metal textures";
        return false;
      }
      id<MTLTexture> sourceTexture =
          (__bridge id<MTLTexture>)sourceInfo.fTexture.get();
      id<MTLTexture> targetTexture =
          (__bridge id<MTLTexture>)targetInfo.fTexture.get();
      if (sourceTexture == nil || targetTexture == nil ||
          sourceTexture.pixelFormat != MTLPixelFormatRGBA8Unorm ||
          targetTexture.pixelFormat != targetColorFormat ||
          sourceTexture.width != static_cast<NSUInteger>(pageImage->width()) ||
          sourceTexture.height !=
              static_cast<NSUInteger>(pageImage->height()) ||
          targetTexture.width !=
              static_cast<NSUInteger>(request.presentationWidth) ||
          targetTexture.height !=
              static_cast<NSUInteger>(request.presentationHeight)) {
        error =
            "Qt RenderGroup Metal texture contract does not match its "
            "bounded RGBA target";
        return false;
      }

      // Flush both the Page producer and all existing target content. The
      // following command is committed to the same queue, so no CPU wait or
      // GPU->CPU->GPU round trip is required.
      FlushGpu();

      MTLTextureDescriptor *depthDescriptor =
          [MTLTextureDescriptor
              texture2DDescriptorWithPixelFormat:MTLPixelFormatDepth32Float
                                           width:request.presentationWidth
                                          height:request.presentationHeight
                                       mipmapped:NO];
      depthDescriptor.storageMode = MTLStorageModePrivate;
      depthDescriptor.usage = MTLTextureUsageRenderTarget |
                              MTLTextureUsageShaderRead;
      id<MTLTexture> depthTexture =
          [device newTextureWithDescriptor:depthDescriptor];
      id<MTLCommandBuffer> commandBuffer = [ganeshQueue_ commandBuffer];
      if (depthTexture == nil || commandBuffer == nil) {
        [depthTexture release];
        error = "Qt RenderGroup per-draw Metal resource allocation failed";
        return false;
      }
      commandBuffer.label = @"Qt Text RenderGroup composite v1";

      MTLRenderPassDescriptor *pass =
          [MTLRenderPassDescriptor renderPassDescriptor];
      pass.colorAttachments[0].texture = targetTexture;
      pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
      pass.colorAttachments[0].storeAction = MTLStoreActionStore;
      pass.depthAttachment.texture = depthTexture;
      pass.depthAttachment.loadAction = MTLLoadActionClear;
      pass.depthAttachment.storeAction = MTLStoreActionStore;
      pass.depthAttachment.clearDepth = 1.0;
      id<MTLRenderCommandEncoder> encoder =
          [commandBuffer renderCommandEncoderWithDescriptor:pass];
      if (encoder == nil) {
        [depthTexture release];
        error = "Qt RenderGroup Metal encoder creation failed";
        return false;
      }
      [encoder setViewport:MTLViewport{
                               0.0, 0.0,
                               static_cast<double>(request.presentationWidth),
                               static_cast<double>(request.presentationHeight),
                               0.0, 1.0}];
      [encoder setScissorRect:MTLScissorRect{
                                  0U, 0U,
                                  static_cast<NSUInteger>(
                                      request.presentationWidth),
                                  static_cast<NSUInteger>(
                                      request.presentationHeight)}];
      [encoder setCullMode:MTLCullModeNone];
      [encoder setFrontFacingWinding:MTLWindingClockwise];
      [encoder setTriangleFillMode:MTLTriangleFillModeFill];
      [encoder setRenderPipelineState:
                   qtTextRenderGroupCompositeState_->pipeline];
      [encoder setDepthStencilState:
                   qtTextRenderGroupCompositeState_->depthStencil];
      [encoder setVertexBytes:request.mvp.data()
                       length:sizeof(request.mvp)
                      atIndex:0U];
      [encoder setVertexBytes:request.customMatrix.data()
                       length:sizeof(request.customMatrix)
                      atIndex:1U];
      [encoder setVertexBuffer:qtTextRenderGroupCompositeState_->vertices
                        offset:0U
                       atIndex:30U];
      const float alpha = request.alpha;
      // Top-left Ganesh textures store the logical top row at Metal row zero;
      // Qt's captured Page transport stores it at the opposite edge. Flip_v=1
      // makes both transports sample identically without a texture copy.
      const std::array<float, 4> textureFlip{
          sourceOrigin == kTopLeft_GrSurfaceOrigin ? 1.0F : 0.0F, 0.0F,
          0.0F, 0.0F};
      [encoder setFragmentBytes:&alpha length:sizeof(alpha) atIndex:0U];
      [encoder setFragmentBytes:textureFlip.data()
                         length:sizeof(textureFlip)
                        atIndex:1U];
      [encoder setFragmentTexture:sourceTexture atIndex:0U];
      [encoder setFragmentSamplerState:
                   qtTextRenderGroupCompositeState_->sampler
                               atIndex:0U];
      [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                          indexCount:6U
                           indexType:MTLIndexTypeUInt16
                         indexBuffer:qtTextRenderGroupCompositeState_->indices
                   indexBufferOffset:0U
                       instanceCount:1U];
      [encoder endEncoding];
      if (!SubmitNativeTextureCommand(commandBuffer, Submission(),
          {sourceTexture, targetTexture, depthTexture}, error)) {
        [depthTexture release];
        return false;
      }
      [depthTexture release];
      if (std::getenv("VIDEOCUT_TRACE_QT_TEXT_RENDER_GROUP_NATIVE") !=
          nullptr) {
        const auto traceFloatBits = [](const float value) noexcept {
          std::uint32_t bits = 0U;
          static_assert(sizeof(bits) == sizeof(value));
          std::memcpy(&bits, &value, sizeof(bits));
          return bits;
        };
        std::fprintf(
            stderr,
            "[VIDEOCUT_QT_TEXT_RENDER_GROUP_NATIVE] version=%u "
            "page=%dx%d target=%dx%d source_origin=%s texture_flip=%.0f "
            "queue=ganesh status=submitted\n",
            request.implementationVersion, pageImage->width(),
            pageImage->height(), request.presentationWidth,
            request.presentationHeight,
            sourceOrigin == kTopLeft_GrSurfaceOrigin ? "top-left"
                                                     : "bottom-left",
            textureFlip[0]);
        std::fprintf(
            stderr,
            "[VIDEOCUT_QT_TEXT_RENDER_GROUP_REQUEST] version=%08x "
            "presentation=[%08x %08x] mvp=",
            request.implementationVersion,
            static_cast<std::uint32_t>(request.presentationWidth),
            static_cast<std::uint32_t>(request.presentationHeight));
        for (const float value : request.mvp)
          std::fprintf(stderr, "%08x,", traceFloatBits(value));
        std::fprintf(stderr, " custom=");
        for (const float value : request.customMatrix)
          std::fprintf(stderr, "%08x,", traceFloatBits(value));
        std::fprintf(stderr, " alpha=%08x\n", traceFloatBits(request.alpha));
      }
      error.clear();
      return true;
    }
  }

  bool CompositeQtTextFollower(
      const sk_sp<SkImage> &followerImage, SkSurface &presentationTarget,
      const QtTextFollowerCompositeRequest &request,
      std::string &error) override {
    @autoreleasepool {
      if (request.implementationVersion !=
          kQtTextFollowerCompositeImplementationVersion) {
        error = "Qt follower composite implementation version is unsupported";
        return false;
      }
      if (!followerImage || followerImage->width() <= 0 ||
          followerImage->height() <= 0 || request.presentationWidth <= 0 ||
          request.presentationHeight <= 0 ||
          presentationTarget.width() != request.presentationWidth ||
          presentationTarget.height() != request.presentationHeight ||
          (presentationTarget.imageInfo().colorType() !=
               kRGBA_8888_SkColorType &&
           presentationTarget.imageInfo().colorType() !=
               kRGBA_F16_SkColorType) ||
          !std::isfinite(request.opacity) || request.opacity < 0.0F ||
          !std::all_of(request.imageToPresentation.begin(),
                       request.imageToPresentation.end(),
                       [](const float value) {
                         return std::isfinite(value);
                       })) {
        error = "Qt follower composite request is invalid";
        return false;
      }
      const auto &matrix = request.imageToPresentation;
      if (matrix[6] != 0.0F || matrix[7] != 0.0F || matrix[8] != 1.0F) {
        error = "Qt follower composite requires an affine image transform";
        return false;
      }
      if (!context_ || context_->abandoned() || ganeshQueue_ == nil) {
        error = "Qt follower composite lost its Ganesh Metal context";
        return false;
      }
      id<MTLDevice> device = (__bridge id<MTLDevice>)(
          reinterpret_cast<void *>(executionDevice_->nativeDeviceHandle()));
      if (device == nil) {
        error = "Qt follower composite Metal device is unavailable";
        return false;
      }
      const MTLPixelFormat targetColorFormat =
          presentationTarget.imageInfo().colorType() == kRGBA_F16_SkColorType
              ? MTLPixelFormatRGBA16Float
              : MTLPixelFormatRGBA8Unorm;
      if (!qtTextFollowerCompositeState_ ||
          qtTextFollowerCompositeState_->colorFormat != targetColorFormat) {
        qtTextFollowerCompositeState_ =
            CreateQtTextFollowerCompositeMetalState(
                device, targetColorFormat, error);
        if (!qtTextFollowerCompositeState_)
          return false;
      }

      // TextureFromImage uploads raster follower bytes without asking Skia to
      // sample or color-convert them. The subsequent Metal fragment therefore
      // sees the same raw RGBA8 transport as Qt's VideoAnimSeq Sprite.
      auto textureImage = SkImages::TextureFromImage(
          context_.get(), followerImage.get(), skgpu::Mipmapped::kNo,
          skgpu::Budgeted::kNo);
      if (!textureImage) {
        error = "Qt follower could not resolve its raw image as a texture";
        return false;
      }
      GrBackendTexture sourceBackend;
      GrSurfaceOrigin sourceOrigin = kTopLeft_GrSurfaceOrigin;
      if (!SkImages::GetBackendTextureFromImage(
              textureImage.get(), &sourceBackend, false, &sourceOrigin) ||
          !sourceBackend.isValid()) {
        error = "Qt follower source backend texture is unavailable";
        return false;
      }
      auto targetBackend = SkSurfaces::GetBackendTexture(
          &presentationTarget,
          SkSurface::BackendHandleAccess::kFlushWrite);
      if (!targetBackend.isValid()) {
        error = "Qt follower target backend texture is unavailable";
        return false;
      }
      GrMtlTextureInfo sourceInfo;
      GrMtlTextureInfo targetInfo;
      if (!GrBackendTextures::GetMtlTextureInfo(sourceBackend, &sourceInfo) ||
          !GrBackendTextures::GetMtlTextureInfo(targetBackend, &targetInfo) ||
          !sourceInfo.fTexture || !targetInfo.fTexture) {
        error = "Qt follower could not unwrap its Metal textures";
        return false;
      }
      id<MTLTexture> sourceTexture =
          (__bridge id<MTLTexture>)sourceInfo.fTexture.get();
      id<MTLTexture> targetTexture =
          (__bridge id<MTLTexture>)targetInfo.fTexture.get();
      if (sourceTexture == nil || targetTexture == nil ||
          sourceTexture.pixelFormat != MTLPixelFormatRGBA8Unorm ||
          targetTexture.pixelFormat != targetColorFormat ||
          sourceTexture.width !=
              static_cast<NSUInteger>(followerImage->width()) ||
          sourceTexture.height !=
              static_cast<NSUInteger>(followerImage->height()) ||
          targetTexture.width !=
              static_cast<NSUInteger>(request.presentationWidth) ||
          targetTexture.height !=
              static_cast<NSUInteger>(request.presentationHeight)) {
        error =
            "Qt follower Metal texture contract does not match its bounded "
            "RGBA target";
        return false;
      }

      const auto transformed = [&](const float x, const float y) {
        return std::array<float, 2>{matrix[0] * x + matrix[1] * y + matrix[2],
                                    matrix[3] * x + matrix[4] * y + matrix[5]};
      };
      const auto topLeft = transformed(0.0F, 0.0F);
      const auto topRight =
          transformed(static_cast<float>(followerImage->width()), 0.0F);
      const auto bottomRight = transformed(
          static_cast<float>(followerImage->width()),
          static_cast<float>(followerImage->height()));
      const auto bottomLeft =
          transformed(0.0F, static_cast<float>(followerImage->height()));
      const float inverseWidth =
          1.0F / static_cast<float>(request.presentationWidth);
      const float inverseHeight =
          1.0F / static_cast<float>(request.presentationHeight);
      const auto clip = [&](const std::array<float, 2> &point,
                            const float u, const float v) {
        return QtTextFollowerCompositeVertex{
            2.0F * point[0] * inverseWidth - 1.0F,
            1.0F - 2.0F * point[1] * inverseHeight, u, v};
      };
      const float topV =
          sourceOrigin == kTopLeft_GrSurfaceOrigin ? 0.0F : 1.0F;
      const float bottomV = 1.0F - topV;
      const std::array<QtTextFollowerCompositeVertex, 6> vertices{
          clip(topLeft, 0.0F, topV),
          clip(topRight, 1.0F, topV),
          clip(bottomRight, 1.0F, bottomV),
          clip(topLeft, 0.0F, topV),
          clip(bottomRight, 1.0F, bottomV),
          clip(bottomLeft, 0.0F, bottomV)};
      if (!std::all_of(vertices.begin(), vertices.end(), [](const auto &vertex) {
            return std::isfinite(vertex.positionX) &&
                   std::isfinite(vertex.positionY);
          })) {
        error = "Qt follower transformed quad contains a non-finite value";
        return false;
      }

      FlushGpu();
      id<MTLCommandBuffer> commandBuffer = [ganeshQueue_ commandBuffer];
      if (commandBuffer == nil) {
        error = "Qt follower Metal command buffer allocation failed";
        return false;
      }
      commandBuffer.label = @"Qt Text Follower composite v1";
      MTLRenderPassDescriptor *pass =
          [MTLRenderPassDescriptor renderPassDescriptor];
      pass.colorAttachments[0].texture = targetTexture;
      pass.colorAttachments[0].loadAction = MTLLoadActionLoad;
      pass.colorAttachments[0].storeAction = MTLStoreActionStore;
      id<MTLRenderCommandEncoder> encoder =
          [commandBuffer renderCommandEncoderWithDescriptor:pass];
      if (encoder == nil) {
        error = "Qt follower Metal encoder creation failed";
        return false;
      }
      [encoder setViewport:MTLViewport{
                               0.0, 0.0,
                               static_cast<double>(request.presentationWidth),
                               static_cast<double>(request.presentationHeight),
                               0.0, 1.0}];
      [encoder setScissorRect:MTLScissorRect{
                                  0U, 0U,
                                  static_cast<NSUInteger>(
                                      request.presentationWidth),
                                  static_cast<NSUInteger>(
                                      request.presentationHeight)}];
      [encoder setCullMode:MTLCullModeNone];
      [encoder setTriangleFillMode:MTLTriangleFillModeFill];
      [encoder setRenderPipelineState:qtTextFollowerCompositeState_->pipeline];
      [encoder setVertexBytes:vertices.data()
                       length:sizeof(vertices)
                      atIndex:0U];
      const float opacity = request.opacity;
      [encoder setFragmentBytes:&opacity length:sizeof(opacity) atIndex:0U];
      [encoder setFragmentTexture:sourceTexture atIndex:0U];
      [encoder setFragmentSamplerState:qtTextFollowerCompositeState_->sampler
                               atIndex:0U];
      [encoder drawPrimitives:MTLPrimitiveTypeTriangle
                  vertexStart:0U
                  vertexCount:vertices.size()];
      [encoder endEncoding];
      if (!SubmitNativeTextureCommand(commandBuffer, Submission(),
          {sourceTexture, targetTexture}, error)) {

        return false;
      }
      if (std::getenv("VIDEOCUT_TRACE_QT_TEXT_FOLLOWER_NATIVE") != nullptr) {
        std::fprintf(stderr,
                     "[VIDEOCUT_QT_TEXT_FOLLOWER_NATIVE] version=%u "
                     "source=%dx%d target=%dx%d origin=%s opacity=%g "
                     "status=submitted\n",
                     request.implementationVersion, followerImage->width(),
                     followerImage->height(), request.presentationWidth,
                     request.presentationHeight,
                     sourceOrigin == kTopLeft_GrSurfaceOrigin ? "top-left"
                                                              : "bottom-left",
                     request.opacity);
      }
      error.clear();
      return true;
    }
  }

  bool RenderTextSdfMaterial(const TextSdfMaterialGpuRequest &request,
                             SkSurface &destination,
                             std::string &error) override {
    @autoreleasepool {
      if (!context_ || context_->abandoned() || ganeshQueue_ == nil) {
        error = "VideoCut text SDF material lost its Ganesh Metal context";
        return false;
      }
      float rasterDistanceRange = 0.0F;
      if (!ValidateTextSdfGpuMesh(request.mesh, rasterDistanceRange, error))
        return false;
      if (destination.width() <= 0 || destination.height() <= 0) {
        error = "VideoCut text SDF material destination is invalid";
        return false;
      }
      id<MTLDevice> device = (__bridge id<MTLDevice>)(
          reinterpret_cast<void *>(executionDevice_->nativeDeviceHandle()));
      if (device == nil ||
          request.mesh.targetWidth >
              kMaximumMetalTextSdfTextureExtent ||
          request.mesh.targetHeight >
              kMaximumMetalTextSdfTextureExtent) {
        error = "VideoCut text SDF material Metal device is unavailable or "
                "the distance target is too large";
        return false;
      }
      if (!textSdfState_) {
        textSdfState_ = CreateTextSdfMetalState(device, error);
        if (!textSdfState_)
          return false;
      }

      auto targetBackend = SkSurfaces::GetBackendTexture(
          &destination, SkSurface::BackendHandleAccess::kFlushWrite);
      GrMtlTextureInfo targetInfo;
      if (!targetBackend.isValid() ||
          !GrBackendTextures::GetMtlTextureInfo(targetBackend, &targetInfo) ||
          !targetInfo.fTexture) {
        error = "VideoCut text SDF material destination Metal texture is "
                "unavailable";
        return false;
      }
      id<MTLTexture> targetTexture =
          (__bridge id<MTLTexture>)targetInfo.fTexture.get();
      if (targetTexture == nil ||
          targetTexture.width != static_cast<NSUInteger>(destination.width()) ||
          targetTexture.height !=
              static_cast<NSUInteger>(destination.height()) ||
          (targetTexture.pixelFormat != MTLPixelFormatRGBA8Unorm &&
           targetTexture.pixelFormat != MTLPixelFormatBGRA8Unorm)) {
        error = "VideoCut text SDF material requires an RGBA8 or BGRA8 "
                "destination";
        return false;
      }
      const bool fixedFunctionSourceOver =
          request.presentationBlend == text::TextBlendMode::SourceOver;
      id<MTLRenderPipelineState> materialPipeline = nil;
      if (targetTexture.pixelFormat == MTLPixelFormatRGBA8Unorm) {
        materialPipeline = fixedFunctionSourceOver
                               ? textSdfState_->materialSourceOverRgbaPipeline
                               : textSdfState_->materialRgbaPipeline;
      } else {
        materialPipeline = fixedFunctionSourceOver
                               ? textSdfState_->materialSourceOverBgraPipeline
                               : textSdfState_->materialBgraPipeline;
      }

      sk_sp<SkImage> resolvedTexture;
      GrSurfaceOrigin textureOrigin = kTopLeft_GrSurfaceOrigin;
      id<MTLTexture> sourceTexture = nil;
      sk_sp<SkImage> resolvedReplacementMask;
      GrSurfaceOrigin replacementMaskOrigin = kTopLeft_GrSurfaceOrigin;
      id<MTLTexture> replacementMaskTexture = nil;
      const auto resolveTexture = [&](const sk_sp<SkImage> &source,
                                      sk_sp<SkImage> &resolved,
                                      GrSurfaceOrigin &origin,
                                      id<MTLTexture> &native,
                                      const char *role) {
        if (!source)
          return true;
        resolved = SkImages::TextureFromImage(
            context_.get(), source.get(), skgpu::Mipmapped::kNo,
            skgpu::Budgeted::kNo);
        GrBackendTexture sourceBackend;
        GrMtlTextureInfo sourceInfo;
        if (!resolved ||
            !SkImages::GetBackendTextureFromImage(
                resolved.get(), &sourceBackend, false, &origin) ||
            !sourceBackend.isValid() ||
            !GrBackendTextures::GetMtlTextureInfo(sourceBackend,
                                                  &sourceInfo) ||
            !sourceInfo.fTexture) {
          error = std::string("VideoCut text SDF ") + role +
                  " could not be resolved on the destination Metal device";
          return false;
        }
        native = (__bridge id<MTLTexture>)sourceInfo.fTexture.get();
        if (native == nil ||
            native.width != static_cast<NSUInteger>(resolved->width()) ||
            native.height != static_cast<NSUInteger>(resolved->height())) {
          error = std::string("VideoCut text SDF resolved ") + role +
                  " is invalid";
          return false;
        }
        if (std::getenv("VIDEOCUT_TRACE_TEXT_SDF_TEXTURE_UNIFORMS") !=
            nullptr) {
          std::fprintf(
              stderr,
              "text-sdf-bound-texture role=%s size=%lux%lu pixel_format=%lu "
              "origin=%u storage_mode=%lu usage=%lu\n",
              role, static_cast<unsigned long>(native.width),
              static_cast<unsigned long>(native.height),
              static_cast<unsigned long>(native.pixelFormat),
              static_cast<unsigned>(origin),
              static_cast<unsigned long>(native.storageMode),
              static_cast<unsigned long>(native.usage));
        }
        return true;
      };
      if (!resolveTexture(request.texture, resolvedTexture, textureOrigin,
                          sourceTexture, "material texture") ||
          !resolveTexture(request.replacementMask, resolvedReplacementMask,
                          replacementMaskOrigin, replacementMaskTexture,
                          "replacement mask"))
        return false;

      TextSdfMaterialUniforms uniforms;
      if (!BuildTextSdfMaterialUniforms(
              request, destination.width(), destination.height(),
              request.materialDistanceRange, rasterDistanceRange,
              textureOrigin,
              replacementMaskOrigin, uniforms, error)) {
        return false;
      }
      id<MTLTexture> gradientLutTexture = nil;
      if (uniforms.modes1.z != 0U) {
        gradientLutTexture =
            CreateTextSdfGradientLutTexture(device, uniforms, error);
        if (gradientLutTexture == nil)
          return false;
      }

      FlushGpu();
      MTLTextureDescriptor *distanceDescriptor =
          [MTLTextureDescriptor
              texture2DDescriptorWithPixelFormat:MTLPixelFormatRG8Unorm
                                           width:request.mesh.targetWidth
                                          height:request.mesh.targetHeight
                                       mipmapped:NO];
      distanceDescriptor.storageMode = MTLStorageModePrivate;
      distanceDescriptor.usage = MTLTextureUsageRenderTarget |
                                 MTLTextureUsageShaderRead;
      id<MTLTexture> distanceTexture =
          [device newTextureWithDescriptor:distanceDescriptor];

      id<MTLTexture> destinationSnapshot = nil;
      if (!fixedFunctionSourceOver) {
        MTLTextureDescriptor *snapshotDescriptor =
            [MTLTextureDescriptor
                texture2DDescriptorWithPixelFormat:targetTexture.pixelFormat
                                             width:destination.width()
                                            height:destination.height()
                                         mipmapped:NO];
        snapshotDescriptor.storageMode = MTLStorageModePrivate;
        snapshotDescriptor.usage = MTLTextureUsageShaderRead;
        destinationSnapshot =
            [device newTextureWithDescriptor:snapshotDescriptor];
      }
      id<MTLCommandBuffer> commandBuffer = [ganeshQueue_ commandBuffer];
      if (distanceTexture == nil ||
          (!fixedFunctionSourceOver && destinationSnapshot == nil) ||
          commandBuffer == nil) {
        [gradientLutTexture release];
        [destinationSnapshot release];
        [distanceTexture release];
        error = "VideoCut text SDF material GPU allocation failed";
        return false;
      }
      commandBuffer.label = @"VideoCut TextSdfMaterial";

      TextSdfEncodingResources encodingResources;
      if (!EncodeTextSdfDistance(request.mesh, *textSdfState_, device,
                                 commandBuffer, distanceTexture, true,
                                 encodingResources, error)) {
        [gradientLutTexture release];
        [destinationSnapshot release];
        [distanceTexture release];
        return false;
      }

      if (!fixedFunctionSourceOver) {
        id<MTLBlitCommandEncoder> snapshotBlit =
            [commandBuffer blitCommandEncoder];
        if (snapshotBlit == nil) {
          [gradientLutTexture release];
          [destinationSnapshot release];
          [distanceTexture release];
          error = "VideoCut text SDF destination snapshot encoder failed";
          return false;
        }
        [snapshotBlit copyFromTexture:targetTexture
                          sourceSlice:0U
                          sourceLevel:0U
                         sourceOrigin:MTLOriginMake(0U, 0U, 0U)
                           sourceSize:MTLSizeMake(destination.width(),
                                                  destination.height(), 1U)
                            toTexture:destinationSnapshot
                     destinationSlice:0U
                     destinationLevel:0U
                    destinationOrigin:MTLOriginMake(0U, 0U, 0U)];
        [snapshotBlit endEncoding];
      }

      MTLRenderPassDescriptor *materialPass =
          [MTLRenderPassDescriptor renderPassDescriptor];
      materialPass.colorAttachments[0].texture = targetTexture;
      materialPass.colorAttachments[0].loadAction = MTLLoadActionLoad;
      materialPass.colorAttachments[0].storeAction = MTLStoreActionStore;
      id<MTLRenderCommandEncoder> materialEncoder =
          [commandBuffer renderCommandEncoderWithDescriptor:materialPass];
      if (materialEncoder == nil) {
        [gradientLutTexture release];
        [destinationSnapshot release];
        [distanceTexture release];
        error = "VideoCut text SDF material encoder creation failed";
        return false;
      }
      [materialEncoder setViewport:MTLViewport{
                                       0.0, 0.0,
                                       static_cast<double>(destination.width()),
                                       static_cast<double>(destination.height()),
                                       0.0, 1.0}];
      [materialEncoder setScissorRect:MTLScissorRect{
                                          0U, 0U,
                                          static_cast<NSUInteger>(
                                              destination.width()),
                                          static_cast<NSUInteger>(
                                              destination.height())}];
      [materialEncoder setCullMode:MTLCullModeNone];
      [materialEncoder setFrontFacingWinding:MTLWindingClockwise];
      [materialEncoder setRenderPipelineState:materialPipeline];
      [materialEncoder setDepthStencilState:
                           textSdfState_->materialDepthStencil];
      [materialEncoder setVertexBytes:&uniforms
                               length:sizeof(uniforms)
                              atIndex:0U];
      [materialEncoder setFragmentBytes:&uniforms
                                 length:sizeof(uniforms)
                                atIndex:0U];
      [materialEncoder setFragmentTexture:distanceTexture atIndex:0U];
      [materialEncoder setFragmentTexture:
                           sourceTexture != nil ? sourceTexture
                                                : distanceTexture
                                     atIndex:1U];
      [materialEncoder setFragmentTexture:
                           destinationSnapshot != nil ? destinationSnapshot
                                                      : distanceTexture
                                     atIndex:2U];
      [materialEncoder setFragmentTexture:
                           replacementMaskTexture != nil
                               ? replacementMaskTexture
                               : distanceTexture
                                     atIndex:3U];
      // Captured TEXT_SDF_BLUR_MAT renders into RG8Unorm. Its fragment output
      // carries the original packed distance in RG and the 25-tap result in
      // BA; the target retains RG, so TextPro subsequently observes a second
      // atlas input that is byte-identical to _MainTex. Keep the two texture
      // bindings distinct at the Letter shader ABI while aliasing the native
      // RG8 resource exactly as the pre-refactor path did.
      [materialEncoder setFragmentTexture:distanceTexture atIndex:4U];
      [materialEncoder setFragmentTexture:
                           gradientLutTexture != nil ? gradientLutTexture
                                                     : distanceTexture
                                     atIndex:5U];
      [materialEncoder setFragmentSamplerState:
                           textSdfState_->distanceSampler
                                         atIndex:0U];
      [materialEncoder setFragmentSamplerState:textSdfState_->sourceSampler
                                         atIndex:1U];
      [materialEncoder setFragmentSamplerState:
                           textSdfState_->destinationSampler
                                         atIndex:2U];
      [materialEncoder setFragmentSamplerState:textSdfState_->gradientSampler
                                         atIndex:3U];
      [materialEncoder drawPrimitives:MTLPrimitiveTypeTriangle
                          vertexStart:0U
                          vertexCount:6U];
      [materialEncoder endEncoding];
      id<MTLTexture> diagnosticDistanceReadback = nil;
      id<MTLTexture> diagnosticSourceReadback = nil;
      const char *diagnosticAtlasDirectory =
          std::getenv("VIDEOCUT_DUMP_TEXT_SDF_ATLAS_DIR");
      const char *diagnosticSourceDirectory =
          std::getenv("VIDEOCUT_DUMP_TEXT_SDF_SOURCE_TEXTURE_DIR");
      static unsigned diagnosticAtlasOrdinal = 0U;
      static unsigned diagnosticSourceOrdinal = 0U;
      const bool dumpDiagnosticAtlas =
          diagnosticAtlasDirectory != nullptr &&
          diagnosticAtlasDirectory[0] != '\0' && diagnosticAtlasOrdinal < 2U;
      const bool dumpDiagnosticSource =
          diagnosticSourceDirectory != nullptr &&
          diagnosticSourceDirectory[0] != '\0' && sourceTexture != nil &&
          (sourceTexture.pixelFormat == MTLPixelFormatRGBA8Unorm ||
           sourceTexture.pixelFormat == MTLPixelFormatBGRA8Unorm) &&
          diagnosticSourceOrdinal < 64U;
      unsigned currentDiagnosticAtlasOrdinal = 0U;
      unsigned currentDiagnosticSourceOrdinal = 0U;
      if (dumpDiagnosticAtlas) {
        currentDiagnosticAtlasOrdinal = diagnosticAtlasOrdinal++;
        MTLTextureDescriptor *readbackDescriptor =
            [MTLTextureDescriptor
                texture2DDescriptorWithPixelFormat:MTLPixelFormatRG8Unorm
                                             width:request.mesh.targetWidth
                                            height:request.mesh.targetHeight
                                         mipmapped:NO];
        readbackDescriptor.storageMode = MTLStorageModeShared;
        readbackDescriptor.usage = MTLTextureUsageShaderRead;
        diagnosticDistanceReadback =
            [device newTextureWithDescriptor:readbackDescriptor];
        id<MTLBlitCommandEncoder> diagnosticBlit =
            diagnosticDistanceReadback != nil
                ? [commandBuffer blitCommandEncoder]
                : nil;
        if (diagnosticBlit != nil) {
          [diagnosticBlit copyFromTexture:distanceTexture
                              sourceSlice:0U
                              sourceLevel:0U
                             sourceOrigin:MTLOriginMake(0U, 0U, 0U)
                               sourceSize:MTLSizeMake(request.mesh.targetWidth,
                                                      request.mesh.targetHeight,
                                                      1U)
                                toTexture:diagnosticDistanceReadback
                         destinationSlice:0U
                         destinationLevel:0U
                        destinationOrigin:MTLOriginMake(0U, 0U, 0U)];
          [diagnosticBlit endEncoding];
        }
      }
      if (dumpDiagnosticSource) {
        currentDiagnosticSourceOrdinal = diagnosticSourceOrdinal++;
        MTLTextureDescriptor *readbackDescriptor =
            [MTLTextureDescriptor
                texture2DDescriptorWithPixelFormat:sourceTexture.pixelFormat
                                             width:sourceTexture.width
                                            height:sourceTexture.height
                                         mipmapped:NO];
        readbackDescriptor.storageMode = MTLStorageModeShared;
        readbackDescriptor.usage = MTLTextureUsageShaderRead;
        diagnosticSourceReadback =
            [device newTextureWithDescriptor:readbackDescriptor];
        id<MTLBlitCommandEncoder> diagnosticBlit =
            diagnosticSourceReadback != nil
                ? [commandBuffer blitCommandEncoder]
                : nil;
        if (diagnosticBlit != nil) {
          [diagnosticBlit copyFromTexture:sourceTexture
                              sourceSlice:0U
                              sourceLevel:0U
                             sourceOrigin:MTLOriginMake(0U, 0U, 0U)
                               sourceSize:MTLSizeMake(sourceTexture.width,
                                                      sourceTexture.height,
                                                      1U)
                                toTexture:diagnosticSourceReadback
                         destinationSlice:0U
                         destinationLevel:0U
                        destinationOrigin:MTLOriginMake(0U, 0U, 0U)];
          [diagnosticBlit endEncoding];
        }
      }
      if (!SubmitNativeTextureCommand(commandBuffer, Submission(),
          {sourceTexture, targetTexture, replacementMaskTexture, gradientLutTexture, distanceTexture, destinationSnapshot, encodingResources.depthTexture, encodingResources.distanceBuffer, encodingResources.shapeBuffer, diagnosticDistanceReadback, diagnosticSourceReadback}, error)) {
        [diagnosticDistanceReadback release]; [diagnosticSourceReadback release]; [gradientLutTexture release]; [destinationSnapshot release]; [distanceTexture release];
        return false;
      }
      if (diagnosticDistanceReadback != nil ||
          diagnosticSourceReadback != nil) {
        [commandBuffer waitUntilCompleted];
      }
      if (diagnosticDistanceReadback != nil) {
        const std::size_t rowBytes =
            static_cast<std::size_t>(request.mesh.targetWidth) * 2U;
        std::vector<std::uint8_t> pixels(
            rowBytes * static_cast<std::size_t>(request.mesh.targetHeight));
        [diagnosticDistanceReadback
            getBytes:pixels.data()
         bytesPerRow:rowBytes
          fromRegion:MTLRegionMake2D(0U, 0U, request.mesh.targetWidth,
                                     request.mesh.targetHeight)
         mipmapLevel:0U];
        char diagnosticPath[4096]{};
        const int pathLength = std::snprintf(
            diagnosticPath, sizeof(diagnosticPath),
            "%s/text-sdf-atlas-%u-%ux%u-rg8.bin", diagnosticAtlasDirectory,
            currentDiagnosticAtlasOrdinal, request.mesh.targetWidth,
            request.mesh.targetHeight);
        if (pathLength > 0 &&
            static_cast<std::size_t>(pathLength) < sizeof(diagnosticPath)) {
          if (FILE *file = std::fopen(diagnosticPath, "wb")) {
            std::fwrite(pixels.data(), 1U, pixels.size(), file);
            std::fclose(file);
          }
          const auto writeDiagnosticVertices = [&](const char *kind,
                                                   const void *bytes,
                                                   const std::size_t size) {
            char vertexPath[4096]{};
            const int vertexPathLength = std::snprintf(
                vertexPath, sizeof(vertexPath), "%s/text-sdf-%s-%u.bin",
                diagnosticAtlasDirectory, kind,
                currentDiagnosticAtlasOrdinal);
            if (vertexPathLength <= 0 ||
                static_cast<std::size_t>(vertexPathLength) >=
                    sizeof(vertexPath))
              return;
            if (FILE *file = std::fopen(vertexPath, "wb")) {
              std::fwrite(bytes, 1U, size, file);
              std::fclose(file);
            }
          };
          writeDiagnosticVertices(
              "distance-vertices", request.mesh.distanceVertices.data(),
              request.mesh.distanceVertices.size() *
                  sizeof(TextSdfDistanceVertex));
          writeDiagnosticVertices(
              "shape-vertices", request.mesh.shapeVertices.data(),
              request.mesh.shapeVertices.size() * sizeof(TextSdfShapeVertex));
        }
        [diagnosticDistanceReadback release];
      }
      if (diagnosticSourceReadback != nil) {
        const std::size_t rowBytes =
            static_cast<std::size_t>(diagnosticSourceReadback.width) * 4U;
        std::vector<std::uint8_t> pixels(
            rowBytes *
            static_cast<std::size_t>(diagnosticSourceReadback.height));
        [diagnosticSourceReadback
            getBytes:pixels.data()
         bytesPerRow:rowBytes
          fromRegion:MTLRegionMake2D(0U, 0U, diagnosticSourceReadback.width,
                                     diagnosticSourceReadback.height)
         mipmapLevel:0U];
        char diagnosticPath[4096]{};
        const int pathLength = std::snprintf(
            diagnosticPath, sizeof(diagnosticPath),
            "%s/text-sdf-source-%u-%lux%lu-pf%lu.bin",
            diagnosticSourceDirectory, currentDiagnosticSourceOrdinal,
            static_cast<unsigned long>(diagnosticSourceReadback.width),
            static_cast<unsigned long>(diagnosticSourceReadback.height),
            static_cast<unsigned long>(diagnosticSourceReadback.pixelFormat));
        if (pathLength > 0 &&
            static_cast<std::size_t>(pathLength) < sizeof(diagnosticPath)) {
          if (FILE *file = std::fopen(diagnosticPath, "wb")) {
            std::fwrite(pixels.data(), 1U, pixels.size(), file);
            std::fclose(file);
          }
        }
        [diagnosticSourceReadback release];
      }
      [destinationSnapshot release];
      [distanceTexture release];
      [gradientLutTexture release];
      error.clear();
      return true;
    }
  }

  std::shared_ptr<void> RenderTextSdfAtlas(
      const TextSdfGpuMesh &mesh, const std::vector<SkIRect> &cells,
      std::string &error, std::vector<std::uint8_t> *diagnosticRg8) override {
    @autoreleasepool {
      if (diagnosticRg8)
        diagnosticRg8->clear();
      float distanceRange = 0.0F;
      if (!ValidateTextSdfGpuMesh(mesh, distanceRange, error))
        return {};
      id<MTLDevice> device = (__bridge id<MTLDevice>)(
          reinterpret_cast<void *>(executionDevice_->nativeDeviceHandle()));
      if (device == nil || !context_ || context_->abandoned() ||
          ganeshQueue_ == nil || cells.empty() ||
          mesh.targetWidth > kMaximumMetalTextSdfTextureExtent ||
          mesh.targetHeight >
              kMaximumMetalTextSdfTextureExtent) {
        error = "text SDF Metal device is unavailable or the target is too "
                "large";
        return {};
      }
      const auto extent = SkIRect::MakeWH(mesh.targetWidth, mesh.targetHeight);
      for (const auto &cell : cells) {
        if (cell.isEmpty() || !extent.contains(cell)) {
          error = "text SDF atlas cell is outside its target";
          return {};
        }
      }
      if (!textSdfState_) {
        textSdfState_ = CreateTextSdfMetalState(device, error);
        if (!textSdfState_)
          return {};
      }

      MTLTextureDescriptor *colorDescriptor =
          [MTLTextureDescriptor
              texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                           width:mesh.targetWidth
                                          height:mesh.targetHeight
                                       mipmapped:NO];
      colorDescriptor.storageMode = MTLStorageModePrivate;
      colorDescriptor.usage = MTLTextureUsageRenderTarget |
                              MTLTextureUsageShaderRead;
      id<MTLTexture> colorTexture =
          [device newTextureWithDescriptor:colorDescriptor];
      id<MTLTexture> atlasTexture =
          [device newTextureWithDescriptor:colorDescriptor];

      const NSUInteger compactRowBytes =
          static_cast<NSUInteger>(mesh.targetWidth) * 4U;
      const NSUInteger alignment = std::max<NSUInteger>(
          1U, [device minimumLinearTextureAlignmentForPixelFormat:
                         MTLPixelFormatRGBA8Unorm]);
      const NSUInteger readbackRowBytes =
          ((compactRowBytes + alignment - 1U) / alignment) * alignment;
      id<MTLBuffer> readbackBuffer = diagnosticRg8
          ? [device newBufferWithLength:readbackRowBytes *
                                        static_cast<NSUInteger>(mesh.targetHeight)
                                options:MTLResourceStorageModeShared]
          : nil;

      const auto releaseResources = [&]() {
        [readbackBuffer release];
        [atlasTexture release];
        [colorTexture release];
      };
      if (colorTexture == nil || atlasTexture == nil ||
          (diagnosticRg8 && readbackBuffer == nil)) {
        error = "text SDF Metal resource allocation failed";
        releaseResources();
        return {};
      }

      FlushGpu();
      id<MTLCommandBuffer> commandBuffer =
          [ganeshQueue_ commandBuffer];
      commandBuffer.label = @"VideoCut TextSdfAtlas";
      TextSdfEncodingResources encodingResources;
      if (!EncodeTextSdfDistance(mesh, *textSdfState_, device,
                                 commandBuffer, colorTexture, false,
                                 encodingResources, error)) {
        releaseResources();
        return {};
      }

      // The previous CPU round trip flipped rows twice and copied only packed
      // cells. In Metal row order that is exactly an identity cell copy.
      MTLRenderPassDescriptor *clear = [MTLRenderPassDescriptor renderPassDescriptor];
      clear.colorAttachments[0].texture = atlasTexture;
      clear.colorAttachments[0].loadAction = MTLLoadActionClear;
      clear.colorAttachments[0].storeAction = MTLStoreActionStore;
      clear.colorAttachments[0].clearColor = MTLClearColorMake(0, 0, 0, 0);
      id<MTLRenderCommandEncoder> clearEncoder =
          [commandBuffer renderCommandEncoderWithDescriptor:clear];
      if (clearEncoder == nil) {
        error = "text SDF atlas clear encoder creation failed";
        releaseResources();
        return {};
      }
      [clearEncoder endEncoding];
      id<MTLBlitCommandEncoder> blit = [commandBuffer blitCommandEncoder];
      if (blit == nil) {
        error = "text SDF atlas copy encoder creation failed";
        releaseResources();
        return {};
      }
      for (const auto &cell : cells) {
        const auto origin = MTLOriginMake(cell.x(), cell.y(), 0);
        [blit copyFromTexture:colorTexture sourceSlice:0 sourceLevel:0
                 sourceOrigin:origin
                   sourceSize:MTLSizeMake(cell.width(), cell.height(), 1)
                    toTexture:atlasTexture destinationSlice:0 destinationLevel:0
             destinationOrigin:origin];
      }
      if (diagnosticRg8) {
        CopyTextureToBuffer(blit, atlasTexture, readbackBuffer,
                            MTLSizeMake(mesh.targetWidth, mesh.targetHeight, 1),
                            readbackRowBytes,
                            readbackRowBytes * mesh.targetHeight);
      }
      [blit endEncoding];
      if (!SubmitNativeTextureCommand(commandBuffer, Submission(),
          {colorTexture, atlasTexture, readbackBuffer, encodingResources.depthTexture, encodingResources.distanceBuffer, encodingResources.shapeBuffer}, error)) {
        releaseResources();
        return {};
      }
      if (diagnosticRg8) {
        [commandBuffer waitUntilCompleted];
        if (commandBuffer.status != MTLCommandBufferStatusCompleted) {
          error = MetalErrorMessage(@"text SDF Metal command failed",
                                    commandBuffer.error);
          releaseResources();
          return {};
        }
        diagnosticRg8->resize(static_cast<std::size_t>(mesh.targetWidth) *
                              mesh.targetHeight * 2U);
        const auto *source =
            static_cast<const std::uint8_t *>(readbackBuffer.contents);
        for (int row = 0; row < mesh.targetHeight; ++row) {
          for (int column = 0; column < mesh.targetWidth; ++column) {
            const auto src = static_cast<std::size_t>(row) * readbackRowBytes +
                             static_cast<std::size_t>(column) * 4U;
            const auto dst = (static_cast<std::size_t>(row) * mesh.targetWidth +
                              static_cast<std::size_t>(column)) * 2U;
            (*diagnosticRg8)[dst] = source[src];
            (*diagnosticRg8)[dst + 1U] = source[src + 1U];
          }
        }
      }
      std::shared_ptr<void> result((void *)[atlasTexture retain], [](void *value) {
        [(id<MTLTexture>)value release];
      });
      releaseResources();
      error.clear();
      return result;
    }
  }

private:
  void FlushGpu() {
    // This sole Metal bridge is tied to the repository's pinned Skia artifact.
    // Public finished callbacks omit native error status and may be delivered
    // only by a later Ganesh poll. Attach to the actual encoded Metal command
    // after flush instead, before submit; an empty buffer has no receipt.
    context_->flush();
    if (context_->abandoned()) return;
    auto *gpu = static_cast<GrMtlGpu *>(context_->priv().getGpu());
    auto *command = gpu ? gpu->commandBuffer() : nullptr;
    if (!command || !command->hasWork()) return;
    std::size_t retainedBytes = 0;
    context_->getResourceCacheUsage(nullptr, &retainedBytes);
    std::string error;
    auto admission = gpu_execution::detail::WaitForAppleNativeCompletionAdmission(
        retainedBytes, error);
    const auto timeline = completionTimeline_;
    const auto serial = timeline->Append();
    if (!admission) {
      timeline->Complete(serial, false, error);
      context_->abandonContext();
      return;
    }
    @try {
      command->addCompletedHandler(^(id<MTLCommandBuffer> completed) {
        @autoreleasepool {
          const bool success = completed.status == MTLCommandBufferStatusCompleted;
          timeline->Complete(serial, success, success ? std::string{} :
              MetalErrorMessage(@"Ganesh GPU command failed", completed.error));
          (void)admission;
        }
      });
      if (!context_->submit(GrSyncCpu::kNo))
        timeline->Complete(serial, false, "Ganesh GPU submission failed");
    } @catch (NSException *) {
      // The registered block owns its bounded admission until Metal retires.
      context_->abandonContext();
    }
  }

  bool SubmitNative(id<MTLCommandBuffer> command, std::string &error,
                    std::size_t retainedBytes,
                    std::shared_ptr<const void> storageLease = {}) {
    if (!context_ || context_->abandoned() || !completionTimeline_->healthy() || !command ||
        command.commandQueue != ganeshQueue_ ||
        !completionTimeline_->event()) {
      error = "Skia native command does not belong to its producing lane";
      return false;
    }
    auto admission = gpu_execution::detail::WaitForAppleNativeCompletionAdmission(
        retainedBytes, error);
    if (!admission)
      return false;
    const auto timeline = completionTimeline_;
    const auto serial = timeline->Append();
    lastSubmission_ = serial;
    @try {
      [command addCompletedHandler:^(id<MTLCommandBuffer> completed) {
        @autoreleasepool {
          const bool success = completed.status == MTLCommandBufferStatusCompleted;
          const auto message = success ? std::string{} :
              MetalErrorMessage(@"Skia native GPU command failed", completed.error);
          timeline->Complete(serial, success, message);
          // Both the admission and the allocation (not its fence) survive to
          // native completion. The block owns no command, avoiding a cycle.
          (void)admission;
          (void)storageLease;
        }
      }];
      [command commit];
    } @catch (NSException *) {
      error = "Skia native command submission raised an exception";
      // Ownership can be indeterminate after callback registration. The block
      // retains its bounded admission; do not publish or reuse this lane.
      context_->abandonContext();
      return false;
    }
    error.clear();
    return true;
  }

  NativeCommandSubmission Submission() {
    return [this](void *command, std::size_t retainedBytes, std::string &error) {
      return SubmitNative((id<MTLCommandBuffer>)command, error, retainedBytes);
    };
  }

  MetalSkiaGpuContext(std::shared_ptr<gpu_execution::Device> executionDevice,
                      sk_sp<GrDirectContext> context,
                      id<MTLCommandQueue> ganeshQueue)
      : executionDevice_(std::move(executionDevice)),
        context_(std::move(context)), ganeshQueue_(ganeshQueue),
        completionTimeline_(std::make_shared<SkiaMetalCompletionTimeline>(
            ganeshQueue.device)) {}

  std::shared_ptr<gpu_execution::Device> executionDevice_;
  sk_sp<GrDirectContext> context_;
  id<MTLCommandQueue> ganeshQueue_{nil};
  id<MTLComputePipelineState> publicationPipeline_{nil};
  std::shared_ptr<SkiaMetalCompletionTimeline> completionTimeline_;
  std::uint64_t lastSubmission_{0};
  std::unique_ptr<TextSdfMetalState> textSdfState_;
  std::unique_ptr<QtTextRenderGroupCompositeMetalState>
      qtTextRenderGroupCompositeState_;
  std::unique_ptr<QtTextFollowerCompositeMetalState>
      qtTextFollowerCompositeState_;
  std::unique_ptr<TextVatMeshMetalState> textVatMeshState_;
};

} // namespace

std::unique_ptr<SkiaGpuContext> CreateSkiaGpuContext(
    std::string &error, std::int32_t deviceIndex, std::uint64_t deviceGeneration) {
  @autoreleasepool {
    return MetalSkiaGpuContext::Create(error, deviceIndex, deviceGeneration);
  }
}

bool NativeTextGpuRequired() noexcept { return true; }

} // namespace videocut::skia_runtime::internal
