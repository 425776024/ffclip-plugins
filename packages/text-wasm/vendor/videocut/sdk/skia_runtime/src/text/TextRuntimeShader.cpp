#include "text/TextRuntimeShader.h"

#include "text/shaders/LinearGradient.h"
#include "text/shaders/MaterialAxisBlur.h"
#include "text/shaders/MaterialAlphaOutline.h"
#include "text/shaders/MaterialFlowWarp.h"
#include "text/shaders/MaterialBallistic.h"
#include "text/shaders/MaterialClosed.h"
#include "text/shaders/MaterialDeepGlowScreen.h"
#include "text/shaders/MaterialProjectionComposite.h"
#include "text/shaders/MaterialParticleScatter.h"
#include "text/shaders/MaterialCylinder.h"
#include "text/shaders/MaterialUnitedShadow.h"
#include "text/shaders/MaterialCutLineReveal.h"
#include "text/shaders/MaterialDirectionalBoxBlur.h"
#include "text/shaders/MaterialDualOffset.h"
#include "text/shaders/MaterialDualPackedGlow.h"
#include "text/shaders/MaterialEncodedAlphaBlur.h"
#include "text/shaders/MaterialFaceNoise.h"
#include "text/shaders/MaterialFixedNineTap.h"
#include "text/shaders/MaterialFlux.h"
#include "text/shaders/MaterialGaussian.h"
#include "text/shaders/MaterialGlow.h"
#include "text/shaders/MaterialHsvOriginalOver.h"
#include "text/shaders/MaterialLightSweepGlow.h"
#include "text/shaders/MaterialLineColumn.h"
#include "text/shaders/MaterialMaskMotion.h"
#include "text/shaders/MaterialMaskedCutLine.h"
#include "text/shaders/MaterialModulate.h"
#include "text/shaders/MaterialMultimesh.h"
#include "text/shaders/MaterialNoiseDissolve.h"
#include "text/shaders/MaterialPackedGaussian.h"
#include "text/shaders/MaterialParticleCircle.h"
#include "text/shaders/MaterialRadialDecayHsv.h"
#include "text/shaders/MaterialSdfNormalSweep.h"
#include "text/shaders/MaterialShapedRamp.h"
#include "text/shaders/MaterialSpiralWarp.h"
#include "text/shaders/MaterialTransition.h"
#include "text/shaders/MaterialWave.h"
#include "text/shaders/PostAlphaOutline.h"
#include "text/shaders/PostChromaticAberration.h"
#include "text/shaders/PostDeepGlowBlur.h"
#include "text/shaders/PostDeepGlowComposite.h"
#include "text/shaders/PostDeepGlowDownscale.h"
#include "text/shaders/PostDeepGlowPostprocess.h"
#include "text/shaders/PostDeepGlowPreprocess.h"
#include "text/shaders/PostGaussianBlurSeparable.h"
#include "text/shaders/PostGodRayComposite.h"
#include "text/shaders/PostGodRayGaussian.h"
#include "text/shaders/PostGodRayThreshold.h"
#include "text/shaders/PostLinearBlit.h"
#include "text/shaders/PostLinearWipe.h"
#include "text/shaders/PostMultiShadow.h"
#include "text/shaders/PostOpticsCompensationAntiAliasing.h"
#include "text/shaders/PostOpticsCompensationDistort.h"
#include "text/shaders/PostProjectionCopyScale.h"
#include "text/shaders/PostRadianceBlend.h"
#include "text/shaders/PostRadianceDirectionalBlur.h"
#include "text/shaders/PostRadianceThreshold.h"
#include "text/shaders/PostRadianceErode.h"
#include "text/shaders/PostSGlowComposite.h"
#include "text/shaders/PostSGlowGaussian.h"
#include "text/shaders/PostSGlowThreshold.h"
#include "text/shaders/PostSemanticPostEffect.h"
#include "text/shaders/PostSimpleChokerBlend.h"
#include "text/shaders/PostSimpleChokerDownscale.h"
#include "text/shaders/PostSimpleChokerMatte.h"
#include "text/shaders/PostSoftGlowBlend.h"
#include "text/shaders/PostSoftGlowSeparable.h"
#include "text/shaders/PostTurbulenceDisplacement.h"
#include "text/shaders/PostTurbulenceNoise.h"
#include "text/shaders/PostWaveWarp.h"
#include "text/shaders/RadialGradient.h"

#include <array>
#include <cstddef>
#include <mutex>
#include <utility>

