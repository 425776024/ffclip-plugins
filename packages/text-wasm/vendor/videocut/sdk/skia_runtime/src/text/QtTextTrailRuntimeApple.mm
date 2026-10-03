#include "runtime/MetalRuntimeSupport.h"
#include "text/NativeMetalSubmission.h"
#include "text/shaders/MetalQtTextTrail.h"

#include "text/QtTextTrailRuntime.h"

#include "videocut/gpu_execution/GpuExecution.h"

#import <CoreGraphics/CoreGraphics.h>
#import <ImageIO/ImageIO.h>
#import <Metal/Metal.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <memory>
#include <optional>
#include <utility>

namespace videocut::skia_runtime::internal {
namespace {

constexpr const char *kQtTextTrailMetalSource =
    shader_sources::MetalQtTextTrail;
#include "text/QtTextTrailPattern0Jpeg.inc"

constexpr int kPatternWidth = 204;
constexpr int kPatternHeight = 4;
constexpr std::size_t kMaximumTrailPixels = 16U * 1024U * 1024U;

struct TrailVertex final {
  float positionX;
  float positionY;
  float positionZ;
  float positionW;
  float uvX;
  float uvY;
};

struct alignas(8) TrailBlurUniforms final {
  float sigma;
  std::int32_t samples;
  float stepValue;
  float alignmentPadding;
  float direction[2];
};

struct alignas(16) TrailTimeUniforms final {
  float passPreviousHistory;
  float decayStep;
  float alignmentPadding0[2];
  float screenParams[4];
  float authoredBlur;
  float alignmentPadding1[3];
};

struct alignas(16) TrailHintUniforms final {
  float hintOffset;
  float hintMin;
  float hintMax;
  float hintHue;
  std::int32_t baseEnabled;
  std::uint32_t alignmentPadding[3];
  float baseHint[4];
};

constexpr TrailVertex kFullscreenQuad[] = {
    {-1.0F, -1.0F, 0.0F, 1.0F, 0.0F, 0.0F},
    {1.0F, -1.0F, 0.0F, 1.0F, 1.0F, 0.0F},
    {1.0F, 1.0F, 0.0F, 1.0F, 1.0F, 1.0F},
    {-1.0F, 1.0F, 0.0F, 1.0F, 0.0F, 1.0F},
};
// The captured Qt command stream uses MTLIndexTypeUInt32 (Metal value 1) and
// this exact two-triangle ordering for every Trail pass.
constexpr std::uint32_t kFullscreenIndices[] = {0U, 1U, 2U, 0U, 2U, 3U};

static_assert(sizeof(TrailVertex) == 24U);
static_assert(sizeof(TrailBlurUniforms) == 24U);
static_assert(sizeof(TrailTimeUniforms) == 48U);
static_assert(sizeof(TrailHintUniforms) == 48U);
static_assert(static_cast<std::uint32_t>(MTLPrimitiveTypeTriangle) == 3U);
static_assert(static_cast<std::uint32_t>(MTLIndexTypeUInt32) == 1U);
static_assert(static_cast<std::uint32_t>(MTLLoadActionLoad) == 1U);
static_assert(static_cast<std::uint32_t>(MTLStoreActionStore) == 1U);
static_assert(static_cast<std::uint32_t>(MTLColorWriteMaskAll) == 15U);

bool SameIdentity(const QtTextTrailStateIdentity &left,
                  const QtTextTrailStateIdentity &right) {
  return left.renderGraphInstanceId == right.renderGraphInstanceId &&
         left.effectNodeInstanceId == right.effectNodeInstanceId &&
         left.lifecycleEpoch == right.lifecycleEpoch &&
         left.implementationId == right.implementationId &&
         left.implementationVersion == right.implementationVersion &&
         left.stateSchemaVersion == right.stateSchemaVersion &&
         left.sourceContractDigest == right.sourceContractDigest;
}

bool ValidateIdentity(const QtTextTrailStateIdentity &identity,
                      std::string &error) {
  if (identity.implementationId != kQtTextTrailImplementationId) {
    error = "Qt Trail implementation id is unsupported";
    return false;
  }
  if (identity.implementationVersion != kQtTextTrailImplementationVersion) {
    error = "Qt Trail implementation version is unsupported";
    return false;
  }
  if (identity.stateSchemaVersion != kQtTextTrailStateSchemaVersion) {
    error = "Qt Trail state schema version is unsupported";
    return false;
  }
  if (identity.sourceContractDigest != kQtTextTrailSourceContractDigest) {
    error = "Qt Trail source contract digest is unsupported";
    return false;
  }
  if (identity.renderGraphInstanceId == 0U ||
      identity.effectNodeInstanceId == 0U) {
    error = "Qt Trail state identity is incomplete";
    return false;
  }
  return true;
}

bool ValidateImageView(const QtTextTrailRawRgba8ImageView &image,
                       std::string &error) {
  if ((!image.nativeTexture && image.pixels == nullptr) ||
      image.width <= 0 || image.height <= 0) {
    error = "Qt Trail Page image is empty";
    return false;
  }
  const auto width = static_cast<std::size_t>(image.width);
  const auto height = static_cast<std::size_t>(image.height);
  if (width > std::numeric_limits<std::size_t>::max() / 4U ||
      width * height > kMaximumTrailPixels) {
    error = "Qt Trail Page dimensions exceed the v1 boundary";
    return false;
  }
  if (image.nativeTexture)
    return true;
  const std::size_t compactRowBytes = width * 4U;
  if (image.rowBytes < compactRowBytes ||
      height > std::numeric_limits<std::size_t>::max() / image.rowBytes) {
    error = "Qt Trail Page rowBytes is invalid";
    return false;
  }
  const std::size_t required = (height - 1U) * image.rowBytes + compactRowBytes;
  if (image.byteSize < required) {
    error = "Qt Trail Page byteSize is smaller than its final row";
    return false;
  }
  return true;
}

bool ValidateParameters(const QtTextTrailParameters &parameters,
                        std::string &error) {
  const bool finite =
      std::isfinite(parameters.blur) && std::isfinite(parameters.weaken) &&
      std::isfinite(parameters.hintHue) &&
      std::isfinite(parameters.hintOffset) &&
      std::isfinite(parameters.hintMin) && std::isfinite(parameters.hintMax) &&
      std::all_of(parameters.baseHint.begin(), parameters.baseHint.end(),
                  [](const float value) { return std::isfinite(value); });
  if (!finite) {
    error = "Qt Trail parameters contain a non-finite value";
    return false;
  }
  if (parameters.blur < 0.0F || parameters.blur > 1.0F ||
      parameters.weaken < 0.0F || parameters.weaken > 1.0F ||
      parameters.hintMin < 0.0F || parameters.hintMin > 1.0F ||
      parameters.hintMax < 0.0F || parameters.hintMax > 1.0F ||
      parameters.hintProfile != 0) {
    error = "Qt Trail parameters are outside the audited pattern0 v1 branch";
    return false;
  }
  return true;
}

bool DecodePattern0(std::vector<std::uint8_t> &pixels, std::string &error) {
  CFDataRef data = CFDataCreate(kCFAllocatorDefault, kQtTextTrailPattern0Jpeg,
                                sizeof(kQtTextTrailPattern0Jpeg));
  if (data == nullptr) {
    error = "Qt Trail pattern0 CFData creation failed";
    return false;
  }
  CGImageSourceRef source = CGImageSourceCreateWithData(data, nullptr);
  CFRelease(data);
  if (source == nullptr) {
    error = "Qt Trail pattern0 JPEG decoder creation failed";
    return false;
  }
  CGImageRef image = CGImageSourceCreateImageAtIndex(source, 0U, nullptr);
  CFRelease(source);
  if (image == nullptr || CGImageGetWidth(image) != kPatternWidth ||
      CGImageGetHeight(image) != kPatternHeight) {
    if (image != nullptr)
      CGImageRelease(image);
    error = "Qt Trail pattern0 JPEG dimensions are not 204x4";
    return false;
  }
  pixels.assign(static_cast<std::size_t>(kPatternWidth) * kPatternHeight * 4U,
                0U);
  CGColorSpaceRef colorSpace = CGColorSpaceCreateDeviceRGB();
  CGContextRef context = CGBitmapContextCreate(
      pixels.data(), kPatternWidth, kPatternHeight, 8U,
      static_cast<std::size_t>(kPatternWidth) * 4U, colorSpace,
      static_cast<CGBitmapInfo>(kCGImageAlphaPremultipliedLast) |
          static_cast<CGBitmapInfo>(kCGBitmapByteOrder32Big));
  CGColorSpaceRelease(colorSpace);
  if (context == nullptr) {
    CGImageRelease(image);
    error = "Qt Trail pattern0 RGBA decoder surface creation failed";
    return false;
  }
  CGContextSetBlendMode(context, kCGBlendModeCopy);
  CGContextTranslateCTM(context, 0.0, static_cast<CGFloat>(kPatternHeight));
  CGContextScaleCTM(context, 1.0, -1.0);
  CGContextDrawImage(
      context, CGRectMake(0.0, 0.0, kPatternWidth, kPatternHeight), image);
  CGContextRelease(context);
  CGImageRelease(image);
  return true;
}

NSUInteger AlignedRowBytes(id<MTLDevice> device, const MTLPixelFormat format,
                           const int width) {
  const NSUInteger pixelBytes = format == MTLPixelFormatRGBA32Float ? 16U : 4U;
  const NSUInteger compact = static_cast<NSUInteger>(width) * pixelBytes;
  const NSUInteger alignment = std::max<NSUInteger>(
      1U, [device minimumLinearTextureAlignmentForPixelFormat:format]);
  return ((compact + alignment - 1U) / alignment) * alignment;
}

id<MTLTexture> MakePrivateTexture(id<MTLDevice> device,
                                  const MTLPixelFormat format, const int width,
                                  const int height, const MTLTextureUsage usage,
                                  NSString *label) {
  id<MTLTexture> texture =
      MakePrivateTexture2D(device, format, width, height, usage);
  texture.label = label;
  return texture;
}

bool UploadTopLeftRgba8(id<MTLDevice> device, id<MTLCommandQueue> queue,
                        id<MTLTexture> texture,
                        const std::uint8_t *sourcePixels,
                        const std::size_t sourceRowBytes, const int width,
                        const int height, std::string &error) {
  const NSUInteger rowBytes =
      AlignedRowBytes(device, MTLPixelFormatRGBA8Unorm, width);
  const NSUInteger length = rowBytes * static_cast<NSUInteger>(height);
  id<MTLBuffer> upload =
      [device newBufferWithLength:length options:MTLResourceStorageModeShared];
  if (upload == nil) {
    error = "Qt Trail RGBA8 upload buffer allocation failed";
    return false;
  }
  const std::size_t compactRowBytes = static_cast<std::size_t>(width) * 4U;
  auto *destination = static_cast<std::uint8_t *>(upload.contents);
  CopyVerticallyFlippedRows(destination, static_cast<std::size_t>(rowBytes),
                            sourcePixels, sourceRowBytes, compactRowBytes,
                            height);
  id<MTLCommandBuffer> commandBuffer = [queue commandBuffer];
  id<MTLBlitCommandEncoder> blit = [commandBuffer blitCommandEncoder];
  if (commandBuffer == nil || blit == nil) {
    [upload release];
    error = "Qt Trail RGBA8 upload encoder creation failed";
    return false;
  }
  CopyBufferToTexture(blit, upload, texture,
                      MTLSizeMake(static_cast<NSUInteger>(width),
                                  static_cast<NSUInteger>(height), 1U),
                      rowBytes, length);
  [blit endEncoding];
  [commandBuffer commit];
  [commandBuffer waitUntilCompleted];
  const bool completed =
      commandBuffer.status == MTLCommandBufferStatusCompleted;
  if (!completed)
    error =
        MetalErrorMessage(@"Qt Trail RGBA8 upload failed", commandBuffer.error);
  [upload release];
  return completed;
}

id<MTLRenderPipelineState>
MakePipeline(id<MTLDevice> device, id<MTLLibrary> library, NSString *vertexName,
             NSString *fragmentName, const MTLPixelFormat outputFormat,
             NSString *label, std::string &error) {
  id<MTLFunction> vertex = [library newFunctionWithName:vertexName];
  id<MTLFunction> fragment = [library newFunctionWithName:fragmentName];
  if (vertex == nil || fragment == nil) {
    [fragment release];
    [vertex release];
    error = "Qt Trail Metal shader entry point is unavailable";
    return nil;
  }
  auto *descriptor = [[MTLRenderPipelineDescriptor alloc] init];
  descriptor.label = label;
  descriptor.vertexFunction = vertex;
  descriptor.fragmentFunction = fragment;
  descriptor.rasterSampleCount = 1U;
  auto *attachment = descriptor.colorAttachments[0];
  attachment.pixelFormat = outputFormat;
  attachment.blendingEnabled = NO;
  attachment.writeMask = MTLColorWriteMaskAll;
  NSError *pipelineError = nil;
  id<MTLRenderPipelineState> pipeline =
      [device newRenderPipelineStateWithDescriptor:descriptor
                                             error:&pipelineError];
  if (pipeline == nil)
    error = MetalErrorMessage(@"Qt Trail Metal pipeline creation failed",
                              pipelineError);
  [descriptor release];
  [fragment release];
  [vertex release];
  return pipeline;
}

struct TrailMetalState final {
  id<MTLCommandQueue> queue{nil};
  id<MTLRenderPipelineState> blurPipeline{nil};
  id<MTLRenderPipelineState> timePipeline{nil};
  id<MTLRenderPipelineState> passPipeline{nil};
  id<MTLRenderPipelineState> hintPipeline{nil};
  id<MTLSamplerState> sampler{nil};
  id<MTLBuffer> vertexBuffer{nil};
  id<MTLBuffer> indexBuffer{nil};
  id<MTLTexture> pattern0{nil};

