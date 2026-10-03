#include "runtime/MetalRuntimeSupport.h"
#include "text/NativeMetalSubmission.h"
#include "text/shaders/MetalDistortChroma.h"

#include "text/QtTextDistortChromaMetalRuntime.h"

#include "videocut/gpu_execution/GpuExecution.h"

#import <Metal/Metal.h>
#import <simd/simd.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <memory>
#include <utility>

namespace videocut::skia_runtime::internal {
namespace {

// Direct transcription of the captured LumiDistortChroma vertex, lens,
// Gaussian scalar blur, and final chromatic-distortion shaders. The six
// render passes below are intentionally separate RGBA8Unorm targets.
constexpr const char *kDistortChromaMetalSource =
    shader_sources::MetalDistortChroma;

struct DistortBlurUniforms final {
  float stride;
  float angle;
  float padding0[2];
  simd_float4 screenParams;
  std::int32_t steps;
  float padding1[3];
};

struct DistortFinalUniforms final {
  float rotateWarpDirection;
  float amountRelX;
  float amountRelY;
  std::int32_t wrapModeX;
  std::int32_t wrapModeY;
  std::int32_t steps;
  float warpRed;
  float warpBlue;
  simd_float4 screenParams;
  float amount;
  float padding0[3];
  simd_float3 color1;
  simd_float3 color2;
  simd_float3 color3;
  float colorMix;
  float padding1[3];
};

static_assert(offsetof(DistortBlurUniforms, screenParams) == 16U);
static_assert(offsetof(DistortBlurUniforms, steps) == 32U);
static_assert(sizeof(DistortBlurUniforms) == 48U);
static_assert(offsetof(DistortFinalUniforms, screenParams) == 32U);
static_assert(offsetof(DistortFinalUniforms, amount) == 48U);
static_assert(offsetof(DistortFinalUniforms, color1) == 64U);
static_assert(offsetof(DistortFinalUniforms, color2) == 80U);
static_assert(offsetof(DistortFinalUniforms, color3) == 96U);
static_assert(offsetof(DistortFinalUniforms, colorMix) == 112U);
static_assert(sizeof(DistortFinalUniforms) == 128U);

bool ValidateRequest(const QtTextDistortChromaRenderRequest &request,
                     std::string &error) {
  const auto &source = request.source;
  if ((source.pixels == nullptr && source.nativeTexture == nullptr) || source.width <= 0 || source.height <= 0) {
    error = "Qt LumiDistortChroma source image is empty";
    return false;
  }
  const std::size_t compactRowBytes =
      static_cast<std::size_t>(source.width) * 4U;
  if (!source.nativeTexture && (source.rowBytes < compactRowBytes ||
      source.byteSize <
          (static_cast<std::size_t>(source.height) - 1U) * source.rowBytes +
              compactRowBytes)) {
    error = "Qt LumiDistortChroma source image ABI is invalid";
    return false;
  }
  if (request.lensWidth <= 0 || request.lensHeight <= 0 ||
      request.blurSteps <= 0 || request.chromaSteps <= 0) {
    error = "Qt LumiDistortChroma pass dimensions or sample counts are empty";
    return false;
  }
  const std::size_t fullPixels = static_cast<std::size_t>(source.width) *
                                 static_cast<std::size_t>(source.height);
  const std::size_t lensPixels = static_cast<std::size_t>(request.lensWidth) *
                                 static_cast<std::size_t>(request.lensHeight);
  if (fullPixels > 16U * 1024U * 1024U || lensPixels > 16U * 1024U * 1024U) {
    error = "Qt LumiDistortChroma render target exceeds the allocation cap";
    return false;
  }
  const std::array<float, 21> values{
      request.blurStrideFirst,
      request.blurStrideSecond,
      request.blurAngleDegrees,
      request.blurPerpendicularAngleDegrees,
      request.rotateWarpDirectionDegrees,
      request.amountRelX,
      request.amountRelY,
      request.warpRed,
      request.warpBlue,
      request.warpAmount,
      request.color1[0],
      request.color1[1],
      request.color1[2],
      request.color2[0],
      request.color2[1],
      request.color2[2],
      request.color3[0],
      request.color3[1],
      request.color3[2],
      request.colorMix,
      static_cast<float>(source.width) / static_cast<float>(source.height),
  };
  if (!std::all_of(values.begin(), values.end(),
                   [](const float value) { return std::isfinite(value); })) {
    error = "Qt LumiDistortChroma uniforms are non-finite";
    return false;
  }
  return true;
}

id<MTLRenderPipelineState> MakePipeline(id<MTLDevice> device,
                                        id<MTLFunction> vertex,
                                        id<MTLFunction> fragment,
                                        NSString *label, std::string &error) {
  auto *descriptor = [[MTLRenderPipelineDescriptor alloc] init];
  descriptor.label = label;
  descriptor.vertexFunction = vertex;
  descriptor.fragmentFunction = fragment;
  descriptor.rasterSampleCount = 1U;
  auto *color = descriptor.colorAttachments[0];
  color.pixelFormat = MTLPixelFormatRGBA8Unorm;
  color.blendingEnabled = NO;
  color.writeMask = MTLColorWriteMaskAll;
  NSError *pipelineError = nil;
  id<MTLRenderPipelineState> result =
      [device newRenderPipelineStateWithDescriptor:descriptor
                                             error:&pipelineError];
  [descriptor release];
  if (result == nil)
    error = MetalErrorMessage(label, pipelineError);
  return result;
}

struct DistortChromaMetalState final {
  id<MTLCommandQueue> queue{nil};
  id<MTLRenderPipelineState> lensPipeline{nil};
  id<MTLRenderPipelineState> blurPipeline{nil};
  id<MTLRenderPipelineState> finalPipeline{nil};
  id<MTLSamplerState> sampler{nil};
  id<MTLBuffer> vertexBuffer{nil};
  id<MTLBuffer> indexBuffer{nil};

