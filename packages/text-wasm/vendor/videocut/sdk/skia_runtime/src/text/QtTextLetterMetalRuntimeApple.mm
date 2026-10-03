#include "runtime/MetalRuntimeSupport.h"
#include "text/NativeMetalSubmission.h"
#include "text/shaders/MetalQtTextLetterVertex.h"

#include "text/shaders/MetalQtTextLetterFragment.h"

#include "text/QtTextLetterMetalRuntime.h"

#import <Metal/Metal.h>

#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <utility>

namespace videocut::skia_runtime::internal {
namespace {

constexpr const char *kVertexSource = shader_sources::MetalQtTextLetterVertex;
constexpr const char *kFragmentSource =
    shader_sources::MetalQtTextLetterFragment;

std::string MetalError(NSString *prefix, NSError *error) {
  std::string result(prefix.UTF8String);
  if (error != nil) {
    result += ": ";
    result += error.localizedDescription.UTF8String;
  }
  return result;
}

bool Validate(const QtTextLetterRenderRequest &request, std::string &error) {
  if (request.implementationVersion != kQtTextLetterImplementationVersion) {
    error = "unsupported Qt TextPro letter implementation version";
    return false;
  }
  if (!request.atlas.nativeTexture && request.atlas.pixels == nullptr) {
    error = "base-fill RG8 atlas pixels are unavailable";
    return false;
  }
  if (request.atlas.width <= 0 || request.atlas.height <= 0 ||
      request.atlas.width > kQtTextLetterMaxAtlasDimension ||
      request.atlas.height > kQtTextLetterMaxAtlasDimension) {
    error = "base-fill RG8 atlas dimensions violate the texture budget";
    return false;
  }
  const auto atlasWidth = static_cast<std::size_t>(request.atlas.width);
  const auto atlasHeight = static_cast<std::size_t>(request.atlas.height);
  if (atlasWidth > std::numeric_limits<std::size_t>::max() / 2U) {
    error = "base-fill RG8 atlas compact row size overflows";
    return false;
  }
  const std::size_t compactAtlasRowBytes = atlasWidth * 2U;
  if (request.atlas.rowBytes < compactAtlasRowBytes) {
    error = "base-fill RG8 atlas rowBytes is shorter than width*2";
    return false;
  }
  const std::size_t precedingAtlasRows = atlasHeight - 1U;
  if (precedingAtlasRows != 0U &&
      request.atlas.rowBytes >
          (std::numeric_limits<std::size_t>::max() - compactAtlasRowBytes) /
              precedingAtlasRows) {
    error = "base-fill RG8 atlas readable byte range overflows";
    return false;
  }
  const std::size_t requiredAtlasBytes =
      precedingAtlasRows * request.atlas.rowBytes + compactAtlasRowBytes;
  if (requiredAtlasBytes > kQtTextLetterMaxAtlasBytes) {
    error = "base-fill RG8 atlas violates the byte budget";
    return false;
  }
  if (!request.atlas.nativeTexture &&
      request.atlas.byteSize < requiredAtlasBytes) {
    error = "base-fill RG8 atlas byteSize is smaller than its final row";
    return false;
  }
  if (request.vertices.bytes == nullptr || request.vertexCount == 0U ||
      request.vertexCount > kQtTextLetterMaxVertexCount ||
      request.vertexStride != kQtTextLetterVertexStride ||
      request.vertexCount >
          std::numeric_limits<std::size_t>::max() / request.vertexStride ||
      request.vertices.byteSize < request.vertexCount * request.vertexStride) {
    error = "base-fill vertex batch violates the stride108 byte budget";
    return false;
  }
  if (request.indices == nullptr || request.indexCount == 0U ||
      request.indexCount > kQtTextLetterMaxIndexCount ||
      request.indexCount % 3U != 0U ||
      request.indexCount >
          std::numeric_limits<std::size_t>::max() / sizeof(std::uint16_t)) {
    error = "base-fill index batch violates the uint16 triangle budget";
    return false;
  }
  for (std::size_t index = 0U; index < request.indexCount; ++index) {
    if (static_cast<std::size_t>(request.indices[index]) >=
        request.vertexCount) {
      error = "base-fill index is outside the vertex batch";
      return false;
    }
  }
  if (request.outputWidth <= 0 || request.outputHeight <= 0 ||
      request.outputWidth > kQtTextLetterMaxOutputDimension ||
      request.outputHeight > kQtTextLetterMaxOutputDimension) {
    error = "base-fill output dimensions violate the texture budget";
    return false;
  }
  const auto width = static_cast<std::size_t>(request.outputWidth);
  const auto height = static_cast<std::size_t>(request.outputHeight);
  if (width > std::numeric_limits<std::size_t>::max() / 4U ||
      height > std::numeric_limits<std::size_t>::max() / (width * 4U) ||
      width * height * 4U > kQtTextLetterMaxOutputBytes) {
    error = "base-fill output byte size violates the readback budget";
    return false;
  }
  const auto finite = [](float value) { return std::isfinite(value); };
  if (!finite(request.uniforms.opacity) || request.uniforms.opacity < 0.0F ||
      request.uniforms.opacity > 1.0F ||
      !finite(request.uniforms.extraSmooth) ||
      !finite(request.uniforms.minimumSdf) ||
      !finite(request.uniforms.extraWidth)) {
    error = "base-fill scalar uniform is invalid";
    return false;
  }
  for (float value : request.uniforms.mvp) {
    if (!finite(value))
      return error = "base-fill MVP contains a non-finite value", false;
  }
  for (float value : request.uniforms.fillColor) {
    if (!finite(value))
      return error = "base-fill color contains a non-finite value", false;
  }
  for (float value : request.uniforms.offsetInfo) {
    if (!finite(value))
      return error = "base-fill offset contains a non-finite value", false;
  }
  for (float value : request.uniforms.textureFlip) {
    if (!finite(value))
      return error = "base-fill texture flip contains a non-finite value",
             false;
  }
  return true;
}

id<MTLTexture> MakeTexture(id<MTLDevice> device, MTLPixelFormat format,
                           NSUInteger width, NSUInteger height,
                           MTLTextureUsage usage) {
  auto *descriptor =
      [MTLTextureDescriptor texture2DDescriptorWithPixelFormat:format
                                                         width:width
                                                        height:height
                                                     mipmapped:NO];
  descriptor.storageMode = MTLStorageModeShared;
  descriptor.usage = usage;
  return [device newTextureWithDescriptor:descriptor];
}

struct State final {
  id<MTLDevice> device{nil};
  id<MTLCommandQueue> queue{nil};
  id<MTLRenderPipelineState> pipeline{nil};
  id<MTLSamplerState> linear{nil};
  id<MTLSamplerState> nearest{nil};
  id<MTLTexture> dummy{nil};

