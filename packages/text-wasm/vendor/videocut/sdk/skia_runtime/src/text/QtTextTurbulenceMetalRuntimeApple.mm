#include "runtime/MetalRuntimeSupport.h"
#include "text/NativeMetalSubmission.h"
#include "text/shaders/MetalTurbulence.h"
#include "text/shaders/MetalMaterialTurbulence.h"

#include "text/shaders/MetalQtTextTurbulenceVertex.h"

#include "text/shaders/MetalQtTextTurbulenceNoise.h"

#include "text/shaders/MetalQtTextTurbulenceDisplacement.h"

#include "text/QtTextTurbulenceMetalRuntime.h"

#include "videocut/gpu_execution/GpuExecution.h"

#import <Metal/Metal.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <limits>
#include <memory>
#include <mutex>
#include <utility>

namespace videocut::skia_runtime::internal {
namespace {

constexpr const char *kTurbulenceMetalSource = shader_sources::MetalTurbulence;

// These three sources are the exact runtime MSL strings observed in the Qt
// renderer.  Keep them separate: Qt creates independent libraries whose
// source SHA-256 values are part of the v1 executor contract.
constexpr const char *kCapturedTurbulenceVertexMetalSource =
    shader_sources::MetalQtTextTurbulenceVertex;
constexpr const char *kCapturedTurbulenceNoiseMetalSource =
    shader_sources::MetalQtTextTurbulenceNoise;
constexpr const char *kCapturedTurbulenceDisplacementMetalSource =
    shader_sources::MetalQtTextTurbulenceDisplacement;

struct TurbulenceVertex final {
  float positionX;
  float positionY;
  float positionZ;
  float uvX;
  float uvY;
};

struct alignas(16) TurbulenceNoiseUniforms final {
  float cycle;
  std::uint32_t padding0[3];
  float screenParams[4];
  float offset[2];
  float rotate;
  std::uint32_t padding1;
  float scale[2];
  float type;
  float complexity;
  float evolution;
  float subImpact;
  float subScale;
  float subRotate;
  float subOffset[2];
  std::uint32_t padding2[2];
};

struct alignas(16) TurbulenceDisplacementUniforms final {
  float brightness;
  float contrast;
  std::uint32_t padding0[2];
  float screenParams[4];
  float scale[2];
  float range;
  float type;
  float fixType;
  float motionTileType;
  std::uint32_t padding1[2];
};

constexpr TurbulenceVertex kFullscreenQuad[] = {
    {-1.0F, -1.0F, 0.0F, 0.0F, 0.0F},
    {1.0F, -1.0F, 0.0F, 1.0F, 0.0F},
    {1.0F, 1.0F, 0.0F, 1.0F, 1.0F},
    {-1.0F, 1.0F, 0.0F, 0.0F, 1.0F},
};
constexpr std::uint16_t kFullscreenIndices[] = {0U, 1U, 2U, 2U, 3U, 0U};

static_assert(sizeof(TurbulenceVertex) == 20U);
static_assert(sizeof(TurbulenceNoiseUniforms) == 96U);
static_assert(sizeof(TurbulenceDisplacementUniforms) == 64U);

bool ValidateImageView(const QtTextTurbulenceRawRgba8ImageView &image,
                       std::string &error) {
  if ((!image.nativeTexture && image.pixels == nullptr) ||
      image.width <= 0 || image.height <= 0) {
    error = "Qt Turbulence source image is empty";
    return false;
  }
  const auto width = static_cast<std::size_t>(image.width);
  const auto height = static_cast<std::size_t>(image.height);
  if (width > std::numeric_limits<std::size_t>::max() / 4U) {
    error = "Qt Turbulence source row size overflows";
    return false;
  }
  const std::size_t compactRowBytes = width * 4U;
  if (image.nativeTexture)
    return true;
  if (image.rowBytes < compactRowBytes ||
      height > std::numeric_limits<std::size_t>::max() / image.rowBytes) {
    error = "Qt Turbulence source rowBytes is invalid";
    return false;
  }
  const std::size_t requiredBytes =
      (height - 1U) * image.rowBytes + compactRowBytes;
  if (image.byteSize < requiredBytes) {
    error = "Qt Turbulence source byteSize is smaller than its final row";
    return false;
  }
  return true;
}

bool ValidateRequest(const QtTextTurbulenceMetalRequest &request,
                     std::string &error) {
  if (request.implementationVersion !=
      kQtTextTurbulenceMetalImplementationVersion) {
    error = "Qt Turbulence implementation version is unsupported";
    return false;
  }
  if (!ValidateImageView(request.source, error))
    return false;
  if (request.noiseWidth <= 0 || request.noiseHeight <= 0) {
    error = "Qt Turbulence noise dimensions must be positive";
    return false;
  }
  const float values[] = {
      request.cycle,      request.offsetX, request.offsetY,
      request.quantity,   request.complexity, request.evolution,
      request.type,       request.contrast, request.range,
      request.motionTileType,
  };
  if (!std::all_of(std::begin(values), std::end(values),
                   [](const float value) { return std::isfinite(value); }) ||
      request.cycle < 2.0F || request.complexity < 0.0F ||
      request.complexity > 10.0F) {
    error = "Qt Turbulence numeric contract is invalid";
    return false;
  }
  return true;
}

struct TurbulenceMetalState final {
  id<MTLCommandQueue> queue{nil};
  id<MTLRenderPipelineState> noisePipeline{nil};
  id<MTLRenderPipelineState> displacementPipeline{nil};
  id<MTLSamplerState> linearSampler{nil};
  id<MTLBuffer> vertexBuffer{nil};
  id<MTLBuffer> indexBuffer{nil};

