#include "runtime/MetalRuntimeSupport.h"
#include "text/SinglePassMetalRgba8.h"
#include "text/shaders/MetalGaussian.h"

#include "text/QtTextGaussianMetalRuntime.h"

#include "videocut/gpu_execution/GpuExecution.h"

#import <Metal/Metal.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <utility>

namespace videocut::skia_runtime::internal {
namespace {

// This is the closed v1 branch captured from LumiGaussianBlur's two generated
// Metal fragment programs: float texture sampling, float accumulation,
// normalized linear clamp, gamma-domain convolution, and RGBA8Unorm pass
// quantization after each axis. The authored corpus fixes inverse-gamma=1,
// borderType=0, blurAlpha=1, and spaceDither=0.
constexpr const char *kGaussianMetalSource = shader_sources::MetalGaussian;

struct alignas(16) GaussianUniforms final {
  float screenParams[4];
  std::int32_t inverseGammaCorrection;
  float gamma;
  float sampleCount;
  float sigma;
  float spaceDither;
  float stepX;
  float stepY;
  std::int32_t borderType;
  std::int32_t blurAlpha;
  std::uint32_t padding[3];
};

static_assert(sizeof(GaussianUniforms) == 64U);

bool ValidateImageView(const QtTextGaussianRawRgba8ImageView &image,
                       std::string &error) {
  if ((!image.nativeTexture && image.pixels == nullptr) ||
      image.width <= 0 || image.height <= 0) {
    error = "Qt Gaussian source image is empty";
    return false;
  }
  const auto width = static_cast<std::size_t>(image.width);
  const auto height = static_cast<std::size_t>(image.height);
  if (width > std::numeric_limits<std::size_t>::max() / 4U) {
    error = "Qt Gaussian source row size overflows";
    return false;
  }
  const std::size_t compactRowBytes = width * 4U;
  if (image.nativeTexture)
    return true;
  if (image.rowBytes < compactRowBytes ||
      height > std::numeric_limits<std::size_t>::max() / image.rowBytes) {
    error = "Qt Gaussian source rowBytes is invalid";
    return false;
  }
  const std::size_t requiredBytes =
      (height - 1U) * image.rowBytes + compactRowBytes;
  if (image.byteSize < requiredBytes) {
    error = "Qt Gaussian source byteSize is smaller than its final row";
    return false;
  }
  return true;
}

bool ValidateRequest(const QtTextGaussianMetalRequest &request,
                     std::string &error) {
  if (request.implementationVersion !=
      kQtTextGaussianMetalImplementationVersion) {
    error = "Qt Gaussian implementation version is unsupported";
    return false;
  }
  if (!ValidateImageView(request.source, error))
    return false;
  if (request.axis != QtTextGaussianAxis::Horizontal &&
      request.axis != QtTextGaussianAxis::Vertical) {
    error = "Qt Gaussian axis is unsupported";
    return false;
  }
  if (!std::isfinite(request.sampleCount) || !std::isfinite(request.sigma) ||
      !std::isfinite(request.step) || !std::isfinite(request.gamma) ||
      request.sampleCount < 0.0F || request.sampleCount > 1024.0F ||
      request.sigma <= 0.0F || request.step < 0.0F ||
      request.gamma <= 0.0F) {
    error = "Qt Gaussian numeric contract is invalid";
    return false;
  }
  return true;
}

struct GaussianMetalState final {
  id<MTLCommandQueue> queue{nil};
  id<MTLRenderPipelineState> horizontalPipeline{nil};
  id<MTLRenderPipelineState> verticalPipeline{nil};
  id<MTLSamplerState> sampler{nil};
  id<MTLBuffer> vertexBuffer{nil};
  id<MTLBuffer> indexBuffer{nil};