  id<MTLTexture> page{nil};
  id<MTLTexture> blurX{nil};
  id<MTLTexture> blurY{nil};
  id<MTLTexture> timeA{nil};
  id<MTLTexture> timeB{nil};
  id<MTLTexture> output{nil};
  int width{0};
  int height{0};

  ~TrailMetalState() {
    ReleaseDynamicTextures();
    [pattern0 release];
    [indexBuffer release];
    [vertexBuffer release];
    [sampler release];
    [hintPipeline release];
    [passPipeline release];
    [timePipeline release];
    [blurPipeline release];
    [queue release];
  }

  void ReleaseDynamicTextures() {
    [output release];
    [timeB release];
    [timeA release];
    [blurY release];
    [blurX release];
    [page release];
    output = nil;
    timeB = nil;
    timeA = nil;
    blurY = nil;
    blurX = nil;
    page = nil;
    width = 0;
    height = 0;
  }
};

std::unique_ptr<TrailMetalState> CreateMetalState(id<MTLDevice> device,
                                                  std::string &error) {
  auto state = std::make_unique<TrailMetalState>();
  state->queue = [device newCommandQueue];
  if (state->queue == nil) {
    error = "Qt Trail Metal command queue creation failed";
    return {};
  }
  NSError *libraryError = nil;
  id<MTLLibrary> library = [device
      newLibraryWithSource:[NSString
                               stringWithUTF8String:kQtTextTrailMetalSource]
                   options:nil
                     error:&libraryError];
  if (library == nil) {
    error = MetalErrorMessage(@"Qt Trail Metal shader compilation failed",
                              libraryError);
    return {};
  }
  state->blurPipeline = MakePipeline(
      device, library, @"qtTextTrailVertex", @"qtTextTrailBlurFragment",
      MTLPixelFormatRGBA8Unorm, @"LumiTrail Blur v1", error);
  if (state->blurPipeline != nil) {
    state->timePipeline = MakePipeline(
        device, library, @"qtTextTrailTimeVertex", @"qtTextTrailTimeFragment",
        MTLPixelFormatRGBA32Float, @"LumiTrail Time v1", error);
  }
  if (state->timePipeline != nil) {
    state->passPipeline = MakePipeline(
        device, library, @"qtTextTrailVertex", @"qtTextTrailPassFragment",
        MTLPixelFormatRGBA32Float, @"LumiTrail Pass v1", error);
  }
  if (state->passPipeline != nil) {
    state->hintPipeline = MakePipeline(
        device, library, @"qtTextTrailVertex", @"qtTextTrailHintFragment",
        MTLPixelFormatRGBA8Unorm, @"LumiTrail Hint v1", error);
  }
  [library release];
  if (state->blurPipeline == nil || state->timePipeline == nil ||
      state->passPipeline == nil || state->hintPipeline == nil) {
    return {};
  }

  state->sampler =
      MakeNormalizedClampSampler(device, MTLSamplerMinMagFilterLinear,
                                 @"LumiTrail normalized linear clamp v1");
  state->vertexBuffer =
      [device newBufferWithBytes:kFullscreenQuad
                          length:sizeof(kFullscreenQuad)
                         options:MTLResourceStorageModeShared];
  state->indexBuffer = [device newBufferWithBytes:kFullscreenIndices
                                           length:sizeof(kFullscreenIndices)
                                          options:MTLResourceStorageModeShared];
  state->pattern0 = MakePrivateTexture(
      device, MTLPixelFormatRGBA8Unorm, kPatternWidth, kPatternHeight,
      MTLTextureUsageShaderRead, @"LumiTrail pattern0 v1");
  if (state->sampler == nil || state->vertexBuffer == nil ||
      state->indexBuffer == nil || state->pattern0 == nil) {
    error = "Qt Trail immutable Metal resource creation failed";
    return {};
  }
  std::vector<std::uint8_t> patternPixels;
  if (!DecodePattern0(patternPixels, error) ||
      !UploadTopLeftRgba8(device, state->queue, state->pattern0,
                          patternPixels.data(), kPatternWidth * 4U,
                          kPatternWidth, kPatternHeight, error)) {
    return {};
  }
  error.clear();
  return state;
}

bool EncodeFullscreenPass(id<MTLCommandBuffer> commandBuffer,
                          TrailMetalState &state,
                          id<MTLRenderPipelineState> pipeline,
                          id<MTLTexture> target, NSString *label,
                          const void *uniformBytes,
                          const NSUInteger uniformLength,
                          const std::array<id<MTLTexture>, 3> &textures,
                          const NSUInteger textureCount, std::string &error) {
  auto *descriptor = [MTLRenderPassDescriptor renderPassDescriptor];
  descriptor.colorAttachments[0].texture = target;
  descriptor.colorAttachments[0].loadAction = MTLLoadActionLoad;
  descriptor.colorAttachments[0].storeAction = MTLStoreActionStore;
  id<MTLRenderCommandEncoder> encoder =
      [commandBuffer renderCommandEncoderWithDescriptor:descriptor];
  if (encoder == nil) {
    error = "Qt Trail render encoder creation failed";
    return false;
  }
  encoder.label = label;
  const MTLViewport viewport{0.0,
                             static_cast<double>(state.height),
                             static_cast<double>(state.width),
                             -static_cast<double>(state.height),
                             0.0,
                             1.0};
  [encoder setViewport:viewport];
  const MTLScissorRect scissor{0U, 0U, static_cast<NSUInteger>(state.width),
                               static_cast<NSUInteger>(state.height)};
  [encoder setScissorRect:scissor];
  [encoder setCullMode:MTLCullModeNone];
  [encoder setFrontFacingWinding:MTLWindingClockwise];
  [encoder setRenderPipelineState:pipeline];
  [encoder setVertexBuffer:state.vertexBuffer offset:0U atIndex:0U];
  if (uniformBytes != nullptr && uniformLength > 0U)
    [encoder setFragmentBytes:uniformBytes length:uniformLength atIndex:0U];
  for (NSUInteger index = 0U; index < textureCount; ++index) {
    [encoder setFragmentTexture:textures[index] atIndex:index];
    [encoder setFragmentSamplerState:state.sampler atIndex:index];
  }
  [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                      indexCount:6U
                       indexType:MTLIndexTypeUInt32
                     indexBuffer:state.indexBuffer
               indexBufferOffset:0U
                   instanceCount:1U];
  [encoder endEncoding];
  return true;
}

bool IdentityMatchesCheckpoint(const QtTextTrailStateIdentity &expected,
                               const QtTextTrailCheckpoint &checkpoint) {
  return SameIdentity(expected, checkpoint.identity) &&
         checkpoint.implementationVersion ==
             kQtTextTrailImplementationVersion &&
         checkpoint.stateSchemaVersion == kQtTextTrailStateSchemaVersion &&
         checkpoint.sourceContractDigest == kQtTextTrailSourceContractDigest &&
         checkpoint.width > 0 && checkpoint.height > 0 &&
         checkpoint.committedSampleOrdinal > 0U &&
         checkpoint.committedTimeB.size() ==
             static_cast<std::size_t>(checkpoint.width) *
                 static_cast<std::size_t>(checkpoint.height) * 4U &&
         std::all_of(checkpoint.committedTimeB.begin(),
                     checkpoint.committedTimeB.end(), [](const float value) {
                       return std::isfinite(value) && value >= 0.0F &&
                              value <= 1.0F;
                     });
}

class AppleQtTextTrailRuntime final : public QtTextTrailRuntime {
public:
  static std::unique_ptr<AppleQtTextTrailRuntime> Create(std::string &error) {
    gpu_execution::DeviceRequest request;
    request.preferred = gpu_execution::Backend::Metal;
    auto executionDevice = gpu_execution::Device::Create(request, error);
    if (!executionDevice) {
      if (error.empty())
        error = "product Metal device is unavailable for Qt Trail";
      return {};
    }
    if (executionDevice->backend() != gpu_execution::Backend::Metal ||
        executionDevice->nativeDeviceHandle() == 0U) {
      error = "product GPU device is not a usable Metal device for Qt Trail";
      return {};
    }
    id<MTLDevice> device = (__bridge id<MTLDevice>)(reinterpret_cast<void *>(
        executionDevice->nativeDeviceHandle()));
    if (device == nil) {
      error = "product Metal native handle is unavailable for Qt Trail";
      return {};
    }
    auto state = CreateMetalState(device, error);
    if (!state)
      return {};
    error.clear();
    return std::unique_ptr<AppleQtTextTrailRuntime>(new AppleQtTextTrailRuntime(
        std::move(executionDevice), std::move(state)));
  }

