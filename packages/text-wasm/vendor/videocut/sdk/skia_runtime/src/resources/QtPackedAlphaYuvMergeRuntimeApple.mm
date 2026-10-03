#include "runtime/MetalRuntimeSupport.h"
#include "resources/QtPackedAlphaYuvMergeRuntime.h"

#include "videocut/gpu_execution/GpuExecution.h"
#include "videocut/skia_runtime/SkiaRuntimeFactory.h"

#include <cmath>

#import <Metal/Metal.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <memory>
#include <new>
#include <utility>

namespace videocut::skia_runtime::internal {
namespace {

// Captured Qt/AmazingEngine shaders:
//   vertex SHA-256 prefix   aebca...
//   fragment SHA-256 prefix 39cd...
// Entry-point names are local; the coordinate and float math are preserved.
constexpr const char *kQtPackedAlphaYuvMergeMetalSource = R"metal(
#include <metal_stdlib>
#include <simd/simd.h>
using namespace metal;

struct PackedAlphaVertexOut {
  float2 texCoord;
  float4 gl_Position [[position]];
};

vertex PackedAlphaVertexOut qtPackedAlphaVertex(
    const device packed_float2 *positions [[buffer(0)]],
    uint vertexId [[vertex_id]]) {
  PackedAlphaVertexOut out = {};
  float2 pos = float2(positions[vertexId]);
  out.texCoord = pos.xy * 0.5 + 0.5;
  out.gl_Position = float4(pos.xy, 0.0, 1.0);
  out.gl_Position.z =
      (out.gl_Position.z + out.gl_Position.w) * 0.5;
  return out;
}

struct PackedAlphaFragmentOut {
  float4 gl_FragColor [[color(0)]];
};

fragment PackedAlphaFragmentOut qtPackedAlphaYuv420pFragment(
    PackedAlphaVertexOut in [[stage_in]],
    constant float& yChannelStride [[buffer(0)]],
    constant float& yWidth [[buffer(1)]],
    constant float& uvChannelStride [[buffer(2)]],
    constant float& uvWidth [[buffer(3)]],
    constant float3x3& colorConversionMatrix [[buffer(4)]],
    texture2d<float> luminanceTexture [[texture(0)]],
    texture2d<float> chrominanceUTexture [[texture(1)]],
    texture2d<float> chrominanceVTexture [[texture(2)]],
    sampler luminanceTextureSmplr [[sampler(0)]],
    sampler chrominanceUTextureSmplr [[sampler(1)]],
    sampler chrominanceVTextureSmplr [[sampler(2)]]) {
  PackedAlphaFragmentOut out = {};
  float yPixelW = 1.0 / yChannelStride;
  float yRange = ((yWidth / yChannelStride) / 2.0) - yPixelW;
  float uvPixelW = 1.0 / uvChannelStride;
  float uvRange = ((uvWidth / uvChannelStride) / 2.0) - uvPixelW;
  float2 yRgbTexCoord =
      float2(((in.texCoord.x * yRange) + yRange) + yPixelW, in.texCoord.y);
  float2 uvRgbTexCoord =
      float2(((in.texCoord.x * uvRange) + uvRange) + uvPixelW,
             in.texCoord.y);
  float2 yAlphaTexCoord =
      float2(in.texCoord.x * yRange, in.texCoord.y);
  float2 uvAlphaTexCoord =
      float2(in.texCoord.x * uvRange, in.texCoord.y);

  float3 yuvRgb;
  yuvRgb.x = luminanceTexture.sample(luminanceTextureSmplr,
                                     yRgbTexCoord).x -
             0.062745101749897003173828125;
  yuvRgb.y = chrominanceUTexture.sample(chrominanceUTextureSmplr,
                                        uvRgbTexCoord).x -
             0.5;
  yuvRgb.z = chrominanceVTexture.sample(chrominanceVTextureSmplr,
                                        uvRgbTexCoord).x -
             0.5;
  float3 rgb = colorConversionMatrix * yuvRgb;

  float3 yuvAlpha;
  yuvAlpha.x = luminanceTexture.sample(luminanceTextureSmplr,
                                       yAlphaTexCoord).x -
               0.062745101749897003173828125;
  yuvAlpha.y = chrominanceUTexture.sample(chrominanceUTextureSmplr,
                                          uvAlphaTexCoord).x -
               0.5;
  yuvAlpha.z = chrominanceVTexture.sample(chrominanceVTextureSmplr,
                                          uvAlphaTexCoord).x -
               0.5;
  float3 alphaColor = colorConversionMatrix * yuvAlpha;
  float alpha = ((alphaColor.x + alphaColor.y) + alphaColor.z) / 3.0;

  rgb = fast::clamp(rgb, float3(0.0), float3(1.0));
  out.gl_FragColor = float4(rgb, alpha);
  return out;
}
)metal";

struct PackedAlphaVertex final {
  float x;
  float y;
};

constexpr PackedAlphaVertex kQuadVertices[] = {
    {-1.0F, 1.0F},
    {1.0F, 1.0F},
    {1.0F, -1.0F},
    {-1.0F, -1.0F},
};
constexpr std::uint16_t kQuadIndices[] = {0U, 1U, 2U, 2U, 3U, 0U};

static_assert(sizeof(PackedAlphaVertex) == 8U);

struct alignas(16) MetalFloat3 final {
  float x;
  float y;
  float z;
  float padding;
};

struct alignas(16) MetalFloat3x3 final {
  MetalFloat3 columns[3];
};

static_assert(sizeof(MetalFloat3) == 16U);
static_assert(sizeof(MetalFloat3x3) == 48U);

struct PlaneGeometry final {
  frame::PlaneReadView view;
  std::uint32_t visibleWidth{0U};
  std::uint32_t visibleHeight{0U};
  std::size_t sourceBytes{0U};
  std::size_t uploadRowBytes{0U};
  std::size_t uploadBytes{0U};
};

struct ValidatedRequest final {
  frame::CpuReadMap map;
  std::array<PlaneGeometry, 3U> planes;
  std::uint32_t outputWidth{0U};
  std::uint32_t outputHeight{0U};
  std::size_t compactOutputRowBytes{0U};
  std::size_t compactOutputBytes{0U};
  std::size_t readbackRowBytes{0U};
  std::size_t readbackBytes{0U};
};

bool CheckedMultiply(const std::size_t left, const std::size_t right,
                     std::size_t &result) noexcept {
  if (left != 0U && right > std::numeric_limits<std::size_t>::max() / left)
    return false;
  result = left * right;
  return true;
}

bool CheckedAdd(const std::size_t left, const std::size_t right,
                std::size_t &result) noexcept {
  if (right > std::numeric_limits<std::size_t>::max() - left)
    return false;
  result = left + right;
  return true;
}

bool AlignUp(const std::size_t value, const std::size_t alignment,
             std::size_t &result) noexcept {
  if (alignment == 0U)
    return false;
  const std::size_t remainder = value % alignment;
  if (remainder == 0U) {
    result = value;
    return true;
  }
  return CheckedAdd(value, alignment - remainder, result);
}

bool AddToBudget(std::size_t value, std::size_t &total,
                 const std::size_t maximum) noexcept {
  std::size_t next = 0U;
  if (!CheckedAdd(total, value, next) || next > maximum)
    return false;
  total = next;
  return true;
}

bool ValidatePlane(const frame::PlaneReadView &view,
                   const std::uint32_t visibleWidth,
                   const std::uint32_t visibleHeight, const char *planeName,
                   PlaneGeometry &output, std::string &error) {
  if (view.data == nullptr || view.pixel_stride != 1U ||
      view.row_stride < visibleWidth || visibleWidth == 0U ||
      visibleHeight == 0U) {
    error = std::string("Qt packed-alpha ") + planeName +
            " plane geometry is invalid";
    return false;
  }

  std::size_t sourceBytes = 0U;
  if (!CheckedMultiply(static_cast<std::size_t>(view.row_stride),
                       static_cast<std::size_t>(visibleHeight), sourceBytes) ||
      sourceBytes != view.size) {
    error = std::string("Qt packed-alpha ") + planeName +
            " plane size must equal every full-stride row";
    return false;
  }

  output.view = view;
  output.visibleWidth = visibleWidth;
  output.visibleHeight = visibleHeight;
  output.sourceBytes = sourceBytes;
  return true;
}

bool ValidateRequest(const frame::VideoFrame &source,
                     const QtPackedAlphaYuvMergeLimits &limits,
                     id<MTLDevice> device, ValidatedRequest &output,
                     std::string &error) {
  if (limits.implementationVersion !=
      kQtPackedAlphaYuvMergeImplementationVersion) {
    error = "Qt packed-alpha YUV merge implementation version is unsupported";
    return false;
  }
  if (limits.maximumOutputBytes == 0U || limits.maximumWorkingSetBytes == 0U) {
    error = "Qt packed-alpha YUV merge byte budgets must be positive";
    return false;
  }
  if (!source) {
    error = "Qt packed-alpha YUV source frame is empty";
    return false;
  }

  const auto &desc = source.desc();
  if (desc.format != frame::PixelFormat::Yuv420p ||
      desc.data_type != frame::DataType::U8 || desc.plane_count != 3U) {
    error = "Qt packed-alpha YUV source must be Yuv420p/U8 with 3 planes";
    return false;
  }
  if (desc.rotation != frame::Rotation::R0) {
    error = "Qt packed-alpha YUV source must have stored rotation R0";
    return false;
  }
  // Every packed half must itself be valid 4:2:0 content. Consequently the
  // stored 2W width is divisible by four, and H is even.
  if (desc.width < 4U || desc.height < 2U || (desc.width % 4U) != 0U ||
      (desc.height % 2U) != 0U) {
    error = "Qt packed-alpha YUV source dimensions must be 2W x H with even "
            "W and H";
    return false;
  }

  const auto descriptorValidation = frame::ValidateFrameDesc(desc);
  if (!descriptorValidation) {
    error = "Qt packed-alpha YUV source descriptor is invalid: " +
            descriptorValidation.error().message();
    return false;
  }
  auto mapResult = source.mapRead();
  if (!mapResult) {
    error = "Qt packed-alpha YUV source is not CPU-mappable: " +
            mapResult.error().message();
    return false;
  }
  output.map = std::move(mapResult).value();
  if (output.map.planeCount() != 3U) {
    error = "Qt packed-alpha YUV mapped source has an invalid plane count";
    return false;
  }

  const std::array<std::uint32_t, 3U> widths = {desc.width, desc.width / 2U,
                                                desc.width / 2U};
  const std::array<std::uint32_t, 3U> heights = {desc.height, desc.height / 2U,
                                                 desc.height / 2U};
  constexpr std::array<const char *, 3U> names = {"Y", "U", "V"};
  for (std::size_t index = 0U; index < output.planes.size(); ++index) {
    auto planeResult = output.map.plane(static_cast<std::uint8_t>(index));
    if (!planeResult) {
      error = std::string("Qt packed-alpha ") + names[index] +
              " plane cannot be mapped: " + planeResult.error().message();
      return false;
    }
    if (!ValidatePlane(planeResult.value(), widths[index], heights[index],
                       names[index], output.planes[index], error)) {
      return false;
    }
  }
  if (output.planes[1].view.row_stride != output.planes[2].view.row_stride) {
    error = "Qt packed-alpha U and V row strides must match the captured "
            "single uvChannelStride contract";
    return false;
  }

  // The product backend requires macOS Metal family hardware; its 2D texture
  // limit is 16384. Keep the limit explicit because row stride is itself used
  // as the R8 texture width.
  constexpr std::uint32_t maximumTextureDimension =
      kRuntimePackedYuvMaximumPlaneStride;
  for (std::size_t index = 0U; index < output.planes.size(); ++index) {
    if (output.planes[index].view.row_stride > maximumTextureDimension ||
        output.planes[index].visibleHeight > maximumTextureDimension) {
      error = std::string("Qt packed-alpha ") + names[index] +
              " stride texture exceeds the Metal device limit";
      return false;
    }
  }

  output.outputWidth = desc.width / 2U;
  output.outputHeight = desc.height;
  if (output.outputWidth > maximumTextureDimension ||
      output.outputHeight > maximumTextureDimension) {
    error = "Qt packed-alpha output exceeds the Metal device limit";
    return false;
  }
  if (!CheckedMultiply(static_cast<std::size_t>(output.outputWidth), 4U,
                       output.compactOutputRowBytes) ||
      !CheckedMultiply(output.compactOutputRowBytes,
                       static_cast<std::size_t>(output.outputHeight),
                       output.compactOutputBytes)) {
    error = "Qt packed-alpha output byte size overflows";
    return false;
  }
  if (output.compactOutputBytes > limits.maximumOutputBytes) {
    error = "Qt packed-alpha output exceeds its byte budget";
    return false;
  }

  const std::size_t r8Alignment = std::max<std::size_t>(
      1U,
      [device
          minimumLinearTextureAlignmentForPixelFormat:MTLPixelFormatR8Unorm]);
  for (auto &plane : output.planes) {
    if (!AlignUp(static_cast<std::size_t>(plane.view.row_stride), r8Alignment,
                 plane.uploadRowBytes) ||
        !CheckedMultiply(plane.uploadRowBytes,
                         static_cast<std::size_t>(plane.visibleHeight),
                         plane.uploadBytes)) {
      error = "Qt packed-alpha plane upload byte size overflows";
      return false;
    }
  }

  const std::size_t rgbaAlignment = std::max<std::size_t>(
      1U, [device minimumLinearTextureAlignmentForPixelFormat:
                      MTLPixelFormatRGBA8Unorm]);
  if (!AlignUp(output.compactOutputRowBytes, rgbaAlignment,
               output.readbackRowBytes) ||
      !CheckedMultiply(output.readbackRowBytes,
                       static_cast<std::size_t>(output.outputHeight),
                       output.readbackBytes)) {
    error = "Qt packed-alpha readback byte size overflows";
    return false;
  }

  std::size_t workingSetBytes = 0U;
  for (const auto &plane : output.planes) {
    // One private R8 texture at the true stride width and one aligned upload
    // buffer. The source frame itself remains caller-owned and is not counted.
    if (!AddToBudget(plane.sourceBytes, workingSetBytes,
                     limits.maximumWorkingSetBytes) ||
        !AddToBudget(plane.uploadBytes, workingSetBytes,
                     limits.maximumWorkingSetBytes)) {
      error = "Qt packed-alpha plane resources exceed the working-set budget";
      return false;
    }
  }
  if (!AddToBudget(output.compactOutputBytes, workingSetBytes,
                   limits.maximumWorkingSetBytes) ||
      !AddToBudget(output.readbackBytes, workingSetBytes,
                   limits.maximumWorkingSetBytes) ||
      !AddToBudget(output.compactOutputBytes, workingSetBytes,
                   limits.maximumWorkingSetBytes)) {
    error = "Qt packed-alpha output resources exceed the working-set budget";
    return false;
  }
  return true;
}

id<MTLTexture> MakePrivateR8Texture(id<MTLDevice> device,
                                    const std::uint32_t width,
                                    const std::uint32_t height) {
  MTLTextureDescriptor *descriptor = [MTLTextureDescriptor
      texture2DDescriptorWithPixelFormat:MTLPixelFormatR8Unorm
                                   width:static_cast<NSUInteger>(width)
                                  height:static_cast<NSUInteger>(height)
                               mipmapped:NO];
  descriptor.storageMode = MTLStorageModePrivate;
  descriptor.usage = MTLTextureUsageShaderRead;
  return [device newTextureWithDescriptor:descriptor];
}

id<MTLTexture> MakeOutputTexture(id<MTLDevice> device,
                                 const std::uint32_t width,
                                 const std::uint32_t height) {
  MTLTextureDescriptor *descriptor = [MTLTextureDescriptor
      texture2DDescriptorWithPixelFormat:MTLPixelFormatRGBA8Unorm
                                   width:static_cast<NSUInteger>(width)
                                  height:static_cast<NSUInteger>(height)
                               mipmapped:NO];
  descriptor.storageMode = MTLStorageModePrivate;
  descriptor.usage = MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead;
  return [device newTextureWithDescriptor:descriptor];
}

struct PackedAlphaMetalState final {
  id<MTLCommandQueue> queue{nil};
  id<MTLRenderPipelineState> pipeline{nil};
  id<MTLSamplerState> sampler{nil};
  id<MTLBuffer> vertexBuffer{nil};
  id<MTLBuffer> indexBuffer{nil};

