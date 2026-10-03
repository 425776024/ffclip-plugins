#include "runtime/MetalRuntimeSupport.h"
#include "text/NativeMetalSubmission.h"
#include "text/shaders/MetalQtTextSoftGlow.h"

#include "text/QtTextSoftGlowMetalRuntime.h"

#include "videocut/gpu_execution/GpuExecution.h"

#import <Metal/Metal.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <utility>

namespace videocut::skia_runtime::internal {
namespace {

constexpr const char *kSoftGlowMetalSource =
    shader_sources::MetalQtTextSoftGlow;

struct SoftGlowThresholdUniforms final {
  std::int32_t thresholdType;
  float thresholdLow;
  float thresholdHigh;
  float thresholdSmooth;
  float grayScale;
};

struct SoftGlowXUniforms final {
  float sampleCount;
  float sigma;
  float step;
};

struct SoftGlowYUniforms final {
  float sampleCount;
  float sigma;
  float step;
  float exposure;
};

struct alignas(16) SoftGlowBlendUniforms final {
  float exposure;
  std::uint32_t padding0[3];
  float glowColor[3];
  std::uint32_t padding1;
  std::int32_t displayGlow;
  std::uint32_t padding2[3];
};

struct DeepGlowPreprocessUniforms final {
  float glowFromAlpha;
  std::int32_t chromaticAberration;
  float redOffset;
  float greenOffset;
  float blueOffset;
  std::int32_t gammaCorrect;
  float gammaValue;
};

struct DeepGlowBlurUniforms final {
  float gammaValue;
  float stepsInt;
  float angle;
  std::uint32_t padding0;
  float aspect[2];
  float rotate;
  float steps;
  float stride;
  float sigma;
};

struct DeepGlowCompositeUniforms final {
  std::int32_t blendMode;
  float opacity;
  float gammaValue;
  std::int32_t composite;
  float multiplier;
};

struct alignas(16) DeepGlowPostprocessUniforms final {
  std::int32_t blendMode;
  std::int32_t tint;
  std::int32_t tintMode;
  std::uint32_t padding0;
  // MSL float3 occupies one 16-byte constant-buffer slot.
  float tintColor[4];
  float tintMix;
  float sourceOpacity;
  std::uint32_t padding1[2];
};

constexpr std::array<MetalTexturedVertex, 4> kFullscreenQuad{{
    {-1.0F, -1.0F, 0.0F, 0.0F},
    {1.0F, -1.0F, 1.0F, 0.0F},
    {1.0F, 1.0F, 1.0F, 1.0F},
    {-1.0F, 1.0F, 0.0F, 1.0F},
}};
constexpr std::array<std::uint32_t, 6> kFullscreenIndices{
    0U, 1U, 2U, 0U, 2U, 3U};

static_assert(sizeof(SoftGlowThresholdUniforms) == 20U);
static_assert(sizeof(SoftGlowXUniforms) == 12U);
static_assert(sizeof(SoftGlowYUniforms) == 16U);
static_assert(sizeof(SoftGlowBlendUniforms) == 48U);
static_assert(sizeof(DeepGlowPreprocessUniforms) == 28U);
static_assert(sizeof(DeepGlowBlurUniforms) == 40U);
static_assert(sizeof(DeepGlowCompositeUniforms) == 20U);
static_assert(sizeof(DeepGlowPostprocessUniforms) == 48U);

bool ValidateImageView(const QtTextSoftGlowRawRgba8ImageView &image,
                       std::string &error) {
  if ((!image.pixels && !image.nativeTexture) ||
      image.width <= 0 || image.height <= 0) {
    error = "Qt SoftGlow source image is empty";
    return false;
  }
  if (image.nativeTexture)
    return true;
  const auto width = static_cast<std::size_t>(image.width);
  const auto height = static_cast<std::size_t>(image.height);
  if (width > std::numeric_limits<std::size_t>::max() / 4U) {
    error = "Qt SoftGlow source row size overflows";
    return false;
  }
  const std::size_t compactRowBytes = width * 4U;
  if (image.rowBytes < compactRowBytes ||
      height > std::numeric_limits<std::size_t>::max() / image.rowBytes) {
    error = "Qt SoftGlow source rowBytes is invalid";
    return false;
  }
  const std::size_t requiredBytes =
      (height - 1U) * image.rowBytes + compactRowBytes;
  if (image.byteSize < requiredBytes) {
    error = "Qt SoftGlow source byteSize is smaller than its final row";
    return false;
  }
  return true;
}

bool ValidateNativeTarget(const QtTextSoftGlowRawRgba8ImageView &source,
                          const QtTextSoftGlowMetalTextureTarget *target,
                          id<MTLDevice> device, std::string &error) {
  if (!source.nativeTexture && !target)
    return true;
  if (!source.nativeTexture || !target || !target->texture ||
      !target->commandQueue) {
    error = "Qt Glow native source, target and queue must be supplied together";
    return false;
  }
  id<MTLTexture> input = (id<MTLTexture>)source.nativeTexture;
  id<MTLTexture> output = (id<MTLTexture>)target->texture;
  id<MTLCommandQueue> queue = (id<MTLCommandQueue>)target->commandQueue;
  if (input.device != device || output.device != device ||
      queue.device != device || input.width != static_cast<NSUInteger>(source.width) ||
      input.height != static_cast<NSUInteger>(source.height) ||
      output.width != input.width || output.height != input.height ||
      input.pixelFormat != MTLPixelFormatRGBA8Unorm ||
      output.pixelFormat != MTLPixelFormatRGBA8Unorm ||
      input.sampleCount != 1U || output.sampleCount != 1U ||
      !(input.usage & MTLTextureUsageShaderRead) ||
      !(output.usage & MTLTextureUsageRenderTarget)) {
    error = "Qt Glow native texture device, format, extent or usage is invalid";
    return false;
  }
  return true;
}

bool ValidateRequest(const QtTextSoftGlowMetalRequest &request,
                     std::string &error) {
  if (request.implementationVersion !=
      kQtTextSoftGlowMetalImplementationVersion) {
    error = "Qt SoftGlow implementation version is unsupported";
    return false;
  }
  if (!ValidateImageView(request.source, error))
    return false;
  if (request.glowWidth <= 0 || request.glowHeight <= 0 ||
      request.thresholdType < 0 || request.thresholdType > 1 ||
      request.displayGlow < 0 || request.displayGlow > 1) {
    error = "Qt SoftGlow topology contract is invalid";
    return false;
  }
  const std::array<float, 13> values{
      request.thresholdLow, request.thresholdHigh, request.thresholdSmooth,
      request.grayScale,    request.sampleCount,   request.sigmaX,
      request.sigmaY,       request.stepX,         request.stepY,
      request.exposure,     request.glowColor[0],  request.glowColor[1],
      request.glowColor[2]};
  if (!std::all_of(values.begin(), values.end(),
                   [](const float value) { return std::isfinite(value); }) ||
      request.sampleCount < 0.0F || request.sampleCount > 1024.0F ||
      request.stepX < 0.0F || request.stepY < 0.0F ||
      request.exposure < 0.0F ||
      (request.sampleCount >= 0.00001F &&
       (request.sigmaX <= 0.0F || request.sigmaY <= 0.0F))) {
    error = "Qt SoftGlow numeric contract is invalid";
    return false;
  }
  return true;
}

bool ValidateDeepGlowRequest(const QtTextDeepGlowMetalRequest &request,
                             std::string &error) {
  if (request.implementationVersion !=
      kQtTextDeepGlowMetalImplementationVersion) {
    error = "Qt DeepGlow implementation version is unsupported";
    return false;
  }
  if (!ValidateImageView(request.source, error))
    return false;
  if (request.blurWidth <= 0 || request.blurHeight <= 0 ||
      request.glowIterations < 1 || request.glowIterations > 8 ||
      request.postprocessIteration < 1 ||
      request.postprocessIteration > request.glowIterations ||
      request.chromaticAberration < 0 || request.chromaticAberration > 1 ||
      request.gammaCorrect < 0 || request.gammaCorrect > 1 ||
      request.compositeBlendMode < 0 || request.compositeBlendMode > 1 ||
      request.postprocessBlendMode < 0 ||
      request.postprocessBlendMode > 1 || request.tint < 0 ||
      request.tint > 1 || request.tintMode < 0 || request.tintMode > 3) {
    error = "Qt DeepGlow topology contract is invalid";
    return false;
  }
  const std::array<float, 20> scalarValues{
      request.glowFromAlpha,
      request.redOffset,
      request.greenOffset,
      request.blueOffset,
      request.authoredGamma,
      request.blurGamma,
      request.stepsInt,
      request.aspect[0],
      request.aspect[1],
      request.rotateDegrees,
      request.ratio,
      request.exposure,
      request.tintColor[0],
      request.tintColor[1],
      request.tintColor[2],
      request.tintMix,
      request.sourceOpacity,
      request.sampledSteps[0],
      request.strides[0],
      request.opacities[0],
  };
  const auto finite = [](const auto &values) {
    return std::all_of(values.begin(), values.end(),
                       [](const float value) { return std::isfinite(value); });
  };
  if (!finite(scalarValues) || !finite(request.sampledSteps) ||
      !finite(request.strides) || !finite(request.opacities) ||
      request.authoredGamma <= 0.0F || request.blurGamma <= 0.0F ||
      request.stepsInt < 1.0F || request.aspect[0] <= 0.0F ||
      request.aspect[1] <= 0.0F || request.ratio < 0.0F ||
      request.ratio > 2.0F || request.exposure < 0.0F) {
    error = "Qt DeepGlow numeric contract is invalid";
    return false;
  }
  return true;
}

struct SoftGlowMetalState final {
  id<MTLCommandQueue> queue{nil};
  id<MTLRenderPipelineState> thresholdPipeline{nil};
  id<MTLRenderPipelineState> copyPipeline{nil};
  id<MTLRenderPipelineState> horizontalPipeline{nil};
  id<MTLRenderPipelineState> verticalPipeline{nil};
  id<MTLRenderPipelineState> blendPipeline{nil};
  id<MTLRenderPipelineState> deepGlowPreprocessPipeline{nil};
  id<MTLRenderPipelineState> deepGlowDownscalePipeline{nil};
  id<MTLRenderPipelineState> deepGlowBlurPipeline{nil};
  id<MTLRenderPipelineState> deepGlowCompositePipeline{nil};
  id<MTLRenderPipelineState> deepGlowPostprocessPipeline{nil};
  id<MTLSamplerState> sampler{nil};
  id<MTLBuffer> vertexBuffer{nil};
  id<MTLBuffer> indexBuffer{nil};