  bool Render(const QtTextTrailRenderRequest &request,
              QtTextTrailRenderResult &result, std::string &error) override {
    @autoreleasepool {
      result = {};
      if (!ValidateIdentity(request.identity, error)) {
        QtTextTrailStateIdentity normalized = request.identity;
        normalized.implementationId = kQtTextTrailImplementationId;
        normalized.implementationVersion = kQtTextTrailImplementationVersion;
        normalized.stateSchemaVersion = kQtTextTrailStateSchemaVersion;
        normalized.sourceContractDigest = kQtTextTrailSourceContractDigest;
        Invalidate(QtTextTrailResetReason::IncompatibleVersion, normalized);
        return false;
      }
      if (request.directionContract !=
          QtTextTrailDirectionContract::TopLeftRowsNegativeHeightViewport) {
        error = "Qt Trail direction contract is unsupported";
        return false;
      }
      if (!ValidateImageView(request.source, error) ||
          !ValidateParameters(request.parameters, error)) {
        return false;
      }
      id<MTLDevice> device = Device();
      if (device == nil || state_ == nullptr) {
        error = "Qt Trail Metal runtime lost its product device";
        return false;
      }
      const bool native = request.source.nativeTexture ||
                          request.target.texture || request.target.commandQueue;
      if (native) {
        id<MTLTexture> input = (__bridge id<MTLTexture>)request.source.nativeTexture;
        id<MTLTexture> output = (__bridge id<MTLTexture>)request.target.texture;
        id<MTLCommandQueue> queue =
            (__bridge id<MTLCommandQueue>)request.target.commandQueue;
        const auto validTexture = [&](id<MTLTexture> texture,
                                      const MTLTextureUsage usage) {
          return texture != nil && texture.device == device &&
                 texture.textureType == MTLTextureType2D &&
                 texture.pixelFormat == MTLPixelFormatRGBA8Unorm &&
                 texture.sampleCount == 1U &&
                 texture.width == static_cast<NSUInteger>(request.source.width) &&
                 texture.height == static_cast<NSUInteger>(request.source.height) &&
                 (texture.usage & usage) == usage;
        };
        if (!validTexture(input, MTLTextureUsageShaderRead) ||
            !validTexture(output, MTLTextureUsageRenderTarget) ||
            input == output || queue == nil || queue.device != device) {
          error = "Qt Trail native source, target or queue violates the RGBA8 contract";
          return false;
        }
        if (state_->queue != queue) {
          // Pattern upload is already complete at construction. A resident
          // history needs a barrier only when moving to a different queue;
          // sequential rendering and checkpoints use the retained queue.
          if (committedSampleOrdinal_ > 0U) {
            id<MTLCommandBuffer> barrier = [state_->queue commandBuffer];
            if (barrier == nil) {
              error = "Qt Trail queue handoff barrier creation failed";
              return false;
            }
            [barrier commit];
            [barrier waitUntilCompleted];
            if (barrier.status != MTLCommandBufferStatusCompleted) {
              error = MetalErrorMessage(@"Qt Trail queue handoff failed", barrier.error);
              return false;
            }
          }
          [queue retain];
          [state_->queue release];
          state_->queue = queue;
        }
      }

      QtTextTrailResetReason resetReason = QtTextTrailResetReason::None;
      if (!activeIdentity_ ||
          !SameIdentity(*activeIdentity_, request.identity)) {
        if (state_->width > 0)
          state_->ReleaseDynamicTextures();
        committedSampleOrdinal_ = 0U;
        if (pendingIdentity_ &&
            SameIdentity(*pendingIdentity_, request.identity)) {
          resetReason = pendingResetReason_;
        } else {
          resetReason = QtTextTrailResetReason::NewLifecycle;
        }
        activeIdentity_ = request.identity;
        pendingIdentity_.reset();
        pendingResetReason_ = QtTextTrailResetReason::None;
      }
      if (state_->width > 0 && (state_->width != request.source.width ||
                                state_->height != request.source.height)) {
        state_->ReleaseDynamicTextures();
        committedSampleOrdinal_ = 0U;
        resetReason = QtTextTrailResetReason::DimensionsChanged;
      }
      const bool needsAllocation = state_->width == 0;
      if (needsAllocation &&
          !AllocateDynamicTextures(request.source.width, request.source.height,
                                   error, request.target.submit)) {
        Invalidate(QtTextTrailResetReason::NewLifecycle, request.identity);
        return false;
      }
      if (needsAllocation && resetReason == QtTextTrailResetReason::None)
        resetReason = QtTextTrailResetReason::NewLifecycle;

      if (!ExecuteSample(request, committedSampleOrdinal_ > 0U, result,
                         error)) {
        Invalidate(resetReason == QtTextTrailResetReason::None
                       ? QtTextTrailResetReason::NewLifecycle
                       : resetReason,
                   request.identity);
        return false;
      }
      ++committedSampleOrdinal_;
      result.width = state_->width;
      result.height = state_->height;
      result.resourceGeneration = resourceGeneration_;
      result.committedSampleOrdinal = committedSampleOrdinal_;
      result.resetReason = resetReason;
      result.passes = {
          {"BlurX", QtTextTrailPixelFormat::Rgba8Unorm, false, true, true},
          {"BlurY", QtTextTrailPixelFormat::Rgba8Unorm, false, true, true},
          {"Time", QtTextTrailPixelFormat::Rgba32Float, false, true, true},
          {"Pass", QtTextTrailPixelFormat::Rgba32Float, false, true, true},
          {"Hint", QtTextTrailPixelFormat::Rgba8Unorm, false, true, true},
      };
      error.clear();
      return true;
    }
  }

