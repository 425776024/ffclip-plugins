#pragma once

#include "text/NativeCommandSubmission.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace videocut::skia_runtime::internal {

// A non-owning, top-left-row-first view of raw RGBA8 bytes.  Qt's Dust D3
// output is not constrained to RGB<=A, so "premultiplied" is deliberately not
// part of this ABI. rowBytes may include padding; byteSize protects callers
// from an undersized final row. The Metal sidecar never unpremultiplies,
// color-converts, or clamps RGB against alpha.
struct QtTextDustRawRgba8ImageView final {
  const std::uint8_t *pixels{nullptr};
  std::size_t byteSize{0U};
  int width{0};
  int height{0};
  std::size_t rowBytes{0U};
  // Borrowed bottom-left RGBA8 texture for the explicit native target path.
  void *nativeTexture{nullptr};
};

// LumiDust's procedural-noise material contract. The authored fields are the
// values exposed by the native LumiDust contract. The sub-noise fields are
// material parameters used by the payload Metal programs but not surfaced by
// that component.
// Keeping both groups explicit avoids template-fingerprint defaults in the
// renderer.
struct QtTextDustNoiseParameters final {
  float brightness{0.0F};
  float contrast{0.0F};
  float quantity{2.0F};
  float complexity{1.0F};
  float evolutionDegrees{0.0F};
  int cycle{100};
  float offsetX{0.0F};
  float offsetY{0.0F};

  float rotateDegrees{0.0F};
  float type{0.0F};
  float subImpact{0.6F};
  float subScale{56.0F};
  float subRotateDegrees{0.0F};
  float subOffsetX{0.0F};
  float subOffsetY{0.0F};
  float pictureScale{1.0F};
};

enum class QtTextDustMaskType : std::int32_t {
  Noise = 0,
  Line = 1,
  Image = 2,
};

// Complete LumiDust inputs. gravityRotationDegrees is the authored value; the
// Apple implementation applies the authored contract's exact
// (360 - value) * 3.141 / 180 mapping. maskLineRotationRadians is passed
// through exactly because the source contract does not convert it before the Metal
// vertex shader uses it as an angle.
struct QtTextDustParameters final {
  QtTextDustNoiseParameters noise;
  QtTextDustNoiseParameters maskNoise;

  float distortionIntensity{1.0F};
  float gravity{0.0F};
  float gravityRotationDegrees{0.0F};
  QtTextDustMaskType maskType{QtTextDustMaskType::Noise};
  float maskFeather{1.0F};
  float maskLineRotationRadians{0.0F};

  // Required only for QtTextDustMaskType::Image. It follows the same top-left,
  // raw-RGBA8 transport contract as source.
  QtTextDustRawRgba8ImageView maskImage;
};

struct QtTextDustRenderRequest final {
  QtTextDustRawRgba8ImageView source;
  QtTextDustParameters parameters;

  // Renderer ABI generation. v1 preserves the historical half-quad mesh;
  // v2 uses the pixel-certified full quad and branch-correct line-mask ABI.
  std::uint32_t implementationVersion{0U};

  // Authored LumiDust progress in [0, 100] units. The runtime deliberately
  // performs the authored progress / 100 mapping instead of accepting a pre-normalized
  // value, so animation sampling has one unambiguous owner.
  float progressPercent{0.0F};

  // Explicit because the payload/runtime contract does not establish a valid
  // generic formula from Page size to the two procedural-noise RT sizes. For
  // the captured 84d Page these are 384 x 216; no template ID is consulted.
  int noiseTextureWidth{0};
  int noiseTextureHeight{0};
};

class QtTextDustMetalRuntime {
public:
  virtual ~QtTextDustMetalRuntime() = default;

  QtTextDustMetalRuntime(const QtTextDustMetalRuntime &) = delete;
  QtTextDustMetalRuntime &operator=(const QtTextDustMetalRuntime &) = delete;

  // On success, outputPixels is tightly packed, top-left-row-first RGBA8
  // raw bytes with source.width * source.height * 4 bytes.
  [[nodiscard]] virtual bool
  Render(const QtTextDustRenderRequest &request,
         std::vector<std::uint8_t> &outputPixels,
         std::string &error,
         const NativeRgba8TextureTarget *target = nullptr) = 0;

protected:
  QtTextDustMetalRuntime() = default;
};

// Apple creates the runtime from VideoCutGpuExecution's selected Metal device.
// Unsupported platforms fail closed by returning null and a non-empty error.
[[nodiscard]] std::unique_ptr<QtTextDustMetalRuntime>
CreateQtTextDustMetalRuntime(std::string &error);

} // namespace videocut::skia_runtime::internal
