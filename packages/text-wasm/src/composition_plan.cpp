#include "composition_plan.h"
#include "videocut/text_composition/DecorationRenderPlan.h"
#include "text/QtTextPostEffectRuntime.h"
#include <stdexcept>

namespace videocut::text_wasm {
namespace comp = text_composition;
namespace runtime = skia_runtime::internal;
using Json = nlohmann::json;

void ValidateBrowserComposition(const comp::TextCompositionDocument& doc) {
  // A narrow admission profile. The adapter executes this topology explicitly;
  // it must never turn an arbitrary authored DAG into a flat approximation.
  if (!doc.templateOrigin ||
      (doc.templateOrigin->templateId != "builtin.text.qt-type.anim-studio-time-transform-softglow" &&
       doc.templateOrigin->templateId != "builtin.text.qt-type.anim-studio-dual-selector-radial"))
    throw std::invalid_argument("Browser composition currently admits only time-transform-softglow and dual-selector-radial");
  if (doc.animations.layers.size() != 1 || doc.decorations.size() != 1)
    throw std::invalid_argument("Unsupported browser composition topology");
  const auto& layer = doc.animations.layers.front();
  if (layer.postEffects.size() != 1 ||
      (layer.postEffects.front().kind != text::TextPostEffectKind::SoftGlow &&
       layer.postEffects.front().kind != text::TextPostEffectKind::RadialBlur))
    throw std::invalid_argument("Unsupported browser composition post effect");
  for (const auto& animator : layer.animators)
    if (!animator.effectProgramId.empty())
      throw std::invalid_argument("Browser composition cannot externalize program-controlled graph nodes");
  const auto& decoration = doc.decorations.front();
  if (decoration.mode != comp::VectorDecorationMode::CompositionOverlay ||
      decoration.effectScope != comp::DecorationEffectScope::InsideGroupBehindText)
    throw std::invalid_argument("Unsupported browser decoration placement");
}

Json SampleBrowserComposition(const comp::TextCompositionDocument& doc,
    const text::TextLayout& layout, std::int64_t timeUs, unsigned width, unsigned height) {
  const auto durationUs = doc.durationTicks * 25 / 3;
  comp::DecorationSampleTime time;
  time.absoluteCompositionUs = std::min(timeUs, durationUs - 1);
  time.compositionEndUs = durationUs;
  time.absoluteSourceTicks = time.absoluteCompositionUs * 3 / 25;
  time.sourceWindowEndTicks = doc.durationTicks;
  auto plan = comp::SampleDecorationRenderPlan(doc, layout, time);
  for (const auto& diagnostic : plan.diagnostics)
    if (diagnostic.severity == comp::DiagnosticSeverity::Error)
      throw std::runtime_error(diagnostic.code + ": " + diagnostic.message);
  Json decorations = Json::array();
  for (const auto& item : plan.items) {
    for (const auto& diagnostic : item.diagnostics)
      if (diagnostic.severity == comp::DiagnosticSeverity::Error)
        throw std::runtime_error(diagnostic.code + ": " + diagnostic.message);
    if (!item.enabled || !item.visible) continue;
    for (const auto& instance : item.instances) {
      const auto w = instance.asset.intrinsicWidth, h = instance.asset.intrinsicHeight;
      auto affine = comp::ResolveDecorationRasterAffine(instance, w, h, 0, 0, 1, 1);
      decorations.push_back({{"assetId",instance.asset.assetId},{"timeUs",instance.assetTimeUs},
        {"width",w},{"height",h},{"opacity",instance.opacity},{"zOrder",item.zOrder},
        {"affine",{affine.xx,affine.yx,affine.xy,affine.yy,affine.xOffset,affine.yOffset}}});
    }
  }
  Json effects = Json::array();
  for (const auto& layer : doc.animations.layers) {
    const auto sampled = text::SampleTextAnimationLayerEvaluation(doc.animations,layer,timeUs,durationUs);
    if (!sampled.active) continue;
    for (const auto& spec : layer.postEffects) {
      text::TextEffectPostEffectNode effect;
      effect.nodeId = spec.effectId; effect.kind = spec.kind; effect.parameters = spec.parameters;
      effect.amount = text::SampleTextKeyframeCurve(spec.amountKeyframes,sampled.progress,spec.amount);
      effect.paddingPx = spec.paddingPx;
      if (spec.kind == text::TextPostEffectKind::SoftGlow) {
        const auto c = runtime::ResolveQtSoftGlowContract(effect,sampled.progress,width,height);
        if (!c.exactPathSupported) throw std::runtime_error(c.unsupportedReason);
        effects.push_back({{"kind","soft-glow"},{"progress",sampled.progress},
          {"glowWidth",c.glowWidth},{"glowHeight",c.glowHeight},{"sampleCount",c.sampleCount},
          {"sigmaX",c.sigmaX},{"sigmaY",c.sigmaY},{"stepX",c.stepX},{"stepY",c.stepY},
          {"thresholdType",c.thresholdType},{"thresholdLow",c.thresholdLow},{"thresholdHigh",c.thresholdHigh},
          {"thresholdSmooth",c.thresholdSmooth},{"grayScale",c.grayScale},{"exposure",c.shaderExposure},
          {"displayGlow",c.displayGlow},{"glowColor",c.glowColor}});
      } else {
        const auto c = runtime::ResolveQtRadialBlurContract(effect,sampled.progress,width,height);
        if (!c.exactPathSupported) throw std::runtime_error(c.unsupportedReason);
        effects.push_back({{"kind","radial-blur"},{"progress",sampled.progress},
          {"intensity",c.intensity},{"blurType",c.blurType},{"center",c.center},{"quality",c.quality},
          {"weightDecay",c.weightDecay},{"dither",c.dither},{"borderType",c.borderType},
          {"blurAlpha",c.blurAlpha},{"inverseGammaCorrection",c.inverseGammaCorrection},{"gamma",c.gamma},
          {"lightIntensity",c.lightIntensity},{"lightTransferMode",c.lightTransferMode}});
      }
    }
  }
  return {{"profile","browser-composition-experimental-v1"},{"decorations",decorations},
      {"effects",effects},{"surface","output-canvas"}};
}
}
