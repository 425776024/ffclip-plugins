#include "text/QtTextPostEffectRuntime.h"
#include "text/QtTextPostEffectParameters.h"
#include "text/QtTextDirectionalBlursRuntime.h"
#include "text/QtTextTrailRuntime.h"
#include "text/QtTextShakeMetalRuntime.h"
#include "text/QtTextRadialBlurMetalRuntime.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace videocut::skia_runtime::internal {
using namespace post_effect;
namespace {
using ShakeMatrix4 = std::array<double, 16>;

constexpr double kShakePi = 3.141592653589793238462643383279502884;

double &ShakeMatrixAt(ShakeMatrix4 &matrix, const int row, const int column) {
  return matrix[static_cast<std::size_t>(column) * 4U +
                static_cast<std::size_t>(row)];
}

double ShakeMatrixAt(const ShakeMatrix4 &matrix, const int row,
                     const int column) {
  return matrix[static_cast<std::size_t>(column) * 4U +
                static_cast<std::size_t>(row)];
}

ShakeMatrix4 ShakeIdentityMatrix() {
  ShakeMatrix4 result{};
  for (int index = 0; index < 4; ++index)
    ShakeMatrixAt(result, index, index) = 1.0;
  return result;
}

ShakeMatrix4 ShakeMultiply(const ShakeMatrix4 &left,
                           const ShakeMatrix4 &right) {
  ShakeMatrix4 result{};
  for (int column = 0; column < 4; ++column) {
    for (int row = 0; row < 4; ++row) {
      double value = 0.0;
      for (int inner = 0; inner < 4; ++inner) {
        value += ShakeMatrixAt(left, row, inner) *
                 ShakeMatrixAt(right, inner, column);
      }
      ShakeMatrixAt(result, row, column) = value;
    }
  }
  return result;
}

ShakeMatrix4 ShakeScaleMatrix(const double x, const double y,
                              const double z = 1.0) {
  auto result = ShakeIdentityMatrix();
  ShakeMatrixAt(result, 0, 0) = x;
  ShakeMatrixAt(result, 1, 1) = y;
  ShakeMatrixAt(result, 2, 2) = z;
  return result;
}

ShakeMatrix4 ShakeTranslationMatrix(const double x, const double y) {
  auto result = ShakeIdentityMatrix();
  ShakeMatrixAt(result, 0, 3) = x;
  ShakeMatrixAt(result, 1, 3) = y;
  return result;
}

ShakeMatrix4 ShakeRotationMatrix(const double radians) {
  auto result = ShakeIdentityMatrix();
  const double cosine = std::cos(radians);
  const double sine = std::sin(radians);
  ShakeMatrixAt(result, 0, 0) = cosine;
  ShakeMatrixAt(result, 1, 0) = sine;
  ShakeMatrixAt(result, 0, 1) = -sine;
  ShakeMatrixAt(result, 1, 1) = cosine;
  return result;
}

double FloorModulo(const double value, const double divisor) {
  return value - divisor * std::floor(value / divisor);
}

double ShakeMainWave(const double amplitude, const double frequency,
                     const double phase, const double timeSeconds) {
  return std::sin(kShakePi * timeSeconds * frequency + phase) * amplitude;
}

enum class ShakeRandomAxis : unsigned char { X, Y, Z, W };

double ShakeRandomWave(const ShakeRandomAxis axis, const double amplitude,
                       const double frequency, const double phase,
                       const double timeSeconds, const double seed) {
  const double x = kShakePi * timeSeconds * frequency;
  const double firstAngle =
      x * (2.0 + FloorModulo(seed, 0.33)) * 0.5 + phase + FloorModulo(seed, 1.13);
  const double secondAngle =
      x * (3.0 + FloorModulo(seed, 0.73)) * 0.5 + phase + FloorModulo(seed, 0.69);
  const double thirdAngle =
      x * (5.0 + FloorModulo(seed, 1.37)) * 0.5 + phase + FloorModulo(seed, 0.33);
  switch (axis) {
  case ShakeRandomAxis::X:
    return (std::cos(firstAngle) * 0.25 + std::sin(secondAngle) * 0.16 +
            std::cos(thirdAngle) * 0.09) *
           amplitude;
  case ShakeRandomAxis::Y:
    return (std::sin(firstAngle) * 0.25 + std::cos(secondAngle) * 0.16 +
            std::sin(thirdAngle) * 0.09) *
           amplitude;
  case ShakeRandomAxis::Z:
    return (std::sin(firstAngle) * 0.50 + std::sin(secondAngle) * 0.32 +
            std::cos(thirdAngle) * 0.18) *
           amplitude;
  case ShakeRandomAxis::W:
    return (std::sin(firstAngle) * 0.50 + std::cos(secondAngle) * 0.32 +
            std::cos(thirdAngle) * 0.18) *
           amplitude;
  }
  return 0.0;
}

} // namespace

QtRadianceGlowContract
ResolveQtRadianceGlowContract(const text::TextEffectPostEffectNode &effect,
                              const double progress, const int width,
                              const int height) {
  QtRadianceGlowContract result;
  result.width = width;
  result.height = height;
  if (width <= 0 || height <= 0) {
    result.unsupportedReason = "empty target";
    return result;
  }
  const float useMask = SampleParameter(effect, "useMask", progress, 0.0F);
  result.erodeIterations =
      std::clamp(static_cast<int>(std::floor(SampleParameter(
                     effect, "erodeIterations", progress, 0.0F))),
                 0, 5);
  result.directionCount =
      std::clamp(static_cast<int>(std::floor(
                     SampleParameter(effect, "directionNum", progress, 1.0F))),
                 1, 4);
  float glowIntensity = std::clamp(
      SampleParameter(effect, "glowIntensity", progress, 0.0F), 0.0F, 1000.0F);
  result.exposure = std::clamp(
      SampleParameter(effect, "exposure", progress, 1.0F), 0.0F, 20.0F);
  if (result.exposure < 1.0F) {
    glowIntensity *= result.exposure;
    result.exposure = 1.0F;
  }
  float downscale = 1.0F;
  float sampleCount = 0.0F;
  if (glowIntensity <= 10.0F) {
    sampleCount = glowIntensity;
  } else if (glowIntensity <= 50.0F) {
    downscale = 0.7F;
    sampleCount = (0.7F * glowIntensity + 2.0F) * 0.78F;
  } else if (glowIntensity <= 200.0F) {
    downscale = 0.5F;
    sampleCount = (0.5F * glowIntensity + 10.0F) * 0.66F;
  } else {
    downscale = 0.25F;
    sampleCount = (0.25F * glowIntensity + 60.0F) * 0.7F;
  }
  if (sampleCount < 2.0F)
    sampleCount = std::floor(sampleCount + 0.5F);
  result.blurWidth = std::max(
      1, static_cast<int>(static_cast<float>(width) * downscale));
  result.blurHeight = std::max(
      1, static_cast<int>(static_cast<float>(height) * downscale));
  const float quality = std::clamp(
      SampleParameter(effect, "quality", progress, 0.5F), 0.0F, 1.0F);
  float qualityParameter = quality * 2.0F - 1.0F;
  qualityParameter = qualityParameter < 0.0F ? std::pow(10.0F, qualityParameter)
                                             : qualityParameter * 2.0F + 1.0F;
  result.sampleCount = sampleCount * qualityParameter;
  const float radius = glowIntensity / 1000.0F;
  result.sigma = radius / 3.0F;
  const float sampleStep = radius / std::max(result.sampleCount, 1.0e-5F);
  const float angleDegrees = SampleParameter(effect, "angle", progress, 0.0F);
  const float angle =
      (-angleDegrees - 90.0F) * 0.01745329251994329576923690768489F;
  const float minimum = static_cast<float>(std::min(width, height));
  const float maximum = static_cast<float>(std::max(width, height));
  const float baseSize = std::max(minimum, maximum * 0.5F);
  const float erodeStepMultiplier = std::clamp(
      SampleParameter(effect, "erodeStepMulplier", progress, 1.0F), 0.5F, 5.0F);
  result.erodeStepX = erodeStepMultiplier / 1000.0F *
                     (baseSize / static_cast<float>(width));
  result.erodeStepY = erodeStepMultiplier / 1000.0F *
                     (baseSize / static_cast<float>(height));
  result.stepX =
      sampleStep * std::cos(angle) * (baseSize / static_cast<float>(width));
  result.stepY =
      sampleStep * std::sin(angle) * (baseSize / static_cast<float>(height));
  const float directionAngleStep =
      3.1415926535897932384626433832795F /
      static_cast<float>(result.directionCount);
  for (int direction = 0; direction < result.directionCount; ++direction) {
    const float directionAngle =
        angle + static_cast<float>(direction) * directionAngleStep;
    result.directionSteps[static_cast<std::size_t>(direction)] = {
        sampleStep * std::cos(directionAngle) *
            (baseSize / static_cast<float>(width)),
        sampleStep * std::sin(directionAngle) *
            (baseSize / static_cast<float>(height))};
  }
  result.thresholdLow = std::clamp(
      SampleParameter(effect, "thresholdLow", progress, 0.5F), 0.0F, 1.0F);
  result.thresholdHigh = std::clamp(
      SampleParameter(effect, "thresholdHigh", progress, 1.0F), 0.0F, 1.0F);
  result.thresholdSmooth = std::clamp(
      SampleParameter(effect, "thresholdSmooth", progress, 0.0F), 0.0F, 1.0F);
  result.thresholdType =
      SampleParameter(effect, "thresholdType", progress, 0.0F);
  result.grayScale = std::clamp(
      SampleParameter(effect, "grayScale", progress, 0.0F), 0.0F, 1.0F);
  result.spaceDither = std::clamp(
      SampleParameter(effect, "spaceDither", progress, 0.0F), 0.0F, 1.0F);
  result.borderType = SampleParameter(effect, "borderType", progress, 0.0F);
  result.displayGlow =
      SampleParameter(effect, "displayGlow", progress, 0.0F) >= 0.5F ? 1.0F
                                                                     : 0.0F;
  const auto glowColor =
      VectorParameter(effect, "glowColor", {1.0F, 1.0F, 1.0F, 1.0F});
  result.glowColor = {glowColor[0], glowColor[1], glowColor[2]};

  // The source graph renders one independent directional target for every
  // authored direction and screen-combines those targets in the blend pass.
  // Erosion ping-pongs at the downsampled size before these directional passes.
  if (useMask >= 0.5F) {
    result.unsupportedReason = "mask input is not source-closed";
  } else if (!std::isfinite(result.erodeStepX) ||
             !std::isfinite(result.erodeStepY)) {
    result.unsupportedReason = "Radiance erosion steps are non-finite";
  } else {
    result.exactPathSupported = true;
  }
  return result;
}

QtAlphaOutlineContract
ResolveQtAlphaOutlineContract(const text::TextEffectPostEffectNode &effect,
                              const double progress, const int width,
                              const int height) {
  QtAlphaOutlineContract result;
  result.width = width;
  result.height = height;
  result.offsetX = SampleParameter(effect, "offsetX", progress, 0.0F);
  result.offsetY = SampleParameter(effect, "offsetY", progress, 0.0F);
  result.ratio = height > 0
                     ? static_cast<float>(width) / static_cast<float>(height)
                     : 1.0F;
  result.size = SampleParameter(effect, "size", progress, 1.0F);
  result.scaleX = SampleParameter(effect, "scaleX", progress, 1.0F);
  result.scaleY = SampleParameter(effect, "scaleY", progress, 1.0F);
  result.outlineColor =
      VectorParameter(effect, "outlineColor", {1.0F, 1.0F, 1.0F, 1.0F});
  result.intensity = SampleParameter(effect, "intensity", progress, 1.0F);
  return result;
}

