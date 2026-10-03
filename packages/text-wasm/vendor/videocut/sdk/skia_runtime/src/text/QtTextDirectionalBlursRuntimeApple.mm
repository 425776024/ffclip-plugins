#include "runtime/MetalRuntimeSupport.h"
#include "text/NativeMetalSubmission.h"
#include "text/shaders/MetalQtTextDirectionalBlurs.h"

#include "text/QtTextDirectionalBlursRuntime.h"

#include "videocut/gpu_execution/GpuExecution.h"

#import <Metal/Metal.h>

#include <algorithm>
#include <array>
#include <limits>
#include <memory>
#include <utility>

namespace videocut::skia_runtime::internal {
namespace {

constexpr const char *kQtTextDirectionalBlursMetalSource =
    shader_sources::MetalQtTextDirectionalBlurs;

struct DirectionalCopyVertex final {
  float positionX;
  float positionY;
  float positionZ;
  float uvX;
  float uvY;
};

struct DirectionalQuadVertex final {
  float positionX;
  float positionY;
  float positionZ;
  float positionW;
  float normalX;
  float normalY;
  float normalZ;
  float uvX;
  float uvY;
};

constexpr DirectionalCopyVertex kFullscreenTriangle[] = {
    {-1.0F, -1.0F, 0.0F, 0.0F, 0.0F},
    {3.0F, -1.0F, 0.0F, 2.0F, 0.0F},
    {-1.0F, 3.0F, 0.0F, 0.0F, 2.0F},
};

// Qt's EngineCopy index buffer is 8 bytes: three UInt16 indices plus one
// zero-filled UInt16 padding word.
constexpr std::uint16_t kFullscreenTriangleIndices[] = {0U, 1U, 2U, 0U};

constexpr DirectionalQuadVertex kFullscreenQuad[] = {
    {-1.0F, -1.0F, 0.0F, 1.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F},
    {1.0F, -1.0F, 0.0F, 1.0F, 0.0F, 0.0F, 1.0F, 1.0F, 0.0F},
    {1.0F, 1.0F, 0.0F, 1.0F, 0.0F, 0.0F, 1.0F, 1.0F, 1.0F},
    {-1.0F, 1.0F, 0.0F, 1.0F, 0.0F, 0.0F, 1.0F, 0.0F, 1.0F},
};

constexpr std::uint32_t kFullscreenIndices[] = {0U, 1U, 2U, 0U, 2U, 3U};

static_assert(sizeof(DirectionalCopyVertex) == 20U);
static_assert(sizeof(DirectionalQuadVertex) == 36U);
static_assert(sizeof(QtTextDirectionalBlursUniforms) == 48U);
static_assert(sizeof(QtTextDirectionalBlursBlendUniforms) == 12U);
static_assert(sizeof(QtTextAlphaOutlineUniforms) == 64U);
static_assert(static_cast<std::uint32_t>(MTLPixelFormatRGBA8Unorm) == 70U);
static_assert(static_cast<std::uint32_t>(MTLStorageModePrivate) == 2U);
static_assert(static_cast<std::uint32_t>(MTLPrimitiveTypeTriangle) == 3U);
static_assert(static_cast<std::uint32_t>(MTLIndexTypeUInt16) == 0U);
static_assert(static_cast<std::uint32_t>(MTLIndexTypeUInt32) == 1U);
static_assert(static_cast<std::uint32_t>(MTLCullModeNone) == 0U);
static_assert(static_cast<std::uint32_t>(MTLCullModeFront) == 1U);
static_assert(static_cast<std::uint32_t>(MTLWindingClockwise) == 0U);
static_assert(static_cast<std::uint32_t>(MTLWindingCounterClockwise) == 1U);
static_assert(static_cast<std::uint32_t>(MTLLoadActionClear) == 2U);
static_assert(static_cast<std::uint32_t>(MTLStoreActionStore) == 1U);
static_assert(static_cast<std::uint32_t>(MTLColorWriteMaskAll) == 15U);

bool ValidateImageView(const QtTextDirectionalBlursRawRgba8ImageView &image,
                       const int pageWidth, const int pageHeight,
                       std::string &error) {
  if ((image.pixels == nullptr && image.nativeTexture == nullptr) || image.width <= 0 || image.height <= 0) {
    error = "Qt DirectionalBlurs Page image is empty";
    return false;
  }
  if (image.width != pageWidth || image.height != pageHeight) {
    error = "Qt DirectionalBlurs Page image and contract dimensions differ";
    return false;
  }
  if (image.nativeTexture) return true;
  const std::size_t width = static_cast<std::size_t>(image.width);
  const std::size_t height = static_cast<std::size_t>(image.height);
  if (width > std::numeric_limits<std::size_t>::max() / 4U) {
    error = "Qt DirectionalBlurs Page row size overflows";
    return false;
  }
  const std::size_t compactRowBytes = width * 4U;
  if (image.rowBytes < compactRowBytes ||
      height > std::numeric_limits<std::size_t>::max() / image.rowBytes) {
    error = "Qt DirectionalBlurs Page rowBytes is invalid";
    return false;
  }
  const std::size_t required = (height - 1U) * image.rowBytes + compactRowBytes;
  if (image.byteSize < required) {
    error = "Qt DirectionalBlurs Page byteSize is smaller than its final row";
    return false;
  }
  return true;
}

bool ValidateImageView(const QtTextDirectionalBlursRenderRequest &request,
                       std::string &error) {
  return ValidateImageView(request.source, request.contract.pageWidth,
                           request.contract.pageHeight, error);
}

bool ValidateImageView(
    const QtTextDirectionalBlurStandaloneRenderRequest &request,
    std::string &error) {
  return ValidateImageView(request.source, request.contract.pageWidth,
                           request.contract.pageHeight, error);
}

id<MTLTexture> MakePrivateRgba8Texture(id<MTLDevice> device, const int width,
                                       const int height, NSString *label) {
  id<MTLTexture> texture = MakePrivateTexture2D(
      device, MTLPixelFormatRGBA8Unorm, width, height,
      MTLTextureUsageShaderRead | MTLTextureUsageRenderTarget);
  texture.label = label;
  return texture;
}

id<MTLRenderPipelineState>
MakePipeline(id<MTLDevice> device, id<MTLLibrary> library, NSString *vertexName,
             NSString *fragmentName, NSString *label, std::string &error) {
  id<MTLFunction> vertex = [library newFunctionWithName:vertexName];
  id<MTLFunction> fragment = [library newFunctionWithName:fragmentName];
  if (vertex == nil || fragment == nil) {
    [fragment release];
    [vertex release];
    error = "Qt DirectionalBlurs Metal shader entry point is unavailable";
    return nil;
  }
  auto *descriptor = [[MTLRenderPipelineDescriptor alloc] init];
  descriptor.label = label;
  descriptor.vertexFunction = vertex;
  descriptor.fragmentFunction = fragment;
  descriptor.rasterSampleCount = 1U;
  auto *attachment = descriptor.colorAttachments[0];
  attachment.pixelFormat = MTLPixelFormatRGBA8Unorm;
  attachment.blendingEnabled = NO;
  attachment.writeMask = MTLColorWriteMaskAll;
  NSError *pipelineError = nil;
  id<MTLRenderPipelineState> pipeline =
      [device newRenderPipelineStateWithDescriptor:descriptor
                                             error:&pipelineError];
  if (pipeline == nil) {
    error = MetalErrorMessage(
        @"Qt DirectionalBlurs Metal pipeline creation failed", pipelineError);
  }
  [descriptor release];
  [fragment release];
  [vertex release];
  return pipeline;
}

struct DirectionalMetalState final {
  id<MTLCommandQueue> queue{nil};
  id<MTLRenderPipelineState> downsamplePipeline{nil};
  id<MTLRenderPipelineState> directionalPipeline{nil};
  id<MTLRenderPipelineState> blendPipeline{nil};
  id<MTLRenderPipelineState> alphaOutlinePipeline{nil};
  id<MTLSamplerState> sampler{nil};
  id<MTLBuffer> triangleVertexBuffer{nil};
  id<MTLBuffer> triangleIndexBuffer{nil};
  id<MTLBuffer> quadVertexBuffer{nil};
  id<MTLBuffer> quadIndexBuffer{nil};