  bool SaveCheckpoint(QtTextTrailCheckpoint &checkpoint,
                      std::string &error) const override {
    @autoreleasepool {
      checkpoint = {};
      if (!activeIdentity_ || state_ == nullptr || state_->timeB == nil ||
          committedSampleOrdinal_ == 0U) {
        error = "Qt Trail has no committed TimeB checkpoint";
        return false;
      }
      std::vector<float> pixels;
      if (!ReadTimeB(pixels, error))
        return false;
      checkpoint.identity = *activeIdentity_;
      checkpoint.implementationVersion = kQtTextTrailImplementationVersion;
      checkpoint.stateSchemaVersion = kQtTextTrailStateSchemaVersion;
      checkpoint.sourceContractDigest = kQtTextTrailSourceContractDigest;
      checkpoint.width = state_->width;
      checkpoint.height = state_->height;
      checkpoint.committedSampleOrdinal = committedSampleOrdinal_;
      checkpoint.committedTimeB = std::move(pixels);
      error.clear();
      return true;
    }
  }

  bool RestoreCheckpoint(const QtTextTrailStateIdentity &expectedIdentity,
                         const QtTextTrailCheckpoint &checkpoint,
                         std::string &error) override {
    @autoreleasepool {
      if (!ValidateIdentity(expectedIdentity, error) ||
          !IdentityMatchesCheckpoint(expectedIdentity, checkpoint)) {
        if (error.empty())
          error = "Qt Trail checkpoint is incompatible";
        Invalidate(QtTextTrailResetReason::IncompatibleCheckpoint,
                   expectedIdentity);
        return false;
      }
      Invalidate(QtTextTrailResetReason::None, std::nullopt);
      activeIdentity_ = expectedIdentity;
      if (!AllocateDynamicTextures(checkpoint.width, checkpoint.height,
                                   error) ||
          !UploadTimeB(checkpoint.committedTimeB, error)) {
        Invalidate(QtTextTrailResetReason::IncompatibleCheckpoint,
                   expectedIdentity);
        return false;
      }
      committedSampleOrdinal_ = checkpoint.committedSampleOrdinal;
      pendingIdentity_.reset();
      pendingResetReason_ = QtTextTrailResetReason::None;
      error.clear();
      return true;
    }
  }