QtTurbulenceContract
ResolveQtTurbulenceContract(const text::TextEffectPostEffectNode &effect,
                            const double progress, const int width,
                            const int height) {
  QtTurbulenceContract result;
  result.width = width;
  result.height = height;
  if (width <= 0 || height <= 0) {
    result.unsupportedReason = "empty target";
    return result;
  }
  const float qualityType =
      SampleParameter(effect, "qualityType", progress, 3.0F);
  const float noiseScale = qualityType < 0.5F   ? 1.0F
                           : qualityType < 1.5F ? 0.75F
                           : qualityType < 2.5F ? 0.5F
                                                : 0.25F;
  result.noiseWidth =
      std::max(1, static_cast<int>(std::floor(width * noiseScale)));
  result.noiseHeight =
      std::max(1, static_cast<int>(std::floor(height * noiseScale)));
  result.cycle = std::max(
      2.0F,
      std::floor(SampleParameter(effect, "cycle", progress, 100.0F) * 3.0F +
                 0.5F));
  result.offsetX = SampleParameter(effect, "offset_x", progress, 0.0F);
  result.offsetY = SampleParameter(effect, "offset_y", progress, 0.0F);
  result.quantity = SampleParameter(effect, "quantity", progress, 2.0F);
  result.complexity = SampleParameter(effect, "complexity", progress, 1.0F);
  result.evolution = static_cast<float>(
      std::fabs(SampleParameterValue(effect, "evolution", progress, 0.0)) /
      90.0);
  result.type = SampleParameter(effect, "type", progress, 0.0F);
  result.contrast = SampleParameter(effect, "size", progress, 0.0F);
  result.pictureScale =
      SampleParameter(effect, "picture_scale", progress, 1.0F);
  result.motionTileType =
      SampleParameter(effect, "motion_tile_type", progress, 0.0F);
  const float fixType = SampleParameter(effect, "fix_type", progress, 0.0F);
  // The authored contract forwards picture_scale by name, but neither pinned f52 Metal shader
  // declares or consumes that uniform. Only the captured value 1 is therefore
  // executor-closed; inventing UV scaling here would be visual guesswork.
  if (std::fabs(result.pictureScale - 1.0F) > 1.0e-6F) {
    result.unsupportedReason =
        "picture_scale != 1 is absent from the pinned shader contract";
  } else if (fixType >= 0.5F) {
    result.unsupportedReason =
        "nonzero fix_type is outside the captured active branch";
  } else {
    result.exactPathSupported = true;
  }
  return result;
}

QtGodRayContract ResolveQtGodRayContract(const text::TextEffectPostEffectNode &effect,
                                         const double progress, const int width,
                                         const int height) {
  QtGodRayContract result;
  result.width = width;
  result.height = height;
  if (width <= 0 || height <= 0) {
    result.unsupportedReason = "empty target";
    return result;
  }

  // ScriptCompGodRay.setEffectAttr maps numeric option indexes to strings;
  // unknown indexes use the authored defaults. Preserve that discrete mapping
  // instead of treating these values as continuous tuning knobs.
  const float scaleType = SampleParameter(effect, "scaleType", progress, 2.0F);
  if (scaleType == 0.0F)
    result.downscale = 1.0F;
  else if (scaleType == 1.0F)
    result.downscale = 0.75F;
  else if (scaleType == 2.0F)
    result.downscale = 0.5F;
  else if (scaleType == 3.0F)
    result.downscale = 0.25F;
  else if (scaleType == 4.0F)
    result.downscale = 0.1F;
  else if (scaleType == 5.0F)
    result.downscale = 0.05F;
  else
    result.downscale = 0.5F;
  result.rayWidth = static_cast<int>(
      std::floor(static_cast<float>(width) * result.downscale));
  result.rayHeight = static_cast<int>(
      std::floor(static_cast<float>(height) * result.downscale));
  if (result.rayWidth <= 0 || result.rayHeight <= 0) {
    result.unsupportedReason = "scaled GodRay target is empty";
    return result;
  }

  result.useAlphaThreshold =
      SampleParameter(effect, "useAlphaThreshold", progress, 1.0F) >= 0.5F
          ? 1.0F
          : 0.0F;
  const float rawColorType =
      SampleParameter(effect, "colorType", progress, 0.0F);
  result.colorType = rawColorType == 1.0F ? 1.0F : 0.0F;
  const float range =
      std::clamp(SampleParameter(effect, "range", progress, 0.5F), 0.0F, 1.0F);
  result.threshold = 1.0F - range;

  const float smoothness = std::clamp(
      SampleParameter(effect, "smoothness", progress, 0.0F), 0.0F, 1.0F);
  result.sampleCount = smoothness * 30.0F;
  const float minimum = static_cast<float>(std::min(width, height));
  const float maximum = static_cast<float>(std::max(width, height));
  const float baseSize = std::max(minimum, maximum * 0.5F);
  result.dx = (baseSize / static_cast<float>(width)) / 1000.0F;
  result.dy = (baseSize / static_cast<float>(height)) / 1000.0F;
  result.sigmaX = result.sampleCount * result.dx / 2.5F;
  result.sigmaY = result.sampleCount * result.dy / 2.5F;

  result.intensity = std::clamp(
      SampleParameter(effect, "intensity", progress, 1.0F), 0.0F, 1.0F);
  result.brightness = std::clamp(
      SampleParameter(effect, "brightness", progress, 0.1F), 0.0F, 1.0F);
  const float authoredQuality = std::clamp(
      SampleParameter(effect, "quality", progress, 50.0F), 10.0F, 100.0F);
  result.quality = authoredQuality * std::sqrt(result.downscale);
  const float authoredGrayscale =
      std::clamp(SampleParameter(effect, "grayscaleCorrection", progress, 0.0F),
                 -1.0F, 1.0F);
  result.grayscaleCorrection = std::pow(10.0F, -authoredGrayscale);

  const auto center =
      VectorParameter(effect, "center", {0.5F, 0.5F, 0.0F, 0.0F});
  result.center = {
      std::clamp(SampleParameter(effect, "center_x", progress, center[0]),
                 -1.0F, 2.0F),
      std::clamp(SampleParameter(effect, "center_y", progress, center[1]),
                 -1.0F, 2.0F),
  };
  const float rawBorderType =
      SampleParameter(effect, "borderType", progress, 0.0F);
  result.borderType = rawBorderType == 1.0F ? 1.0F : 0.0F;
  const float rawBlendMode =
      SampleParameter(effect, "blendMode", progress, 0.0F);
  result.blendMode = rawBlendMode == 1.0F || rawBlendMode == 2.0F ||
                             rawBlendMode == 3.0F || rawBlendMode == 4.0F
                         ? rawBlendMode
                         : 0.0F;
  result.displayRayOnly =
      SampleParameter(effect, "displayRayOnly", progress, 0.0F) >= 0.5F ? 1.0F
                                                                        : 0.0F;
  result.inverseGammaCorrection =
      SampleParameter(effect, "inverseGammaCorrection", progress, 0.0F) >= 0.5F
          ? 1.0F
          : 0.0F;
  const auto authoredLightColor =
      VectorParameter(effect, "lightColor", {1.0F, 1.0F, 1.0F, 1.0F});
  result.lightColor = {authoredLightColor[0], authoredLightColor[1],
                       authoredLightColor[2]};
  if (result.inverseGammaCorrection >= 0.5F) {
    for (float &component : result.lightColor)
      component = std::pow(component, result.gamma);
  }
  result.dither = std::clamp(SampleParameter(effect, "dither", progress, 0.0F),
                             0.0F, 1.0F) *
                  2.5F;
  result.noiseIntensity =
      SampleParameter(effect, "noiseIntensity", progress, 0.0F) * 2.0F;
  result.weightDecay = std::clamp(
      SampleParameter(effect, "weightDecay", progress, 1.0F), 0.0F, 2.0F);
  result.colorDecay = std::clamp(
      SampleParameter(effect, "colorDecay", progress, 1.0F), 0.0F, 1.0F);

  const float rawLightSource =
      SampleParameter(effect, "lightSource", progress, 0.0F);
  const bool maskLightSource = rawLightSource == 1.0F;
  result.useAngle =
      SampleParameter(effect, "useAngle", progress, 0.0F) >= 0.5F ? 1.0F : 0.0F;
  // ScriptCompGodRay uses north=0 clockwise for Angle, and aspect-corrected
  // atan2 in Qt UV coordinates for Direction Point. It resolves the interval
  // even when angular gating is disabled.
  constexpr double radiansPerDegree = 0.017453292519943295;
  double directionDegrees;
  if (SampleParameter(effect, "angleType", progress, 0.0F) == 1.0F) {
    const auto point =
        VectorParameter(effect, "directionPoint", {0.5F, 0.5F, 0.0F, 0.0F});
    const double x = static_cast<double>(point[0]) - result.center[0];
    const double y = (static_cast<double>(point[1]) - result.center[1]) *
                     static_cast<double>(height) / width;
    directionDegrees = x == 0.0 && y == 0.0
                           ? 90.0
                           : std::atan2(y, x) / radiansPerDegree;
  } else {
    directionDegrees =
        90.0 - SampleParameter(effect, "angle", progress, 0.0F);
  }
  directionDegrees = std::fmod(directionDegrees, 360.0);
  if (directionDegrees < 0.0)
    directionDegrees += 360.0;
  const double halfRange =
      std::clamp(SampleParameter(effect, "angleRange", progress, 360.0F),
                 0.0F, 360.0F) * 0.5;
  result.minAngle = static_cast<float>(
      (directionDegrees - halfRange) * radiansPerDegree);
  result.maxAngle = static_cast<float>(
      (directionDegrees + halfRange) * radiansPerDegree);
  const std::array<float, 23> scalars{
      result.useAngle,
      result.minAngle,
      result.maxAngle,
      result.downscale,
      result.useAlphaThreshold,
      result.colorType,
      result.threshold,
      result.sampleCount,
      result.dx,
      result.dy,
      result.sigmaX,
      result.sigmaY,
      result.intensity,
      result.brightness,
      result.quality,
      result.grayscaleCorrection,
      result.borderType,
      result.blendMode,
      result.dither,
      result.noiseIntensity,
      result.weightDecay,
      result.colorDecay,
      result.gamma,
  };
  if (!std::all_of(scalars.begin(), scalars.end(),
                   [](const float value) { return std::isfinite(value); }) ||
      !std::all_of(result.center.begin(), result.center.end(),
                   [](const float value) { return std::isfinite(value); }) ||
      !std::all_of(result.lightColor.begin(), result.lightColor.end(),
                   [](const float value) { return std::isfinite(value); })) {
    result.unsupportedReason = "GodRay uniforms are non-finite";
  } else if (maskLightSource) {
    result.unsupportedReason =
        "mask light source requires an explicit TextPro mask texture";
  } else {
    result.exactPathSupported = true;
  }
  return result;
}