  ~State() {
    [dummy release];
    [nearest release];
    [linear release];
    [pipeline release];
    [queue release];
    [device release];
  }
};

std::unique_ptr<State> MakeState(std::string &error) {
  auto state = std::make_unique<State>();
  state->device = [MTLCreateSystemDefaultDevice() retain];
  state->queue = [state->device newCommandQueue];
  if (state->device == nil || state->queue == nil) {
    error = "Metal device or command queue is unavailable";
    return {};
  }

  NSError *vertexError = nil;
  NSError *fragmentError = nil;
  id<MTLLibrary> vertexLibrary = [state->device
      newLibraryWithSource:[NSString stringWithUTF8String:kVertexSource]
                   options:nil
                     error:&vertexError];
  id<MTLLibrary> fragmentLibrary = [state->device
      newLibraryWithSource:[NSString stringWithUTF8String:kFragmentSource]
                   options:nil
                     error:&fragmentError];
  if (vertexLibrary == nil || fragmentLibrary == nil) {
    error =
        vertexLibrary == nil
            ? MetalError(@"exact Qt vertex MSL compilation failed", vertexError)
            : MetalError(@"exact Qt fragment MSL compilation failed",
                         fragmentError);
    [fragmentLibrary release];
    [vertexLibrary release];
    return {};
  }
  id<MTLFunction> vertex = [vertexLibrary newFunctionWithName:@"main0"];
  id<MTLFunction> fragment = [fragmentLibrary newFunctionWithName:@"main0"];

  auto *layout = [[MTLVertexDescriptor alloc] init];
  const NSUInteger offsets[] = {0U, 12U, 28U, 44U, 60U, 76U, 92U};
  const MTLVertexFormat formats[] = {
      MTLVertexFormatFloat3, MTLVertexFormatFloat4, MTLVertexFormatFloat4,
      MTLVertexFormatFloat4, MTLVertexFormatFloat4, MTLVertexFormatFloat4,
      MTLVertexFormatFloat4};
  for (NSUInteger i = 0; i < 7U; ++i) {
    layout.attributes[i].format = formats[i];
    layout.attributes[i].offset = offsets[i];
    layout.attributes[i].bufferIndex = 30U;
  }
  layout.layouts[30].stride = 108U;
  layout.layouts[30].stepFunction = MTLVertexStepFunctionPerVertex;

  auto *descriptor = [[MTLRenderPipelineDescriptor alloc] init];
  descriptor.vertexFunction = vertex;
  descriptor.fragmentFunction = fragment;
  descriptor.vertexDescriptor = layout;
  descriptor.rasterSampleCount = 1U;
  auto *color = descriptor.colorAttachments[0];
  color.pixelFormat = MTLPixelFormatRGBA8Unorm;
  color.blendingEnabled = YES;
  color.rgbBlendOperation = MTLBlendOperationAdd;
  color.alphaBlendOperation = MTLBlendOperationAdd;
  color.sourceRGBBlendFactor = MTLBlendFactorOne;
  color.sourceAlphaBlendFactor = MTLBlendFactorOne;
  color.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
  color.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
  NSError *pipelineError = nil;
  state->pipeline =
      [state->device newRenderPipelineStateWithDescriptor:descriptor
                                                    error:&pipelineError];
  [descriptor release];
  [layout release];
  [fragment release];
  [vertex release];
  [fragmentLibrary release];
  [vertexLibrary release];
  if (state->pipeline == nil) {
    error = MetalError(@"Qt TextPro pipeline creation failed", pipelineError);
    return {};
  }

  state->linear =
      MakeNormalizedClampSampler(state->device, MTLSamplerMinMagFilterLinear);
  state->nearest =
      MakeNormalizedClampSampler(state->device, MTLSamplerMinMagFilterNearest);
  state->dummy = MakeTexture(state->device, MTLPixelFormatBGRA8Unorm, 1U, 1U,
                             MTLTextureUsageShaderRead);
  if (state->linear == nil || state->nearest == nil || state->dummy == nil) {
    error = "Qt TextPro immutable resources are unavailable";
    return {};
  }
  return state;
}

class AppleRuntime final : public QtTextLetterMetalRuntime {
public:
  static std::unique_ptr<AppleRuntime> Create(std::string &error) {
    auto state = MakeState(error);
    if (!state)
      return {};
    return std::unique_ptr<AppleRuntime>(new AppleRuntime(std::move(state)));
  }

