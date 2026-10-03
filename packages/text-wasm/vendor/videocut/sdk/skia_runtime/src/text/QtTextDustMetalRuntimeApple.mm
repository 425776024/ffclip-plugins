#include "runtime/MetalRuntimeSupport.h"
#include "text/NativeMetalSubmission.h"
#include "text/shaders/MetalDustFullscreenVertex.h"
#include "text/shaders/MetalDustMaskNoiseFragment.h"
#include "text/shaders/MetalDustNoiseFragment.h"
#include "text/shaders/MetalDustParticleFragment.h"
#include "text/shaders/MetalDustParticleVertex.h"

#if __cplusplus >= 202002L && !defined(INFINITY)
// The CommandLineTools macOS SDK still references the C INFINITY macro from
// Metal C++ headers even though libc++20 intentionally does not export it.
#define VIDEOCUT_QT_DUST_UNDEF_INFINITY 1
#define INFINITY (__builtin_inff())
#endif

#import <Foundation/Foundation.h>
#import <Metal/Metal.h>

#include "text/QtTextDustMetalRuntime.h"

#include "videocut/gpu_execution/GpuExecution.h"

#include <simd/simd.h>

#if defined(VIDEOCUT_QT_DUST_UNDEF_INFINITY)
#undef INFINITY
#undef VIDEOCUT_QT_DUST_UNDEF_INFINITY
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <string>
#include <utility>