  ~DistortChromaMetalState() {
    [indexBuffer release];
    [vertexBuffer release];
    [sampler release];
    [finalPipeline release];
    [blurPipeline release];
    [lensPipeline release];
    [queue release];
  }
};

std::unique_ptr<DistortChromaMetalState> CreateMetalState(id<MTLDevice> device,
                                                          std::string &error) {
  auto state = std::make_unique<DistortChromaMetalState>();
  state->queue = [device newCommandQueue];
  if (state->queue == nil) {
    error = "Qt LumiDistortChroma Metal command queue creation failed";
    return {};
  }

  NSError *libraryError = nil;
  id<MTLLibrary> library = [device
      newLibraryWithSource:[NSString
                               stringWithUTF8String:kDistortChromaMetalSource]
                   options:nil
                     error:&libraryError];
  if (library == nil) {
    error = MetalErrorMessage(
        @"Qt LumiDistortChroma Metal shader compilation failed", libraryError);
    return {};
  }
  id<MTLFunction> vertex =
      [library newFunctionWithName:@"qtTextDistortChromaVertex"];
  id<MTLFunction> lens =
      [library newFunctionWithName:@"qtTextDistortChromaLensFragment"];
  id<MTLFunction> blur =
      [library newFunctionWithName:@"qtTextDistortChromaBlurFragment"];
  id<MTLFunction> final =
      [library newFunctionWithName:@"qtTextDistortChromaFinalFragment"];
  if (vertex == nil || lens == nil || blur == nil || final == nil) {
    error = "Qt LumiDistortChroma Metal shader entry point is unavailable";
  } else {
    state->lensPipeline = MakePipeline(
        device, vertex, lens, @"Qt LumiDistortChroma lens pipeline", error);
    if (state->lensPipeline != nil) {
      state->blurPipeline = MakePipeline(
          device, vertex, blur, @"Qt LumiDistortChroma blur pipeline", error);
    }
    if (state->blurPipeline != nil) {
      state->finalPipeline = MakePipeline(
          device, vertex, final, @"Qt LumiDistortChroma final pipeline", error);
    }
  }
  [final release];
  [blur release];
  [lens release];
  [vertex release];
  [library release];
  if (state->lensPipeline == nil || state->blurPipeline == nil ||
      state->finalPipeline == nil) {
    return {};
  }

  state->sampler = MakeNormalizedClampSampler(
      device, MTLSamplerMinMagFilterLinear,
      @"Qt LumiDistortChroma normalized linear clamp");
  state->vertexBuffer =
      [device newBufferWithBytes:kTexturedQuadVertices
                          length:sizeof(kTexturedQuadVertices)
                         options:MTLResourceStorageModeShared];
  state->indexBuffer = [device newBufferWithBytes:kTexturedQuadIndices
                                           length:sizeof(kTexturedQuadIndices)
                                          options:MTLResourceStorageModeShared];
  if (state->sampler == nil || state->vertexBuffer == nil ||
      state->indexBuffer == nil) {
    error = "Qt LumiDistortChroma immutable Metal resource creation failed";
    return {};
  }
  error.clear();
  return state;
}

void CopyTopLeft(id<MTLBuffer> buffer, const NSUInteger transferRowBytes,
                 const int width, const int height,
                 std::vector<std::uint8_t> &output) {
  const std::size_t compactRowBytes = static_cast<std::size_t>(width) * 4U;
  output.resize(compactRowBytes * static_cast<std::size_t>(height));
  const auto *bytes = static_cast<const std::uint8_t *>(buffer.contents);
  CopyVerticallyFlippedRows(output.data(), compactRowBytes, bytes,
                            static_cast<std::size_t>(transferRowBytes),
                            compactRowBytes, height);
}

class AppleQtTextDistortChromaMetalRuntime final
    : public QtTextDistortChromaMetalRuntime {
public:
  static std::unique_ptr<AppleQtTextDistortChromaMetalRuntime>
  Create(std::string &error) {
    gpu_execution::DeviceRequest request;
    request.preferred = gpu_execution::Backend::Metal;
    auto executionDevice = gpu_execution::Device::Create(request, error);
    if (!executionDevice ||
        executionDevice->backend() != gpu_execution::Backend::Metal ||
        executionDevice->nativeDeviceHandle() == 0U) {
      if (error.empty()) {
        error = "product Metal device is unavailable for Qt LumiDistortChroma";
      }
      return {};
    }
    id<MTLDevice> device = (__bridge id<MTLDevice>)(reinterpret_cast<void *>(
        executionDevice->nativeDeviceHandle()));
    if (device == nil) {
      error =
          "product Metal native handle is unavailable for Qt LumiDistortChroma";
      return {};
    }
    auto state = CreateMetalState(device, error);
    if (!state)
      return {};
    return std::unique_ptr<AppleQtTextDistortChromaMetalRuntime>(
        new AppleQtTextDistortChromaMetalRuntime(std::move(executionDevice),
                                                 std::move(state)));
  }

  bool Render(const QtTextDistortChromaRenderRequest &request,
              QtTextDistortChromaRenderResult &result,
              std::string &error, const NativeRgba8TextureTarget *target) override {
    @autoreleasepool {
      result = {};
      if (!ValidateRequest(request, error))
        return false;
      id<MTLDevice> device = (__bridge id<MTLDevice>)(reinterpret_cast<void *>(
          executionDevice_->nativeDeviceHandle()));
      if (device == nil || state_ == nullptr) {
        error = "Qt LumiDistortChroma Metal runtime lost its product device";
        return false;
      }

      if (!ValidateNativeRgba8Target(request.source.nativeTexture,
          request.source.width, request.source.height, target, device, error))
        return false;
      const int width = request.source.width;
      const int height = request.source.height;
      const int lensWidth = request.lensWidth;
      const int lensHeight = request.lensHeight;
      const MTLTextureUsage intermediateUsage =
          MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
      id<MTLTexture> sourceTexture =
          target ? [(id<MTLTexture>)request.source.nativeTexture retain]
                 : MakePrivateRgba8Texture(device, width, height,
                                           MTLTextureUsageShaderRead);
      id<MTLTexture> temporary1 = MakePrivateRgba8Texture(
          device, lensWidth, lensHeight, intermediateUsage);
      id<MTLTexture> temporary2 = MakePrivateRgba8Texture(
          device, lensWidth, lensHeight, intermediateUsage);
      id<MTLTexture> outputTexture =
          target ? [(id<MTLTexture>)target->texture retain]
                 : MakePrivateRgba8Texture(device, width, height,
                                           intermediateUsage);

      const NSUInteger fullRowBytes = AlignedRgba8RowBytes(device, width);
      const NSUInteger lensRowBytes = AlignedRgba8RowBytes(device, lensWidth);
      const std::size_t fullTransferLength =
          static_cast<std::size_t>(fullRowBytes) *
          static_cast<std::size_t>(height);
      const std::size_t lensTransferLength =
          static_cast<std::size_t>(lensRowBytes) *
          static_cast<std::size_t>(lensHeight);
      id<MTLBuffer> upload = target ? nil :
          [device newBufferWithLength:fullTransferLength
                              options:MTLResourceStorageModeShared];
      id<MTLBuffer> outputReadback = target ? nil :
          [device newBufferWithLength:fullTransferLength
                              options:MTLResourceStorageModeShared];
      std::array<id<MTLBuffer>, 5> intermediateReadbacks{nil, nil, nil, nil,
                                                         nil};
      if (request.captureIntermediates) {
        for (auto &buffer : intermediateReadbacks) {
          buffer = [device newBufferWithLength:lensTransferLength
                                       options:MTLResourceStorageModeShared];
        }
      }
      const auto releaseResources = [&]() {
        for (auto &buffer : intermediateReadbacks)
          [buffer release];
        [outputReadback release];
        [upload release];
        [outputTexture release];
        [temporary2 release];
        [temporary1 release];
        [sourceTexture release];
      };
      const bool missingIntermediate =
          request.captureIntermediates &&
          std::any_of(intermediateReadbacks.begin(),
                      intermediateReadbacks.end(),
                      [](id<MTLBuffer> buffer) { return buffer == nil; });
      if (sourceTexture == nil || temporary1 == nil || temporary2 == nil ||
          outputTexture == nil || (!target && (upload == nil || outputReadback == nil)) ||
          missingIntermediate) {
        error =
            "Qt LumiDistortChroma per-pass Metal resource allocation failed";
        releaseResources();
        return false;
      }

      if (upload) std::memset(upload.contents, 0, fullTransferLength);
      const std::size_t compactRowBytes = static_cast<std::size_t>(width) * 4U;
      auto *uploadBytes = static_cast<std::uint8_t *>(upload.contents);
      if (!target) {
        CopyVerticallyFlippedRows(
            uploadBytes, static_cast<std::size_t>(fullRowBytes),
            request.source.pixels, request.source.rowBytes, compactRowBytes,
            height);
      }

      const auto screenParams = [](const int passWidth,
                                   const int passHeight) -> simd_float4 {
        return simd_make_float4(static_cast<float>(passWidth),
                                static_cast<float>(passHeight),
                                1.0F + 1.0F / static_cast<float>(passWidth),
                                1.0F + 1.0F / static_cast<float>(passHeight));
      };
      const DistortBlurUniforms blurX1{
          request.blurStrideFirst,
          request.blurAngleDegrees,
          {0.0F, 0.0F},
          screenParams(lensWidth, lensHeight),
          request.blurSteps,
          {0.0F, 0.0F, 0.0F},
      };
      DistortBlurUniforms blurY1 = blurX1;
      blurY1.angle = request.blurPerpendicularAngleDegrees;
      DistortBlurUniforms blurX2 = blurX1;
      blurX2.stride = request.blurStrideSecond;
      DistortBlurUniforms blurY2 = blurX2;
      blurY2.angle = request.blurPerpendicularAngleDegrees;
      const DistortFinalUniforms finalUniforms{
          request.rotateWarpDirectionDegrees,
          request.amountRelX,
          request.amountRelY,
          request.wrapModeX,
          request.wrapModeY,
          request.chromaSteps,
          request.warpRed,
          request.warpBlue,
          screenParams(width, height),
          request.warpAmount,
          {0.0F, 0.0F, 0.0F},
          simd_make_float3(request.color1[0], request.color1[1],
                           request.color1[2]),
          simd_make_float3(request.color2[0], request.color2[1],
                           request.color2[2]),
          simd_make_float3(request.color3[0], request.color3[1],
                           request.color3[2]),
          request.colorMix,
          {0.0F, 0.0F, 0.0F},
      };

      id<MTLCommandQueue> queue = target ? (id<MTLCommandQueue>)target->commandQueue : state_->queue;
      id<MTLCommandBuffer> commandBuffer = [queue commandBuffer];
      if (commandBuffer == nil) {
        error = "Qt LumiDistortChroma Metal command buffer creation failed";
        releaseResources();
        return false;
      }
      commandBuffer.label = @"Qt LumiDistortChroma six-pass source graph";
      if (!target) {
      id<MTLBlitCommandEncoder> uploadBlit = [commandBuffer blitCommandEncoder];
      if (uploadBlit == nil) {
        error = "Qt LumiDistortChroma upload encoder creation failed";
        releaseResources();
        return false;
      }
      CopyBufferToTexture(uploadBlit, upload, sourceTexture,
                          MTLSizeMake(static_cast<NSUInteger>(width),
                                      static_cast<NSUInteger>(height), 1U),
                          fullRowBytes,
                          fullRowBytes * static_cast<NSUInteger>(height));
      [uploadBlit endEncoding];
      }

      const auto encodePass =
          [&](id<MTLRenderPipelineState> pipeline, id<MTLTexture> target,
              const int passWidth, const int passHeight, const void *uniforms,
              const std::size_t uniformSize, id<MTLTexture> texture0,
              id<MTLTexture> texture1, NSString *label) -> bool {
        MTLRenderPassDescriptor *pass =
            [MTLRenderPassDescriptor renderPassDescriptor];
        pass.colorAttachments[0].texture = target;
        pass.colorAttachments[0].loadAction = MTLLoadActionClear;
        pass.colorAttachments[0].storeAction = MTLStoreActionStore;
        pass.colorAttachments[0].clearColor =
            MTLClearColorMake(0.0, 0.0, 0.0, 1.0);
        id<MTLRenderCommandEncoder> encoder =
            [commandBuffer renderCommandEncoderWithDescriptor:pass];
        if (encoder == nil) {
          error = "Qt LumiDistortChroma render encoder creation failed";
          return false;
        }
        encoder.label = label;
        const MTLViewport viewport{0.0,
                                   static_cast<double>(passHeight),
                                   static_cast<double>(passWidth),
                                   -static_cast<double>(passHeight),
                                   0.0,
                                   1.0};
        [encoder setViewport:viewport];
        [encoder setScissorRect:MTLScissorRect{
                                    0U, 0U, static_cast<NSUInteger>(passWidth),
                                    static_cast<NSUInteger>(passHeight)}];
        [encoder setCullMode:MTLCullModeNone];
        [encoder setRenderPipelineState:pipeline];
        [encoder setVertexBuffer:state_->vertexBuffer offset:0U atIndex:0U];
        if (uniforms != nullptr && uniformSize > 0U)
          [encoder setFragmentBytes:uniforms length:uniformSize atIndex:0U];
        [encoder setFragmentTexture:texture0 atIndex:0U];
        [encoder setFragmentSamplerState:state_->sampler atIndex:0U];
        if (texture1 != nil) {
          [encoder setFragmentTexture:texture1 atIndex:1U];
          [encoder setFragmentSamplerState:state_->sampler atIndex:1U];
        }
        [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                            indexCount:6U
                             indexType:MTLIndexTypeUInt32
                           indexBuffer:state_->indexBuffer
                     indexBufferOffset:0U
                         instanceCount:1U];
        [encoder endEncoding];
        return true;
      };

      const auto encodeReadback =
          [&](id<MTLTexture> texture, id<MTLBuffer> buffer, const int passWidth,
              const int passHeight, const NSUInteger rowBytes) -> bool {
        if (buffer == nil)
          return true;
        id<MTLBlitCommandEncoder> blit = [commandBuffer blitCommandEncoder];
        if (blit == nil) {
          error = "Qt LumiDistortChroma readback encoder creation failed";
          return false;
        }
        CopyTextureToBuffer(
            blit, texture, buffer,
            MTLSizeMake(static_cast<NSUInteger>(passWidth),
                        static_cast<NSUInteger>(passHeight), 1U),
            rowBytes, rowBytes * static_cast<NSUInteger>(passHeight));
        [blit endEncoding];
        return true;
      };

      if (!encodePass(state_->lensPipeline, temporary1, lensWidth, lensHeight,
                      nullptr, 0U, sourceTexture, nil,
                      @"Qt LumiDistortChroma 1/6 lens") ||
          !encodeReadback(temporary1, intermediateReadbacks[0], lensWidth,
                          lensHeight, lensRowBytes) ||
          !encodePass(state_->blurPipeline, temporary2, lensWidth, lensHeight,
                      &blurX1, sizeof(blurX1), temporary1, nil,
                      @"Qt LumiDistortChroma 2/6 blur X1") ||
          !encodeReadback(temporary2, intermediateReadbacks[1], lensWidth,
                          lensHeight, lensRowBytes) ||
          !encodePass(state_->blurPipeline, temporary1, lensWidth, lensHeight,
                      &blurY1, sizeof(blurY1), temporary2, nil,
                      @"Qt LumiDistortChroma 3/6 blur Y1") ||
          !encodeReadback(temporary1, intermediateReadbacks[2], lensWidth,
                          lensHeight, lensRowBytes) ||
          !encodePass(state_->blurPipeline, temporary2, lensWidth, lensHeight,
                      &blurX2, sizeof(blurX2), temporary1, nil,
                      @"Qt LumiDistortChroma 4/6 blur X2") ||
          !encodeReadback(temporary2, intermediateReadbacks[3], lensWidth,
                          lensHeight, lensRowBytes) ||
          !encodePass(state_->blurPipeline, temporary1, lensWidth, lensHeight,
                      &blurY2, sizeof(blurY2), temporary2, nil,
                      @"Qt LumiDistortChroma 5/6 blur Y2") ||
          !encodeReadback(temporary1, intermediateReadbacks[4], lensWidth,
                          lensHeight, lensRowBytes) ||
          !encodePass(state_->finalPipeline, outputTexture, width, height,
                      &finalUniforms, sizeof(finalUniforms), temporary1,
                      sourceTexture, @"Qt LumiDistortChroma 6/6 distort") ||
          !encodeReadback(outputTexture, outputReadback, width, height,
                          fullRowBytes)) {
        releaseResources();
        return false;
      }

      if (!SubmitNativeTextureCommand(commandBuffer,
          target ? target->submit : NativeCommandSubmission{},
          {sourceTexture, temporary1, temporary2, outputTexture, upload, outputReadback,
           intermediateReadbacks[0], intermediateReadbacks[1], intermediateReadbacks[2],
           intermediateReadbacks[3], intermediateReadbacks[4]}, error)) {
        releaseResources();
        return false;
      }
      if (target && !request.captureIntermediates) {
        releaseResources();
        error.clear();
        return true;
      }
      [commandBuffer waitUntilCompleted];
      if (commandBuffer.status != MTLCommandBufferStatusCompleted) {
        error = MetalErrorMessage(@"Qt LumiDistortChroma Metal command failed",
                                  commandBuffer.error);
        releaseResources();
        return false;
      }

      if (request.captureIntermediates) {
        CopyTopLeft(intermediateReadbacks[0], lensRowBytes, lensWidth,
                    lensHeight, result.lensPixels);
        CopyTopLeft(intermediateReadbacks[1], lensRowBytes, lensWidth,
                    lensHeight, result.blurX1Pixels);
        CopyTopLeft(intermediateReadbacks[2], lensRowBytes, lensWidth,
                    lensHeight, result.blurY1Pixels);
        CopyTopLeft(intermediateReadbacks[3], lensRowBytes, lensWidth,
                    lensHeight, result.blurX2Pixels);
        CopyTopLeft(intermediateReadbacks[4], lensRowBytes, lensWidth,
                    lensHeight, result.blurY2Pixels);
      }
      if (!target) CopyTopLeft(outputReadback, fullRowBytes, width, height,
                  result.outputPixels);
      releaseResources();
      error.clear();
      return true;
    }
  }

  const char *BackendName() const noexcept override {
    return "qt-lumi-distort-chroma-metal";
  }

private:
  AppleQtTextDistortChromaMetalRuntime(
      std::shared_ptr<gpu_execution::Device> executionDevice,
      std::unique_ptr<DistortChromaMetalState> state)
      : executionDevice_(std::move(executionDevice)), state_(std::move(state)) {
  }

  std::shared_ptr<gpu_execution::Device> executionDevice_;
  std::unique_ptr<DistortChromaMetalState> state_;
};

} // namespace

std::unique_ptr<QtTextDistortChromaMetalRuntime>
CreateQtTextDistortChromaMetalRuntime(std::string &error) {
  @autoreleasepool {
    return AppleQtTextDistortChromaMetalRuntime::Create(error);
  }
}

} // namespace videocut::skia_runtime::internal
