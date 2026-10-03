#pragma once

#include "internal/skia/SkiaHeaders.h"

#include <string>

namespace videocut::skia_runtime::internal {

// Immutable product-owned SkSL programs. Uniform and render-target bindings
// stay with the execution pipeline that owns their coordinate domain.
enum class TextRuntimeShader {
  MaterialModulate,
  MaterialParticleCircle,
  MaterialGaussian,
  MaterialTransition,
  MaterialDirectionalBoxBlur,
  MaterialNoiseDissolve,
  MaterialBallistic,
  MaterialAxisBlur,
  MaterialSpiralWarp,
  MaterialFlux,
  MaterialGlow,
  MaterialMultimesh,
  MaterialLineColumn,
  MaterialClosed,
  MaterialMaskedCutLine,
  MaterialCutLineReveal,
  MaterialPackedGaussian,
  MaterialDualPackedGlow,
  MaterialEncodedAlphaBlur,
  MaterialSdfNormalSweep,
  MaterialLightSweepGlow,
  MaterialRadialDecayHsv,
  MaterialFixedNineTap,
  MaterialHsvOriginalOver,
  PostRadianceThreshold,
  PostRadianceErode,
  PostLinearBlit,
  PostRadianceDirectionalBlur,
  PostRadianceBlend,
  PostSoftGlowBlend,
  PostGaussianBlurSeparable,
  PostDeepGlowPreprocess,
  PostDeepGlowDownscale,
  PostDeepGlowBlur,
  PostDeepGlowComposite,
  PostDeepGlowPostprocess,
  PostWaveWarp,
  PostSGlowThreshold,
  PostSGlowGaussian,
  PostSGlowComposite,
  PostChromaticAberration,
  PostMultiShadow,
  PostSoftGlowSeparable,
  PostAlphaOutline,
  PostTurbulenceNoise,
  PostTurbulenceDisplacement,
  PostGodRayThreshold,
  PostGodRayGaussian,
  PostGodRayComposite,
  PostLinearWipe,
  PostSimpleChokerDownscale,
  PostSimpleChokerMatte,
  PostSimpleChokerBlend,
  PostOpticsCompensationDistort,
  PostOpticsCompensationAntiAliasing,
  PostProjectionCopyScale,
  PostSemanticPostEffect,
  LinearGradient,
  RadialGradient,
  MaterialMaskMotion,
  MaterialFaceNoise,
  MaterialDualOffset,
  MaterialShapedRamp,
  MaterialWave,
  MaterialAlphaOutline,
  MaterialDeepGlowScreen,
  MaterialProjectionComposite,
  MaterialParticleScatter,
  MaterialCylinder,
  MaterialUnitedShadow,
  MaterialFlowWarp,
  Count
};

struct TextRuntimeProgram final {
  sk_sp<SkRuntimeEffect> effect;
  std::string error;
};

// Lazy, thread-safe per-program cache, including compile failures.
const TextRuntimeProgram &GetTextRuntimeProgram(TextRuntimeShader shader);

} // namespace videocut::skia_runtime::internal
