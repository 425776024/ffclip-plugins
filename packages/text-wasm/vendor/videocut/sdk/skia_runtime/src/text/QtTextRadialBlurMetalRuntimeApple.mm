#include "runtime/MetalRuntimeSupport.h"
#include "text/SinglePassMetalRgba8.h"
#include "text/shaders/MetalRadialBlur.h"

#include "text/QtTextRadialBlurMetalRuntime.h"

#include "videocut/gpu_execution/GpuExecution.h"

#import <Metal/Metal.h>
#import <simd/simd.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <memory>
#include <utility>

namespace videocut::skia_runtime::internal {
namespace {

constexpr const char *kRadialBlurMetalSource = shader_sources::MetalRadialBlur;

struct RadialUniforms final {
  std::int32_t inverseGammaCorrection;
  float gamma;
  float intensity;
  std::int32_t blurType;
  simd_float2 center;
  float quality;
  float sampleScale;
  float sampleBias;
  float weightDecay;
  float normalizationSample;
  float dither;
  std::int32_t borderType;
  float lightIntensity;
  std::int32_t blurAlpha;
  float lightTransferMode;
  float screenWidth;
  float screenHeight;
};

static_assert(offsetof(RadialUniforms, center) == 16U);
static_assert(offsetof(RadialUniforms, screenWidth) == 64U);
static_assert(sizeof(RadialUniforms) == 72U);

bool ValidateRequest(const QtTextRadialBlurRenderRequest &request,
                     std::string &error) {
  if (request.implementationId != kQtTextRadialBlurImplementationId ||
      request.implementationVersion != kQtTextRadialBlurImplementationVersion ||
      request.sourceContractDigest != kQtTextRadialBlurSourceContractDigest) {
    error = "Qt LumiRadialBlur implementation identity is unsupported";
    return false;
  }
  const auto &source = request.source;
  if ((!source.pixels && !source.nativeTexture) ||
      source.width <= 0 || source.height <= 0) {
    error = "Qt LumiRadialBlur source image is empty";
    return false;
  }
  const std::size_t compactRowBytes =
      static_cast<std::size_t>(source.width) * 4U;
  if (!source.nativeTexture && (source.rowBytes < compactRowBytes ||
      source.byteSize <
          (static_cast<std::size_t>(source.height) - 1U) * source.rowBytes +
              compactRowBytes)) {
    error = "Qt LumiRadialBlur source image ABI is invalid";
    return false;
  }
  const std::array<float, 10> values{request.intensity,
                                     request.center[0],
                                     request.center[1],
                                     request.quality,
                                     request.weightDecay,
                                     request.dither,
                                     request.gamma,
                                     request.lightIntensity,
                                     request.lightTransferMode,
                                     static_cast<float>(source.width) /
                                         static_cast<float>(source.height)};
  if (!std::all_of(values.begin(), values.end(),
                   [](const float value) { return std::isfinite(value); })) {
    error = "Qt LumiRadialBlur uniforms are non-finite";
    return false;
  }
  if (request.blurType < 0 || request.blurType > 5 || request.borderType < 0 ||
      request.borderType > 1 || request.blurAlpha < 0 ||
      request.blurAlpha > 1 || request.inverseGammaCorrection < 0 ||
      request.inverseGammaCorrection > 1 || request.quality <= 0.0F ||
      request.gamma <= 0.0F || request.lightIntensity < 1.0F ||
      request.lightTransferMode < 0.0F || request.lightTransferMode > 3.0F) {
    error = "Qt LumiRadialBlur uniform branch is unsupported";
    return false;
  }
  return true;
}

class AppleQtTextRadialBlurMetalRuntime final
    : public QtTextRadialBlurMetalRuntime {
public:
  static std::unique_ptr<AppleQtTextRadialBlurMetalRuntime>
  Create(std::string &error) {
    gpu_execution::DeviceRequest request;
    request.preferred = gpu_execution::Backend::Metal;
    auto executionDevice = gpu_execution::Device::Create(request, error);
    if (!executionDevice ||
        executionDevice->backend() != gpu_execution::Backend::Metal ||
        executionDevice->nativeDeviceHandle() == 0U) {
      if (error.empty())
        error = "product Metal device is unavailable for Qt LumiRadialBlur";
      return {};
    }
    id<MTLDevice> device = (__bridge id<MTLDevice>)(reinterpret_cast<void *>(
        executionDevice->nativeDeviceHandle()));
    if (device == nil) {
      error =
          "product Metal native handle is unavailable for Qt LumiRadialBlur";
      return {};
    }
    auto state = MakeTexturedMetalPipelineState(
        device, kRadialBlurMetalSource, @"qtTextRadialBlurVertex",
        @"qtTextRadialBlurFragment", @"Qt LumiRadialBlur",
        kTexturedQuadVertices, kTexturedQuadIndices, error);
    if (!state)
      return {};
    return std::unique_ptr<AppleQtTextRadialBlurMetalRuntime>(
        new AppleQtTextRadialBlurMetalRuntime(std::move(executionDevice),
                                              std::move(state)));
  }

  bool Render(const QtTextRadialBlurRenderRequest &request,
              std::vector<std::uint8_t> &outputPixels,
              std::string &error,
              const QtTextRadialBlurTextureTarget *target) override {
    @autoreleasepool {
      outputPixels.clear();
      if (!ValidateRequest(request, error))
        return false;
      id<MTLDevice> device = (__bridge id<MTLDevice>)(reinterpret_cast<void *>(
          executionDevice_->nativeDeviceHandle()));
      if (device == nil || state_ == nullptr) {
        error = "Qt LumiRadialBlur Metal runtime lost its product device";
        return false;
      }

      return RenderSinglePassMetalRgba8(
          device, state_->queue, request.source, target, @"Qt LumiRadialBlur",
          @"Qt LumiRadialBlur native v1",
          [&](id<MTLRenderCommandEncoder> encoder, id<MTLTexture> sourceTexture,
              const int width, const int height) {
            const RadialUniforms uniforms{
                request.inverseGammaCorrection,
                request.gamma,
                request.intensity,
                request.blurType,
                simd_make_float2(request.center[0], request.center[1]),
                request.quality,
                8.0F,
                5.0F,
                request.weightDecay,
                50.0F,
                request.dither,
                request.borderType,
                request.lightIntensity,
                request.blurAlpha,
                request.lightTransferMode,
                static_cast<float>(width),
                static_cast<float>(height),
            };

            const MTLViewport viewport{0.0,
                                       static_cast<double>(height),
                                       static_cast<double>(width),
                                       -static_cast<double>(height),
                                       0.0,
                                       1.0};
            [encoder setViewport:viewport];
            [encoder setCullMode:MTLCullModeNone];
            [encoder setRenderPipelineState:state_->pipeline];
            [encoder setVertexBuffer:state_->vertexBuffer offset:0U atIndex:0U];
            [encoder setFragmentBytes:&uniforms
                               length:sizeof(uniforms)
                              atIndex:0U];
            [encoder setFragmentTexture:sourceTexture atIndex:0U];
            [encoder setFragmentSamplerState:state_->sampler atIndex:0U];
            [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                                indexCount:6U
                                 indexType:MTLIndexTypeUInt32
                               indexBuffer:state_->indexBuffer
                         indexBufferOffset:0U
                             instanceCount:1U];
          },
          outputPixels, error);
    }
  }

  const char *BackendName() const noexcept override {
    return "qt-lumi-radial-blur-metal-v1";
  }

private:
  AppleQtTextRadialBlurMetalRuntime(
      std::shared_ptr<gpu_execution::Device> executionDevice,
      std::unique_ptr<MetalTexturedPipelineState> state)
      : executionDevice_(std::move(executionDevice)), state_(std::move(state)) {
  }

  std::shared_ptr<gpu_execution::Device> executionDevice_;
  std::unique_ptr<MetalTexturedPipelineState> state_;
};

} // namespace

std::unique_ptr<QtTextRadialBlurMetalRuntime>
CreateQtTextRadialBlurMetalRuntime(std::string &error) {
  @autoreleasepool {
    return AppleQtTextRadialBlurMetalRuntime::Create(error);
  }
}

} // namespace videocut::skia_runtime::internal