QtLinearWipeContract
ResolveQtLinearWipeContract(const text::TextEffectPostEffectNode &effect,
                            const double progress, const int width,
                            const int height) {
  QtLinearWipeContract result;
  result.width = width;
  result.height = height;
  if (width <= 0 || height <= 0) {
    result.unsupportedReason = "empty target";
    return result;
  }
  const float authoredProgress =
      SampleParameter(effect, "progress", progress, 0.0F);
  result.mappedProgress = authoredProgress * 2.0F - 0.5F;
  result.rotationDegrees = SampleParameter(effect, "rotation", progress, 0.0F);
  result.feather = SampleParameter(effect, "feather", progress, 0.0F);
  if (!std::isfinite(result.mappedProgress) ||
      !std::isfinite(result.rotationDegrees) ||
      !std::isfinite(result.feather)) {
    result.unsupportedReason = "LinearWipe uniforms are non-finite";
  } else {
    result.exactPathSupported = true;
  }
  return result;
}

QtGaussianBlurContract
ResolveQtGaussianBlurContract(const text::TextEffectPostEffectNode &effect,
                              const double progress, const int width,
                              const int height) {
  QtGaussianBlurContract result;
  result.width = width;
  result.height = height;
  if (width <= 0 || height <= 0) {
    result.unsupportedReason = "empty GaussianBlur target";
    return result;
  }
  result.implementationVersion = 1U;

  result.intensity = SampleParameter(effect, "blurIntensity", progress, 0.0F);
  const float quality = SampleParameter(effect, "quality", progress, 0.5F);
  const float horizontalStrength =
      SampleParameter(effect, "horizontalStrength", progress, 1.0F);
  const float verticalStrength =
      SampleParameter(effect, "verticalStrength", progress, 1.0F);
  const float blurDirection =
      SampleParameter(effect, "blurDirection", progress, 0.0F);
  const float borderType =
      SampleParameter(effect, "borderType", progress, 0.0F);
  const float blurAlpha = SampleParameter(effect, "blurAlpha", progress, 1.0F);
  const float inverseGamma =
      SampleParameter(effect, "inverseGammaCorrection", progress, 1.0F);
  const float spaceDither =
      SampleParameter(effect, "spaceDither", progress, 0.0F);
  result.gamma = SampleParameter(effect, "gamma", progress, 2.2F);
  const std::array<float, 10> inputs{
      result.intensity, quality,     horizontalStrength, verticalStrength,
      blurDirection,    borderType,  blurAlpha,          inverseGamma,
      spaceDither,      result.gamma};
  if (!std::all_of(inputs.begin(), inputs.end(),
                   [](const float value) { return std::isfinite(value); })) {
    result.unsupportedReason = "GaussianBlur uniforms are non-finite";
    return result;
  }

  // This is the exact resource generation captured for the current corpus.
  // Reject material-private branches that have not been closed instead of
  // silently feeding them through this profile.
  if (quality != 0.5F || horizontalStrength != 1.0F ||
      verticalStrength != 1.0F || blurDirection != 0.0F || borderType != 0.0F ||
      blurAlpha != 1.0F || inverseGamma != 1.0F || spaceDither != 0.0F ||
      result.gamma != 2.2F) {
    result.unsupportedReason =
        "GaussianBlur authored branch is not source-closed";
    return result;
  }
  if (result.intensity < 0.0F || result.intensity > 100.0F) {
    result.unsupportedReason =
        "GaussianBlur intensity is outside the closed schedule";
    return result;
  }

  result.active = result.intensity > 0.01F;
  if (!result.active) {
    result.blurWidth = width;
    result.blurHeight = height;
    result.exactPathSupported = true;
    return result;
  }

  result.blurWidth = static_cast<int>(static_cast<float>(width) * 0.5F);
  result.blurHeight = static_cast<int>(static_cast<float>(height) * 0.5F);
  if (result.blurWidth <= 0 || result.blurHeight <= 0) {
    result.unsupportedReason = "GaussianBlur half-resolution target is empty";
    return result;
  }
  const float minimumSize = static_cast<float>(std::min(width, height));
  const float maximumSize = static_cast<float>(std::max(width, height));
  const float baseSize = std::max(minimumSize, maximumSize * 0.5F);
  const float scaleX = baseSize / static_cast<float>(width);
  const float scaleY = baseSize / static_cast<float>(height);
  result.sampleCount = result.intensity <= 30.0F
                           ? (result.intensity * 0.5F + 2.0F) * 0.78F
                           : (result.intensity * 0.5F + 10.0F) * 0.66F;
  // The authored GaussianBlur contract rounds the small-kernel branch before the
  // value is published to the two material uniforms.  Keeping the fractional
  // pre-round value drops the second tap for low animated intensities and
  // visibly thins the translucent outline even though the main glyph remains
  // nearly unchanged.
  if (result.sampleCount < 2.0F)
    result.sampleCount = std::floor(result.sampleCount + 0.5F);
  const float radiusX = scaleX * result.intensity / 1000.0F;
  const float radiusY = scaleY * result.intensity / 1000.0F;
  result.sigmaX = radiusX / 2.5F;
  result.sigmaY = radiusY / 2.5F;
  result.stepX = radiusX / std::max(result.sampleCount, 1.0e-5F);
  result.stepY = radiusY / std::max(result.sampleCount, 1.0e-5F);
  result.exactPathSupported = true;
  return result;
}

QtSoftGlowContract
ResolveQtSoftGlowContract(const text::TextEffectPostEffectNode &effect,
                          const double progress, const int width,
                          const int height) {
  QtSoftGlowContract result;
  result.width = width;
  result.height = height;
  result.glowWidth = width;
  result.glowHeight = height;
  if (width <= 0 || height <= 0) {
    result.unsupportedReason = "empty SoftGlow target";
    return result;
  }

  // The authored SoftGlow contract evaluates in binary64 precision and
  // material setters narrow the final uniforms to float.  Preserve that
  // operation boundary so animated curves match the captured Metal buffers.
  const double authoredExposure =
      SampleParameterValue(effect, "exposure", progress, 0.0);
  const double authoredGlowIntensity =
      SampleParameterValue(effect, "glowIntensity", progress, 2.5);
  const double quality = SampleParameterValue(effect, "quality", progress, 0.5);
  const double thresholdLow =
      SampleParameterValue(effect, "thresholdLow", progress, 0.1);
  const double thresholdHigh =
      SampleParameterValue(effect, "thresholdHigh", progress, 1.0);
  const double thresholdSmooth =
      SampleParameterValue(effect, "thresholdSmooth", progress, 0.0);
  const double thresholdType =
      SampleParameterValue(effect, "thresholdType", progress, 0.0);
  const double grayScale =
      SampleParameterValue(effect, "grayScale", progress, 0.0);
  const double displayGlow =
      SampleParameterValue(effect, "displayGlow", progress, 0.0);
  const double missingSemantic =
      std::numeric_limits<double>::quiet_NaN();
  const double lowIntensityScheduleScale = SampleParameterValue(
      effect, "lowIntensityScheduleScale", progress, missingSemantic);
  const double foldSubunitExposureIntoIntensity = SampleParameterValue(
      effect, "foldSubunitExposureIntoIntensity", progress, missingSemantic);
  const auto color =
      VectorParameter(effect, "glowColor", {1.0F, 1.0F, 1.0F, 1.0F});
  result.glowColor = {color[0], color[1], color[2]};
  const std::array<double, 11> scalarInputs{
      authoredExposure, authoredGlowIntensity, quality,       thresholdLow,
      thresholdHigh,    thresholdSmooth,       thresholdType, grayScale,
      displayGlow,       lowIntensityScheduleScale,
      foldSubunitExposureIntoIntensity};
  if (!std::all_of(scalarInputs.begin(), scalarInputs.end(),
                   [](const double value) { return std::isfinite(value); }) ||
      !std::all_of(result.glowColor.begin(), result.glowColor.end(),
                   [](const float value) { return std::isfinite(value); })) {
    result.unsupportedReason = "SoftGlow uniforms are non-finite";
    return result;
  }
  if (lowIntensityScheduleScale <= 0.0 ||
      lowIntensityScheduleScale > 1.0 ||
      (foldSubunitExposureIntoIntensity != 0.0 &&
       foldSubunitExposureIntoIntensity != 1.0)) {
    result.unsupportedReason =
        "SoftGlow compiled schedule semantics are invalid";
    return result;
  }
  result.lowIntensityScheduleScale =
      static_cast<float>(lowIntensityScheduleScale);
  result.foldSubunitExposureIntoIntensity =
      foldSubunitExposureIntoIntensity == 1.0;

  result.thresholdLow = static_cast<float>(std::clamp(thresholdLow, 0.0, 1.0));
  result.thresholdHigh =
      static_cast<float>(std::clamp(thresholdHigh, 0.0, 1.0));
  result.thresholdSmooth =
      static_cast<float>(std::clamp(thresholdSmooth, 0.0, 1.0));
  result.thresholdType = thresholdType == 1.0 ? 1.0F : 0.0F;
  result.grayScale = static_cast<float>(std::clamp(grayScale, 0.0, 1.0));
  result.displayGlow = displayGlow >= 0.5 ? 1.0F : 0.0F;

  double exposure = std::clamp(authoredExposure, 0.0, 10.0);
  if (authoredGlowIntensity < 20.0) {
    const double attenuation = authoredGlowIntensity / 20.0;
    exposure *= attenuation * attenuation;
  }
  double blurIntensity = std::clamp(authoredGlowIntensity, 0.0, 500.0);
  if (result.foldSubunitExposureIntoIntensity && exposure < 1.0) {
    blurIntensity *= exposure;
    exposure = 1.0;
  }
  result.effectiveIntensity = static_cast<float>(blurIntensity);
  result.shaderExposure = static_cast<float>(exposure);

  double scheduleScale = 1.0;
  double scheduleBias = 0.0;
  double sampleScale = 1.0;
  if (blurIntensity <= 10.0) {
    scheduleScale = lowIntensityScheduleScale;
  } else if (blurIntensity <= 50.0) {
    scheduleScale = 0.7;
    scheduleBias = 2.0;
    sampleScale = 0.78;
  } else if (blurIntensity <= 200.0) {
    scheduleScale = 0.5;
    scheduleBias = 10.0;
    sampleScale = 0.66;
  } else {
    scheduleScale = 0.25;
    scheduleBias = 60.0;
    sampleScale = 0.7;
  }
  double sampleCount =
      (scheduleScale * blurIntensity + scheduleBias) * sampleScale;
  if (sampleCount < 2.0)
    sampleCount = std::floor(sampleCount + 0.5);
  const double qualityInput = quality * 2.0 - 1.0;
  const double qualityParameter = qualityInput < 0.0
                                      ? std::pow(10.0, qualityInput)
                                      : qualityInput * 2.0 + 1.0;
  sampleCount *= qualityParameter;

  result.glowWidth =
      static_cast<int>(static_cast<double>(width) * scheduleScale);
  result.glowHeight =
      static_cast<int>(static_cast<double>(height) * scheduleScale);
  if (result.glowWidth <= 0 || result.glowHeight <= 0 ||
      !std::isfinite(sampleCount) || !std::isfinite(qualityParameter)) {
    result.unsupportedReason = "SoftGlow derived schedule is invalid";
    return result;
  }

  const double minimumSize = static_cast<double>(std::min(width, height));
  const double maximumSize = static_cast<double>(std::max(width, height));
  const double baseSize = std::max(minimumSize, maximumSize * 0.5);
  const double radiusX =
      (baseSize / static_cast<double>(width)) * blurIntensity / 1000.0;
  const double radiusY =
      (baseSize / static_cast<double>(height)) * blurIntensity / 1000.0;
  result.sampleCount = static_cast<float>(sampleCount);
  result.sigmaX = static_cast<float>(radiusX / 2.5);
  result.sigmaY = static_cast<float>(radiusY / 2.5);
  result.stepX = static_cast<float>(radiusX / std::max(sampleCount, 1.0e-5));
  result.stepY = static_cast<float>(radiusY / std::max(sampleCount, 1.0e-5));
  result.exactPathSupported = true;
  return result;
}