  ~DirectionalMetalState() {
    [quadIndexBuffer release];
    [quadVertexBuffer release];
    [triangleIndexBuffer release];
    [triangleVertexBuffer release];
    [sampler release];
    [alphaOutlinePipeline release];
    [blendPipeline release];
    [directionalPipeline release];
    [downsamplePipeline release];
    [queue release];
  }
};

std::unique_ptr<DirectionalMetalState> CreateMetalState(id<MTLDevice> device,
                                                        std::string &error) {
  auto state = std::make_unique<DirectionalMetalState>();
  state->queue = [device newCommandQueue];
  if (state->queue == nil) {
    error = "Qt DirectionalBlurs Metal command queue creation failed";
    return {};
  }

  NSError *libraryError = nil;
  id<MTLLibrary> library = [device
      newLibraryWithSource:
          [NSString stringWithUTF8String:kQtTextDirectionalBlursMetalSource]
                   options:nil
                     error:&libraryError];
  if (library == nil) {
    error = MetalErrorMessage(
        @"Qt DirectionalBlurs Metal shader compilation failed", libraryError);
    return {};
  }
  state->downsamplePipeline =
      MakePipeline(device, library, @"qtDirectionalCopyVertex",
                   @"qtDirectionalDownsampleFragment",
                   @"Qt DirectionalBlurs downsample RGBA8 v1", error);
  if (state->downsamplePipeline != nil) {
    state->directionalPipeline =
        MakePipeline(device, library, @"qtDirectionalQuadVertex",
                     @"qtDirectionalGaussianFragment",
                     @"Qt DirectionalBlurs Gaussian RGBA8 v1", error);
  }
  if (state->directionalPipeline != nil) {
    state->blendPipeline =
        MakePipeline(device, library, @"qtDirectionalQuadVertex",
                     @"qtDirectionalMeanBlendFragment",
                     @"Qt DirectionalBlurs Mean blend RGBA8 v1", error);
  }
  if (state->blendPipeline != nil) {
    state->alphaOutlinePipeline = MakePipeline(
        device, library, @"qtDirectionalQuadVertex", @"qtAlphaOutlineFragment",
        @"Qt AlphaOutline RGBA8 v1", error);
  }
  [library release];
  if (state->downsamplePipeline == nil || state->directionalPipeline == nil ||
      state->blendPipeline == nil || state->alphaOutlinePipeline == nil) {
    return {};
  }

  state->sampler = MakeNormalizedClampSampler(
      device, MTLSamplerMinMagFilterLinear,
      @"Qt DirectionalBlurs normalized linear clamp v1");
  state->triangleVertexBuffer =
      [device newBufferWithBytes:kFullscreenTriangle
                          length:sizeof(kFullscreenTriangle)
                         options:MTLResourceStorageModeShared];
  state->triangleIndexBuffer =
      [device newBufferWithBytes:kFullscreenTriangleIndices
                          length:sizeof(kFullscreenTriangleIndices)
                         options:MTLResourceStorageModeShared];
  state->quadVertexBuffer =
      [device newBufferWithBytes:kFullscreenQuad
                          length:sizeof(kFullscreenQuad)
                         options:MTLResourceStorageModeShared];
  state->quadIndexBuffer =
      [device newBufferWithBytes:kFullscreenIndices
                          length:sizeof(kFullscreenIndices)
                         options:MTLResourceStorageModeShared];
  if (state->sampler == nil || state->triangleVertexBuffer == nil ||
      state->triangleIndexBuffer == nil || state->quadVertexBuffer == nil ||
      state->quadIndexBuffer == nil) {
    error = "Qt DirectionalBlurs immutable Metal resource creation failed";
    return {};
  }
  error.clear();
  return state;
}

struct DirectionalTextures final {
  id<MTLTexture> page{nil};
  id<MTLTexture> downsample{nil};
  std::array<id<MTLTexture>, 4> directions{nil, nil, nil, nil};
  id<MTLTexture> blend{nil};
  id<MTLTexture> output{nil};