  ~TurbulenceMetalState() {
    [indexBuffer release];
    [vertexBuffer release];
    [linearSampler release];
    [displacementPipeline release];
    [noisePipeline release];
    [queue release];
  }
};

id<MTLRenderPipelineState>
MakePipeline(id<MTLDevice> device, id<MTLFunction> vertex,
             id<MTLFunction> fragment, NSString *label, std::string &error,
             const bool proceduralVertices = false) {
  auto *descriptor = [[MTLRenderPipelineDescriptor alloc] init];
  auto *vertexDescriptor = [[MTLVertexDescriptor alloc] init];
  vertexDescriptor.attributes[0].format = MTLVertexFormatFloat3;
  vertexDescriptor.attributes[0].offset = 0U;
  vertexDescriptor.attributes[0].bufferIndex = 30U;
  vertexDescriptor.attributes[1].format = MTLVertexFormatFloat2;
  vertexDescriptor.attributes[1].offset = 3U * sizeof(float);
  vertexDescriptor.attributes[1].bufferIndex = 30U;
  vertexDescriptor.layouts[30].stride = sizeof(TurbulenceVertex);
  vertexDescriptor.layouts[30].stepFunction = MTLVertexStepFunctionPerVertex;
  vertexDescriptor.layouts[30].stepRate = 1U;
  descriptor.label = label;
  descriptor.vertexFunction = vertex;
  descriptor.fragmentFunction = fragment;
  descriptor.vertexDescriptor = proceduralVertices ? nil : vertexDescriptor;
  descriptor.rasterSampleCount = 1U;
  auto *color = descriptor.colorAttachments[0];
  color.pixelFormat = MTLPixelFormatRGBA8Unorm;
  color.blendingEnabled = NO;
  color.writeMask = MTLColorWriteMaskAll;
  NSError *pipelineError = nil;
  id<MTLRenderPipelineState> pipeline =
      [device newRenderPipelineStateWithDescriptor:descriptor
                                             error:&pipelineError];
  [vertexDescriptor release];
  [descriptor release];
  if (pipeline == nil) {
    error = MetalErrorMessage(@"Qt Turbulence Metal pipeline creation failed",
                              pipelineError);
  }
  return pipeline;
}

std::unique_ptr<TurbulenceMetalState>
CreateMetalState(id<MTLDevice> device, std::string &error) {
  auto state = std::make_unique<TurbulenceMetalState>();
  state->queue = [device newCommandQueue];
  if (state->queue == nil) {
    error = "Qt Turbulence Metal command queue creation failed";
    return {};
  }
  NSError *vertexLibraryError = nil;
  NSError *noiseLibraryError = nil;
  NSError *displacementLibraryError = nil;
  id<MTLLibrary> vertexLibrary = [device
      newLibraryWithSource:[NSString
                               stringWithUTF8String:
                                   kCapturedTurbulenceVertexMetalSource]
                   options:nil
                     error:&vertexLibraryError];
  id<MTLLibrary> noiseLibrary = [device
      newLibraryWithSource:[NSString
                               stringWithUTF8String:
                                   kCapturedTurbulenceNoiseMetalSource]
                   options:nil
                     error:&noiseLibraryError];
  id<MTLLibrary> displacementLibrary = [device
      newLibraryWithSource:[NSString
                               stringWithUTF8String:
                                   kCapturedTurbulenceDisplacementMetalSource]
                   options:nil
                     error:&displacementLibraryError];
  if (vertexLibrary == nil || noiseLibrary == nil ||
      displacementLibrary == nil) {
    NSError *libraryError = vertexLibraryError != nil   ? vertexLibraryError
                            : noiseLibraryError != nil   ? noiseLibraryError
                                                        : displacementLibraryError;
    error = MetalErrorMessage(
        @"Qt Turbulence captured Metal shader compilation failed",
        libraryError);
    [displacementLibrary release];
    [noiseLibrary release];
    [vertexLibrary release];
    return {};
  }
  id<MTLFunction> vertex = [vertexLibrary newFunctionWithName:@"main0"];
  id<MTLFunction> noise = [noiseLibrary newFunctionWithName:@"main0"];
  id<MTLFunction> displacement =
      [displacementLibrary newFunctionWithName:@"main0"];
  if (vertex == nil || noise == nil || displacement == nil) {
    error = "Qt Turbulence Metal shader entry point is unavailable";
  } else {
    state->noisePipeline = MakePipeline(
        device, vertex, noise, @"LumiTurbulence noise v1", error);
    if (state->noisePipeline != nil) {
      state->displacementPipeline =
          MakePipeline(device, vertex, displacement,
                       @"LumiTurbulence displacement v1", error);
    }
  }
  [displacement release];
  [noise release];
  [vertex release];
  [displacementLibrary release];
  [noiseLibrary release];
  [vertexLibrary release];
  if (state->noisePipeline == nil || state->displacementPipeline == nil)
    return {};

  state->linearSampler =
      MakeNormalizedClampSampler(device, MTLSamplerMinMagFilterLinear,
                                 @"Qt Turbulence normalized linear clamp");
  state->vertexBuffer =
      [device newBufferWithBytes:kFullscreenQuad
                          length:sizeof(kFullscreenQuad)
                         options:MTLResourceStorageModeShared];
  state->indexBuffer = [device newBufferWithBytes:kFullscreenIndices
                                           length:sizeof(kFullscreenIndices)
                                          options:MTLResourceStorageModeShared];
  if (state->linearSampler == nil || state->vertexBuffer == nil ||
      state->indexBuffer == nil) {
    error = "Qt Turbulence immutable Metal resource creation failed";
    return {};
  }
  error.clear();
  return state;
}

void ConfigurePass(MTLRenderPassDescriptor *pass, id<MTLTexture> texture) {
  pass.colorAttachments[0].texture = texture;
  pass.colorAttachments[0].loadAction = MTLLoadActionClear;
  pass.colorAttachments[0].storeAction = MTLStoreActionStore;
  pass.colorAttachments[0].clearColor =
      MTLClearColorMake(0.0, 0.0, 0.0, 0.0);
}

void ConfigureFullscreenEncoder(id<MTLRenderCommandEncoder> encoder,
                                TurbulenceMetalState &state, const int width,
                                const int height) {
  const MTLViewport viewport{0.0, static_cast<double>(height),
                             static_cast<double>(width),
                             -static_cast<double>(height), 0.0, 1.0};
  [encoder setViewport:viewport];
  [encoder setCullMode:MTLCullModeBack];
  [encoder setFrontFacingWinding:MTLWindingClockwise];
  const float pictureScale = 1.0F;
  [encoder setVertexBytes:&pictureScale
                   length:sizeof(pictureScale)
                  atIndex:0U];
  [encoder setVertexBuffer:state.vertexBuffer offset:0U atIndex:30U];
}

void DrawFullscreen(id<MTLRenderCommandEncoder> encoder,
                    TurbulenceMetalState &state) {
  [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                      indexCount:6U
                       indexType:MTLIndexTypeUInt16
                     indexBuffer:state.indexBuffer
               indexBufferOffset:0U
                   instanceCount:1U];
}

bool EncodeReadback(id<MTLCommandBuffer> commandBuffer,
                    id<MTLTexture> texture, id<MTLBuffer> buffer,
                    const NSUInteger rowBytes, std::string &error) {
  id<MTLBlitCommandEncoder> blit = [commandBuffer blitCommandEncoder];
  if (blit == nil) {
    error = "Qt Turbulence readback encoder creation failed";
    return false;
  }
  CopyTextureToBuffer(blit, texture, buffer,
                      MTLSizeMake(texture.width, texture.height, 1U), rowBytes,
                      rowBytes * texture.height);
  [blit endEncoding];
  return true;
}

void ReadbackTopLeft(id<MTLBuffer> buffer, const NSUInteger rowBytes,
                     const int width, const int height,
                     std::vector<std::uint8_t> &pixels) {
  const std::size_t compactRowBytes = static_cast<std::size_t>(width) * 4U;
  pixels.resize(compactRowBytes * static_cast<std::size_t>(height));
  const auto *source = static_cast<const std::uint8_t *>(buffer.contents);
  CopyVerticallyFlippedRows(pixels.data(), compactRowBytes, source,
                            static_cast<std::size_t>(rowBytes), compactRowBytes,
                            height);
}

class AppleQtTextTurbulenceMetalRuntime final
    : public QtTextTurbulenceMetalRuntime {
public:
  static std::unique_ptr<AppleQtTextTurbulenceMetalRuntime>
  Create(std::string &error) {
    gpu_execution::DeviceRequest request;
    request.preferred = gpu_execution::Backend::Metal;
    auto executionDevice = gpu_execution::Device::Create(request, error);
    if (!executionDevice) {
      if (error.empty())
        error = "product Metal device is unavailable for Qt Turbulence";
      return {};
    }
    if (executionDevice->backend() != gpu_execution::Backend::Metal ||
        executionDevice->nativeDeviceHandle() == 0U) {
      error =
          "product GPU device is not a usable Metal device for Qt Turbulence";
      return {};
    }
    id<MTLDevice> device = (__bridge id<MTLDevice>)(reinterpret_cast<void *>(
        executionDevice->nativeDeviceHandle()));
    if (device == nil) {
      error = "product Metal native handle is unavailable for Qt Turbulence";
      return {};
    }
    auto state = CreateMetalState(device, error);
    if (!state)
      return {};
    error.clear();
    return std::unique_ptr<AppleQtTextTurbulenceMetalRuntime>(
        new AppleQtTextTurbulenceMetalRuntime(std::move(executionDevice),
                                              std::move(state)));
  }

  bool Render(const QtTextTurbulenceMetalRequest &request,
              std::vector<std::uint8_t> &noisePixels,
              std::vector<std::uint8_t> &outputPixels,
              std::string &error,
              const QtTextTurbulenceTextureTarget *target) override {
    @autoreleasepool {
      noisePixels.clear();
      outputPixels.clear();
      if (!ValidateRequest(request, error))
        return false;
      id<MTLDevice> device = (__bridge id<MTLDevice>)(reinterpret_cast<void *>(
          executionDevice_->nativeDeviceHandle()));
      if (device == nil || state_ == nullptr) {
        error = "Qt Turbulence Metal runtime lost its product device";
        return false;
      }

      const int width = request.source.width;
      const int height = request.source.height;
      id<MTLCommandQueue> queue = state_->queue;
      if (request.source.nativeTexture || target) {
        if (!request.source.nativeTexture || !target || !target->texture ||
            !target->commandQueue) {
          error = "Qt Turbulence native source, target and queue are required";
          return false;
        }
        id<MTLTexture> input = (id<MTLTexture>)request.source.nativeTexture;
        id<MTLTexture> output = (id<MTLTexture>)target->texture;
        queue = (id<MTLCommandQueue>)target->commandQueue;
        if (!IsValidNativeRgba8TexturePair(input, output, queue, device, width,
                                           height)) {
          error = "Qt Turbulence native texture device, format, extent or usage is invalid";
          return false;
        }
      }
      const bool captureNoise = !target || target->captureNoisePixels;
      id<MTLTexture> sourceTexture =
          target ? [(id<MTLTexture>)request.source.nativeTexture retain]
                 : MakePrivateRgba8Texture(device, width, height,
                                           MTLTextureUsageShaderRead);
      id<MTLTexture> noiseTexture = MakePrivateRgba8Texture(
          device, request.noiseWidth, request.noiseHeight,
          MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead);
      id<MTLTexture> outputTexture =
          target ? [(id<MTLTexture>)target->texture retain]
                 : MakePrivateRgba8Texture(device, width, height,
                                           MTLTextureUsageRenderTarget);
      const NSUInteger sourceRowBytes = target ? 0U : AlignedRgba8RowBytes(device, width);
      const NSUInteger noiseRowBytes =
          captureNoise ? AlignedRgba8RowBytes(device, request.noiseWidth) : 0U;
      const std::size_t sourceLength =
          static_cast<std::size_t>(sourceRowBytes) *
          static_cast<std::size_t>(height);
      const std::size_t noiseLength =
          static_cast<std::size_t>(noiseRowBytes) *
          static_cast<std::size_t>(request.noiseHeight);
      id<MTLBuffer> upload = target ? nil :
          [device newBufferWithLength:sourceLength
                              options:MTLResourceStorageModeShared];
      id<MTLBuffer> noiseReadback = !captureNoise ? nil :
          [device newBufferWithLength:noiseLength
                              options:MTLResourceStorageModeShared];
      id<MTLBuffer> outputReadback = target ? nil :
          [device newBufferWithLength:sourceLength
                              options:MTLResourceStorageModeShared];
      const auto releaseResources = [&]() {
        [outputReadback release];
        [noiseReadback release];
        [upload release];
        [outputTexture release];
        [noiseTexture release];
        [sourceTexture release];
      };
      if (sourceTexture == nil || noiseTexture == nil || outputTexture == nil ||
          (captureNoise && noiseReadback == nil) ||
          (!target && (upload == nil || outputReadback == nil))) {
        error = "Qt Turbulence per-pass Metal resource allocation failed";
        releaseResources();
        return false;
      }

      auto *uploadBytes = static_cast<std::uint8_t *>(upload.contents);
      const std::size_t compactSourceRowBytes =
          static_cast<std::size_t>(width) * 4U;
      if (!target) {
        CopyVerticallyFlippedRows(
            uploadBytes, static_cast<std::size_t>(sourceRowBytes),
            request.source.pixels, request.source.rowBytes,
            compactSourceRowBytes, height);
      }

      TurbulenceNoiseUniforms noiseUniforms{};
      noiseUniforms.cycle = request.cycle;
      noiseUniforms.screenParams[0] = static_cast<float>(request.noiseWidth);
      noiseUniforms.screenParams[1] = static_cast<float>(request.noiseHeight);
      noiseUniforms.screenParams[2] =
          static_cast<float>(request.noiseWidth + 1) /
          static_cast<float>(request.noiseWidth);
      noiseUniforms.screenParams[3] =
          static_cast<float>(request.noiseHeight + 1) /
          static_cast<float>(request.noiseHeight);
      noiseUniforms.offset[0] = request.offsetX;
      noiseUniforms.offset[1] = request.offsetY;
      noiseUniforms.rotate = 0.0F;
      noiseUniforms.scale[0] = request.quantity;
      noiseUniforms.scale[1] = request.quantity;
      noiseUniforms.type = request.type;
      noiseUniforms.complexity = request.complexity;
      noiseUniforms.evolution = request.evolution;
      noiseUniforms.subImpact = 0.60000002384185791F;
      noiseUniforms.subScale = 56.0F;
      noiseUniforms.subRotate = 0.0F;
      noiseUniforms.subOffset[0] = 0.0F;
      noiseUniforms.subOffset[1] = 0.0F;

      TurbulenceDisplacementUniforms displacementUniforms{};
      displacementUniforms.brightness = 0.0F;
      displacementUniforms.contrast = request.contrast;
      displacementUniforms.screenParams[0] = static_cast<float>(width);
      displacementUniforms.screenParams[1] = static_cast<float>(height);
      displacementUniforms.screenParams[2] =
          static_cast<float>(width + 1) / static_cast<float>(width);
      displacementUniforms.screenParams[3] =
          static_cast<float>(height + 1) / static_cast<float>(height);
      displacementUniforms.scale[0] = request.quantity;
      displacementUniforms.scale[1] = request.quantity;
      displacementUniforms.range = request.range;
      displacementUniforms.type = request.type;
      displacementUniforms.fixType = 0.0F;
      displacementUniforms.motionTileType = request.motionTileType;

      id<MTLCommandBuffer> commandBuffer = [queue commandBuffer];
      if (commandBuffer == nil) {
        error = "Qt Turbulence Metal command buffer creation failed";
        releaseResources();
        return false;
      }
      commandBuffer.label = @"Qt LumiTurbulence native float v1";
      if (!target) {
        id<MTLBlitCommandEncoder> uploadBlit = [commandBuffer blitCommandEncoder];
        if (uploadBlit == nil) {
          error = "Qt Turbulence upload encoder creation failed";
          releaseResources();
          return false;
        }
        CopyBufferToTexture(uploadBlit, upload, sourceTexture,
                            MTLSizeMake(static_cast<NSUInteger>(width),
                                        static_cast<NSUInteger>(height), 1U),
                            sourceRowBytes,
                            sourceRowBytes * static_cast<NSUInteger>(height));
        [uploadBlit endEncoding];
      }

      MTLRenderPassDescriptor *noisePass =
          [MTLRenderPassDescriptor renderPassDescriptor];
      ConfigurePass(noisePass, noiseTexture);
      id<MTLRenderCommandEncoder> noiseEncoder =
          [commandBuffer renderCommandEncoderWithDescriptor:noisePass];
      if (noiseEncoder == nil) {
        error = "Qt Turbulence noise encoder creation failed";
        releaseResources();
        return false;
      }
      noiseEncoder.label = @"LumiTurbulence noise";
      ConfigureFullscreenEncoder(noiseEncoder, *state_, request.noiseWidth,
                                 request.noiseHeight);
      [noiseEncoder setRenderPipelineState:state_->noisePipeline];
      [noiseEncoder setFragmentBytes:&noiseUniforms
                              length:sizeof(noiseUniforms)
                             atIndex:0U];
      DrawFullscreen(noiseEncoder, *state_);
      [noiseEncoder endEncoding];

      MTLRenderPassDescriptor *displacementPass =
          [MTLRenderPassDescriptor renderPassDescriptor];
      ConfigurePass(displacementPass, outputTexture);
      id<MTLRenderCommandEncoder> displacementEncoder =
          [commandBuffer renderCommandEncoderWithDescriptor:displacementPass];
      if (displacementEncoder == nil) {
        error = "Qt Turbulence displacement encoder creation failed";
        releaseResources();
        return false;
      }
      displacementEncoder.label = @"LumiTurbulence displacement";
      ConfigureFullscreenEncoder(displacementEncoder, *state_, width, height);
      [displacementEncoder setRenderPipelineState:state_->displacementPipeline];
      [displacementEncoder setFragmentBytes:&displacementUniforms
                                     length:sizeof(displacementUniforms)
                                    atIndex:0U];
      [displacementEncoder setFragmentTexture:noiseTexture atIndex:0U];
      [displacementEncoder setFragmentTexture:sourceTexture atIndex:1U];
      [displacementEncoder setFragmentSamplerState:state_->linearSampler
                                            atIndex:0U];
      [displacementEncoder setFragmentSamplerState:state_->linearSampler
                                            atIndex:1U];
      const std::array<float, 4> textureFlipFlags{0.0F, 0.0F, 0.0F, 0.0F};
      [displacementEncoder setFragmentBytes:textureFlipFlags.data()
                                     length:sizeof(textureFlipFlags)
                                    atIndex:1U];
      DrawFullscreen(displacementEncoder, *state_);
      [displacementEncoder endEncoding];

      if ((captureNoise &&
           !EncodeReadback(commandBuffer, noiseTexture, noiseReadback,
                           noiseRowBytes, error)) ||
          (!target &&
           !EncodeReadback(commandBuffer, outputTexture, outputReadback,
                           sourceRowBytes, error))) {
        releaseResources();
        return false;
      }
      if (!SubmitNativeTextureCommand(commandBuffer,
          target ? target->submit : NativeCommandSubmission{},
          {outputReadback, noiseReadback, upload, outputTexture, noiseTexture, sourceTexture}, error)) {
        releaseResources();
        return false;
      }
      if (target && !captureNoise) {
        // Encoded commands retain their transient noise texture and execute
        // before downstream Ganesh work on the same queue.
        releaseResources();
        error.clear();
        return true;
      }
      [commandBuffer waitUntilCompleted];
      if (commandBuffer.status != MTLCommandBufferStatusCompleted) {
        error = MetalErrorMessage(@"Qt Turbulence Metal command failed",
                                  commandBuffer.error);
        releaseResources();
        return false;
      }

      ReadbackTopLeft(noiseReadback, noiseRowBytes, request.noiseWidth,
                      request.noiseHeight, noisePixels);
      if (!target)
        ReadbackTopLeft(outputReadback, sourceRowBytes, width, height,
                        outputPixels);
      releaseResources();
      error.clear();
      return true;
    }
  }

  const char *BackendName() const noexcept override {
    return "qt-lumi-turbulence-metal-float-v1";
  }

private:
  AppleQtTextTurbulenceMetalRuntime(
      std::shared_ptr<gpu_execution::Device> executionDevice,
      std::unique_ptr<TurbulenceMetalState> state)
      : executionDevice_(std::move(executionDevice)), state_(std::move(state)) {
  }

  std::shared_ptr<gpu_execution::Device> executionDevice_;
  std::unique_ptr<TurbulenceMetalState> state_;
};

} // namespace

bool RenderQtTextMaterialTurbulence(
    const QtTextMaterialTurbulenceUniforms &uniforms, void *sourceHandle,
    void *outputHandle, void *queueHandle, std::string &error,
    const NativeCommandSubmission &submit) {
  @autoreleasepool {
    id<MTLTexture> source = (__bridge id<MTLTexture>)sourceHandle;
    id<MTLTexture> output = (__bridge id<MTLTexture>)outputHandle;
    id<MTLCommandQueue> queue = (__bridge id<MTLCommandQueue>)queueHandle;
    if (!source || !output || !queue || source == output ||
        source.device != queue.device || output.device != queue.device ||
        source.pixelFormat != MTLPixelFormatRGBA8Unorm ||
        output.pixelFormat != MTLPixelFormatRGBA8Unorm ||
        source.width != output.width || source.height != output.height ||
        uniforms.textureSize[0] != static_cast<float>(output.width) ||
        uniforms.textureSize[1] != static_cast<float>(output.height)) {
      error = "text material turbulence requires matching Ganesh RGBA8 textures";
      return false;
    }
    struct PipelineCache final {
      std::mutex mutex;
      id<MTLDevice> device{nil};
      id<MTLRenderPipelineState> pipeline{nil};
      ~PipelineCache() { [pipeline release]; [device release]; }
    };
    static PipelineCache cache;
    std::lock_guard<std::mutex> lock(cache.mutex);
    if (cache.device != queue.device || cache.pipeline == nil) {
      NSError *failure = nil;
      id<MTLLibrary> library = [queue.device
          newLibraryWithSource:[NSString stringWithUTF8String:
              shader_sources::MetalMaterialTurbulence]
          options:nil error:&failure];
      if (library == nil) {
        error = MetalErrorMessage(@"material turbulence shader compilation failed", failure);
        return false;
      }
      id<MTLFunction> vertex = [library newFunctionWithName:@"materialTurbulenceVertex"];
      id<MTLFunction> fragment = [library newFunctionWithName:@"materialTurbulenceFragment"];
      id<MTLRenderPipelineState> pipeline = nil;
      if (vertex && fragment)
        pipeline = MakePipeline(queue.device, vertex, fragment,
            @"text material turbulence v1", error, true);
      else
        error = "material turbulence shader entry point missing";
      [fragment release]; [vertex release]; [library release];
      if (pipeline == nil)
        return false;
      [cache.pipeline release]; [cache.device release];
      cache.pipeline = pipeline;
      cache.device = [queue.device retain];
    }
    id<MTLCommandBuffer> command = [queue commandBuffer];
    auto *pass = [MTLRenderPassDescriptor renderPassDescriptor];
    pass.colorAttachments[0].texture = output;
    pass.colorAttachments[0].loadAction = MTLLoadActionDontCare;
    pass.colorAttachments[0].storeAction = MTLStoreActionStore;
    id<MTLRenderCommandEncoder> encoder = [command renderCommandEncoderWithDescriptor:pass];
    if (!command || !encoder) {
      error = "material turbulence command encoding failed";
      return false;
    }
    [encoder setRenderPipelineState:cache.pipeline];
    [encoder setViewport:MTLViewport{0.0, static_cast<double>(output.height),
        static_cast<double>(output.width), -static_cast<double>(output.height), 0.0, 1.0}];
    [encoder setFragmentBytes:&uniforms length:sizeof(uniforms) atIndex:0];
    [encoder setFragmentTexture:source atIndex:0];
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle vertexStart:0 vertexCount:6];
    [encoder endEncoding];
    // Explicit, bounded diagnostics only. Normal playback never reads back or
    // waits for this GPU pass. Keep native bottom-left RGBA rows unchanged.
    const char *dumpDirectory = std::getenv("VIDEOCUT_DUMP_MATERIAL_TURBULENCE_DIR");
    const char *dumpEvolution = std::getenv("VIDEOCUT_DUMP_MATERIAL_TURBULENCE_EVOLUTION");
    static unsigned dumpCount = 0;
    id<MTLBuffer> readback = nil;
    const NSUInteger rowBytes = output.width * 4;
    const NSUInteger imageBytes = rowBytes * output.height;
    if (dumpDirectory && dumpDirectory[0] && dumpEvolution && dumpCount < 2 &&
        uniforms.evolution == std::strtof(dumpEvolution, nullptr)) {
      readback = [queue.device newBufferWithLength:imageBytes * 2
                                          options:MTLResourceStorageModeShared];
      id<MTLBlitCommandEncoder> blit = readback ? [command blitCommandEncoder] : nil;
      if (blit) {
        for (NSUInteger index = 0; index < 2; ++index) {
          CopyTextureToBuffer(blit, index == 0 ? source : output, readback,
                              MTLSizeMake(output.width, output.height, 1),
                              rowBytes, imageBytes, imageBytes * index);
        }
        [blit endEncoding];
      } else {
        [readback release];
        readback = nil;
      }
    }
    if (!SubmitNativeTextureCommand(command, submit, {source, output, readback}, error)) {
      [readback release];
      return false;
    }
    if (readback) {
      [command waitUntilCompleted];
      if (command.status == MTLCommandBufferStatusCompleted) {
        for (unsigned index = 0; index < 2; ++index) {
          char path[4096];
          const int length = std::snprintf(path, sizeof(path),
              "%s/turbulence-%u-%s-%lux%lu-rgba8.bin", dumpDirectory, dumpCount,
              index == 0 ? "input" : "output", (unsigned long)output.width,
              (unsigned long)output.height);
          if (length > 0 && static_cast<std::size_t>(length) < sizeof(path)) {
            if (FILE *file = std::fopen(path, "wb")) {
              std::fwrite(static_cast<const char *>(readback.contents) + imageBytes * index,
                          1, imageBytes, file);
              std::fclose(file);
            }
          }
        }
        ++dumpCount;
      }
      [readback release];
    }
    error.clear();
    return true;
  }
}

std::unique_ptr<QtTextTurbulenceMetalRuntime>
CreateQtTextTurbulenceMetalRuntime(std::string &error) {
  @autoreleasepool {
    return AppleQtTextTurbulenceMetalRuntime::Create(error);
  }
}

} // namespace videocut::skia_runtime::internal