QtTrailContract ResolveQtTrailContract(const text::TextEffectPostEffectNode &effect,
                                       const double progress, const int width,
                                       const int height) {
  QtTrailContract result;
  result.width = width;
  result.height = height;
  if (width <= 0 || height <= 0) {
    result.unsupportedReason = "empty Trail target";
    return result;
  }
  result.implementationVersion = kQtTextTrailImplementationVersion;
  result.stateSchemaVersion = kQtTextTrailStateSchemaVersion;
  result.blur = SampleParameter(effect, "blur", progress, 0.0F);
  result.weaken = SampleParameter(effect, "weaken", progress, 0.2F);
  const float hintProfile =
      SampleParameter(effect, "hintProfile", progress, 0.0F);
  result.hintHue = SampleParameter(effect, "hintHue", progress, 0.0F);
  result.hintOffset = SampleParameter(effect, "hintOffset", progress, 0.0F);
  result.hintMin = SampleParameter(effect, "hintMin", progress, 0.0F);
  result.hintMax = SampleParameter(effect, "hintMax", progress, 1.0F);
  const float baseEnabled =
      SampleParameter(effect, "baseEnabled", progress, 1.0F);
  result.baseHint =
      VectorParameter(effect, "baseHint", {1.0F, 1.0F, 1.0F, 1.0F});
  result.baseHint[3] = 1.0F;
  const std::array<float, 12> inputs{
      result.blur,        result.weaken,      hintProfile,
      result.hintHue,     result.hintOffset,  result.hintMin,
      result.hintMax,     baseEnabled,        result.baseHint[0],
      result.baseHint[1], result.baseHint[2], result.baseHint[3]};
  if (!std::all_of(inputs.begin(), inputs.end(),
                   [](const float value) { return std::isfinite(value); })) {
    result.unsupportedReason = "Trail uniforms are non-finite";
    return result;
  }
  result.hintProfile = static_cast<int>(std::lround(hintProfile));
  result.baseEnabled = baseEnabled >= 0.5F;
  if (result.blur < 0.0F || result.blur > 1.0F || result.weaken < 0.0F ||
      result.weaken > 1.0F || result.hintProfile != 0 ||
      result.hintMin < 0.0F || result.hintMin > 1.0F || result.hintMax < 0.0F ||
      result.hintMax > 1.0F ||
      std::fabs(hintProfile - static_cast<float>(result.hintProfile)) >
          0.000001F) {
    result.unsupportedReason =
        "Trail parameters are outside the audited pattern0 branch";
    return result;
  }
  const float blurPixels = 20.0F * result.blur;
  result.blurSamples = static_cast<int>(std::ceil(blurPixels));
  result.stepX = result.blurSamples == 0
                     ? 1.0F / static_cast<float>(width)
                     : blurPixels / static_cast<float>(result.blurSamples) /
                           static_cast<float>(width);
  result.stepY = result.blurSamples == 0
                     ? 1.0F / static_cast<float>(height)
                     : blurPixels / static_cast<float>(result.blurSamples) /
                           static_cast<float>(height);
  result.sigmaX = result.blurSamples == 0
                      ? 1.0F / static_cast<float>(width)
                      : blurPixels / static_cast<float>(width) * 2.5F;
  result.sigmaY = result.blurSamples == 0
                      ? 1.0F / static_cast<float>(height)
                      : blurPixels / static_cast<float>(height) * 2.5F;
  result.decayStep = 0.3F * result.weaken;
  result.exactPathSupported = true;
  return result;
}

QtDustContract ResolveQtDustContract(const text::TextEffectPostEffectNode &effect,
                                     const double progress, const int width,
                                     const int height,
                                     const int presentationWidth,
                                     const int presentationHeight) {
  QtDustContract result;
  result.width = width;
  result.height = height;
  result.presentationWidth = presentationWidth;
  result.presentationHeight = presentationHeight;
  if (width <= 0 || height <= 0 || presentationWidth <= 0 ||
      presentationHeight <= 0) {
    result.unsupportedReason = "empty Dust target";
    return result;
  }

  result.implementationVersion = 1U;

  // The fixed resource graph authors outputTex.rt at 1280x720 and
  // noiseRT/maskNoiseRT at 640x360. AmazingEngine scales those resources with
  // the presentation target, while the particle pass itself targets the
  // independent Page rectangle. The captured 768x432 execution therefore
  // materializes both noise targets at exactly 384x216.
  if (static_cast<std::int64_t>(presentationWidth) * 720 !=
      static_cast<std::int64_t>(presentationHeight) * 1280) {
    result.unsupportedReason =
        "Dust presentation target is outside the authored 16:9 RT graph";
    return result;
  }
  result.noiseWidth = presentationWidth / 2;
  result.noiseHeight = presentationHeight / 2;

  result.noise.brightness =
      SampleParameter(effect, "brightness", progress, 0.0F);
  result.noise.contrast = SampleParameter(effect, "contrast", progress, 0.0F);
  result.noise.quantity = SampleParameter(effect, "quantity", progress, 2.0F);
  result.noise.complexity =
      SampleParameter(effect, "complexity", progress, 1.0F);
  result.noise.evolutionDegrees =
      SampleParameter(effect, "evolution", progress, 0.0F);
  const float noiseCycle = SampleParameter(effect, "cycle", progress, 100.0F);
  result.noise.cycle = static_cast<int>(std::lround(noiseCycle));
  result.noise.offsetX = SampleParameter(effect, "offset_x", progress, 0.0F);
  result.noise.offsetY = SampleParameter(effect, "offset_y", progress, 0.0F);

  result.maskNoise.brightness =
      SampleParameter(effect, "mask_noise_brightness", progress, 0.0F);
  result.maskNoise.contrast =
      SampleParameter(effect, "mask_noise_contrast", progress, 0.0F);
  result.maskNoise.quantity =
      SampleParameter(effect, "mask_noise_quantity", progress, 2.0F);
  result.maskNoise.complexity =
      SampleParameter(effect, "mask_noise_complexity", progress, 1.0F);
  result.maskNoise.evolutionDegrees =
      SampleParameter(effect, "mask_noise_evolution", progress, 0.0F);
  const float maskNoiseCycle =
      SampleParameter(effect, "mask_noise_cycle", progress, 100.0F);
  result.maskNoise.cycle = static_cast<int>(std::lround(maskNoiseCycle));
  result.maskNoise.offsetX =
      SampleParameter(effect, "mask_noise_offset_x", progress, 0.0F);
  result.maskNoise.offsetY =
      SampleParameter(effect, "mask_noise_offset_y", progress, 0.0F);

  // These fields are defaults in the source-closed material files rather than
  // visual tuning constants. The native node contract owns them together.
  result.noise.subImpact = result.maskNoise.subImpact = 0.6F;
  result.noise.subScale = result.maskNoise.subScale = 56.0F;
  result.noise.pictureScale = result.maskNoise.pictureScale = 1.0F;

  result.distortionIntensity =
      SampleParameter(effect, "distorIns", progress, 1.0F);
  result.gravity = SampleParameter(effect, "gravity", progress, 0.0F);
  result.gravityRotationDegrees =
      SampleParameter(effect, "gravityRot", progress, 0.0F);
  const float authoredMaskType =
      SampleParameter(effect, "maskType", progress, 0.0F);
  result.maskType = static_cast<int>(std::lround(authoredMaskType));
  result.maskFeather = SampleParameter(effect, "maskFeather", progress, 1.0F);
  result.maskLineRotationRadians =
      SampleParameter(effect, "mask_line_rot", progress, 0.0F);
  result.progressPercent = SampleParameter(effect, "progress", progress, 1.0F);

  const std::array<float, 25> scalars{
      result.noise.brightness,
      result.noise.contrast,
      result.noise.quantity,
      result.noise.complexity,
      result.noise.evolutionDegrees,
      noiseCycle,
      result.noise.offsetX,
      result.noise.offsetY,
      result.maskNoise.brightness,
      result.maskNoise.contrast,
      result.maskNoise.quantity,
      result.maskNoise.complexity,
      result.maskNoise.evolutionDegrees,
      maskNoiseCycle,
      result.maskNoise.offsetX,
      result.maskNoise.offsetY,
      result.distortionIntensity,
      result.gravity,
      result.gravityRotationDegrees,
      authoredMaskType,
      result.maskFeather,
      result.maskLineRotationRadians,
      result.progressPercent,
      result.noise.subImpact,
      result.noise.subScale,
  };
  if (!std::all_of(scalars.begin(), scalars.end(),
                   [](const float value) { return std::isfinite(value); })) {
    result.unsupportedReason = "Dust uniforms are non-finite";
  } else if (result.noiseWidth <= 0 || result.noiseHeight <= 0) {
    result.unsupportedReason = "Dust procedural-noise target is empty";
  } else if (std::fabs(noiseCycle - static_cast<float>(result.noise.cycle)) >
                 1.0e-5F ||
             std::fabs(maskNoiseCycle -
                       static_cast<float>(result.maskNoise.cycle)) > 1.0e-5F) {
    result.unsupportedReason = "Dust cycle is outside the authored integer UI";
  } else if (std::fabs(authoredMaskType - static_cast<float>(result.maskType)) >
                 1.0e-5F ||
             result.maskType < 0 || result.maskType > 2) {
    result.unsupportedReason = "Dust mask type is invalid";
  } else if (result.maskType == 2) {
    result.unsupportedReason =
        "Dust image-mask asset binding is not source-closed";
  } else if (result.noise.quantity == 0.0F ||
             result.maskNoise.quantity == 0.0F) {
    result.unsupportedReason = "Dust noise divisor is zero";
  } else {
    result.exactPathSupported = true;
  }
  return result;
}

