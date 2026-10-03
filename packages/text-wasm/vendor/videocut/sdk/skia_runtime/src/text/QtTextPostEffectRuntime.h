#pragma once

#include "internal/skia/SkiaHeaders.h"
#include "text/SkiaGpuContext.h"

#include "videocut/text/TextEffectFramePlan.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace videocut::skia_runtime::internal {

// Color targets follow Skia's ordinary premultiplied-color contract. Data
// targets carry four independent normalized bytes and must never acquire
// RGB<=A premultiplication/clamping semantics while being rendered, snapped,
// dumped, or sampled by the following pass. The Skia alphaType used to
// transport a data target is intentionally not its interpretation contract.
enum class QtTextPostEffectSurfaceSemantics : unsigned char {
  ColorPremultiplied = 0,
  RawRgbaData,
};

/// Stable owner identity for stateful post nodes. This is a runtime graph
/// contract, not an address borrowed from a transient frame plan.
struct QtTextPostEffectStateContext final {
  std::uint32_t contractVersion{1U};
  std::uint64_t renderGraphInstanceId{0U};
  std::uint64_t lifecycleEpoch{0U};
  std::uint64_t effectNodeInstanceId{0U};
  std::uint64_t renderGroupInstanceId{0U};
  int presentationWidth{0};
  int presentationHeight{0};
  float renderGroupExpandRatioX{0.0F};
  float renderGroupExpandRatioY{0.0F};
  double effectTimeSeconds{0.0};
  std::uint64_t effectRuntimeClockRevision{0U};
  std::uint64_t randomSeed{0U};
};

struct QtRadianceGlowContract final {
  bool exactPathSupported{false};
  std::string unsupportedReason;
  int width{0};
  int height{0};
  int blurWidth{0};
  int blurHeight{0};
  int directionCount{1};
  int erodeIterations{0};
  float erodeStepX{0.0F};
  float erodeStepY{0.0F};
  float thresholdLow{0.5F};
  float thresholdHigh{1.0F};
  float thresholdSmooth{0.0F};
  float thresholdType{0.0F};
  float grayScale{0.0F};
  float sampleCount{0.0F};
  float sigma{0.0F};
  float stepX{0.0F};
  float stepY{0.0F};
  std::array<std::array<float, 2>, 4> directionSteps{};
  float spaceDither{0.0F};
  float borderType{0.0F};
  float exposure{1.0F};
  float displayGlow{0.0F};
  std::array<float, 3> glowColor{1.0F, 1.0F, 1.0F};
};

struct QtAlphaOutlineContract final {
  int width{0};
  int height{0};
  float offsetX{0.0F};
  float offsetY{0.0F};
  float ratio{1.0F};
  float size{1.0F};
  float scaleX{1.0F};
  float scaleY{1.0F};
  std::array<float, 4> outlineColor{1.0F, 1.0F, 1.0F, 1.0F};
  float intensity{1.0F};
};

struct QtTurbulenceContract final {
  bool exactPathSupported{false};
  std::string unsupportedReason;
  int width{0};
  int height{0};
  int noiseWidth{0};
  int noiseHeight{0};
  float cycle{300.0F};
  float offsetX{0.0F};
  float offsetY{0.0F};
  float quantity{2.0F};
  float complexity{2.0F};
  float evolution{0.0F};
  float type{0.0F};
  float contrast{0.0F};
  float range{0.3F};
  float pictureScale{1.0F};
  float motionTileType{0.0F};
};