  bool ResetForSequentialReplay(const QtTextTrailStateIdentity &identity,
                                std::string &error) override {
    if (!ValidateIdentity(identity, error)) {
      Invalidate(QtTextTrailResetReason::IncompatibleVersion, identity);
      return false;
    }
    Invalidate(QtTextTrailResetReason::SequentialReplay, identity);
    error.clear();
    return true;
  }

  const char *BackendName() const noexcept override {
    return "qt-lumi-trail-metal-rgba32f-history-v1";
  }

private:
  AppleQtTextTrailRuntime(
      std::shared_ptr<gpu_execution::Device> executionDevice,
      std::unique_ptr<TrailMetalState> state)
      : executionDevice_(std::move(executionDevice)), state_(std::move(state)) {
  }

  id<MTLDevice> Device() const {
    return executionDevice_ ? (__bridge id<MTLDevice>)(reinterpret_cast<void *>(
                                  executionDevice_->nativeDeviceHandle()))
                            : nil;
  }

  void Invalidate(const QtTextTrailResetReason reason,
                  std::optional<QtTextTrailStateIdentity> pendingIdentity) {
    if (state_)
      state_->ReleaseDynamicTextures();
    activeIdentity_.reset();
    committedSampleOrdinal_ = 0U;
    pendingIdentity_ = std::move(pendingIdentity);
    pendingResetReason_ = reason;
  }