namespace videocut::skia_runtime::internal {
namespace {

// These programs are embedded verbatim from the LumiDust payload. They are
// compiled as separate MTLLibraries because each generated source intentionally
// exports the same entry point name, main0.
// Payload shader SHA-256:
// 9fae33923cf6740f83a7a2ae5b6eb7d0a40d7cd23de5e393fb4a4cdcf9f5878e
constexpr const char *kDustFullscreenVertexSource =
    shader_sources::MetalDustFullscreenVertex;
// Payload shader SHA-256:
// b48437ed640732822299df384bf92125287a2e432f5457a7595135316bdd892e
constexpr const char *kDustNoiseFragmentSource =
    shader_sources::MetalDustNoiseFragment;
// Payload shader SHA-256:
// 335048c092251718bcd4b430ab89e06d90853ced096a75d0553ce044b518de3a
constexpr const char *kDustMaskNoiseFragmentSource =
    shader_sources::MetalDustMaskNoiseFragment;
// Payload shader SHA-256:
// 5271958fd731f4cfb13ea53cf228168c56a1277e5467bfb61492f245465890dd
constexpr const char *kDustParticleVertexSource =
    shader_sources::MetalDustParticleVertex;
// Payload shader SHA-256:
// 992017c8eeb56526694a36a687eda295656e9115a8d775d23009332dfb3bf755
constexpr const char *kDustParticleFragmentSource =
    shader_sources::MetalDustParticleFragment;

struct alignas(16) DustNoiseUniforms final {
  float cycle;
  vector_float4 screenParams;
  vector_float2 offset;
  float rotate;
  vector_float2 scale;
  float type;
  float complexity;
  float evolution;
  float subImpact;
  float subScale;
  float subRotate;
  vector_float2 subOffset;
  float brightness;
  float contrast;
};

struct alignas(16) DustParticleUniforms final {
  vector_float4 screenParams;
  float maskLineRotation;
  float distortionIntensity;
  float gravity;
  float gravityRotation;
  std::int32_t maskType;
  float noiseFeather;
  float progress;
  float maskLineFeather;
};

static_assert(sizeof(DustNoiseUniforms) == 96U);
static_assert(offsetof(DustNoiseUniforms, cycle) == 0U);
static_assert(offsetof(DustNoiseUniforms, screenParams) == 16U);
static_assert(offsetof(DustNoiseUniforms, offset) == 32U);
static_assert(offsetof(DustNoiseUniforms, rotate) == 40U);
static_assert(offsetof(DustNoiseUniforms, scale) == 48U);
static_assert(offsetof(DustNoiseUniforms, type) == 56U);
static_assert(offsetof(DustNoiseUniforms, subOffset) == 80U);
static_assert(offsetof(DustNoiseUniforms, contrast) == 92U);

static_assert(sizeof(DustParticleUniforms) == 48U);
static_assert(offsetof(DustParticleUniforms, screenParams) == 0U);
static_assert(offsetof(DustParticleUniforms, maskLineRotation) == 16U);
static_assert(offsetof(DustParticleUniforms, maskType) == 32U);
static_assert(offsetof(DustParticleUniforms, progress) == 40U);

constexpr std::array<float, 36U> kDustFullscreenVertices{
    -1.0F, -1.0F, 0.0F, 1.0F, 0.0F, 0.0F, 1.0F, 0.0F, 0.0F,
     1.0F, -1.0F, 0.0F, 1.0F, 0.0F, 0.0F, 1.0F, 1.0F, 0.0F,
     1.0F,  1.0F, 0.0F, 1.0F, 0.0F, 0.0F, 1.0F, 1.0F, 1.0F,
    -1.0F,  1.0F, 0.0F, 1.0F, 0.0F, 0.0F, 1.0F, 0.0F, 1.0F,
};
constexpr std::array<std::uint32_t, 6U> kDustFullscreenIndices{
    0U, 1U, 2U, 0U, 2U, 3U,
};
constexpr std::array<float, 5U> kDustPointVertex{
    0.0F, 0.0F, 14.5F, 1.0F, 1.0F,
};
constexpr std::array<std::uint16_t, 1U> kDustPointIndex{0U};

id<MTLFunction> CompilePayloadFunction(id<MTLDevice> device,
                                       const char *source,
                                       NSString *label,
                                       std::string &error) {
  NSString *metalSource =
      [[NSString alloc] initWithBytes:source
                              length:std::strlen(source)
                            encoding:NSUTF8StringEncoding];
  if (metalSource == nil) {
    error = "LumiDust Metal source is not valid UTF-8";
    return nil;
  }
  NSError *libraryError = nil;
  id<MTLLibrary> library =
      [device newLibraryWithSource:metalSource
                          options:nil
                            error:&libraryError];
  [metalSource release];
  if (library == nil) {
    error = MetalErrorMessage(label, libraryError);
    return nil;
  }
  id<MTLFunction> function = [library newFunctionWithName:@"main0"];
  [library release];
  if (function == nil) {
    error = std::string(label.UTF8String) +
            ": payload entry point main0 is unavailable";
  }
  return function;
}

MTLVertexDescriptor *MakeFullscreenVertexDescriptor() {
  auto *descriptor = [[MTLVertexDescriptor alloc] init];
  descriptor.attributes[0].format = MTLVertexFormatFloat3;
  descriptor.attributes[0].offset = 0U;
  descriptor.attributes[0].bufferIndex = 30U;
  descriptor.attributes[2].format = MTLVertexFormatFloat2;
  descriptor.attributes[2].offset = 7U * sizeof(float);
  descriptor.attributes[2].bufferIndex = 30U;
  descriptor.layouts[30].stride = 9U * sizeof(float);
  descriptor.layouts[30].stepFunction = MTLVertexStepFunctionPerVertex;
  descriptor.layouts[30].stepRate = 1U;
  return descriptor;
}

MTLVertexDescriptor *MakePointVertexDescriptor() {
  auto *descriptor = [[MTLVertexDescriptor alloc] init];
  descriptor.attributes[0].format = MTLVertexFormatFloat3;
  descriptor.attributes[0].offset = 0U;
  descriptor.attributes[0].bufferIndex = 30U;
  descriptor.attributes[2].format = MTLVertexFormatFloat2;
  descriptor.attributes[2].offset = 3U * sizeof(float);
  descriptor.attributes[2].bufferIndex = 30U;
  descriptor.layouts[30].stride = 5U * sizeof(float);
  descriptor.layouts[30].stepFunction = MTLVertexStepFunctionPerVertex;
  descriptor.layouts[30].stepRate = 1U;
  return descriptor;
}

id<MTLRenderPipelineState>
MakePayloadPipeline(id<MTLDevice> device,
                    id<MTLFunction> vertexFunction,
                    id<MTLFunction> fragmentFunction,
                    MTLVertexDescriptor *vertexDescriptor,
                    const bool particle,
                    NSString *label,
                    std::string &error) {
  auto *descriptor = [[MTLRenderPipelineDescriptor alloc] init];
  descriptor.label = label;
  descriptor.vertexFunction = vertexFunction;
  descriptor.fragmentFunction = fragmentFunction;
  descriptor.vertexDescriptor = vertexDescriptor;
  descriptor.rasterSampleCount = 1U;
  descriptor.inputPrimitiveTopology =
      particle ? MTLPrimitiveTopologyClassPoint
               : MTLPrimitiveTopologyClassTriangle;
  auto *color = descriptor.colorAttachments[0];
  color.pixelFormat = MTLPixelFormatRGBA8Unorm;
  color.writeMask = MTLColorWriteMaskAll;
  if (particle) {
    // Captured AmazingEngine state: both RGB and alpha use
    // SourceAlpha / OneMinusSourceAlpha.
    color.blendingEnabled = YES;
    color.rgbBlendOperation = MTLBlendOperationAdd;
    color.alphaBlendOperation = MTLBlendOperationAdd;
    color.sourceRGBBlendFactor = MTLBlendFactorSourceAlpha;
    color.destinationRGBBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    color.sourceAlphaBlendFactor = MTLBlendFactorSourceAlpha;
    color.destinationAlphaBlendFactor = MTLBlendFactorOneMinusSourceAlpha;
    descriptor.depthAttachmentPixelFormat = MTLPixelFormatDepth32Float;
  }

  NSError *pipelineError = nil;
  id<MTLRenderPipelineState> pipeline =
      [device newRenderPipelineStateWithDescriptor:descriptor
                                             error:&pipelineError];
  [descriptor release];
  if (pipeline == nil)
    error = MetalErrorMessage(label, pipelineError);
  return pipeline;
}

struct UploadedTexture final {
  id<MTLTexture> texture{nil};
  id<MTLBuffer> staging{nil};
  NSUInteger stagingRowBytes{0U};