QtDeepGlowContract
ResolveQtDeepGlowContract(const text::TextEffectPostEffectNode &effect,
                          const double progress, const int width,
                          const int height) {
  QtDeepGlowContract result;
  result.width = width;
  result.height = height;
  if (width <= 0 || height <= 0) {
    result.unsupportedReason = "empty target";
    return result;
  }

  result.quality = SampleParameter(effect, "quality", progress, 1.0F);
  const float authoredIterations =
      SampleParameter(effect, "glowIter", progress, 8.0F);
  result.glowIterations = static_cast<int>(std::floor(authoredIterations));
  const float authoredRadius =
      SampleParameter(effect, "radius", progress, 1.0F);
  result.internalRadius = authoredRadius * 5.0F;
  result.radiusFactor = result.internalRadius / 500.0F;
  result.downSample = SampleParameter(effect, "downSample", progress, 1.0F);
  result.stepsInt = 1.0F / result.downSample;
  result.authoredGamma = SampleParameter(effect, "gammaValue", progress, 2.2F);
  result.gamma = SampleParameter(effect, "gammaCorrect", progress, 1.0F) >= 0.5F
                     ? result.authoredGamma
                     : 1.0F;
  result.exposure = SampleParameter(effect, "exposure", progress, 1.0F);

  float radiusStepsMultiplier = 1.0F;
  if (result.internalRadius >= 500.0F) {
    radiusStepsMultiplier =
        1.0F + ((result.internalRadius - 500.0F) / 1500.0F) * 1.5F;
  }
  result.stepsMultiplier =
      SampleParameter(effect, "stepsMult", progress, 1.0F) *
      radiusStepsMultiplier;

  const float missingSemantic = std::numeric_limits<float>::quiet_NaN();
  result.kernelStrideBase = SampleParameter(
      effect, "kernelStrideBase", progress, missingSemantic);
  result.kernelStrideAspectPower = SampleParameter(
      effect, "kernelStrideAspectPower", progress, missingSemantic);
  const float aspectRatio = static_cast<float>(std::min(width, height)) /
                            static_cast<float>(std::max(width, height));
  result.basePercentage =
      result.kernelStrideBase *
      std::pow(aspectRatio, result.kernelStrideAspectPower);
  result.sharedDownscale = 0.5F * result.quality;
  // Every GlowIter points at the same A/B RenderTexture objects. onUpdate
  // mutates all layer extents before any Camera pass executes, so the final
  // active layer's 0.5 * quality write owns the shared targets for the whole
  // frame (intermediate 0.25 writes never become pass-local extents).
  result.blurWidth = static_cast<int>(
      std::floor(static_cast<float>(width) * result.sharedDownscale));
  result.blurHeight = static_cast<int>(
      std::floor(static_cast<float>(height) * result.sharedDownscale));
  result.postprocessIteration = ((result.glowIterations - 1) / 2) * 2 + 1;

  constexpr std::array<float, 8> kFibonacci{1.0F, 1.0F, 2.0F,  3.0F,
                                            5.0F, 8.0F, 13.0F, 21.0F};
  constexpr float kFalloff = 0.6876560219336321F;
  for (int index = 0; index < std::clamp(result.glowIterations, 0, 8);
       ++index) {
    const float layer = static_cast<float>(index + 1);
    const float maximumSteps =
        std::min(layer * result.radiusFactor, layer * 6.0F);
    const float requestedSteps = result.radiusFactor * kFibonacci[index];
    result.sampledSteps[index] = std::min(requestedSteps, maximumSteps);
    result.strides[index] = result.radiusFactor * layer *
                            result.basePercentage / result.stepsMultiplier;
    result.opacities[index] = result.authoredGamma / layer * kFalloff;
  }

  const std::array<float, 15> scalars{
      result.quality,         authoredIterations,    result.internalRadius,
      result.radiusFactor,    result.basePercentage, result.sharedDownscale,
      result.stepsMultiplier, result.downSample,     result.stepsInt,
      result.authoredGamma,   result.gamma,          result.exposure,
      radiusStepsMultiplier,  result.kernelStrideBase,
      result.kernelStrideAspectPower,
  };
  const auto finiteArray = [](const auto &values) {
    return std::all_of(values.begin(), values.end(),
                       [](const float value) { return std::isfinite(value); });
  };
  if (!std::all_of(scalars.begin(), scalars.end(),
                   [](const float value) { return std::isfinite(value); }) ||
      !finiteArray(result.sampledSteps) || !finiteArray(result.strides) ||
      !finiteArray(result.opacities)) {
    result.unsupportedReason =
        "DeepGlow uniforms or compiled stride semantics are non-finite";
  } else if (result.quality <= 0.0F || result.sharedDownscale <= 0.0F ||
             result.blurWidth <= 0 || result.blurHeight <= 0) {
    result.unsupportedReason = "DeepGlow scaled target is empty";
  } else if (result.glowIterations < 1 || result.glowIterations > 8) {
    result.unsupportedReason =
        "DeepGlow iteration count is outside the eight-layer source graph";
  } else if (result.downSample <= 0.0F || result.stepsMultiplier <= 0.0F ||
             result.gamma <= 0.0F) {
    result.unsupportedReason = "DeepGlow divisor is non-positive";
  } else {
    result.exactPathSupported = true;
  }
  return result;
}

QtDeepGlowContract ResolveQtTypedDeepGlowContract(
    const text::TextEffectPostEffectNode &effect, const double progress,
    const int width, const int height) {
  auto normalized = effect;
  if (!FindParameter(normalized, "glowIter"))
    SetScalarParameter(normalized, "glowIter", 8.0F);
  if (!FindParameter(normalized, "downSample"))
    SetScalarParameter(normalized, "downSample", 1.0F);
  if (!FindParameter(normalized, "stepsMult"))
    SetScalarParameter(normalized, "stepsMult", 1.0F);
  if (!FindParameter(normalized, "gammaValue"))
    SetScalarParameter(normalized, "gammaValue", 2.2F);
  if (!FindParameter(normalized, "gammaCorrect"))
    SetScalarParameter(normalized, "gammaCorrect", 1.0F);
  if (!FindParameter(normalized, "kernelStrideBase"))
    SetScalarParameter(normalized, "kernelStrideBase", 0.004F);
  if (!FindParameter(normalized, "kernelStrideAspectPower"))
    SetScalarParameter(normalized, "kernelStrideAspectPower", 1.0F);
  if (!FindParameter(normalized, "exposure")) {
    SetScalarParameter(normalized, "exposure",
                       SampleParameter(effect, "intensity", progress, 1.0F));
  }
  auto result = ResolveQtDeepGlowContract(normalized, progress, width, height);
  result.blendMode = static_cast<int>(std::lround(
      SampleParameter(effect, "blendMode", progress, 1.0F)));
  result.glowFromAlpha =
      SampleParameter(effect, "glowFromAlpha", progress, 1.0F);
  result.sourceOpacity =
      SampleParameter(effect, "sourceOpacity", progress, 0.0F);
  const float tint = SampleParameter(effect, "tint", progress, 1.0F);
  result.tintEnabled = tint >= 0.5F;
  result.tintMode = static_cast<int>(
      std::lround(SampleParameter(effect, "tintMode", progress, 1.0F)));
  result.tintMix = SampleParameter(effect, "tintMix", progress, 1.0F);
  const auto tintColor =
      VectorParameter(effect, "tintColor", {1.0F, 0.0F, 0.0F, 1.0F});
  result.tintColor = {tintColor[0], tintColor[1], tintColor[2]};
  const float ca = SampleParameter(effect, "ca", progress, 0.0F);
  result.chromaticAberrationEnabled = ca >= 0.5F;
  result.chromaticOffsets = {
      SampleParameter(effect, "redOffset", progress, 0.01F),
      SampleParameter(effect, "greenOffset", progress, -0.01F),
      SampleParameter(effect, "blueOffset", progress, 0.0F),
  };
  const float ratio = SampleParameter(effect, "ratio", progress, 1.0F);
  const float rotate = SampleParameter(effect, "rotate", progress, 0.0F);
  const float gammaCorrect =
      SampleParameter(effect, "gammaCorrect", progress, 1.0F);
  const std::array<float, 13> exactInputs{
      static_cast<float>(result.blendMode), result.glowFromAlpha,
      result.sourceOpacity,                 tint,
      static_cast<float>(result.tintMode),  result.tintMix,
      ca,                                   result.chromaticOffsets[0],
      result.chromaticOffsets[1],           result.chromaticOffsets[2],
      ratio,                                rotate,
      gammaCorrect};
  if (!std::all_of(exactInputs.begin(), exactInputs.end(),
                   [](const float value) { return std::isfinite(value); }) ||
      !std::all_of(result.tintColor.begin(), result.tintColor.end(),
                   [](const float value) { return std::isfinite(value); }) ||
      !result.exactPathSupported) {
    result.exactPathSupported = false;
    if (result.unsupportedReason.empty())
      result.unsupportedReason = "typed DeepGlow uniforms are non-finite";
  }
  return result;
}

QtSGlowContract ResolveQtSGlowContract(
    const text::TextEffectPostEffectNode &effect, const double progress,
    const int width, const int height) {
  QtSGlowContract result;
  result.width = width;
  result.height = height;
  const float quality = std::clamp(
      SampleParameter(effect, "quality", progress, 0.5F), 0.01F, 1.0F);
  constexpr float kMaximumEffectWidth = 720.0F;
  const float cappedScale =
      width > 0
          ? std::min(1.0F,
                     kMaximumEffectWidth / static_cast<float>(width))
          : 1.0F;
  result.blurWidth = std::max(
      1, static_cast<int>(std::floor(static_cast<float>(width) *
                                     cappedScale * quality)));
  result.blurHeight = std::max(
      1, static_cast<int>(std::floor(static_cast<float>(height) *
                                     cappedScale * quality)));
  result.brightness = SampleParameter(effect, "brightness", progress, 2.0F);
  result.glowWidth = SampleParameter(effect, "glowWidth", progress, 0.1F);
  result.threshold = SampleParameter(effect, "threshold", progress, 0.5F);
  result.glowFromAlpha =
      SampleParameter(effect, "glowFromAlpha", progress, 0.0F);
  result.useAlphaThreshold =
      SampleParameter(effect, "useAlphaThreshold", progress, 1.0F);
  result.bgBrightness =
      SampleParameter(effect, "bgBrightness", progress, 1.0F);
  result.lightBackground =
      SampleParameter(effect, "lightBackground", progress, 0.0F);
  result.widthX = SampleParameter(effect, "widthX", progress, 1.0F);
  result.widthY = SampleParameter(effect, "widthY", progress, 1.0F);
  result.channelWidths = {
      SampleParameter(effect, "widthRed", progress, 1.0F),
      SampleParameter(effect, "widthGreen", progress, 1.2F),
      SampleParameter(effect, "widthBlue", progress, 1.4F),
  };
  const auto color =
      VectorParameter(effect, "glowColor", {1.0F, 1.0F, 1.0F, 1.0F});
  result.glowColor = {color[0], color[1], color[2]};
  const std::array<float, 7> exactBranch{
      SampleParameter(effect, "combine", progress, 0.0F),
      SampleParameter(effect, "dither", progress, 0.0F),
      SampleParameter(effect, "edgeMode", progress, 1.0F),
      SampleParameter(effect, "glowUnderSource", progress, 0.0F),
      SampleParameter(effect, "show", progress, 0.0F),
      SampleParameter(effect, "sourceOpacity", progress, 1.0F),
      SampleParameter(effect, "quality", progress, 0.5F),
  };
  const auto thresholdAddColor = VectorParameter(
      effect, "thresholdAddColor", {0.0F, 0.0F, 0.0F, 1.0F});
  const std::array<float, 19> inputs{
      result.brightness, result.glowWidth, result.threshold,
      result.glowFromAlpha, result.useAlphaThreshold, result.bgBrightness,
      result.lightBackground, result.widthX, result.widthY,
      result.channelWidths[0], result.channelWidths[1],
      result.channelWidths[2], result.glowColor[0], result.glowColor[1],
      result.glowColor[2], thresholdAddColor[0], thresholdAddColor[1],
      thresholdAddColor[2], thresholdAddColor[3]};
  if (width <= 0 || height <= 0 ||
      !std::all_of(inputs.begin(), inputs.end(),
                   [](const float value) { return std::isfinite(value); }) ||
      !std::all_of(exactBranch.begin(), exactBranch.end(),
                   [](const float value) { return std::isfinite(value); })) {
    result.unsupportedReason = "SGlow uniforms are invalid";
    return result;
  }
  result.exactPathSupported = true;
  return result;
}

