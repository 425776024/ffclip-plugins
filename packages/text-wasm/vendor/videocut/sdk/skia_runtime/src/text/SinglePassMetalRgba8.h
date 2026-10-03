#pragma once

#include "runtime/MetalRuntimeSupport.h"
#include "text/NativeMetalSubmission.h"

#include <vector>

namespace videocut::skia_runtime::internal {

// The caller validates the effect request. Native targets use their supplied
// queue; CPU callers receive vertically flipped RGBA8 pixels after completion.
template <typename Source, typename Target, typename Encode>
bool RenderSinglePassMetalRgba8(id<MTLDevice> device, id<MTLCommandQueue> queue,
                                const Source &source, const Target *target,
                                NSString *effectName, NSString *commandLabel,
                                Encode encode,
                                std::vector<std::uint8_t> &outputPixels,
                                std::string &error) {
  const auto message = [effectName](NSString *suffix) {
    return [effectName stringByAppendingString:suffix];
  };
  const int width = source.width;
  const int height = source.height;
  if (source.nativeTexture || target) {
    if (!source.nativeTexture || !target || !target->texture ||
        !target->commandQueue) {
      error = MetalErrorMessage(
          message(@" native source, target and queue are required"), nil);
      return false;
    }
    id<MTLTexture> input = (id<MTLTexture>)source.nativeTexture;
    id<MTLTexture> output = (id<MTLTexture>)target->texture;
    queue = (id<MTLCommandQueue>)target->commandQueue;
    if (!IsValidNativeRgba8TexturePair(input, output, queue, device, width,
                                       height)) {
      error = MetalErrorMessage(
          message(
              @" native texture device, format, extent or usage is invalid"),
          nil);
      return false;
    }
  }
  id<MTLTexture> sourceTexture =
      target ? [(id<MTLTexture>)source.nativeTexture retain]
             : MakePrivateRgba8Texture(device, width, height,
                                       MTLTextureUsageShaderRead);
  id<MTLTexture> outputTexture =
      target ? [(id<MTLTexture>)target->texture retain]
             : MakePrivateRgba8Texture(device, width, height,
                                       MTLTextureUsageRenderTarget |
                                           MTLTextureUsageShaderRead);
  const NSUInteger transferRowBytes =
      target ? 0U : AlignedRgba8RowBytes(device, width);
  const std::size_t transferLength =
      static_cast<std::size_t>(transferRowBytes) *
      static_cast<std::size_t>(height);
  id<MTLBuffer> upload =
      target ? nil
             : [device newBufferWithLength:transferLength
                                   options:MTLResourceStorageModeShared];
  id<MTLBuffer> readback =
      target ? nil
             : [device newBufferWithLength:transferLength
                                   options:MTLResourceStorageModeShared];
  const auto releaseResources = [&]() {
    [readback release];
    [upload release];
    [outputTexture release];
    [sourceTexture release];
  };
  if (sourceTexture == nil || outputTexture == nil ||
      (!target && (upload == nil || readback == nil))) {
    error = MetalErrorMessage(
        message(@" per-pass Metal resource allocation failed"), nil);
    releaseResources();
    return false;
  }

  const std::size_t compactRowBytes = static_cast<std::size_t>(width) * 4U;
  auto *uploadBytes = static_cast<std::uint8_t *>(upload.contents);
  if (!target) {
    CopyVerticallyFlippedRows(
        uploadBytes, static_cast<std::size_t>(transferRowBytes), source.pixels,
        source.rowBytes, compactRowBytes, height);
  }

  id<MTLCommandBuffer> commandBuffer = [queue commandBuffer];
  if (commandBuffer == nil) {
    error = MetalErrorMessage(message(@" Metal command buffer creation failed"),
                              nil);
    releaseResources();
    return false;
  }
  commandBuffer.label = commandLabel;
  if (!target) {
    id<MTLBlitCommandEncoder> uploadBlit = [commandBuffer blitCommandEncoder];
    if (uploadBlit == nil) {
      error =
          MetalErrorMessage(message(@" upload encoder creation failed"), nil);
      releaseResources();
      return false;
    }
    CopyBufferToTexture(uploadBlit, upload, sourceTexture,
                        MTLSizeMake(static_cast<NSUInteger>(width),
                                    static_cast<NSUInteger>(height), 1U),
                        transferRowBytes,
                        transferRowBytes * static_cast<NSUInteger>(height));
    [uploadBlit endEncoding];
  }

  MTLRenderPassDescriptor *pass =
      [MTLRenderPassDescriptor renderPassDescriptor];
  pass.colorAttachments[0].texture = outputTexture;
  pass.colorAttachments[0].loadAction = MTLLoadActionClear;
  pass.colorAttachments[0].storeAction = MTLStoreActionStore;
  pass.colorAttachments[0].clearColor = MTLClearColorMake(0.0, 0.0, 0.0, 0.0);
  id<MTLRenderCommandEncoder> encoder =
      [commandBuffer renderCommandEncoderWithDescriptor:pass];
  if (encoder == nil) {
    error = MetalErrorMessage(message(@" render encoder creation failed"), nil);
    releaseResources();
    return false;
  }
  encode(encoder, sourceTexture, width, height);
  [encoder endEncoding];

  if (target) {
    // The caller retains the published output; the submitted command buffer
    // retains its resources. Later Ganesh work uses this same ordered queue.
    if (!SubmitNativeTextureCommand(
            commandBuffer, target ? target->submit : NativeCommandSubmission{},
            {readback, upload, outputTexture, sourceTexture}, error)) {
      releaseResources();
      return false;
    }
    releaseResources();
    error.clear();
    return true;
  }

  id<MTLBlitCommandEncoder> readbackBlit = [commandBuffer blitCommandEncoder];
  if (readbackBlit == nil) {
    error =
        MetalErrorMessage(message(@" readback encoder creation failed"), nil);
    releaseResources();
    return false;
  }
  CopyTextureToBuffer(readbackBlit, outputTexture, readback,
                      MTLSizeMake(static_cast<NSUInteger>(width),
                                  static_cast<NSUInteger>(height), 1U),
                      transferRowBytes,
                      transferRowBytes * static_cast<NSUInteger>(height));
  [readbackBlit endEncoding];
  if (!SubmitNativeTextureCommand(
          commandBuffer, target ? target->submit : NativeCommandSubmission{},
          {readback, upload, outputTexture, sourceTexture}, error)) {
    releaseResources();
    return false;
  }
  [commandBuffer waitUntilCompleted];
  if (commandBuffer.status != MTLCommandBufferStatusCompleted) {
    error = MetalErrorMessage(message(@" Metal command failed"),
                              commandBuffer.error);
    releaseResources();
    return false;
  }

  outputPixels.resize(compactRowBytes * static_cast<std::size_t>(height));
  const auto *readbackBytes =
      static_cast<const std::uint8_t *>(readback.contents);
  CopyVerticallyFlippedRows(outputPixels.data(), compactRowBytes, readbackBytes,
                            static_cast<std::size_t>(transferRowBytes),
                            compactRowBytes, height);
  releaseResources();
  error.clear();
  return true;
}

} // namespace videocut::skia_runtime::internal