  void Release() {
    [staging release];
    [texture release];
    staging = nil;
    texture = nil;
    stagingRowBytes = 0U;
  }
};

bool ValidateImageView(const QtTextDustRawRgba8ImageView &image,
                       const char *name,
                       std::string &error) {
  if ((image.pixels == nullptr && image.nativeTexture == nullptr) || image.width <= 0 || image.height <= 0) {
    error = std::string("LumiDust ") + name +
            " has null pixels or non-positive dimensions";
    return false;
  }
  if (image.nativeTexture) return true;
  const auto width = static_cast<std::size_t>(image.width);
  const auto height = static_cast<std::size_t>(image.height);
  if (width > std::numeric_limits<std::size_t>::max() / 4U) {
    error = std::string("LumiDust ") + name + " row size overflows";
    return false;
  }
  const std::size_t compactRowBytes = width * 4U;
  if (image.rowBytes < compactRowBytes) {
    error = std::string("LumiDust ") + name +
            " rowBytes is smaller than width * 4";
    return false;
  }
  if (height > 1U &&
      image.rowBytes >
          (std::numeric_limits<std::size_t>::max() - compactRowBytes) /
              (height - 1U)) {
    error = std::string("LumiDust ") + name + " byte extent overflows";
    return false;
  }
  const std::size_t required =
      (height - 1U) * image.rowBytes + compactRowBytes;
  if (image.byteSize < required) {
    error = std::string("LumiDust ") + name +
            " byteSize does not cover the final row";
    return false;
  }
  return true;
}

bool FiniteNoiseParameters(const QtTextDustNoiseParameters &parameters) {
  return std::isfinite(parameters.brightness) &&
         std::isfinite(parameters.contrast) &&
         std::isfinite(parameters.quantity) &&
         std::isfinite(parameters.complexity) &&
         std::isfinite(parameters.evolutionDegrees) &&
         std::isfinite(parameters.offsetX) &&
         std::isfinite(parameters.offsetY) &&
         std::isfinite(parameters.rotateDegrees) &&
         std::isfinite(parameters.type) &&
         std::isfinite(parameters.subImpact) &&
         std::isfinite(parameters.subScale) &&
         std::isfinite(parameters.subRotateDegrees) &&
         std::isfinite(parameters.subOffsetX) &&
         std::isfinite(parameters.subOffsetY) &&
         std::isfinite(parameters.pictureScale) &&
         parameters.quantity != 0.0F &&
         parameters.subScale != 0.0F &&
         parameters.pictureScale != 0.0F;
}

bool ValidateRequest(const QtTextDustRenderRequest &request,
                     std::string &error) {
  if (request.implementationVersion != 1U) {
    error = "LumiDust renderer ABI version is unsupported";
    return false;
  }
  if (!ValidateImageView(request.source, "source", error))
    return false;
  if (request.noiseTextureWidth <= 0 || request.noiseTextureHeight <= 0) {
    error = "LumiDust procedural-noise texture dimensions must be positive";
    return false;
  }
  if (!FiniteNoiseParameters(request.parameters.noise) ||
      !FiniteNoiseParameters(request.parameters.maskNoise) ||
      !std::isfinite(request.parameters.distortionIntensity) ||
      !std::isfinite(request.parameters.gravity) ||
      !std::isfinite(request.parameters.gravityRotationDegrees) ||
      !std::isfinite(request.parameters.maskFeather) ||
      !std::isfinite(request.parameters.maskLineRotationRadians) ||
      !std::isfinite(request.progressPercent)) {
    error = "LumiDust parameters must be finite and noise divisors non-zero";
    return false;
  }

  const auto maskType =
      static_cast<std::int32_t>(request.parameters.maskType);
  if (maskType < static_cast<std::int32_t>(QtTextDustMaskType::Noise) ||
      maskType > static_cast<std::int32_t>(QtTextDustMaskType::Image)) {
    error = "LumiDust mask type is invalid";
    return false;
  }
  if (request.parameters.maskType == QtTextDustMaskType::Image &&
      !ValidateImageView(request.parameters.maskImage, "mask image", error)) {
    return false;
  }

  const auto width = static_cast<std::size_t>(request.source.width);
  const auto height = static_cast<std::size_t>(request.source.height);
  if (width > std::numeric_limits<std::size_t>::max() / height ||
      width * height >
          static_cast<std::size_t>(
              std::numeric_limits<NSUInteger>::max())) {
    error = "LumiDust point instance count overflows NSUInteger";
    return false;
  }
  return true;
}

UploadedTexture
MakeUploadedTexture(id<MTLDevice> device,
                    const QtTextDustRawRgba8ImageView &image,
                    std::string &error) {
  UploadedTexture result;
  if (image.nativeTexture) {
    id<MTLTexture> native = (id<MTLTexture>)image.nativeTexture;
    if (native.device != device || native.pixelFormat != MTLPixelFormatRGBA8Unorm ||
        native.textureType != MTLTextureType2D || native.sampleCount != 1U ||
        native.width != static_cast<NSUInteger>(image.width) ||
        native.height != static_cast<NSUInteger>(image.height) ||
        !(native.usage & MTLTextureUsageShaderRead)) {
      error = "LumiDust borrowed source texture is incompatible";
      return result;
    }
    result.texture = [native retain];
    return result;
  }
  result.texture = MakePrivateRgba8Texture(device, image.width, image.height,
                                           MTLTextureUsageShaderRead);
  result.stagingRowBytes = AlignedRgba8RowBytes(device, image.width);
  const std::size_t stagingLength =
      static_cast<std::size_t>(result.stagingRowBytes) *
      static_cast<std::size_t>(image.height);
  result.staging =
      [device newBufferWithLength:stagingLength
                         options:MTLResourceStorageModeShared];
  if (result.texture == nil || result.staging == nil) {
    error = "LumiDust source texture upload allocation failed";
    result.Release();
    return {};
  }

  // AmazingEngine's negative-height viewport stores its internal textures in
  // the opposite row order from the sidecar's top-left public byte contract.
  // Flip only transport rows; channel bytes remain untouched.
  auto *destination =
      static_cast<std::uint8_t *>(result.staging.contents);
  const std::size_t compactRowBytes =
      static_cast<std::size_t>(image.width) * 4U;
  CopyVerticallyFlippedRows(
      destination, static_cast<std::size_t>(result.stagingRowBytes),
      image.pixels, image.rowBytes, compactRowBytes, image.height);
  return result;
}

bool EncodeTextureUpload(id<MTLBlitCommandEncoder> blit,
                         const UploadedTexture &upload,
                         const int width,
                         const int height) {
  if (upload.texture != nil && upload.staging == nil) return true;
  if (blit == nil || upload.texture == nil || upload.staging == nil)
    return false;
  CopyBufferToTexture(blit, upload.staging, upload.texture,
                      MTLSizeMake(static_cast<NSUInteger>(width),
                                  static_cast<NSUInteger>(height), 1U),
                      upload.stagingRowBytes,
                      upload.stagingRowBytes * static_cast<NSUInteger>(height));
  return true;
}

DustNoiseUniforms
ResolveNoiseUniforms(const QtTextDustNoiseParameters &parameters,
                     const int width,
                     const int height) {
  DustNoiseUniforms uniforms{};
  uniforms.cycle = static_cast<float>(
      std::max(2, static_cast<int>(
                      std::floor(static_cast<float>(parameters.cycle) *
                                     3.0F +
                                 0.5F))));
  uniforms.screenParams =
      vector_float4{static_cast<float>(width), static_cast<float>(height),
                    1.0F + 1.0F / static_cast<float>(width),
                    1.0F + 1.0F / static_cast<float>(height)};
  uniforms.offset = vector_float2{parameters.offsetX, parameters.offsetY};
  uniforms.rotate = parameters.rotateDegrees;
  uniforms.scale = vector_float2{parameters.quantity, parameters.quantity};
  uniforms.type = parameters.type;
  uniforms.complexity = parameters.complexity;
  uniforms.evolution = std::abs(parameters.evolutionDegrees) / 90.0F;
  uniforms.subImpact = parameters.subImpact;
  uniforms.subScale = parameters.subScale;
  uniforms.subRotate = parameters.subRotateDegrees;
  uniforms.subOffset =
      vector_float2{parameters.subOffsetX, parameters.subOffsetY};
  uniforms.brightness = parameters.brightness;
  uniforms.contrast = parameters.contrast;
  return uniforms;
}

DustParticleUniforms
ResolveParticleUniforms(const QtTextDustRenderRequest &request) {
  DustParticleUniforms uniforms{};
  uniforms.screenParams =
      vector_float4{static_cast<float>(request.source.width),
                    static_cast<float>(request.source.height),
                    1.0F + 1.0F / static_cast<float>(request.source.width),
                    1.0F + 1.0F / static_cast<float>(request.source.height)};
  uniforms.maskLineRotation =
      request.parameters.maskLineRotationRadians;
  uniforms.distortionIntensity =
      request.parameters.distortionIntensity;
  uniforms.gravity = request.parameters.gravity;
  uniforms.gravityRotation =
      ((360.0F - request.parameters.gravityRotationDegrees) * 3.141F) /
      180.0F;
  uniforms.maskType =
      static_cast<std::int32_t>(request.parameters.maskType);
  uniforms.noiseFeather = request.parameters.maskFeather;
  uniforms.progress = request.progressPercent / 100.0F;
  // LumiDust only writes mask_line_feather for the Line mask; leave the
  // inactive field zero-initialized.
  if (request.parameters.maskType == QtTextDustMaskType::Line) {
    uniforms.maskLineFeather = request.parameters.maskFeather * 0.5F;
  }
  return uniforms;
}

struct QtTextDustMetalState final {
  id<MTLCommandQueue> queue{nil};
  id<MTLRenderPipelineState> noisePipeline{nil};
  id<MTLRenderPipelineState> maskNoisePipeline{nil};
  id<MTLRenderPipelineState> particlePipeline{nil};
  id<MTLDepthStencilState> particleDepthStencil{nil};
  id<MTLSamplerState> linearSampler{nil};
  id<MTLSamplerState> nearestSampler{nil};
  id<MTLBuffer> fullscreenVertexBuffer{nil};
  id<MTLBuffer> fullscreenIndexBuffer{nil};
  id<MTLBuffer> pointVertexBuffer{nil};
  id<MTLBuffer> pointIndexBuffer{nil};
  id<MTLTexture> dummyTexture{nil};