QtSoftGlowContract ResolveQtTypedSGlowContract(
    const text::TextEffectPostEffectNode &effect, const double progress,
    const int width, const int height) {
  auto normalized = effect;
  if (FindParameter(effect, "radius")) {
    SetScalarParameter(normalized, "glowIntensity",
                       SampleParameter(effect, "radius", progress, 2.5F));
  } else if (!FindParameter(effect, "glowIntensity")) {
    SetScalarParameter(normalized, "glowIntensity", 2.5F);
  }
  if (FindParameter(effect, "intensity")) {
    SetScalarParameter(normalized, "exposure",
                       SampleParameter(effect, "intensity", progress, 1.0F));
  } else if (!FindParameter(effect, "exposure")) {
    SetScalarParameter(normalized, "exposure", 1.0F);
  }
  if (FindParameter(effect, "threshold")) {
    SetScalarParameter(normalized, "thresholdLow",
                       SampleParameter(effect, "threshold", progress, 0.1F));
  } else if (!FindParameter(effect, "thresholdLow")) {
    SetScalarParameter(normalized, "thresholdLow", 0.1F);
  }
  if (!FindParameter(normalized, "thresholdHigh"))
    SetScalarParameter(normalized, "thresholdHigh", 1.0F);
  if (!FindParameter(normalized, "lowIntensityScheduleScale"))
    SetScalarParameter(normalized, "lowIntensityScheduleScale", 1.0F);
  if (!FindParameter(normalized, "foldSubunitExposureIntoIntensity"))
    SetScalarParameter(normalized, "foldSubunitExposureIntoIntensity", 0.0F);
  if (FindParameter(effect, "color")) {
    const auto color =
        VectorParameter(effect, "color", {1.0F, 1.0F, 1.0F, 1.0F});
    SetVectorParameter(normalized, "glowColor",
                       {color[0], color[1], color[2], color[3]});
  } else if (!FindParameter(effect, "glowColor")) {
    SetVectorParameter(normalized, "glowColor", {1.0F, 1.0F, 1.0F, 1.0F});
  }
  return ResolveQtSoftGlowContract(normalized, progress, width, height);
}

QtDirectionalBlurContract ResolveQtDirectionalBlurContract(
    const text::TextEffectPostEffectNode &effect, const double progress,
    const int width, const int height) {
  QtDirectionalBlurContract result;
  result.width = width;
  result.height = height;
  result.blurIntensity = SampleParameter(
      effect, "blurIntensity", progress,
      SampleParameter(effect, "radius", progress, effect.amount));
  result.angleDegrees = SampleParameter(effect, "angle", progress, 0.0F);
  result.directionNum = static_cast<int>(std::llround(
      SampleParameterValue(effect, "directionNum", progress, 1.0)));
  if (result.directionNum == 4) {
    QtTextDirectionalBlursContractRequest pairRequest;
    pairRequest.identity.implementationId =
        kQtTextDirectionalBlursImplementationId;
    pairRequest.identity.implementationVersion =
        kQtTextDirectionalBlursImplementationVersion;
    pairRequest.identity.contractSchemaVersion =
        kQtTextDirectionalBlursContractSchemaVersion;
    pairRequest.identity.sourceContractDigest =
        kQtTextDirectionalBlursSourceContractDigest;
    pairRequest.pageWidth = width;
    pairRequest.pageHeight = height;
    pairRequest.directional.blurIntensity = result.blurIntensity;
    pairRequest.directional.angleDegrees = result.angleDegrees;
    pairRequest.directional.directionNum = result.directionNum;
    pairRequest.directional.exposure =
        SampleParameterValue(effect, "exposure", progress, 1.0);
    pairRequest.directional.quality =
        SampleParameterValue(effect, "quality", progress, 0.5);
    pairRequest.directional.spaceDither =
        SampleParameterValue(effect, "spaceDither", progress, 0.0);
    pairRequest.directional.borderType =
        static_cast<QtTextDirectionalBlursBorderType>(std::llround(
            SampleParameterValue(effect, "borderType", progress, 0.0)));
    pairRequest.directional.blendMode =
        static_cast<QtTextDirectionalBlursBlendMode>(std::llround(
            SampleParameterValue(effect, "blendMode", progress, 2.0)));
    QtTextDirectionalBlursDerivedContract pairDerived;
    if (!ResolveQtTextDirectionalBlursContract(
            pairRequest, pairDerived, result.unsupportedReason)) {
      return result;
    }
    result.blurWidth = pairDerived.downsampleWidth;
    result.blurHeight = pairDerived.downsampleHeight;
    result.requiresAlphaOutlineFusion = true;
    result.exactPathSupported = true;
    return result;
  }
  if (result.directionNum != 1) {
    result.unsupportedReason =
        "DirectionalBlur directionNum requires a source-closed topology";
    return result;
  }
  QtTextDirectionalBlurStandaloneContractRequest request;
  request.pageWidth = width;
  request.pageHeight = height;
  request.directional.blurIntensity = result.blurIntensity;
  request.directional.angleDegrees = result.angleDegrees;
  request.directional.directionNum = result.directionNum;
  request.directional.exposure =
      SampleParameterValue(effect, "exposure", progress, 1.0);
  request.directional.quality =
      SampleParameterValue(effect, "quality", progress, 0.5);
  request.directional.spaceDither =
      SampleParameterValue(effect, "spaceDither", progress, 0.0);
  request.directional.borderType =
      static_cast<QtTextDirectionalBlursBorderType>(std::llround(
          SampleParameterValue(effect, "borderType", progress, 0.0)));
  request.directional.blendMode =
      static_cast<QtTextDirectionalBlursBlendMode>(std::llround(
          SampleParameterValue(effect, "blendMode", progress, 2.0)));
  QtTextDirectionalBlurStandaloneDerivedContract derived;
  if (!ResolveQtTextDirectionalBlurStandaloneContract(
          request, derived, result.unsupportedReason)) {
    return result;
  }
  result.blurWidth = derived.downsampleWidth;
  result.blurHeight = derived.downsampleHeight;
  result.exactPathSupported = true;
  return result;
}

QtWaveWarpContract ResolveQtWaveWarpContract(
    const text::TextEffectPostEffectNode &effect, const double progress,
    const int width, const int height) {
  QtWaveWarpContract result;
  result.width = width;
  result.height = height;
  result.amplitude = {
      SampleParameter(effect, "amplitude", progress, effect.amount), 0.0F};
  result.wavelength = std::max(
      0.1F,
      std::fabs(SampleParameter(effect, "wavelength", progress, 2.0F)));
  const float directionDegrees = SampleParameter(
      effect, "direction", progress,
      SampleParameter(effect, "angle", progress, 0.0F));
  const float speed = SampleParameter(effect, "speed", progress, 0.0F);
  result.phaseRadians =
      -(SampleParameter(effect, "phase", progress, 0.0F) + speed * progress);
  result.angleRadians =
      (directionDegrees - 90.0F) * 0.01745329251994329577F;
  result.waveType = SampleParameter(effect, "type", progress, 0.0F);
  result.antiAliasing =
      SampleParameter(effect, "antiAliasing", progress, 0.0F);
  result.fixedType =
      SampleParameter(effect, "fixedType", progress, 0.0F);
  if (width <= 0 || height <= 0 || !std::isfinite(result.amplitude[0]) ||
      !std::isfinite(result.amplitude[1]) || result.amplitude[1] != 0.0F ||
      !std::isfinite(result.wavelength) || result.wavelength <= 0.0F ||
      !std::isfinite(result.phaseRadians) ||
      !std::isfinite(result.angleRadians) ||
      !std::isfinite(result.waveType) ||
      !std::isfinite(result.antiAliasing) ||
      !std::isfinite(result.fixedType) || !std::isfinite(speed)) {
    result.unsupportedReason = "WaveWarp uniforms are invalid";
    return result;
  }
  result.exactPathSupported = true;
  return result;
}

QtChromaticAberrationContract ResolveQtChromaticAberrationContract(
    const text::TextEffectPostEffectNode &effect, const double progress,
    const int width, const int height) {
  QtChromaticAberrationContract result;
  result.width = width;
  result.height = height;
  constexpr float kQtChromaticOffsetScale = 0.2F;
  result.offsetX =
      SampleParameter(effect, "offsetX", progress, effect.amount) *
      kQtChromaticOffsetScale;
  result.offsetY =
      -SampleParameter(effect, "offsetY", progress, 0.0F) *
      kQtChromaticOffsetScale;
  if (width <= 0 || height <= 0 || !std::isfinite(result.offsetX) ||
      !std::isfinite(result.offsetY)) {
    result.unsupportedReason = "ChromaticAberration offsets are invalid";
    return result;
  }
  result.exactPathSupported = true;
  return result;
}

QtMultiShadowContract ResolveQtMultiShadowContract(
    const text::TextEffectPostEffectNode &effect, const double progress,
    const int width, const int height, const float renderGroupExpandRatioX,
    const float renderGroupExpandRatioY) {
  QtMultiShadowContract result;
  result.width = width;
  result.height = height;
  const auto textExpandRatio = VectorParameter(
      effect, "textExpandRatio",
      {renderGroupExpandRatioX, renderGroupExpandRatioY, 0.0F, 0.0F});
  result.renderGroupExpandRatio = {textExpandRatio[0], textExpandRatio[1]};
  const float authoredCount = SampleParameter(
      effect, "layerNum", progress,
      SampleParameter(effect, "count", progress, 1.0F));
  result.layerCount = static_cast<int>(std::lround(authoredCount));
  result.originalAlpha = SampleParameter(effect, "oriAlpha", progress, 1.0F);
  const float distance = SampleParameter(effect, "distance", progress, 0.0F);
  const float angle = SampleParameter(effect, "angle", progress, 0.0F) *
                      0.01745329251994329577F;
  const auto commonColor =
      VectorParameter(effect, "color", {0.0F, 0.0F, 0.0F, 1.0F});
  const float blur = SampleParameter(effect, "blur", progress, 0.0F);
  const float spread = SampleParameter(effect, "spread", progress, 0.0F);
  const std::array<float, 12> originalBranch{
      SampleParameter(effect, "isAssociation", progress, 0.0F),
      SampleParameter(effect, "oriBgAlpha", progress, 0.0F),
      SampleParameter(effect, "oriBgFeather", progress, 0.0F),
      SampleParameter(effect, "oriBgPad", progress, 0.0F),
      SampleParameter(effect, "oriPivotX", progress, 0.0F),
      SampleParameter(effect, "oriPivotY", progress, 0.0F),
      SampleParameter(effect, "oriPositionX", progress, 0.0F),
      SampleParameter(effect, "oriPositionY", progress, 0.0F),
      SampleParameter(effect, "oriRotation", progress, 0.0F),
      SampleParameter(effect, "oriScaleX", progress, 1.0F),
      SampleParameter(effect, "oriScaleY", progress, 1.0F),
      SampleParameter(effect, "unifiedScale", progress, 1.0F),
  };
  const auto originalColor =
      VectorParameter(effect, "oriColor", {1.0F, 1.0F, 1.0F, 1.0F});
  if (width <= 0 || height <= 0 ||
      !std::isfinite(result.renderGroupExpandRatio[0]) ||
      !std::isfinite(result.renderGroupExpandRatio[1]) ||
      result.renderGroupExpandRatio[0] <= 0.0F ||
      result.renderGroupExpandRatio[1] <= 0.0F ||
      std::fabs(authoredCount - static_cast<float>(result.layerCount)) >
          1.0e-5F ||
      result.layerCount < 0 || result.layerCount > 8 ||
      !std::isfinite(result.originalAlpha) || !std::isfinite(distance) ||
      !std::isfinite(angle) || !std::isfinite(blur) ||
      !std::isfinite(spread) || blur != 0.0F || spread != 0.0F ||
      !std::all_of(originalBranch.begin(), originalBranch.end(),
                   [](const float value) { return std::isfinite(value); }) ||
      originalBranch[0] != 0.0F || originalBranch[1] != 0.0F ||
      originalBranch[4] != 0.0F || originalBranch[5] != 0.0F ||
      originalBranch[6] != 0.0F || originalBranch[7] != 0.0F ||
      originalBranch[8] != 0.0F || originalBranch[9] != 1.0F ||
      originalBranch[10] != 1.0F || originalBranch[11] != 1.0F ||
      originalColor != std::array<float, 4>{1.0F, 1.0F, 1.0F, 1.0F}) {
    result.unsupportedReason =
        "MultiShadow requires 0..8 unblurred, zero-spread typed layers";
    return result;
  }
  for (int index = 0; index < result.layerCount; ++index) {
    auto &layer = result.layers[static_cast<std::size_t>(index)];
    const auto suffix = std::to_string(index + 1);
    layer.alpha = SampleParameter(effect, ("alpha_" + suffix).c_str(),
                                  progress, 1.0F);
    layer.positionX = SampleParameter(
        effect, ("positionX_" + suffix).c_str(), progress,
        distance * std::cos(angle) / static_cast<float>(width));
    layer.positionY = SampleParameter(
        effect, ("positionY_" + suffix).c_str(), progress,
        distance * std::sin(angle) / static_cast<float>(height));
    layer.scale = SampleParameter(effect, ("scale_" + suffix).c_str(),
                                  progress, 1.0F);
    layer.rotationRadians =
        SampleParameter(effect, ("rotation_" + suffix).c_str(), progress,
                        0.0F) *
        0.01745329251994329577F;
    layer.color = VectorParameter(effect, ("color_" + suffix).c_str(),
                                  commonColor);
    const float backgroundAlpha = SampleParameter(
        effect, ("bgAlpha_" + suffix).c_str(), progress, 0.0F);
    if (!std::isfinite(layer.alpha) || !std::isfinite(layer.positionX) ||
        !std::isfinite(layer.positionY) || !std::isfinite(layer.scale) ||
        layer.scale <= 0.0F || !std::isfinite(layer.rotationRadians) ||
        !std::isfinite(backgroundAlpha) || backgroundAlpha != 0.0F ||
        !std::all_of(layer.color.begin(), layer.color.end(),
                     [](const float value) { return std::isfinite(value); })) {
      result.unsupportedReason = "MultiShadow layer uniforms are invalid";
      return result;
    }
  }
  result.exactPathSupported = true;
  return result;
}

