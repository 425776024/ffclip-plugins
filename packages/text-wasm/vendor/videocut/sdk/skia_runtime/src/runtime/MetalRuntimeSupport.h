#pragma once

#import <Metal/Metal.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <string>

namespace videocut::skia_runtime::internal {

struct MetalTexturedVertex final {
  float positionX;
  float positionY;
  float uvX;
  float uvY;
};

static_assert(sizeof(MetalTexturedVertex) == 16U);

inline constexpr MetalTexturedVertex kTexturedTriangleVertices[] = {
    {-1.0F, -1.0F, 0.0F, 0.0F},
    {3.0F, -1.0F, 2.0F, 0.0F},
    {-1.0F, 3.0F, 0.0F, 2.0F},
};

inline constexpr std::uint16_t kTexturedTriangleIndices[] = {0U, 1U, 2U};

inline constexpr MetalTexturedVertex kTexturedQuadVertices[] = {
    {-1.0F, -1.0F, 0.0F, 0.0F},
    {1.0F, -1.0F, 1.0F, 0.0F},
    {-1.0F, 1.0F, 0.0F, 1.0F},
    {1.0F, 1.0F, 1.0F, 1.0F},
};

inline constexpr std::uint32_t kTexturedQuadIndices[] = {0U, 1U, 2U,
                                                         1U, 3U, 2U};

struct MetalTexturedPipelineState final {
  id<MTLCommandQueue> queue{nil};
  id<MTLRenderPipelineState> pipeline{nil};
  id<MTLSamplerState> sampler{nil};
  id<MTLBuffer> vertexBuffer{nil};
  id<MTLBuffer> indexBuffer{nil};

  ~MetalTexturedPipelineState() {
    [indexBuffer release];
    [vertexBuffer release];
    [sampler release];
    [pipeline release];
    [queue release];
  }
};

inline std::string MetalErrorMessage(NSString *prefix, NSError *error) {
  std::string result = prefix != nil ? std::string(prefix.UTF8String)
                                     : std::string("Metal failure");
  if (error != nil && error.localizedDescription.length > 0) {
    result += ": ";
    result += error.localizedDescription.UTF8String;
  }
  return result;
}

inline id<MTLSamplerState>
MakeNormalizedClampSampler(id<MTLDevice> device,
                           const MTLSamplerMinMagFilter filter,
                           NSString *label = nil) {
  auto *descriptor = [[MTLSamplerDescriptor alloc] init];
  descriptor.normalizedCoordinates = YES;
  descriptor.minFilter = filter;
  descriptor.magFilter = filter;
  descriptor.mipFilter = MTLSamplerMipFilterNotMipmapped;
  descriptor.sAddressMode = MTLSamplerAddressModeClampToEdge;
  descriptor.tAddressMode = MTLSamplerAddressModeClampToEdge;
  descriptor.rAddressMode = MTLSamplerAddressModeClampToEdge;
  descriptor.maxAnisotropy = 1U;
  if (label != nil)
    descriptor.label = label;
  id<MTLSamplerState> sampler =
      [device newSamplerStateWithDescriptor:descriptor];
  [descriptor release];
  return sampler;
}

template <typename Index, std::size_t VertexCount, std::size_t IndexCount>
std::unique_ptr<MetalTexturedPipelineState> MakeTexturedMetalPipelineState(
    id<MTLDevice> device, const char *source, NSString *vertexName,
    NSString *fragmentName, NSString *effectName,
    const MetalTexturedVertex (&vertices)[VertexCount],
    const Index (&indices)[IndexCount], std::string &error) {
  const auto message = [effectName](NSString *suffix) {
    return [effectName stringByAppendingString:suffix];
  };
  auto state = std::make_unique<MetalTexturedPipelineState>();
  state->queue = [device newCommandQueue];
  if (state->queue == nil) {
    error = MetalErrorMessage(message(@" Metal command queue creation failed"),
                              nil);
    return {};
  }

  NSError *libraryError = nil;
  id<MTLLibrary> library =
      [device newLibraryWithSource:[NSString stringWithUTF8String:source]
                           options:nil
                             error:&libraryError];
  if (library == nil) {
    error = MetalErrorMessage(message(@" Metal shader compilation failed"),
                              libraryError);
    return {};
  }
  id<MTLFunction> vertex = [library newFunctionWithName:vertexName];
  id<MTLFunction> fragment = [library newFunctionWithName:fragmentName];
  if (vertex == nil || fragment == nil) {
    error = MetalErrorMessage(
        message(@" Metal shader entry point is unavailable"), nil);
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
      error = MetalErrorMessage(message(@" Metal pipeline creation failed"),
                                pipelineError);
    }
  }
  [fragment release];
  [vertex release];
  [library release];
  if (state->pipeline == nil)
    return {};

