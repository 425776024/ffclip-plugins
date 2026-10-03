#include "text/QtTextProPostEffectPipeline.h"
#include "text/TextExecutionParameterContract.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <initializer_list>
#include <string_view>
#include <utility>

namespace videocut::skia_runtime::internal {
namespace {

const text::TextPostEffectParameter *
FindParameter(const text::TextEffectPostEffectNode &effect, const char *name) {
  const auto found =
      std::find_if(effect.parameters.begin(), effect.parameters.end(),
                   [name](const text::TextPostEffectParameter &parameter) {
                     return parameter.name == name;
                   });
  return found == effect.parameters.end() ? nullptr : &*found;
}

float SampleParameter(const text::TextEffectPostEffectNode &effect, const char *name,
                      const double progress, const float fallback) {
  const auto *parameter = FindParameter(effect, name);
  if (!parameter)
    return fallback;
  const float base =
      parameter->values.empty() ? fallback : parameter->values.front();
  return static_cast<float>(text::SampleTextKeyframeCurve(
      parameter->keyframes, progress, static_cast<double>(base)));
}

std::array<float, 2> Vector2Parameter(const text::TextEffectPostEffectNode &effect,
                                      const char *name,
                                      const std::array<float, 2> fallback) {
  const auto *parameter = FindParameter(effect, name);
  if (!parameter || parameter->values.empty())
    return fallback;
  auto value = fallback;
  for (std::size_t index = 0;
       index < std::min(value.size(), parameter->values.size()); ++index) {
    value[index] = parameter->values[index];
  }
  return value;
}

bool Finite(const float value) { return std::isfinite(value); }

bool ParametersNamed(
    const text::TextEffectPostEffectNode &effect,
    const std::initializer_list<std::string_view> names,
    const std::initializer_list<std::string_view> prefixes = {}) {
  return std::all_of(
      effect.parameters.begin(), effect.parameters.end(),
      [&](const text::TextPostEffectParameter &parameter) {
        const std::string_view name{parameter.name};
        if (std::find(names.begin(), names.end(), name) != names.end())
          return true;
        return std::any_of(prefixes.begin(), prefixes.end(),
                           [&](const std::string_view prefix) {
                             return name.starts_with(prefix);
                           });
      });
}

bool ScalarParameterClosed(const text::TextEffectPostEffectNode &effect,
                           const char *name) {
  const auto *parameter = FindParameter(effect, name);
  return !parameter || parameter->values.size() <= 1U;
}

bool Vector2ParameterClosed(const text::TextEffectPostEffectNode &effect,
                            const char *name) {
  const auto *parameter = FindParameter(effect, name);
  return !parameter ||
         (parameter->values.size() <= 2U && parameter->keyframes.empty());
}

bool Vector2Or3ParameterClosed(const text::TextEffectPostEffectNode &effect,
                               const char *name) {
  const auto *parameter = FindParameter(effect, name);
  return !parameter ||
         ((parameter->values.size() == 2U ||
           parameter->values.size() == 3U) &&
          parameter->keyframes.empty());
}

bool Vector4ParameterClosed(const text::TextEffectPostEffectNode &effect,
                            const char *name) {
  const auto *parameter = FindParameter(effect, name);
  return !parameter ||
         (parameter->values.size() == 4U && parameter->keyframes.empty());
}

bool ParametersFinite(const text::TextEffectPostEffectNode &effect,
                      const double progress) {
  return std::all_of(
      effect.parameters.begin(), effect.parameters.end(),
      [&](const text::TextPostEffectParameter &parameter) {
        return std::all_of(parameter.values.begin(), parameter.values.end(),
                           [](const float value) { return Finite(value); }) &&
               (parameter.keyframes.empty() ||
                Finite(static_cast<float>(text::SampleTextKeyframeCurve(
                    parameter.keyframes, progress,
                    parameter.values.empty() ? 0.0
                                             : parameter.values.front()))));
      });
}

bool InClosedRange(const float value, const float minimum,
                   const float maximum) {
  return Finite(value) && value >= minimum && value <= maximum;
}

template <std::size_t Size> bool Finite(const std::array<float, Size> &values) {
  return std::all_of(values.begin(), values.end(),
                     [](const float value) { return Finite(value); });
}

bool ValidBounds(const SkRect &recordingBounds, SkIRect &rasterBounds,
                 QtTextProPostEffectPlan &plan) {
  if (!recordingBounds.isFinite() || recordingBounds.isEmpty()) {
    plan.unsupportedReason = "recording bounds are empty or non-finite";
    return false;
  }
  recordingBounds.roundOut(&rasterBounds);
  plan.width = rasterBounds.width();
  plan.height = rasterBounds.height();
  if (plan.width <= 0 || plan.height <= 0) {
    plan.unsupportedReason = "rounded Page target is empty";
    return false;
  }
  const auto pixels = static_cast<std::size_t>(plan.width) *
                      static_cast<std::size_t>(plan.height);
  if (pixels > 16U * 1024U * 1024U) {
    plan.unsupportedReason = "Page target exceeds the verified allocation cap";
    return false;
  }
  return true;
}

void AppendFullSizePass(QtTextProPostEffectPlan &plan,
                        const QtTextProPostEffectPassKind kind) {
  plan.passes.push_back({kind, plan.width, plan.height,
                         QtTextPostEffectSurfaceSemantics::ColorPremultiplied});
}

void AppendRawFullSizePass(QtTextProPostEffectPlan &plan,
                           const QtTextProPostEffectPassKind kind) {
  plan.passes.push_back({kind, plan.width, plan.height,
                         QtTextPostEffectSurfaceSemantics::RawRgbaData});
}

const char *ExpectedChainName(const text::TextPostEffectKind kind) {
  switch (kind) {
  case text::TextPostEffectKind::RadianceGlow:
    return "radiance";
  case text::TextPostEffectKind::AlphaOutline:
    return "alpha_outline";
  case text::TextPostEffectKind::TurbulenceDisplacement:
    return "turbulence";
  case text::TextPostEffectKind::GodRay:
    return "god_ray";
  case text::TextPostEffectKind::LinearWipe:
    return "linear_wipe";
  case text::TextPostEffectKind::GaussianBlur:
    return "gaussian_blur";
  case text::TextPostEffectKind::SoftGlow:
    return "soft_glow";
  case text::TextPostEffectKind::DistortChroma:
    return "distort_chroma";
  case text::TextPostEffectKind::RadialBlur:
    return "radial_blur";
  case text::TextPostEffectKind::Shake:
    return "shake";
  case text::TextPostEffectKind::Trail:
    return "trail";
  case text::TextPostEffectKind::DirectionalBlur:
    return "directional_blur";
  case text::TextPostEffectKind::Dust:
    return "dust";
  case text::TextPostEffectKind::DeepGlow:
    return "deep_glow";
  case text::TextPostEffectKind::SGlow:
    return "s_glow";
  case text::TextPostEffectKind::WaveWarp:
    return "wave_warp";
  case text::TextPostEffectKind::ChromaticAberration:
    return "chromatic_aberration";
  case text::TextPostEffectKind::MultiShadow:
    return "multi_shadow";
  case text::TextPostEffectKind::SimpleChoker:
    return "simple_choker";
  case text::TextPostEffectKind::CCLens:
    return "cc_lens";
  case text::TextPostEffectKind::OpticsCompensation:
    return "optics_compensation";
  case text::TextPostEffectKind::Projection:
    return "projection";
  case text::TextPostEffectKind::DynamicSignalGlitch:
    return "dynamic_signal_glitch";
  case text::TextPostEffectKind::ElectricPulseMotionBlur:
    return "electric_pulse_motion_blur";
  case text::TextPostEffectKind::MorphologicalOutline:
    return "morphological_outline";
  case text::TextPostEffectKind::AlternatingSegmentMask:
    return "alternating_segment_mask";
  case text::TextPostEffectKind::CenterSplitDisplacement:
    return "center_split_displacement";
  case text::TextPostEffectKind::CutAndDrop:
    return "cut_and_drop";
  case text::TextPostEffectKind::BlockGlitchDotMatrix:
    return "block_glitch_dot_matrix";
  case text::TextPostEffectKind::ProceduralFlameOutline:
    return "procedural_flame_outline";
  case text::TextPostEffectKind::CylindricalScroll:
    return "cylindrical_scroll";
  case text::TextPostEffectKind::SplitSqueezeWave:
    return "split_squeeze_wave";
  case text::TextPostEffectKind::PerspectiveEchoTrail:
    return "perspective_echo_trail";
  case text::TextPostEffectKind::PulseEnvelope:
    return "pulse_envelope";
  default:
    return "unsupported";
  }
}

std::string
ValidateExecutionTrace(const QtTextProPostEffectPlan &plan,
                       const QtTextPostEffectExecutionTrace &execution) {
  if (execution.pageWidth != plan.width ||
      execution.pageHeight != plan.height) {
    return "executor Page target differs from the resolved plan";
  }
  if (execution.passes.size() != plan.passes.size()) {
    return "executor pass count differs from the resolved plan";
  }
  const char *chain = ExpectedChainName(plan.kind);
  for (std::size_t index = 0; index < plan.passes.size(); ++index) {
    const auto &planned = plan.passes[index];
    const auto &executed = execution.passes[index];
    if (executed.chain != chain ||
        executed.stage != QtTextProPostEffectPassName(planned.kind) ||
        executed.ordinal != index + 1U ||
        executed.passCount != plan.passes.size() ||
        executed.width != planned.width || executed.height != planned.height ||
        executed.surfaceSemantics != planned.surfaceSemantics) {
      return "executor pass topology differs from the resolved plan";
    }
  }
  return {};
}

} // namespace

const char *
QtTextProPostEffectPassName(const QtTextProPostEffectPassKind kind) noexcept {
  switch (kind) {
  case QtTextProPostEffectPassKind::RadianceThreshold:
    return "radiance_threshold";
  case QtTextProPostEffectPassKind::RadianceDownsample:
    return "radiance_downsample";
  case QtTextProPostEffectPassKind::RadianceErode:
    return "radiance_erode";
  case QtTextProPostEffectPassKind::RadianceDirectionalBlur:
    return "radiance_directional_blur";
  case QtTextProPostEffectPassKind::RadianceBlend:
    return "radiance_blend";
  case QtTextProPostEffectPassKind::AlphaOutline:
    return "alpha_outline";
  case QtTextProPostEffectPassKind::TurbulenceNoise:
    return "turbulence_noise";
  case QtTextProPostEffectPassKind::TurbulenceDisplacement:
    return "turbulence_displacement";
  case QtTextProPostEffectPassKind::GodRayThreshold:
    return "god_ray_threshold";
  case QtTextProPostEffectPassKind::GodRayGaussianX:
    return "god_ray_gaussian_x";
  case QtTextProPostEffectPassKind::GodRayGaussianY:
    return "god_ray_gaussian_y";
  case QtTextProPostEffectPassKind::GodRayComposite:
    return "god_ray_composite";
  case QtTextProPostEffectPassKind::LinearWipe:
    return "linear_wipe";
  case QtTextProPostEffectPassKind::GaussianCopy:
    return "gaussian_copy";
  case QtTextProPostEffectPassKind::GaussianDownsample:
    return "gaussian_downsample";
  case QtTextProPostEffectPassKind::GaussianX:
    return "gaussian_x";
  case QtTextProPostEffectPassKind::GaussianY:
    return "gaussian_y";
  case QtTextProPostEffectPassKind::GaussianUpsample:
    return "gaussian_upsample";
  case QtTextProPostEffectPassKind::SoftGlowThreshold:
    return "soft_glow_threshold";
  case QtTextProPostEffectPassKind::SoftGlowCopy:
    return "soft_glow_copy";
  case QtTextProPostEffectPassKind::SoftGlowX:
    return "soft_glow_x";
  case QtTextProPostEffectPassKind::SoftGlowY:
    return "soft_glow_y";
  case QtTextProPostEffectPassKind::SoftGlowBlend:
    return "soft_glow_blend";
  case QtTextProPostEffectPassKind::DistortLens:
    return "distort_lens";
  case QtTextProPostEffectPassKind::DistortBlurX1:
    return "distort_blur_x1";
  case QtTextProPostEffectPassKind::DistortBlurY1:
    return "distort_blur_y1";
  case QtTextProPostEffectPassKind::DistortBlurX2:
    return "distort_blur_x2";
  case QtTextProPostEffectPassKind::DistortBlurY2:
    return "distort_blur_y2";
  case QtTextProPostEffectPassKind::DistortOutput:
    return "distort_output";
  case QtTextProPostEffectPassKind::RadialBlur:
    return "radial_blur";
  case QtTextProPostEffectPassKind::Shake:
    return "shake";
  case QtTextProPostEffectPassKind::TrailBlurX:
    return "trail_blur_x";
  case QtTextProPostEffectPassKind::TrailBlurY:
    return "trail_blur_y";
  case QtTextProPostEffectPassKind::TrailTimeA:
    return "trail_time_a";
  case QtTextProPostEffectPassKind::TrailTimeBCommit:
    return "trail_time_b_commit";
  case QtTextProPostEffectPassKind::TrailHint:
    return "trail_hint";
  case QtTextProPostEffectPassKind::DirectionalBlurCopy:
    return "directional_blur_copy";
  case QtTextProPostEffectPassKind::DirectionalBlur:
    return "directional_blur";
  case QtTextProPostEffectPassKind::DustNoise:
    return "dust_noise";
  case QtTextProPostEffectPassKind::DustMaskNoise:
    return "dust_mask_noise";
  case QtTextProPostEffectPassKind::DustParticles:
    return "dust_particles";
  case QtTextProPostEffectPassKind::DeepGlowPreprocess:
    return "deep_glow_preprocess";
  case QtTextProPostEffectPassKind::DeepGlowDownscale:
    return "deep_glow_downscale";
  case QtTextProPostEffectPassKind::DeepGlowX:
    return "deep_glow_x";
  case QtTextProPostEffectPassKind::DeepGlowY:
    return "deep_glow_y";
  case QtTextProPostEffectPassKind::DeepGlowComposite:
    return "deep_glow_composite";
  case QtTextProPostEffectPassKind::DeepGlowPostprocess:
    return "deep_glow_postprocess";
  case QtTextProPostEffectPassKind::SGlowThreshold:
    return "s_glow_threshold";
  case QtTextProPostEffectPassKind::SGlowHorizontalFirst:
    return "s_glow_horizontal_first";
  case QtTextProPostEffectPassKind::SGlowHorizontalSecond:
    return "s_glow_horizontal_second";
  case QtTextProPostEffectPassKind::SGlowVerticalFirst:
    return "s_glow_vertical_first";
  case QtTextProPostEffectPassKind::SGlowVerticalSecond:
    return "s_glow_vertical_second";
  case QtTextProPostEffectPassKind::SGlowComposite:
    return "s_glow_composite";
  case QtTextProPostEffectPassKind::WaveWarp:
    return "wave_warp";
  case QtTextProPostEffectPassKind::ChromaticAberration:
    return "chromatic_aberration";
  case QtTextProPostEffectPassKind::MultiShadow:
    return "multi_shadow";
  case QtTextProPostEffectPassKind::SimpleChoker:
    return "simple_choker";
  case QtTextProPostEffectPassKind::SimpleChokerDownscale:
    return "simple_choker_downscale";
  case QtTextProPostEffectPassKind::SimpleChokerMatte:
    return "simple_choker_matte";
  case QtTextProPostEffectPassKind::SimpleChokerBlend:
    return "simple_choker_blend";
  case QtTextProPostEffectPassKind::CCLens:
    return "cc_lens";
  case QtTextProPostEffectPassKind::OpticsCompensation:
    return "optics_compensation";
  case QtTextProPostEffectPassKind::OpticsCompensationDistort:
    return "optics_compensation_distort";
  case QtTextProPostEffectPassKind::OpticsCompensationAntiAliasing:
    return "optics_compensation_anti_aliasing";
  case QtTextProPostEffectPassKind::Projection:
    return "projection";
  case QtTextProPostEffectPassKind::DynamicSignalGlitch:
    return "dynamic_signal_glitch";
  case QtTextProPostEffectPassKind::ElectricPulseMotionBlur:
    return "electric_pulse_motion_blur";
  case QtTextProPostEffectPassKind::MorphologicalOutline:
    return "morphological_outline";
  case QtTextProPostEffectPassKind::AlternatingSegmentMask:
    return "alternating_segment_mask";
  case QtTextProPostEffectPassKind::CenterSplitDisplacement:
    return "center_split_displacement";
  case QtTextProPostEffectPassKind::CutAndDrop:
    return "cut_and_drop";
  case QtTextProPostEffectPassKind::BlockGlitchDotMatrix:
    return "block_glitch_dot_matrix";
  case QtTextProPostEffectPassKind::ProceduralFlameOutline:
    return "procedural_flame_outline";
  case QtTextProPostEffectPassKind::CylindricalScroll:
    return "cylindrical_scroll";
  case QtTextProPostEffectPassKind::SplitSqueezeWave:
    return "split_squeeze_wave";
  case QtTextProPostEffectPassKind::PerspectiveEchoTrail:
    return "perspective_echo_trail";
  case QtTextProPostEffectPassKind::PulseEnvelope:
    return "pulse_envelope";
  }
  return "unknown";
}

QtTextProPostEffectPlan ResolveQtTextProPostEffectPlan(
    const text::TextEffectPostEffectNode &effect, const double progress,
    const SkRect &recordingBounds,
    const QtTextPostEffectStateContext *stateContext) {
  QtTextProPostEffectPlan plan;
  plan.kind = effect.kind;
  SkIRect rasterBounds;
  if (!Finite(progress)) {
    plan.unsupportedReason = "progress is non-finite";
    return plan;
  }
  if (!ValidBounds(recordingBounds, rasterBounds, plan))
    return plan;

  const auto kind = effect.kind;
  if (kind == text::TextPostEffectKind::RadianceGlow) {
    const auto contract = ResolveQtRadianceGlowContract(
        effect, progress, plan.width, plan.height);
    auto &uniforms = plan.radiance;
    uniforms.blurWidth = contract.blurWidth;
    uniforms.blurHeight = contract.blurHeight;
    uniforms.effectiveExposure = contract.exposure;
    uniforms.sampleCount = contract.sampleCount;
    uniforms.sigma = contract.sigma;
    uniforms.radius = contract.sigma * 3.0F;
    uniforms.effectiveGlowIntensity = uniforms.radius * 1000.0F;
    uniforms.sampleStep = uniforms.sampleCount > 0.0F
                              ? uniforms.radius / uniforms.sampleCount
                              : 0.0F;
    uniforms.stepX = contract.stepX;
    uniforms.stepY = contract.stepY;
    uniforms.directionCount = contract.directionCount;
    uniforms.erodeIterations = contract.erodeIterations;
    uniforms.erodeStepX = contract.erodeStepX;
    uniforms.erodeStepY = contract.erodeStepY;
    uniforms.thresholdLow = contract.thresholdLow;
    uniforms.thresholdHigh = contract.thresholdHigh;
    uniforms.thresholdSmooth = contract.thresholdSmooth;
    uniforms.thresholdType = contract.thresholdType;
    uniforms.grayScale = contract.grayScale;
    uniforms.spaceDither = contract.spaceDither;
    uniforms.borderType = contract.borderType;
    uniforms.displayGlow = contract.displayGlow;
    uniforms.glowColor = contract.glowColor;
    if (!Finite(uniforms.effectiveGlowIntensity) ||
        !Finite(uniforms.effectiveExposure) || !Finite(uniforms.sampleCount) ||
        !Finite(uniforms.sigma) || !Finite(uniforms.stepX) ||
        !Finite(uniforms.stepY) || !Finite(uniforms.thresholdLow) ||
        !Finite(uniforms.thresholdHigh) || !Finite(uniforms.thresholdSmooth) ||
        !Finite(uniforms.thresholdType) || !Finite(uniforms.grayScale) ||
        !Finite(uniforms.spaceDither) || !Finite(uniforms.borderType) ||
        !Finite(uniforms.displayGlow) || !Finite(uniforms.glowColor) ||
        !Finite(uniforms.erodeStepX) || !Finite(uniforms.erodeStepY)) {
      plan.unsupportedReason = "Radiance uniforms are non-finite";
      return plan;
    }
    if (!contract.exactPathSupported) {
      plan.unsupportedReason = contract.unsupportedReason;
      return plan;
    }
    plan.supported = true;
    AppendFullSizePass(plan, QtTextProPostEffectPassKind::RadianceThreshold);
    plan.passes.push_back({QtTextProPostEffectPassKind::RadianceDownsample,
                           uniforms.blurWidth, uniforms.blurHeight,
                           QtTextPostEffectSurfaceSemantics::ColorPremultiplied});
    for (int iteration = 0; iteration < uniforms.erodeIterations; ++iteration) {
      plan.passes.push_back({QtTextProPostEffectPassKind::RadianceErode,
                            uniforms.blurWidth, uniforms.blurHeight,
                            QtTextPostEffectSurfaceSemantics::ColorPremultiplied});
    }
    for (int direction = 0; direction < uniforms.directionCount; ++direction) {
      plan.passes.push_back(
          {QtTextProPostEffectPassKind::RadianceDirectionalBlur,
           uniforms.blurWidth, uniforms.blurHeight,
           QtTextPostEffectSurfaceSemantics::ColorPremultiplied});
    }
    AppendFullSizePass(plan, QtTextProPostEffectPassKind::RadianceBlend);
    return plan;
  }

  if (kind == text::TextPostEffectKind::AlphaOutline) {
    const auto contract = ResolveQtAlphaOutlineContract(
        effect, progress, plan.width, plan.height);
    auto &uniforms = plan.alphaOutline;
    uniforms.offsetX = contract.offsetX;
    uniforms.offsetY = contract.offsetY;
    uniforms.ratio = contract.ratio;
    uniforms.size = contract.size;
    uniforms.scaleX = contract.scaleX;
    uniforms.scaleY = contract.scaleY;
    uniforms.outlineColor = contract.outlineColor;
    uniforms.intensity = contract.intensity;
    if (!Finite(uniforms.offsetX) || !Finite(uniforms.offsetY) ||
        !Finite(uniforms.ratio) || uniforms.ratio <= 0.0F ||
        !Finite(uniforms.size) || !Finite(uniforms.scaleX) ||
        !Finite(uniforms.scaleY) || !Finite(uniforms.outlineColor) ||
        !Finite(uniforms.intensity)) {
      plan.unsupportedReason = "AlphaOutline uniforms are non-finite";
      return plan;
    }
    plan.supported = true;
    AppendFullSizePass(plan, QtTextProPostEffectPassKind::AlphaOutline);
    return plan;
  }

  if (kind == text::TextPostEffectKind::TurbulenceDisplacement) {
    const auto contract =
        ResolveQtTurbulenceContract(effect, progress, plan.width, plan.height);
    auto &uniforms = plan.turbulence;
    const float qualityType =
        SampleParameter(effect, "qualityType", progress, 3.0F);
    if (!Finite(qualityType)) {
      plan.unsupportedReason = "Turbulence qualityType is non-finite";
      return plan;
    }
    // setEffectAttr maps exact numeric 0/1/2 to Ultimate/High/Medium and
    // every other value to Low. The shared executor is source-closed for the
    // four authored integer values; reject fractional values rather than
    // silently choosing an untyped threshold branch.
    if (qualityType == 0.0F)
      uniforms.qualityType = 0;
    else if (qualityType == 1.0F)
      uniforms.qualityType = 1;
    else if (qualityType == 2.0F)
      uniforms.qualityType = 2;
    else if (qualityType == 3.0F)
      uniforms.qualityType = 3;
    else {
      plan.unsupportedReason =
          "fractional Turbulence qualityType is not executor-closed";
      return plan;
    }
    uniforms.qualityScale = uniforms.qualityType == 0   ? 1.0F
                            : uniforms.qualityType == 1 ? 0.75F
                            : uniforms.qualityType == 2 ? 0.5F
                                                        : 0.25F;
    uniforms.noiseWidth = contract.noiseWidth;
    uniforms.noiseHeight = contract.noiseHeight;
    uniforms.cycle = contract.cycle;
    uniforms.offsetX = contract.offsetX;
    uniforms.offsetY = contract.offsetY;
    uniforms.quantity = contract.quantity;
    uniforms.complexity = contract.complexity;
    uniforms.evolution = contract.evolution;
    uniforms.type = contract.type;
    uniforms.contrast = contract.contrast;
    uniforms.fixType = SampleParameter(effect, "fix_type", progress, 0.0F);
    uniforms.pictureScale = contract.pictureScale;
    uniforms.motionTileType = contract.motionTileType;
    uniforms.subImpact =
        SampleParameter(effect, "subImpact", progress, 0.60000002384185791F);
    uniforms.subScale = SampleParameter(effect, "subScale", progress, 56.0F);
    uniforms.subRotate = SampleParameter(effect, "subRotate", progress, 0.0F);
    uniforms.subOffset = Vector2Parameter(effect, "subOffset", {0.0F, 0.0F});
    uniforms.range =
        SampleParameter(effect, "range", progress, 0.30000001192092896F);

    if (!Finite(uniforms.cycle) || !Finite(uniforms.offsetX) ||
        !Finite(uniforms.offsetY) || !Finite(uniforms.quantity) ||
        !Finite(uniforms.complexity) || !Finite(uniforms.evolution) ||
        !Finite(uniforms.type) || !Finite(uniforms.contrast) ||
        !Finite(uniforms.fixType) || !Finite(uniforms.pictureScale) ||
        !Finite(uniforms.motionTileType) || !Finite(uniforms.subImpact) ||
        !Finite(uniforms.subScale) || !Finite(uniforms.subRotate) ||
        !Finite(uniforms.subOffset) || !Finite(uniforms.range)) {
      plan.unsupportedReason = "Turbulence uniforms are non-finite";
      return plan;
    }

    if (!contract.exactPathSupported) {
      plan.unsupportedReason = contract.unsupportedReason;
      return plan;
    }
    // The active f52 branch and its fixed shaders prove these defaults. The
    // existing executor cannot bind alternate material-private values yet;
    // fail closed instead of silently ignoring an override.
    if (uniforms.subImpact != 0.60000002384185791F ||
        uniforms.subScale != 56.0F || uniforms.subRotate != 0.0F ||
        uniforms.subOffset != std::array<float, 2>{0.0F, 0.0F} ||
        uniforms.range != 0.30000001192092896F) {
      plan.unsupportedReason =
          "material-private Turbulence override is not executor-bound";
      return plan;
    }
    plan.supported = true;
    plan.passes.push_back({QtTextProPostEffectPassKind::TurbulenceNoise,
                           uniforms.noiseWidth, uniforms.noiseHeight,
                           QtTextPostEffectSurfaceSemantics::RawRgbaData});
    AppendFullSizePass(plan,
                       QtTextProPostEffectPassKind::TurbulenceDisplacement);
    return plan;
  }

  if (kind == text::TextPostEffectKind::GodRay) {
    const auto contract =
        ResolveQtGodRayContract(effect, progress, plan.width, plan.height);
    auto &uniforms = plan.godRay;
    uniforms.rayWidth = contract.rayWidth;
    uniforms.rayHeight = contract.rayHeight;
    uniforms.downscale = contract.downscale;
    uniforms.useAlphaThreshold = contract.useAlphaThreshold;
    uniforms.colorType = contract.colorType;
    uniforms.threshold = contract.threshold;
    uniforms.sampleCount = contract.sampleCount;
    uniforms.dx = contract.dx;
    uniforms.dy = contract.dy;
    uniforms.sigmaX = contract.sigmaX;
    uniforms.sigmaY = contract.sigmaY;
    uniforms.intensity = contract.intensity;
    uniforms.brightness = contract.brightness;
    uniforms.quality = contract.quality;
    uniforms.grayscaleCorrection = contract.grayscaleCorrection;
    uniforms.center = contract.center;
    uniforms.useAngle = contract.useAngle;
    uniforms.minAngle = contract.minAngle;
    uniforms.maxAngle = contract.maxAngle;
    uniforms.borderType = contract.borderType;
    uniforms.blendMode = contract.blendMode;
    uniforms.lightColor = contract.lightColor;
    uniforms.displayRayOnly = contract.displayRayOnly;
    uniforms.inverseGammaCorrection = contract.inverseGammaCorrection;
    uniforms.gamma = contract.gamma;
    uniforms.dither = contract.dither;
    uniforms.noiseIntensity = contract.noiseIntensity;
    uniforms.weightDecay = contract.weightDecay;
    uniforms.colorDecay = contract.colorDecay;
    uniforms.normalizationSample = contract.normalizationSample;
    uniforms.sampleScale = contract.sampleScale;
    uniforms.sampleBias = contract.sampleBias;
    const std::array<float, 26> scalarUniforms{
        uniforms.useAngle,
        uniforms.minAngle,
        uniforms.maxAngle,
        uniforms.downscale,
        uniforms.useAlphaThreshold,
        uniforms.colorType,
        uniforms.threshold,
        uniforms.sampleCount,
        uniforms.dx,
        uniforms.dy,
        uniforms.sigmaX,
        uniforms.sigmaY,
        uniforms.intensity,
        uniforms.brightness,
        uniforms.quality,
        uniforms.grayscaleCorrection,
        uniforms.borderType,
        uniforms.blendMode,
        uniforms.displayRayOnly,
        uniforms.inverseGammaCorrection,
        uniforms.gamma,
        uniforms.dither,
        uniforms.noiseIntensity,
        uniforms.weightDecay,
        uniforms.colorDecay,
        uniforms.normalizationSample,
    };
    if (!Finite(scalarUniforms) || !Finite(uniforms.center) ||
        !Finite(uniforms.lightColor) || !Finite(uniforms.sampleScale) ||
        !Finite(uniforms.sampleBias)) {
      plan.unsupportedReason = "GodRay uniforms are non-finite";
      return plan;
    }
    if (!contract.exactPathSupported) {
      plan.unsupportedReason = contract.unsupportedReason;
      return plan;
    }
    plan.supported = true;
    plan.passes.push_back(
        {QtTextProPostEffectPassKind::GodRayThreshold, uniforms.rayWidth,
         uniforms.rayHeight,
         QtTextPostEffectSurfaceSemantics::ColorPremultiplied});
    if (uniforms.sampleCount > 0.00000999999974737875163555145263671875F) {
      plan.passes.push_back(
          {QtTextProPostEffectPassKind::GodRayGaussianX, uniforms.rayWidth,
           uniforms.rayHeight,
           QtTextPostEffectSurfaceSemantics::ColorPremultiplied});
      plan.passes.push_back(
          {QtTextProPostEffectPassKind::GodRayGaussianY, uniforms.rayWidth,
           uniforms.rayHeight,
           QtTextPostEffectSurfaceSemantics::ColorPremultiplied});
    }
    AppendFullSizePass(plan, QtTextProPostEffectPassKind::GodRayComposite);
    return plan;
  }

  if (kind == text::TextPostEffectKind::LinearWipe) {
    const auto contract =
        ResolveQtLinearWipeContract(effect, progress, plan.width, plan.height);
    auto &uniforms = plan.linearWipe;
    uniforms.mappedProgress = contract.mappedProgress;
    uniforms.rotationDegrees = contract.rotationDegrees;
    uniforms.feather = contract.feather;
    if (!Finite(uniforms.mappedProgress) || !Finite(uniforms.rotationDegrees) ||
        !Finite(uniforms.feather)) {
      plan.unsupportedReason = "LinearWipe uniforms are non-finite";
      return plan;
    }
    if (!contract.exactPathSupported) {
      plan.unsupportedReason = contract.unsupportedReason;
      return plan;
    }
    plan.supported = true;
    AppendFullSizePass(plan, QtTextProPostEffectPassKind::LinearWipe);
    return plan;
  }

  if (kind == text::TextPostEffectKind::GaussianBlur) {
    const auto contract = ResolveQtGaussianBlurContract(
        effect, progress, plan.width, plan.height);
    auto &uniforms = plan.gaussian;
    uniforms.active = contract.active;
    uniforms.blurWidth = contract.blurWidth;
    uniforms.blurHeight = contract.blurHeight;
    uniforms.intensity = contract.intensity;
    uniforms.sampleCount = contract.sampleCount;
    uniforms.sigmaX = contract.sigmaX;
    uniforms.sigmaY = contract.sigmaY;
    uniforms.stepX = contract.stepX;
    uniforms.stepY = contract.stepY;
    uniforms.gamma = contract.gamma;
    const std::array<float, 7> scalars{uniforms.intensity, uniforms.sampleCount,
                                       uniforms.sigmaX,    uniforms.sigmaY,
                                       uniforms.stepX,     uniforms.stepY,
                                       uniforms.gamma};
    if (!Finite(scalars)) {
      plan.unsupportedReason = "GaussianBlur uniforms are non-finite";
      return plan;
    }
    if (!contract.exactPathSupported) {
      plan.unsupportedReason = contract.unsupportedReason;
      return plan;
    }
    plan.supported = true;
    if (!uniforms.active) {
      AppendRawFullSizePass(plan, QtTextProPostEffectPassKind::GaussianCopy);
      return plan;
    }
    plan.passes.push_back({QtTextProPostEffectPassKind::GaussianDownsample,
                           uniforms.blurWidth, uniforms.blurHeight,
                           QtTextPostEffectSurfaceSemantics::RawRgbaData});
    plan.passes.push_back({QtTextProPostEffectPassKind::GaussianX,
                           uniforms.blurWidth, uniforms.blurHeight,
                           QtTextPostEffectSurfaceSemantics::RawRgbaData});
    plan.passes.push_back({QtTextProPostEffectPassKind::GaussianY,
                           uniforms.blurWidth, uniforms.blurHeight,
                           QtTextPostEffectSurfaceSemantics::RawRgbaData});
    AppendRawFullSizePass(plan, QtTextProPostEffectPassKind::GaussianUpsample);
    return plan;
  }

  if (kind == text::TextPostEffectKind::DirectionalBlur) {
    if (!ParametersNamed(effect,
                         {"angle", "radius", "blurIntensity", "directionNum",
                          "exposure", "quality", "spaceDither", "borderType",
                          "blendMode"})) {
      plan.unsupportedReason = "DirectionalBlur contains an unknown parameter";
      return plan;
    }
    plan.directionalBlur = ResolveQtDirectionalBlurContract(
        effect, progress, plan.width, plan.height);
    if (!plan.directionalBlur.exactPathSupported) {
      plan.unsupportedReason = plan.directionalBlur.unsupportedReason;
      return plan;
    }
    plan.supported = true;
    if (plan.directionalBlur.requiresAlphaOutlineFusion) {
      if (!stateContext || stateContext->renderGraphInstanceId == 0U ||
          effect.nodeId.empty()) {
        plan.supported = false;
        plan.unsupportedReason =
            "directionNum=4 requires a stable authored DAG identity";
        return plan;
      }
      plan.deferredDirectionalBlurFusion = true;
      return plan;
    }
    plan.passes.push_back({QtTextProPostEffectPassKind::DirectionalBlurCopy,
                           plan.directionalBlur.blurWidth,
                           plan.directionalBlur.blurHeight,
                           QtTextPostEffectSurfaceSemantics::RawRgbaData});
    AppendRawFullSizePass(plan, QtTextProPostEffectPassKind::DirectionalBlur);
    return plan;
  }

  if (kind == text::TextPostEffectKind::Dust) {
    if (!ParametersNamed(
            effect,
            {"brightness", "contrast", "quantity", "complexity",
             "evolution", "cycle", "offset_x", "offset_y",
             "mask_noise_brightness", "mask_noise_contrast",
             "mask_noise_quantity", "mask_noise_complexity",
             "mask_noise_evolution", "mask_noise_cycle",
             "mask_noise_offset_x", "mask_noise_offset_y", "distorIns",
             "gravity", "gravityRot", "maskType", "maskFeather",
             "mask_line_rot", "progress"})) {
      plan.unsupportedReason =
          "Dust parameter requires an explicit typed LumiDust lowering";
      return plan;
    }
    if (!stateContext || stateContext->presentationWidth <= 0 ||
        stateContext->presentationHeight <= 0) {
      plan.unsupportedReason =
          "Dust requires the caller-owned presentation target dimensions";
      return plan;
    }
    plan.dust = ResolveQtDustContract(effect, progress, plan.width, plan.height,
                                      stateContext->presentationWidth,
                                      stateContext->presentationHeight);
    if (!plan.dust.exactPathSupported) {
      plan.unsupportedReason = plan.dust.unsupportedReason;
      return plan;
    }
    plan.supported = true;
    plan.passes.push_back({QtTextProPostEffectPassKind::DustNoise,
                           plan.dust.noiseWidth, plan.dust.noiseHeight,
                           QtTextPostEffectSurfaceSemantics::RawRgbaData});
    plan.passes.push_back({QtTextProPostEffectPassKind::DustMaskNoise,
                           plan.dust.noiseWidth, plan.dust.noiseHeight,
                           QtTextPostEffectSurfaceSemantics::RawRgbaData});
    AppendRawFullSizePass(plan, QtTextProPostEffectPassKind::DustParticles);
    return plan;
  }

  if (kind == text::TextPostEffectKind::DeepGlow) {
    if (!ParametersNamed(
            effect,
            {"downSample", "exposure", "gammaCorrect", "gammaValue",
             "glowIter", "quality", "radius", "stepsMult",
             "kernelStrideBase", "kernelStrideAspectPower", "intensity",
             "threshold", "color", "blendMode", "blueOffset", "ca",
             "glowFromAlpha", "greenOffset", "ratio", "redOffset",
             "rotate", "sourceOpacity", "tint", "tintColor", "tintMix",
             "tintMode", "unmult"})) {
      plan.unsupportedReason = "DeepGlow contains an unknown parameter";
      return plan;
    }
    plan.deepGlow = ResolveQtTypedDeepGlowContract(
        effect, progress, plan.width, plan.height);
    if (!plan.deepGlow.exactPathSupported) {
      plan.unsupportedReason = plan.deepGlow.unsupportedReason;
      return plan;
    }
    plan.supported = true;
    AppendRawFullSizePass(plan,
                          QtTextProPostEffectPassKind::DeepGlowPreprocess);
    for (int index = 0; index < plan.deepGlow.glowIterations; ++index) {
      plan.passes.push_back({QtTextProPostEffectPassKind::DeepGlowDownscale,
                             plan.deepGlow.blurWidth,
                             plan.deepGlow.blurHeight,
                             QtTextPostEffectSurfaceSemantics::RawRgbaData});
      plan.passes.push_back({QtTextProPostEffectPassKind::DeepGlowX,
                             plan.deepGlow.blurWidth, plan.deepGlow.blurHeight,
                             QtTextPostEffectSurfaceSemantics::RawRgbaData});
      plan.passes.push_back({QtTextProPostEffectPassKind::DeepGlowY,
                             plan.deepGlow.blurWidth, plan.deepGlow.blurHeight,
                             QtTextPostEffectSurfaceSemantics::RawRgbaData});
      AppendRawFullSizePass(
          plan, QtTextProPostEffectPassKind::DeepGlowComposite);
    }
    AppendRawFullSizePass(plan,
                          QtTextProPostEffectPassKind::DeepGlowPostprocess);
    return plan;
  }

  if (kind == text::TextPostEffectKind::SGlow) {
    if (!ParametersNamed(
            effect,
            {"radius", "intensity", "threshold", "color", "quality",
             "brightness", "combine", "dither", "edgeMode", "glowColor",
             "glowFromAlpha", "glowUnderSource", "glowWidth", "show",
             "sourceOpacity", "thresholdAddColor", "widthBlue",
             "widthGreen", "widthRed", "widthX", "widthY",
             "useAlphaThreshold", "bgBrightness", "lightBackground"})) {
      plan.unsupportedReason = "SGlow contains an unknown parameter";
      return plan;
    }
    plan.sGlow =
        ResolveQtSGlowContract(effect, progress, plan.width, plan.height);
    if (!plan.sGlow.exactPathSupported) {
      plan.unsupportedReason = plan.sGlow.unsupportedReason;
      return plan;
    }
    plan.supported = true;
    plan.passes.push_back({QtTextProPostEffectPassKind::SGlowThreshold,
                           plan.sGlow.blurWidth, plan.sGlow.blurHeight,
                           QtTextPostEffectSurfaceSemantics::RawRgbaData});
    plan.passes.push_back(
        {QtTextProPostEffectPassKind::SGlowHorizontalFirst,
         plan.sGlow.blurWidth, plan.sGlow.blurHeight,
         QtTextPostEffectSurfaceSemantics::RawRgbaData});
    plan.passes.push_back(
        {QtTextProPostEffectPassKind::SGlowHorizontalSecond,
         plan.sGlow.blurWidth, plan.sGlow.blurHeight,
         QtTextPostEffectSurfaceSemantics::RawRgbaData});
    plan.passes.push_back({QtTextProPostEffectPassKind::SGlowVerticalFirst,
                           plan.sGlow.blurWidth, plan.sGlow.blurHeight,
                           QtTextPostEffectSurfaceSemantics::RawRgbaData});
    plan.passes.push_back({QtTextProPostEffectPassKind::SGlowVerticalSecond,
                           plan.sGlow.blurWidth, plan.sGlow.blurHeight,
                           QtTextPostEffectSurfaceSemantics::RawRgbaData});
    AppendRawFullSizePass(plan,
                          QtTextProPostEffectPassKind::SGlowComposite);
    return plan;
  }

  if (kind == text::TextPostEffectKind::WaveWarp) {
    if (!ParametersNamed(effect,
                         {"amplitude", "wavelength", "phase", "angle",
                          "antiAliasing", "direction", "fixedType", "speed",
                          "type"})) {
      plan.unsupportedReason = "WaveWarp contains an unknown parameter";
      return plan;
    }
    plan.waveWarp =
        ResolveQtWaveWarpContract(effect, progress, plan.width, plan.height);
    if (!plan.waveWarp.exactPathSupported) {
      plan.unsupportedReason = plan.waveWarp.unsupportedReason;
      return plan;
    }
    plan.supported = true;
    AppendRawFullSizePass(plan, QtTextProPostEffectPassKind::WaveWarp);
    return plan;
  }

  if (kind == text::TextPostEffectKind::MultiShadow) {
    if (!ParametersNamed(
            effect,
            {"count", "distance", "angle", "blur", "spread", "color",
             "layerNum", "oriAlpha", "isAssociation", "oriBgAlpha",
             "oriBgColor", "oriBgFeather", "oriBgPad", "oriColor",
             "oriPivotX", "oriPivotY", "oriPositionX", "oriPositionY",
             "oriRotation", "oriScaleX", "oriScaleY", "unifiedScale",
             "textExpandRatio"},
            {"alpha_", "positionX_", "positionY_", "scale_", "rotation_",
             "color_", "bgAlpha_", "bgColor_", "bgFeather_", "bgPad_"})) {
      plan.unsupportedReason = "MultiShadow contains an unknown parameter";
      return plan;
    }
    if (!stateContext || stateContext->renderGroupExpandRatioX <= 0.0F ||
        stateContext->renderGroupExpandRatioY <= 0.0F) {
      plan.unsupportedReason =
          "MultiShadow requires render-group expansion ratios";
      return plan;
    }
    plan.multiShadow =
        ResolveQtMultiShadowContract(effect, progress, plan.width, plan.height,
                                     stateContext->renderGroupExpandRatioX,
                                     stateContext->renderGroupExpandRatioY);
    if (!plan.multiShadow.exactPathSupported) {
      plan.unsupportedReason = plan.multiShadow.unsupportedReason;
      return plan;
    }
    plan.supported = true;
    AppendRawFullSizePass(plan, QtTextProPostEffectPassKind::MultiShadow);
    return plan;
  }

  if (kind == text::TextPostEffectKind::SoftGlow) {
    const auto contract =
        ResolveQtSoftGlowContract(effect, progress, plan.width, plan.height);
    auto &uniforms = plan.softGlow;
    uniforms.glowWidth = contract.glowWidth;
    uniforms.glowHeight = contract.glowHeight;
    uniforms.effectiveIntensity = contract.effectiveIntensity;
    uniforms.sampleCount = contract.sampleCount;
    uniforms.sigmaX = contract.sigmaX;
    uniforms.sigmaY = contract.sigmaY;
    uniforms.stepX = contract.stepX;
    uniforms.stepY = contract.stepY;
    uniforms.thresholdLow = contract.thresholdLow;
    uniforms.thresholdHigh = contract.thresholdHigh;
    uniforms.thresholdSmooth = contract.thresholdSmooth;
    uniforms.thresholdType = contract.thresholdType;
    uniforms.grayScale = contract.grayScale;
    uniforms.shaderExposure = contract.shaderExposure;
    uniforms.displayGlow = contract.displayGlow;
    uniforms.glowColor = contract.glowColor;
    const std::array<float, 15> scalars{uniforms.effectiveIntensity,
                                        uniforms.sampleCount,
                                        uniforms.sigmaX,
                                        uniforms.sigmaY,
                                        uniforms.stepX,
                                        uniforms.stepY,
                                        uniforms.thresholdLow,
                                        uniforms.thresholdHigh,
                                        uniforms.thresholdSmooth,
                                        uniforms.thresholdType,
                                        uniforms.grayScale,
                                        uniforms.shaderExposure,
                                        uniforms.displayGlow,
                                        uniforms.glowColor[0],
                                        uniforms.glowColor[1]};
    if (!Finite(scalars) || !Finite(uniforms.glowColor)) {
      plan.unsupportedReason = "SoftGlow uniforms are non-finite";
      return plan;
    }
    if (!contract.exactPathSupported) {
      plan.unsupportedReason = contract.unsupportedReason;
      return plan;
    }
    plan.supported = true;
    AppendRawFullSizePass(plan, QtTextProPostEffectPassKind::SoftGlowThreshold);
    plan.passes.push_back({QtTextProPostEffectPassKind::SoftGlowCopy,
                           uniforms.glowWidth, uniforms.glowHeight,
                           QtTextPostEffectSurfaceSemantics::RawRgbaData});
    plan.passes.push_back({QtTextProPostEffectPassKind::SoftGlowX,
                           uniforms.glowWidth, uniforms.glowHeight,
                           QtTextPostEffectSurfaceSemantics::RawRgbaData});
    plan.passes.push_back({QtTextProPostEffectPassKind::SoftGlowY,
                           uniforms.glowWidth, uniforms.glowHeight,
                           QtTextPostEffectSurfaceSemantics::RawRgbaData});
    AppendRawFullSizePass(plan, QtTextProPostEffectPassKind::SoftGlowBlend);
    return plan;
  }

  if (kind == text::TextPostEffectKind::ChromaticAberration) {
    if (!ParametersNamed(effect, {"offsetX", "offsetY", "amount", "angle"})) {
      plan.unsupportedReason = "ChromaticAberration contains an unknown parameter";
      return plan;
    }
    plan.chromaticAberration = ResolveQtChromaticAberrationContract(
        effect, progress, plan.width, plan.height);
    if (!plan.chromaticAberration.exactPathSupported) {
      plan.unsupportedReason = plan.chromaticAberration.unsupportedReason;
      return plan;
    }
    plan.supported = true;
    AppendRawFullSizePass(plan,
                          QtTextProPostEffectPassKind::ChromaticAberration);
    return plan;
  }

  if (kind == text::TextPostEffectKind::DistortChroma) {
    plan.distortChroma = ResolveQtDistortChromaContract(
        effect, progress, plan.width, plan.height);
    if (!plan.distortChroma.exactPathSupported) {
      plan.unsupportedReason = plan.distortChroma.unsupportedReason;
      return plan;
    }
    plan.supported = true;
    constexpr std::array<QtTextProPostEffectPassKind, 5> kLensPasses{
        QtTextProPostEffectPassKind::DistortLens,
        QtTextProPostEffectPassKind::DistortBlurX1,
        QtTextProPostEffectPassKind::DistortBlurY1,
        QtTextProPostEffectPassKind::DistortBlurX2,
        QtTextProPostEffectPassKind::DistortBlurY2,
    };
    for (const auto pass : kLensPasses) {
      plan.passes.push_back(
          {pass, plan.distortChroma.lensWidth,
           plan.distortChroma.lensHeight,
           QtTextPostEffectSurfaceSemantics::RawRgbaData});
    }
    AppendRawFullSizePass(plan, QtTextProPostEffectPassKind::DistortOutput);
    return plan;
  }

  if (kind == text::TextPostEffectKind::RadialBlur) {
    plan.radialBlur =
        ResolveQtRadialBlurContract(effect, progress, plan.width, plan.height);
    if (!plan.radialBlur.exactPathSupported) {
      plan.unsupportedReason = plan.radialBlur.unsupportedReason;
      return plan;
    }
    plan.supported = true;
    AppendRawFullSizePass(plan, QtTextProPostEffectPassKind::RadialBlur);
    return plan;
  }

  if (kind == text::TextPostEffectKind::Shake) {
    plan.shake = ResolveQtShakeContract(
        effect, progress,
        stateContext ? stateContext->effectTimeSeconds : progress, plan.width,
        plan.height);
    if (!plan.shake.exactPathSupported) {
      plan.unsupportedReason = plan.shake.unsupportedReason;
      return plan;
    }
    plan.supported = true;
    AppendRawFullSizePass(plan, QtTextProPostEffectPassKind::Shake);
    return plan;
  }

  if (kind == text::TextPostEffectKind::Trail) {
    plan.trail =
        ResolveQtTrailContract(effect, progress, plan.width, plan.height);
    if (!plan.trail.exactPathSupported) {
      plan.unsupportedReason = plan.trail.unsupportedReason;
      return plan;
    }
    plan.supported = true;
    AppendRawFullSizePass(plan, QtTextProPostEffectPassKind::TrailBlurX);
    AppendRawFullSizePass(plan, QtTextProPostEffectPassKind::TrailBlurY);
    AppendRawFullSizePass(plan, QtTextProPostEffectPassKind::TrailTimeA);
    AppendRawFullSizePass(plan, QtTextProPostEffectPassKind::TrailTimeBCommit);
    AppendRawFullSizePass(plan, QtTextProPostEffectPassKind::TrailHint);
    return plan;
  }

  if (kind == text::TextPostEffectKind::SimpleChoker) {
    if (!ParametersNamed(effect,
                         {"amount", "radius", "softness", "threshold",
                          "chokeMatte", "colorTolerance", "enableColorKey",
                          "quality", "simpleColorKey", "view"}) ||
        !ScalarParameterClosed(effect, "amount") ||
        !ScalarParameterClosed(effect, "radius") ||
        !ScalarParameterClosed(effect, "softness") ||
        !ScalarParameterClosed(effect, "threshold") ||
        !ScalarParameterClosed(effect, "chokeMatte") ||
        !ScalarParameterClosed(effect, "colorTolerance") ||
        !ScalarParameterClosed(effect, "enableColorKey") ||
        !ScalarParameterClosed(effect, "quality") ||
        !Vector4ParameterClosed(effect, "simpleColorKey") ||
        !ScalarParameterClosed(effect, "view")) {
      plan.unsupportedReason = "SimpleChoker contains an unknown parameter";
      return plan;
    }
    if (!ParametersFinite(effect, progress)) {
      plan.unsupportedReason = "SimpleChoker parameter is non-finite";
      return plan;
    }
    if (FindParameter(effect, "chokeMatte")) {
      const float quality = SampleParameter(effect, "quality", progress, 0.5F);
      const float tolerance =
          SampleParameter(effect, "colorTolerance", progress, 0.1F);
      const float enableColorKey =
          SampleParameter(effect, "enableColorKey", progress, 0.0F);
      const float view = SampleParameter(effect, "view", progress, 0.0F);
      if (!InClosedRange(quality, 0.25F, 1.0F) ||
          !InClosedRange(tolerance, 0.0F, 1.0F) ||
          !InClosedRange(enableColorKey, 0.0F, 1.0F) ||
          !InClosedRange(view, 0.0F, 1.0F)) {
        plan.unsupportedReason =
            "SimpleChoker parameter is outside its executor domain";
        return plan;
      }
    }
    const float amount =
        SampleParameter(effect, "amount", progress, effect.amount);
    const float radius = SampleParameter(effect, "radius", progress, 6.0F);
    const float softness =
        SampleParameter(effect, "softness", progress, 0.02F);
    const float threshold =
        SampleParameter(effect, "threshold", progress, 0.5F);
    if (!InClosedRange(amount, -1.0F, 1.0F) ||
        !InClosedRange(radius, 0.0F, 6.0F) ||
        !InClosedRange(softness, 0.000001F, 1.0F) ||
        !InClosedRange(threshold, 0.0F, 1.0F)) {
      plan.unsupportedReason = "SimpleChoker parameter is outside its executor domain";
      return plan;
    }
    plan.supported = true;
    if (FindParameter(effect, "chokeMatte")) {
      const float quality = SampleParameter(effect, "quality", progress, 0.5F);
      const int downsampleWidth =
          std::max(1, static_cast<int>(static_cast<float>(plan.width) * quality));
      const int downsampleHeight = std::max(
          1, static_cast<int>(static_cast<float>(plan.height) * quality));
      plan.passes.push_back(
          {QtTextProPostEffectPassKind::SimpleChokerDownscale, downsampleWidth,
           downsampleHeight,
           QtTextPostEffectSurfaceSemantics::ColorPremultiplied});
      plan.passes.push_back(
          {QtTextProPostEffectPassKind::SimpleChokerMatte, downsampleWidth,
           downsampleHeight, QtTextPostEffectSurfaceSemantics::RawRgbaData});
      AppendFullSizePass(plan, QtTextProPostEffectPassKind::SimpleChokerBlend);
    } else {
      AppendFullSizePass(plan, QtTextProPostEffectPassKind::SimpleChoker);
    }
    return plan;
  }

  if (kind == text::TextPostEffectKind::CCLens) {
    if (!ParametersNamed(effect,
                         {"size", "radius", "convergence", "distortion", "fieldOfView",
                          "invert", "orientation", "center"}) ||
        !ScalarParameterClosed(effect, "size") ||
        !ScalarParameterClosed(effect, "radius") ||
        !ScalarParameterClosed(effect, "convergence") ||
        !ScalarParameterClosed(effect, "distortion") ||
        !ScalarParameterClosed(effect, "fieldOfView") ||
        !ScalarParameterClosed(effect, "invert") ||
        !ScalarParameterClosed(effect, "orientation") ||
        !Vector2ParameterClosed(effect, "center")) {
      plan.unsupportedReason = "CCLens contains an unknown parameter";
      return plan;
    }
    const auto center = Vector2Parameter(effect, "center", {0.5F, 0.5F});
    const float size = SampleParameter(
        effect, FindParameter(effect, "radius") ? "radius" : "size",
        progress, 1.0F);
    const float convergence =
        SampleParameter(effect, "convergence", progress, 0.0F);
    const float distortion = SampleParameter(
        effect, "distortion", progress,
        FindParameter(effect, "convergence") ? 0.0F : effect.amount);
    const float fieldOfView =
        SampleParameter(effect, "fieldOfView", progress, 0.0F);
    const float invert = SampleParameter(effect, "invert", progress, 0.0F);
    const float orientation =
        SampleParameter(effect, "orientation", progress, 0.0F);
    if (!Finite(center) || !Finite(size) || size <= 0.0F ||
        !Finite(convergence) || !Finite(distortion) ||
        !InClosedRange(fieldOfView, -179.0F, 179.0F) ||
        !InClosedRange(invert, 0.0F, 1.0F) || !Finite(orientation)) {
      plan.unsupportedReason = "CCLens parameter is outside its executor domain";
      return plan;
    }
    plan.supported = true;
    AppendFullSizePass(plan, QtTextProPostEffectPassKind::CCLens);
    return plan;
  }

  if (kind == text::TextPostEffectKind::OpticsCompensation) {
    if (!ParametersNamed(effect,
                         {"fieldOfView", "reverseLensDistortion", "viewMode",
                          "optimalPixels", "resize", "center",
                          "antiAliasing", "fillBorders", "fov",
                          "fovOrientation", "inverseLensDistortion"}) ||
        !ScalarParameterClosed(effect, "fieldOfView") ||
        !ScalarParameterClosed(effect, "reverseLensDistortion") ||
        !ScalarParameterClosed(effect, "viewMode") ||
        !ScalarParameterClosed(effect, "optimalPixels") ||
        !ScalarParameterClosed(effect, "resize") ||
        !ScalarParameterClosed(effect, "antiAliasing") ||
        !ScalarParameterClosed(effect, "fillBorders") ||
        !ScalarParameterClosed(effect, "fov") ||
        !ScalarParameterClosed(effect, "fovOrientation") ||
        !ScalarParameterClosed(effect, "inverseLensDistortion") ||
        !Vector2ParameterClosed(effect, "center")) {
      plan.unsupportedReason =
          "OpticsCompensation contains an unknown parameter";
      return plan;
    }
    const auto center = Vector2Parameter(effect, "center", {0.5F, 0.5F});
    const float fieldOfView = SampleParameter(
        effect, FindParameter(effect, "fov") ? "fov" : "fieldOfView",
        progress, effect.amount);
    const float reverse = SampleParameter(
        effect, "reverseLensDistortion", progress, 1.0F);
    const float viewMode =
        SampleParameter(effect, "viewMode", progress, 0.0F);
    const float optimal =
        SampleParameter(effect, "optimalPixels", progress, 0.0F);
    const float resize = SampleParameter(effect, "resize", progress, 0.0F);
    if (!Finite(center) ||
        !InClosedRange(fieldOfView, -179.0F, 180.0F) ||
        !InClosedRange(reverse, 0.0F, 1.0F) ||
        !InClosedRange(viewMode, 0.0F, 1.0F) ||
        !InClosedRange(optimal, 0.0F, 1.0F) ||
        !InClosedRange(resize, 0.0F, 1.0F)) {
      plan.unsupportedReason =
          "OpticsCompensation parameter is outside its executor domain";
      return plan;
    }
    plan.supported = true;
    if (FindParameter(effect, "fov")) {
      AppendFullSizePass(
          plan, QtTextProPostEffectPassKind::OpticsCompensationDistort);
      if (SampleParameter(effect, "antiAliasing", progress, 1.0F) >= 0.5F) {
        AppendFullSizePass(
            plan,
            QtTextProPostEffectPassKind::OpticsCompensationAntiAliasing);
      }
    } else {
      AppendFullSizePass(plan,
                         QtTextProPostEffectPassKind::OpticsCompensation);
    }
    return plan;
  }

  if (kind == text::TextPostEffectKind::Projection) {
    if (!ParametersNamed(effect,
                         {"rotationX", "rotationY", "rotationZ",
                          "perspective", "fieldOfView", "focalLength",
                          "scale", "center", "vanishingPoint", "backAlpha",
                          "mixWithBlack", "offsetY_0", "shakeScale",
                          "shakeTime"}) ||
        !ScalarParameterClosed(effect, "rotationX") ||
        !ScalarParameterClosed(effect, "rotationY") ||
        !ScalarParameterClosed(effect, "rotationZ") ||
        !ScalarParameterClosed(effect, "perspective") ||
        !ScalarParameterClosed(effect, "fieldOfView") ||
        !ScalarParameterClosed(effect, "focalLength") ||
        !ScalarParameterClosed(effect, "backAlpha") ||
        !ScalarParameterClosed(effect, "mixWithBlack") ||
        !ScalarParameterClosed(effect, "offsetY_0") ||
        !ScalarParameterClosed(effect, "shakeScale") ||
        !ScalarParameterClosed(effect, "shakeTime") ||
        !Vector2ParameterClosed(effect, "scale") ||
        !Vector2ParameterClosed(effect, "center") ||
        !Vector2ParameterClosed(effect, "vanishingPoint")) {
      plan.unsupportedReason = "Projection contains an unknown parameter";
      return plan;
    }
    const auto center = Vector2Parameter(effect, "center", {0.5F, 0.5F});
    auto scale = Vector2Parameter(effect, "scale", {1.0F, 1.0F});
    if (const auto *parameter = FindParameter(effect, "scale");
        parameter && parameter->values.size() == 1U) {
      scale[1] = scale[0];
    }
    const auto vanishing =
        Vector2Parameter(effect, "vanishingPoint", {0.5F, 0.5F});
    const float rotationX =
        SampleParameter(effect, "rotationX", progress, 0.0F);
    const float rotationY =
        SampleParameter(effect, "rotationY", progress, 0.0F);
    const float rotationZ =
        SampleParameter(effect, "rotationZ", progress, 0.0F);
    const float perspective =
        SampleParameter(effect, "perspective", progress, effect.amount);
    const float fieldOfView =
        SampleParameter(effect, "fieldOfView", progress, 0.0F);
    const float focalLength =
        SampleParameter(effect, "focalLength", progress, 1.0F);
    if (!Finite(center) || !Finite(scale) || !Finite(vanishing) ||
        !InClosedRange(rotationX, -87.0F, 87.0F) ||
        !InClosedRange(rotationY, -87.0F, 87.0F) || !Finite(rotationZ) ||
        !InClosedRange(perspective, -0.95F, 0.95F) ||
        !InClosedRange(fieldOfView, -87.0F, 87.0F) ||
        !Finite(focalLength) || focalLength <= 0.0F || scale[0] <= 0.0F ||
        scale[1] <= 0.0F) {
      plan.unsupportedReason = "Projection parameter is outside its executor domain";
      return plan;
    }
    plan.supported = true;
    AppendFullSizePass(plan, QtTextProPostEffectPassKind::Projection);
    return plan;
  }

  if (kind == text::TextPostEffectKind::DynamicSignalGlitch) {
    if (!ParametersNamed(effect,
                         {"amount", "blockSize", "chromaShift", "scanline",
                          "seed", "frequency", "phase", "speed",
                          "intensity", "initialGlitchStrength",
                          "initialFlashFrequency", "initialColorOffset",
                          "initialDistortionAmount"}) ||
        !ScalarParameterClosed(effect, "amount") ||
        !ScalarParameterClosed(effect, "blockSize") ||
        !ScalarParameterClosed(effect, "chromaShift") ||
        !ScalarParameterClosed(effect, "scanline") ||
        !ScalarParameterClosed(effect, "seed") ||
        !ScalarParameterClosed(effect, "frequency") ||
        !ScalarParameterClosed(effect, "phase") ||
        !ScalarParameterClosed(effect, "speed") ||
        !ScalarParameterClosed(effect, "intensity") ||
        !ScalarParameterClosed(effect, "initialGlitchStrength") ||
        !ScalarParameterClosed(effect, "initialFlashFrequency") ||
        !ScalarParameterClosed(effect, "initialColorOffset") ||
        !ScalarParameterClosed(effect, "initialDistortionAmount")) {
      plan.unsupportedReason =
          "DynamicSignalGlitch contains an unknown parameter";
      return plan;
    }
    const float amount =
        SampleParameter(effect, "amount", progress, effect.amount);
    const float blockSize =
        SampleParameter(effect, "blockSize", progress, 8.0F);
    const float chroma =
        SampleParameter(effect, "chromaShift", progress, 0.0F);
    const float scanline =
        SampleParameter(effect, "scanline", progress, 0.0F);
    const float seed = SampleParameter(effect, "seed", progress, 0.0F);
    const float frequency =
        SampleParameter(effect, "frequency", progress, 12.0F);
    const float phase = SampleParameter(effect, "phase", progress, 0.0F);
    const float speed = SampleParameter(effect, "speed", progress, 1.0F);
    const float intensity =
        SampleParameter(effect, "intensity", progress, 1.0F);
    const float initialGlitchStrength =
        SampleParameter(effect, "initialGlitchStrength", progress, 0.5F);
    const float initialFlashFrequency =
        SampleParameter(effect, "initialFlashFrequency", progress, 10.0F);
    const float initialColorOffset =
        SampleParameter(effect, "initialColorOffset", progress, 10.0F);
    const float initialDistortionAmount =
        SampleParameter(effect, "initialDistortionAmount", progress, 5.0F);
    if (!Finite(amount) || !Finite(blockSize) || blockSize <= 0.0F ||
        !Finite(chroma) || !InClosedRange(scanline, 0.0F, 1.0F) ||
        !Finite(seed) || !Finite(frequency) || !Finite(phase) ||
        !Finite(speed) || !InClosedRange(intensity, 0.0F, 1.0F) ||
        !InClosedRange(initialGlitchStrength, 0.0F, 1.0F) ||
        !InClosedRange(initialFlashFrequency, 0.0F, 20.0F) ||
        !InClosedRange(initialColorOffset, 0.0F, 50.0F) ||
        !InClosedRange(initialDistortionAmount, 0.0F, 20.0F)) {
      plan.unsupportedReason =
          "DynamicSignalGlitch parameter is outside its executor domain";
      return plan;
    }
    plan.supported = true;
    AppendFullSizePass(plan,
                       QtTextProPostEffectPassKind::DynamicSignalGlitch);
    return plan;
  }

  if (kind == text::TextPostEffectKind::ElectricPulseMotionBlur) {
    if (!ParametersNamed(effect,
                         {"amount", "angle", "radius", "samples", "pulse",
                          "phase", "exposure", "speed", "intensity",
                          "maxBlurStrength", "minBlurStrength",
                          "syncCycle"}) ||
        !ScalarParameterClosed(effect, "amount") ||
        !ScalarParameterClosed(effect, "angle") ||
        !ScalarParameterClosed(effect, "radius") ||
        !ScalarParameterClosed(effect, "samples") ||
        !ScalarParameterClosed(effect, "pulse") ||
        !ScalarParameterClosed(effect, "phase") ||
        !ScalarParameterClosed(effect, "exposure") ||
        !ScalarParameterClosed(effect, "speed") ||
        !ScalarParameterClosed(effect, "intensity") ||
        !ScalarParameterClosed(effect, "maxBlurStrength") ||
        !ScalarParameterClosed(effect, "minBlurStrength") ||
        !ScalarParameterClosed(effect, "syncCycle")) {
      plan.unsupportedReason =
          "ElectricPulseMotionBlur contains an unknown parameter";
      return plan;
    }
    const float amount =
        SampleParameter(effect, "amount", progress,
                        FindParameter(effect, "radius") ? 1.0F
                                                         : effect.amount);
    const float angle = SampleParameter(effect, "angle", progress, 0.0F);
    const float radius = SampleParameter(effect, "radius", progress, 16.0F);
    const float samples =
        SampleParameter(effect, "samples", progress, 8.0F);
    const float pulse = SampleParameter(effect, "pulse", progress, 0.5F);
    const float phase = SampleParameter(effect, "phase", progress, 0.0F);
    const float exposure =
        SampleParameter(effect, "exposure", progress, 1.0F);
    const float speed = SampleParameter(effect, "speed", progress, 1.0F);
    const float intensity =
        SampleParameter(effect, "intensity", progress, 1.0F);
    const float maximumBlur =
        SampleParameter(effect, "maxBlurStrength", progress, 0.5F);
    const float minimumBlur =
        SampleParameter(effect, "minBlurStrength", progress, 0.0F);
    const float syncCycle =
        SampleParameter(effect, "syncCycle", progress, 3.0F);
    if (!Finite(amount) || amount < 0.0F || !Finite(angle) ||
        !Finite(radius) || radius < 0.0F ||
        !InClosedRange(samples, 1.0F, 16.0F) ||
        !InClosedRange(pulse, 0.0F, 1.0F) || !Finite(phase) ||
        !Finite(exposure) || exposure < 0.0F || !Finite(speed) ||
        !InClosedRange(intensity, 0.0F, 1.0F) ||
        !InClosedRange(maximumBlur, 0.0F, 1.0F) ||
        !InClosedRange(minimumBlur, 0.0F, 1.0F) ||
        !InClosedRange(syncCycle, 0.1F, 5.0F)) {
      plan.unsupportedReason =
          "ElectricPulseMotionBlur parameter is outside its executor domain";
      return plan;
    }
    plan.supported = true;
    AppendFullSizePass(
        plan, QtTextProPostEffectPassKind::ElectricPulseMotionBlur);
    return plan;
  }

  if (kind == text::TextPostEffectKind::PulseEnvelope) {
    if (!ParametersNamed(effect,
                         {"intensity", "maxBlurStrength",
                          "minBlurStrength", "syncCycle"}) ||
        !ScalarParameterClosed(effect, "intensity") ||
        !ScalarParameterClosed(effect, "maxBlurStrength") ||
        !ScalarParameterClosed(effect, "minBlurStrength") ||
        !ScalarParameterClosed(effect, "syncCycle")) {
      plan.unsupportedReason =
          "PulseEnvelope contains an unknown parameter";
      return plan;
    }
    const float intensity =
        SampleParameter(effect, "intensity", progress, effect.amount);
    const float maximumBlur =
        SampleParameter(effect, "maxBlurStrength", progress, 0.5F);
    const float minimumBlur =
        SampleParameter(effect, "minBlurStrength", progress, 0.0F);
    const float syncCycle =
        SampleParameter(effect, "syncCycle", progress, 3.0F);
    if (!InClosedRange(intensity, 0.0F, 1.0F) ||
        !InClosedRange(maximumBlur, 0.0F, 1.0F) ||
        !InClosedRange(minimumBlur, 0.0F, 1.0F) ||
        maximumBlur < minimumBlur ||
        !InClosedRange(syncCycle, 0.1F, 5.0F)) {
      plan.unsupportedReason =
          "PulseEnvelope parameter is outside its executor domain";
      return plan;
    }
    plan.supported = true;
    AppendFullSizePass(plan, QtTextProPostEffectPassKind::PulseEnvelope);
    return plan;
  }

  if (kind >= text::TextPostEffectKind::MorphologicalOutline &&
      kind <= text::TextPostEffectKind::PerspectiveEchoTrail) {
    bool closed = ParametersFinite(effect, progress);
    QtTextProPostEffectPassKind passKind =
        QtTextProPostEffectPassKind::MorphologicalOutline;
    const char *kindName = "MorphologicalOutline";
    switch (kind) {
    case text::TextPostEffectKind::MorphologicalOutline:
      closed = closed &&
               ParametersNamed(effect,
                               {"intensity", "ratio", "outlineColor",
                                "size", "scaleX", "scaleY", "offsetX",
                                "offsetY"}) &&
               ScalarParameterClosed(effect, "intensity") &&
               ScalarParameterClosed(effect, "ratio") &&
               Vector4ParameterClosed(effect, "outlineColor") &&
               ScalarParameterClosed(effect, "size") &&
               ScalarParameterClosed(effect, "scaleX") &&
               ScalarParameterClosed(effect, "scaleY") &&
               ScalarParameterClosed(effect, "offsetX") &&
               ScalarParameterClosed(effect, "offsetY");
      break;
    case text::TextPostEffectKind::AlternatingSegmentMask:
      kindName = "AlternatingSegmentMask";
      passKind = QtTextProPostEffectPassKind::AlternatingSegmentMask;
      closed = closed &&
               ParametersNamed(effect,
                               {"intensity", "segmentWidth",
                                "rotationAngle", "displacement",
                                "segmentColor"}) &&
               ScalarParameterClosed(effect, "intensity") &&
               ScalarParameterClosed(effect, "segmentWidth") &&
               ScalarParameterClosed(effect, "rotationAngle") &&
               Vector2Or3ParameterClosed(effect, "displacement") &&
               Vector4ParameterClosed(effect, "segmentColor");
      break;
    case text::TextPostEffectKind::CenterSplitDisplacement:
      kindName = "CenterSplitDisplacement";
      passKind = QtTextProPostEffectPassKind::CenterSplitDisplacement;
      closed = closed &&
               ParametersNamed(effect,
                               {"intensity", "ratio", "pointA", "pointB",
                                "split"}) &&
               ScalarParameterClosed(effect, "intensity") &&
               ScalarParameterClosed(effect, "ratio") &&
               ScalarParameterClosed(effect, "pointA") &&
               ScalarParameterClosed(effect, "pointB") &&
               ScalarParameterClosed(effect, "split");
      break;
    case text::TextPostEffectKind::CutAndDrop:
      kindName = "CutAndDrop";
      passKind = QtTextProPostEffectPassKind::CutAndDrop;
      closed = closed &&
               ParametersNamed(effect,
                               {"intensity", "ratio", "knifeProgress",
                                "dropStrand", "slant"}) &&
               ScalarParameterClosed(effect, "intensity") &&
               ScalarParameterClosed(effect, "ratio") &&
               ScalarParameterClosed(effect, "knifeProgress") &&
               ScalarParameterClosed(effect, "dropStrand") &&
               ScalarParameterClosed(effect, "slant");
      break;
    case text::TextPostEffectKind::BlockGlitchDotMatrix:
      kindName = "BlockGlitchDotMatrix";
      passKind = QtTextProPostEffectPassKind::BlockGlitchDotMatrix;
      closed = closed &&
               ParametersNamed(effect,
                               {"intensity", "ratio", "glitchAmount",
                                "blockScale", "jitterSpeed"}) &&
               ScalarParameterClosed(effect, "intensity") &&
               ScalarParameterClosed(effect, "ratio") &&
               ScalarParameterClosed(effect, "glitchAmount") &&
               ScalarParameterClosed(effect, "blockScale") &&
               ScalarParameterClosed(effect, "jitterSpeed");
      break;
    case text::TextPostEffectKind::ProceduralFlameOutline:
      kindName = "ProceduralFlameOutline";
      passKind = QtTextProPostEffectPassKind::ProceduralFlameOutline;
      closed = closed &&
               ParametersNamed(effect,
                               {"intensity", "ratio", "loopTime",
                                "flameColor", "flameSpeed", "noiseScale"}) &&
               ScalarParameterClosed(effect, "intensity") &&
               ScalarParameterClosed(effect, "ratio") &&
               ScalarParameterClosed(effect, "loopTime") &&
               Vector4ParameterClosed(effect, "flameColor") &&
               ScalarParameterClosed(effect, "flameSpeed") &&
               ScalarParameterClosed(effect, "noiseScale");
      break;
    case text::TextPostEffectKind::CylindricalScroll:
      kindName = "CylindricalScroll";
      passKind = QtTextProPostEffectPassKind::CylindricalScroll;
      closed = closed &&
               ParametersNamed(effect,
                               {"intensity", "ratio", "scrollProgress",
                                "itemCount", "tunnelRadius", "fov",
                                "pauseStrength"}) &&
               ScalarParameterClosed(effect, "intensity") &&
               ScalarParameterClosed(effect, "ratio") &&
               ScalarParameterClosed(effect, "scrollProgress") &&
               ScalarParameterClosed(effect, "itemCount") &&
               ScalarParameterClosed(effect, "tunnelRadius") &&
               ScalarParameterClosed(effect, "fov") &&
               ScalarParameterClosed(effect, "pauseStrength");
      break;
    case text::TextPostEffectKind::SplitSqueezeWave:
      kindName = "SplitSqueezeWave";
      passKind = QtTextProPostEffectPassKind::SplitSqueezeWave;
      closed = closed &&
               ParametersNamed(effect,
                               {"intensity", "ratio", "waveHeight",
                                "waveFreq"}) &&
               ScalarParameterClosed(effect, "intensity") &&
               ScalarParameterClosed(effect, "ratio") &&
               ScalarParameterClosed(effect, "waveHeight") &&
               ScalarParameterClosed(effect, "waveFreq");
      break;
    case text::TextPostEffectKind::PerspectiveEchoTrail:
      kindName = "PerspectiveEchoTrail";
      passKind = QtTextProPostEffectPassKind::PerspectiveEchoTrail;
      closed = closed &&
               ParametersNamed(effect,
                               {"intensity", "ratio", "echoCount",
                                "echoSpacing", "perspective",
                                "curveTension", "outlineColor"}) &&
               ScalarParameterClosed(effect, "intensity") &&
               ScalarParameterClosed(effect, "ratio") &&
               ScalarParameterClosed(effect, "echoCount") &&
               ScalarParameterClosed(effect, "echoSpacing") &&
               ScalarParameterClosed(effect, "perspective") &&
               ScalarParameterClosed(effect, "curveTension") &&
               Vector4ParameterClosed(effect, "outlineColor");
      break;
    default:
      closed = false;
      break;
    }
    if (!closed) {
      plan.unsupportedReason =
          std::string(kindName) + " contains an unknown or invalid parameter";
      return plan;
    }
    const float intensity =
        SampleParameter(effect, "intensity", progress, effect.amount);
    const float ratio = SampleParameter(
        effect, "ratio", progress,
        static_cast<float>(plan.width) / static_cast<float>(plan.height));
    if (!InClosedRange(intensity, 0.0F, 1.0F) || !Finite(ratio) ||
        ratio <= 0.0F) {
      plan.unsupportedReason =
          std::string(kindName) + " parameter is outside its executor domain";
      return plan;
    }
    if (kind == text::TextPostEffectKind::AlternatingSegmentMask &&
        SampleParameter(effect, "segmentWidth", progress, 0.02F) <= 0.0F) {
      plan.unsupportedReason =
          "AlternatingSegmentMask segmentWidth must be positive";
      return plan;
    }
    if (kind == text::TextPostEffectKind::ProceduralFlameOutline &&
        SampleParameter(effect, "noiseScale", progress, 5.0F) <= 0.0F) {
      plan.unsupportedReason =
          "ProceduralFlameOutline noiseScale must be positive";
      return plan;
    }
    if (kind == text::TextPostEffectKind::CylindricalScroll &&
        (SampleParameter(effect, "itemCount", progress, 5.0F) <= 0.0F ||
         SampleParameter(effect, "tunnelRadius", progress, 1.0F) <= 0.0F ||
         SampleParameter(effect, "fov", progress, 0.5F) <= 0.0F)) {
      plan.unsupportedReason =
          "CylindricalScroll geometry parameters must be positive";
      return plan;
    }
    if (kind == text::TextPostEffectKind::PerspectiveEchoTrail &&
        SampleParameter(effect, "curveTension", progress, 3.0F) <= 0.0F) {
      plan.unsupportedReason =
          "PerspectiveEchoTrail curveTension must be positive";
      return plan;
    }
    plan.supported = true;
    AppendFullSizePass(plan, passKind);
    return plan;
  }

  plan.unsupportedReason = "post-effect kind is outside the TextPro helper";
  return plan;
}

QtTextProPostEffectResult ExecuteQtTextProPostEffectPipeline(
    const sk_sp<SkPicture> &source, const text::TextEffectPostEffectNode &effect,
    const double progress, const SkRect &recordingBounds,
    SkiaGpuContext *gpuContext,
    const QtTextPostEffectStateContext *stateContext,
    const bool collectParameterEvidence) {
  QtTextProPostEffectResult result;
  result.plan = ResolveQtTextProPostEffectPlan(effect, progress,
                                               recordingBounds, stateContext);
  if (!source) {
    result.error = "source picture is null";
    return result;
  }
  if (!result.plan.supported) {
    result.error = result.plan.unsupportedReason.empty()
                       ? "post-effect plan is unsupported"
                       : result.plan.unsupportedReason;
    return result;
  }
  TextPostEffectParameterConsumptionScope consumption(effect,
                                                     collectParameterEvidence);
  result.picture = ApplyExactQtTextPostEffectPicture(
      source, effect, progress, recordingBounds, gpuContext,
      &result.execution, stateContext);
  if (!result.picture) {
    result.error = "RGBA8 TextPro pass materialization failed";
    consumption.Finish(result.parameters, false);
    return result;
  }
  result.error = ValidateExecutionTrace(result.plan, result.execution);
  if (!result.error.empty())
    result.picture.reset();
  consumption.Finish(result.parameters, static_cast<bool>(result));
  return result;
}

sk_sp<SkPicture> ApplyQtTextProPostEffectPipelinePicture(
    const sk_sp<SkPicture> &source, const text::TextEffectPostEffectNode &effect,
    const double progress, const SkRect &recordingBounds,
    SkiaGpuContext *gpuContext,
    const QtTextPostEffectStateContext *stateContext) {
  auto result = ExecuteQtTextProPostEffectPipeline(
      source, effect, progress, recordingBounds, gpuContext, stateContext);
  if (std::getenv("VIDEOCUT_TRACE_TEXT_QT_POSTFX") != nullptr) {
    std::fprintf(
        stderr,
        "[VIDEOCUT_TEXT_QT_POSTFX_GRAPH] kind=%u page=%dx%d planned=%zu "
        "executed=%zu "
        "status=%s%s%s\n",
        static_cast<unsigned>(effect.kind), result.plan.width,
        result.plan.height,
        result.plan.passes.size(), result.execution.passes.size(),
        result ? "executed" : "fallback",
        result.error.empty() ? "" : " reason=", result.error.c_str());
  }
  return std::move(result.picture);
}

} // namespace videocut::skia_runtime::internal
