#include "text/TextRenderPipeline.h"

namespace videocut::skia_runtime::internal::text_lane {


sk_sp<SkPicture> ExecuteClosedExecutionMaterial(
    const sk_sp<SkPicture> &source,
    const text::TextEffectExecutionNodeFramePlan &node,
    const std::vector<sk_sp<SkPicture>> &orderedInputs,
    const std::unordered_map<std::string, sk_sp<SkImage>> &sampledMediaInputs,
    const std::optional<std::uint64_t> stableUnitId,
    const text::TextEffectFramePlan &framePlan, TextVisualAssetStore &assets,
    const PageRenderGroupDomain *pageDomain, const SkRect &recordingBounds,
    const float referenceToExecutionScaleX,
    const float referenceToExecutionScaleY,
    const float projectionCanvasHeight, const SkSize viewportSize,
    SkiaGpuContext *gpuContext,
    std::string &error) {
  using Capability = text::TextEffectExecutionCapability;
  using Kind = text::TextEffectExecutionParameterKind;
  if (!ValidateClosedExecutionMaterialParameters(node, error))
    return {};
  std::size_t requiredInputCount = 0U;
  switch (node.capability) {
  case Capability::MaterialProjectionMeshComposite: {
    const float mode = ConsumeTextExecutionParameter(
        node, Kind::MaterialScalar, 4U)->values.front();
    requiredInputCount = mode < 2.0F ? 1U : mode == 3.0F ? 4U : 2U;
    break;
  }
  case Capability::MaterialDeepGlowComposite: {
    const auto *mode = ConsumeTextExecutionParameter(node, Kind::MaterialScalar, 5U);
    requiredInputCount = mode && mode->values.front() == 2.0F ? 9U
                        : mode && mode->values.front() == 1.0F ? 8U : 1U;
    break;
  }
  case Capability::MaterialTurbulenceDisplacement:
  case Capability::MaterialCylinderProjectionComposite:
  case Capability::MaterialParticleScatterComposite:
  case Capability::MaterialPackedMultiscaleGaussianBlur:
  case Capability::MaterialSdfAlphaOutline:
  case Capability::MaterialSdfWaveEnergy:
  case Capability::MaterialShapedRampMask:
  case Capability::MaterialMaskedDualOffsetCrossfade:
  case Capability::MaterialFaceNoiseShadow:
  case Capability::MaterialMaskMotionNoiseGlow:
    requiredInputCount = 1U;
    break;
  case Capability::MaterialLineColumnNoiseTrail:
    requiredInputCount = 2U;
    break;
  case Capability::MaterialMultimeshTextComposite:
    requiredInputCount = ConsumeTextExecutionParameter(
        node, Kind::MaterialScalar, 7U, stableUnitId)->values.front() == 0.0F
        ? 2U : 4U;
    break;
  case Capability::MaterialFluxTurbulentBlend:
    requiredInputCount = ConsumeTextExecutionParameter(
        node, Kind::MaterialScalar, 11U, stableUnitId)->values.front() == 2.0F
        ? 3U : 1U;
    break;
  case Capability::MaterialWeightedAxisBoxBlur: {
    const auto *restore = ConsumeTextExecutionParameter(
        node, Kind::MaterialScalar, 7U, stableUnitId);
    requiredInputCount = restore && restore->values.front() > 0.5F ? 2U : 1U;
    break;
  }
  case Capability::MaterialMaskedCutLine:
  case Capability::MaterialSdfNormalLightSweep:
  case Capability::MaterialHsvOriginalOverBlur:
  case Capability::MaterialNoiseThresholdDissolve:
    requiredInputCount = 2U;
    break;
  case Capability::MaterialDualPackedGlowComposite:
  case Capability::MaterialLightSweepPackedGlowComposite:
    requiredInputCount = 4U;
    break;
  default:
    break;
  }
  if (requiredInputCount != 0U && orderedInputs.size() != requiredInputCount) {
    error = "text execution material ordered input contract is incomplete: " +
            node.nodeId;
    return {};
  }
  const auto &primarySource =
      requiredInputCount != 0U ? orderedInputs.front() : source;
  ExecutionGraphRaster raster;
  const auto sourceBounds =
      node.capability == Capability::MaterialVatRbdMesh && pageDomain
          ? pageDomain->localBounds
          : recordingBounds;
  if (!MaterializeExecutionGraphRaster(primarySource, sourceBounds,
                                       gpuContext,
                                       raster, error)) {
    return {};
  }
  const auto materializeOrderedInput =
      [&](const std::size_t index, sk_sp<SkImage> &image) -> bool {
    if (index >= orderedInputs.size()) {
      error = "text execution material ordered input is missing: " +
              node.nodeId;
      return false;
    }
    ExecutionGraphRaster inputRaster;
    if (!MaterializeExecutionGraphRaster(orderedInputs[index], recordingBounds,
                                         gpuContext, inputRaster, error)) {
      return false;
    }
    image = std::move(inputRaster.image);
    return static_cast<bool>(image);
  };
  const auto rawInputShader = [](const sk_sp<SkImage> &image) {
    return image->makeRawShader(
        SkTileMode::kClamp, SkTileMode::kClamp,
        SkSamplingOptions(SkFilterMode::kLinear, SkMipmapMode::kNone));
  };
  const auto scalar = [&](const std::uint32_t slot) {
    return ConsumeTextExecutionParameter(node, Kind::MaterialScalar, slot,
                                          stableUnitId)
        ->values.front();
  };
  const auto scalarOrZero = [&](const std::uint32_t slot) {
    const auto *parameter = ConsumeTextExecutionParameter(
        node, Kind::MaterialScalar, slot, stableUnitId);
    return parameter ? parameter->values.front() : 0.0F;
  };
  const auto scalarOr = [&](const std::uint32_t slot, const float defaultValue) {
    const auto *parameter = ConsumeTextExecutionParameter(
        node, Kind::MaterialScalar, slot, stableUnitId);
    return parameter ? parameter->values.front() : defaultValue;
  };
  const auto referenceVectorToRaster = [&](const SkVector vector) {
    if (pageDomain) {
      const auto mapped = pageDomain->authoredToDevice.mapVector(vector);
      return SkVector::Make(mapped.x() * pageDomain->rasterScaleX,
                            mapped.y() * pageDomain->rasterScaleY);
    }
    return SkVector::Make(vector.x() * referenceToExecutionScaleX,
                          vector.y() * referenceToExecutionScaleY);
  };
  const auto vector2OrZero = [&](const std::uint32_t slot) {
    std::array<float, 2> values{0.0F, 0.0F};
    const auto *parameter = ConsumeTextExecutionParameter(
        node, Kind::MaterialVector2, slot, stableUnitId);
    if (parameter) {
      values[0] = parameter->values[0];
      values[1] = parameter->values[1];
    }
    return values;
  };
  const auto vector3OrZero = [&](const std::uint32_t slot) {
    std::array<float, 3> values{0.0F, 0.0F, 0.0F};
    const auto *parameter = ConsumeTextExecutionParameter(
        node, Kind::MaterialVector3, slot, stableUnitId);
    if (parameter) {
      values[0] = parameter->values[0];
      values[1] = parameter->values[1];
      values[2] = parameter->values[2];
    }
    return values;
  };
  const auto vector4OrZero = [&](const std::uint32_t slot) {
    std::array<float, 4> values{0.0F, 0.0F, 0.0F, 0.0F};
    const auto *parameter = ConsumeTextExecutionParameter(
        node, Kind::MaterialVector4, slot, stableUnitId);
    if (parameter) {
      values[0] = parameter->values[0];
      values[1] = parameter->values[1];
      values[2] = parameter->values[2];
      values[3] = parameter->values[3];
    }
    return values;
  };
  sk_sp<SkImage> image;
  SkIRect outputRasterBounds = raster.bounds;
  switch (node.capability) {
  case Capability::MaterialSeparableGaussianBlur:
  case Capability::MaterialWeightedAxisBoxBlur: {
    const bool weighted =
        node.capability == Capability::MaterialWeightedAxisBoxBlur;
    const auto *kernel0 = !weighted ? ConsumeTextExecutionParameter(
        node, Kind::MaterialVector4, 6U, stableUnitId) : nullptr;
    const auto *kernel1 = !weighted ? ConsumeTextExecutionParameter(
        node, Kind::MaterialVector4, 7U, stableUnitId) : nullptr;
    const auto *centerWeight = !weighted ? ConsumeTextExecutionParameter(
        node, Kind::MaterialScalar, 8U, stableUnitId) : nullptr;
    const bool custom = kernel0 || kernel1 || centerWeight;
    if (custom && (!kernel0 || !kernel1 || !centerWeight)) {
      error = "text separable blur custom kernel is incomplete: " + node.nodeId;
      return {};
    }
    if (custom) {
      float total = centerWeight->values.front();
      bool invalid = total < 0.0F;
      for (const auto *kernel : {kernel0, kernel1}) {
        for (const auto weight : kernel->values) {
          invalid |= weight < 0.0F;
          total += weight * 2.0F;
        }
      }
      if (invalid || total <= 0.0F) {
        error = "text separable blur kernel must have nonnegative, nonzero weights";
        return {};
      }
    }
    const bool box = weighted || custom || scalarOr(3U, 0.0F) > 0.5F;
    const float colorExponent = weighted ? 1.0F : scalarOr(9U, 1.0F);
    const float kernelGain = weighted ? 1.0F : scalarOr(10U, 1.0F);
    const float kernelOffset = weighted ? 0.0F : scalarOr(11U, 0.0F);
    const float kernelCoordinate = weighted ? 0.0F : scalarOr(12U, 0.0F);
    const bool normalizedKernel = kernelCoordinate > 0.0F;
    const float trailingSample = weighted ? 0.0F : scalarOr(15U, 0.0F);
    const bool outputSampling = !weighted && scalarOr(13U, 0.0F) == 1.0F;
    const float radius = scalar(0U);
    const auto axis = vector2OrZero(1U);
    const float axisLength = std::hypot(axis[0], axis[1]);
    const float count = normalizedKernel ? scalarOr(4U, 0.0F)
                        : custom ? 8.0F : box ? scalarOr(weighted ? 8U : 4U,
                                       weighted ? 10.0F : 6.0F)
                            : std::min(256.0F, std::ceil(radius));
    const float alpha = weighted ? scalar(3U) : scalarOr(5U, 1.0F);
    if (radius < 0.0F || axisLength <= 0.0F || !std::isfinite(count) || count < 0.0F ||
        count > (normalizedKernel ? 1024.0F : 256.0F) ||
        (!normalizedKernel && count != std::floor(count)) ||
        colorExponent <= 0.0F || kernelCoordinate < 0.0F ||
        (normalizedKernel && box) ||
        (trailingSample != 0.0F && !normalizedKernel) ||
        (outputSampling && ((!normalizedKernel && !box) || viewportSize.width() <= 0.0F ||
                            viewportSize.height() <= 0.0F)) ||
        alpha < 0.0F || alpha > 1.0F ||
        (!box && scalar(2U) <= 0.0F)) {
      error = "text axis blur kernel is outside its finite contract: " +
              node.nodeId;
      return {};
    }
    if (normalizedKernel) {
      // Source shader macros can contain fractional support. Integer taps
      // stop at floor(count), but their Gaussian coordinate divides by the
      // unrounded value. A support below one retains only the centre tap.
      float normalization = kernelGain + kernelOffset;
      const auto taps = std::min(1024U, static_cast<unsigned>(count) +
                                          static_cast<unsigned>(trailingSample));
      for (unsigned index = 1U; index <= taps; ++index) {
        // The source's post-sample break includes one additional pair but
        // keeps count as the Gaussian denominator. At count=0 its positive
        // tap coordinate tends to infinity and the Gaussian tends to zero.
        const float distance = count > 0.0F
            ? static_cast<float>(index) / count * kernelCoordinate : 0.0F;
        const float gaussian = count > 0.0F
            ? std::exp(-0.5F * distance * distance / (scalar(2U) * scalar(2U)))
            : 0.0F;
        normalization += 2.0F * (gaussian * kernelGain + kernelOffset);
      }
      if (!std::isfinite(normalization) || normalization <= 0.0F) {
        error = "text normalized blur kernel has invalid total weight: " + node.nodeId;
        return {};
      }
    } else if (kernelGain != 1.0F || kernelOffset != 0.0F) {
      error = "text blur weight transform requires a normalized kernel: " + node.nodeId;
      return {};
    }
    const float stride = normalizedKernel ? radius
                        : box ? radius * (weighted ? scalar(2U) : 1.0F)
                             : std::max(1.0F, radius / 256.0F);
    auto step = referenceVectorToRaster(SkVector::Make(
        stride * axis[0] / axisLength, -stride * axis[1] / axisLength));
    if (outputSampling) {
      const auto outputStep = ResolveTextOutputTexelStep(
          SkVector::Make(axis[0], axis[1]), stride, viewportSize,
          SkSize::Make(static_cast<float>(raster.image->width()),
                       static_cast<float>(raster.image->height())));
      if (!outputStep) {
        error = "text axis blur output sampling domain is invalid";
        return {};
      }
      step = *outputStep;
    }
    float visibilityAxis = -1.0F;
    float visibilityMid = 0.0F;
    const bool restore = weighted && scalarOr(7U, 0.0F) > 0.5F;
    if (weighted) {
      const auto *screen = ConsumeTextExecutionParameter(
          node, Kind::MaterialVector2, 4U, stableUnitId);
      const auto *sdf = ConsumeTextExecutionParameter(
          node, Kind::MaterialVector2, 5U, stableUnitId);
      if ((screen != nullptr) != (sdf != nullptr)) {
        error = "text weighted blur requires both screen and SDF sizes: " +
                node.nodeId;
        return {};
      }
      if (screen) {
        if (screen->values[0] <= 0.0F || screen->values[1] <= 0.0F ||
            sdf->values[0] <= 0.0F || sdf->values[1] <= 0.0F) {
          error = "text weighted blur sizes must be positive: " + node.nodeId;
          return {};
        }
        visibilityAxis = scalarOr(6U, 0.0F);
        if (visibilityAxis != 0.0F && visibilityAxis != 1.0F) {
          error = "text weighted blur visibility axis must be horizontal or vertical";
          return {};
        }
        const auto component = visibilityAxis < 0.5F ? 1U : 0U;
        visibilityMid = sdf->values[component] /
                        screen->values[component] * 0.45F;
        step.set(stride / screen->values[0] * axis[0] / axisLength *
                     static_cast<float>(raster.image->width()),
                 -stride / screen->values[1] * axis[1] / axisLength *
                     static_cast<float>(raster.image->height()));
      }
    }
    sk_sp<SkImage> original = raster.image;
    if (restore && !materializeOrderedInput(1U, original))
      return {};
    image = ExecuteExecutionMaterialProgram(
        raster.image, gpuContext,
        GetTextRuntimeProgram(TextRuntimeShader::MaterialAxisBlur),
        [&](SkRuntimeEffectBuilder &builder) {
          builder.child("originalTexture") = rawInputShader(original);
          builder.uniform("textureSize") = std::array<float, 2>{
              static_cast<float>(raster.image->width()),
              static_cast<float>(raster.image->height())};
          builder.uniform("sampleStep") =
              std::array<float, 2>{step.x(), step.y()};
          builder.uniform("sampleCount") = count;
          builder.uniform("gaussianSigma") = box ? 0.0F
              : normalizedKernel ? scalar(2U) : scalar(2U) / stride;
          builder.uniform("colorExponent") = colorExponent;
          builder.uniform("kernelCoordinate") = kernelCoordinate;
          builder.uniform("kernelGain") = kernelGain;
          builder.uniform("kernelOffset") = kernelOffset;
          builder.uniform("sumWeights") = weighted ? 0.0F : scalarOr(14U, 0.0F);
          builder.uniform("trailingSample") = trailingSample;
          builder.uniform("customKernel") = custom ? 1.0F : 0.0F;
          builder.uniform("centerWeight") =
              custom ? centerWeight->values.front() : 1.0F;
          builder.uniform("kernel0") = vector4OrZero(6U);
          builder.uniform("kernel1") = vector4OrZero(7U);
          builder.uniform("visibilityAxis") = visibilityAxis;
          builder.uniform("visibilityMid") = visibilityMid;
          builder.uniform("restoreOriginal") = restore ? 1.0F : 0.0F;
          builder.uniform("opacity") = alpha;
        },
        error);
    break;
  }
  case Capability::MaterialAlphaModulate: {
    const float alpha = scalar(0U);
    image = ExecuteExecutionMaterialProgram(
        raster.image, gpuContext,
        GetTextRuntimeProgram(TextRuntimeShader::MaterialModulate),
        [&](SkRuntimeEffectBuilder &builder) {
          builder.uniform("modulation") =
              std::array<float, 4>{alpha, alpha, alpha, alpha};
        },
        error);
    break;
  }
  case Capability::MaterialColorModulate: {
    const auto *color =
        ConsumeTextExecutionParameter(node, Kind::MaterialVector4, 0U,
                                       stableUnitId);
    const float alpha = color->values[3];
    image = ExecuteExecutionMaterialProgram(
        raster.image, gpuContext,
        GetTextRuntimeProgram(TextRuntimeShader::MaterialModulate),
        [&](SkRuntimeEffectBuilder &builder) {
          builder.uniform("modulation") = std::array<float, 4>{
              color->values[0] * alpha, color->values[1] * alpha,
              color->values[2] * alpha, alpha};
        },
        error);
    break;
  }
  case Capability::MaterialThresholdRevealBlur: {
    const float appear = scalar(0U);
    const float blurRadius = scalar(1U);
    auto horizontal = ExecuteExecutionMaterialProgram(
        raster.image, gpuContext,
        GetTextRuntimeProgram(TextRuntimeShader::MaterialGaussian),
        [&](SkRuntimeEffectBuilder &builder) {
          builder.uniform("direction") = std::array<float, 2>{1.0F, 0.0F};
          builder.uniform("sampleStep") = blurRadius;
          builder.uniform("opacity") = appear;
        },
        error);
    if (!horizontal)
      return {};
    image = ExecuteExecutionMaterialProgram(
        horizontal, gpuContext,
        GetTextRuntimeProgram(TextRuntimeShader::MaterialGaussian),
        [&](SkRuntimeEffectBuilder &builder) {
          builder.uniform("direction") = std::array<float, 2>{0.0F, 1.0F};
          builder.uniform("sampleStep") = blurRadius;
          builder.uniform("opacity") = 1.0F;
        },
        error);
    break;
  }
  case Capability::MaterialThresholdTransitionBlur: {
    const auto *direction =
        ConsumeTextExecutionParameter(node, Kind::MaterialVector2, 0U,
                                       stableUnitId);
    image = ExecuteExecutionMaterialProgram(
        raster.image, gpuContext,
        GetTextRuntimeProgram(TextRuntimeShader::MaterialTransition),
        [&](SkRuntimeEffectBuilder &builder) {
          builder.uniform("textureSize") =
              std::array<float, 2>{static_cast<float>(raster.image->width()),
                                   static_cast<float>(raster.image->height())};
          builder.uniform("direction") = std::array<float, 2>{
              direction->values[0], direction->values[1]};
          builder.uniform("blurStep") = scalar(3U);
          builder.uniform("percent") = scalar(0U);
          builder.uniform("transition") = scalar(1U);
          builder.uniform("opacity") = scalar(2U);
        },
        error);
    break;
  }
  case Capability::MaterialGlyphUvBallisticEchoComposite: {
    const auto viewport = ResolveExecutionMaterialViewport(
        raster, pageDomain, viewportSize, error);
    if (!viewport) return {};
    image = ExecuteExecutionMaterialProgram(
        raster.image, gpuContext,
        GetTextRuntimeProgram(TextRuntimeShader::MaterialBallistic),
        [&](SkRuntimeEffectBuilder &builder) {
          builder.uniform("textureSize") =
              std::array<float, 2>{static_cast<float>(raster.image->width()),
                                   static_cast<float>(raster.image->height())};
          builder.uniform("iTime") = scalar(0U);
          builder.uniform("outputSize") = std::array<float, 2>{
              static_cast<float>(viewport->size.width()),
              static_cast<float>(viewport->size.height())};
          builder.uniform("textOrigin") = viewport->textOrigin;
          builder.uniform("textExtent") = viewport->textExtent;
        },
        error, viewport->size);
    if (!image) return {};
    return RecordExecutionMaterialViewport(image, *viewport);
  }
  case Capability::MaterialDirectionalBoxBlur: {
    const auto *direction =
        ConsumeTextExecutionParameter(node, Kind::MaterialVector2, 1U,
                                       stableUnitId);
    const float directionLength =
        std::hypot(direction->values[0], direction->values[1]);
    SkVector sampleStep = SkVector::Make(0.0F, 0.0F);
    const auto *screenSize =
        ConsumeTextExecutionParameter(node, Kind::MaterialVector2, 2U,
                                       stableUnitId);
    if (screenSize && (screenSize->values[0] <= 0.0F ||
                       screenSize->values[1] <= 0.0F)) {
      error = "text directional blur screen size must be positive";
      return {};
    }
    if (directionLength > 0.0F) {
      const float normalizedX = direction->values[0] / directionLength;
      const float normalizedY = direction->values[1] / directionLength;
      if (screenSize) {
        // The source step is normalized by its authored screenSize, which
        // differs from the padded attachment. Both uniforms share one space.
        sampleStep.set(scalar(0U) / screenSize->values[0] * normalizedX *
                           static_cast<float>(raster.image->width()),
                       -scalar(0U) / screenSize->values[1] * normalizedY *
                           static_cast<float>(raster.image->height()));
      } else if (pageDomain) {
        sampleStep = pageDomain->authoredToDevice.mapVector(
            SkVector::Make(scalar(0U) * normalizedX,
                           -scalar(0U) * normalizedY));
        sampleStep.set(sampleStep.x() * pageDomain->rasterScaleX,
                       sampleStep.y() * pageDomain->rasterScaleY);
      } else {
        sampleStep.set(
            scalar(0U) * normalizedX * referenceToExecutionScaleX,
            -scalar(0U) * normalizedY * referenceToExecutionScaleY);
      }
    }
    return RecordExecutionMaterialProgram(
        raster, recordingBounds,
        GetTextRuntimeProgram(TextRuntimeShader::MaterialDirectionalBoxBlur),
        [&](SkRuntimeEffectBuilder &builder) {
          builder.uniform("sampleStep") =
              std::array<float, 2>{sampleStep.x(), sampleStep.y()};
        },
        error);
  }
  case Capability::MaterialMaskedCutLine: {
    const auto viewport = ResolveExecutionMaterialViewport(
        raster, pageDomain, viewportSize, error);
    if (!viewport) return {};
    sk_sp<SkImage> maskImage;
    if (!materializeOrderedInput(1U, maskImage))
      return {};
    const auto authoredRtSize = vector2OrZero(3U);
    image = ExecuteExecutionMaterialProgram(
        raster.image, gpuContext,
        GetTextRuntimeProgram(TextRuntimeShader::MaterialMaskedCutLine),
        [&](SkRuntimeEffectBuilder &builder) {
          builder.child("maskTexture") = rawInputShader(maskImage);
          builder.uniform("textureSize") = std::array<float, 2>{
              static_cast<float>(raster.image->width()),
              static_cast<float>(raster.image->height())};
          builder.uniform("authoredRtSize") = authoredRtSize;
          builder.uniform("textOrigin") = viewport->textOrigin;
          builder.uniform("textExtent") = viewport->textExtent;
          builder.uniform("cutPosition") = scalar(0U);
          builder.uniform("angleRadians") = scalar(1U);
          builder.uniform("lineHalfWidth") = scalar(2U);
          builder.uniform("maskUvScale") = scalar(4U);
          builder.uniform("outsideMaskStrength") = scalar(5U);
          builder.uniform("falloffPower") = scalar(6U);
        },
        error, viewport->size);
    if (!image) return {};
    return RecordExecutionMaterialViewport(image, *viewport);
  }
  case Capability::MaterialCutLineReveal: {
    const auto viewport = ResolveExecutionMaterialViewport(
        raster, pageDomain, viewportSize, error);
    if (!viewport) return {};
    image = ExecuteExecutionMaterialProgram(
        raster.image, gpuContext,
        GetTextRuntimeProgram(TextRuntimeShader::MaterialCutLineReveal),
        [&](SkRuntimeEffectBuilder &builder) {
          builder.uniform("textureSize") =
              std::array<float, 2>{static_cast<float>(raster.image->width()),
                                   static_cast<float>(raster.image->height())};
          builder.uniform("cutPosition") = scalar(0U);
          builder.uniform("textOrigin") = viewport->textOrigin;
          builder.uniform("textExtent") = viewport->textExtent;
          builder.uniform("angleRadians") = scalar(1U);
          builder.uniform("lineWidth") = scalar(2U);
          builder.uniform("tail") = scalar(3U);
          builder.uniform("falloffPower") = scalar(4U);
        },
        error, viewport->size);
    if (!image) return {};
    return RecordExecutionMaterialViewport(image, *viewport);
  }
  case Capability::MaterialPackedMultiscaleGaussianBlur: {
    const auto axis = vector2OrZero(1U);
    const float axisLength = std::hypot(axis[0], axis[1]);
    const float count = scalar(0U);
    if (count < 0.0F || count > 1024.0F || count != std::floor(count) ||
        axisLength <= 0.0F || scalar(2U) < 0.0F || scalar(3U) <= 0.0F ||
        scalar(4U) <= 0.0F || (scalar(5U) != 0.0F && scalar(5U) != 1.0F)) {
      error = "text packed blur requires an integer count, positive kernel and valid alpha mode";
      return {};
    }
    const auto rasterAxis = referenceVectorToRaster(
        SkVector::Make(axis[0] / axisLength, axis[1] / axisLength));
    if (rasterAxis.length() <= 0.0F) {
      error = "text packed blur axis collapses in the attachment domain";
      return {};
    }
    auto step = SkVector::Make(rasterAxis.x() * scalar(2U),
                               rasterAxis.y() * scalar(2U));
    if (scalarOr(6U, 0.0F) == 1.0F) {
      const auto outputStep = ResolveTextOutputTexelStep(
          SkVector::Make(axis[0], axis[1]), scalar(2U), viewportSize,
          SkSize::Make(static_cast<float>(raster.image->width()),
                       static_cast<float>(raster.image->height())));
      if (!outputStep) {
        error = "text packed blur output sampling domain is invalid";
        return {};
      }
      step = *outputStep;
    }
    image = ExecuteExecutionMaterialProgram(
        raster.image, gpuContext,
        GetTextRuntimeProgram(TextRuntimeShader::MaterialPackedGaussian),
        [&](SkRuntimeEffectBuilder &builder) {
          builder.uniform("sampleCount") = count;
          builder.uniform("sampleStep") = std::array<float, 2>{step.x(), step.y()};
          builder.uniform("sigma") = scalar(3U);
          builder.uniform("gamma") = scalar(4U);
          builder.uniform("alphaInput") = scalar(5U);
        },
        error);
    break;
  }
  case Capability::MaterialDualPackedGlowComposite: {
    sk_sp<SkImage> packedGlow0;
    sk_sp<SkImage> packedGlow1;
    sk_sp<SkImage> cutImage;
    if (!materializeOrderedInput(1U, packedGlow0) ||
        !materializeOrderedInput(2U, packedGlow1) ||
        !materializeOrderedInput(3U, cutImage)) {
      return {};
    }
    const auto textColor = vector4OrZero(1U);
    const auto letterColor = vector3OrZero(2U);
    const auto whiteTint = vector4OrZero(4U);
    image = ExecuteExecutionMaterialProgram(
        raster.image, gpuContext,
        GetTextRuntimeProgram(TextRuntimeShader::MaterialDualPackedGlow),
        [&](SkRuntimeEffectBuilder &builder) {
          builder.child("packedGlow0Texture") = rawInputShader(packedGlow0);
          builder.child("packedGlow1Texture") = rawInputShader(packedGlow1);
          builder.child("cutTexture") = rawInputShader(cutImage);
          builder.uniform("glowIntensity") = scalar(0U);
          builder.uniform("textColor") = textColor;
          builder.uniform("letterColor") = letterColor;
          builder.uniform("gamma") = scalar(3U);
          builder.uniform("whiteTint") = whiteTint;
          builder.uniform("screenIterations") = scalar(5U);
        },
        error);
    break;
  }
  case Capability::MaterialEncodedAlphaDistanceBlur: {
    const auto axis = vector2OrZero(1U);
    image = ExecuteExecutionMaterialProgram(
        raster.image, gpuContext,
        GetTextRuntimeProgram(TextRuntimeShader::MaterialEncodedAlphaBlur),
        [&](SkRuntimeEffectBuilder &builder) {
          builder.uniform("textureSize") =
              std::array<float, 2>{static_cast<float>(raster.image->width()),
                                   static_cast<float>(raster.image->height())};
          builder.uniform("sampleCount") = scalar(0U);
          builder.uniform("axis") = axis;
          builder.uniform("sigma") = scalar(2U);
          builder.uniform("transferGamma") = scalar(3U);
          builder.uniform("encodedInput") = scalar(4U);
          builder.uniform("encodeOutput") = scalar(5U);
        },
        error);
    break;
  }
  case Capability::MaterialSdfNormalLightSweep: {
    sk_sp<SkImage> originalImage;
    if (!materializeOrderedInput(1U, originalImage))
      return {};
    const auto lightPosition = vector3OrZero(0U);
    const auto eyePosition = vector3OrZero(1U);
    const auto textPosition = vector2OrZero(5U);
    const auto lightColor = vector4OrZero(7U);
    image = ExecuteExecutionMaterialProgram(
        raster.image, gpuContext,
        GetTextRuntimeProgram(TextRuntimeShader::MaterialSdfNormalSweep),
        [&](SkRuntimeEffectBuilder &builder) {
          builder.child("originalTexture") = rawInputShader(originalImage);
          builder.uniform("textureSize") = std::array<float, 2>{
              static_cast<float>(raster.image->width()),
              static_cast<float>(raster.image->height())};
          builder.uniform("lightPosition") = lightPosition;
          builder.uniform("eyePosition") = eyePosition;
          builder.uniform("shininess") = scalar(2U);
          builder.uniform("fontScale") = scalar(3U);
          builder.uniform("bandWidth") = scalar(4U);
          builder.uniform("textPosition") = textPosition;
          builder.uniform("textRotationDegrees") = scalar(6U);
          builder.uniform("lightColor") = lightColor;
        },
        error);
    break;
  }
  case Capability::MaterialLightSweepPackedGlowComposite: {
    sk_sp<SkImage> packedGlow0;
    sk_sp<SkImage> packedGlow1;
    sk_sp<SkImage> colorBlur;
    if (!materializeOrderedInput(1U, packedGlow0) ||
        !materializeOrderedInput(2U, packedGlow1) ||
        !materializeOrderedInput(3U, colorBlur)) {
      return {};
    }
    image = ExecuteExecutionMaterialProgram(
        raster.image, gpuContext,
        GetTextRuntimeProgram(TextRuntimeShader::MaterialLightSweepGlow),
        [&](SkRuntimeEffectBuilder &builder) {
          builder.child("packedGlow0Texture") = rawInputShader(packedGlow0);
          builder.child("packedGlow1Texture") = rawInputShader(packedGlow1);
          builder.child("colorBlurTexture") = rawInputShader(colorBlur);
          builder.uniform("glowIntensity") = scalar(0U);
          builder.uniform("packedScale") = scalar(1U);
          builder.uniform("gamma") = scalar(2U);
          builder.uniform("screenIterations") = scalar(3U);
        },
        error);
    break;
  }
  case Capability::MaterialRadialDecayHsvGlow: {
    const auto center = vector2OrZero(1U);
    image = ExecuteExecutionMaterialProgram(
        raster.image, gpuContext,
        GetTextRuntimeProgram(TextRuntimeShader::MaterialRadialDecayHsv),
        [&](SkRuntimeEffectBuilder &builder) {
          builder.uniform("textureSize") =
              std::array<float, 2>{static_cast<float>(raster.image->width()),
                                   static_cast<float>(raster.image->height())};
          builder.uniform("strength") = scalar(0U);
          builder.uniform("center") = center;
        },
        error);
    break;
  }
  case Capability::MaterialFixedNineTapAxisBlur: {
    const auto axis = vector2OrZero(1U);
    image = ExecuteExecutionMaterialProgram(
        raster.image, gpuContext,
        GetTextRuntimeProgram(TextRuntimeShader::MaterialFixedNineTap),
        [&](SkRuntimeEffectBuilder &builder) {
          builder.uniform("blurSize") = scalar(0U);
          builder.uniform("axis") = axis;
        },
        error);
    break;
  }
  case Capability::MaterialHsvOriginalOverBlur: {
    sk_sp<SkImage> blurImage;
    if (!materializeOrderedInput(1U, blurImage))
      return {};
    image = ExecuteExecutionMaterialProgram(
        raster.image, gpuContext,
        GetTextRuntimeProgram(TextRuntimeShader::MaterialHsvOriginalOver),
        [&](SkRuntimeEffectBuilder &builder) {
          builder.child("blurTexture") = rawInputShader(blurImage);
        },
        error);
    break;
  }
  case Capability::MaterialNoiseThresholdDissolve: {
    sk_sp<SkImage> noiseImage;
    if (!materializeOrderedInput(1U, noiseImage))
      return {};
    const auto noiseScale = vector2OrZero(0U);
    return RecordExecutionMaterialProgram(
        raster, recordingBounds,
        GetTextRuntimeProgram(TextRuntimeShader::MaterialNoiseDissolve),
        [&](SkRuntimeEffectBuilder &builder) {
          builder.child("noiseTexture") = rawInputShader(noiseImage);
          builder.uniform("textureSize") = std::array<float, 2>{
              static_cast<float>(raster.image->width()),
              static_cast<float>(raster.image->height())};
          builder.uniform("noiseScale") = noiseScale;
          builder.uniform("burnAmount") = scalar(0U);
          builder.uniform("feather") = scalar(1U);
        },
        error);
  }
  case Capability::MaterialVatRbdMesh: {
    constexpr std::array<text::TextEffectResourceKind, 3U> requiredKinds{
        text::TextEffectResourceKind::Mesh,
        text::TextEffectResourceKind::FloatTexture,
        text::TextEffectResourceKind::FloatTexture};
    constexpr std::array<std::string_view, 3U> requiredMediaTypes{
        "application/vnd.videocut.mesh",
        "application/vnd.videocut.float-texture",
        "application/vnd.videocut.float-texture"};
    if (node.resourceIds.size() != requiredKinds.size()) {
      error = "text VAT material requires one mesh and two float textures: " +
              node.nodeId;
      return {};
    }
    std::array<std::shared_ptr<const std::vector<std::uint8_t>>, 3U>
        residentResources;
    for (std::size_t index = 0U; index < requiredKinds.size(); ++index) {
      const auto sample = std::find_if(
          framePlan.resources.begin(), framePlan.resources.end(),
          [&](const auto &candidate) {
            return candidate.resourceId == node.resourceIds[index];
          });
      if (sample == framePlan.resources.end() ||
          sample->kind != requiredKinds[index] ||
          sample->mediaType != requiredMediaTypes[index]) {
        error = "text VAT material resource binding is incomplete: " +
                node.nodeId;
        return {};
      }
      std::string identity;
      std::string mediaType;
      if (!assets.AdmitOpaque(
              sample->assetId, sample->digest,
              RuntimeKindForFrameResource(sample->kind),
              ProjectKindForFrameResource(sample->kind), identity, error,
              &residentResources[index], &mediaType, false) ||
          mediaType != requiredMediaTypes[index]) {
        if (error.empty())
          error = "text VAT material resident resource type is invalid";
        return {};
      }
    }
    ResidentFloatTexture positionTexture;
    ResidentFloatTexture rotationTexture;
    ResidentVatMesh mesh;
    if (!DecodeResidentVatMesh(residentResources[0U], mesh, error) ||
        !DecodeResidentFloatTexture(residentResources[1U], positionTexture,
                                    error) ||
        !DecodeResidentFloatTexture(residentResources[2U], rotationTexture,
                                    error)) {
      return {};
    }
    const auto authoredTextSize = vector2OrZero(4U);
    if (authoredTextSize[0] <= 0.0F || authoredTextSize[1] <= 0.0F) {
      error = "text VAT material text extent is invalid: " + node.nodeId;
      return {};
    }
    // Qt drives the VAT model scale and UV aspect from the guarded SDF
    // render-to-texture attachment, not from the unpadded layout textRect.
    // ResolveSdfSourceAttachmentDomain created that attachment once; retain
    // its integer extent through the scene instead of projecting slot 4 a
    // second time as if it were the downstream texture size.
    const std::array<float, 2U> textSize =
        pageDomain && pageDomain->targetWidth > 0 &&
                pageDomain->targetHeight > 0
            ? std::array<float, 2U>{
                  static_cast<float>(pageDomain->targetWidth),
                  static_cast<float>(pageDomain->targetHeight)}
            : std::array<float, 2U>{
                  authoredTextSize[0] * referenceToExecutionScaleX,
                  authoredTextSize[1] * referenceToExecutionScaleY};
    recordingBounds.roundOut(&outputRasterBounds);
    if (outputRasterBounds.isEmpty()) {
      error = "text VAT material scene target is empty: " + node.nodeId;
      return {};
    }
    const auto textBounds = SkRect::MakeXYWH(
        (static_cast<float>(outputRasterBounds.left()) +
         static_cast<float>(outputRasterBounds.right())) * 0.5F -
            textSize[0] * 0.5F,
        (static_cast<float>(outputRasterBounds.top()) +
         static_cast<float>(outputRasterBounds.bottom())) * 0.5F -
            textSize[1] * 0.5F,
        textSize[0], textSize[1]);
    image = ExecuteResidentVatRbdMesh(
        raster.image, gpuContext, mesh, positionTexture, rotationTexture,
        outputRasterBounds, textBounds, textSize, scalar(0U), scalar(1U),
        scalar(2U), scalar(5U), scalar(6U), scalar(7U),
        scalar(8U), scalar(9U), scalar(10U), vector3OrZero(11U),
        vector3OrZero(12U), scalar(13U), scalar(14U), error);
    break;
  }
  case Capability::MaterialSpiralSdfWarp: {
    const auto letterSize = vector2OrZero(2U);
    const auto *canvasSize = ConsumeTextExecutionParameter(
        node, Kind::MaterialVector2, 3U, stableUnitId);
    if (letterSize[0] <= 0.0F || letterSize[1] <= 0.0F ||
        (canvasSize && (canvasSize->values[0] <= 0.0F ||
                        canvasSize->values[1] <= 0.0F))) {
      error = "text spiral warp requires positive letter and canvas sizes: " + node.nodeId;
      return {};
    }
    const std::array<float, 2> ratio = canvasSize
        ? std::array<float, 2>{canvasSize->values[0] / letterSize[0],
                               canvasSize->values[1] / letterSize[1]}
        : std::array<float, 2>{1.0F, 1.0F};
    image = raster.image;
    for (unsigned pass = 0; pass < 3U; ++pass) {
      image = ExecuteExecutionMaterialProgram(
          image, gpuContext,
          GetTextRuntimeProgram(TextRuntimeShader::MaterialSpiralWarp),
          [&](SkRuntimeEffectBuilder &builder) {
            builder.uniform("textureSize") = std::array<float, 2>{
                static_cast<float>(raster.image->width()),
                static_cast<float>(raster.image->height())};
            builder.uniform("lineToLetterRatio") = ratio;
            builder.uniform("phase") = scalar(0U);
            builder.uniform("quantizedTime") = scalar(1U);
            builder.uniform("passIndex") = static_cast<float>(pass);
          },
          error);
      if (!image)
        return {};
    }
    break;
  }
  case Capability::MaterialFluxTurbulentBlend: {
    const float mode = scalar(11U);
    const float flowScale = scalarOr(1U, 1.0F);
    const float noiseSize = scalarOr(8U, 1.0F);
    const auto exposure = vector3OrZero(12U);
    const auto outsideIntensity = vector2OrZero(14U);
    if (viewportSize.width() <= 0.0F || viewportSize.height() <= 0.0F ||
        (mode != 2.0F && flowScale == 0.0F) ||
        (mode == 1.0F && noiseSize <= 0.0F) ||
        (mode == 2.0F && (scalar(6U) < 0.0F || outsideIntensity[0] < 0.0F ||
                         outsideIntensity[1] < 0.0F ||
                         exposure[0] <= 0.0F || exposure[1] <= 0.0F ||
                         exposure[2] <= 0.0F))) {
      error = "text flux material divisor or bloom power is invalid: " + node.nodeId;
      return {};
    }
    sk_sp<SkImage> bloom0 = raster.image;
    sk_sp<SkImage> bloom1 = raster.image;
    if (mode == 2.0F && (!materializeOrderedInput(1U, bloom0) ||
                         !materializeOrderedInput(2U, bloom1)))
      return {};
    const auto background = vector4OrZero(5U);
    image = ExecuteExecutionMaterialProgram(
        raster.image, gpuContext,
        GetTextRuntimeProgram(TextRuntimeShader::MaterialFlux),
        [&](SkRuntimeEffectBuilder &builder) {
          builder.uniform("viewportSize") = std::array<float, 2>{
              viewportSize.width(), viewportSize.height()};
          builder.child("bloom0Texture") = rawInputShader(bloom0);
          builder.child("bloom1Texture") = rawInputShader(bloom1);
          builder.uniform("textureSize") = std::array<float, 2>{
              static_cast<float>(raster.image->width()),
              static_cast<float>(raster.image->height())};
          builder.uniform("offset") = vector2OrZero(2U);
          builder.uniform("scale") = flowScale;
          builder.uniform("time") = scalarOrZero(0U);
          builder.uniform("intensity") = scalarOrZero(7U);
          builder.uniform("alpha") = scalarOrZero(3U);
          builder.uniform("backgroundColor") = std::array<float, 3>{
              background[0] * background[3], background[1] * background[3],
              background[2] * background[3]};
          builder.uniform("angle") = scalarOrZero(4U);
          builder.uniform("displacement") = scalarOrZero(9U);
          builder.uniform("noiseSize") = noiseSize;
          builder.uniform("passIndex") = mode;
          builder.uniform("glowIntensity") = std::array<float, 3>{
              outsideIntensity[0], scalarOrZero(6U), outsideIntensity[1]};
          builder.uniform("glowExposure") = exposure;
        },
        error);
    break;
  }
  case Capability::MaterialLineColumnNoiseTrail: {
    const auto textSize = vector2OrZero(3U);
    if (textSize[0] <= 0.0F || textSize[1] <= 0.0F ||
        scalar(1U) <= 0.0F || scalar(2U) == 0.0F) {
      error = "text line/column trail requires positive geometry and nonzero parent scale";
      return {};
    }
    const auto viewport = ResolveExecutionMaterialViewport(
        raster, pageDomain, viewportSize, error);
    if (!viewport) return {};
    // The source vertex shader draws the entire viewport while deriving a
    // separate UV from the text quad and inverse model. Subsequent passes
    // sample their fullscreen attachments in viewport UV, not in text UV.
    sk_sp<SkImage> curve;
    if (!materializeOrderedInput(1U, curve)) return {};
    image = raster.image;
    for (unsigned pass = 0U; pass < 3U; ++pass) {
      const auto inputSize = image->dimensions();
      image = ExecuteExecutionMaterialProgram(
          image, gpuContext,
          GetTextRuntimeProgram(TextRuntimeShader::MaterialLineColumn),
          [&](SkRuntimeEffectBuilder &builder) {
            builder.child("curveTexture") = rawInputShader(curve);
            builder.uniform("textureSize") = std::array<float, 2>{
                static_cast<float>(inputSize.width()),
                static_cast<float>(inputSize.height())};
            builder.uniform("outputSize") = std::array<float, 2>{
                static_cast<float>(viewport->size.width()),
                static_cast<float>(viewport->size.height())};
            builder.uniform("textOrigin") = viewport->textOrigin;
            builder.uniform("textExtent") = viewport->textExtent;
            builder.uniform("curveSize") = std::array<float, 2>{
                static_cast<float>(curve->width()), static_cast<float>(curve->height())};
            builder.uniform("textSize") = textSize;
            builder.uniform("progress") = scalar(0U);
            builder.uniform("minimumLineColumn") = scalar(1U);
            builder.uniform("parentScale") = scalar(2U);
            builder.uniform("passIndex") = static_cast<float>(pass);
          },
          error, viewport->size);
      if (!image) return {};
    }
    return RecordExecutionMaterialViewport(image, *viewport);
  }
  case Capability::MaterialMultimeshTextComposite: {
    const float mode = scalar(7U);
    const float weaken = scalarOr(0U, 0.0F);
    const float opacity = scalarOr(1U, 1.0F);
    auto scale = vector2OrZero(3U);
    if (mode == 0.0F) scale = {1.0F, 1.0F};
    if (weaken < 0.0F || weaken > 1.0F || opacity < 0.0F || opacity > 1.0F ||
        scale[0] == 0.0F || scale[1] == 0.0F) {
      error = "text multimesh gain or scale is invalid: " + node.nodeId;
      return {};
    }
    sk_sp<SkImage> history;
    if (!materializeOrderedInput(1U, history)) return {};
    sk_sp<SkImage> color = raster.image;
    sk_sp<SkImage> defaultColor = raster.image;
    if (mode == 1.0F) {
      // LUTs are data textures, not camera attachments. Sampling their original
      // images avoids Page fitting, presentation clipping and a second filter.
      const auto sampleLut = [&](const std::size_t index,
                                 sk_sp<SkImage> &lut) {
        const auto sampled = sampledMediaInputs.find(node.inputIds[index]);
        if (sampled == sampledMediaInputs.end())
          return materializeOrderedInput(index, lut);
        lut = sampled->second;
        return static_cast<bool>(lut);
      };
      if (!sampleLut(2U, color) || !sampleLut(3U, defaultColor)) return {};
    }
    image = ExecuteExecutionMaterialProgram(
        raster.image, gpuContext,
        GetTextRuntimeProgram(TextRuntimeShader::MaterialMultimesh),
        [&](SkRuntimeEffectBuilder &builder) {
          builder.child("historyTexture") = rawInputShader(history);
          builder.child("colorTexture") = rawInputShader(color);
          builder.child("defaultColorTexture") = rawInputShader(defaultColor);
          builder.uniform("textureSize") = std::array<float, 2>{
              static_cast<float>(raster.image->width()),
              static_cast<float>(raster.image->height())};
          builder.uniform("colorSize") = std::array<float, 2>{
              static_cast<float>(color->width()), static_cast<float>(color->height())};
          builder.uniform("defaultColorSize") = std::array<float, 2>{
              static_cast<float>(defaultColor->width()),
              static_cast<float>(defaultColor->height())};
          builder.uniform("passIndex") = mode;
          builder.uniform("weaken") = weaken;
          builder.uniform("opacity") = opacity;
          builder.uniform("translation") = vector2OrZero(2U);
          builder.uniform("scale") = scale;
          builder.uniform("characterColor") = vector4OrZero(6U);
        },
        error);
    break;
  }
  case Capability::MaterialDeepGlowComposite: {
    const float mode = scalarOr(5U, 0.0F);
    if (mode == 1.0F || mode == 2.0F) {
      std::array<sk_sp<SkImage>, 7> layers;
      for (std::size_t index = 0U; index < layers.size(); ++index)
        if (!materializeOrderedInput(index + 1U, layers[index])) return {};
      sk_sp<SkImage> depth = raster.image;
      if (mode == 2.0F && !materializeOrderedInput(8U, depth)) return {};
      image = ExecuteExecutionMaterialProgram(
          raster.image, gpuContext,
          GetTextRuntimeProgram(TextRuntimeShader::MaterialDeepGlowScreen),
          [&](SkRuntimeEffectBuilder &builder) {
            constexpr std::array<const char *, 7> children{
                "glowTexture1", "glowTexture2", "glowTexture3", "glowTexture4",
                "glowTexture5", "glowTexture6", "glowTexture7"};
            for (std::size_t index = 0U; index < layers.size(); ++index)
              builder.child(children[index]) = rawInputShader(layers[index]);
            builder.child("depthTexture") = rawInputShader(depth);
            builder.uniform("intensity") = scalar(0U);
            builder.uniform("colorExponent") = scalar(1U);
            builder.uniform("useDepth") = mode == 2.0F ? 1.0F : 0.0F;
            builder.uniform("tint") = mode == 2.0F ? vector4OrZero(6U)
                : std::array<float, 4>{1.0F, 1.0F, 1.0F, 1.0F};
            builder.uniform("originalAlpha") = mode == 2.0F ? scalar(7U) : 1.0F;
            builder.uniform("outputAlpha") = mode == 2.0F ? scalar(8U) : 1.0F;
          },
          error);
      break;
    }
    const float authoredLevels = scalar(3U);
    if (authoredLevels != std::floor(authoredLevels) || authoredLevels > 8.0F) {
      error = "text deep glow requires between one and eight complete levels: " +
              node.nodeId;
      return {};
    }
    const auto tint = vector4OrZero(4U);
    auto thresholded = ExecuteExecutionMaterialProgram(
        raster.image, gpuContext,
        GetTextRuntimeProgram(TextRuntimeShader::MaterialGlow),
        [&](SkRuntimeEffectBuilder &builder) {
          builder.child("glowTexture") = rawInputShader(raster.image);
          builder.uniform("threshold") = scalar(1U);
          builder.uniform("intensity") = 1.0F;
          builder.uniform("tint") = tint;
          builder.uniform("composite") = 0.0F;
        },
        error);
    if (!thresholded)
      return {};
    const auto info = SkImageInfo::Make(
        raster.image->width(), raster.image->height(), kRGBA_8888_SkColorType,
        kPremul_SkAlphaType, SkColorSpace::MakeSRGB());
    auto glowTarget = gpuContext ? gpuContext->MakeSurface(info, error)
                                 : SkSurfaces::Raster(info);
    if (!glowTarget) {
      if (error.empty()) error = "text deep glow accumulation allocation failed";
      return {};
    }
    glowTarget->getCanvas()->clear(SK_ColorTRANSPARENT);
    const auto axisX = referenceVectorToRaster(SkVector::Make(scalar(2U), 0.0F));
    const auto axisY = referenceVectorToRaster(SkVector::Make(0.0F, scalar(2U)));
    const auto levels = static_cast<unsigned>(authoredLevels);
    float normalization = 0.0F;
    for (unsigned level = 1U; level <= levels; ++level)
      normalization += 1.0F / static_cast<float>(level);
    for (unsigned level = 1U; level <= levels; ++level) {
      const float fraction = static_cast<float>(level) / authoredLevels;
      SkPaint blur;
      blur.setBlendMode(SkBlendMode::kPlus);
      blur.setAlphaf(1.0F / (static_cast<float>(level) * normalization));
      blur.setImageFilter(SkImageFilters::Blur(
          axisX.length() * fraction / 3.0F, axisY.length() * fraction / 3.0F,
          SkTileMode::kDecal, nullptr));
      glowTarget->getCanvas()->drawImage(thresholded, 0.0F, 0.0F,
                                       SkSamplingOptions(), &blur);
    }
    auto glow = glowTarget->makeImageSnapshot();
    if (!glow) {
      error = "text deep glow accumulation snapshot failed";
      return {};
    }
    image = ExecuteExecutionMaterialProgram(
        raster.image, gpuContext,
        GetTextRuntimeProgram(TextRuntimeShader::MaterialGlow),
        [&](SkRuntimeEffectBuilder &builder) {
          builder.child("glowTexture") = rawInputShader(glow);
          builder.uniform("threshold") = scalar(1U);
          builder.uniform("intensity") = scalar(0U);
          builder.uniform("tint") = tint;
          builder.uniform("composite") = 1.0F;
        },
        error);
    break;
  }
  case Capability::MaterialMaskMotionNoiseGlow: {
    if (viewportSize.width() <= 0.0F || scalar(1U) < 0.0F ||
        scalar(2U) < 0.0F || scalar(4U) <= 0.0F || scalar(6U) <= 0.0F) {
      error = "text mask motion requires valid blur, mask and viewport dimensions: " +
              node.nodeId;
      return {};
    }
    const auto original = rawInputShader(raster.image);
    image = raster.image;
    for (unsigned pass = 0U; pass < 3U; ++pass) {
      image = ExecuteExecutionMaterialProgram(
          image, gpuContext,
          GetTextRuntimeProgram(TextRuntimeShader::MaterialMaskMotion),
          [&](SkRuntimeEffectBuilder &builder) {
            builder.child("originalTexture") = original;
            builder.uniform("textureSize") = std::array<float, 2>{
                static_cast<float>(raster.image->width()),
                static_cast<float>(raster.image->height())};
            builder.uniform("screenWidth") = viewportSize.width();
            builder.uniform("passIndex") = static_cast<float>(pass);
            builder.uniform("progress") = scalar(0U);
            builder.uniform("blurStride") = scalar(1U);
            builder.uniform("glowRange") = scalar(2U);
            builder.uniform("maskRight") = scalar(3U);
            builder.uniform("maskWidth") = scalar(4U);
            builder.uniform("shearStrength") = scalar(5U);
            builder.uniform("textureFixScale") = scalar(6U);
          },
          error);
      if (!image)
        return {};
    }
    break;
  }
  case Capability::MaterialFaceNoiseShadow: {
    const auto textSize = vector2OrZero(2U);
    if (textSize[0] <= 0.0F || textSize[1] <= 0.0F || scalar(5U) <= 0.0F ||
        viewportSize.width() <= 0.0F || viewportSize.height() <= 0.0F) {
      error = "text face noise requires positive text, border and viewport dimensions: " +
              node.nodeId;
      return {};
    }
    image = raster.image;
    for (unsigned pass = 0U; pass < 2U; ++pass) {
      image = ExecuteExecutionMaterialProgram(
          image, gpuContext,
          GetTextRuntimeProgram(TextRuntimeShader::MaterialFaceNoise),
          [&](SkRuntimeEffectBuilder &builder) {
            builder.uniform("textureSize") = std::array<float, 2>{
                static_cast<float>(raster.image->width()),
                static_cast<float>(raster.image->height())};
            builder.uniform("viewportSize") = std::array<float, 2>{
                viewportSize.width(), viewportSize.height()};
            builder.uniform("textSize") = textSize;
            builder.uniform("modelOffset") = vector2OrZero(3U);
            builder.uniform("progress") = scalar(0U);
            builder.uniform("progress1") = scalar(1U);
            builder.uniform("noiseRange") = scalar(4U);
            builder.uniform("border") = scalar(5U);
            builder.uniform("noiseStrength") = scalar(6U);
            builder.uniform("passIndex") = static_cast<float>(pass);
          },
          error);
      if (!image)
        return {};
    }
    break;
  }
  case Capability::MaterialShapedRampMask: {
    const auto blurRamp = vector2OrZero(0U);
    const auto alphaRamp = vector2OrZero(1U);
    const auto resolution = vector2OrZero(5U);
    if (resolution[0] <= 0.0F || resolution[1] <= 0.0F ||
        blurRamp[0] == blurRamp[1] || alphaRamp[0] == alphaRamp[1] ||
        scalar(2U) < 0.0F || scalar(2U) > 1.0F || scalar(3U) < 0.0F) {
      error = "text shaped ramp requires finite sampling and nondegenerate ramps: " +
              node.nodeId;
      return {};
    }
    image = ExecuteExecutionMaterialProgram(
        raster.image, gpuContext,
        GetTextRuntimeProgram(TextRuntimeShader::MaterialShapedRamp),
        [&](SkRuntimeEffectBuilder &builder) {
          builder.uniform("textureSize") = std::array<float, 2>{
              static_cast<float>(raster.image->width()),
              static_cast<float>(raster.image->height())};
          builder.uniform("resolution") = resolution;
          builder.uniform("blurRamp") = blurRamp;
          builder.uniform("alphaRamp") = alphaRamp;
          builder.uniform("opacity") = scalar(2U);
          builder.uniform("intensity") = scalar(3U);
          builder.uniform("brightSpotThreshold") = scalar(4U);
        },
        error);
    break;
  }
  case Capability::MaterialMaskedDualOffsetCrossfade: {
    // The authored screenSize is text.targetRTExtraSize, not the output
    // viewport. Reuse the shared source-canvas binding supplied by the IR.
    const auto sourceSize = vector4OrZero(31U);
    const float maskAspect = sourceSize[0] > 0.0F && sourceSize[1] > 0.0F
                                 ? sourceSize[0] / sourceSize[1]
                                 : viewportSize.width() / viewportSize.height();
    if (scalar(0U) == 0.0F || viewportSize.width() <= 0.0F ||
        viewportSize.height() <= 0.0F || scalar(3U) < 0.0F ||
        scalar(3U) > 1.0F || scalar(4U) < 0.0F || scalar(4U) > 1.0F) {
      error = "text dual offset requires valid mask scale, alpha and viewport: " +
              node.nodeId;
      return {};
    }
    image = ExecuteExecutionMaterialProgram(
        raster.image, gpuContext,
        GetTextRuntimeProgram(TextRuntimeShader::MaterialDualOffset),
        [&](SkRuntimeEffectBuilder &builder) {
          builder.uniform("textureSize") = std::array<float, 2>{
              static_cast<float>(raster.image->width()),
              static_cast<float>(raster.image->height())};
          builder.uniform("aspectRatio") = maskAspect;
          builder.uniform("maskScale") = scalar(0U);
          builder.uniform("rotationDegrees") = scalar(6U);
          builder.uniform("panning") = vector2OrZero(5U);
          builder.uniform("offset0") = vector2OrZero(1U);
          builder.uniform("offset1") = vector2OrZero(2U);
          builder.uniform("alpha0") = scalar(3U);
          builder.uniform("alpha1") = scalar(4U);
        },
        error);
    break;
  }
  case Capability::MaterialSdfWaveEnergy: {
    image = ExecuteExecutionMaterialProgram(
        raster.image, gpuContext,
        GetTextRuntimeProgram(TextRuntimeShader::MaterialWave),
        [&](SkRuntimeEffectBuilder &builder) {
          builder.uniform("textureSize") = std::array<float, 2>{
              static_cast<float>(raster.image->width()),
              static_cast<float>(raster.image->height())};
          builder.uniform("wave") = std::array<float, 4>{
              scalar(0U), scalar(1U), scalar(2U), scalar(3U)};
        },
        error);
    break;
  }
  case Capability::MaterialSdfAlphaOutline: {
    const auto stepUv = vector2OrZero(1U);
    const float width = scalar(0U);
    if (width < 0.0F || stepUv[0] <= 0.0F || stepUv[1] <= 0.0F) {
      error = "text alpha outline requires a nonnegative width and positive UV steps";
      return {};
    }
    std::array<float, 2> stepPixels{
        stepUv[0] * width * static_cast<float>(raster.image->width()),
        stepUv[1] * width * static_cast<float>(raster.image->height())};
    if (scalarOr(2U, 0.0F) == 1.0F) {
      // A local-text outline can be consumed after promotion to a full-frame
      // attachment. Its authored radius must follow the parent transform,
      // rather than treating the full-frame extent as the source text UV span.
      const auto x = referenceVectorToRaster(SkVector::Make(width * stepUv[0], 0.0F));
      const auto y = referenceVectorToRaster(SkVector::Make(0.0F, width * stepUv[1]));
      stepPixels = {x.length(), y.length()};
    }
    image = ExecuteExecutionMaterialProgram(
        raster.image, gpuContext,
        GetTextRuntimeProgram(TextRuntimeShader::MaterialAlphaOutline),
        [&](SkRuntimeEffectBuilder &builder) {
          builder.uniform("stepPixels") = stepPixels;
        },
        error);
    break;
  }
  case Capability::MaterialTurbulenceDisplacement: {
    const float spriteMode = scalarOr(13U, 0.0F);
    const bool flowWarp = spriteMode == 2.0F;
    const float cycle = spriteMode == 1.0F ? scalar(15U) : 0.0F;
    SkMatrix spriteToOutput = SkMatrix::I();
    SkMatrix outputToSprite = SkMatrix::I();
    std::array<float, 2> spriteSize{0.0F, 0.0F};
    if (spriteMode > 0.5F) {
      // Flow samples the actual guarded _MainTex attachment. Its UV extent
      // cannot come from the paragraph wrapping box or an unguarded sprite.
      spriteSize = flowWarp && pageDomain
          ? std::array<float, 2>{pageDomain->resolvedFixedGeometryBounds.width(),
                                 pageDomain->resolvedFixedGeometryBounds.height()}
          : vector2OrZero(14U);
      if (!pageDomain || spriteSize[0] <= 0.0F || spriteSize[1] <= 0.0F ||
          cycle < 0.0F) {
        error = "text turbulence sprite projection requires a valid Page, size and cycle";
        return {};
      }
      const auto center = SkPoint::Make(
          pageDomain->resolvedFixedGeometryBounds.centerX(),
          pageDomain->resolvedFixedGeometryBounds.centerY());
      SkMatrix spriteToReference;
      // The authored quad already gives its lower vertices texcoord.y=1.
      // The vertex shader's Y flip therefore produces bottom-left source UV,
      // including Flow. Apply that convention once in the shared projection.
      spriteToReference.setAll(spriteSize[0], 0.0F, center.x() - spriteSize[0] * 0.5F,
                               0.0F, -spriteSize[1], center.y() + spriteSize[1] * 0.5F,
                               0.0F, 0.0F, 1.0F);
      spriteToOutput.setConcat(pageDomain->authoredToDevice, spriteToReference);
      if (!spriteToOutput.invert(&outputToSprite)) {
        error = "text turbulence sprite projection is singular";
        return {};
      }
    }
    const auto bindSpriteProjection = [&](SkRuntimeEffectBuilder &builder) {
      builder.uniform("spriteToOutput0") = std::array<float, 3>{
          spriteToOutput.getScaleX(), spriteToOutput.getSkewX(), spriteToOutput.getTranslateX()};
      builder.uniform("spriteToOutput1") = std::array<float, 3>{
          spriteToOutput.getSkewY(), spriteToOutput.getScaleY(), spriteToOutput.getTranslateY()};
      builder.uniform("outputToSprite0") = std::array<float, 3>{
          outputToSprite.getScaleX(), outputToSprite.getSkewX(), outputToSprite.getTranslateX()};
      builder.uniform("outputToSprite1") = std::array<float, 3>{
          outputToSprite.getSkewY(), outputToSprite.getScaleY(), outputToSprite.getTranslateY()};
    };
    if (flowWarp) {
      const float progress = scalar(0U);
      const float frequency = scalar(1U);
      const float displacement = scalar(2U);
      const auto endScale = vector2OrZero(3U);
      if (progress < 0.0F || progress > 1.0F || frequency < 0.0F ||
          displacement < 0.0F || endScale[0] <= 0.0F || endScale[1] <= 0.0F) {
        error = "text flow warp requires normalized progress and positive source scale";
        return {};
      }
      image = ExecuteExecutionMaterialProgram(
          raster.image, gpuContext,
          GetTextRuntimeProgram(TextRuntimeShader::MaterialFlowWarp),
          [&](SkRuntimeEffectBuilder &builder) {
            bindSpriteProjection(builder);
            const auto textureBounds = pageDomain->authoredToDevice.mapRect(
                pageDomain->resolvedFixedGeometryBounds);
            builder.uniform("textSize") = std::array<float, 2>{
                std::ceil(textureBounds.width()), std::ceil(textureBounds.height())};
            builder.uniform("progress") = progress;
            builder.uniform("frequency") = frequency;
            builder.uniform("displacement") = displacement;
            builder.uniform("endScale") = endScale;
          }, error);
      break;
    }
    const auto scale = vector2OrZero(0U);
    // Optional authored normalization extent for the source's
    // range * extent / min(outputWidth, outputHeight) control.
    const float rangeExtent = scalarOr(12U, 0.0F);
    if (scale[0] == 0.0F || scale[1] == 0.0F || scalar(8U) < 0.0F ||
        scalar(9U) <= 0.0F || viewportSize.width() <= 0.0F ||
        viewportSize.height() <= 0.0F || !std::isfinite(rangeExtent) ||
        rangeExtent < 0.0F) {
      error = "text turbulence requires valid main/sub scale, impact and viewport";
      return {};
    }
    const float range = scalar(1U) *
        (rangeExtent > 0.0F
             ? rangeExtent / std::min(viewportSize.width(), viewportSize.height())
             : 1.0F);
    if (!std::isfinite(range)) {
      error = "text turbulence normalized range is nonfinite: " + node.nodeId;
      return {};
    }
    if (std::getenv("VIDEOCUT_TRACE_TEXT_MATERIAL_TURBULENCE") != nullptr) {
      const auto authoredOffset = vector2OrZero(4U);
      const auto authoredSubOffset = vector2OrZero(11U);
      std::fprintf(stderr,
                   "[VIDEOCUT_TEXT_MATERIAL_TURBULENCE] node=%s mode=%.9g "
                   "texture=%dx%d viewport=[%.9g %.9g] scale=[%.9g %.9g] "
                   "range=%.9g evolution=%.9g complexity=%.9g "
                   "offset=[%.9g %.9g] rotation=%.9g brightness=%.9g "
                   "contrast=%.9g subImpact=%.9g subScale=%.9g "
                   "subRotation=%.9g subOffset=[%.9g %.9g]\n",
                   node.nodeId.c_str(), spriteMode, raster.image->width(),
                   raster.image->height(), viewportSize.width(),
                   viewportSize.height(), scale[0], scale[1], range, scalar(2U),
                   scalar(3U), authoredOffset[0], authoredOffset[1], scalar(5U),
                   scalar(6U), scalar(7U), scalar(8U), scalar(9U), scalar(10U),
                   authoredSubOffset[0], authoredSubOffset[1]);
    }
    if (!gpuContext) {
      error = "text material turbulence requires an active GPU context";
      return {};
    }
    QtTextMaterialTurbulenceUniforms uniforms;
    uniforms.textureSize = {static_cast<float>(raster.image->width()),
                            static_cast<float>(raster.image->height())};
    uniforms.viewportSize = {viewportSize.width(), viewportSize.height()};
    uniforms.scale = scale;
    uniforms.range = range;
    uniforms.evolution = scalar(2U);
    uniforms.complexity = scalar(3U);
    uniforms.offset = vector2OrZero(4U);
    uniforms.rotation = scalar(5U);
    uniforms.brightness = scalar(6U);
    uniforms.contrast = scalar(7U);
    uniforms.subImpact = scalar(8U);
    uniforms.subScale = scalar(9U);
    uniforms.subRotation = scalar(10U);
    uniforms.subOffset = vector2OrZero(11U);
    uniforms.spriteMode = spriteMode;
    uniforms.cycle = cycle;
    uniforms.spriteToOutput0 = {spriteToOutput.getScaleX(), spriteToOutput.getSkewX(),
                                spriteToOutput.getTranslateX(), 0.0F};
    uniforms.spriteToOutput1 = {spriteToOutput.getSkewY(), spriteToOutput.getScaleY(),
                                spriteToOutput.getTranslateY(), 0.0F};
    uniforms.outputToSprite0 = {outputToSprite.getScaleX(), outputToSprite.getSkewX(),
                                outputToSprite.getTranslateX(), 0.0F};
    uniforms.outputToSprite1 = {outputToSprite.getSkewY(), outputToSprite.getScaleY(),
                                outputToSprite.getTranslateY(), 0.0F};
    image = gpuContext->RenderQtTextRawPass(
        raster.image, raster.image->width(), raster.image->height(),
        [&](void *input, void *output, void *queue, const NativeCommandSubmission &submit, std::string &failure) {
          return RenderQtTextMaterialTurbulence(uniforms, input, output, queue, failure, submit);
        }, error);
    break;
  }
  case Capability::MaterialProjectionMeshComposite: {
    const float mode = scalar(4U);
    const auto textSize = vector2OrZero(1U);
    if ((mode < 2.0F && (textSize[0] <= 0.0F || textSize[1] <= 0.0F)) ||
        (mode == 2.0F && (viewportSize.width() <= 0.0F || viewportSize.height() <= 0.0F)) ||
        (mode == 3.0F && (scalar(2U) < 0.0F || scalar(3U) <= 0.0F))) {
      error = "text projection composite has an invalid extent, intensity or exponent";
      return {};
    }
    sk_sp<SkImage> auxiliary = raster.image;
    sk_sp<SkImage> packed = raster.image;
    sk_sp<SkImage> color = raster.image;
    if (mode >= 2.0F && !materializeOrderedInput(1U, auxiliary)) return {};
    if (mode == 3.0F && (!materializeOrderedInput(2U, packed) ||
                         !materializeOrderedInput(3U, color))) return {};
    image = ExecuteExecutionMaterialProgram(
        raster.image, gpuContext,
        GetTextRuntimeProgram(TextRuntimeShader::MaterialProjectionComposite),
        [&](SkRuntimeEffectBuilder &builder) {
          builder.child("auxiliaryTexture") = rawInputShader(auxiliary);
          builder.child("packedTexture") = rawInputShader(packed);
          builder.child("colorTexture") = rawInputShader(color);
          builder.uniform("textureSize") = std::array<float, 2>{
              static_cast<float>(raster.image->width()), static_cast<float>(raster.image->height())};
          builder.uniform("viewportSize") = std::array<float, 2>{viewportSize.width(), viewportSize.height()};
          builder.uniform("textSize") = textSize;
          builder.uniform("passIndex") = mode;
          builder.uniform("progress") = scalar(0U);
          builder.uniform("alpha") = scalarOrZero(2U);
          builder.uniform("glowIntensity") = scalarOrZero(2U);
          builder.uniform("colorExponent") = scalarOr(3U, 2.4F);
          builder.uniform("textColor") = vector4OrZero(5U);
        },
        error);
    break;
  }
  case Capability::MaterialCylinderProjectionComposite: {
    const auto extent = vector2OrZero(1U);
    const float facets = scalarOr(8U, 128.0F);
    const float cameraOffset = scalarOr(9U, 1.732F);
    if (projectionCanvasHeight <= 0.0F || facets < 4.0F || facets > 512.0F ||
        std::fmod(facets, 2.0F) != 0.0F || cameraOffset <= 0.0F) {
      error = "text cylinder requires a positive canvas/camera and an even facet count [4, 512]";
      return {};
    }
    const float width = referenceVectorToRaster(SkVector::Make(extent[0], 0.0F)).length();
    const float height = referenceVectorToRaster(SkVector::Make(0.0F, extent[1])).length();
    const float diameterScale = width * scalar(2U) /
        (3.14159265358979323846F * projectionCanvasHeight);
    return RecordExecutionMaterialProgram(
        raster, recordingBounds, GetTextRuntimeProgram(TextRuntimeShader::MaterialCylinder),
        [&](SkRuntimeEffectBuilder &builder) {
          builder.uniform("textureSize") = std::array<float, 2>{
              static_cast<float>(raster.image->width()), static_cast<float>(raster.image->height())};
          builder.uniform("canvasHeight") = projectionCanvasHeight;
          builder.uniform("modelScale") = std::array<float, 3>{diameterScale, height / projectionCanvasHeight, diameterScale};
          builder.uniform("rotationDegrees") = std::array<float, 3>{scalarOrZero(6U), scalar(0U), scalarOrZero(7U)};
          builder.uniform("fieldOfView") = scalar(3U);
          builder.uniform("cameraOffset") = cameraOffset;
          builder.uniform("facetCount") = facets;
          builder.uniform("uvScale") = scalar(2U);
          builder.uniform("uvOffset") = scalar(4U);
          builder.uniform("opacity") = scalar(5U);
        }, error);
  }
  case Capability::MaterialParticleScatterComposite: {
    const auto inverseX = vector3OrZero(2U);
    const auto inverseY = vector3OrZero(4U);
    const auto extent = vector2OrZero(1U);
    const float determinant = inverseX[0] * inverseY[1] - inverseX[1] * inverseY[0];
    if (viewportSize.width() <= 0.0F || viewportSize.height() <= 0.0F ||
        extent[0] <= 0.0F || extent[1] <= 0.0F ||
        !std::isfinite(determinant) || determinant == 0.0F) {
      error = "text particle scatter requires a viewport, extent and invertible model rows";
      return {};
    }
    const float scaleX = pageDomain ? 1.0F / pageDomain->rasterScaleX : 1.0F;
    const float scaleY = pageDomain ? 1.0F / pageDomain->rasterScaleY : 1.0F;
    const float left = pageDomain ? pageDomain->deviceTargetBounds.left() : 0.0F;
    const float top = pageDomain ? pageDomain->deviceTargetBounds.top() : 0.0F;
    // The source vertex shader draws a fullscreen quad while sampling the
    // text attachment. Keep those two extents distinct; the particle field
    // must not be clipped to the glyph render target.
    const auto outputBounds = SkRect::MakeLTRB(
        -left / scaleX, -top / scaleY,
        (viewportSize.width() - left) / scaleX,
        (viewportSize.height() - top) / scaleY);
    return RecordExecutionMaterialProgram(
        raster, recordingBounds,
        GetTextRuntimeProgram(TextRuntimeShader::MaterialParticleScatter),
        [&](SkRuntimeEffectBuilder &builder) {
          builder.uniform("textureSize") = std::array<float, 2>{
              static_cast<float>(raster.image->width()), static_cast<float>(raster.image->height())};
          builder.uniform("viewportSize") = std::array<float, 2>{viewportSize.width(), viewportSize.height()};
          builder.uniform("pixelOrigin") = std::array<float, 2>{
              left + static_cast<float>(raster.bounds.left()) * scaleX,
              top + static_cast<float>(raster.bounds.top()) * scaleY};
          builder.uniform("pixelScale") = std::array<float, 2>{scaleX, scaleY};
          // The source divides output pixels by getRectExpanded() directly;
          // this is a source numeric grid, not a distance to project again.
          builder.uniform("expandSize") = extent;
          builder.uniform("inverseModelRowX") = inverseX;
          builder.uniform("inverseModelRowY") = inverseY;
          builder.uniform("progress") = scalar(0U);
          builder.uniform("textLength") = scalar(3U);
        }, error, &outputBounds);
  }
  case Capability::MaterialSdfLightSweep:
  case Capability::MaterialSdfCutGlow:
  case Capability::MaterialRadialMediaComposite:
  case Capability::MaterialCubeProjectionComposite: {
    if (node.capability == Capability::MaterialRadialMediaComposite) {
      const float count = scalarOr(5U, 12.0F);
      const float amount = scalar(0U) * scalar(2U);
      if (count < 0.0F || count > 64.0F || count != std::floor(count) ||
          !std::isfinite(amount) || scalar(4U) < 0.0F || scalar(4U) > 1.0F) {
        error = "text radial media requires a finite sample count, zoom and opacity";
        return {};
      }
      for (unsigned tap = 1U; tap <= static_cast<unsigned>(count); ++tap) {
        const float zoom = 1.0F + static_cast<float>(tap) / 720.0F * amount;
        if (!std::isfinite(zoom) || zoom == 0.0F) {
          error = "text radial media sample has a singular zoom";
          return {};
        }
      }
    }
    const auto firstClosed = static_cast<unsigned>(
        Capability::MaterialSeparableGaussianBlur);
    const auto operation = static_cast<float>(
        1U + static_cast<unsigned>(node.capability) - firstClosed);
    const auto referenceAxisX = referenceVectorToRaster(SkVector::Make(1.0F, 0.0F));
    const auto referenceAxisY = referenceVectorToRaster(SkVector::Make(0.0F, 1.0F));
    const float pixelScaleX = referenceAxisX.length();
    const float pixelScaleY = referenceAxisY.length();
    const float pixelScale = std::sqrt(pixelScaleX * pixelScaleY);
    const auto materialScalar = [&](const std::uint32_t slot) {
      const auto *parameter = ConsumeTextExecutionParameter(
          node, Kind::MaterialScalar, slot, stableUnitId);
      if (!parameter) {
        return node.capability == Capability::MaterialRadialMediaComposite &&
                       slot == 5U ? 12.0F : 0.0F;
      }
      return parameter->values.front() *
          (parameter->valueSpace ==
                   text::TextEffectExecutionParameterSpace::ReferencePixels
               ? pixelScale : 1.0F);
    };
    const auto materialVector = [&](const std::uint32_t slot) {
      auto values = vector2OrZero(slot);
      const auto *parameter = ConsumeTextExecutionParameter(
          node, Kind::MaterialVector2, slot, stableUnitId);
      if (parameter && parameter->valueSpace ==
                           text::TextEffectExecutionParameterSpace::ReferencePixels) {
        values[0] *= pixelScaleX;
        values[1] *= pixelScaleY;
      }
      return values;
    };
    const auto configure = [&](SkRuntimeEffectBuilder &builder) {
          builder.uniform("textureSize") = std::array<float, 2>{
              static_cast<float>(raster.image->width()),
              static_cast<float>(raster.image->height())};
          builder.uniform("operation") = operation;
          builder.uniform("projectionCanvasHeight") = projectionCanvasHeight;
          builder.uniform("viewportSize") = std::array<float, 2>{
              viewportSize.width(), viewportSize.height()};
          builder.uniform("scalars0") = std::array<float, 4>{
              materialScalar(0U), materialScalar(1U), materialScalar(2U),
              materialScalar(3U)};
          builder.uniform("scalars1") = std::array<float, 4>{
              materialScalar(4U), materialScalar(5U), materialScalar(6U),
              materialScalar(7U)};
          builder.uniform("vector1") = materialVector(1U);
          builder.uniform("vector3") = materialVector(3U);
          builder.uniform("color2") = vector4OrZero(2U);
        };
    if (node.capability == Capability::MaterialCubeProjectionComposite) {
      return RecordExecutionMaterialProgram(
          raster, recordingBounds,
          GetTextRuntimeProgram(TextRuntimeShader::MaterialClosed), configure,
          error);
    }
    image = ExecuteExecutionMaterialProgram(
        raster.image, gpuContext,
        GetTextRuntimeProgram(TextRuntimeShader::MaterialClosed), configure,
        error);
    break;
  }
  default:
    error = "text execution material capability has no closed native "
            "implementation: " +
            node.nodeId;
    return {};
  }
  if (!image)
    return {};
  return ExecutionGraphPictureFromRaster(image, outputRasterBounds,
                                         recordingBounds, error);
}


SkRect ResolveExecutionMaterialUnitBounds(
    const UnitGeometry &unit, const TextRenderFramePlan &renderPlan,
    const PageRenderGroupDomain *pageDomain,
    const std::optional<text::TextEffectExecutionStaticAffine> &staticAffine) {
  SkRect bounds = !unit.scriptBounds.isEmpty()
                      ? unit.scriptBounds
                      : (!unit.layoutBounds.isEmpty() ? unit.layoutBounds
                                                       : unit.authoredBounds);
  if (const auto *unitPlan =
          renderPlan.FindUnit(unit.binding.stableUnitId);
      unitPlan && !unitPlan->transforms.empty()) {
    if (const auto transform =
            text::ComposeTextEffectTransforms(unitPlan->transforms)) {
      bounds = MapEffectBounds(bounds, *transform).value_or(bounds);
    }
  }
  if (pageDomain) {
    bounds = pageDomain->authoredToDevice.mapRect(bounds);
    bounds.offset(-pageDomain->deviceTargetBounds.left(),
                  -pageDomain->deviceTargetBounds.top());
    bounds = SkRect::MakeLTRB(bounds.left() * pageDomain->rasterScaleX,
                              bounds.top() * pageDomain->rasterScaleY,
                              bounds.right() * pageDomain->rasterScaleX,
                              bounds.bottom() * pageDomain->rasterScaleY);
  }
  if (staticAffine && !bounds.isEmpty()) {
    SkMatrix transform;
    transform.setTranslate(staticAffine->translationX + staticAffine->pivotX,
                           staticAffine->translationY + staticAffine->pivotY);
    transform.postRotate(staticAffine->rotationDegrees);
    transform.postScale(staticAffine->scaleX, staticAffine->scaleY);
    transform.postTranslate(-staticAffine->pivotX, -staticAffine->pivotY);
    bounds = transform.mapRect(bounds);
  }
  return bounds;
}


sk_sp<SkPicture> ExecuteClosedExecutionMaterialDomains(
    const sk_sp<SkPicture> &source,
    const text::TextEffectExecutionNodeFramePlan &node,
    const std::vector<sk_sp<SkPicture>> &orderedInputs,
    const std::unordered_map<std::string, sk_sp<SkImage>> &sampledMediaInputs,
    const TextRenderFramePlan &renderPlan, const ResolvedLayout &layout,
    const PageRenderGroupDomain *pageDomain, const SkRect &recordingBounds,
    const float referenceToExecutionScaleX,
    const float referenceToExecutionScaleY,
    const float projectionCanvasHeight, const SkSize viewportSize,
    TextVisualAssetStore &assets,
    SkiaGpuContext *gpuContext,
    std::string &error) {
  const auto &framePlan = renderPlan.value();
  std::unordered_set<std::uint64_t> pendingUnitIds;
  for (const auto &parameter : node.parameters) {
    if (parameter.domain ==
            text::TextEffectExecutionParameterDomain::PerUnit &&
        parameter.stableUnitId) {
      pendingUnitIds.insert(*parameter.stableUnitId);
    }
  }
  if (pendingUnitIds.empty()) {
    return ExecuteClosedExecutionMaterial(source, node, orderedInputs,
                                          sampledMediaInputs,
                                          std::nullopt, framePlan, assets,
                                          pageDomain, recordingBounds,
                                          referenceToExecutionScaleX,
                                          referenceToExecutionScaleY,
                                          projectionCanvasHeight, viewportSize,
                                          gpuContext, error);
  }

  std::vector<RenderComponent> unitOutputs;
  unitOutputs.reserve(pendingUnitIds.size());
  std::size_t authoredOrder = 0U;
  for (const auto &unit : layout.units) {
    const auto stableUnitId = unit.binding.stableUnitId;
    if (!pendingUnitIds.erase(stableUnitId))
      continue;
    auto bounds = ResolveExecutionMaterialUnitBounds(
        unit, renderPlan, pageDomain, node.staticAffine);
    if (!bounds.isFinite() || bounds.isEmpty() ||
        !bounds.intersect(recordingBounds)) {
      error = "text execution material per-unit source has invalid bounds: " +
              node.nodeId;
      return {};
    }
    SkPictureRecorder recorder;
    auto *canvas = recorder.beginRecording(recordingBounds);
    canvas->clipRect(bounds, SkClipOp::kIntersect, false);
    canvas->drawPicture(source);
    auto unitSource = recorder.finishRecordingAsPicture();
    std::vector<sk_sp<SkPicture>> unitInputs;
    unitInputs.reserve(orderedInputs.size());
    for (const auto &orderedInput : orderedInputs) {
      SkPictureRecorder inputRecorder;
      auto *inputCanvas = inputRecorder.beginRecording(recordingBounds);
      inputCanvas->clipRect(bounds, SkClipOp::kIntersect, false);
      inputCanvas->drawPicture(orderedInput);
      unitInputs.push_back(inputRecorder.finishRecordingAsPicture());
    }
    auto output = ExecuteClosedExecutionMaterial(
        unitSource, node, unitInputs, sampledMediaInputs, stableUnitId, framePlan, assets,
        pageDomain, recordingBounds, referenceToExecutionScaleX,
        referenceToExecutionScaleY, projectionCanvasHeight, viewportSize,
        gpuContext, error);
    if (!output)
      return {};
    unitOutputs.push_back(
        {node.nodeId + ":unit:" + std::to_string(stableUnitId),
         text::TextEffectCompositeItemKind::GlyphMaterial,
         0,
         text::TextBlendMode::SourceOver,
         std::move(output),
         "execution-material-unit",
         authoredOrder++});
  }
  if (!pendingUnitIds.empty()) {
    error = "text execution material parameter references an unknown stable "
            "unit: " +
            node.nodeId;
    return {};
  }
  return ComposePictures(unitOutputs, recordingBounds);
}


void OverridePostEffectScalarParameter(
    text::TextEffectPostEffectNode &effect, const std::string_view name,
    const float value, const bool appendWhenMissing) {
  const auto found = std::find_if(
      effect.parameters.begin(), effect.parameters.end(),
      [&](const auto &parameter) { return parameter.name == name; });
  if (found != effect.parameters.end()) {
    found->values = {value};
    found->keyframes.clear();
  } else if (appendWhenMissing) {
    text::TextPostEffectParameter parameter;
    parameter.name = name;
    parameter.values = {value};
    effect.parameters.push_back(std::move(parameter));
  }
}


bool ApplyPostEffectExecutionParameters(
    const text::TextEffectExecutionNodeFramePlan &node,
    text::TextEffectPostEffectNode &effect, double &progress,
    std::string &error) {
  progress = node.progress;
  for (const auto &parameter : node.parameters) {
    const float value = parameter.values.front();
    switch (parameter.parameter) {
    case text::TextEffectExecutionParameterKind::PostEffectProgress:
      progress = value;
      break;
    case text::TextEffectExecutionParameterKind::PostEffectAmplitude:
      effect.amount = value;
      OverridePostEffectScalarParameter(effect, "amount", value, false);
      OverridePostEffectScalarParameter(
          effect, "amplitude", value,
          effect.kind == text::TextPostEffectKind::WaveWarp);
      break;
    case text::TextEffectExecutionParameterKind::PostEffectBlurRadius: {
      const bool hasBlurIntensity = std::any_of(
          effect.parameters.begin(), effect.parameters.end(),
          [](const auto &candidate) {
            return candidate.name == "blurIntensity";
          });
      const bool hasRadius = std::any_of(
          effect.parameters.begin(), effect.parameters.end(),
          [](const auto &candidate) { return candidate.name == "radius"; });
      const bool hasBlur = std::any_of(
          effect.parameters.begin(), effect.parameters.end(),
          [](const auto &candidate) { return candidate.name == "blur"; });
      if (hasBlurIntensity ||
          effect.kind == text::TextPostEffectKind::GaussianBlur) {
        OverridePostEffectScalarParameter(effect, "blurIntensity", value,
                                          true);
      } else if (hasRadius ||
                 effect.kind == text::TextPostEffectKind::DirectionalBlur) {
        OverridePostEffectScalarParameter(effect, "radius", value, true);
      } else if (hasBlur ||
                 effect.kind == text::TextPostEffectKind::MultiShadow) {
        OverridePostEffectScalarParameter(effect, "blur", value, true);
      } else {
        error = "text execution graph post effect has no closed blur-radius "
                "destination: " +
                node.nodeId;
        return false;
      }
      break;
    }
    case text::TextEffectExecutionParameterKind::MaterialScalar:
    case text::TextEffectExecutionParameterKind::MaterialVector2:
    case text::TextEffectExecutionParameterKind::MaterialVector3:
    case text::TextEffectExecutionParameterKind::MaterialVector4:
    case text::TextEffectExecutionParameterKind::SceneTranslation:
    case text::TextEffectExecutionParameterKind::SceneScale:
    case text::TextEffectExecutionParameterKind::SceneOpacity:
    case text::TextEffectExecutionParameterKind::NodeScalar:
    case text::TextEffectExecutionParameterKind::NodeVector2:
    case text::TextEffectExecutionParameterKind::NodeVector3:
    case text::TextEffectExecutionParameterKind::NodeVector4:
      error = "text execution graph post effect received a non-post parameter: " +
              node.nodeId;
      return false;
    }
    RecordTextExecutionParameterConsumption(node, parameter);
  }
  return true;
}


sk_sp<SkPicture> ApplyExecutionGraphStaticAffine(
    const sk_sp<SkPicture> &source,
    const text::TextEffectExecutionStaticAffine &affine,
    const SkRect &recordingBounds) {
  if (!source)
    return {};
  SkPictureRecorder recorder;
  auto *canvas = recorder.beginRecording(recordingBounds);
  canvas->translate(affine.translationX + affine.pivotX,
                    affine.translationY + affine.pivotY);
  canvas->rotate(affine.rotationDegrees);
  canvas->scale(affine.scaleX, affine.scaleY);
  canvas->translate(-affine.pivotX, -affine.pivotY);
  canvas->drawPicture(source);
  return recorder.finishRecordingAsPicture();
}


sk_sp<SkPicture> ApplyExecutionSceneCloneSample(
    const sk_sp<SkPicture> &source,
    const text::TextEffectExecutionNodeFramePlan &node,
    const std::optional<std::uint64_t> stableUnitId,
    const SkPoint pivot, const PageRenderGroupDomain *pageDomain,
    const SkRect &recordingBounds) {
  using Kind = text::TextEffectExecutionParameterKind;
  const auto *translation =
      ConsumeTextExecutionParameter(node, Kind::SceneTranslation, 0U, stableUnitId);
  const auto *scale =
      ConsumeTextExecutionParameter(node, Kind::SceneScale, 0U, stableUnitId);
  const auto *opacity =
      ConsumeTextExecutionParameter(node, Kind::SceneOpacity, 0U, stableUnitId);
  float translationX = translation ? translation->values[0] : 0.0F;
  float translationY = translation ? translation->values[1] : 0.0F;
  if (translation && pageDomain) {
    SkVector vector = SkVector::Make(translationX, translationY);
    vector = pageDomain->authoredToDevice.mapVector(vector);
    translationX = vector.x() * pageDomain->rasterScaleX;
    translationY = vector.y() * pageDomain->rasterScaleY;
  }
  const float scaleX = scale ? scale->values[0] : 1.0F;
  const float scaleY = scale ? scale->values[1] : 1.0F;
  const float alpha = opacity ? opacity->values[0] : 1.0F;
  const float pivotX = pivot.x();
  const float pivotY = pivot.y();

  SkPictureRecorder recorder;
  auto *canvas = recorder.beginRecording(recordingBounds);
  canvas->translate(translationX + pivotX, translationY + pivotY);
  canvas->scale(scaleX, scaleY);
  canvas->translate(-pivotX, -pivotY);
  SkPaint paint;
  paint.setAlphaf(alpha);
  canvas->drawPicture(source, nullptr, &paint);
  return recorder.finishRecordingAsPicture();
}


sk_sp<SkPicture> ExecuteExecutionSceneCloneParameters(
    const sk_sp<SkPicture> &source,
    const text::TextEffectExecutionNodeFramePlan &node,
    const TextRenderFramePlan &renderPlan, const ResolvedLayout &layout,
    const PageRenderGroupDomain *pageDomain, const SkRect &recordingBounds,
    const SkPoint pageScenePivot,
    const std::unordered_map<std::uint64_t, sk_sp<SkPicture>> *unitSources,
    std::string &error) {
  std::unordered_set<std::uint64_t> pendingUnitIds;
  for (const auto &parameter : node.parameters) {
    if (parameter.domain ==
            text::TextEffectExecutionParameterDomain::PerUnit &&
        parameter.stableUnitId) {
      pendingUnitIds.insert(*parameter.stableUnitId);
    }
  }
  if (pendingUnitIds.empty()) {
    SkRect sourceEntityBounds = SkRect::MakeEmpty();
    for (const auto &unit : layout.units) {
      const auto unitBounds = ResolveExecutionMaterialUnitBounds(
          unit, renderPlan, pageDomain, std::nullopt);
      if (unitBounds.isFinite() && !unitBounds.isEmpty())
        sourceEntityBounds.join(unitBounds);
    }
    if (sourceEntityBounds.isEmpty() || !sourceEntityBounds.isFinite())
      sourceEntityBounds = recordingBounds;
    return ApplyExecutionSceneCloneSample(
        source, node, std::nullopt, pageScenePivot, pageDomain,
        recordingBounds);
  }

  std::vector<RenderComponent> unitOutputs;
  unitOutputs.reserve(pendingUnitIds.size());
  std::size_t authoredOrder = 0U;
  for (const auto &unit : layout.units) {
    const auto stableUnitId = unit.binding.stableUnitId;
    if (!pendingUnitIds.erase(stableUnitId))
      continue;
    auto unitBounds = ResolveExecutionMaterialUnitBounds(
        unit, renderPlan, pageDomain, std::nullopt);
    if (!unitBounds.isFinite() || unitBounds.isEmpty()) {
      error = "text execution scene parameter has invalid unit bounds: " +
              node.nodeId;
      return {};
    }
    if (std::getenv("VIDEOCUT_TRACE_TEXT_RENDER_GROUP_TOPOLOGY") != nullptr) {
      std::fprintf(stderr,
                   "[VIDEOCUT_TEXT_SCENE_UNIT] node=%s unit=%llu page=%d "
                   "clip=[%g %g %g %g] script=[%g %g %g %g] "
                   "layout=[%g %g %g %g] source=[%g %g %g %g]\n",
                   node.nodeId.c_str(),
                   static_cast<unsigned long long>(stableUnitId),
                   pageDomain != nullptr, unitBounds.left(), unitBounds.top(),
                   unitBounds.right(), unitBounds.bottom(),
                   unit.scriptBounds.left(), unit.scriptBounds.top(),
                   unit.scriptBounds.right(), unit.scriptBounds.bottom(),
                   unit.layoutBounds.left(), unit.layoutBounds.top(),
                   unit.layoutBounds.right(), unit.layoutBounds.bottom(),
                   source->cullRect().left(), source->cullRect().top(),
                   source->cullRect().right(), source->cullRect().bottom());
    }
    sk_sp<SkPicture> isolated;
    if (unitSources) {
      const auto found = unitSources->find(stableUnitId);
      if (found == unitSources->end()) {
        error = "text execution scene has no native unit source: " + node.nodeId;
        return {};
      }
      isolated = found->second;
    } else {
      SkPictureRecorder isolateRecorder;
      auto *isolate = isolateRecorder.beginRecording(recordingBounds);
      isolate->clipRect(unitBounds, SkClipOp::kIntersect, false);
      isolate->drawPicture(source);
      isolated = isolateRecorder.finishRecordingAsPicture();
    }
    auto transformed = ApplyExecutionSceneCloneSample(
        isolated, node, stableUnitId,
        SkPoint::Make(unitBounds.centerX(), unitBounds.centerY()), pageDomain,
        recordingBounds);
    if (!transformed) {
      error = "text execution scene parameter recording failed: " +
              node.nodeId;
      return {};
    }
    unitOutputs.push_back(
        {node.nodeId + ":unit:" + std::to_string(stableUnitId),
         text::TextEffectCompositeItemKind::GlyphMaterial,
         0,
         text::TextBlendMode::SourceOver,
         std::move(transformed),
         "execution-scene-unit",
         authoredOrder++});
  }
  if (!pendingUnitIds.empty()) {
    error = "text execution scene parameter references an unknown stable unit: " +
            node.nodeId;
    return {};
  }
  return ComposePictures(unitOutputs, recordingBounds);
}


sk_sp<SkPicture> ExecuteSameFrameHistoryFold(
    const std::vector<sk_sp<SkPicture>> &orderedInputs,
    const SkRect &recordingBounds, const float sourceGain,
    const float feedbackGain, const std::optional<float> finalSourceMix,
    SkiaGpuContext *gpuContext, std::string &error) {
  const std::size_t foldCount =
      orderedInputs.size() - (finalSourceMix ? 1U : 0U);
  if (foldCount == 0U || !std::isfinite(sourceGain) ||
      !std::isfinite(feedbackGain) || sourceGain < 0.0F ||
      feedbackGain < 0.0F || sourceGain > 1.0F || feedbackGain > 1.0F ||
      (finalSourceMix &&
       (!std::isfinite(*finalSourceMix) || *finalSourceMix < 0.0F ||
        *finalSourceMix > 1.0F))) {
    error = "text same-frame history fold parameters are invalid";
    return {};
  }

  sk_sp<SkPicture> accumulated;
  for (std::size_t index = 0U; index < foldCount; ++index) {
    if (!orderedInputs[index]) {
      error = "text same-frame history fold input is empty";
      return {};
    }
    SkPictureRecorder recorder;
    auto *canvas = recorder.beginRecording(recordingBounds);
    if (accumulated) {
      SkPaint feedbackPaint;
      feedbackPaint.setAlphaf(feedbackGain);
      feedbackPaint.setBlendMode(SkBlendMode::kPlus);
      canvas->drawPicture(accumulated, nullptr, &feedbackPaint);
    }
    SkPaint sourcePaint;
    sourcePaint.setAlphaf(sourceGain);
    sourcePaint.setBlendMode(SkBlendMode::kPlus);
    canvas->drawPicture(orderedInputs[index], nullptr, &sourcePaint);
    accumulated = MaterializeExecutionGraphPicture(
        recorder.finishRecordingAsPicture(), recordingBounds, gpuContext,
        error);
    if (!accumulated)
      return {};
  }

  if (!finalSourceMix)
    return accumulated;
  const auto &finalSource = orderedInputs.back();
  if (!finalSource) {
    error = "text same-frame history final source is empty";
    return {};
  }
  SkPictureRecorder recorder;
  auto *canvas = recorder.beginRecording(recordingBounds);
  SkPaint accumulatedPaint;
  accumulatedPaint.setAlphaf(1.0F - *finalSourceMix);
  accumulatedPaint.setBlendMode(SkBlendMode::kPlus);
  canvas->drawPicture(accumulated, nullptr, &accumulatedPaint);
  SkPaint sourcePaint;
  sourcePaint.setAlphaf(*finalSourceMix);
  sourcePaint.setBlendMode(SkBlendMode::kPlus);
  canvas->drawPicture(finalSource, nullptr, &sourcePaint);
  return MaterializeExecutionGraphPicture(
      recorder.finishRecordingAsPicture(), recordingBounds, gpuContext,
      error);
}

} // namespace videocut::skia_runtime::internal::text_lane