  ~SoftGlowMetalState() {
    [indexBuffer release];
    [vertexBuffer release];
    [sampler release];
    [deepGlowPostprocessPipeline release];
    [deepGlowCompositePipeline release];
    [deepGlowBlurPipeline release];
    [deepGlowDownscalePipeline release];
    [deepGlowPreprocessPipeline release];
    [blendPipeline release];
    [verticalPipeline release];
    [horizontalPipeline release];
    [copyPipeline release];
    [thresholdPipeline release];
    [queue release];
  }
};

id<MTLRenderPipelineState>
MakePipeline(id<MTLDevice> device, id<MTLFunction> vertex,
             id<MTLFunction> fragment, MTLVertexDescriptor *vertexDescriptor,
             NSString *label, std::string &error) {
  auto *descriptor = [[MTLRenderPipelineDescriptor alloc] init];
  descriptor.label = label;
  descriptor.vertexFunction = vertex;
  descriptor.fragmentFunction = fragment;
  descriptor.vertexDescriptor = vertexDescriptor;
  descriptor.rasterSampleCount = 1U;
  descriptor.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
  auto *color = descriptor.colorAttachments[0];
  color.pixelFormat = MTLPixelFormatRGBA8Unorm;
  color.blendingEnabled = NO;
  color.writeMask = MTLColorWriteMaskAll;
  NSError *pipelineError = nil;
  id<MTLRenderPipelineState> pipeline =
      [device newRenderPipelineStateWithDescriptor:descriptor
                                             error:&pipelineError];
  [descriptor release];
  if (pipeline == nil)
    error = MetalErrorMessage(@"Qt SoftGlow Metal pipeline creation failed",
                              pipelineError);
  return pipeline;
}

std::unique_ptr<SoftGlowMetalState>
CreateMetalState(id<MTLDevice> device, std::string &error) {
  auto state = std::make_unique<SoftGlowMetalState>();
  state->queue = [device newCommandQueue];
  if (state->queue == nil) {
    error = "Qt SoftGlow Metal command queue creation failed";
    return {};
  }

  NSError *libraryError = nil;
  id<MTLLibrary> library = [device
      newLibraryWithSource:[NSString stringWithUTF8String:kSoftGlowMetalSource]
                   options:nil
                     error:&libraryError];
  if (library == nil) {
    error = MetalErrorMessage(@"Qt SoftGlow Metal shader compilation failed",
                              libraryError);
    return {};
  }
  id<MTLFunction> vertex =
      [library newFunctionWithName:@"qtTextSoftGlowVertex"];
  id<MTLFunction> threshold =
      [library newFunctionWithName:@"qtTextSoftGlowThresholdFragment"];
  id<MTLFunction> copy =
      [library newFunctionWithName:@"qtTextSoftGlowCopyFragment"];
  id<MTLFunction> horizontal =
      [library newFunctionWithName:@"qtTextSoftGlowXFragment"];
  id<MTLFunction> vertical =
      [library newFunctionWithName:@"qtTextSoftGlowYFragment"];
  id<MTLFunction> blend =
      [library newFunctionWithName:@"qtTextSoftGlowBlendFragment"];
  id<MTLFunction> deepGlowPreprocess =
      [library newFunctionWithName:@"qtTextDeepGlowPreprocessFragment"];
  id<MTLFunction> deepGlowDownscale =
      [library newFunctionWithName:@"qtTextDeepGlowDownscaleFragment"];
  id<MTLFunction> deepGlowBlur =
      [library newFunctionWithName:@"qtTextDeepGlowBlurFragment"];
  id<MTLFunction> deepGlowComposite =
      [library newFunctionWithName:@"qtTextDeepGlowCompositeFragment"];
  id<MTLFunction> deepGlowPostprocess =
      [library newFunctionWithName:@"qtTextDeepGlowPostprocessFragment"];

  auto *layout = [[MTLVertexDescriptor alloc] init];
  layout.attributes[0].format = MTLVertexFormatFloat2;
  layout.attributes[0].offset = 0U;
  layout.attributes[0].bufferIndex = 0U;
  layout.attributes[1].format = MTLVertexFormatFloat2;
  layout.attributes[1].offset = 8U;
  layout.attributes[1].bufferIndex = 0U;
  layout.layouts[0].stride = sizeof(MetalTexturedVertex);
  layout.layouts[0].stepFunction = MTLVertexStepFunctionPerVertex;
  layout.layouts[0].stepRate = 1U;

  if (vertex == nil || threshold == nil || copy == nil || horizontal == nil ||
      vertical == nil || blend == nil || deepGlowPreprocess == nil ||
      deepGlowDownscale == nil || deepGlowBlur == nil ||
      deepGlowComposite == nil || deepGlowPostprocess == nil) {
    error = "Qt captured glow Metal shader entry point is unavailable";
  } else {
    state->thresholdPipeline = MakePipeline(
        device, vertex, threshold, layout, @"LumiSoftGlow Threshold v1", error);
    if (state->thresholdPipeline != nil)
      state->copyPipeline = MakePipeline(device, vertex, copy, layout,
                                         @"LumiSoftGlow Copy v1", error);
    if (state->copyPipeline != nil)
      state->horizontalPipeline = MakePipeline(
          device, vertex, horizontal, layout, @"LumiSoftGlow X v1", error);
    if (state->horizontalPipeline != nil)
      state->verticalPipeline = MakePipeline(
          device, vertex, vertical, layout, @"LumiSoftGlow Y v1", error);
    if (state->verticalPipeline != nil)
      state->blendPipeline = MakePipeline(device, vertex, blend, layout,
                                          @"LumiSoftGlow Blend v1", error);
    if (state->blendPipeline != nil)
      state->deepGlowPreprocessPipeline = MakePipeline(
          device, vertex, deepGlowPreprocess, layout,
          @"LumiDeepGlow Preprocess v1", error);
    if (state->deepGlowPreprocessPipeline != nil)
      state->deepGlowDownscalePipeline = MakePipeline(
          device, vertex, deepGlowDownscale, layout,
          @"LumiDeepGlow Downscale v1", error);
    if (state->deepGlowDownscalePipeline != nil)
      state->deepGlowBlurPipeline = MakePipeline(
          device, vertex, deepGlowBlur, layout,
          @"LumiDeepGlow Blur v1", error);
    if (state->deepGlowBlurPipeline != nil)
      state->deepGlowCompositePipeline = MakePipeline(
          device, vertex, deepGlowComposite, layout,
          @"LumiDeepGlow Composite v1", error);
    if (state->deepGlowCompositePipeline != nil)
      state->deepGlowPostprocessPipeline = MakePipeline(
          device, vertex, deepGlowPostprocess, layout,
          @"LumiDeepGlow Postprocess v1", error);
  }
  [layout release];
  [deepGlowPostprocess release];
  [deepGlowComposite release];
  [deepGlowBlur release];
  [deepGlowDownscale release];
  [deepGlowPreprocess release];
  [blend release];
  [vertical release];
  [horizontal release];
  [copy release];
  [threshold release];
  [vertex release];
  [library release];
  if (state->thresholdPipeline == nil || state->copyPipeline == nil ||
      state->horizontalPipeline == nil || state->verticalPipeline == nil ||
      state->blendPipeline == nil ||
      state->deepGlowPreprocessPipeline == nil ||
      state->deepGlowDownscalePipeline == nil ||
      state->deepGlowBlurPipeline == nil ||
      state->deepGlowCompositePipeline == nil ||
      state->deepGlowPostprocessPipeline == nil) {
    return {};
  }

  state->sampler =
      MakeNormalizedClampSampler(device, MTLSamplerMinMagFilterLinear,
                                 @"Qt SoftGlow normalized linear clamp");

  state->vertexBuffer =
      [device newBufferWithBytes:kFullscreenQuad.data()
                          length:sizeof(kFullscreenQuad)
                         options:MTLResourceStorageModeShared];
  state->indexBuffer =
      [device newBufferWithBytes:kFullscreenIndices.data()
                          length:sizeof(kFullscreenIndices)
                         options:MTLResourceStorageModeShared];
  if (state->sampler == nil || state->vertexBuffer == nil ||
      state->indexBuffer == nil) {
    error = "Qt SoftGlow immutable Metal resource creation failed";
    return {};
  }
  error.clear();
  return state;
}

bool EncodePass(id<MTLCommandBuffer> commandBuffer,
                const SoftGlowMetalState &state,
                id<MTLRenderPipelineState> pipeline, id<MTLTexture> target,
                id<MTLTexture> depth, const int width, const int height,
                const void *uniforms, const NSUInteger uniformBytes,
                id<MTLTexture> texture0, id<MTLTexture> texture1,
                NSString *label, std::string &error) {
  MTLRenderPassDescriptor *pass =
      [MTLRenderPassDescriptor renderPassDescriptor];
  pass.colorAttachments[0].texture = target;
  pass.colorAttachments[0].loadAction = MTLLoadActionClear;
  pass.colorAttachments[0].storeAction = MTLStoreActionStore;
  pass.colorAttachments[0].clearColor =
      MTLClearColorMake(0.0, 0.0, 0.0, 0.0);
  pass.depthAttachment.texture = depth;
  pass.depthAttachment.loadAction = MTLLoadActionClear;
  pass.depthAttachment.storeAction = MTLStoreActionStore;
  pass.depthAttachment.clearDepth = 1.0;
  id<MTLRenderCommandEncoder> encoder =
      [commandBuffer renderCommandEncoderWithDescriptor:pass];
  if (encoder == nil) {
    error = "Qt SoftGlow render encoder creation failed";
    return false;
  }
  encoder.label = label;
  [encoder setViewport:MTLViewport{0.0, static_cast<double>(height),
                                   static_cast<double>(width),
                                   -static_cast<double>(height), 0.0, 1.0}];
  [encoder setCullMode:MTLCullModeNone];
  [encoder setFrontFacingWinding:MTLWindingClockwise];
  [encoder setTriangleFillMode:MTLTriangleFillModeFill];
  [encoder setRenderPipelineState:pipeline];
  [encoder setVertexBuffer:state.vertexBuffer offset:0U atIndex:0U];
  if (uniforms != nullptr && uniformBytes != 0U)
    [encoder setFragmentBytes:uniforms length:uniformBytes atIndex:0U];
  [encoder setFragmentTexture:texture0 atIndex:0U];
  [encoder setFragmentSamplerState:state.sampler atIndex:0U];
  if (texture1 != nil) {
    [encoder setFragmentTexture:texture1 atIndex:1U];
    [encoder setFragmentSamplerState:state.sampler atIndex:1U];
  }
  [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                      indexCount:kFullscreenIndices.size()
                       indexType:MTLIndexTypeUInt32
                     indexBuffer:state.indexBuffer
               indexBufferOffset:0U
                   instanceCount:1U];
  [encoder endEncoding];
  return true;
}

class AppleQtTextSoftGlowMetalRuntime final
    : public QtTextSoftGlowMetalRuntime {
public:
  static std::unique_ptr<AppleQtTextSoftGlowMetalRuntime>
  Create(std::string &error) {
    gpu_execution::DeviceRequest request;
    request.preferred = gpu_execution::Backend::Metal;
    auto executionDevice = gpu_execution::Device::Create(request, error);
    if (!executionDevice) {
      if (error.empty())
        error = "product Metal device is unavailable for Qt SoftGlow";
      return {};
    }
    if (executionDevice->backend() != gpu_execution::Backend::Metal ||
        executionDevice->nativeDeviceHandle() == 0U) {
      error = "product GPU device is not a usable Metal device for Qt SoftGlow";
      return {};
    }
    id<MTLDevice> device = (__bridge id<MTLDevice>)(reinterpret_cast<void *>(
        executionDevice->nativeDeviceHandle()));
    if (device == nil) {
      error = "product Metal native handle is unavailable for Qt SoftGlow";
      return {};
    }
    auto state = CreateMetalState(device, error);
    if (!state)
      return {};
    error.clear();
    return std::unique_ptr<AppleQtTextSoftGlowMetalRuntime>(
        new AppleQtTextSoftGlowMetalRuntime(std::move(executionDevice),
                                            std::move(state)));
  }

  bool Render(const QtTextSoftGlowMetalRequest &request,
              std::vector<std::uint8_t> &outputPixels,
              std::string &error,
              QtTextSoftGlowIntermediateCapture *intermediates,
              const QtTextSoftGlowMetalTextureTarget *target) override {
    @autoreleasepool {
      outputPixels.clear();
      if (!ValidateRequest(request, error))
        return false;
      id<MTLDevice> device = (__bridge id<MTLDevice>)(reinterpret_cast<void *>(
          executionDevice_->nativeDeviceHandle()));
      if (device == nil || state_ == nullptr) {
        error = "Qt SoftGlow Metal runtime lost its product device";
        return false;
      }
      if (!ValidateNativeTarget(request.source, target, device, error))
        return false;

      const int width = request.source.width;
      const int height = request.source.height;
      const auto readWriteUsage =
          MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
      id<MTLTexture> sourceTexture =
          target ? [(id<MTLTexture>)request.source.nativeTexture retain]
                 : MakePrivateRgba8Texture(device, width, height,
                                           MTLTextureUsageShaderRead);
      id<MTLTexture> thresholdTexture =
          MakePrivateRgba8Texture(device, width, height, readWriteUsage);
      id<MTLTexture> copyTexture = MakePrivateRgba8Texture(
          device, request.glowWidth, request.glowHeight, readWriteUsage);
      id<MTLTexture> horizontalTexture = MakePrivateRgba8Texture(
          device, request.glowWidth, request.glowHeight, readWriteUsage);
      id<MTLTexture> verticalTexture = MakePrivateRgba8Texture(
          device, request.glowWidth, request.glowHeight, readWriteUsage);
      id<MTLTexture> outputTexture =
          target
              ? [(id<MTLTexture>)target->texture retain]
              : MakePrivateRgba8Texture(device, width, height, readWriteUsage);
      id<MTLTexture> fullDepth = MakePrivateDepthTexture(device, width, height);
      id<MTLTexture> glowDepth = MakePrivateDepthTexture(
          device, request.glowWidth, request.glowHeight);

      const NSUInteger uploadRowBytes = AlignedRgba8RowBytes(device, width);
      const NSUInteger readbackRowBytes = AlignedRgba8RowBytes(device, width);
      const std::size_t uploadLength =
          static_cast<std::size_t>(uploadRowBytes) *
          static_cast<std::size_t>(height);
      const std::size_t readbackLength =
          static_cast<std::size_t>(readbackRowBytes) *
          static_cast<std::size_t>(height);
      id<MTLBuffer> upload = target ? nil :
          [device newBufferWithLength:uploadLength
                              options:MTLResourceStorageModeShared];
      id<MTLBuffer> readback = target && !intermediates ? nil :
          [device newBufferWithLength:readbackLength
                              options:MTLResourceStorageModeShared];
      const NSUInteger glowReadbackRowBytes =
          AlignedRgba8RowBytes(device, request.glowWidth);
      const std::size_t glowReadbackLength =
          static_cast<std::size_t>(glowReadbackRowBytes) *
          static_cast<std::size_t>(request.glowHeight);
      id<MTLBuffer> thresholdReadback = nil;
      id<MTLBuffer> copyReadback = nil;
      id<MTLBuffer> horizontalReadback = nil;
      id<MTLBuffer> verticalReadback = nil;
      if (intermediates != nullptr) {
        thresholdReadback =
            [device newBufferWithLength:readbackLength
                                options:MTLResourceStorageModeShared];
        copyReadback =
            [device newBufferWithLength:glowReadbackLength
                                options:MTLResourceStorageModeShared];
        horizontalReadback =
            [device newBufferWithLength:glowReadbackLength
                                options:MTLResourceStorageModeShared];
        verticalReadback =
            [device newBufferWithLength:glowReadbackLength
                                options:MTLResourceStorageModeShared];
      }
      const auto releaseResources = [&]() {
        [verticalReadback release];
        [horizontalReadback release];
        [copyReadback release];
        [thresholdReadback release];
        [readback release];
        [upload release];
        [glowDepth release];
        [fullDepth release];
        [outputTexture release];
        [verticalTexture release];
        [horizontalTexture release];
        [copyTexture release];
        [thresholdTexture release];
        [sourceTexture release];
      };
      if (sourceTexture == nil || thresholdTexture == nil ||
          copyTexture == nil || horizontalTexture == nil ||
          verticalTexture == nil || outputTexture == nil || fullDepth == nil ||
          glowDepth == nil || (!target && upload == nil) ||
          ((!target || intermediates) && readback == nil) ||
          (intermediates != nullptr &&
           (thresholdReadback == nil || copyReadback == nil ||
            horizontalReadback == nil || verticalReadback == nil))) {
        error = "Qt SoftGlow per-frame Metal resource allocation failed";
        releaseResources();
        return false;
      }

      auto *uploadBytes = static_cast<std::uint8_t *>(upload.contents);
      const std::size_t compactRowBytes =
          static_cast<std::size_t>(width) * 4U;
      if (!target) {
        CopyVerticallyFlippedRows(
            uploadBytes, static_cast<std::size_t>(uploadRowBytes),
            request.source.pixels, request.source.rowBytes, compactRowBytes,
            height);
      }

      SoftGlowThresholdUniforms thresholdUniforms{
          request.thresholdType, request.thresholdLow, request.thresholdHigh,
          request.thresholdSmooth, request.grayScale};
      SoftGlowXUniforms horizontalUniforms{
          request.sampleCount, request.sigmaX, request.stepX};
      SoftGlowYUniforms verticalUniforms{
          request.sampleCount, request.sigmaY, request.stepY,
          request.exposure};
      SoftGlowBlendUniforms blendUniforms{};
      blendUniforms.exposure = request.exposure;
      std::copy(request.glowColor.begin(), request.glowColor.end(),
                blendUniforms.glowColor);
      blendUniforms.displayGlow = request.displayGlow;

      id<MTLCommandQueue> queue = target
          ? (id<MTLCommandQueue>)target->commandQueue : state_->queue;
      id<MTLCommandBuffer> commandBuffer = [queue commandBuffer];
      if (commandBuffer == nil) {
        error = "Qt SoftGlow Metal command buffer creation failed";
        releaseResources();
        return false;
      }
      commandBuffer.label = @"Qt LumiSoftGlow captured five-pass v1";

      id<MTLBlitCommandEncoder> uploadBlit =
          target ? nil : [commandBuffer blitCommandEncoder];
      if (!target && uploadBlit == nil) {
        error = "Qt SoftGlow upload encoder creation failed";
        releaseResources();
        return false;
      }
      CopyBufferToTexture(uploadBlit, upload, sourceTexture,
                          MTLSizeMake(static_cast<NSUInteger>(width),
                                      static_cast<NSUInteger>(height), 1U),
                          uploadRowBytes,
                          uploadRowBytes * static_cast<NSUInteger>(height));
      [uploadBlit endEncoding];

      if (!EncodePass(commandBuffer, *state_, state_->thresholdPipeline,
                      thresholdTexture, fullDepth, width, height,
                      &thresholdUniforms, sizeof(thresholdUniforms),
                      sourceTexture, nil, @"LumiSoftGlow Threshold", error) ||
          !EncodePass(commandBuffer, *state_, state_->copyPipeline, copyTexture,
                      glowDepth, request.glowWidth, request.glowHeight, nullptr,
                      0U, thresholdTexture, nil, @"LumiSoftGlow Copy", error) ||
          !EncodePass(commandBuffer, *state_, state_->horizontalPipeline,
                      horizontalTexture, glowDepth, request.glowWidth,
                      request.glowHeight, &horizontalUniforms,
                      sizeof(horizontalUniforms), copyTexture, nil,
                      @"LumiSoftGlow X", error) ||
          !EncodePass(commandBuffer, *state_, state_->verticalPipeline,
                      verticalTexture, glowDepth, request.glowWidth,
                      request.glowHeight, &verticalUniforms,
                      sizeof(verticalUniforms), horizontalTexture, nil,
                      @"LumiSoftGlow Y", error) ||
          !EncodePass(commandBuffer, *state_, state_->blendPipeline,
                      outputTexture, fullDepth, width, height, &blendUniforms,
                      sizeof(blendUniforms), sourceTexture, verticalTexture,
                      @"LumiSoftGlow ScreenBlend", error)) {
        releaseResources();
        return false;
      }

      if (target && !intermediates) {
        if (!SubmitNativeTextureCommand(commandBuffer,
          target ? target->submit : NativeCommandSubmission{},
          {verticalReadback, horizontalReadback, copyReadback, thresholdReadback, readback, upload, glowDepth, fullDepth, outputTexture, verticalTexture, horizontalTexture, copyTexture, thresholdTexture, sourceTexture}, error)) {
        releaseResources();
        return false;
      }
        releaseResources();
        error.clear();
        return true;
      }

      id<MTLBlitCommandEncoder> readbackBlit =
          [commandBuffer blitCommandEncoder];
      if (readbackBlit == nil) {
        error = "Qt SoftGlow readback encoder creation failed";
        releaseResources();
        return false;
      }
      CopyTextureToBuffer(readbackBlit, outputTexture, readback,
                          MTLSizeMake(static_cast<NSUInteger>(width),
                                      static_cast<NSUInteger>(height), 1U),
                          readbackRowBytes,
                          readbackRowBytes * static_cast<NSUInteger>(height));
      const auto encodeIntermediateReadback =
          [&](id<MTLTexture> texture, id<MTLBuffer> buffer,
              const int captureWidth, const int captureHeight,
              const NSUInteger captureRowBytes) {
            CopyTextureToBuffer(
                readbackBlit, texture, buffer,
                MTLSizeMake(static_cast<NSUInteger>(captureWidth),
                            static_cast<NSUInteger>(captureHeight), 1U),
                captureRowBytes,
                captureRowBytes * static_cast<NSUInteger>(captureHeight));
          };
      if (intermediates != nullptr) {
        encodeIntermediateReadback(thresholdTexture, thresholdReadback, width,
                                   height, readbackRowBytes);
        encodeIntermediateReadback(copyTexture, copyReadback,
                                   request.glowWidth, request.glowHeight,
                                   glowReadbackRowBytes);
        encodeIntermediateReadback(horizontalTexture, horizontalReadback,
                                   request.glowWidth, request.glowHeight,
                                   glowReadbackRowBytes);
        encodeIntermediateReadback(verticalTexture, verticalReadback,
                                   request.glowWidth, request.glowHeight,
                                   glowReadbackRowBytes);
      }
      [readbackBlit endEncoding];

      if (!SubmitNativeTextureCommand(commandBuffer,
          target ? target->submit : NativeCommandSubmission{},
          {verticalReadback, horizontalReadback, copyReadback, thresholdReadback, readback, upload, glowDepth, fullDepth, outputTexture, verticalTexture, horizontalTexture, copyTexture, thresholdTexture, sourceTexture}, error)) {
        releaseResources();
        return false;
      }
      [commandBuffer waitUntilCompleted];
      if (commandBuffer.status != MTLCommandBufferStatusCompleted) {
        error = MetalErrorMessage(@"Qt SoftGlow Metal command failed",
                                  commandBuffer.error);
        releaseResources();
        return false;
      }

      outputPixels.resize(compactRowBytes * static_cast<std::size_t>(height));
      const auto *readbackBytes =
          static_cast<const std::uint8_t *>(readback.contents);
      CopyVerticallyFlippedRows(
          outputPixels.data(), compactRowBytes, readbackBytes,
          static_cast<std::size_t>(readbackRowBytes), compactRowBytes, height);
      if (intermediates != nullptr) {
        const auto unpackTopLeft = [](id<MTLBuffer> buffer,
                                      const NSUInteger bufferRowBytes,
                                      QtTextSoftGlowCapturedImage &capture,
                                      const int captureWidth,
                                      const int captureHeight) {
          capture.width = captureWidth;
          capture.height = captureHeight;
          const std::size_t rowBytes =
              static_cast<std::size_t>(captureWidth) * 4U;
          capture.pixels.resize(rowBytes *
                                static_cast<std::size_t>(captureHeight));
          const auto *bytes =
              static_cast<const std::uint8_t *>(buffer.contents);
          CopyVerticallyFlippedRows(capture.pixels.data(), rowBytes, bytes,
                                    static_cast<std::size_t>(bufferRowBytes),
                                    rowBytes, captureHeight);
        };
        unpackTopLeft(thresholdReadback, readbackRowBytes,
                      intermediates->threshold, width, height);
        unpackTopLeft(copyReadback, glowReadbackRowBytes, intermediates->copy,
                      request.glowWidth, request.glowHeight);
        unpackTopLeft(horizontalReadback, glowReadbackRowBytes,
                      intermediates->horizontal, request.glowWidth,
                      request.glowHeight);
        unpackTopLeft(verticalReadback, glowReadbackRowBytes,
                      intermediates->vertical, request.glowWidth,
                      request.glowHeight);
      }

      releaseResources();
      error.clear();
      return true;
    }
  }

  bool RenderDeepGlow(const QtTextDeepGlowMetalRequest &request,
                      std::vector<std::uint8_t> &outputPixels,
                      std::string &error,
                      const QtTextSoftGlowMetalTextureTarget *target) override {
    @autoreleasepool {
      outputPixels.clear();
      if (!ValidateDeepGlowRequest(request, error))
        return false;
      id<MTLDevice> device = (__bridge id<MTLDevice>)(reinterpret_cast<void *>(
          executionDevice_->nativeDeviceHandle()));
      if (device == nil || state_ == nullptr) {
        error = "Qt DeepGlow Metal runtime lost its product device";
        return false;
      }
      if (!ValidateNativeTarget(request.source, target, device, error))
        return false;

      const int width = request.source.width;
      const int height = request.source.height;
      const auto readWriteUsage =
          MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
      id<MTLTexture> sourceTexture =
          target ? [(id<MTLTexture>)request.source.nativeTexture retain]
                 : MakePrivateRgba8Texture(device, width, height,
                                           MTLTextureUsageShaderRead);
      id<MTLTexture> preprocessTexture =
          MakePrivateRgba8Texture(device, width, height, readWriteUsage);
      id<MTLTexture> blurA = MakePrivateRgba8Texture(
          device, request.blurWidth, request.blurHeight, readWriteUsage);
      id<MTLTexture> blurB = MakePrivateRgba8Texture(
          device, request.blurWidth, request.blurHeight, readWriteUsage);
      id<MTLTexture> compositeA =
          MakePrivateRgba8Texture(device, width, height, readWriteUsage);
      id<MTLTexture> compositeB =
          MakePrivateRgba8Texture(device, width, height, readWriteUsage);
      id<MTLTexture> outputTexture =
          target
              ? [(id<MTLTexture>)target->texture retain]
              : MakePrivateRgba8Texture(device, width, height, readWriteUsage);
      id<MTLTexture> fullDepth = MakePrivateDepthTexture(device, width, height);
      id<MTLTexture> blurDepth = MakePrivateDepthTexture(
          device, request.blurWidth, request.blurHeight);

      const NSUInteger uploadRowBytes = AlignedRgba8RowBytes(device, width);
      const NSUInteger readbackRowBytes = AlignedRgba8RowBytes(device, width);
      const std::size_t uploadLength =
          static_cast<std::size_t>(uploadRowBytes) *
          static_cast<std::size_t>(height);
      const std::size_t readbackLength =
          static_cast<std::size_t>(readbackRowBytes) *
          static_cast<std::size_t>(height);
      id<MTLBuffer> upload = target ? nil :
          [device newBufferWithLength:uploadLength
                              options:MTLResourceStorageModeShared];
      id<MTLBuffer> readback = target ? nil :
          [device newBufferWithLength:readbackLength
                              options:MTLResourceStorageModeShared];
      const auto releaseResources = [&]() {
        [readback release];
        [upload release];
        [blurDepth release];
        [fullDepth release];
        [outputTexture release];
        [compositeB release];
        [compositeA release];
        [blurB release];
        [blurA release];
        [preprocessTexture release];
        [sourceTexture release];
      };
      if (sourceTexture == nil || preprocessTexture == nil || blurA == nil ||
          blurB == nil || compositeA == nil || compositeB == nil ||
          outputTexture == nil || fullDepth == nil || blurDepth == nil ||
          (!target && (upload == nil || readback == nil))) {
        error = "Qt DeepGlow per-frame Metal resource allocation failed";
        releaseResources();
        return false;
      }

      auto *uploadBytes = static_cast<std::uint8_t *>(upload.contents);
      const std::size_t compactRowBytes =
          static_cast<std::size_t>(width) * 4U;
      if (!target) {
        CopyVerticallyFlippedRows(
            uploadBytes, static_cast<std::size_t>(uploadRowBytes),
            request.source.pixels, request.source.rowBytes, compactRowBytes,
            height);
      }

      id<MTLCommandQueue> queue = target
          ? (id<MTLCommandQueue>)target->commandQueue : state_->queue;
      id<MTLCommandBuffer> commandBuffer = [queue commandBuffer];
      if (commandBuffer == nil) {
        error = "Qt DeepGlow Metal command buffer creation failed";
        releaseResources();
        return false;
      }
      commandBuffer.label = @"Qt LumiDeepGlow captured bounded graph v1";
      id<MTLBlitCommandEncoder> uploadBlit =
          target ? nil : [commandBuffer blitCommandEncoder];
      if (!target && uploadBlit == nil) {
        error = "Qt DeepGlow upload encoder creation failed";
        releaseResources();
        return false;
      }
      CopyBufferToTexture(uploadBlit, upload, sourceTexture,
                          MTLSizeMake(static_cast<NSUInteger>(width),
                                      static_cast<NSUInteger>(height), 1U),
                          uploadRowBytes,
                          uploadRowBytes * static_cast<NSUInteger>(height));
      [uploadBlit endEncoding];

      DeepGlowPreprocessUniforms preprocessUniforms{
          request.glowFromAlpha,
          request.chromaticAberration,
          request.redOffset,
          request.greenOffset,
          request.blueOffset,
          request.gammaCorrect,
          request.authoredGamma};
      if (!EncodePass(commandBuffer, *state_,
                      state_->deepGlowPreprocessPipeline, preprocessTexture,
                      fullDepth, width, height, &preprocessUniforms,
                      sizeof(preprocessUniforms), sourceTexture, nil,
                      @"LumiDeepGlow Preprocess", error)) {
        releaseResources();
        return false;
      }

      id<MTLTexture> iterationInput = preprocessTexture;
      id<MTLTexture> postprocessTexture = nil;
      for (int index = 0; index < request.glowIterations; ++index) {
        if (!EncodePass(commandBuffer, *state_,
                        state_->deepGlowDownscalePipeline, blurA, blurDepth,
                        request.blurWidth, request.blurHeight, nullptr, 0U,
                        iterationInput, nil, @"LumiDeepGlow Downscale", error)) {
          releaseResources();
          return false;
        }

        DeepGlowBlurUniforms blurUniforms{};
        blurUniforms.gammaValue = request.blurGamma;
        blurUniforms.stepsInt = request.stepsInt;
        blurUniforms.aspect[0] = request.aspect[0];
        blurUniforms.aspect[1] = request.aspect[1];
        blurUniforms.rotate = request.rotateDegrees;
        blurUniforms.stride = request.strides[static_cast<std::size_t>(index)];
        blurUniforms.sigma = 4.0F;
        blurUniforms.angle = 0.0F;
        blurUniforms.steps =
            request.sampledSteps[static_cast<std::size_t>(index)] *
            request.ratio;
        if (!EncodePass(commandBuffer, *state_, state_->deepGlowBlurPipeline,
                        blurB, blurDepth, request.blurWidth,
                        request.blurHeight, &blurUniforms,
                        sizeof(blurUniforms), blurA, nil,
                        @"LumiDeepGlow Blur X", error)) {
          releaseResources();
          return false;
        }
        blurUniforms.angle = 90.0F;
        blurUniforms.steps =
            request.sampledSteps[static_cast<std::size_t>(index)] *
            (2.0F - request.ratio);
        if (!EncodePass(commandBuffer, *state_, state_->deepGlowBlurPipeline,
                        blurA, blurDepth, request.blurWidth,
                        request.blurHeight, &blurUniforms,
                        sizeof(blurUniforms), blurB, nil,
                        @"LumiDeepGlow Blur Y", error)) {
          releaseResources();
          return false;
        }

        DeepGlowCompositeUniforms compositeUniforms{
            request.compositeBlendMode,
            request.opacities[static_cast<std::size_t>(index)],
            request.blurGamma,
            1,
            request.exposure};
        id<MTLTexture> compositeTarget =
            (index % 2 == 0) ? compositeA : compositeB;
        if (!EncodePass(commandBuffer, *state_,
                        state_->deepGlowCompositePipeline, compositeTarget,
                        fullDepth, width, height, &compositeUniforms,
                        sizeof(compositeUniforms), iterationInput, blurA,
                        @"LumiDeepGlow Composite", error)) {
          releaseResources();
          return false;
        }
        if (index + 1 == request.postprocessIteration)
          postprocessTexture = compositeTarget;
        iterationInput = compositeTarget;
      }

      if (postprocessTexture == nil) {
        error = "Qt DeepGlow postprocess attachment is unavailable";
        releaseResources();
        return false;
      }
      DeepGlowPostprocessUniforms postprocessUniforms{};
      postprocessUniforms.blendMode = request.postprocessBlendMode;
      postprocessUniforms.tint = request.tint;
      postprocessUniforms.tintMode = request.tintMode;
      std::copy(request.tintColor.begin(), request.tintColor.end(),
                postprocessUniforms.tintColor);
      postprocessUniforms.tintMix = request.tintMix;
      postprocessUniforms.sourceOpacity = request.sourceOpacity;
      if (!EncodePass(commandBuffer, *state_,
                      state_->deepGlowPostprocessPipeline, outputTexture,
                      fullDepth, width, height, &postprocessUniforms,
                      sizeof(postprocessUniforms), postprocessTexture,
                      sourceTexture, @"LumiDeepGlow Postprocess", error)) {
        releaseResources();
        return false;
      }

      if (target) {
        if (!SubmitNativeTextureCommand(commandBuffer,
          target ? target->submit : NativeCommandSubmission{},
          {readback, upload, blurDepth, fullDepth, outputTexture, compositeB, compositeA, blurB, blurA, preprocessTexture, sourceTexture}, error)) {
        releaseResources();
        return false;
      }
        releaseResources();
        error.clear();
        return true;
      }

      id<MTLBlitCommandEncoder> readbackBlit =
          [commandBuffer blitCommandEncoder];
      if (readbackBlit == nil) {
        error = "Qt DeepGlow readback encoder creation failed";
        releaseResources();
        return false;
      }
      CopyTextureToBuffer(readbackBlit, outputTexture, readback,
                          MTLSizeMake(static_cast<NSUInteger>(width),
                                      static_cast<NSUInteger>(height), 1U),
                          readbackRowBytes,
                          readbackRowBytes * static_cast<NSUInteger>(height));
      [readbackBlit endEncoding];
      if (!SubmitNativeTextureCommand(commandBuffer,
          target ? target->submit : NativeCommandSubmission{},
          {readback, upload, blurDepth, fullDepth, outputTexture, compositeB, compositeA, blurB, blurA, preprocessTexture, sourceTexture}, error)) {
        releaseResources();
        return false;
      }
      [commandBuffer waitUntilCompleted];
      if (commandBuffer.status != MTLCommandBufferStatusCompleted) {
        error = MetalErrorMessage(@"Qt DeepGlow Metal command failed",
                                  commandBuffer.error);
        releaseResources();
        return false;
      }

      outputPixels.resize(compactRowBytes * static_cast<std::size_t>(height));
      const auto *readbackBytes =
          static_cast<const std::uint8_t *>(readback.contents);
      CopyVerticallyFlippedRows(
          outputPixels.data(), compactRowBytes, readbackBytes,
          static_cast<std::size_t>(readbackRowBytes), compactRowBytes, height);
      releaseResources();
      error.clear();
      return true;
    }
  }

  const char *BackendName() const noexcept override {
    return "qt-lumi-glow-metal-captured-v1";
  }

private:
  AppleQtTextSoftGlowMetalRuntime(
      std::shared_ptr<gpu_execution::Device> executionDevice,
      std::unique_ptr<SoftGlowMetalState> state)
      : executionDevice_(std::move(executionDevice)), state_(std::move(state)) {
  }

  std::shared_ptr<gpu_execution::Device> executionDevice_;
  std::unique_ptr<SoftGlowMetalState> state_;
};

} // namespace

std::unique_ptr<QtTextSoftGlowMetalRuntime>
CreateQtTextSoftGlowMetalRuntime(std::string &error) {
  @autoreleasepool {
    return AppleQtTextSoftGlowMetalRuntime::Create(error);
  }
}

} // namespace videocut::skia_runtime::internal
