#pragma once

#include "text/QtTextPostEffectRuntime.h"
#include "videocut/text/TextExecutionEvidence.h"

#include <array>
#include <string>
#include <vector>

namespace videocut::skia_runtime::internal {

/// Materialized Qt TextPro render passes. The order is part of the runtime
/// contract: every pass is written to an RGBA8 target before its
/// image is sampled by the following pass.
enum class QtTextProPostEffectPassKind : unsigned char {
  RadianceThreshold = 0,
  RadianceDownsample,
  RadianceDirectionalBlur,
  RadianceBlend,
  AlphaOutline,
  TurbulenceNoise,
  TurbulenceDisplacement,
  GodRayThreshold,
  GodRayGaussianX,
  GodRayGaussianY,
  GodRayComposite,
  LinearWipe,
  GaussianCopy,
  GaussianDownsample,
  GaussianX,
  GaussianY,
  GaussianUpsample,
  SoftGlowThreshold,
  SoftGlowCopy,
  SoftGlowX,
  SoftGlowY,
  SoftGlowBlend,
  DistortLens,
  DistortBlurX1,
  DistortBlurY1,
  DistortBlurX2,
  DistortBlurY2,
  DistortOutput,
  RadialBlur,
  Shake,
  TrailBlurX,
  TrailBlurY,
  TrailTimeA,
  TrailTimeBCommit,
  TrailHint,
  DirectionalBlurCopy,
  DirectionalBlur,
  DustNoise,
  DustMaskNoise,
  DustParticles,
  DeepGlowPreprocess,
  DeepGlowDownscale,
  DeepGlowX,
  DeepGlowY,
  DeepGlowComposite,
  DeepGlowPostprocess,
  SGlowThreshold,
  SGlowHorizontalFirst,
  SGlowHorizontalSecond,
  SGlowVerticalFirst,
  SGlowVerticalSecond,
  SGlowComposite,
  WaveWarp,
  ChromaticAberration,
  MultiShadow,
  SimpleChoker,
  SimpleChokerDownscale,
  SimpleChokerMatte,
  SimpleChokerBlend,
  CCLens,
  OpticsCompensation,
  OpticsCompensationDistort,
  OpticsCompensationAntiAliasing,
  Projection,
  DynamicSignalGlitch,
  ElectricPulseMotionBlur,
  MorphologicalOutline,
  AlternatingSegmentMask,
  CenterSplitDisplacement,
  CutAndDrop,
  BlockGlitchDotMatrix,
  ProceduralFlameOutline,
  CylindricalScroll,
  SplitSqueezeWave,
  PerspectiveEchoTrail,
  PulseEnvelope,
  RadianceErode,
};

struct QtTextProPostEffectPass final {
  QtTextProPostEffectPassKind kind{
      QtTextProPostEffectPassKind::RadianceThreshold};
  int width{0};
  int height{0};
  QtTextPostEffectSurfaceSemantics surfaceSemantics{
      QtTextPostEffectSurfaceSemantics::ColorPremultiplied};
};

struct QtTextProRadianceUniforms final {
  int blurWidth{0};
  int blurHeight{0};
  float effectiveGlowIntensity{0.0F};
  float effectiveExposure{1.0F};
  float sampleCount{0.0F};
  float radius{0.0F};
  float sigma{0.0F};
  float sampleStep{0.0F};
  float stepX{0.0F};
  float stepY{0.0F};
  int directionCount{1};
  int erodeIterations{0};
  float erodeStepX{0.0F};
  float erodeStepY{0.0F};
  float thresholdLow{0.5F};
  float thresholdHigh{1.0F};
  float thresholdSmooth{0.0F};
  float thresholdType{0.0F};
  float grayScale{0.0F};
  float spaceDither{0.0F};
  float borderType{0.0F};
  float displayGlow{0.0F};
  std::array<float, 3> glowColor{1.0F, 1.0F, 1.0F};
};

struct QtTextProAlphaOutlineUniforms final {
  float offsetX{0.0F};
  float offsetY{0.0F};
  float ratio{1.0F};
  float size{1.0F};
  float scaleX{1.0F};
  float scaleY{1.0F};
  std::array<float, 4> outlineColor{1.0F, 1.0F, 1.0F, 1.0F};
  float intensity{1.0F};
  int radialTapCount{16};
  bool linearLightBlend{true};
};

struct QtTextProTurbulenceUniforms final {
  int qualityType{3};
  float qualityScale{0.25F};
  int noiseWidth{0};
  int noiseHeight{0};
  float cycle{300.0F};
  float offsetX{0.0F};
  float offsetY{0.0F};
  float quantity{2.0F};
  float complexity{1.0F};
  float evolution{0.0F};
  float type{0.0F};
  float contrast{0.0F};
  float fixType{0.0F};
  float pictureScale{1.0F};
  float motionTileType{0.0F};
  // Captured material defaults. They are deliberately explicit rather than
  // visually fitted constants.
  float subImpact{0.60000002384185791F};
  float subScale{56.0F};
  float subRotate{0.0F};
  std::array<float, 2> subOffset{0.0F, 0.0F};
  float range{0.30000001192092896F};
};

struct QtTextProGodRayUniforms final {
  int rayWidth{0};
  int rayHeight{0};
  float downscale{1.0F};
  float useAlphaThreshold{1.0F};
  float colorType{0.0F};
  float threshold{0.5F};
  float sampleCount{0.0F};
  float dx{0.0F};
  float dy{0.0F};
  float sigmaX{0.0F};
  float sigmaY{0.0F};
  float intensity{1.0F};
  float brightness{0.1F};
  float quality{50.0F};
  float grayscaleCorrection{1.0F};
  std::array<float, 2> center{0.5F, 0.5F};
  float useAngle{0.0F};
  float minAngle{0.0F};
  float maxAngle{0.0F};
  float borderType{0.0F};
  float blendMode{0.0F};
  std::array<float, 3> lightColor{1.0F, 1.0F, 1.0F};
  float displayRayOnly{0.0F};
  float inverseGammaCorrection{0.0F};
  float gamma{2.2F};
  float dither{0.0F};
  float noiseIntensity{0.0F};
  float weightDecay{1.0F};
  float colorDecay{1.0F};
  float normalizationSample{50.0F};
  float sampleScale{15.0F};
  float sampleBias{20.0F};
};

struct QtTextProLinearWipeUniforms final {
  float mappedProgress{-0.5F};
  float rotationDegrees{0.0F};
  float feather{0.0F};
};

struct QtTextProGaussianUniforms final {
  bool active{false};
  int blurWidth{0};
  int blurHeight{0};
  float intensity{0.0F};
  float sampleCount{0.0F};
  float sigmaX{0.0F};
  float sigmaY{0.0F};
  float stepX{0.0F};
  float stepY{0.0F};
  float gamma{2.2F};
};

struct QtTextProSoftGlowUniforms final {
  int glowWidth{0};
  int glowHeight{0};
  float effectiveIntensity{0.0F};
  float sampleCount{0.0F};
  float sigmaX{0.0F};
  float sigmaY{0.0F};
  float stepX{0.0F};
  float stepY{0.0F};
  float thresholdLow{0.1F};
  float thresholdHigh{1.0F};
  float thresholdSmooth{0.0F};
  float thresholdType{0.0F};
  float grayScale{0.0F};
  float shaderExposure{1.0F};
  float displayGlow{0.0F};
  std::array<float, 3> glowColor{1.0F, 1.0F, 1.0F};
};

struct QtTextProPostEffectPlan final {
  text::TextPostEffectKind kind{
      text::TextPostEffectKind::TurbulenceDisplacement};
  int width{0};
  int height{0};
  bool supported{false};
  // directionNum=4 is source-closed only together with its authored
  // AlphaOutline consumer. The directional node publishes its input as the
  // DAG dependency while the runtime keeps the native pair pending.
  bool deferredDirectionalBlurFusion{false};
  std::string unsupportedReason;
  std::vector<QtTextProPostEffectPass> passes;
  QtTextProRadianceUniforms radiance;
  QtTextProAlphaOutlineUniforms alphaOutline;
  QtTextProTurbulenceUniforms turbulence;
  QtTextProGodRayUniforms godRay;
  QtTextProLinearWipeUniforms linearWipe;
  QtTextProGaussianUniforms gaussian;
  QtTextProSoftGlowUniforms softGlow;
  QtDirectionalBlurContract directionalBlur;
  QtDustContract dust;
  QtDeepGlowContract deepGlow;
  QtSGlowContract sGlow;
  QtWaveWarpContract waveWarp;
  QtChromaticAberrationContract chromaticAberration;
  QtMultiShadowContract multiShadow;
  QtDistortChromaContract distortChroma;
  QtRadialBlurContract radialBlur;
  QtShakeContract shake;
  QtTrailContract trail;
};

struct QtTextProPostEffectResult final {
  sk_sp<SkPicture> picture;
  QtTextProPostEffectPlan plan;
  QtTextPostEffectExecutionTrace execution;
  text::TextNamedParameterConsumptionEvidence parameters;
  std::string error;