  bool Render(const QtTextLetterRenderRequest &request,
              std::vector<std::uint8_t> &outputPixels,
              std::string &error) override {
    std::shared_ptr<void> texture;
    return RenderTexture(request, texture, error, &outputPixels, nullptr, {});
  }

  bool RenderTexture(const QtTextLetterRenderRequest &request,
                     std::shared_ptr<void> &texture, std::string &error,
                     std::vector<std::uint8_t> *diagnosticPixels,
                     void *commandQueue, const NativeCommandSubmission &submit) override {
    @autoreleasepool {
      texture.reset();
      if (diagnosticPixels)
        diagnosticPixels->clear();
      if (!Validate(request, error))
        return false;
      id<MTLCommandQueue> queue = commandQueue != nullptr
          ? (id<MTLCommandQueue>)commandQueue : state_->queue;
      if (queue == nil || queue.device != state_->device) {
        error = "Qt TextPro Letter queue does not match its Metal device";
        return false;
      }
      id<MTLTexture> nativeAtlas =
          (id<MTLTexture>)request.atlas.nativeTexture.get();
      if (nativeAtlas != nil &&
          (nativeAtlas.device != state_->device ||
           nativeAtlas.width != static_cast<NSUInteger>(request.atlas.width) ||
           nativeAtlas.height != static_cast<NSUInteger>(request.atlas.height) ||
           (nativeAtlas.pixelFormat != MTLPixelFormatRG8Unorm &&
            nativeAtlas.pixelFormat != MTLPixelFormatRGBA8Unorm) ||
           (nativeAtlas.usage & MTLTextureUsageShaderRead) == 0U)) {
        error = "Qt Letter atlas does not match its Metal device or RG contract";
        return false;
      }
      const auto vertexBytes = request.vertexCount * request.vertexStride;
      const auto indexBytes = request.indexCount * sizeof(std::uint16_t);
      const auto outputWidth = static_cast<NSUInteger>(request.outputWidth);
      const auto outputHeight = static_cast<NSUInteger>(request.outputHeight);
      const auto outputRowBytes =
          static_cast<std::size_t>(request.outputWidth) * 4U;
      const auto outputByteSize =
          outputRowBytes * static_cast<std::size_t>(request.outputHeight);
      const auto atlasWidth = static_cast<NSUInteger>(request.atlas.width);
      const auto atlasHeight = static_cast<NSUInteger>(request.atlas.height);
      id<MTLTexture> atlas =
          nativeAtlas != nil
              ? [nativeAtlas retain]
              : MakeTexture(state_->device, MTLPixelFormatRG8Unorm, atlasWidth,
                            atlasHeight, MTLTextureUsageShaderRead);
      id<MTLTexture> output = MakeTexture(
          state_->device, MTLPixelFormatRGBA8Unorm, outputWidth, outputHeight,
          MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead);
      id<MTLBuffer> vertices =
          [state_->device newBufferWithBytes:request.vertices.bytes
                                      length:vertexBytes
                                     options:MTLResourceStorageModeShared];
      id<MTLBuffer> indices =
          [state_->device newBufferWithBytes:request.indices
                                      length:indexBytes
                                     options:MTLResourceStorageModeShared];
      const auto release = [&]() {
        [indices release];
        [vertices release];
        [output release];
        [atlas release];
      };
      if (atlas == nil || output == nil || vertices == nil || indices == nil) {
        error = "Qt TextPro per-render resources are unavailable";
        release();
        return false;
      }
      if (nativeAtlas == nil) {
        [atlas replaceRegion:MTLRegionMake2D(0U, 0U, atlasWidth, atlasHeight)
                 mipmapLevel:0U
                   withBytes:request.atlas.pixels
                 bytesPerRow:request.atlas.rowBytes];
      }
      if (request.uniforms.opacity != 1.0F) {
        auto *bytes = static_cast<std::uint8_t *>(vertices.contents);
        for (std::size_t i = 0; i < request.vertexCount; ++i) {
          float alpha = 0.0F;
          std::memcpy(&alpha, bytes + i * request.vertexStride + 24U,
                      sizeof(alpha));
          alpha *= request.uniforms.opacity;
          std::memcpy(bytes + i * request.vertexStride + 24U, &alpha,
                      sizeof(alpha));
        }
      }

      id<MTLCommandBuffer> commands = [queue commandBuffer];
      auto *pass = [MTLRenderPassDescriptor renderPassDescriptor];
      pass.colorAttachments[0].texture = output;
      pass.colorAttachments[0].loadAction = MTLLoadActionClear;
      pass.colorAttachments[0].storeAction = MTLStoreActionStore;
      pass.colorAttachments[0].clearColor =
          MTLClearColorMake(0.0, 0.0, 0.0, 0.0);
      id<MTLRenderCommandEncoder> encoder =
          [commands renderCommandEncoderWithDescriptor:pass];
      if (commands == nil || encoder == nil) {
        error = "Qt TextPro Letter command encoding is unavailable";
        release();
        return false;
      }
      [encoder setViewport:MTLViewport{0.0, static_cast<double>(outputHeight),
                                       static_cast<double>(outputWidth),
                                       -static_cast<double>(outputHeight), 0.0,
                                       1.0}];
      [encoder
          setScissorRect:MTLScissorRect{0U, 0U, outputWidth, outputHeight}];
      [encoder setCullMode:MTLCullModeNone];
      [encoder setFrontFacingWinding:MTLWindingClockwise];
      [encoder setRenderPipelineState:state_->pipeline];
      [encoder setVertexBytes:&request.uniforms.multiInstanceColor
                       length:sizeof(std::int32_t)
                      atIndex:0U];
      [encoder setVertexBytes:request.uniforms.offsetInfo
                       length:sizeof(request.uniforms.offsetInfo)
                      atIndex:2U];
      [encoder setVertexBytes:request.uniforms.mvp
                       length:sizeof(request.uniforms.mvp)
                      atIndex:5U];
      [encoder setVertexBuffer:vertices offset:0U atIndex:30U];

      const float zero = 0.0F, one = 1.0F;
      const float zero2[2]{0.0F, 0.0F};
      const float size[2]{static_cast<float>(request.atlas.width),
                          static_cast<float>(request.atlas.height)};
      const float dummySize[2]{1.0F, 1.0F};
      const std::int32_t zeroInt = 0;
      [encoder setFragmentBytes:&request.uniforms.extraSmooth
                         length:4
                        atIndex:0];
      [encoder setFragmentBytes:zero2 length:8 atIndex:1];
      [encoder setFragmentBytes:&request.uniforms.minimumSdf
                         length:4
                        atIndex:2];
      [encoder setFragmentBytes:&request.uniforms.extraWidth
                         length:4
                        atIndex:3];
      [encoder setFragmentBytes:request.uniforms.textureFlip
                         length:16
                        atIndex:4];
      [encoder setFragmentBytes:request.uniforms.fillColor length:16 atIndex:5];
      [encoder setFragmentBytes:&one length:4 atIndex:6];
      [encoder setFragmentBytes:&zeroInt length:4 atIndex:7];
      [encoder setFragmentBytes:size length:8 atIndex:8];
      [encoder setFragmentBytes:&zero length:4 atIndex:9];
      [encoder setFragmentBytes:zero2 length:8 atIndex:10];
      [encoder setFragmentBytes:dummySize length:8 atIndex:11];
      [encoder setFragmentBytes:zero2 length:8 atIndex:12];
      [encoder setFragmentTexture:atlas atIndex:0U];
      [encoder setFragmentSamplerState:state_->linear atIndex:0U];
      for (NSUInteger i = 1U; i < 4U; ++i) {
        [encoder setFragmentTexture:state_->dummy atIndex:i];
        [encoder setFragmentSamplerState:state_->nearest atIndex:i];
      }
      [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                          indexCount:static_cast<NSUInteger>(request.indexCount)
                           indexType:MTLIndexTypeUInt16
                         indexBuffer:indices
                   indexBufferOffset:0U
                       instanceCount:1U];
      [encoder endEncoding];
      if (!SubmitNativeTextureCommand(commands, submit,
          {atlas, output, vertices, indices}, error)) {
        release();
        return false;
      }
      // The ordered Ganesh queue retains encoded resources through completion.
      // Only callers consuming pixels on the CPU need synchronous completion.
      if (commandQueue == nullptr || diagnosticPixels != nullptr) {
        [commands waitUntilCompleted];
        if (commands.status != MTLCommandBufferStatusCompleted) {
          error = MetalError(@"Qt TextPro command failed", commands.error);
          release();
          return false;
        }
      }
      if (diagnosticPixels) {
        diagnosticPixels->resize(outputByteSize);
        [output getBytes:diagnosticPixels->data()
             bytesPerRow:outputRowBytes
              fromRegion:MTLRegionMake2D(0U, 0U, outputWidth, outputHeight)
             mipmapLevel:0U];
      }
      texture = std::shared_ptr<void>((void *)[output retain], [](void *value) {
        [(id<MTLTexture>)value release];
      });
      release();
      error.clear();
      return true;
    }
  }

  const char *BackendName() const noexcept override {
    return "qt-textpro-letter-metal-base-fill-v1";
  }

private:
  explicit AppleRuntime(std::unique_ptr<State> state)
      : state_(std::move(state)) {}
  std::unique_ptr<State> state_;
};

} // namespace

std::unique_ptr<QtTextLetterMetalRuntime>
CreateQtTextLetterMetalRuntime(std::string &error) {
  @autoreleasepool {
    return AppleRuntime::Create(error);
  }
}

} // namespace videocut::skia_runtime::internal