QtDistortChromaContract
ResolveQtDistortChromaContract(const text::TextEffectPostEffectNode &effect,
                               const double progress, const int width,
                               const int height) {
  QtDistortChromaContract result;
  result.width = width;
  result.height = height;
  if (width <= 0 || height <= 0) {
    result.unsupportedReason = "empty DistortChroma target";
    return result;
  }

  const auto finite = [](const double value) { return std::isfinite(value); };
  const auto integerInRange = [&](const double value) {
    return finite(value) &&
           value >= static_cast<double>(std::numeric_limits<int>::min()) &&
           value <= static_cast<double>(std::numeric_limits<int>::max());
  };
  const double quality =
      SampleParameterValue(effect, "quality", progress, 0.5);
  const double scaledWidth = static_cast<double>(width) * quality;
  const double scaledHeight = static_cast<double>(height) * quality;
  if (!integerInRange(scaledWidth) || !integerInRange(scaledHeight)) {
    result.unsupportedReason =
        "DistortChroma scaled target dimensions are non-finite";
    return result;
  }
  // Assigning the authored binary64 value to RenderTexture.width/height truncates positive
  // fractional dimensions. It does not round to the nearest pixel.
  result.lensWidth = static_cast<int>(scaledWidth);
  result.lensHeight = static_cast<int>(scaledHeight);
  if (result.lensWidth <= 0 || result.lensHeight <= 0) {
    result.unsupportedReason = "DistortChroma scaled target is empty";
    return result;
  }

  const bool hasBlurMap = FindParameter(effect, "blurMap") != nullptr;
  const double authoredBlurSteps = SampleParameterValue(
      effect, hasBlurMap ? "blurMap" : "blurLens", progress, 1.0);
  const double mappedBlurSteps = std::max(authoredBlurSteps, 1.0);
  const double authoredChromaSteps =
      SampleParameterValue(effect, "steps", progress, 8.0);
  const double wrapX =
      SampleParameterValue(effect, "wrapModeX", progress, 0.0);
  const double wrapY =
      SampleParameterValue(effect, "wrapModeY", progress, 0.0);
  if (!integerInRange(mappedBlurSteps) ||
      !integerInRange(authoredChromaSteps) || !integerInRange(wrapX) ||
      !integerInRange(wrapY)) {
    result.unsupportedReason =
        "DistortChroma integer uniform is outside the source ABI";
    return result;
  }
  // setInt consumes the numeric property after the explicit transforms.
  result.blurSteps = static_cast<int>(mappedBlurSteps);
  result.chromaSteps = static_cast<int>(authoredChromaSteps);
  result.wrapModeX = static_cast<int>(std::floor(wrapX));
  result.wrapModeY = static_cast<int>(std::floor(wrapY));
  if (result.blurSteps <= 0 || result.chromaSteps <= 0) {
    result.unsupportedReason =
        "DistortChroma sample count would divide by zero";
    return result;
  }

  const double stride =
      SampleParameterValue(effect, "stride", progress, 1.0);
  const double angle =
      SampleParameterValue(effect, "angle", progress, 0.0);
  const double authoredAmount = SampleParameterValue(
      effect, "amount", progress, static_cast<double>(effect.amount));
  const double warpMagnitude =
      std::sqrt(std::abs(authoredAmount) * 18.18);
  const double mappedWarpAmount =
      authoredAmount > 0.0 ? warpMagnitude : -warpMagnitude;

  result.blurStrideFirst = static_cast<float>(stride / 2.0);
  result.blurStrideSecond = static_cast<float>((stride / 2.0) * 4.0);
  result.blurAngleDegrees = static_cast<float>(angle);
  result.blurPerpendicularAngleDegrees = static_cast<float>(angle + 90.0);
  result.rotateWarpDirectionDegrees = static_cast<float>(
      SampleParameterValue(effect, "rotateWarpDir", progress, 0.0));
  result.amountRelX = static_cast<float>(
      SampleParameterValue(effect, "amountRelX", progress, 1.0));
  result.amountRelY = static_cast<float>(
      SampleParameterValue(effect, "amountRelY", progress, 1.0));
  result.warpRed = static_cast<float>(
      SampleParameterValue(effect, "warpRed", progress, 0.5));
  result.warpBlue = static_cast<float>(
      SampleParameterValue(effect, "warpBlue", progress, 0.5));
  result.warpAmount = static_cast<float>(mappedWarpAmount);
  const auto color1 =
      VectorParameter(effect, "color1", {1.0F, 0.0F, 0.0F, 1.0F});
  const auto color2 =
      VectorParameter(effect, "color2", {0.0F, 1.0F, 0.0F, 1.0F});
  const auto color3 =
      VectorParameter(effect, "color3", {0.0F, 0.0F, 1.0F, 1.0F});
  std::copy_n(color1.begin(), result.color1.size(), result.color1.begin());
  std::copy_n(color2.begin(), result.color2.size(), result.color2.begin());
  std::copy_n(color3.begin(), result.color3.size(), result.color3.begin());
  result.colorMix = static_cast<float>(
      SampleParameterValue(effect, "mix", progress, 1.0));

  const std::array<float, 20> scalars{
      result.blurStrideFirst,
      result.blurStrideSecond,
      result.blurAngleDegrees,
      result.blurPerpendicularAngleDegrees,
      result.rotateWarpDirectionDegrees,
      result.amountRelX,
      result.amountRelY,
      result.warpRed,
      result.warpBlue,
      result.warpAmount,
      result.color1[0],
      result.color1[1],
      result.color1[2],
      result.color2[0],
      result.color2[1],
      result.color2[2],
      result.color3[0],
      result.color3[1],
      result.color3[2],
      result.colorMix,
  };
  if (!finite(quality) || !finite(stride) || !finite(angle) ||
      !finite(authoredAmount) || !finite(mappedWarpAmount) ||
      !std::all_of(scalars.begin(), scalars.end(), [](const float value) {
        return std::isfinite(value);
      })) {
    result.unsupportedReason = "DistortChroma uniforms are non-finite";
    return result;
  }
  const std::size_t lensPixels = static_cast<std::size_t>(result.lensWidth) *
                                 static_cast<std::size_t>(result.lensHeight);
  if (lensPixels > 16U * 1024U * 1024U) {
    result.unsupportedReason =
        "DistortChroma scaled target exceeds the allocation cap";
    return result;
  }
  result.exactPathSupported = true;
  return result;
}

QtRadialBlurContract
ResolveQtRadialBlurContract(const text::TextEffectPostEffectNode &effect,
                            const double progress, const int width,
                            const int height) {
  QtRadialBlurContract result;
  result.width = width;
  result.height = height;
  if (width <= 0 || height <= 0) {
    result.unsupportedReason = "empty RadialBlur target";
    return result;
  }
  result.implementationVersion = kQtTextRadialBlurImplementationVersion;

  const double authoredAmount =
      SampleParameterValue(effect, "amount", progress, 0.0);
  const double authoredBlurType =
      SampleParameterValue(effect, "blurType", progress, 0.0);
  const double authoredCenterX =
      SampleParameterValue(effect, "center_x", progress, 0.5);
  const double authoredCenterY =
      SampleParameterValue(effect, "center_y", progress, 0.5);
  const double authoredQuality =
      SampleParameterValue(effect, "quality", progress, 0.2);
  const double authoredWeightDecay =
      SampleParameterValue(effect, "weightDecay", progress, 0.965);
  const double authoredDither =
      SampleParameterValue(effect, "dither", progress, 0.0);
  const double authoredBorderType =
      SampleParameterValue(effect, "borderType", progress, 0.0);
  const double authoredBlurAlpha =
      SampleParameterValue(effect, "blurAlpha", progress, 1.0);
  const double authoredInverseGamma =
      SampleParameterValue(effect, "inverseGammaCorrection", progress, 0.0);
  const double authoredLightIntensity =
      SampleParameterValue(effect, "lightIntensity", progress, 1.0);
  const double authoredLightTransfer =
      SampleParameterValue(effect, "lightTransferMode", progress, 1.0);
  const std::array<double, 12> authoredScalars{
      authoredAmount,          authoredBlurType,      authoredCenterX,
      authoredCenterY,         authoredQuality,       authoredWeightDecay,
      authoredDither,          authoredBorderType,    authoredBlurAlpha,
      authoredInverseGamma,    authoredLightIntensity,
      authoredLightTransfer,
  };
  if (!std::all_of(authoredScalars.begin(), authoredScalars.end(),
                   [](const double value) { return std::isfinite(value); })) {
    result.unsupportedReason = "RadialBlur authored parameters are non-finite";
    return result;
  }

  const auto integralParameter = [](const double sampled, const int minimum,
                                    const int maximum) -> int {
    const long rounded = std::lround(sampled);
    return static_cast<int>(std::clamp<long>(rounded, minimum, maximum));
  };
  result.intensity = static_cast<float>(
      std::clamp(authoredAmount, -250.0, 250.0) / 200.0);
  result.blurType = integralParameter(authoredBlurType, 0, 5);
  result.center = {
      static_cast<float>(std::clamp(authoredCenterX, -1.0, 2.0)),
      static_cast<float>(std::clamp(authoredCenterY, -1.0, 2.0)),
  };
  result.quality = static_cast<float>(
      std::clamp(authoredQuality, 0.1, 1.0) * 100.0);
  result.weightDecay =
      static_cast<float>(std::clamp(authoredWeightDecay, 0.0, 1.0));
  if (result.blurType != 1 && result.blurType != 5)
    result.weightDecay = 1.0F;
  result.dither =
      static_cast<float>(std::clamp(authoredDither, 0.0, 1.0) * 0.5);
  result.borderType = integralParameter(authoredBorderType, 0, 1);
  result.blurAlpha = integralParameter(authoredBlurAlpha, 0, 1);
  result.inverseGammaCorrection =
      integralParameter(authoredInverseGamma, 0, 1);
  result.gamma = 2.2F;
  result.lightIntensity =
      static_cast<float>(std::clamp(authoredLightIntensity, 1.0, 10.0));
  result.lightTransferMode =
      static_cast<float>(integralParameter(authoredLightTransfer, 0, 3));

  const std::array<float, 10> scalars{
      result.intensity,
      result.center[0],
      result.center[1],
      result.quality,
      result.weightDecay,
      result.dither,
      result.gamma,
      result.lightIntensity,
      result.lightTransferMode,
      static_cast<float>(width) / static_cast<float>(height),
  };
  if (!std::all_of(scalars.begin(), scalars.end(),
                   [](const float value) { return std::isfinite(value); })) {
    result.unsupportedReason = "RadialBlur uniforms are non-finite";
    return result;
  }
  result.exactPathSupported = true;
  return result;
}