  bool AllocateDynamicTextures(const int width, const int height,
                               std::string &error, const NativeCommandSubmission &submit = {}) {
    id<MTLDevice> device = Device();
    if (device == nil || state_ == nullptr || width <= 0 || height <= 0) {
      error = "Qt Trail dynamic texture request is invalid";
      return false;
    }
    state_->ReleaseDynamicTextures();
    state_->blurX = MakePrivateTexture(
        device, MTLPixelFormatRGBA8Unorm, width, height,
        MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead,
        @"LumiTrail BlurX v1");
    state_->blurY = MakePrivateTexture(
        device, MTLPixelFormatRGBA8Unorm, width, height,
        MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead,
        @"LumiTrail BlurY v1");
    state_->timeA = MakePrivateTexture(
        device, MTLPixelFormatRGBA32Float, width, height,
        MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead,
        @"LumiTrail TimeA RGBA32F v1");
    state_->timeB = MakePrivateTexture(
        device, MTLPixelFormatRGBA32Float, width, height,
        MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead,
        @"LumiTrail TimeB RGBA32F v1");
    if (state_->blurX == nil || state_->blurY == nil ||
        state_->timeA == nil || state_->timeB == nil) {
      state_->ReleaseDynamicTextures();
      error = "Qt Trail private render-target allocation failed";
      return false;
    }
    state_->width = width;
    state_->height = height;
    ++resourceGeneration_;

    // Qt suppresses old history with u_pass=0 after resize. Explicitly clear
    // the otherwise undefined private TimeB so multiplying by zero cannot
    // preserve a NaN on a Metal implementation.
    id<MTLCommandBuffer> commandBuffer = [state_->queue commandBuffer];
    auto *descriptor = [MTLRenderPassDescriptor renderPassDescriptor];
    descriptor.colorAttachments[0].texture = state_->timeB;
    descriptor.colorAttachments[0].loadAction = MTLLoadActionClear;
    descriptor.colorAttachments[0].storeAction = MTLStoreActionStore;
    descriptor.colorAttachments[0].clearColor =
        MTLClearColorMake(0.0, 0.0, 0.0, 0.0);
    id<MTLRenderCommandEncoder> encoder =
        [commandBuffer renderCommandEncoderWithDescriptor:descriptor];
    if (commandBuffer == nil || encoder == nil) {
      error = "Qt Trail TimeB initialization encoder creation failed";
      state_->ReleaseDynamicTextures();
      return false;
    }
    [encoder endEncoding];
    if (!SubmitNativeTextureCommand(commandBuffer, submit, {state_->timeB}, error))
      return false;
    // ExecuteSample and explicit checkpoint operations follow on this queue.
    // GPU ordering initializes history without stalling the CPU.
    return true;
  }