  [[nodiscard]] explicit operator bool() const noexcept {
    return picture != nullptr && error.empty();
  }
};

/// Resolves source-backed material uniforms and the exact RGBA8 pass
/// topology for one post-effect node. No rendering or template identity is
/// consulted, so this can be used by any text renderer that owns a Page RT.
[[nodiscard]] QtTextProPostEffectPlan
ResolveQtTextProPostEffectPlan(const text::TextEffectPostEffectNode &effect,
                               double progress, const SkRect &recordingBounds,
                               const QtTextPostEffectStateContext *stateContext =
                                   nullptr);

/// Executes the resolved TextPro pass chain. `progress` samples authored
/// parameters while stateContext carries the caller-owned per-frame clock for
/// time-dependent effects.
[[nodiscard]] QtTextProPostEffectResult ExecuteQtTextProPostEffectPipeline(
    const sk_sp<SkPicture> &source, const text::TextEffectPostEffectNode &effect,
    double progress, const SkRect &recordingBounds,
    SkiaGpuContext *gpuContext,
    const QtTextPostEffectStateContext *stateContext = nullptr,
    bool collectParameterEvidence = false);

/// Convenience entry for one typed current post-effect node. A null result
/// means the caller must retain the node input and report the failed branch.
[[nodiscard]] sk_sp<SkPicture> ApplyQtTextProPostEffectPipelinePicture(
    const sk_sp<SkPicture> &source, const text::TextEffectPostEffectNode &effect,
    double progress, const SkRect &recordingBounds,
    SkiaGpuContext *gpuContext,
    const QtTextPostEffectStateContext *stateContext = nullptr);

[[nodiscard]] const char *
QtTextProPostEffectPassName(QtTextProPostEffectPassKind kind) noexcept;

} // namespace videocut::skia_runtime::internal