/// Source-closed LumiGodRay contract recovered from
/// the native GodRay contract and the four fixed Metal fragment programs used by
/// Qt 11.3. The intermediate target dimensions intentionally remain integer
/// fields because Qt truncates the scaled RenderTexture dimensions.
struct QtGodRayContract final {
  bool exactPathSupported{false};
  std::string unsupportedReason;
  int width{0};
  int height{0};
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

struct QtLinearWipeContract final {
  bool exactPathSupported{false};
  std::string unsupportedReason;
  int width{0};
  int height{0};
  float mappedProgress{-0.5F};
  float rotationDegrees{0.0F};
  float feather{0.0F};
};

/// Fixed LumiGaussianBlur contract recovered from the component graph and the
/// two separable Metal shaders. Every pass writes an RGBA8Unorm target; the
/// dimensions below therefore describe real quantization boundaries rather
/// than an optimization hint.
struct QtGaussianBlurContract final {
  bool exactPathSupported{false};
  std::string unsupportedReason;
  std::uint32_t implementationVersion{0U};
  int width{0};
  int height{0};
  int blurWidth{0};
  int blurHeight{0};
  bool active{false};
  float intensity{0.0F};
  float sampleCount{0.0F};
  float sigmaX{0.0F};
  float sigmaY{0.0F};
  float stepX{0.0F};
  float stepY{0.0F};
  float gamma{2.2F};
};

/// Fixed LumiSoftGlow contract. The current graph owns five materialized
/// passes even when the separable kernels take their zero-sample copy branch.
/// Source-generation differences are compiled into explicit mathematical
/// parameters before execution; the runtime has no source-version switch.
struct QtSoftGlowContract final {
  bool exactPathSupported{false};
  std::string unsupportedReason;
  int width{0};
  int height{0};
  int glowWidth{0};
  int glowHeight{0};
  float lowIntensityScheduleScale{0.0F};
  bool foldSubunitExposureIntoIntensity{false};
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

/// Fixed LumiTrail contract. Unlike stateless post effects, one successful
/// execution is one history sample; progress is used only to evaluate authored
/// uniforms and is never a state key.
struct QtTrailContract final {
  bool exactPathSupported{false};
  std::string unsupportedReason;
  std::uint32_t implementationVersion{0U};
  std::uint32_t stateSchemaVersion{0U};
  int width{0};
  int height{0};
  float blur{0.0F};
  float weaken{0.2F};
  int blurSamples{0};
  float stepX{0.0F};
  float stepY{0.0F};
  float sigmaX{0.0F};
  float sigmaY{0.0F};
  float decayStep{0.06F};
  int hintProfile{0};
  float hintHue{0.0F};
  float hintOffset{0.0F};
  float hintMin{0.0F};
  float hintMax{1.0F};
  bool baseEnabled{true};
  std::array<float, 4> baseHint{1.0F, 1.0F, 1.0F, 1.0F};
};

struct QtDustNoiseContract final {
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

/// Source-closed LumiDust pass graph. The procedural-noise RT is derived from
/// the active presentation target using the authored 1280x720 -> 640x360
/// resource ratio; it is intentionally independent of the Page-sized particle
/// target.
struct QtDustContract final {
  bool exactPathSupported{false};
  std::string unsupportedReason;
  std::uint32_t implementationVersion{0U};
  int width{0};
  int height{0};
  int presentationWidth{0};
  int presentationHeight{0};
  int noiseWidth{0};
  int noiseHeight{0};
  QtDustNoiseContract noise;
  QtDustNoiseContract maskNoise;
  float distortionIntensity{1.0F};
  float gravity{0.0F};
  float gravityRotationDegrees{0.0F};
  int maskType{0};
  float maskFeather{1.0F};
  float maskLineRotationRadians{0.0F};
  float progressPercent{0.0F};
};

/// Native geometry and kernel schedule for LumiDeepGlow. These values
/// must stay a function of the active Page dimensions; in particular the
/// normalized stride is not a fixed blur constant.
struct QtDeepGlowContract final {
  bool exactPathSupported{false};
  std::string unsupportedReason;
  int width{0};
  int height{0};
  int blurWidth{0};
  int blurHeight{0};
  int glowIterations{0};
  int postprocessIteration{0};
  float quality{1.0F};
  float sharedDownscale{0.5F};
  float internalRadius{0.0F};
  float radiusFactor{0.0F};
  float kernelStrideBase{0.0F};
  float kernelStrideAspectPower{0.0F};
  float basePercentage{0.004F};
  float stepsMultiplier{1.0F};
  float downSample{1.0F};
  float stepsInt{1.0F};
  float authoredGamma{2.2F};
  float gamma{1.0F};
  float exposure{1.0F};
  float threshold{0.0F};
  std::array<float, 3> glowColor{1.0F, 1.0F, 1.0F};
  int blendMode{0};
  float glowFromAlpha{0.0F};
  float sourceOpacity{1.0F};
  bool tintEnabled{false};
  int tintMode{0};
  float tintMix{1.0F};
  std::array<float, 3> tintColor{1.0F, 1.0F, 1.0F};
  bool chromaticAberrationEnabled{false};
  std::array<float, 3> chromaticOffsets{0.0F, 0.0F, 0.0F};
  std::array<float, 8> sampledSteps{};
  std::array<float, 8> strides{};
  std::array<float, 8> opacities{};
};

struct QtSGlowContract final {
  bool exactPathSupported{false};
  std::string unsupportedReason;
  int width{0};
  int height{0};
  int blurWidth{0};
  int blurHeight{0};
  float brightness{1.0F};
  float glowWidth{0.05F};
  float threshold{0.1F};
  float glowFromAlpha{0.0F};
  float useAlphaThreshold{1.0F};
  float bgBrightness{1.0F};
  float lightBackground{0.0F};
  float widthX{1.0F};
  float widthY{1.0F};
  std::array<float, 3> channelWidths{1.0F, 1.0F, 1.0F};
  std::array<float, 3> glowColor{1.0F, 1.0F, 1.0F};
};

struct QtDirectionalBlurContract final {
  bool exactPathSupported{false};
  std::string unsupportedReason;
  int width{0};
  int height{0};
  int blurWidth{0};
  int blurHeight{0};
  int directionNum{1};
  bool requiresAlphaOutlineFusion{false};
  float blurIntensity{0.0F};
  float angleDegrees{0.0F};
};

struct QtWaveWarpContract final {
  bool exactPathSupported{false};
  std::string unsupportedReason;
  int width{0};
  int height{0};
  std::array<float, 2> amplitude{0.0F, 0.0F};
  float wavelength{1.0F};
  float phaseRadians{0.0F};
  float angleRadians{0.0F};
  float waveType{0.0F};
  float fixedType{0.0F};
  float antiAliasing{0.0F};
};

struct QtChromaticAberrationContract final {
  bool exactPathSupported{false};
  std::string unsupportedReason;
  int width{0};
  int height{0};
  float offsetX{0.0F};
  float offsetY{0.0F};
};

struct QtMultiShadowLayer final {
  float alpha{0.0F};
  float positionX{0.0F};
  float positionY{0.0F};
  float scale{1.0F};
  float rotationRadians{0.0F};
  std::array<float, 4> color{0.0F, 0.0F, 0.0F, 1.0F};
};

struct QtMultiShadowContract final {
  bool exactPathSupported{false};
  std::string unsupportedReason;
  int width{0};
  int height{0};
  int layerCount{0};
  float originalAlpha{1.0F};
  std::array<float, 2> renderGroupExpandRatio{0.0F, 0.0F};
  std::array<QtMultiShadowLayer, 8> layers{};
};

/// Source-closed six-pass LumiDistortChroma contract. The lens target is a
/// real RGBA8 scalar-encoding surface shared by four ping-pong blur passes;
/// these dimensions and mapped values therefore cannot be collapsed into a
/// single visual approximation.
struct QtDistortChromaContract final {
  bool exactPathSupported{false};
  std::string unsupportedReason;
  int width{0};
  int height{0};
  int lensWidth{0};
  int lensHeight{0};
  int blurSteps{1};
  float blurStrideFirst{0.5F};
  float blurStrideSecond{2.0F};
  float blurAngleDegrees{0.0F};
  float blurPerpendicularAngleDegrees{90.0F};
  float rotateWarpDirectionDegrees{0.0F};
  float amountRelX{1.0F};
  float amountRelY{1.0F};
  int wrapModeX{0};
  int wrapModeY{0};
  int chromaSteps{8};
  float warpRed{0.5F};
  float warpBlue{0.5F};
  float warpAmount{0.0F};
  std::array<float, 3> color1{1.0F, 0.0F, 0.0F};
  std::array<float, 3> color2{0.0F, 1.0F, 0.0F};
  std::array<float, 3> color3{0.0F, 0.0F, 1.0F};
  float colorMix{1.0F};
};

/// Source-closed LumiRadialBlur contract. These fields are the values bound
/// after the recovered clamps and unit mappings are applied.
struct QtRadialBlurContract final {
  bool exactPathSupported{false};
  std::string unsupportedReason;
  std::uint32_t implementationVersion{0U};
  int width{0};
  int height{0};
  float intensity{0.0F};
  int blurType{0};
  std::array<float, 2> center{0.5F, 0.5F};
  float quality{20.0F};
  float weightDecay{1.0F};
  float dither{0.0F};
  int borderType{0};
  int blurAlpha{1};
  int inverseGammaCorrection{0};
  float gamma{2.2F};
  float lightIntensity{1.0F};
  float lightTransferMode{0.0F};
};

/// Source-closed non-motion-blur LumiSShake contract.  The three matrices are
/// the exact column-major float4x4 ABI consumed by the recovered Metal vertex
/// shader; keeping them in the resolved contract makes random-wave and matrix
/// regressions testable without rendering a template screenshot.
struct QtShakeContract final {
  bool exactPathSupported{false};
  std::string unsupportedReason;
  std::uint32_t implementationVersion{0U};
  int width{0};
  int height{0};
  int fillModeX{0};
  int fillModeY{0};
  bool motionBlurEnabled{false};
  double waveTimeSeconds{0.0};
  std::array<std::array<float, 16>, 3> uvMatrices{};
};

/// One pass that was actually materialized by the post-effect executor. This
/// is deliberately a renderer-neutral byte-target record: it describes the
/// pass boundary without exposing SkSurface, Ganesh, or Metal objects.
struct QtTextPostEffectExecutedPass final {
  std::string chain;
  std::string stage;
  std::size_t ordinal{0};
  std::size_t passCount{0};
  int width{0};
  int height{0};
  QtTextPostEffectSurfaceSemantics surfaceSemantics{
      QtTextPostEffectSurfaceSemantics::ColorPremultiplied};
  // Names the implementation that crossed this quantized pass boundary.
  // EngineCopy records native and portable shader backends explicitly.
  std::string executor{"skia-runtime-effect"};
  std::uint32_t executorVersion{0U};
  std::string pixelFormat{"RGBA8Unorm"};
};

struct QtTextPostEffectExecutionTrace final {
  int pageWidth{0};
  int pageHeight{0};
  std::vector<QtTextPostEffectExecutedPass> passes;
};

[[nodiscard]] QtRadianceGlowContract
ResolveQtRadianceGlowContract(const text::TextEffectPostEffectNode &effect,
                              double progress, int width, int height);

[[nodiscard]] QtAlphaOutlineContract
ResolveQtAlphaOutlineContract(const text::TextEffectPostEffectNode &effect,
                              double progress, int width, int height);

[[nodiscard]] QtTurbulenceContract
ResolveQtTurbulenceContract(const text::TextEffectPostEffectNode &effect,
                            double progress, int width, int height);

[[nodiscard]] QtGodRayContract
ResolveQtGodRayContract(const text::TextEffectPostEffectNode &effect, double progress,
                        int width, int height);

[[nodiscard]] QtLinearWipeContract
ResolveQtLinearWipeContract(const text::TextEffectPostEffectNode &effect,
                            double progress, int width, int height);

[[nodiscard]] QtGaussianBlurContract
ResolveQtGaussianBlurContract(const text::TextEffectPostEffectNode &effect,
                              double progress, int width, int height);

[[nodiscard]] QtSoftGlowContract
ResolveQtSoftGlowContract(const text::TextEffectPostEffectNode &effect,
                          double progress, int width, int height);

[[nodiscard]] QtTrailContract
ResolveQtTrailContract(const text::TextEffectPostEffectNode &effect, double progress,
                       int width, int height);

[[nodiscard]] QtDustContract
ResolveQtDustContract(const text::TextEffectPostEffectNode &effect, double progress,
                      int width, int height, int presentationWidth,
                      int presentationHeight);

[[nodiscard]] QtDeepGlowContract
ResolveQtDeepGlowContract(const text::TextEffectPostEffectNode &effect,
                          double progress, int width, int height);

[[nodiscard]] QtDeepGlowContract
ResolveQtTypedDeepGlowContract(const text::TextEffectPostEffectNode &effect,
                               double progress, int width, int height);

[[nodiscard]] QtSoftGlowContract
ResolveQtTypedSGlowContract(const text::TextEffectPostEffectNode &effect,
                            double progress, int width, int height);

[[nodiscard]] QtSGlowContract
ResolveQtSGlowContract(const text::TextEffectPostEffectNode &effect,
                       double progress, int width, int height);

[[nodiscard]] QtDirectionalBlurContract ResolveQtDirectionalBlurContract(
    const text::TextEffectPostEffectNode &effect, double progress, int width,
    int height);

[[nodiscard]] QtWaveWarpContract
ResolveQtWaveWarpContract(const text::TextEffectPostEffectNode &effect,
                          double progress, int width, int height);

[[nodiscard]] QtChromaticAberrationContract
ResolveQtChromaticAberrationContract(
    const text::TextEffectPostEffectNode &effect, double progress, int width,
    int height);

[[nodiscard]] QtMultiShadowContract
ResolveQtMultiShadowContract(const text::TextEffectPostEffectNode &effect,
                             double progress, int width, int height,
                             float renderGroupExpandRatioX,
                             float renderGroupExpandRatioY);

[[nodiscard]] QtDistortChromaContract
ResolveQtDistortChromaContract(const text::TextEffectPostEffectNode &effect,
                               double progress, int width, int height);

[[nodiscard]] QtRadialBlurContract
ResolveQtRadialBlurContract(const text::TextEffectPostEffectNode &effect,
                            double progress, int width, int height);

[[nodiscard]] QtShakeContract
ResolveQtShakeContract(const text::TextEffectPostEffectNode &effect, double progress,
                       double effectTimeSeconds, int width, int height);

/// Executes the source-closed DirectionalBlurs -> AlphaOutline pair used by
/// the audited Qt TextPro graph.  The two nodes remain separate in the model;
/// this helper fuses only their seven materialized Metal passes so the
/// intermediate four-direction RGBA8 textures are not approximated or
/// double-applied.
[[nodiscard]] sk_sp<SkPicture> ApplyExactQtTextDirectionalBlursChainPicture(
    const sk_sp<SkPicture> &source,
    const text::TextEffectPostEffectNode &directionalEffect,
    const text::TextEffectPostEffectNode &alphaOutlineEffect, double progress,
    const SkRect &recordingBounds, SkiaGpuContext *gpuContext,
    std::string *diagnostic = nullptr);

/// Variant used by the typed DAG executor when the two authored nodes own
/// different transition clocks. The source is still materialized only once.
[[nodiscard]] sk_sp<SkPicture> ApplyExactQtTextDirectionalBlursChainPicture(
    const sk_sp<SkPicture> &source,
    const text::TextEffectPostEffectNode &directionalEffect,
    const text::TextEffectPostEffectNode &alphaOutlineEffect,
    double directionalProgress, double alphaOutlineProgress,
    const SkRect &recordingBounds, SkiaGpuContext *gpuContext,
    std::string *diagnostic = nullptr);

/// Executes Qt's directionNum=1 branch as EngineCopy downsample followed by a
/// single DirectionalGaussian pass directly into OutputTex. This is a distinct
/// graph from the fused four-direction + AlphaOutline chain above.
[[nodiscard]] sk_sp<SkPicture> ApplyExactQtTextDirectionalBlurPicture(
    const sk_sp<SkPicture> &source,
    const text::TextEffectPostEffectNode &directionalEffect, double progress,
    const SkRect &recordingBounds, SkiaGpuContext *gpuContext,
    std::string *diagnostic = nullptr);

/// Executes the source-proven Qt TextPro post pass for the supported native
/// node contract. A null picture means that the authored parameter branch is
/// not yet source-closed or that a render target/runtime-effect failed; the
/// caller retains the typed node input and reports the unsupported branch.
[[nodiscard]] sk_sp<SkPicture> ApplyExactQtTextPostEffectPicture(
    const sk_sp<SkPicture> &source, const text::TextEffectPostEffectNode &effect,
    double progress, const SkRect &recordingBounds,
    SkiaGpuContext *gpuContext,
    QtTextPostEffectExecutionTrace *executionTrace = nullptr,
    const QtTextPostEffectStateContext *stateContext = nullptr);

/// Releases every persistent post-effect node owned by one render graph.
/// Text render lanes call this at destruction so a later lane can never
/// inherit history even if an allocator reuses the same process address.
void ReleaseExactQtTextPostEffectState(
    std::uint64_t renderGraphInstanceId) noexcept;

} // namespace videocut::skia_runtime::internal