  ~PackedAlphaMetalState() {
    [indexBuffer release];
    [vertexBuffer release];
    [sampler release];
    [pipeline release];
    [queue release];
  }
};

std::unique_ptr<PackedAlphaMetalState> CreateMetalState(id<MTLDevice> device,
                                                        std::string &error) {
  auto state = std::make_unique<PackedAlphaMetalState>();
  state->queue = [device newCommandQueue];
  if (state->queue == nil) {
    error = "Qt packed-alpha Metal command queue creation failed";
    return {};
  }

  NSError *libraryError = nil;
  id<MTLLibrary> library = [device
      newLibraryWithSource:
          [NSString stringWithUTF8String:kQtPackedAlphaYuvMergeMetalSource]
                   options:nil
                     error:&libraryError];
  if (library == nil) {
    error = MetalErrorMessage(
        @"Qt packed-alpha Metal shader compilation failed", libraryError);
    return {};
  }
  id<MTLFunction> vertex = [library newFunctionWithName:@"qtPackedAlphaVertex"];
  id<MTLFunction> fragment =
      [library newFunctionWithName:@"qtPackedAlphaYuv420pFragment"];
  if (vertex == nil || fragment == nil) {
    error = "Qt packed-alpha Metal shader entry point is unavailable";
  } else {
    auto *descriptor = [[MTLRenderPipelineDescriptor alloc] init];
    descriptor.vertexFunction = vertex;
    descriptor.fragmentFunction = fragment;
    descriptor.rasterSampleCount = 1U;
    auto *color = descriptor.colorAttachments[0];
    color.pixelFormat = MTLPixelFormatRGBA8Unorm;
    color.blendingEnabled = NO;
    color.writeMask = MTLColorWriteMaskAll;
    NSError *pipelineError = nil;
    state->pipeline =
        [device newRenderPipelineStateWithDescriptor:descriptor
                                               error:&pipelineError];
    [descriptor release];
    if (state->pipeline == nil) {
      error = MetalErrorMessage(
          @"Qt packed-alpha Metal pipeline creation failed", pipelineError);
    }
  }
  [fragment release];
  [vertex release];
  [library release];
  if (state->pipeline == nil)
    return {};

  state->sampler =
      MakeNormalizedClampSampler(device, MTLSamplerMinMagFilterLinear,
                                 @"Qt packed-alpha normalized linear clamp");

  state->vertexBuffer =
      [device newBufferWithBytes:kQuadVertices
                          length:sizeof(kQuadVertices)
                         options:MTLResourceStorageModeShared];
  state->indexBuffer = [device newBufferWithBytes:kQuadIndices
                                           length:sizeof(kQuadIndices)
                                          options:MTLResourceStorageModeShared];
  if (state->sampler == nil || state->vertexBuffer == nil ||
      state->indexBuffer == nil) {
    error = "Qt packed-alpha immutable Metal resource creation failed";
    return {};
  }
  error.clear();
  return state;
}

class AppleQtPackedAlphaYuvMergeRuntime final
    : public QtPackedAlphaYuvMergeRuntime {
public:
  static std::unique_ptr<AppleQtPackedAlphaYuvMergeRuntime>
  Create(std::string &error) {
    gpu_execution::DeviceRequest request;
    request.preferred = gpu_execution::Backend::Metal;
    auto executionDevice = gpu_execution::Device::Create(request, error);
    if (!executionDevice) {
      if (error.empty())
        error = "product Metal device is unavailable for Qt packed-alpha";
      return {};
    }
    if (executionDevice->backend() != gpu_execution::Backend::Metal ||
        executionDevice->nativeDeviceHandle() == 0U) {
      error = "product GPU device is not a usable Metal device for Qt "
              "packed-alpha";
      return {};
    }
    id<MTLDevice> device = (__bridge id<MTLDevice>)(reinterpret_cast<void *>(
        executionDevice->nativeDeviceHandle()));
    if (device == nil) {
      error = "product Metal native handle is unavailable for Qt "
              "packed-alpha";
      return {};
    }
    auto state = CreateMetalState(device, error);
    if (!state)
      return {};
    error.clear();
    return std::unique_ptr<AppleQtPackedAlphaYuvMergeRuntime>(
        new AppleQtPackedAlphaYuvMergeRuntime(std::move(executionDevice),
                                              std::move(state)));
  }

  bool Render(const frame::VideoFrame &source,
              const QtPackedAlphaYuvMergeLimits &limits,
              QtPackedAlphaYuvMergeOutput &output,
              std::string &error) override {
    @autoreleasepool {
      output.Clear();
      id<MTLDevice> device = (__bridge id<MTLDevice>)(reinterpret_cast<void *>(
          executionDevice_->nativeDeviceHandle()));
      if (device == nil || state_ == nullptr) {
        error = "Qt packed-alpha Metal runtime lost its product device";
        return false;
      }

      ValidatedRequest validated;
      if (!ValidateRequest(source, limits, device, validated, error))
        return false;

      std::array<id<MTLTexture>, 3U> planeTextures = {nil, nil, nil};
      std::array<id<MTLBuffer>, 3U> uploadBuffers = {nil, nil, nil};
      id<MTLTexture> outputTexture = nil;
      id<MTLBuffer> readbackBuffer = nil;
      const auto releaseResources = [&]() {
        [readbackBuffer release];
        [outputTexture release];
        for (std::size_t index = 0U; index < planeTextures.size(); ++index) {
          [uploadBuffers[index] release];
          [planeTextures[index] release];
        }
      };

      for (std::size_t index = 0U; index < planeTextures.size(); ++index) {
        const auto &plane = validated.planes[index];
        planeTextures[index] = MakePrivateR8Texture(
            device, plane.view.row_stride, plane.visibleHeight);
        uploadBuffers[index] =
            [device newBufferWithLength:plane.uploadBytes
                                options:MTLResourceStorageModeShared];
        if (planeTextures[index] == nil || uploadBuffers[index] == nil) {
          error = "Qt packed-alpha plane Metal allocation failed";
          releaseResources();
          return false;
        }

        auto *destination =
            static_cast<std::uint8_t *>(uploadBuffers[index].contents);
        const auto *sourceBytes =
            reinterpret_cast<const std::uint8_t *>(plane.view.data);
        for (std::uint32_t row = 0U; row < plane.visibleHeight; ++row) {
          std::memcpy(destination +
                          static_cast<std::size_t>(row) * plane.uploadRowBytes,
                      sourceBytes +
                          static_cast<std::size_t>(row) * plane.view.row_stride,
                      plane.view.row_stride);
        }
      }

      outputTexture = MakeOutputTexture(device, validated.outputWidth,
                                        validated.outputHeight);
      readbackBuffer =
          [device newBufferWithLength:validated.readbackBytes
                              options:MTLResourceStorageModeShared];
      if (outputTexture == nil || readbackBuffer == nil) {
        error = "Qt packed-alpha output Metal allocation failed";
        releaseResources();
        return false;
      }

      id<MTLCommandBuffer> commandBuffer = [state_->queue commandBuffer];
      if (commandBuffer == nil) {
        error = "Qt packed-alpha Metal command buffer creation failed";
        releaseResources();
        return false;
      }
      commandBuffer.label = @"Qt VideoAnimSeq packed-alpha YUV merge v1";

      id<MTLBlitCommandEncoder> uploadEncoder =
          [commandBuffer blitCommandEncoder];
      if (uploadEncoder == nil) {
        error = "Qt packed-alpha upload encoder creation failed";
        releaseResources();
        return false;
      }
      for (std::size_t index = 0U; index < planeTextures.size(); ++index) {
        const auto &plane = validated.planes[index];
        CopyBufferToTexture(
            uploadEncoder, uploadBuffers[index], planeTextures[index],
            MTLSizeMake(static_cast<NSUInteger>(plane.view.row_stride),
                        static_cast<NSUInteger>(plane.visibleHeight), 1U),
            static_cast<NSUInteger>(plane.uploadRowBytes),
            static_cast<NSUInteger>(plane.uploadBytes));
      }
      [uploadEncoder endEncoding];

      MTLRenderPassDescriptor *pass =
          [MTLRenderPassDescriptor renderPassDescriptor];
      pass.colorAttachments[0].texture = outputTexture;
      pass.colorAttachments[0].loadAction = MTLLoadActionClear;
      pass.colorAttachments[0].storeAction = MTLStoreActionStore;
      pass.colorAttachments[0].clearColor =
          MTLClearColorMake(0.0, 0.0, 0.0, 0.0);
      id<MTLRenderCommandEncoder> encoder =
          [commandBuffer renderCommandEncoderWithDescriptor:pass];
      if (encoder == nil) {
        error = "Qt packed-alpha render encoder creation failed";
        releaseResources();
        return false;
      }
      encoder.label = @"AmazingEngine VideoAnimSeq alpha merge";
      const MTLViewport viewport{0.0,
                                 static_cast<double>(validated.outputHeight),
                                 static_cast<double>(validated.outputWidth),
                                 -static_cast<double>(validated.outputHeight),
                                 0.0,
                                 1.0};
      [encoder setViewport:viewport];
      [encoder setCullMode:MTLCullModeNone];
      [encoder setRenderPipelineState:state_->pipeline];
      [encoder setVertexBuffer:state_->vertexBuffer offset:0U atIndex:0U];

      const float yChannelStride =
          static_cast<float>(validated.planes[0].view.row_stride);
      const float yWidth = static_cast<float>(validated.planes[0].visibleWidth);
      const float uvChannelStride =
          static_cast<float>(validated.planes[1].view.row_stride);
      const float uvWidth =
          static_cast<float>(validated.planes[1].visibleWidth);
      const MetalFloat3x3 colorConversionMatrix{{
          {1.164F, 1.164F, 1.164F, 0.0F},
          {0.0F, -0.392F, 2.017F, 0.0F},
          {1.596F, -0.813F, 0.0F, 0.0F},
      }};
      [encoder setFragmentBytes:&yChannelStride
                         length:sizeof(yChannelStride)
                        atIndex:0U];
      [encoder setFragmentBytes:&yWidth length:sizeof(yWidth) atIndex:1U];
      [encoder setFragmentBytes:&uvChannelStride
                         length:sizeof(uvChannelStride)
                        atIndex:2U];
      [encoder setFragmentBytes:&uvWidth length:sizeof(uvWidth) atIndex:3U];
      [encoder setFragmentBytes:&colorConversionMatrix
                         length:sizeof(colorConversionMatrix)
                        atIndex:4U];
      for (NSUInteger index = 0U; index < planeTextures.size(); ++index) {
        [encoder setFragmentTexture:planeTextures[index] atIndex:index];
        [encoder setFragmentSamplerState:state_->sampler atIndex:index];
      }
      [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                          indexCount:6U
                           indexType:MTLIndexTypeUInt16
                         indexBuffer:state_->indexBuffer
                   indexBufferOffset:0U
                       instanceCount:1U];
      [encoder endEncoding];

      id<MTLBlitCommandEncoder> readbackEncoder =
          [commandBuffer blitCommandEncoder];
      if (readbackEncoder == nil) {
        error = "Qt packed-alpha readback encoder creation failed";
        releaseResources();
        return false;
      }
      CopyTextureToBuffer(
          readbackEncoder, outputTexture, readbackBuffer,
          MTLSizeMake(static_cast<NSUInteger>(validated.outputWidth),
                      static_cast<NSUInteger>(validated.outputHeight), 1U),
          static_cast<NSUInteger>(validated.readbackRowBytes),
          static_cast<NSUInteger>(validated.readbackBytes));
      [readbackEncoder endEncoding];

      [commandBuffer commit];
      [commandBuffer waitUntilCompleted];
      if (commandBuffer.status != MTLCommandBufferStatusCompleted) {
        error = MetalErrorMessage(@"Qt packed-alpha Metal command failed",
                                  commandBuffer.error);
        releaseResources();
        return false;
      }

      try {
        output.pixels.resize(validated.compactOutputBytes);
      } catch (const std::bad_alloc &) {
        error = "Qt packed-alpha compact output allocation failed";
        output.Clear();
        releaseResources();
        return false;
      }
      const auto *readback =
          static_cast<const std::uint8_t *>(readbackBuffer.contents);
      // CPU YUV planes are uploaded top-left first. With the captured
      // originY=H,height=-H viewport, Metal row zero already corresponds to
      // source row zero. Preserve that order while removing readback padding;
      // an additional flip here would invert the public top-left ABI.
      for (std::uint32_t destinationRow = 0U;
           destinationRow < validated.outputHeight; ++destinationRow) {
        std::memcpy(output.pixels.data() +
                        static_cast<std::size_t>(destinationRow) *
                            validated.compactOutputRowBytes,
                    readback + static_cast<std::size_t>(destinationRow) *
                                   validated.readbackRowBytes,
                    validated.compactOutputRowBytes);
      }
      output.width = validated.outputWidth;
      output.height = validated.outputHeight;

      releaseResources();
      error.clear();
      return true;
    }
  }

  const char *BackendName() const noexcept override {
    return "qt-packed-alpha-yuv420p-metal-associated-v1";
  }

private:
  AppleQtPackedAlphaYuvMergeRuntime(
      std::shared_ptr<gpu_execution::Device> executionDevice,
      std::unique_ptr<PackedAlphaMetalState> state)
      : executionDevice_(std::move(executionDevice)), state_(std::move(state)) {
  }

  std::shared_ptr<gpu_execution::Device> executionDevice_;
  std::unique_ptr<PackedAlphaMetalState> state_;
};

} // namespace

std::unique_ptr<QtPackedAlphaYuvMergeRuntime>
CreateQtPackedAlphaYuvMergeRuntime(std::string &error) {
  @autoreleasepool {
    return AppleQtPackedAlphaYuvMergeRuntime::Create(error);
  }
}

} // namespace videocut::skia_runtime::internal