namespace videocut::skia_runtime::internal {
namespace {

constexpr auto kSources = std::to_array<const char *>({
    shader_sources::MaterialModulate,
    shader_sources::MaterialParticleCircle,
    shader_sources::MaterialGaussian,
    shader_sources::MaterialTransition,
    shader_sources::MaterialDirectionalBoxBlur,
    shader_sources::MaterialNoiseDissolve,
    shader_sources::MaterialBallistic,
    shader_sources::MaterialAxisBlur,
    shader_sources::MaterialSpiralWarp,
    shader_sources::MaterialFlux,
    shader_sources::MaterialGlow,
    shader_sources::MaterialMultimesh,
    shader_sources::MaterialLineColumn,
    shader_sources::MaterialClosed,
    shader_sources::MaterialMaskedCutLine,
    shader_sources::MaterialCutLineReveal,
    shader_sources::MaterialPackedGaussian,
    shader_sources::MaterialDualPackedGlow,
    shader_sources::MaterialEncodedAlphaBlur,
    shader_sources::MaterialSdfNormalSweep,
    shader_sources::MaterialLightSweepGlow,
    shader_sources::MaterialRadialDecayHsv,
    shader_sources::MaterialFixedNineTap,
    shader_sources::MaterialHsvOriginalOver,
    shader_sources::PostRadianceThreshold,
    shader_sources::PostRadianceErode,
    shader_sources::PostLinearBlit,
    shader_sources::PostRadianceDirectionalBlur,
    shader_sources::PostRadianceBlend,
    shader_sources::PostSoftGlowBlend,
    shader_sources::PostGaussianBlurSeparable,
    shader_sources::PostDeepGlowPreprocess,
    shader_sources::PostDeepGlowDownscale,
    shader_sources::PostDeepGlowBlur,
    shader_sources::PostDeepGlowComposite,
    shader_sources::PostDeepGlowPostprocess,
    shader_sources::PostWaveWarp,
    shader_sources::PostSGlowThreshold,
    shader_sources::PostSGlowGaussian,
    shader_sources::PostSGlowComposite,
    shader_sources::PostChromaticAberration,
    shader_sources::PostMultiShadow,
    shader_sources::PostSoftGlowSeparable,
    shader_sources::PostAlphaOutline,
    shader_sources::PostTurbulenceNoise,
    shader_sources::PostTurbulenceDisplacement,
    shader_sources::PostGodRayThreshold,
    shader_sources::PostGodRayGaussian,
    shader_sources::PostGodRayComposite,
    shader_sources::PostLinearWipe,
    shader_sources::PostSimpleChokerDownscale,
    shader_sources::PostSimpleChokerMatte,
    shader_sources::PostSimpleChokerBlend,
    shader_sources::PostOpticsCompensationDistort,
    shader_sources::PostOpticsCompensationAntiAliasing,
    shader_sources::PostProjectionCopyScale,
    shader_sources::PostSemanticPostEffect,
    shader_sources::LinearGradient,
    shader_sources::RadialGradient,
    shader_sources::MaterialMaskMotion,
    shader_sources::MaterialFaceNoise,
    shader_sources::MaterialDualOffset,
    shader_sources::MaterialShapedRamp,
    shader_sources::MaterialWave,
    shader_sources::MaterialAlphaOutline,
    shader_sources::MaterialDeepGlowScreen,
    shader_sources::MaterialProjectionComposite,
    shader_sources::MaterialParticleScatter,
    shader_sources::MaterialCylinder,
    shader_sources::MaterialUnitedShadow,
    shader_sources::MaterialFlowWarp,
});
static_assert(kSources.size() ==
              static_cast<std::size_t>(TextRuntimeShader::Count));

struct CachedProgram final {
  std::once_flag initialized;
  TextRuntimeProgram program;
};

} // namespace

const TextRuntimeProgram &
GetTextRuntimeProgram(const TextRuntimeShader shader) {
  const auto index = static_cast<std::size_t>(shader);
  if (index >= kSources.size()) {
    static const TextRuntimeProgram invalid{{}, "invalid text runtime shader"};
    return invalid;
  }
  static std::array<CachedProgram, kSources.size()> programs;
  auto &entry = programs[index];
  std::call_once(entry.initialized, [&entry, index] {
    auto compiled = SkRuntimeEffect::MakeForShader(SkString(kSources[index]));
    entry.program.effect = std::move(compiled.effect);
    if (!compiled.errorText.isEmpty())
      entry.program.error = compiled.errorText.c_str();
  });
  return entry.program;
}

} // namespace videocut::skia_runtime::internal