  ~QtTextDustMetalState() {
    [dummyTexture release];
    [pointIndexBuffer release];
    [pointVertexBuffer release];
    [fullscreenIndexBuffer release];
    [fullscreenVertexBuffer release];
    [nearestSampler release];
    [linearSampler release];
    [particleDepthStencil release];
    [particlePipeline release];
    [maskNoisePipeline release];
    [noisePipeline release];
    [queue release];
  }
};

bool InitializeDummyTexture(id<MTLDevice> device,
                            QtTextDustMetalState &state,
                            std::string &error) {
  state.dummyTexture =
      MakePrivateRgba8Texture(device, 1, 1, MTLTextureUsageShaderRead);
  const NSUInteger rowBytes = AlignedRgba8RowBytes(device, 1);
  id<MTLBuffer> staging =
      [device newBufferWithLength:rowBytes
                         options:MTLResourceStorageModeShared];
  if (state.dummyTexture == nil || staging == nil) {
    error = "LumiDust dummy texture allocation failed";
    [staging release];
    return false;
  }
  std::memset(staging.contents, 0, rowBytes);
  id<MTLCommandBuffer> commandBuffer = [state.queue commandBuffer];
  id<MTLBlitCommandEncoder> blit = [commandBuffer blitCommandEncoder];
  if (commandBuffer == nil || blit == nil) {
    error = "LumiDust dummy texture upload encoder creation failed";
    [staging release];
    return false;
  }
  CopyBufferToTexture(blit, staging, state.dummyTexture,
                      MTLSizeMake(1U, 1U, 1U), rowBytes, rowBytes);
  [blit endEncoding];
  [commandBuffer commit];
  [commandBuffer waitUntilCompleted];
  [staging release];
  if (commandBuffer.status != MTLCommandBufferStatusCompleted) {
    error = MetalErrorMessage(@"LumiDust dummy texture upload failed",
                              commandBuffer.error);
    return false;
  }
  return true;
}

std::unique_ptr<QtTextDustMetalState>
CreateMetalState(id<MTLDevice> device, std::string &error) {
  auto state = std::make_unique<QtTextDustMetalState>();
  state->queue = [device newCommandQueue];
  if (state->queue == nil) {
    error = "LumiDust Metal command queue creation failed";
    return {};
  }

  id<MTLFunction> fullscreenVertex =
      CompilePayloadFunction(device, kDustFullscreenVertexSource,
                             @"LumiDust fullscreen vertex compilation failed",
                             error);
  id<MTLFunction> noiseFragment = nil;
  id<MTLFunction> maskNoiseFragment = nil;
  id<MTLFunction> particleVertex = nil;
  id<MTLFunction> particleFragment = nil;
  if (fullscreenVertex != nil) {
    noiseFragment =
        CompilePayloadFunction(device, kDustNoiseFragmentSource,
                               @"LumiDust noise fragment compilation failed",
                               error);
  }
  if (noiseFragment != nil) {
    maskNoiseFragment =
        CompilePayloadFunction(
            device, kDustMaskNoiseFragmentSource,
            @"LumiDust mask-noise fragment compilation failed", error);
  }
  if (maskNoiseFragment != nil) {
    particleVertex =
        CompilePayloadFunction(device, kDustParticleVertexSource,
                               @"LumiDust particle vertex compilation failed",
                               error);
  }
  if (particleVertex != nil) {
    particleFragment =
        CompilePayloadFunction(
            device, kDustParticleFragmentSource,
            @"LumiDust particle fragment compilation failed", error);
  }

  MTLVertexDescriptor *fullscreenDescriptor = nil;
  MTLVertexDescriptor *pointDescriptor = nil;
  if (particleFragment != nil) {
    fullscreenDescriptor = MakeFullscreenVertexDescriptor();
    pointDescriptor = MakePointVertexDescriptor();
    state->noisePipeline =
        MakePayloadPipeline(device, fullscreenVertex, noiseFragment,
                            fullscreenDescriptor, false,
                            @"LumiDust noise pipeline", error);
    if (state->noisePipeline != nil) {
      state->maskNoisePipeline =
          MakePayloadPipeline(device, fullscreenVertex, maskNoiseFragment,
                              fullscreenDescriptor, false,
                              @"LumiDust mask-noise pipeline", error);
    }
    if (state->maskNoisePipeline != nil) {
      state->particlePipeline =
          MakePayloadPipeline(device, particleVertex, particleFragment,
                              pointDescriptor, true,
                              @"LumiDust particle pipeline", error);
    }
  }

  [pointDescriptor release];
  [fullscreenDescriptor release];
  [particleFragment release];
  [particleVertex release];
  [maskNoiseFragment release];
  [noiseFragment release];
  [fullscreenVertex release];
  if (state->noisePipeline == nil ||
      state->maskNoisePipeline == nil ||
      state->particlePipeline == nil) {
    return {};
  }

  auto *depthDescriptor = [[MTLDepthStencilDescriptor alloc] init];
  depthDescriptor.depthCompareFunction = MTLCompareFunctionAlways;
  depthDescriptor.depthWriteEnabled = NO;
  state->particleDepthStencil =
      [device newDepthStencilStateWithDescriptor:depthDescriptor];
  [depthDescriptor release];

  state->linearSampler = MakeNormalizedClampSampler(
      device, MTLSamplerMinMagFilterLinear, @"LumiDust linear sampler");
  state->nearestSampler = MakeNormalizedClampSampler(
      device, MTLSamplerMinMagFilterNearest, @"LumiDust nearest sampler");
  state->fullscreenVertexBuffer =
      [device newBufferWithBytes:kDustFullscreenVertices.data()
                          length:sizeof(kDustFullscreenVertices)
                         options:MTLResourceStorageModeShared];
  state->fullscreenIndexBuffer =
      [device newBufferWithBytes:kDustFullscreenIndices.data()
                          length:sizeof(kDustFullscreenIndices)
                         options:MTLResourceStorageModeShared];
  state->pointVertexBuffer =
      [device newBufferWithBytes:kDustPointVertex.data()
                          length:sizeof(kDustPointVertex)
                         options:MTLResourceStorageModeShared];
  state->pointIndexBuffer =
      [device newBufferWithBytes:kDustPointIndex.data()
                          length:sizeof(kDustPointIndex)
                         options:MTLResourceStorageModeShared];

  if (state->particleDepthStencil == nil ||
      state->linearSampler == nil || state->nearestSampler == nil ||
      state->fullscreenVertexBuffer == nil ||
      state->fullscreenIndexBuffer == nil ||
      state->pointVertexBuffer == nil ||
      state->pointIndexBuffer == nil) {
    error = "LumiDust immutable Metal resource creation failed";
    return {};
  }
  if (!InitializeDummyTexture(device, *state, error))
    return {};

  error.clear();
  return state;
}

bool EncodeNoisePass(id<MTLCommandBuffer> commandBuffer,
                     id<MTLRenderPipelineState> pipeline,
                     id<MTLTexture> destination,
                     id<MTLBuffer> fullscreenVertexBuffer,
                     id<MTLBuffer> fullscreenIndexBuffer,
                     const DustNoiseUniforms &uniforms,
                     const float pictureScale,
                     const int width,
                     const int height,
                     NSString *label,
                     std::string &error) {
  MTLRenderPassDescriptor *pass =
      [MTLRenderPassDescriptor renderPassDescriptor];
  pass.colorAttachments[0].texture = destination;
  pass.colorAttachments[0].loadAction = MTLLoadActionClear;
  pass.colorAttachments[0].storeAction = MTLStoreActionStore;
  pass.colorAttachments[0].clearColor =
      MTLClearColorMake(0.0, 0.0, 0.0, 0.0);
  id<MTLRenderCommandEncoder> encoder =
      [commandBuffer renderCommandEncoderWithDescriptor:pass];
  if (encoder == nil) {
    error = std::string(label.UTF8String) +
            ": render encoder creation failed";
    return false;
  }
  encoder.label = label;
  const MTLViewport viewport{
      0.0, static_cast<double>(height),
      static_cast<double>(width), -static_cast<double>(height),
      0.0, 1.0};
  [encoder setViewport:viewport];
  [encoder setRenderPipelineState:pipeline];
  [encoder setVertexBytes:&pictureScale
                   length:sizeof(pictureScale)
                  atIndex:0U];
  [encoder setFragmentBytes:&uniforms
                     length:sizeof(uniforms)
                    atIndex:0U];
  [encoder setVertexBuffer:fullscreenVertexBuffer
                    offset:0U
                   atIndex:30U];
  [encoder drawIndexedPrimitives:MTLPrimitiveTypeTriangle
                      indexCount:kDustFullscreenIndices.size()
                       indexType:MTLIndexTypeUInt32
                     indexBuffer:fullscreenIndexBuffer
               indexBufferOffset:0U
                   instanceCount:1U];
  [encoder endEncoding];
  return true;
}

bool EncodeParticlePass(id<MTLCommandBuffer> commandBuffer,
                        const QtTextDustMetalState &state,
                        id<MTLTexture> noiseTexture,
                        id<MTLTexture> maskNoiseTexture,
                        id<MTLTexture> maskImageTexture,
                        id<MTLTexture> sourceTexture,
                        id<MTLTexture> destination,
                        id<MTLTexture> depthTexture,
                        const DustParticleUniforms &uniforms,
                        const NSUInteger instanceCount,
                        const int width,
                        const int height,
                        std::string &error) {
  MTLRenderPassDescriptor *pass =
      [MTLRenderPassDescriptor renderPassDescriptor];
  pass.colorAttachments[0].texture = destination;
  pass.colorAttachments[0].loadAction = MTLLoadActionClear;
  pass.colorAttachments[0].storeAction = MTLStoreActionStore;
  pass.colorAttachments[0].clearColor =
      MTLClearColorMake(0.0, 0.0, 0.0, 0.0);
  pass.depthAttachment.texture = depthTexture;
  pass.depthAttachment.loadAction = MTLLoadActionClear;
  pass.depthAttachment.storeAction = MTLStoreActionDontCare;
  pass.depthAttachment.clearDepth = 1.0;

  id<MTLRenderCommandEncoder> encoder =
      [commandBuffer renderCommandEncoderWithDescriptor:pass];
  if (encoder == nil) {
    error = "LumiDust particle render encoder creation failed";
    return false;
  }
  encoder.label = @"LumiDust renderParticle";
  const MTLViewport viewport{
      0.0, static_cast<double>(height),
      static_cast<double>(width), -static_cast<double>(height),
      0.0, 1.0};
  [encoder setViewport:viewport];
  [encoder setFrontFacingWinding:MTLWindingCounterClockwise];
  [encoder setCullMode:MTLCullModeBack];
  [encoder setRenderPipelineState:state.particlePipeline];
  [encoder setDepthStencilState:state.particleDepthStencil];
  [encoder setVertexBytes:&uniforms
                   length:sizeof(uniforms)
                  atIndex:0U];
  [encoder setVertexBuffer:state.pointVertexBuffer
                    offset:0U
                   atIndex:30U];
  [encoder setVertexTexture:noiseTexture atIndex:0U];
  [encoder setVertexTexture:maskNoiseTexture atIndex:1U];
  [encoder setVertexTexture:maskImageTexture atIndex:2U];
  [encoder setVertexTexture:sourceTexture atIndex:3U];
  [encoder setVertexSamplerState:state.linearSampler atIndex:0U];
  [encoder setVertexSamplerState:state.linearSampler atIndex:1U];
  [encoder setVertexSamplerState:state.nearestSampler atIndex:2U];
  [encoder setVertexSamplerState:state.linearSampler atIndex:3U];
  [encoder drawIndexedPrimitives:MTLPrimitiveTypePoint
                      indexCount:1U
                       indexType:MTLIndexTypeUInt16
                     indexBuffer:state.pointIndexBuffer
               indexBufferOffset:0U
                   instanceCount:instanceCount];
  [encoder endEncoding];
  return true;
}

class AppleQtTextDustMetalRuntime final : public QtTextDustMetalRuntime {
public:
  static std::unique_ptr<AppleQtTextDustMetalRuntime>
  Create(std::string &error) {
    gpu_execution::DeviceRequest request;
    request.preferred = gpu_execution::Backend::Metal;
    auto executionDevice =
        gpu_execution::Device::Create(request, error);
    if (!executionDevice) {
      if (error.empty())
        error = "product Metal device is unavailable for LumiDust";
      return {};
    }
    if (executionDevice->backend() != gpu_execution::Backend::Metal ||
        executionDevice->nativeDeviceHandle() == 0U) {
      error = "product GPU device is not a usable Metal device for LumiDust";
      return {};
    }
    id<MTLDevice> device = (__bridge id<MTLDevice>)(
        reinterpret_cast<void *>(
            executionDevice->nativeDeviceHandle()));
    if (device == nil) {
      error = "product Metal device native handle is unavailable for LumiDust";
      return {};
    }
    auto state = CreateMetalState(device, error);
    if (!state)
      return {};
    error.clear();
    return std::unique_ptr<AppleQtTextDustMetalRuntime>(
        new AppleQtTextDustMetalRuntime(
            std::move(executionDevice), std::move(state)));
  }