  bool ExecuteSample(const QtTextTrailRenderRequest &request,
                     const bool passPreviousHistory,
                     QtTextTrailRenderResult &result, std::string &error) {
    id<MTLDevice> device = Device();
    const int width = state_->width;
    const int height = state_->height;
    const bool native = request.source.nativeTexture != nullptr;
    if (!native) {
      // Pixel entry points are retained for explicit CPU callers. Production
      // texture rendering never allocates these staging render targets.
      if (state_->page == nil)
        state_->page = MakePrivateTexture(
            device, MTLPixelFormatRGBA8Unorm, width, height,
            MTLTextureUsageShaderRead, @"LumiTrail Page v1");
      if (state_->output == nil)
        state_->output = MakePrivateTexture(
            device, MTLPixelFormatRGBA8Unorm, width, height,
            MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead,
            @"LumiTrail Output v1");
      if (state_->page == nil || state_->output == nil) {
        error = "Qt Trail CPU pixel target allocation failed";
        return false;
      }
    }
    id<MTLTexture> page = native
        ? (__bridge id<MTLTexture>)request.source.nativeTexture : state_->page;
    id<MTLTexture> output = native
        ? (__bridge id<MTLTexture>)request.target.texture : state_->output;
    const NSUInteger uploadRowBytes =
        AlignedRowBytes(device, MTLPixelFormatRGBA8Unorm, width);
    const NSUInteger uploadLength =
        uploadRowBytes * static_cast<NSUInteger>(height);
    id<MTLBuffer> upload = native ? nil :
        [device newBufferWithLength:uploadLength
                            options:MTLResourceStorageModeShared];
    id<MTLBuffer> readback = native ? nil :
        [device newBufferWithLength:uploadLength
                            options:MTLResourceStorageModeShared];
    const auto releaseBuffers = [&]() {
      [readback release];
      [upload release];
    };
    if (!native && (upload == nil || readback == nil)) {
      error = "Qt Trail per-sample staging allocation failed";
      releaseBuffers();
      return false;
    }
    const std::size_t compactRowBytes = static_cast<std::size_t>(width) * 4U;
    auto *uploadBytes = static_cast<std::uint8_t *>(upload.contents);
    if (!native) {
      CopyVerticallyFlippedRows(uploadBytes,
                                static_cast<std::size_t>(uploadRowBytes),
                                request.source.pixels, request.source.rowBytes,
                                compactRowBytes, height);
    }

    const float blurPixels = 20.0F * request.parameters.blur;
    const int samples = static_cast<int>(std::ceil(blurPixels));
    TrailBlurUniforms blurX{};
    blurX.samples = samples;
    blurX.stepValue = samples == 0 ? 1.0F / static_cast<float>(width)
                                   : blurPixels / static_cast<float>(samples) /
                                         static_cast<float>(width);
    blurX.sigma = samples == 0 ? 1.0F / static_cast<float>(width)
                               : blurPixels / static_cast<float>(width) * 2.5F;
    blurX.direction[0] = 1.0F;
    blurX.direction[1] = 0.0F;
    TrailBlurUniforms blurY = blurX;
    blurY.stepValue = samples == 0 ? 1.0F / static_cast<float>(height)
                                   : blurPixels / static_cast<float>(samples) /
                                         static_cast<float>(height);
    blurY.sigma = samples == 0 ? 1.0F / static_cast<float>(height)
                               : blurPixels / static_cast<float>(height) * 2.5F;
    blurY.direction[0] = 0.0F;
    blurY.direction[1] = 1.0F;
    TrailTimeUniforms time{};
    time.passPreviousHistory = passPreviousHistory ? 1.0F : 0.0F;
    time.decayStep = 0.3F * request.parameters.weaken;
    time.screenParams[0] = static_cast<float>(width);
    time.screenParams[1] = static_cast<float>(height);
    time.screenParams[2] =
        static_cast<float>(width + 1) / static_cast<float>(width);
    time.screenParams[3] =
        static_cast<float>(height + 1) / static_cast<float>(height);
    time.authoredBlur = request.parameters.blur;
    TrailHintUniforms hint{};
    hint.hintOffset = request.parameters.hintOffset;
    hint.hintMin = request.parameters.hintMin;
    hint.hintMax = request.parameters.hintMax;
    hint.hintHue = request.parameters.hintHue;
    hint.baseEnabled = request.parameters.baseEnabled ? 1 : 0;
    hint.baseHint[0] = request.parameters.baseHint[0];
    hint.baseHint[1] = request.parameters.baseHint[1];
    hint.baseHint[2] = request.parameters.baseHint[2];
    hint.baseHint[3] = 1.0F;

    id<MTLCommandBuffer> commandBuffer = [state_->queue commandBuffer];
    if (commandBuffer == nil) {
      error = "Qt Trail command buffer creation failed";
      releaseBuffers();
      return false;
    }
    commandBuffer.label = @"Qt LumiTrail persistent history v1";
    if (!native) {
      id<MTLBlitCommandEncoder> uploadBlit = [commandBuffer blitCommandEncoder];
      if (uploadBlit == nil) {
        error = "Qt Trail Page upload encoder creation failed";
        releaseBuffers();
        return false;
      }
      CopyBufferToTexture(uploadBlit, upload, state_->page,
                          MTLSizeMake(static_cast<NSUInteger>(width),
                                      static_cast<NSUInteger>(height), 1U),
                          uploadRowBytes, uploadLength);
      [uploadBlit endEncoding];
    }

    if (!EncodeFullscreenPass(commandBuffer, *state_, state_->blurPipeline,
                              state_->blurX, @"LumiTrail BlurX", &blurX,
                              sizeof(blurX), {page, nil, nil}, 1U,
                              error) ||
        !EncodeFullscreenPass(commandBuffer, *state_, state_->blurPipeline,
                              state_->blurY, @"LumiTrail BlurY", &blurY,
                              sizeof(blurY), {state_->blurX, nil, nil}, 1U,
                              error) ||
        !EncodeFullscreenPass(commandBuffer, *state_, state_->timePipeline,
                              state_->timeA, @"LumiTrail Time", &time,
                              sizeof(time), {state_->timeB, state_->blurY, nil},
                              2U, error) ||
        !EncodeFullscreenPass(commandBuffer, *state_, state_->passPipeline,
                              state_->timeB, @"LumiTrail Pass", nullptr, 0U,
                              {state_->timeA, nil, nil}, 1U, error) ||
        !EncodeFullscreenPass(
            commandBuffer, *state_, state_->hintPipeline, output,
            @"LumiTrail Hint", &hint, sizeof(hint),
            {state_->timeB, state_->pattern0, page}, 3U, error)) {
      releaseBuffers();
      return false;
    }

    if (native) {
      if (!SubmitNativeTextureCommand(commandBuffer, request.target.submit,
          {page, output, state_->blurX, state_->blurY, state_->timeA, state_->timeB, state_->pattern0}, error))
        return false;
      error.clear();
      return true;
    }

    id<MTLBlitCommandEncoder> readbackBlit = [commandBuffer blitCommandEncoder];
    if (readbackBlit == nil) {
      error = "Qt Trail output readback encoder creation failed";
      releaseBuffers();
      return false;
    }
    CopyTextureToBuffer(readbackBlit, state_->output, readback,
                        MTLSizeMake(static_cast<NSUInteger>(width),
                                    static_cast<NSUInteger>(height), 1U),
                        uploadRowBytes, uploadLength);
    [readbackBlit endEncoding];
    [commandBuffer commit];
    [commandBuffer waitUntilCompleted];
    if (commandBuffer.status != MTLCommandBufferStatusCompleted) {
      error = MetalErrorMessage(@"Qt Trail Metal command failed",
                                commandBuffer.error);
      releaseBuffers();
      return false;
    }

    result.outputPixels.resize(compactRowBytes *
                               static_cast<std::size_t>(height));
    const auto *readbackBytes =
        static_cast<const std::uint8_t *>(readback.contents);
    CopyVerticallyFlippedRows(
        result.outputPixels.data(), compactRowBytes, readbackBytes,
        static_cast<std::size_t>(uploadRowBytes), compactRowBytes, height);
    releaseBuffers();
    return true;
  }