  state->sampler =
      MakeNormalizedClampSampler(device, MTLSamplerMinMagFilterLinear,
                                 message(@" normalized linear clamp"));
  state->vertexBuffer =
      [device newBufferWithBytes:vertices
                          length:sizeof(vertices)
                         options:MTLResourceStorageModeShared];
  state->indexBuffer = [device newBufferWithBytes:indices
                                           length:sizeof(indices)
                                          options:MTLResourceStorageModeShared];
  if (state->sampler == nil || state->vertexBuffer == nil ||
      state->indexBuffer == nil) {
    error = MetalErrorMessage(
        message(@" immutable Metal resource creation failed"), nil);
    return {};
  }
  error.clear();
  return state;
}

inline NSUInteger AlignedRgba8RowBytes(id<MTLDevice> device, const int width) {
  const NSUInteger compact = static_cast<NSUInteger>(width) * 4U;
  const NSUInteger alignment = std::max<NSUInteger>(
      1U, [device minimumLinearTextureAlignmentForPixelFormat:
                      MTLPixelFormatRGBA8Unorm]);
  return ((compact + alignment - 1U) / alignment) * alignment;
}

inline id<MTLTexture> MakePrivateTexture2D(id<MTLDevice> device,
                                           const MTLPixelFormat format,
                                           const int width, const int height,
                                           const MTLTextureUsage usage) {
  MTLTextureDescriptor *descriptor = [MTLTextureDescriptor
      texture2DDescriptorWithPixelFormat:format
                                   width:static_cast<NSUInteger>(width)
                                  height:static_cast<NSUInteger>(height)
                               mipmapped:NO];
  descriptor.storageMode = MTLStorageModePrivate;
  descriptor.usage = usage;
  return [device newTextureWithDescriptor:descriptor];
}

inline id<MTLTexture> MakePrivateRgba8Texture(id<MTLDevice> device,
                                              const int width, const int height,
                                              const MTLTextureUsage usage) {
  return MakePrivateTexture2D(device, MTLPixelFormatRGBA8Unorm, width, height,
                              usage);
}

inline id<MTLTexture> MakePrivateDepthTexture(id<MTLDevice> device,
                                              const int width,
                                              const int height) {
  return MakePrivateTexture2D(device, MTLPixelFormatDepth32Float, width, height,
                              MTLTextureUsageRenderTarget);
}

inline void CopyVerticallyFlippedRows(void *destination,
                                      const std::size_t destinationRowBytes,
                                      const void *source,
                                      const std::size_t sourceRowBytes,
                                      const std::size_t rowBytes,
                                      const int height) {
  auto *destinationBytes = static_cast<std::uint8_t *>(destination);
  const auto *sourceBytes = static_cast<const std::uint8_t *>(source);
  for (int destinationRow = 0; destinationRow < height; ++destinationRow) {
    const int sourceRow = height - 1 - destinationRow;
    std::memcpy(destinationBytes + static_cast<std::size_t>(destinationRow) *
                                       destinationRowBytes,
                sourceBytes +
                    static_cast<std::size_t>(sourceRow) * sourceRowBytes,
                rowBytes);
  }
}

inline void CopyBufferToTexture(id<MTLBlitCommandEncoder> encoder,
                                id<MTLBuffer> source,
                                id<MTLTexture> destination, const MTLSize size,
                                const NSUInteger rowBytes,
                                const NSUInteger imageBytes) {
  [encoder copyFromBuffer:source
             sourceOffset:0U
        sourceBytesPerRow:rowBytes
      sourceBytesPerImage:imageBytes
               sourceSize:size
                toTexture:destination
         destinationSlice:0U
         destinationLevel:0U
        destinationOrigin:MTLOriginMake(0U, 0U, 0U)];
}

inline void CopyTextureToBuffer(id<MTLBlitCommandEncoder> encoder,
                                id<MTLTexture> source,
                                id<MTLBuffer> destination, const MTLSize size,
                                const NSUInteger rowBytes,
                                const NSUInteger imageBytes,
                                const NSUInteger destinationOffset = 0U) {
  [encoder copyFromTexture:source
                   sourceSlice:0U
                   sourceLevel:0U
                  sourceOrigin:MTLOriginMake(0U, 0U, 0U)
                    sourceSize:size
                      toBuffer:destination
             destinationOffset:destinationOffset
        destinationBytesPerRow:rowBytes
      destinationBytesPerImage:imageBytes];
}

} // namespace videocut::skia_runtime::internal
