#include "runtime/MetalRuntimeSupport.h"
#include "text/NativeMetalSubmission.h"
#include "text/shaders/MetalEngineCopy.h"

#include "text/QtTextEngineCopyMetalRuntime.h"

#include "videocut/gpu_execution/GpuExecution.h"

#import <Metal/Metal.h>

#include <algorithm>
#include <limits>
#include <memory>
#include <utility>

namespace videocut::skia_runtime::internal {
namespace {

constexpr const char *kEngineCopyMetalSource = shader_sources::MetalEngineCopy;


bool ValidateImageView(const QtTextEngineCopyRawRgba8ImageView &image,
                       std::string &error) {
  if (image.pixels == nullptr || image.width <= 0 || image.height <= 0) {
    error = "Qt EngineCopy source image is empty";
    return false;
  }
  const auto width = static_cast<std::size_t>(image.width);
  const auto height = static_cast<std::size_t>(image.height);
  if (width > std::numeric_limits<std::size_t>::max() / 4U) {
    error = "Qt EngineCopy source row size overflows";
    return false;
  }
  const std::size_t compactRowBytes = width * 4U;
  if (image.rowBytes < compactRowBytes ||
      height > std::numeric_limits<std::size_t>::max() / image.rowBytes) {
    error = "Qt EngineCopy source rowBytes is invalid";
    return false;
  }
  const std::size_t requiredBytes =
      (height - 1U) * image.rowBytes + compactRowBytes;
  if (image.byteSize < requiredBytes) {
    error = "Qt EngineCopy source byteSize is smaller than its final row";
    return false;
  }
  return true;
}

bool ValidateRequest(const QtTextEngineCopyRequest &request,
                     std::string &error) {
  if (request.implementationVersion != kQtTextEngineCopyImplementationVersion) {
    error = "Qt EngineCopy implementation version is unsupported";
    return false;
  }
  if (!ValidateImageView(request.source, error))
    return false;
  if (request.outputWidth <= 0 || request.outputHeight <= 0) {
    error = "Qt EngineCopy output dimensions must be positive";
    return false;
  }
  const auto outputWidth = static_cast<std::size_t>(request.outputWidth);
  const auto outputHeight = static_cast<std::size_t>(request.outputHeight);
  if (outputWidth > std::numeric_limits<std::size_t>::max() / 4U ||
      outputHeight >
          std::numeric_limits<std::size_t>::max() / (outputWidth * 4U)) {
    error = "Qt EngineCopy output byte size overflows";
    return false;
  }
  return true;
}

bool EncodeEngineCopy(const MetalTexturedPipelineState &state,
                      id<MTLCommandBuffer> commandBuffer,
                      id<MTLTexture> sourceTexture,
                      id<MTLTexture> outputTexture, std::string &error) {
  MTLRenderPassDescriptor *pass = [MTLRenderPassDescriptor renderPassDescriptor];
  pass.colorAttachments[0].texture = outputTexture;
  pass.colorAttachments[0].loadAction = MTLLoadActionClear;
  pass.colorAttachments[0].storeAction = MTLStoreActionStore;
  pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 0.0);
  id<MTLRenderCommandEncoder> encoder =
      [commandBuffer renderCommandEncoderWithDescriptor:pass];
  if (encoder == nil) {
    error = "Qt EngineCopy render encoder creation failed";
    return false;
  }
  encoder.label = @"AmazingEngine EngineCopy";
  const MTLViewport viewport{0.0, static_cast<double>(outputTexture.height),
                             static_cast<double>(outputTexture.width),
                             -static_cast<double>(outputTexture.height), 0.0, 1.0};
  [encoder setViewport:viewport];
  [encoder setCullMode:MTLCullModeNone];
  [encoder setFrontFacingWinding:MTLWindingClockwise];
  [encoder setRenderPipelineState:state.pipeline];
  [encoder setVertexBuffer:state.vertexBuffer offset:0U atIndex:0U];
  [encoder setFragmentTexture:sourceTexture atIndex:0U];
  [encoder setFragmentSamplerState:state.sampler atIndex:0U];
  [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                      indexCount:3U indexType:MTLIndexTypeUInt16
                     indexBuffer:state.indexBuffer indexBufferOffset:0U
                   instanceCount:1U];
  [encoder endEncoding];
  return true;
}

class AppleQtTextEngineCopyMetalRuntime final
    : public QtTextEngineCopyMetalRuntime {
public:
  static std::unique_ptr<AppleQtTextEngineCopyMetalRuntime>
  Create(std::string &error) {
    gpu_execution::DeviceRequest request;
    request.preferred = gpu_execution::Backend::Metal;
    auto executionDevice = gpu_execution::Device::Create(request, error);
    if (!executionDevice) {
      if (error.empty())
        error = "product Metal device is unavailable for Qt EngineCopy";
      return {};
    }
    if (executionDevice->backend() != gpu_execution::Backend::Metal ||
        executionDevice->nativeDeviceHandle() == 0U) {
      error =
          "product GPU device is not a usable Metal device for Qt EngineCopy";
      return {};
    }
    id<MTLDevice> device = (__bridge id<MTLDevice>)(reinterpret_cast<void *>(
        executionDevice->nativeDeviceHandle()));
    if (device == nil) {
      error = "product Metal native handle is unavailable for Qt EngineCopy";
      return {};
    }
    auto state = MakeTexturedMetalPipelineState(
        device, kEngineCopyMetalSource, @"qtTextEngineCopyVertex",
        @"qtTextEngineCopyFragment", @"Qt EngineCopy",
        kTexturedTriangleVertices, kTexturedTriangleIndices, error);
    if (!state)
      return {};
    error.clear();
    return std::unique_ptr<AppleQtTextEngineCopyMetalRuntime>(
        new AppleQtTextEngineCopyMetalRuntime(std::move(executionDevice),
                                              std::move(state)));
  }

  bool Render(const QtTextEngineCopyRequest &request,
              std::vector<std::uint8_t> &outputPixels,
              std::string &error) override {
    @autoreleasepool {
      outputPixels.clear();
      if (!ValidateRequest(request, error))
        return false;

      id<MTLDevice> device = (__bridge id<MTLDevice>)(reinterpret_cast<void *>(
          executionDevice_->nativeDeviceHandle()));
      if (device == nil || state_ == nullptr) {
        error = "Qt EngineCopy Metal runtime lost its product device";
        return false;
      }

      id<MTLTexture> sourceTexture = MakePrivateRgba8Texture(
          device, request.source.width, request.source.height,
          MTLTextureUsageShaderRead);
      id<MTLTexture> outputTexture = MakePrivateRgba8Texture(
          device, request.outputWidth, request.outputHeight,
          MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead);
      const NSUInteger uploadRowBytes =
          AlignedRgba8RowBytes(device, request.source.width);
      const NSUInteger readbackRowBytes =
          AlignedRgba8RowBytes(device, request.outputWidth);
      const std::size_t uploadLength =
          static_cast<std::size_t>(uploadRowBytes) *
          static_cast<std::size_t>(request.source.height);
      const std::size_t readbackLength =
          static_cast<std::size_t>(readbackRowBytes) *
          static_cast<std::size_t>(request.outputHeight);
      id<MTLBuffer> upload =
          [device newBufferWithLength:uploadLength
                              options:MTLResourceStorageModeShared];
      id<MTLBuffer> readback =
          [device newBufferWithLength:readbackLength
                              options:MTLResourceStorageModeShared];
      const auto releaseResources = [&]() {
        [readback release];
        [upload release];
        [outputTexture release];
        [sourceTexture release];
      };
      if (sourceTexture == nil || outputTexture == nil || upload == nil ||
          readback == nil) {
        error = "Qt EngineCopy per-pass Metal resource allocation failed";
        releaseResources();
        return false;
      }

      // Public CPU rows are top-left first. AmazingEngine's negative-height
      // viewport consumes its source texture in the opposite transport row
      // order, so upload performs exactly one vertical row flip.
      auto *uploadBytes = static_cast<std::uint8_t *>(upload.contents);
      const std::size_t sourceCompactRowBytes =
          static_cast<std::size_t>(request.source.width) * 4U;
      CopyVerticallyFlippedRows(uploadBytes,
                                static_cast<std::size_t>(uploadRowBytes),
                                request.source.pixels, request.source.rowBytes,
                                sourceCompactRowBytes, request.source.height);

      id<MTLCommandBuffer> commandBuffer = [state_->queue commandBuffer];
      if (commandBuffer == nil) {
        error = "Qt EngineCopy Metal command buffer creation failed";
        releaseResources();
        return false;
      }
      commandBuffer.label = @"Qt EngineCopy native float v1";

      id<MTLBlitCommandEncoder> uploadBlit = [commandBuffer blitCommandEncoder];
      if (uploadBlit == nil) {
        error = "Qt EngineCopy upload encoder creation failed";
        releaseResources();
        return false;
      }
      CopyBufferToTexture(
          uploadBlit, upload, sourceTexture,
          MTLSizeMake(static_cast<NSUInteger>(request.source.width),
                      static_cast<NSUInteger>(request.source.height), 1U),
          uploadRowBytes,
          uploadRowBytes * static_cast<NSUInteger>(request.source.height));
      [uploadBlit endEncoding];

      if (!EncodeEngineCopy(*state_, commandBuffer, sourceTexture,
                            outputTexture, error)) {
        releaseResources();
        return false;
      }

      id<MTLBlitCommandEncoder> readbackBlit =
          [commandBuffer blitCommandEncoder];
      if (readbackBlit == nil) {
        error = "Qt EngineCopy readback encoder creation failed";
        releaseResources();
        return false;
      }
      CopyTextureToBuffer(
          readbackBlit, outputTexture, readback,
          MTLSizeMake(static_cast<NSUInteger>(request.outputWidth),
                      static_cast<NSUInteger>(request.outputHeight), 1U),
          readbackRowBytes,
          readbackRowBytes * static_cast<NSUInteger>(request.outputHeight));
      [readbackBlit endEncoding];

      [commandBuffer commit];
      [commandBuffer waitUntilCompleted];
      if (commandBuffer.status != MTLCommandBufferStatusCompleted) {
        error = MetalErrorMessage(@"Qt EngineCopy Metal command failed",
                                  commandBuffer.error);
        releaseResources();
        return false;
      }

      // Metal readback remains in AmazingEngine's internal row order. Convert
      // it back exactly once to the public top-left CPU ABI.
      const std::size_t outputCompactRowBytes =
          static_cast<std::size_t>(request.outputWidth) * 4U;
      outputPixels.resize(outputCompactRowBytes *
                          static_cast<std::size_t>(request.outputHeight));
      const auto *readbackBytes =
          static_cast<const std::uint8_t *>(readback.contents);
      CopyVerticallyFlippedRows(outputPixels.data(), outputCompactRowBytes,
                                readbackBytes,
                                static_cast<std::size_t>(readbackRowBytes),
                                outputCompactRowBytes, request.outputHeight);

      releaseResources();
      error.clear();
      return true;
    }
  }

  bool RenderTextures(void *sourceHandle, void *outputHandle, void *queueHandle,
                       std::string &error, const NativeCommandSubmission &submit) override {
    @autoreleasepool {
      id<MTLTexture> source = (id<MTLTexture>)sourceHandle;
      id<MTLTexture> output = (id<MTLTexture>)outputHandle;
      id<MTLCommandQueue> queue = (id<MTLCommandQueue>)queueHandle;
      id<MTLDevice> device = (__bridge id<MTLDevice>)(reinterpret_cast<void *>(
          executionDevice_->nativeDeviceHandle()));
      if (!state_ || source == nil || output == nil || queue == nil ||
          source == output || source.device != device || output.device != device ||
          queue.device != device || source.pixelFormat != MTLPixelFormatRGBA8Unorm ||
          output.pixelFormat != MTLPixelFormatRGBA8Unorm ||
          source.textureType != MTLTextureType2D || output.textureType != MTLTextureType2D ||
          source.sampleCount != 1U || output.sampleCount != 1U) {
        error = "Qt EngineCopy GPU texture/queue contract is invalid";
        return false;
      }
      id<MTLCommandBuffer> commands = [queue commandBuffer];
      if (commands == nil) {
        error = "Qt EngineCopy GPU command buffer creation failed";
        return false;
      }
      commands.label = @"Qt EngineCopy resident float v1";
      if (!EncodeEngineCopy(*state_, commands, source, output, error))
        return false;
      if (!SubmitNativeTextureCommand(commands, submit, {source, output}, error))
        return false;
      error.clear();
      return true;
    }
  }

  const char *BackendName() const noexcept override {
    return "qt-engine-copy-metal-float-v1";
  }

private:
  AppleQtTextEngineCopyMetalRuntime(
      std::shared_ptr<gpu_execution::Device> executionDevice,
      std::unique_ptr<MetalTexturedPipelineState> state)
      : executionDevice_(std::move(executionDevice)), state_(std::move(state)) {
  }

  std::shared_ptr<gpu_execution::Device> executionDevice_;
  std::unique_ptr<MetalTexturedPipelineState> state_;
};

} // namespace

std::unique_ptr<QtTextEngineCopyMetalRuntime>
CreateQtTextEngineCopyMetalRuntime(std::string &error) {
  @autoreleasepool {
    return AppleQtTextEngineCopyMetalRuntime::Create(error);
  }
}

} // namespace videocut::skia_runtime::internal