  bool ReadTimeB(std::vector<float> &pixels, std::string &error) const {
    id<MTLDevice> device = Device();
    const NSUInteger rowBytes =
        AlignedRowBytes(device, MTLPixelFormatRGBA32Float, state_->width);
    const NSUInteger length =
        rowBytes * static_cast<NSUInteger>(state_->height);
    id<MTLBuffer> readback =
        [device newBufferWithLength:length
                            options:MTLResourceStorageModeShared];
    if (readback == nil) {
      error = "Qt Trail TimeB checkpoint buffer allocation failed";
      return false;
    }
    id<MTLCommandBuffer> commandBuffer = [state_->queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [commandBuffer blitCommandEncoder];
    if (commandBuffer == nil || blit == nil) {
      [readback release];
      error = "Qt Trail TimeB checkpoint encoder creation failed";
      return false;
    }
    CopyTextureToBuffer(blit, state_->timeB, readback,
                        MTLSizeMake(static_cast<NSUInteger>(state_->width),
                                    static_cast<NSUInteger>(state_->height),
                                    1U),
                        rowBytes, length);
    [blit endEncoding];
    [commandBuffer commit];
    [commandBuffer waitUntilCompleted];
    if (commandBuffer.status != MTLCommandBufferStatusCompleted) {
      error = MetalErrorMessage(@"Qt Trail TimeB checkpoint read failed",
                                commandBuffer.error);
      [readback release];
      return false;
    }
    const std::size_t compactRowFloats =
        static_cast<std::size_t>(state_->width) * 4U;
    pixels.resize(compactRowFloats * static_cast<std::size_t>(state_->height));
    const auto *bytes = static_cast<const std::uint8_t *>(readback.contents);
    CopyVerticallyFlippedRows(pixels.data(), compactRowFloats * sizeof(float),
                              bytes, static_cast<std::size_t>(rowBytes),
                              compactRowFloats * sizeof(float), state_->height);
    [readback release];
    return true;
  }

  bool UploadTimeB(const std::vector<float> &pixels, std::string &error) {
    id<MTLDevice> device = Device();
    const NSUInteger rowBytes =
        AlignedRowBytes(device, MTLPixelFormatRGBA32Float, state_->width);
    const NSUInteger length =
        rowBytes * static_cast<NSUInteger>(state_->height);
    id<MTLBuffer> upload =
        [device newBufferWithLength:length
                            options:MTLResourceStorageModeShared];
    if (upload == nil) {
      error = "Qt Trail TimeB restore buffer allocation failed";
      return false;
    }
    const std::size_t compactRowBytes =
        static_cast<std::size_t>(state_->width) * 4U * sizeof(float);
    auto *bytes = static_cast<std::uint8_t *>(upload.contents);
    CopyVerticallyFlippedRows(
        bytes, static_cast<std::size_t>(rowBytes),
        reinterpret_cast<const std::uint8_t *>(pixels.data()), compactRowBytes,
        compactRowBytes, state_->height);
    id<MTLCommandBuffer> commandBuffer = [state_->queue commandBuffer];
    id<MTLBlitCommandEncoder> blit = [commandBuffer blitCommandEncoder];
    if (commandBuffer == nil || blit == nil) {
      [upload release];
      error = "Qt Trail TimeB restore encoder creation failed";
      return false;
    }
    CopyBufferToTexture(blit, upload, state_->timeB,
                        MTLSizeMake(static_cast<NSUInteger>(state_->width),
                                    static_cast<NSUInteger>(state_->height),
                                    1U),
                        rowBytes, length);
    [blit endEncoding];
    [commandBuffer commit];
    [commandBuffer waitUntilCompleted];
    const bool completed =
        commandBuffer.status == MTLCommandBufferStatusCompleted;
    if (!completed)
      error = MetalErrorMessage(@"Qt Trail TimeB restore failed",
                                commandBuffer.error);
    [upload release];
    return completed;
  }

  std::shared_ptr<gpu_execution::Device> executionDevice_;
  std::unique_ptr<TrailMetalState> state_;
  std::optional<QtTextTrailStateIdentity> activeIdentity_;
  std::optional<QtTextTrailStateIdentity> pendingIdentity_;
  QtTextTrailResetReason pendingResetReason_{
      QtTextTrailResetReason::NewLifecycle};
  std::uint64_t committedSampleOrdinal_{0U};
  std::uint64_t resourceGeneration_{0U};
};

} // namespace

std::unique_ptr<QtTextTrailRuntime>
CreateQtTextTrailRuntime(std::string &error) {
  @autoreleasepool {
    return AppleQtTextTrailRuntime::Create(error);
  }
}

} // namespace videocut::skia_runtime::internal