  bool Render(const QtTextDustRenderRequest &request,
              std::vector<std::uint8_t> &outputPixels,
              std::string &error, const NativeRgba8TextureTarget *target) override {
    @autoreleasepool {
      outputPixels.clear();
      if (!ValidateRequest(request, error))
        return false;

      id<MTLDevice> device = (__bridge id<MTLDevice>)(
          reinterpret_cast<void *>(
              executionDevice_->nativeDeviceHandle()));
      if (device == nil || state_ == nullptr) {
        error = "LumiDust Metal runtime lost its product device";
        return false;
      }

      if (!ValidateNativeRgba8Target(request.source.nativeTexture,
          request.source.width, request.source.height, target, device, error))
        return false;
      UploadedTexture sourceUpload =
          MakeUploadedTexture(device, request.source, error);
      if (sourceUpload.texture == nil)
        return false;

      UploadedTexture maskUpload;
      const bool hasMaskImage =
          request.parameters.maskType == QtTextDustMaskType::Image;
      if (hasMaskImage) {
        maskUpload =
            MakeUploadedTexture(device, request.parameters.maskImage, error);
        if (maskUpload.texture == nil) {
          sourceUpload.Release();
          return false;
        }
      }

      id<MTLTexture> noiseTexture = MakePrivateRgba8Texture(
          device, request.noiseTextureWidth, request.noiseTextureHeight,
          MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead);
      id<MTLTexture> maskNoiseTexture = MakePrivateRgba8Texture(
          device, request.noiseTextureWidth, request.noiseTextureHeight,
          MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead);
      id<MTLTexture> outputTexture =
          target ? [(id<MTLTexture>)target->texture retain]
                 : MakePrivateRgba8Texture(
                       device, request.source.width, request.source.height,
                       MTLTextureUsageRenderTarget | MTLTextureUsageShaderRead);
      id<MTLTexture> depthTexture =
          MakePrivateDepthTexture(
              device, request.source.width, request.source.height);

      const NSUInteger readbackRowBytes =
          AlignedRgba8RowBytes(device, request.source.width);
      const std::size_t readbackLength =
          static_cast<std::size_t>(readbackRowBytes) *
          static_cast<std::size_t>(request.source.height);
      id<MTLBuffer> readback = target ? nil :
          [device newBufferWithLength:readbackLength
                             options:MTLResourceStorageModeShared];

      const auto releaseResources = [&]() {
        [readback release];
        [depthTexture release];
        [outputTexture release];
        [maskNoiseTexture release];
        [noiseTexture release];
        maskUpload.Release();
        sourceUpload.Release();
      };
      if (noiseTexture == nil || maskNoiseTexture == nil ||
          outputTexture == nil || depthTexture == nil ||
          (!target && readback == nil)) {
        error = "LumiDust per-frame Metal resource allocation failed";
        releaseResources();
        return false;
      }

      id<MTLCommandBuffer> commandBuffer =
          [(target ? (id<MTLCommandQueue>)target->commandQueue : state_->queue) commandBuffer];
      if (commandBuffer == nil) {
        error = "LumiDust Metal command buffer creation failed";
        releaseResources();
        return false;
      }
      commandBuffer.label = @"Qt LumiDust native sidecar";

      id<MTLBlitCommandEncoder> uploadBlit =
          [commandBuffer blitCommandEncoder];
      if (!EncodeTextureUpload(uploadBlit, sourceUpload,
                               request.source.width,
                               request.source.height) ||
          (hasMaskImage &&
           !EncodeTextureUpload(
               uploadBlit, maskUpload,
               request.parameters.maskImage.width,
               request.parameters.maskImage.height))) {
        error = "LumiDust texture upload encoding failed";
        [uploadBlit endEncoding];
        releaseResources();
        return false;
      }
      [uploadBlit endEncoding];

      const DustNoiseUniforms noiseUniforms =
          ResolveNoiseUniforms(request.parameters.noise,
                               request.noiseTextureWidth,
                               request.noiseTextureHeight);
      const DustNoiseUniforms maskNoiseUniforms =
          ResolveNoiseUniforms(request.parameters.maskNoise,
                               request.noiseTextureWidth,
                               request.noiseTextureHeight);
      id<MTLBuffer> fullscreenIndexBuffer = state_->fullscreenIndexBuffer;
      if (!EncodeNoisePass(
              commandBuffer, state_->noisePipeline, noiseTexture,
              state_->fullscreenVertexBuffer,
              fullscreenIndexBuffer, noiseUniforms,
              request.parameters.noise.pictureScale,
              request.noiseTextureWidth,
              request.noiseTextureHeight,
              @"LumiDust noise", error) ||
          !EncodeNoisePass(
              commandBuffer, state_->maskNoisePipeline,
              maskNoiseTexture, state_->fullscreenVertexBuffer,
              fullscreenIndexBuffer, maskNoiseUniforms,
              request.parameters.maskNoise.pictureScale,
              request.noiseTextureWidth,
              request.noiseTextureHeight,
              @"LumiDust maskNoise", error)) {
        releaseResources();
        return false;
      }

      const DustParticleUniforms particleUniforms =
          ResolveParticleUniforms(request);
      const NSUInteger instanceCount =
          static_cast<NSUInteger>(request.source.width) *
          static_cast<NSUInteger>(request.source.height);
      id<MTLTexture> maskImageTexture =
          hasMaskImage ? maskUpload.texture : state_->dummyTexture;
      if (!EncodeParticlePass(
              commandBuffer, *state_, noiseTexture,
              maskNoiseTexture, maskImageTexture,
              sourceUpload.texture, outputTexture, depthTexture,
              particleUniforms, instanceCount,
              request.source.width, request.source.height, error)) {
        releaseResources();
        return false;
      }

      if (target) {
        const bool submitted = SubmitNativeTextureCommand(commandBuffer, target->submit,
            {sourceUpload.texture, sourceUpload.staging, maskUpload.texture,
             maskUpload.staging, noiseTexture, maskNoiseTexture, outputTexture, depthTexture}, error);
        releaseResources();
        return submitted;
      }
      id<MTLBlitCommandEncoder> readbackBlit =
          [commandBuffer blitCommandEncoder];
      if (readbackBlit == nil) {
        error = "LumiDust readback encoder creation failed";
        releaseResources();
        return false;
      }
      CopyTextureToBuffer(
          readbackBlit, outputTexture, readback,
          MTLSizeMake(static_cast<NSUInteger>(request.source.width),
                      static_cast<NSUInteger>(request.source.height), 1U),
          readbackRowBytes,
          readbackRowBytes * static_cast<NSUInteger>(request.source.height));
      [readbackBlit endEncoding];

      [commandBuffer commit];
      [commandBuffer waitUntilCompleted];
      if (commandBuffer.status != MTLCommandBufferStatusCompleted) {
        error = MetalErrorMessage(
            @"LumiDust Metal command failed", commandBuffer.error);
        releaseResources();
        return false;
      }

      const std::size_t compactRowBytes =
          static_cast<std::size_t>(request.source.width) * 4U;
      outputPixels.resize(
          compactRowBytes *
          static_cast<std::size_t>(request.source.height));
      const auto *source =
          static_cast<const std::uint8_t *>(readback.contents);
      CopyVerticallyFlippedRows(outputPixels.data(), compactRowBytes, source,
                                static_cast<std::size_t>(readbackRowBytes),
                                compactRowBytes, request.source.height);

      releaseResources();
      error.clear();
      return true;
    }
  }

private:
  AppleQtTextDustMetalRuntime(
      std::shared_ptr<gpu_execution::Device> executionDevice,
      std::unique_ptr<QtTextDustMetalState> state)
      : executionDevice_(std::move(executionDevice)),
        state_(std::move(state)) {}

  std::shared_ptr<gpu_execution::Device> executionDevice_;
  std::unique_ptr<QtTextDustMetalState> state_;
};

} // namespace

std::unique_ptr<QtTextDustMetalRuntime>
CreateQtTextDustMetalRuntime(std::string &error) {
  @autoreleasepool {
    return AppleQtTextDustMetalRuntime::Create(error);
  }
}

} // namespace videocut::skia_runtime::internal