  ~DirectionalTextures() {
    [output release];
    [blend release];
    for (auto texture : directions)
      [texture release];
    [downsample release];
    [page release];
  }

  bool Complete() const {
    return page != nil && downsample != nil && blend != nil && output != nil &&
           std::all_of(directions.begin(), directions.end(),
                       [](id<MTLTexture> texture) { return texture != nil; });
  }
};

struct StandaloneDirectionalTextures final {
  id<MTLTexture> page{nil};
  id<MTLTexture> downsample{nil};
  id<MTLTexture> output{nil};

  ~StandaloneDirectionalTextures() {
    [output release];
    [downsample release];
    [page release];
  }

  bool Complete() const {
    return page != nil && downsample != nil && output != nil;
  }
};

bool AllocateTextures(id<MTLDevice> device,
                      const QtTextDirectionalBlursDerivedContract &contract,
                      DirectionalTextures &textures, std::string &error,
                      void *source, const NativeRgba8TextureTarget *target) {
  textures.page = target ? [(id<MTLTexture>)source retain] :
      MakePrivateRgba8Texture(device, contract.pageWidth, contract.pageHeight,
                              @"Qt DirectionalBlurs Page RGBA8 v1");
  textures.downsample = MakePrivateRgba8Texture(
      device, contract.downsampleWidth, contract.downsampleHeight,
      @"Qt DirectionalBlurs downsample RGBA8 v1");
  for (std::size_t index = 0U; index < textures.directions.size(); ++index) {
    NSString *label = [NSString
        stringWithFormat:@"Qt DirectionalBlurs direction %zu RGBA8 v1",
                         index + 1U];
    textures.directions[index] = MakePrivateRgba8Texture(
        device, contract.downsampleWidth, contract.downsampleHeight, label);
  }
  textures.blend =
      MakePrivateRgba8Texture(device, contract.pageWidth, contract.pageHeight,
                              @"Qt DirectionalBlurs Mean blend RGBA8 v1");
  textures.output = target ? [(id<MTLTexture>)target->texture retain] :
      MakePrivateRgba8Texture(device, contract.pageWidth, contract.pageHeight,
                              @"Qt DirectionalBlurs AlphaOutline RGBA8 v1");
  if (!textures.Complete()) {
    error = "Qt DirectionalBlurs private RGBA8 texture allocation failed";
    return false;
  }
  return true;
}

bool AllocateTextures(
    id<MTLDevice> device,
    const QtTextDirectionalBlurStandaloneDerivedContract &contract,
    StandaloneDirectionalTextures &textures, std::string &error,
                      void *source, const NativeRgba8TextureTarget *target) {
  textures.page = target ? [(id<MTLTexture>)source retain] : MakePrivateRgba8Texture(
      device, contract.pageWidth, contract.pageHeight,
      @"Qt standalone DirectionalBlur Page RGBA8");
  textures.downsample = MakePrivateRgba8Texture(
      device, contract.downsampleWidth, contract.downsampleHeight,
      @"Qt standalone DirectionalBlur downsample RGBA8");
  textures.output = target ? [(id<MTLTexture>)target->texture retain] : MakePrivateRgba8Texture(
      device, contract.pageWidth, contract.pageHeight,
      @"Qt standalone DirectionalBlur output RGBA8");
  if (!textures.Complete()) {
    error = "Qt standalone DirectionalBlur private RGBA8 texture allocation "
            "failed";
    return false;
  }
  return true;
}

bool EncodePass(id<MTLCommandBuffer> commandBuffer,
                DirectionalMetalState &state,
                const QtTextDirectionalBlursPassTrace &trace,
                id<MTLRenderPipelineState> pipeline, id<MTLTexture> target,
                const void *uniformBytes, const NSUInteger uniformLength,
                const std::array<id<MTLTexture>, 4> &inputTextures,
                const NSUInteger inputTextureCount, std::string &error,
                const void *secondaryUniformBytes = nullptr,
                const NSUInteger secondaryUniformLength = 0U) {
  auto *descriptor = [MTLRenderPassDescriptor renderPassDescriptor];
  auto *attachment = descriptor.colorAttachments[0];
  attachment.texture = target;
  attachment.loadAction = MTLLoadActionClear;
  attachment.storeAction = MTLStoreActionStore;
  attachment.clearColor =
      MTLClearColorMake(trace.clearColor[0], trace.clearColor[1],
                        trace.clearColor[2], trace.clearColor[3]);
  id<MTLRenderCommandEncoder> encoder =
      [commandBuffer renderCommandEncoderWithDescriptor:descriptor];
  if (encoder == nil) {
    error =
        "Qt DirectionalBlurs render encoder creation failed for " + trace.name;
    return false;
  }
  encoder.label = [NSString stringWithUTF8String:trace.name.c_str()];
  const MTLViewport viewport{0.0,
                             static_cast<double>(trace.outputHeight),
                             static_cast<double>(trace.outputWidth),
                             -static_cast<double>(trace.outputHeight),
                             0.0,
                             1.0};
  [encoder setViewport:viewport];
  const MTLScissorRect scissor{0U, 0U,
                               static_cast<NSUInteger>(trace.outputWidth),
                               static_cast<NSUInteger>(trace.outputHeight)};
  [encoder setScissorRect:scissor];
  [encoder setCullMode:static_cast<MTLCullMode>(trace.cullModeMetalValue)];
  [encoder setFrontFacingWinding:static_cast<MTLWinding>(
                                     trace.frontFacingWindingMetalValue)];
  [encoder setRenderPipelineState:pipeline];
  const bool engineCopy = trace.indexTypeMetalValue == 0U;
  [encoder setVertexBuffer:engineCopy ? state.triangleVertexBuffer
                                      : state.quadVertexBuffer
                    offset:0U
                   atIndex:0U];
  if (uniformBytes != nullptr && uniformLength > 0U) {
    [encoder setFragmentBytes:uniformBytes length:uniformLength atIndex:0U];
  }
  if (secondaryUniformBytes != nullptr && secondaryUniformLength > 0U) {
    [encoder setFragmentBytes:secondaryUniformBytes
                       length:secondaryUniformLength
                      atIndex:1U];
  }
  for (NSUInteger index = 0U; index < inputTextureCount; ++index) {
    [encoder setFragmentTexture:inputTextures[index] atIndex:index];
    [encoder setFragmentSamplerState:state.sampler atIndex:index];
  }
  if (trace.indexed) {
    [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                        indexCount:trace.elementCount
                         indexType:engineCopy ? MTLIndexTypeUInt16
                                              : MTLIndexTypeUInt32
                       indexBuffer:engineCopy ? state.triangleIndexBuffer
                                              : state.quadIndexBuffer
                 indexBufferOffset:0U
                     instanceCount:trace.instanceCount];
  } else {
    [encoder drawPrimitives:MTLPrimitiveTypeTriangle
                vertexStart:0U
                vertexCount:trace.elementCount
              instanceCount:trace.instanceCount];
  }
  [encoder endEncoding];
  return true;
}

class AppleQtTextDirectionalBlursRuntime final
    : public QtTextDirectionalBlursRuntime {
public:
  static std::unique_ptr<AppleQtTextDirectionalBlursRuntime>
  Create(std::string &error) {
    gpu_execution::DeviceRequest request;
    request.preferred = gpu_execution::Backend::Metal;
    auto executionDevice = gpu_execution::Device::Create(request, error);
    if (!executionDevice) {
      if (error.empty())
        error = "product Metal device is unavailable for Qt DirectionalBlurs";
      return {};
    }
    if (executionDevice->backend() != gpu_execution::Backend::Metal ||
        executionDevice->nativeDeviceHandle() == 0U) {
      error = "product GPU device is not a usable Metal device for Qt "
              "DirectionalBlurs";
      return {};
    }
    id<MTLDevice> device = (__bridge id<MTLDevice>)(reinterpret_cast<void *>(
        executionDevice->nativeDeviceHandle()));
    if (device == nil) {
      error =
          "product Metal native handle is unavailable for Qt DirectionalBlurs";
      return {};
    }
    auto state = CreateMetalState(device, error);
    if (!state)
      return {};
    error.clear();
    return std::unique_ptr<AppleQtTextDirectionalBlursRuntime>(
        new AppleQtTextDirectionalBlursRuntime(std::move(executionDevice),
                                               std::move(state)));
  }

  bool Render(const QtTextDirectionalBlursRenderRequest &request,
              QtTextDirectionalBlursRenderResult &result,
              std::string &error, const NativeRgba8TextureTarget *target) override {
    @autoreleasepool {
      result = {};
      QtTextDirectionalBlursDerivedContract contract;
      if (!ResolveQtTextDirectionalBlursContract(request.contract, contract,
                                                   error) ||
          !ValidateImageView(request, error)) {
        return false;
      }
      id<MTLDevice> device = Device();
      if (device == nil || state_ == nullptr) {
        error = "Qt DirectionalBlurs Metal runtime lost its product device";
        return false;
      }

      DirectionalTextures textures;
      if (!ValidateNativeRgba8Target(request.source.nativeTexture,
          contract.pageWidth, contract.pageHeight, target, device, error) ||
          !AllocateTextures(device, contract, textures, error, request.source.nativeTexture, target))
        return false;

      const NSUInteger stagingRowBytes =
          AlignedRgba8RowBytes(device, contract.pageWidth);
      const NSUInteger stagingLength =
          stagingRowBytes * static_cast<NSUInteger>(contract.pageHeight);
      id<MTLBuffer> upload = target ? nil :
          [device newBufferWithLength:stagingLength
                              options:MTLResourceStorageModeShared];
      id<MTLBuffer> readback = target ? nil :
          [device newBufferWithLength:stagingLength
                              options:MTLResourceStorageModeShared];
      const auto releaseStaging = [&]() {
        [readback release];
        [upload release];
      };
      if (!target && (upload == nil || readback == nil)) {
        error = "Qt DirectionalBlurs staging buffer allocation failed";
        releaseStaging();
        return false;
      }

      // CPU ABI is logical top-left RGBA8. Qt's internal render targets use a
      // negative-height viewport and no shader flip; one transport flip here
      // and one on readback reproduce that convention without an inner flip.
      const std::size_t compactRowBytes =
          static_cast<std::size_t>(contract.pageWidth) * 4U;
      auto *uploadBytes = static_cast<std::uint8_t *>(upload.contents);
      if (!target) {
        CopyVerticallyFlippedRows(
            uploadBytes, static_cast<std::size_t>(stagingRowBytes),
            request.source.pixels, request.source.rowBytes, compactRowBytes,
            contract.pageHeight);
      }

      id<MTLCommandQueue> queue = target ? (id<MTLCommandQueue>)target->commandQueue : state_->queue;
      id<MTLCommandBuffer> commandBuffer = [queue commandBuffer];
      if (commandBuffer == nil) {
        error = "Qt DirectionalBlurs command buffer creation failed";
        releaseStaging();
        return false;
      }
      commandBuffer.label =
          @"Qt DirectionalBlurs four-pass Mean plus AlphaOutline v1";
      if (!target) {
      id<MTLBlitCommandEncoder> uploadBlit = [commandBuffer blitCommandEncoder];
      if (uploadBlit == nil) {
        error = "Qt DirectionalBlurs Page upload encoder creation failed";
        releaseStaging();
        return false;
      }
      CopyBufferToTexture(
          uploadBlit, upload, textures.page,
          MTLSizeMake(static_cast<NSUInteger>(contract.pageWidth),
                      static_cast<NSUInteger>(contract.pageHeight), 1U),
          stagingRowBytes, stagingLength);
      [uploadBlit endEncoding];
      }

      if (!EncodePass(commandBuffer, *state_, contract.passes[0],
                      state_->downsamplePipeline, textures.downsample, nullptr,
                      0U, {textures.page, nil, nil, nil}, 1U, error)) {
        releaseStaging();
        return false;
      }
      for (std::size_t index = 0U; index < textures.directions.size();
           ++index) {
        if (!EncodePass(commandBuffer, *state_, contract.passes[index + 1U],
                        state_->directionalPipeline, textures.directions[index],
                        &contract.directionalUniforms[index],
                        sizeof(contract.directionalUniforms[index]),
                        {textures.downsample, nil, nil, nil}, 1U, error)) {
          releaseStaging();
          return false;
        }
      }
      if (!EncodePass(commandBuffer, *state_, contract.passes[5],
                      state_->blendPipeline, textures.blend,
                      &contract.blendUniforms, sizeof(contract.blendUniforms),
                      textures.directions, 4U, error) ||
          !EncodePass(commandBuffer, *state_, contract.passes[6],
                      state_->alphaOutlinePipeline, textures.output,
                      &contract.alphaOutlineUniforms,
                      sizeof(contract.alphaOutlineUniforms),
                      {textures.blend, nil, nil, nil}, 1U, error)) {
        releaseStaging();
        return false;
      }

      if (target) {
        if (!SubmitNativeTextureCommand(commandBuffer, target->submit,
            {textures.page, textures.output, textures.downsample, textures.blend, textures.directions[0], textures.directions[1], textures.directions[2], textures.directions[3]}, error)) {
          releaseStaging();
          return false;
        }
        result.width = contract.pageWidth;
        result.height = contract.pageHeight;
        result.contract = contract;
        releaseStaging();
        error.clear();
        return true;
      }
      id<MTLBlitCommandEncoder> readbackBlit /* CPU delivery */ =
          [commandBuffer blitCommandEncoder];
      if (readbackBlit == nil) {
        error = "Qt DirectionalBlurs readback encoder creation failed";
        releaseStaging();
        return false;
      }
      CopyTextureToBuffer(
          readbackBlit, textures.output, readback,
          MTLSizeMake(static_cast<NSUInteger>(contract.pageWidth),
                      static_cast<NSUInteger>(contract.pageHeight), 1U),
          stagingRowBytes, stagingLength);
      [readbackBlit endEncoding];
      [commandBuffer commit];
      [commandBuffer waitUntilCompleted];
      if (commandBuffer.status != MTLCommandBufferStatusCompleted) {
        error = MetalErrorMessage(@"Qt DirectionalBlurs Metal command failed",
                                  commandBuffer.error);
        releaseStaging();
        return false;
      }

      QtTextDirectionalBlursRenderResult rendered;
      rendered.width = contract.pageWidth;
      rendered.height = contract.pageHeight;
      rendered.contract = contract;
      rendered.outputPixels.resize(
          compactRowBytes * static_cast<std::size_t>(contract.pageHeight));
      const auto *readbackBytes =
          static_cast<const std::uint8_t *>(readback.contents);
      if (!target) {
        CopyVerticallyFlippedRows(rendered.outputPixels.data(), compactRowBytes,
                                  readbackBytes,
                                  static_cast<std::size_t>(stagingRowBytes),
                                  compactRowBytes, contract.pageHeight);
      }
      releaseStaging();
      result = std::move(rendered);
      error.clear();
      return true;
    }
  }

  bool RenderStandalone(
      const QtTextDirectionalBlurStandaloneRenderRequest &request,
      QtTextDirectionalBlurStandaloneRenderResult &result,
      std::string &error, const NativeRgba8TextureTarget *target) override {
    @autoreleasepool {
      result = {};
      QtTextDirectionalBlurStandaloneDerivedContract contract;
      if (!ResolveQtTextDirectionalBlurStandaloneContract(
              request.contract, contract, error) ||
          !ValidateImageView(request, error)) {
        return false;
      }
      id<MTLDevice> device = Device();
      if (device == nil || state_ == nullptr) {
        error =
            "Qt standalone DirectionalBlur Metal runtime lost its product "
            "device";
        return false;
      }

      StandaloneDirectionalTextures textures;
      if (!ValidateNativeRgba8Target(request.source.nativeTexture,
          contract.pageWidth, contract.pageHeight, target, device, error) ||
          !AllocateTextures(device, contract, textures, error, request.source.nativeTexture, target))
        return false;

      const NSUInteger stagingRowBytes =
          AlignedRgba8RowBytes(device, contract.pageWidth);
      const NSUInteger stagingLength =
          stagingRowBytes * static_cast<NSUInteger>(contract.pageHeight);
      id<MTLBuffer> upload = target ? nil :
          [device newBufferWithLength:stagingLength
                              options:MTLResourceStorageModeShared];
      id<MTLBuffer> readback = target ? nil :
          [device newBufferWithLength:stagingLength
                              options:MTLResourceStorageModeShared];
      const auto releaseStaging = [&]() {
        [readback release];
        [upload release];
      };
      if (!target && (upload == nil || readback == nil)) {
        error = "Qt standalone DirectionalBlur staging buffer allocation "
                "failed";
        releaseStaging();
        return false;
      }

      const std::size_t compactRowBytes =
          static_cast<std::size_t>(contract.pageWidth) * 4U;
      auto *uploadBytes = static_cast<std::uint8_t *>(upload.contents);
      if (!target) {
        CopyVerticallyFlippedRows(
            uploadBytes, static_cast<std::size_t>(stagingRowBytes),
            request.source.pixels, request.source.rowBytes, compactRowBytes,
            contract.pageHeight);
      }

      id<MTLCommandQueue> queue = target ? (id<MTLCommandQueue>)target->commandQueue : state_->queue;
      id<MTLCommandBuffer> commandBuffer = [queue commandBuffer];
      if (commandBuffer == nil) {
        error = "Qt standalone DirectionalBlur command buffer creation failed";
        releaseStaging();
        return false;
      }
      commandBuffer.label =
          @"Qt standalone DirectionalBlur EngineCopy plus Gaussian";
      if (!target) {
      id<MTLBlitCommandEncoder> uploadBlit = [commandBuffer blitCommandEncoder];
      if (uploadBlit == nil) {
        error = "Qt standalone DirectionalBlur Page upload encoder creation "
                "failed";
        releaseStaging();
        return false;
      }
      CopyBufferToTexture(
          uploadBlit, upload, textures.page,
          MTLSizeMake(static_cast<NSUInteger>(contract.pageWidth),
                      static_cast<NSUInteger>(contract.pageHeight), 1U),
          stagingRowBytes, stagingLength);
      [uploadBlit endEncoding];
      }

      if (!EncodePass(commandBuffer, *state_, contract.passes[0],
                      state_->downsamplePipeline, textures.downsample,
                      contract.textureFlip.data(),
                      sizeof(contract.textureFlip),
                      {textures.page, nil, nil, nil}, 1U, error) ||
          !EncodePass(commandBuffer, *state_, contract.passes[1],
                      state_->directionalPipeline, textures.output,
                      &contract.directionalUniforms,
                      sizeof(contract.directionalUniforms),
                      {textures.downsample, nil, nil, nil}, 1U, error,
                      contract.textureFlip.data(),
                      sizeof(contract.textureFlip))) {
        releaseStaging();
        return false;
      }

      if (target) {
        if (!SubmitNativeTextureCommand(commandBuffer, target->submit,
            {textures.page, textures.output, textures.downsample}, error)) {
          releaseStaging();
          return false;
        }
        result.width = contract.pageWidth;
        result.height = contract.pageHeight;
        result.contract = contract;
        releaseStaging();
        error.clear();
        return true;
      }
      id<MTLBlitCommandEncoder> readbackBlit /* CPU delivery */ =
          [commandBuffer blitCommandEncoder];
      if (readbackBlit == nil) {
        error = "Qt standalone DirectionalBlur readback encoder creation "
                "failed";
        releaseStaging();
        return false;
      }
      CopyTextureToBuffer(
          readbackBlit, textures.output, readback,
          MTLSizeMake(static_cast<NSUInteger>(contract.pageWidth),
                      static_cast<NSUInteger>(contract.pageHeight), 1U),
          stagingRowBytes, stagingLength);
      [readbackBlit endEncoding];
      [commandBuffer commit];
      [commandBuffer waitUntilCompleted];
      if (commandBuffer.status != MTLCommandBufferStatusCompleted) {
        error = MetalErrorMessage(
            @"Qt standalone DirectionalBlur Metal command failed",
            commandBuffer.error);
        releaseStaging();
        return false;
      }

      QtTextDirectionalBlurStandaloneRenderResult rendered;
      rendered.width = contract.pageWidth;
      rendered.height = contract.pageHeight;
      rendered.contract = contract;
      rendered.outputPixels.resize(
          compactRowBytes * static_cast<std::size_t>(contract.pageHeight));
      const auto *readbackBytes =
          static_cast<const std::uint8_t *>(readback.contents);
      if (!target) {
        CopyVerticallyFlippedRows(rendered.outputPixels.data(), compactRowBytes,
                                  readbackBytes,
                                  static_cast<std::size_t>(stagingRowBytes),
                                  compactRowBytes, contract.pageHeight);
      }
      releaseStaging();
      result = std::move(rendered);
      error.clear();
      return true;
    }
  }

  const char *BackendName() const noexcept override {
    return "qt-lumi-directional-blurs-metal-rgba8-boundaries-alpha-outline-v1";
  }

private:
  AppleQtTextDirectionalBlursRuntime(
      std::shared_ptr<gpu_execution::Device> executionDevice,
      std::unique_ptr<DirectionalMetalState> state)
      : executionDevice_(std::move(executionDevice)), state_(std::move(state)) {
  }

  id<MTLDevice> Device() const {
    return executionDevice_ ? (__bridge id<MTLDevice>)(reinterpret_cast<void *>(
                                  executionDevice_->nativeDeviceHandle()))
                            : nil;
  }

  std::shared_ptr<gpu_execution::Device> executionDevice_;
  std::unique_ptr<DirectionalMetalState> state_;
};

} // namespace

std::unique_ptr<QtTextDirectionalBlursRuntime>
CreateQtTextDirectionalBlursRuntime(std::string &error) {
  @autoreleasepool {
    return AppleQtTextDirectionalBlursRuntime::Create(error);
  }
}

} // namespace videocut::skia_runtime::internal
