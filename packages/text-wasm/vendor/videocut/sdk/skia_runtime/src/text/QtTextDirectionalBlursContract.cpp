#include "text/QtTextDirectionalBlursRuntime.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <string>

namespace videocut::skia_runtime::internal {
namespace {

constexpr double kPi = 3.14159265358979323846264338327950288419716939937510;
constexpr double kNormalizationSize = 1000.0;
constexpr double kRadiusOverSigma = 3.0;
constexpr double kEpsilon = 1.0e-5;

// Canonical source/capture closure used to produce
// kQtTextDirectionalBlursSourceContractDigest:
//
// alpha-source=e78a822de60bdd786bd60b7ddddd8405a1d589f169db22139eaf66b41ad81714
// alpha-package-fragment=3ab220a79e63807b983fc7688c5043db18ed19e931521eea0d8a906637ce246f
// alpha-package-vertex=04b7a13be400187f0f8af8b397fa161094945ea69ae8a3c21750a213f77ba3b6
// alpha-runtime-fragment=b8da6de24ca4f2b8da16c600cb96e2d822441aa1d7b8cffff8f8b4efea93a7d1
// directional-source=056385a881ca92c85333e0309cbaa0bddcdc16041913c3d7d7a182d49ad4f69b
// directional-package-fragment=682234855e6626d0ae85ee29d7a60481b520bed05bbd965aa5f351cf15839b21
// directional-package-vertex=e0d4d9dfe3400d7a6fa3160771448f90055b04751b876c8675d1c4b710c5b002
// directional-runtime-fragment=9223c8f7462cde920aca5ee54fb53f6874bb11e2d709b95fd62cfe77b69f05d6
// blend-package-fragment=7339b42449c9e1fd49ef6aefedbeafa387a90f0a39256c28e75e908d5d530e68
// blend-runtime-fragment=d711e20477c020c443f53dd75280affa00de7860ca7ff8bfd8bd244a6770e2d2
// downsample-runtime-fragment=e0a619790c50b081c26271141744cb0141aa0be76ab7aefdfcd22f3a596a19dd
// page-contract=755x437-rgba8unorm-top-left-negative-height-viewport
// active-branch=direction4-mean-normal-exposure1-quality0.5-dither0-alphaoutline-v1

static_assert(sizeof(QtTextDirectionalBlursUniforms) == 48U);
static_assert(alignof(QtTextDirectionalBlursUniforms) == 16U);
static_assert(offsetof(QtTextDirectionalBlursUniforms, screenParams) == 0U);
static_assert(offsetof(QtTextDirectionalBlursUniforms, sample) == 16U);
static_assert(offsetof(QtTextDirectionalBlursUniforms, sigma) == 20U);
static_assert(offsetof(QtTextDirectionalBlursUniforms, spaceDither) == 24U);
static_assert(offsetof(QtTextDirectionalBlursUniforms, stepX) == 28U);
static_assert(offsetof(QtTextDirectionalBlursUniforms, stepY) == 32U);
static_assert(offsetof(QtTextDirectionalBlursUniforms, borderType) == 36U);
static_assert(offsetof(QtTextDirectionalBlursUniforms, directionNum) == 40U);
static_assert(offsetof(QtTextDirectionalBlursUniforms, exposure) == 44U);
static_assert(sizeof(QtTextDirectionalBlursBlendUniforms) == 12U);
static_assert(sizeof(QtTextAlphaOutlineUniforms) == 64U);
static_assert(alignof(QtTextAlphaOutlineUniforms) == 16U);
static_assert(offsetof(QtTextAlphaOutlineUniforms, outlineColor) == 32U);
static_assert(offsetof(QtTextAlphaOutlineUniforms, intensity) == 48U);

bool SameMetalFloat(const double value, const float expected) {
  return std::isfinite(value) && static_cast<float>(value) == expected;
}

bool ValidateIdentity(const QtTextDirectionalBlursIdentity &identity,
                      std::string &error) {
  if (identity.implementationId != kQtTextDirectionalBlursImplementationId) {
    error = "Qt DirectionalBlurs implementation id is unsupported";
    return false;
  }
  if (identity.implementationVersion !=
      kQtTextDirectionalBlursImplementationVersion) {
    error = "Qt DirectionalBlurs implementation version is unsupported";
    return false;
  }
  if (identity.contractSchemaVersion !=
      kQtTextDirectionalBlursContractSchemaVersion) {
    error = "Qt DirectionalBlurs contract schema version is unsupported";
    return false;
  }
  if (identity.sourceContractDigest !=
      kQtTextDirectionalBlursSourceContractDigest) {
    error = "Qt DirectionalBlurs source contract digest is unsupported";
    return false;
  }
  return true;
}

bool ValidateStandaloneIdentity(
    const QtTextDirectionalBlurStandaloneIdentity &identity,
    std::string &error) {
  if (identity.implementationId !=
      kQtTextDirectionalBlurStandaloneImplementationId) {
    error = "Qt standalone DirectionalBlur implementation id is unsupported";
    return false;
  }
  if (identity.implementationVersion !=
      kQtTextDirectionalBlurStandaloneImplementationVersion) {
    error =
        "Qt standalone DirectionalBlur implementation version is unsupported";
    return false;
  }
  if (identity.contractSchemaVersion !=
      kQtTextDirectionalBlurStandaloneContractSchemaVersion) {
    error = "Qt standalone DirectionalBlur contract schema is unsupported";
    return false;
  }
  if (identity.sourceContractDigest !=
          kQtTextDirectionalBlurStandaloneSourceTreeDigestA &&
      identity.sourceContractDigest !=
          kQtTextDirectionalBlurStandaloneSourceTreeDigestB) {
    error = "Qt standalone DirectionalBlur source tree is unsupported";
    return false;
  }
  return true;
}

bool ValidateParameters(const QtTextDirectionalBlursContractRequest &request,
                        std::string &error) {
  const auto &directional = request.directional;
  const auto &outline = request.alphaOutline;
  const bool finite =
      std::isfinite(directional.blurIntensity) &&
      std::isfinite(directional.angleDegrees) &&
      std::isfinite(directional.exposure) &&
      std::isfinite(directional.quality) &&
      std::isfinite(directional.spaceDither) &&
      std::isfinite(outline.offsetX) && std::isfinite(outline.offsetY) &&
      std::isfinite(outline.size) && std::isfinite(outline.scaleX) &&
      std::isfinite(outline.scaleY) && std::isfinite(outline.intensity) &&
      std::all_of(outline.outlineColor.begin(), outline.outlineColor.end(),
                  [](const double value) { return std::isfinite(value); });
  if (!finite) {
    error = "Qt DirectionalBlurs parameters contain a non-finite value";
    return false;
  }

  // The template's recovered authored envelope is [0,15] and [-90,90].
  // Formula branches outside that envelope are deliberately not claimed by
  // implementation v1 even though the source package declares a wider UI range.
  if (directional.blurIntensity < 0.0 || directional.blurIntensity > 15.0 ||
      directional.angleDegrees < -90.0 || directional.angleDegrees > 90.0) {
    error = "Qt DirectionalBlurs animation is outside the audited v1 envelope";
    return false;
  }
  if (directional.directionNum != 4 ||
      directional.borderType != QtTextDirectionalBlursBorderType::Normal ||
      directional.blendMode != QtTextDirectionalBlursBlendMode::Mean ||
      !SameMetalFloat(directional.exposure, 1.0F) ||
      !SameMetalFloat(directional.quality, 0.5F) ||
      !SameMetalFloat(directional.spaceDither, 0.0F)) {
    error = "Qt DirectionalBlurs options are outside direction4/Mean/Normal v1";
    return false;
  }

  if (!SameMetalFloat(outline.offsetX, 0.0F) ||
      !SameMetalFloat(outline.offsetY, 0.0F) ||
      !SameMetalFloat(outline.size, 1.0F) ||
      !SameMetalFloat(outline.scaleX, 1.0F) ||
      !SameMetalFloat(outline.scaleY, 1.0F) ||
      !SameMetalFloat(outline.outlineColor[0], 0.2F) ||
      !SameMetalFloat(outline.outlineColor[1], 0.2F) ||
      !SameMetalFloat(outline.outlineColor[2], 0.2F) ||
      !SameMetalFloat(outline.outlineColor[3], 1.0F) ||
      outline.intensity < 0.0 || outline.intensity > 1.0) {
    error = "Qt AlphaOutline parameters are outside the audited v1 branch";
    return false;
  }
  return true;
}

bool ValidateStandaloneParameters(
    const QtTextDirectionalBlurStandaloneContractRequest &request,
    std::string &error) {
  const auto &directional = request.directional;
  if (!std::isfinite(directional.blurIntensity) ||
      !std::isfinite(directional.angleDegrees) ||
      !std::isfinite(directional.exposure) ||
      !std::isfinite(directional.quality) ||
      !std::isfinite(directional.spaceDither)) {
    error = "Qt standalone DirectionalBlur parameters contain a non-finite "
            "value";
    return false;
  }
  if (directional.directionNum != 1) {
    error = "Qt standalone DirectionalBlur requires directionNum=1";
    return false;
  }
  const auto border = static_cast<std::int32_t>(directional.borderType);
  const auto blend = static_cast<std::int32_t>(directional.blendMode);
  if (border < static_cast<std::int32_t>(
                   QtTextDirectionalBlursBorderType::Normal) ||
      border > static_cast<std::int32_t>(
                   QtTextDirectionalBlursBorderType::Mirror)) {
    error = "Qt standalone DirectionalBlur border type is invalid";
    return false;
  }
  if (blend <
          static_cast<std::int32_t>(QtTextDirectionalBlursBlendMode::Screen) ||
      blend >
          static_cast<std::int32_t>(QtTextDirectionalBlursBlendMode::Mean)) {
    error = "Qt standalone DirectionalBlur authored blend mode is invalid";
    return false;
  }
  return true;
}

void ResolveSampleSchedule(const double intensity, double &sampleNumber,
                           double &downsampleScale) {
  double scale = 1.0;
  double bias = 0.0;
  double sampleScale = 1.0;
  if (intensity <= 10.0) {
    scale = 0.8;
  } else if (intensity <= 50.0) {
    scale = 0.7;
    bias = 2.0;
    sampleScale = 0.78;
  } else if (intensity <= 200.0) {
    scale = 0.5;
    bias = 10.0;
    sampleScale = 0.66;
  } else if (intensity <= 500.0) {
    scale = 0.25;
    bias = 60.0;
    sampleScale = 0.7;
  } else {
    scale = 0.125;
    bias = 100.0;
    sampleScale = 0.8;
  }
  sampleNumber = (scale * intensity + bias) * sampleScale;
  if (sampleNumber < 2.0)
    sampleNumber = std::floor(sampleNumber + 0.5);
  downsampleScale = scale;
}

double ResolveQualityParameter(const double quality) {
  const double mapped = quality * 2.0 - 1.0;
  return mapped < 0.0 ? std::pow(10.0, mapped) : mapped * 2.0 + 1.0;
}

QtTextDirectionalBlursPassTrace
MakePass(std::string name, const int inputWidth, const int inputHeight,
         const int outputWidth, const int outputHeight,
         const std::uint32_t inputTextureCount) {
  QtTextDirectionalBlursPassTrace pass;
  pass.name = std::move(name);
  pass.inputWidth = inputWidth;
  pass.inputHeight = inputHeight;
  pass.outputWidth = outputWidth;
  pass.outputHeight = outputHeight;
  pass.inputTextureCount = inputTextureCount;
  return pass;
}

} // namespace

bool ResolveQtTextDirectionalBlurStandaloneContract(
    const QtTextDirectionalBlurStandaloneContractRequest &request,
    QtTextDirectionalBlurStandaloneDerivedContract &derived,
    std::string &error) {
  derived = {};
  if (!ValidateStandaloneIdentity(request.identity, error))
    return false;
  if (request.directionContract != QtTextDirectionalBlursDirectionContract::
                                       TopLeftRowsNegativeHeightViewport) {
    error = "Qt standalone DirectionalBlur direction contract is unsupported";
    return false;
  }
  if (request.pageWidth <= 0 || request.pageHeight <= 0) {
    error = "Qt standalone DirectionalBlur Page dimensions are invalid";
    return false;
  }
  if (!ValidateStandaloneParameters(request, error))
    return false;

  QtTextDirectionalBlurStandaloneDerivedContract result;
  result.pageWidth = request.pageWidth;
  result.pageHeight = request.pageHeight;
  const double intensity =
      std::clamp(request.directional.blurIntensity, 0.0, 1000.0);
  const double exposure =
      std::clamp(request.directional.exposure, 0.0, 10.0);
  const double quality = std::clamp(request.directional.quality, 0.0, 1.0);
  const double spaceDither =
      std::clamp(request.directional.spaceDither, 0.0, 1.0);
  ResolveSampleSchedule(intensity, result.sampleNumberBeforeQuality,
                        result.downsampleScale);
  result.qualityParameter = ResolveQualityParameter(quality);
  result.sampleNumber =
      result.sampleNumberBeforeQuality * result.qualityParameter;
  result.downsampleWidth = static_cast<int>(
      static_cast<double>(result.pageWidth) * result.downsampleScale);
  result.downsampleHeight = static_cast<int>(
      static_cast<double>(result.pageHeight) * result.downsampleScale);
  if (result.downsampleWidth <= 0 || result.downsampleHeight <= 0 ||
      !std::isfinite(result.sampleNumber)) {
    error = "Qt standalone DirectionalBlur derived render target is invalid";
    return false;
  }

  const double minimumSize =
      static_cast<double>(std::min(result.pageWidth, result.pageHeight));
  const double maximumSize =
      static_cast<double>(std::max(result.pageWidth, result.pageHeight));
  result.baseSize = std::max(minimumSize, maximumSize / 2.0);
  result.xScale = result.baseSize / static_cast<double>(result.pageWidth);
  result.yScale = result.baseSize / static_cast<double>(result.pageHeight);
  result.radius = intensity / kNormalizationSize;
  result.sigma = result.radius / kRadiusOverSigma;
  result.sampleDistance =
      result.radius / std::max(result.sampleNumber, kEpsilon);

  auto &uniforms = result.directionalUniforms;
  // directionNum=1 renders directly into OutputTex. The engine-generated
  // u_ScreenParams therefore describe the final target, not DownsampleTex.
  uniforms.screenParams[0] = static_cast<float>(result.pageWidth);
  uniforms.screenParams[1] = static_cast<float>(result.pageHeight);
  uniforms.screenParams[2] = static_cast<float>(result.pageWidth + 1) /
                             static_cast<float>(result.pageWidth);
  uniforms.screenParams[3] = static_cast<float>(result.pageHeight + 1) /
                             static_cast<float>(result.pageHeight);
  uniforms.sample = static_cast<float>(result.sampleNumber);
  uniforms.sigma = static_cast<float>(result.sigma);
  uniforms.spaceDither = static_cast<float>(spaceDither);
  const double angle =
      (-request.directional.angleDegrees - 90.0) * kPi / 180.0;
  uniforms.stepX = static_cast<float>(result.sampleDistance * std::cos(angle) *
                                     result.xScale);
  uniforms.stepY = static_cast<float>(result.sampleDistance * std::sin(angle) *
                                     result.yScale);
  uniforms.borderType =
      static_cast<std::int32_t>(request.directional.borderType);
  uniforms.directionNum = 1;
  uniforms.exposure = static_cast<float>(exposure);

  result.passes[0] =
      MakePass("Downsample", result.pageWidth, result.pageHeight,
               result.downsampleWidth, result.downsampleHeight, 1U);
  result.passes[0].indexTypeMetalValue = 0U;
  result.passes[0].cullModeMetalValue = 1U;
  result.passes[0].frontFacingWindingMetalValue = 1U;
  result.passes[0].elementCount = 3U;
  result.passes[0].clearColor = {0.0F, 0.0F, 0.0F, 1.0F};
  result.passes[1] =
      MakePass("DirectionalGaussian", result.downsampleWidth,
               result.downsampleHeight, result.pageWidth, result.pageHeight,
               1U);

  derived = std::move(result);
  error.clear();
  return true;
}

bool ResolveQtTextDirectionalBlursContract(
    const QtTextDirectionalBlursContractRequest &request,
    QtTextDirectionalBlursDerivedContract &derived, std::string &error) {
  derived = {};
  if (!ValidateIdentity(request.identity, error))
    return false;
  if (request.directionContract != QtTextDirectionalBlursDirectionContract::
                                       TopLeftRowsNegativeHeightViewport) {
    error = "Qt DirectionalBlurs direction contract is unsupported";
    return false;
  }
  if (!ValidateParameters(request, error))
    return false;

  QtTextDirectionalBlursDerivedContract result;
  result.pageWidth = request.pageWidth;
  result.pageHeight = request.pageHeight;
  ResolveSampleSchedule(request.directional.blurIntensity,
                        result.sampleNumberBeforeQuality,
                        result.downsampleScale);
  result.qualityParameter =
      ResolveQualityParameter(request.directional.quality);
  result.sampleNumber =
      result.sampleNumberBeforeQuality * result.qualityParameter;
  result.downsampleWidth = static_cast<int>(
      static_cast<double>(result.pageWidth) * result.downsampleScale);
  result.downsampleHeight = static_cast<int>(
      static_cast<double>(result.pageHeight) * result.downsampleScale);
  if (result.downsampleWidth <= 0 || result.downsampleHeight <= 0 ||
      !std::isfinite(result.sampleNumber)) {
    error = "Qt DirectionalBlurs derived render target is invalid";
    return false;
  }

  const double minimumSize =
      static_cast<double>(std::min(result.pageWidth, result.pageHeight));
  const double maximumSize =
      static_cast<double>(std::max(result.pageWidth, result.pageHeight));
  result.baseSize = std::max(minimumSize, maximumSize / 2.0);
  result.xScale = result.baseSize / static_cast<double>(result.pageWidth);
  result.yScale = result.baseSize / static_cast<double>(result.pageHeight);
  result.radius = request.directional.blurIntensity / kNormalizationSize;
  result.sigma = result.radius / kRadiusOverSigma;
  result.sampleDistance =
      result.radius / std::max(result.sampleNumber, kEpsilon);

  const double startAngle =
      (-request.directional.angleDegrees - 90.0) * kPi / 180.0;
  const double deltaAngle = kPi / 4.0;
  for (std::size_t index = 0U; index < result.directionalUniforms.size();
       ++index) {
    auto &uniforms = result.directionalUniforms[index];
    uniforms.screenParams[0] = static_cast<float>(result.downsampleWidth);
    uniforms.screenParams[1] = static_cast<float>(result.downsampleHeight);
    uniforms.screenParams[2] = static_cast<float>(result.downsampleWidth + 1) /
                               static_cast<float>(result.downsampleWidth);
    uniforms.screenParams[3] = static_cast<float>(result.downsampleHeight + 1) /
                               static_cast<float>(result.downsampleHeight);
    uniforms.sample = static_cast<float>(result.sampleNumber);
    uniforms.sigma = static_cast<float>(result.sigma);
    const double angle = startAngle + static_cast<double>(index) * deltaAngle;
    uniforms.stepX = static_cast<float>(result.sampleDistance *
                                        std::cos(angle) * result.xScale);
    uniforms.stepY = static_cast<float>(result.sampleDistance *
                                        std::sin(angle) * result.yScale);
  }

  // The authored contract writes shared option uniforms only to matBlurs[1]. The
  // captured buffers therefore contain direction=4/exposure=1 only on pass 1;
  // passes 2..4 retain zero. Reproducing that material state is observable and
  // avoids silently inventing a cleaner-but-different contract.
  result.directionalUniforms[0].spaceDither =
      static_cast<float>(request.directional.spaceDither);
  result.directionalUniforms[0].borderType =
      static_cast<std::int32_t>(request.directional.borderType);
  result.directionalUniforms[0].directionNum = request.directional.directionNum;
  result.directionalUniforms[0].exposure =
      static_cast<float>(request.directional.exposure);

  result.blendUniforms.blendMode =
      static_cast<std::int32_t>(request.directional.blendMode);
  result.blendUniforms.directionNum = request.directional.directionNum;
  result.blendUniforms.exposure =
      static_cast<float>(request.directional.exposure);

  result.alphaOutlineUniforms.offsetX =
      static_cast<float>(request.alphaOutline.offsetX);
  result.alphaOutlineUniforms.offsetY =
      static_cast<float>(request.alphaOutline.offsetY);
  result.alphaOutlineUniforms.ratio = static_cast<float>(result.pageWidth) /
                                      static_cast<float>(result.pageHeight);
  result.alphaOutlineUniforms.size =
      static_cast<float>(request.alphaOutline.size);
  result.alphaOutlineUniforms.scaleX =
      static_cast<float>(request.alphaOutline.scaleX);
  result.alphaOutlineUniforms.scaleY =
      static_cast<float>(request.alphaOutline.scaleY);
  for (std::size_t channel = 0U; channel < 4U; ++channel) {
    result.alphaOutlineUniforms.outlineColor[channel] =
        static_cast<float>(request.alphaOutline.outlineColor[channel]);
  }
  result.alphaOutlineUniforms.intensity =
      static_cast<float>(request.alphaOutline.intensity);

  result.passes[0] =
      MakePass("Downsample", result.pageWidth, result.pageHeight,
               result.downsampleWidth, result.downsampleHeight, 1U);
  result.passes[0].indexTypeMetalValue = 0U;
  result.passes[0].cullModeMetalValue = 1U;
  result.passes[0].frontFacingWindingMetalValue = 1U;
  result.passes[0].elementCount = 3U;
  result.passes[0].clearColor = {0.0F, 0.0F, 0.0F, 1.0F};
  for (std::size_t index = 0U; index < 4U; ++index) {
    result.passes[index + 1U] =
        MakePass("DirectionalGaussian" + std::to_string(index + 1U),
                 result.downsampleWidth, result.downsampleHeight,
                 result.downsampleWidth, result.downsampleHeight, 1U);
  }
  result.passes[5] = MakePass("MeanBlendUpscale", result.downsampleWidth,
                              result.downsampleHeight, result.pageWidth,
                              result.pageHeight, 4U);
  result.passes[6] =
      MakePass("AlphaOutline", result.pageWidth, result.pageHeight,
               result.pageWidth, result.pageHeight, 1U);

  derived = std::move(result);
  error.clear();
  return true;
}

} // namespace videocut::skia_runtime::internal
