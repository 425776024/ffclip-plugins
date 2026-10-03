#pragma once

#include "text/NativeCommandSubmission.h"
#import <Metal/Metal.h>
#include <initializer_list>
#include <limits>

namespace videocut::skia_runtime::internal {

inline bool IsValidNativeRgba8TexturePair(id<MTLTexture> source,
                                          id<MTLTexture> output,
                                          id<MTLCommandQueue> queue,
                                          id<MTLDevice> device, const int width,
                                          const int height) {
  const auto valid = [&](id<MTLTexture> texture, MTLTextureUsage usage) {
    return texture && texture.device == device &&
           texture.textureType == MTLTextureType2D &&
           texture.sampleCount == 1 &&
           texture.pixelFormat == MTLPixelFormatRGBA8Unorm &&
           texture.width == static_cast<NSUInteger>(width) &&
           texture.height == static_cast<NSUInteger>(height) &&
           (texture.usage & usage) == usage;
  };
  return queue && queue.device == device && source != output &&
         valid(source, MTLTextureUsageShaderRead) &&
         valid(output, MTLTextureUsageRenderTarget);
}

inline bool ValidateNativeRgba8Target(
    void *sourceHandle, int width, int height,
    const NativeRgba8TextureTarget *target, id<MTLDevice> device,
    std::string &error) {
  if (!sourceHandle && !target) return true;
  id<MTLTexture> source = (id<MTLTexture>)sourceHandle;
  id<MTLTexture> output = target ? (id<MTLTexture>)target->texture : nil;
  id<MTLCommandQueue> queue = target ? (id<MTLCommandQueue>)target->commandQueue : nil;
  if (!IsValidNativeRgba8TexturePair(source, output, queue, device, width,
                                     height)) {
    error = "native text source, target or queue violates the RGBA8 contract";
    return false;
  }
  return true;
}

inline bool SubmitNativeTextureCommand(
    id<MTLCommandBuffer> command, const NativeCommandSubmission &submit,
    std::initializer_list<id<MTLResource>> resources, std::string &error) {
  if (!command) { error = "native text command is unavailable"; return false; }
  if (!submit) {
    [command commit];
    return true;
  }
  std::size_t bytes = 0;
  for (id<MTLResource> resource : resources) {
    if (resource.allocatedSize > std::numeric_limits<std::size_t>::max() - bytes) {
      error = "native text retained-resource size overflow";
      return false;
    }
    bytes += resource.allocatedSize;
  }
  return submit((void *)command, bytes, error);
}

} // namespace videocut::skia_runtime::internal