QtShakeContract ResolveQtShakeContract(const text::TextEffectPostEffectNode &effect,
                                       const double progress,
                                       const double effectTimeSeconds,
                                       const int width, const int height) {
  QtShakeContract result;
  result.width = width;
  result.height = height;
  if (width <= 0 || height <= 0 || !std::isfinite(effectTimeSeconds)) {
    result.unsupportedReason = "Shake target or effect clock is invalid";
    return result;
  }
  // The caller supplies the runtime-node lifecycle clock. It is deliberately
  // independent from random-access animation progress; captured checkpoints
  // belong to preview/evidence orchestration, never to this executor.
  result.waveTimeSeconds = effectTimeSeconds;
  result.implementationVersion = kQtTextShakeImplementationVersion;

  const double ampRatio =
      SampleParameterValue(effect, "ampRatio", progress, 1.0);
  const double frequencyRatio =
      SampleParameterValue(effect, "frqRatio", progress, 1.0);
  const double phase0 = SampleParameterValue(effect, "phase0", progress, 0.0);
  const double scale0 = SampleParameterValue(effect, "scale0", progress, 1.0);
  const double blurEnabled =
      SampleParameterValue(effect, "blurEnabled", progress, 0.0);
  const double blurIntensity =
      SampleParameterValue(effect, "blurIntensity", progress, 1.0);
  const double blurQuality =
      SampleParameterValue(effect, "blurQuality", progress, 0.0);
  const double seed = SampleParameterValue(effect, "seed", progress, 0.0);
  const double authoredFillX =
      SampleParameterValue(effect, "xFill", progress, 0.0);
  const double authoredFillY =
      SampleParameterValue(effect, "yFill", progress, 0.0);

  const double xAmpRandom =
      SampleParameterValue(effect, "xAmpR", progress, 192.0);
  const double xFrequencyRandom =
      SampleParameterValue(effect, "xFrqR", progress, 1.0);
  const double xAmp = SampleParameterValue(effect, "xAmp", progress, 0.0);
  const double xFrequency = SampleParameterValue(effect, "xFrq", progress, 0.5);
  const double xPhase = SampleParameterValue(effect, "xPhase", progress, 0.0);
  const double yAmpRandom =
      SampleParameterValue(effect, "yAmpR", progress, 192.0);
  const double yFrequencyRandom =
      SampleParameterValue(effect, "yFrqR", progress, 1.0);
  const double yAmp = SampleParameterValue(effect, "yAmp", progress, 0.0);
  const double yFrequency = SampleParameterValue(effect, "yFrq", progress, 0.5);
  const double yPhase = SampleParameterValue(effect, "yPhase", progress, 0.0);
  const double zAmpRandom =
      SampleParameterValue(effect, "zAmpR", progress, 0.0);
  const double zFrequencyRandom =
      SampleParameterValue(effect, "zFrqR", progress, 1.0);
  const double zAmp = SampleParameterValue(effect, "zAmp", progress, 0.0);
  const double zFrequency = SampleParameterValue(effect, "zFrq", progress, 0.5);
  const double zPhase = SampleParameterValue(effect, "zPhase", progress, 0.0);
  const double wAmpRandom =
      SampleParameterValue(effect, "wAmpR", progress, 0.0);
  const double wFrequencyRandom =
      SampleParameterValue(effect, "wFrqR", progress, 1.0);
  const double wAmp = SampleParameterValue(effect, "wAmp", progress, 0.0);
  const double wFrequency = SampleParameterValue(effect, "wFrq", progress, 0.5);
  const double wPhase = SampleParameterValue(effect, "wPhase", progress, 0.0);

  const std::array<double, 3> channelAmpRatio{
      SampleParameterValue(effect, "rAmpRatio", progress, 1.0),
      SampleParameterValue(effect, "gAmpRatio", progress, 1.0),
      SampleParameterValue(effect, "bAmpRatio", progress, 1.0)};
  const std::array<double, 3> channelPhase{
      SampleParameterValue(effect, "rPhase", progress, 0.0),
      SampleParameterValue(effect, "gPhase", progress, 0.0),
      SampleParameterValue(effect, "bPhase", progress, 0.0)};
  // Present in the source component but intentionally unused by apply().
  const double rgbRandom =
      SampleParameterValue(effect, "rgbRnd", progress, 0.0);
  const double rgbFrequency =
      SampleParameterValue(effect, "rgbFrq", progress, 2.0);

  const std::array<double, 36> finiteInputs{ampRatio,
                                            frequencyRatio,
                                            phase0,
                                            scale0,
                                            blurEnabled,
                                            blurIntensity,
                                            blurQuality,
                                            seed,
                                            authoredFillX,
                                            authoredFillY,
                                            xAmpRandom,
                                            xFrequencyRandom,
                                            xAmp,
                                            xFrequency,
                                            xPhase,
                                            yAmpRandom,
                                            yFrequencyRandom,
                                            yAmp,
                                            yFrequency,
                                            yPhase,
                                            zAmpRandom,
                                            zFrequencyRandom,
                                            zAmp,
                                            zFrequency,
                                            zPhase,
                                            wAmpRandom,
                                            wFrequencyRandom,
                                            wAmp,
                                            wFrequency,
                                            wPhase,
                                            channelAmpRatio[0],
                                            channelAmpRatio[1],
                                            channelAmpRatio[2],
                                            channelPhase[0],
                                            channelPhase[1],
                                            channelPhase[2]};
  if (!std::all_of(finiteInputs.begin(), finiteInputs.end(),
                   [](const double value) { return std::isfinite(value); }) ||
      !std::isfinite(rgbRandom) || !std::isfinite(rgbFrequency)) {
    result.unsupportedReason = "Shake parameters are non-finite";
    return result;
  }
  result.fillModeX = static_cast<int>(std::lround(authoredFillX));
  result.fillModeY = static_cast<int>(std::lround(authoredFillY));
  if (std::fabs(authoredFillX - static_cast<double>(result.fillModeX)) >
          1.0e-9 ||
      std::fabs(authoredFillY - static_cast<double>(result.fillModeY)) >
          1.0e-9 ||
      result.fillModeX < 0 || result.fillModeX > 2 || result.fillModeY < 0 ||
      result.fillModeY > 2) {
    result.unsupportedReason = "Shake fill mode is invalid";
    return result;
  }
  if (blurEnabled != 0.0 && blurEnabled != 1.0) {
    result.unsupportedReason = "Shake blurEnabled is not boolean";
    return result;
  }
  result.motionBlurEnabled = blurEnabled != 0.0;
  if (result.motionBlurEnabled) {
    result.unsupportedReason =
        "Shake motion-blur matrix-array branch is not source-closed";
    return result;
  }

  constexpr double kDesignSize = 1080.0;
  const double designScale =
      kDesignSize / static_cast<double>(std::min(width, height));
  const double normalizedWidth = static_cast<double>(width) * designScale;
  const double normalizedHeight = static_cast<double>(height) * designScale;
  for (std::size_t channel = 0U; channel < result.uvMatrices.size();
       ++channel) {
    const double amplitude = ampRatio * channelAmpRatio[channel];
    const double phase = phase0 + channelPhase[channel];
    const double time = result.waveTimeSeconds;
    const double dx =
        ShakeMainWave(amplitude * xAmp, frequencyRatio * xFrequency,
                      phase + xPhase, time) +
        ShakeRandomWave(ShakeRandomAxis::X, amplitude * xAmpRandom,
                        frequencyRatio * xFrequencyRandom, phase + xPhase, time,
                        seed);
    const double dy =
        ShakeMainWave(amplitude * yAmp, frequencyRatio * yFrequency,
                      phase + yPhase, time) +
        ShakeRandomWave(ShakeRandomAxis::Y, amplitude * yAmpRandom,
                        frequencyRatio * yFrequencyRandom, phase + yPhase, time,
                        seed);
    const double dz =
        ShakeMainWave(amplitude * zAmp, frequencyRatio * zFrequency,
                      phase + zPhase, time) +
        ShakeRandomWave(ShakeRandomAxis::Z, amplitude * zAmpRandom,
                        frequencyRatio * zFrequencyRandom, phase + zPhase, time,
                        seed);
    const double dw =
        ShakeMainWave(amplitude * wAmp, frequencyRatio * wFrequency,
                      phase + wPhase, time) +
        ShakeRandomWave(ShakeRandomAxis::W, amplitude * wAmpRandom,
                        frequencyRatio * wFrequencyRandom, phase + wPhase, time,
                        seed);
    const double inverseLayerScale =
        std::max(scale0 * kDesignSize + dz, kDesignSize * 0.1) / kDesignSize;
    ShakeMatrix4 matrix =
        ShakeScaleMatrix(1.0 / normalizedWidth, 1.0 / normalizedHeight);
    matrix =
        ShakeMultiply(matrix, ShakeTranslationMatrix(normalizedWidth * 0.5,
                                                     normalizedHeight * 0.5));
    matrix = ShakeMultiply(matrix, ShakeRotationMatrix(dw * kShakePi / 180.0));
    matrix = ShakeMultiply(
        matrix, ShakeScaleMatrix(inverseLayerScale, inverseLayerScale));
    matrix = ShakeMultiply(
        matrix, ShakeTranslationMatrix(-normalizedWidth * 0.5 + dx,
                                       -normalizedHeight * 0.5 + dy));
    matrix = ShakeMultiply(matrix,
                           ShakeScaleMatrix(normalizedWidth, normalizedHeight));
    for (std::size_t index = 0U; index < matrix.size(); ++index) {
      result.uvMatrices[channel][index] = static_cast<float>(matrix[index]);
      if (!std::isfinite(result.uvMatrices[channel][index])) {
        result.unsupportedReason = "Shake matrix is non-finite";
        return result;
      }
    }
  }
  result.exactPathSupported = true;
  return result;
}

} // namespace videocut::skia_runtime::internal