  ~GaussianMetalState() {
    [indexBuffer release];
    [vertexBuffer release];
    [sampler release];
    [verticalPipeline release];
    [horizontalPipeline release];
    [queue release];
  }
};

id<MTLRenderPipelineState>
MakePipeline(id<MTLDevice> device, id<MTLFunction> vertex,
             id<MTLFunction> fragment, NSString *label, std::string &error) {
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
  id<MTLRenderPipelineState> pipeline =
      [device newRenderPipelineStateWithDescriptor:descriptor
                                             error:&pipelineError];
  [descriptor release];
  if (pipeline == nil)
    error = MetalErrorMessage(@"Qt Gaussian Metal pipeline creation failed",
                              pipelineError);
  return pipeline;
}

std::unique_ptr<GaussianMetalState>
CreateMetalState(id<MTLDevice> device, std::string &error) {
  auto state = std::make_unique<GaussianMetalState>();
  state->queue = [device newCommandQueue];
  if (state->queue == nil) {
    error = "Qt Gaussian Metal command queue creation failed";
    return {};
  }

  NSError *libraryError = nil;
  id<MTLLibrary> library = [device
      newLibraryWithSource:[NSString stringWithUTF8String:kGaussianMetalSource]
                   options:nil
                     error:&libraryError];
  if (library == nil) {
    error = MetalErrorMessage(@"Qt Gaussian Metal shader compilation failed",
                              libraryError);
    return {};
  }
  id<MTLFunction> vertex =
      [library newFunctionWithName:@"qtTextGaussianVertex"];
  id<MTLFunction> horizontal =
      [library newFunctionWithName:@"qtTextGaussianXFragment"];
  id<MTLFunction> vertical =
      [library newFunctionWithName:@"qtTextGaussianYFragment"];
  if (vertex == nil || horizontal == nil || vertical == nil) {
    error = "Qt Gaussian Metal shader entry point is unavailable";
  } else {
    state->horizontalPipeline =
        MakePipeline(device, vertex, horizontal, @"LumiGaussianBlur X v1",
                     error);
    if (state->horizontalPipeline != nil) {
      state->verticalPipeline =
          MakePipeline(device, vertex, vertical, @"LumiGaussianBlur Y v1",
                       error);
    }
  }
  [vertical release];
  [horizontal release];
  [vertex release];
  [library release];
  if (state->horizontalPipeline == nil || state->verticalPipeline == nil)
    return {};

  state->sampler =
      MakeNormalizedClampSampler(device, MTLSamplerMinMagFilterLinear,
                                 @"Qt Gaussian normalized linear clamp");

  state->vertexBuffer =
      [device newBufferWithBytes:kTexturedTriangleVertices
                          length:sizeof(kTexturedTriangleVertices)
                         options:MTLResourceStorageModeShared];
  state->indexBuffer =
      [device newBufferWithBytes:kTexturedTriangleIndices
                          length:sizeof(kTexturedTriangleIndices)
                         options:MTLResourceStorageModeShared];
  if (state->sampler == nil || state->vertexBuffer == nil ||
      state->indexBuffer == nil) {
    error = "Qt Gaussian immutable Metal resource creation failed";
    return {};
  }
  error.clear();
  return state;
}

class AppleQtTextGaussianMetalRuntime final
    : public QtTextGaussianMetalRuntime {
public:
  static std::unique_ptr<AppleQtTextGaussianMetalRuntime>
  Create(std::string &error) {
    gpu_execution::DeviceRequest request;
    request.preferred = gpu_execution::Backend::Metal;
    auto executionDevice = gpu_execution::Device::Create(request, error);
    if (!executionDevice) {
      if (error.empty())
        error = "product Metal device is unavailable for Qt Gaussian";
      return {};
    }
    if (executionDevice->backend() != gpu_execution::Backend::Metal ||
        executionDevice->nativeDeviceHandle() == 0U) {
      error = "product GPU device is not a usable Metal device for Qt Gaussian";
      return {};
    }
    id<MTLDevice> device = (__bridge id<MTLDevice>)(reinterpret_cast<void *>(
        executionDevice->nativeDeviceHandle()));
    if (device == nil) {
      error = "product Metal native handle is unavailable for Qt Gaussian";
      return {};
    }
    auto state = CreateMetalState(device, error);
    if (!state)
      return {};
    error.clear();
    return std::unique_ptr<AppleQtTextGaussianMetalRuntime>(
        new AppleQtTextGaussianMetalRuntime(std::move(executionDevice),
                                            std::move(state)));
  }

  bool Render(const QtTextGaussianMetalRequest &request,
              std::vector<std::uint8_t> &outputPixels,
              std::string &error,
              const QtTextGaussianTextureTarget *target) override {
    @autoreleasepool {
      outputPixels.clear();
      if (!ValidateRequest(request, error))
        return false;

      id<MTLDevice> device = (__bridge id<MTLDevice>)(reinterpret_cast<void *>(
          executionDevice_->nativeDeviceHandle()));
      if (device == nil || state_ == nullptr) {
        error = "Qt Gaussian Metal runtime lost its product device";
        return false;
      }

      return RenderSinglePassMetalRgba8(
          device, state_->queue, request.source, target, @"Qt Gaussian",
          @"Qt LumiGaussianBlur native float v1",
          [&](id<MTLRenderCommandEncoder> encoder, id<MTLTexture> sourceTexture,
              const int width, const int height) {
            GaussianUniforms uniforms{};
            uniforms.screenParams[0] = static_cast<float>(width);
            uniforms.screenParams[1] = static_cast<float>(height);
            uniforms.screenParams[2] =
                static_cast<float>(width + 1) / static_cast<float>(width);
            uniforms.screenParams[3] =
                static_cast<float>(height + 1) / static_cast<float>(height);
            uniforms.inverseGammaCorrection = 1;
            uniforms.gamma = request.gamma;
            uniforms.sampleCount = request.sampleCount;
            uniforms.sigma = request.sigma;
            uniforms.spaceDither = 0.0F;
            uniforms.stepX = request.axis == QtTextGaussianAxis::Horizontal
                                 ? request.step
                                 : 0.0F;
            uniforms.stepY = request.axis == QtTextGaussianAxis::Vertical
                                 ? request.step
                                 : 0.0F;
            uniforms.borderType = 0;
            uniforms.blurAlpha = 1;

            encoder.label = request.axis == QtTextGaussianAxis::Horizontal
                                ? @"LumiGaussianBlur X"
                                : @"LumiGaussianBlur Y";
            const MTLViewport viewport{0.0,
                                       static_cast<double>(height),
                                       static_cast<double>(width),
                                       -static_cast<double>(height),
                                       0.0,
                                       1.0};
            [encoder setViewport:viewport];
            [encoder setCullMode:MTLCullModeNone];
            [encoder setFrontFacingWinding:MTLWindingClockwise];
            [encoder
                setRenderPipelineState:request.axis ==
                                               QtTextGaussianAxis::Horizontal
                                           ? state_->horizontalPipeline
                                           : state_->verticalPipeline];
            [encoder setVertexBuffer:state_->vertexBuffer offset:0U atIndex:0U];
            [encoder setFragmentBytes:&uniforms
                               length:sizeof(uniforms)
                              atIndex:0U];
            [encoder setFragmentTexture:sourceTexture atIndex:0U];
            [encoder setFragmentSamplerState:state_->sampler atIndex:0U];
            [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                                indexCount:3U
                                 indexType:MTLIndexTypeUInt16
                               indexBuffer:state_->indexBuffer
                         indexBufferOffset:0U
                             instanceCount:1U];
          },
          outputPixels, error);
    }
  }

  const char *BackendName() const noexcept override {
    return "qt-lumi-gaussian-metal-float-v1";
  }

private:
  AppleQtTextGaussianMetalRuntime(
      std::shared_ptr<gpu_execution::Device> executionDevice,
      std::unique_ptr<GaussianMetalState> state)
      : executionDevice_(std::move(executionDevice)), state_(std::move(state)) {
  }

  std::shared_ptr<gpu_execution::Device> executionDevice_;
  std::unique_ptr<GaussianMetalState> state_;
};

} // namespace

std::unique_ptr<QtTextGaussianMetalRuntime>
CreateQtTextGaussianMetalRuntime(std::string &error) {
  @autoreleasepool {
    return AppleQtTextGaussianMetalRuntime::Create(error);
  }
}

} // namespace videocut::skia_runtime::internal
