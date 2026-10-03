#include "runtime/MetalRuntimeSupport.h"
#include "text/SinglePassMetalRgba8.h"
#include "text/shaders/MetalShake.h"

#include "text/QtTextShakeMetalRuntime.h"

#include "videocut/gpu_execution/GpuExecution.h"

#import <Metal/Metal.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <memory>
#include <utility>

namespace videocut::skia_runtime::internal {
namespace {

constexpr const char *kShakeMetalSource = shader_sources::MetalShake;

struct ShakeFillModes final {
  std::int32_t x;
  std::int32_t y;
};

static_assert(sizeof(ShakeFillModes) == 8U);
static_assert(sizeof(QtTextShakeRenderRequest::uvMatrices) == 192U);

bool ValidateRequest(const QtTextShakeRenderRequest &request,
                     std::string &error) {
  if (request.implementationId != kQtTextShakeImplementationId ||
      request.implementationVersion != kQtTextShakeImplementationVersion ||
      request.sourceContractDigest != kQtTextShakeSourceContractDigest) {
    error = "Qt LumiSShake implementation identity is unsupported";
    return false;
  }
  const auto &source = request.source;
  if ((!source.nativeTexture && source.pixels == nullptr) || source.width <= 0 || source.height <= 0) {
    error = "Qt LumiSShake source image is empty";
    return false;
  }
  const std::size_t compactRowBytes =
      static_cast<std::size_t>(source.width) * 4U;
  if (!source.nativeTexture && (source.rowBytes < compactRowBytes ||
      source.byteSize <
          (static_cast<std::size_t>(source.height) - 1U) * source.rowBytes +
              compactRowBytes)) {
    error = "Qt LumiSShake source image ABI is invalid";
    return false;
  }
  if (request.fillModeX < 0 || request.fillModeX > 2 ||
      request.fillModeY < 0 || request.fillModeY > 2) {
    error = "Qt LumiSShake fill mode is unsupported";
    return false;
  }
  for (const auto &matrix : request.uvMatrices) {
    for (const float value : matrix) {
      if (!std::isfinite(value)) {
        error = "Qt LumiSShake matrix is non-finite";
        return false;
      }
    }
  }
  return true;
}

class AppleQtTextShakeMetalRuntime final : public QtTextShakeMetalRuntime {
public:
  static std::unique_ptr<AppleQtTextShakeMetalRuntime>
  Create(std::string &error) {
    gpu_execution::DeviceRequest request;
    request.preferred = gpu_execution::Backend::Metal;
    auto executionDevice = gpu_execution::Device::Create(request, error);
    if (!executionDevice ||
        executionDevice->backend() != gpu_execution::Backend::Metal ||
        executionDevice->nativeDeviceHandle() == 0U) {
      if (error.empty())
        error = "product Metal device is unavailable for Qt LumiSShake";
      return {};
    }
    id<MTLDevice> device = (__bridge id<MTLDevice>)(reinterpret_cast<void *>(
        executionDevice->nativeDeviceHandle()));
    if (device == nil) {
      error = "product Metal native handle is unavailable for Qt LumiSShake";
      return {};
    }
    auto state = MakeTexturedMetalPipelineState(
        device, kShakeMetalSource, @"qtTextShakeVertex", @"qtTextShakeFragment",
        @"Qt LumiSShake", kTexturedQuadVertices, kTexturedQuadIndices, error);
    if (!state)
      return {};
    return std::unique_ptr<AppleQtTextShakeMetalRuntime>(
        new AppleQtTextShakeMetalRuntime(std::move(executionDevice),
                                         std::move(state)));
  }

  bool Render(const QtTextShakeRenderRequest &request,
              std::vector<std::uint8_t> &outputPixels,
              std::string &error,
              const QtTextShakeTextureTarget *target) override {
    @autoreleasepool {
      outputPixels.clear();
      if (!ValidateRequest(request, error))
        return false;
      id<MTLDevice> device = (__bridge id<MTLDevice>)(reinterpret_cast<void *>(
          executionDevice_->nativeDeviceHandle()));
      if (device == nil || state_ == nullptr) {
        error = "Qt LumiSShake Metal runtime lost its product device";
        return false;
      }

      return RenderSinglePassMetalRgba8(
          device, state_->queue, request.source, target, @"Qt LumiSShake",
          @"Qt LumiSShake native v1",
          [&](id<MTLRenderCommandEncoder> encoder, id<MTLTexture> sourceTexture,
              const int width, const int height) {
            const MTLViewport viewport{0.0,
                                       static_cast<double>(height),
                                       static_cast<double>(width),
                                       -static_cast<double>(height),
                                       0.0,
                                       1.0};
            const ShakeFillModes fill{request.fillModeX, request.fillModeY};
            [encoder setViewport:viewport];
            [encoder setCullMode:MTLCullModeBack];
            [encoder setFrontFacingWinding:MTLWindingClockwise];
            [encoder setRenderPipelineState:state_->pipeline];
            [encoder setVertexBuffer:state_->vertexBuffer offset:0U atIndex:0U];
            [encoder setVertexBytes:request.uvMatrices.data()
                             length:sizeof(request.uvMatrices)
                            atIndex:1U];
            [encoder setFragmentBytes:&fill length:sizeof(fill) atIndex:0U];
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
    return "qt-lumi-s-shake-metal-v1";
  }

private:
  AppleQtTextShakeMetalRuntime(
      std::shared_ptr<gpu_execution::Device> executionDevice,
      std::unique_ptr<MetalTexturedPipelineState> state)
      : executionDevice_(std::move(executionDevice)), state_(std::move(state)) {
  }

  std::shared_ptr<gpu_execution::Device> executionDevice_;
  std::unique_ptr<MetalTexturedPipelineState> state_;
};

} // namespace

std::unique_ptr<QtTextShakeMetalRuntime>
CreateQtTextShakeMetalRuntime(std::string &error) {
  @autoreleasepool {
    return AppleQtTextShakeMetalRuntime::Create(error);
  }
}

} // namespace videocut::skia_runtime::internal
