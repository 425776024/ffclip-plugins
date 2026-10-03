#include "text/TextExecutionParameterContract.h"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <unordered_set>
#include <utility>

namespace videocut::skia_runtime::internal {

bool ExecutionGraphCapabilityMatchesKind(
    const text::TextEffectExecutionNodeKind kind,
    const text::TextEffectExecutionCapability capability) noexcept {
  switch (kind) {
  case text::TextEffectExecutionNodeKind::Scene:
    return capability >= text::TextEffectExecutionCapability::ScenePrefab &&
           capability <= text::TextEffectExecutionCapability::SceneClone;
  case text::TextEffectExecutionNodeKind::MaterialPass:
    return (capability >=
                text::TextEffectExecutionCapability::MaterialColorPass &&
            capability <=
                text::TextEffectExecutionCapability::MaterialProgramPass) ||
           (capability >=
                text::TextEffectExecutionCapability::MaterialAlphaModulate &&
            capability <= text::TextEffectExecutionCapability::
                              MaterialNoiseThresholdDissolve);
  case text::TextEffectExecutionNodeKind::PostEffectPass:
    return capability == text::TextEffectExecutionCapability::None;
  case text::TextEffectExecutionNodeKind::RenderTarget:
    return capability ==
               text::TextEffectExecutionCapability::RenderTargetOffscreen ||
           capability ==
               text::TextEffectExecutionCapability::RenderTargetExpanded;
  case text::TextEffectExecutionNodeKind::History:
    return capability == text::TextEffectExecutionCapability::HistoryFeedback;
  case text::TextEffectExecutionNodeKind::MediaInput:
    return capability >= text::TextEffectExecutionCapability::MediaFont &&
           capability <= text::TextEffectExecutionCapability::MediaParticle;
  case text::TextEffectExecutionNodeKind::State:
    return capability >=
               text::TextEffectExecutionCapability::StateDeterministicRandom &&
           capability <= text::TextEffectExecutionCapability::StateParticle;
  case text::TextEffectExecutionNodeKind::Composite:
    return capability ==
           text::TextEffectExecutionCapability::CompositeSourceOver;
  case text::TextEffectExecutionNodeKind::Layout:
    return capability >= text::TextEffectExecutionCapability::LayoutGlyphRun &&
           capability <= text::TextEffectExecutionCapability::LayoutTimedLyric;
  case text::TextEffectExecutionNodeKind::Selector:
    return capability == text::TextEffectExecutionCapability::SelectorBase ||
           capability == text::TextEffectExecutionCapability::SelectorTime;
  case text::TextEffectExecutionNodeKind::Operator:
    return capability >= text::TextEffectExecutionCapability::OperatorProgram &&
           capability <= text::TextEffectExecutionCapability::OperatorLifecycle;
  }
  return false;
}

bool ValidateExecutionGraphParameters(
    const text::TextEffectExecutionNodeFramePlan &node, std::string &error) {
  std::unordered_set<std::string> parameterKeys;
  parameterKeys.reserve(node.parameters.size());
  for (const auto &parameter : node.parameters) {
    const bool postParameter =
        text::IsPostEffectExecutionParameter(parameter.parameter);
    const bool materialParameter =
        text::IsMaterialExecutionParameter(parameter.parameter);
    const bool sceneParameter =
        text::IsSceneExecutionParameter(parameter.parameter);
    const bool nodeParameter =
        text::IsNodeExecutionParameter(parameter.parameter);
    const bool postNode =
        node.kind == text::TextEffectExecutionNodeKind::PostEffectPass;
    const bool materialNode =
        node.kind == text::TextEffectExecutionNodeKind::MaterialPass;
    const bool sceneNode =
        node.kind == text::TextEffectExecutionNodeKind::Scene;
    const bool parameterizedNode =
        node.kind == text::TextEffectExecutionNodeKind::Operator ||
        node.kind == text::TextEffectExecutionNodeKind::MediaInput ||
        node.kind == text::TextEffectExecutionNodeKind::History;
    if (parameter.nodeId != node.nodeId ||
        parameter.values.size() !=
            text::TextEffectExecutionParameterComponentCount(
                parameter.parameter) ||
        parameter.valueSpace ==
            text::TextEffectExecutionParameterSpace::SourcePixels ||
        (parameter.domain == text::TextEffectExecutionParameterDomain::Page &&
         parameter.stableUnitId) ||
        (parameter.domain ==
             text::TextEffectExecutionParameterDomain::PerUnit &&
         !parameter.stableUnitId) ||
        (postParameter &&
         (!postNode ||
          parameter.domain != text::TextEffectExecutionParameterDomain::Page ||
          parameter.slot != 0U)) ||
        (materialParameter && (!materialNode || parameter.slot >= 32U)) ||
        (nodeParameter && (!parameterizedNode || parameter.slot >= 32U)) ||
        (sceneParameter &&
         (!sceneNode ||
          (node.capability != text::TextEffectExecutionCapability::SceneClone &&
           (node.capability != text::TextEffectExecutionCapability::SceneEntity ||
            parameter.domain != text::TextEffectExecutionParameterDomain::Page)) ||
          parameter.slot != 0U)) ||
        (!postParameter && !materialParameter && !sceneParameter &&
         !nodeParameter) ||
        std::any_of(parameter.values.begin(), parameter.values.end(),
                    [](const float value) { return !std::isfinite(value); })) {
      error = "text execution graph parameter is outside its closed domain: " +
              node.nodeId;
      return false;
    }
    using ParameterKind = text::TextEffectExecutionParameterKind;
    using ParameterSpace = text::TextEffectExecutionParameterSpace;
    const bool invalidSceneContract =
        (parameter.parameter == ParameterKind::SceneTranslation &&
         parameter.valueSpace != ParameterSpace::ReferencePixels) ||
        (parameter.parameter == ParameterKind::SceneScale &&
         parameter.valueSpace != ParameterSpace::Unitless) ||
        (parameter.parameter == ParameterKind::SceneOpacity &&
         (parameter.valueSpace != ParameterSpace::Unitless ||
          parameter.values.front() < 0.0F || parameter.values.front() > 1.0F));
    if (invalidSceneContract) {
      error =
          "text execution scene parameter is outside its closed contract: " +
          node.nodeId;
      return false;
    }
    if (parameter.parameter ==
            text::TextEffectExecutionParameterKind::PostEffectProgress &&
        (parameter.values.front() < 0.0F || parameter.values.front() > 1.0F)) {
      error = "text execution graph post progress is outside [0, 1]: " +
              node.nodeId;
      return false;
    }
    if (parameter.parameter ==
            text::TextEffectExecutionParameterKind::PostEffectBlurRadius &&
        parameter.values.front() < 0.0F) {
      error =
          "text execution graph post blur radius is negative: " + node.nodeId;
      return false;
    }
    std::string key;
    try {
      key = std::to_string(static_cast<unsigned>(parameter.parameter)) + ":" +
            std::to_string(parameter.slot) + ":";
      key += parameter.stableUnitId ? std::to_string(*parameter.stableUnitId)
                                    : "page";
    } catch (...) {
      error = "text execution graph parameter identity allocation failed";
      return false;
    }
    if (!parameterKeys.emplace(std::move(key)).second) {
      error = "text execution graph parameter is bound more than once: " +
              node.nodeId;
      return false;
    }
  }
  if (node.kind == text::TextEffectExecutionNodeKind::History) {
    using Kind = text::TextEffectExecutionParameterKind;
    using Domain = text::TextEffectExecutionParameterDomain;
    using Space = text::TextEffectExecutionParameterSpace;
    for (const auto &parameter : node.parameters) {
      const bool fold = parameter.slot == 0U &&
          (parameter.parameter == Kind::NodeScalar ||
           parameter.parameter == Kind::NodeVector2);
      const bool writeMode = parameter.parameter == Kind::NodeScalar &&
          parameter.slot == kTextExecutionHistoryWriteModeSlot;
      if (parameter.domain != Domain::Page ||
          parameter.valueSpace != Space::Unitless || (!fold && !writeMode) ||
          (writeMode && parameter.values.front() != 0.0F &&
           parameter.values.front() != 1.0F)) {
        error = "text history parameter is outside its closed contract: " + node.nodeId;
        return false;
      }
    }
    const auto *mode = FindTextExecutionParameter(
        node, Kind::NodeScalar, kTextExecutionHistoryWriteModeSlot);
    if (mode && (node.parameters.size() != 1U || node.inputIds.size() != 1U)) {
      error = "text history write mode requires one input and no same-frame fold: " +
              node.nodeId;
      return false;
    }
  }
  return true;
}

const text::TextEffectExecutionParameterSample *FindTextExecutionParameter(
    const text::TextEffectExecutionNodeFramePlan &node,
    const text::TextEffectExecutionParameterKind kind, const std::uint32_t slot,
    const std::optional<std::uint64_t> stableUnitId) noexcept {
  if (stableUnitId) {
    const auto unit = std::find_if(
        node.parameters.begin(), node.parameters.end(),
        [&](const auto &sample) {
          return sample.parameter == kind && sample.slot == slot &&
                 sample.domain ==
                     text::TextEffectExecutionParameterDomain::PerUnit &&
                 sample.stableUnitId == stableUnitId;
        });
    if (unit != node.parameters.end())
      return &*unit;
  }
  const auto page = std::find_if(
      node.parameters.begin(), node.parameters.end(), [&](const auto &sample) {
        return sample.parameter == kind && sample.slot == slot &&
               sample.domain == text::TextEffectExecutionParameterDomain::Page;
      });
  return page == node.parameters.end() ? nullptr : &*page;
}

thread_local TextExecutionParameterConsumptionScope *
    TextExecutionParameterConsumptionScope::active_ = nullptr;

thread_local TextPostEffectParameterConsumptionScope *
    TextPostEffectParameterConsumptionScope::active_ = nullptr;

TextPostEffectParameterConsumptionScope::TextPostEffectParameterConsumptionScope(
    const text::TextEffectPostEffectNode &effect, const bool enabled)
    : previous_(active_), effect_(effect), enabled_(enabled),
      consumed_(enabled ? effect.parameters.size() : 0U, false) {
  active_ = enabled ? this : nullptr;
}

TextPostEffectParameterConsumptionScope::~TextPostEffectParameterConsumptionScope() {
  active_ = previous_;
}

void TextPostEffectParameterConsumptionScope::Record(
    const text::TextEffectPostEffectNode &effect,
    const text::TextPostEffectParameter &parameter) noexcept {
  if (!active_ || effect.nodeId != active_->effect_.nodeId ||
      effect.kind != active_->effect_.kind)
    return;
  for (std::size_t index = 0; index < active_->effect_.parameters.size(); ++index) {
    const auto &authored = active_->effect_.parameters[index];
    if (authored.name != parameter.name || authored.values != parameter.values ||
        !std::equal(authored.keyframes.begin(), authored.keyframes.end(),
                    parameter.keyframes.begin(), parameter.keyframes.end(),
                    [](const auto &a, const auto &b) {
                      return a.offset == b.offset && a.value == b.value &&
                             a.tangentIn == b.tangentIn &&
                             a.tangentOut == b.tangentOut &&
                             a.cubicBezier == b.cubicBezier &&
                             a.bezierTimeIn == b.bezierTimeIn &&
                             a.bezierTimeOut == b.bezierTimeOut;
                    }))
      continue;
    active_->consumed_[index] = true;
    return;
  }
}

void TextPostEffectParameterConsumptionScope::Finish(
    text::TextNamedParameterConsumptionEvidence &evidence,
    const bool completed) const {
  evidence = {};
  if (!enabled_)
    return;
  evidence.recorded = true;
  evidence.declaredParameterCount = effect_.parameters.size();
  for (std::size_t index = 0; index < consumed_.size(); ++index) {
    if (consumed_[index])
      ++evidence.consumedParameterCount;
    else
      evidence.unconsumedParameters.push_back(effect_.parameters[index].name);
  }
  evidence.verified = completed && evidence.unconsumedParameters.empty();
}

TextExecutionParameterConsumptionScope::TextExecutionParameterConsumptionScope(
    const text::TextEffectExecutionNodeFramePlan &node, const bool enabled)
    : previous_(active_), node_(node), enabled_(enabled),
      consumed_(enabled ? node.parameters.size() : 0U, false) {
  active_ = enabled ? this : nullptr;
}

TextExecutionParameterConsumptionScope::~TextExecutionParameterConsumptionScope() {
  active_ = previous_;
}

void TextExecutionParameterConsumptionScope::Record(
    const text::TextEffectExecutionNodeFramePlan &node,
    const text::TextEffectExecutionParameterSample &sample) noexcept {
  if (!active_ || active_->node_.nodeId != node.nodeId ||
      sample.nodeId != node.nodeId)
    return;
  for (std::size_t index = 0; index < active_->node_.parameters.size(); ++index) {
    const auto &authored = active_->node_.parameters[index];
    if (authored.parameter == sample.parameter && authored.slot == sample.slot &&
        authored.domain == sample.domain &&
        authored.stableUnitId == sample.stableUnitId) {
      active_->consumed_[index] = true;
      return;
    }
  }
}

void TextExecutionParameterConsumptionScope::Finish(
    text::TextExecutionNodeEvidence &evidence,
    const text::TextExecutionNodeEvidence *earlier) const {
  if (earlier == &evidence)
    earlier = nullptr;
  evidence.consumedParameterCount = 0;
  evidence.unconsumedParameters.clear();
  evidence.parameterConsumptionVerified = false;
  if (!enabled_)
    return;
  const auto sameIdentity = [](const auto &sample, const auto &identity) {
    return static_cast<std::uint32_t>(sample.parameter) == identity.kind &&
           static_cast<std::uint32_t>(sample.domain) == identity.domain &&
           sample.slot == identity.slot && sample.stableUnitId == identity.stableUnitId;
  };
  const bool mergeEarlier = earlier && earlier->nodeId == node_.nodeId &&
      earlier->active && earlier->completed &&
      earlier->declaredParameterCount == node_.parameters.size() &&
      earlier->unconsumedParameters.size() <= node_.parameters.size() &&
      earlier->consumedParameterCount ==
          node_.parameters.size() - earlier->unconsumedParameters.size() &&
      std::all_of(earlier->unconsumedParameters.begin(),
                  earlier->unconsumedParameters.end(), [&](const auto &identity) {
        return std::count_if(node_.parameters.begin(), node_.parameters.end(),
                             [&](const auto &sample) {
                               return sameIdentity(sample, identity);
                             }) == 1 &&
               std::count_if(earlier->unconsumedParameters.begin(),
                             earlier->unconsumedParameters.end(),
                             [&](const auto &other) {
          return identity.kind == other.kind && identity.domain == other.domain &&
                 identity.slot == other.slot && identity.stableUnitId == other.stableUnitId;
        }) == 1;
      });
  for (std::size_t index = 0; index < node_.parameters.size(); ++index) {
    const auto &sample = node_.parameters[index];
    const bool consumedEarlier = mergeEarlier &&
        std::none_of(earlier->unconsumedParameters.begin(),
                     earlier->unconsumedParameters.end(), [&](const auto &identity) {
                       return sameIdentity(sample, identity);
                     });
    if (consumed_[index] || consumedEarlier) {
      ++evidence.consumedParameterCount;
    } else {
      evidence.unconsumedParameters.push_back(
          {static_cast<std::uint32_t>(sample.parameter),
           static_cast<std::uint32_t>(sample.domain), sample.slot,
           sample.stableUnitId});
    }
  }
  evidence.parameterConsumptionVerified =
      evidence.completed && evidence.active && evidence.unconsumedParameters.empty();
}

const text::TextEffectExecutionParameterSample *ConsumeTextExecutionParameter(
    const text::TextEffectExecutionNodeFramePlan &node,
    const text::TextEffectExecutionParameterKind kind, const std::uint32_t slot,
    const std::optional<std::uint64_t> stableUnitId) noexcept {
  const auto *sample = FindTextExecutionParameter(node, kind, slot, stableUnitId);
  if (sample)
    TextExecutionParameterConsumptionScope::Record(node, *sample);
  return sample;
}

void RecordTextExecutionParameterConsumption(
    const text::TextEffectExecutionNodeFramePlan &node,
    const text::TextEffectExecutionParameterSample &sample) noexcept {
  TextExecutionParameterConsumptionScope::Record(node, sample);
}

bool ValidateClosedExecutionMaterialParameters(
    const text::TextEffectExecutionNodeFramePlan &node, std::string &error) {
  using Capability = text::TextEffectExecutionCapability;
  using Kind = text::TextEffectExecutionParameterKind;
  using Space = text::TextEffectExecutionParameterSpace;
  std::size_t expectedCount = 0U;
  std::unordered_set<std::uint64_t> perUnitIdentities;
  bool hasPerUnitContract = false;
  const auto require = [&](const Kind kind, const std::uint32_t slot,
                           const Space space,
                           const std::size_t components) -> bool {
    std::size_t matchCount = 0U;
    std::size_t pageCount = 0U;
    std::unordered_set<std::uint64_t> unitIdentities;
    for (const auto &parameter : node.parameters) {
      if (parameter.parameter != kind || parameter.slot != slot)
        continue;
      ++matchCount;
      if (parameter.valueSpace != space ||
          parameter.values.size() != components) {
        error = "text execution material parameter contract is incomplete: " +
                node.nodeId;
        return false;
      }
      if (parameter.domain == text::TextEffectExecutionParameterDomain::Page) {
        ++pageCount;
      } else if (!parameter.stableUnitId ||
                 !unitIdentities.insert(*parameter.stableUnitId).second) {
        error = "text execution material per-unit parameter identity is "
                "invalid: " +
                node.nodeId;
        return false;
      }
    }
    if (matchCount == 0U || pageCount > 1U ||
        (pageCount != 0U && pageCount != matchCount)) {
      error = "text execution material parameter domain is incomplete or "
              "mixed within a slot: " +
              node.nodeId;
      return false;
    }
    if (pageCount == 0U) {
      if (!hasPerUnitContract) {
        perUnitIdentities = std::move(unitIdentities);
        hasPerUnitContract = true;
      } else if (perUnitIdentities != unitIdentities) {
        error = "text execution material per-unit parameter sets do not "
                "share one stable topology: " +
                node.nodeId;
        return false;
      }
    }
    expectedCount += matchCount;
    return true;
  };
  for (const auto &parameter : node.parameters) {
    if (parameter.slot != kTextExecutionSourceCanvasSlot)
      continue;
    if (parameter.parameter != Kind::MaterialVector4 ||
        parameter.domain != text::TextEffectExecutionParameterDomain::Page ||
        parameter.stableUnitId || parameter.valueSpace != Space::ReferencePixels ||
        parameter.values.size() != 4U ||
        !std::all_of(parameter.values.begin(), parameter.values.end(),
                     [](float value) { return std::isfinite(value); }) ||
        parameter.values[0] <= 0.0F || parameter.values[1] <= 0.0F ||
        parameter.values[2] < 0.0F || parameter.values[3] < 0.0F) {
      error = "text source canvas requires positive Page extents and nonnegative RT expansion: " +
              node.nodeId;
      return false;
    }
    ++expectedCount;
  }
  switch (node.capability) {
  case Capability::MaterialAlphaModulate:
    if (!require(Kind::MaterialScalar, 0U, Space::Unitless, 1U))
      return false;
    break;
  case Capability::MaterialColorModulate:
    if (!require(Kind::MaterialVector4, 0U, Space::Unitless, 4U))
      return false;
    break;
  case Capability::MaterialThresholdRevealBlur:
    if (!require(Kind::MaterialScalar, 0U, Space::Unitless, 1U) ||
        !require(Kind::MaterialScalar, 1U, Space::ReferencePixels, 1U)) {
      return false;
    }
    break;
  case Capability::MaterialThresholdTransitionBlur:
    if (!require(Kind::MaterialScalar, 0U, Space::Unitless, 1U) ||
        !require(Kind::MaterialScalar, 1U, Space::Unitless, 1U) ||
        !require(Kind::MaterialScalar, 2U, Space::Unitless, 1U) ||
        !require(Kind::MaterialScalar, 3U, Space::ReferencePixels, 1U) ||
        !require(Kind::MaterialVector2, 0U, Space::Unitless, 2U)) {
      return false;
    }
    break;
  case Capability::MaterialGlyphUvBallisticEchoComposite:
    if (!require(Kind::MaterialScalar, 0U, Space::Unitless, 1U))
      return false;
    break;
  case Capability::MaterialDirectionalBoxBlur:
    if (!require(Kind::MaterialScalar, 0U, Space::ReferencePixels, 1U) ||
        !require(Kind::MaterialVector2, 1U, Space::Unitless, 2U)) {
      return false;
    }
    for (const auto slot : {2U, 4U}) {
      if (FindTextExecutionParameter(node, Kind::MaterialVector2, slot) &&
          !require(Kind::MaterialVector2, slot, Space::ReferencePixels, 2U))
        return false;
    }
    break;
  case Capability::MaterialSeparableGaussianBlur: {
    const auto *trailing = FindTextExecutionParameter(node, Kind::MaterialScalar, 15U);
    if (trailing && (!require(Kind::MaterialScalar, 15U, Space::Unitless, 1U) ||
                     (trailing->values[0] != 0.0F && trailing->values[0] != 1.0F))) {
      error = "text axis blur requires a Page pre/post-sample limit selector";
      return false;
    }
    // Some authored passes accumulate weighted neighbours without dividing
    // by their total. This changes reduction, not the sampling/kernel code.
    const auto *sum = FindTextExecutionParameter(node, Kind::MaterialScalar, 14U);
    if (sum && (!require(Kind::MaterialScalar, 14U, Space::Unitless, 1U) ||
                (sum->values[0] != 0.0F && sum->values[0] != 1.0F))) {
      error = "text axis blur requires a Page average/sum selector";
      return false;
    }
    const auto *sampling = FindTextExecutionParameter(node, Kind::MaterialScalar, 13U);
    if (sampling && (!require(Kind::MaterialScalar, 13U, Space::Unitless, 1U) ||
                     (sampling->values[0] != 0.0F && sampling->values[0] != 1.0F))) {
      error = "text axis blur requires a Page reference/output sampling selector";
      return false;
    }
    const auto strideSpace = sampling && sampling->values[0] == 1.0F
                                 ? Space::Unitless : Space::ReferencePixels;
    const bool custom = std::any_of(
        node.parameters.begin(), node.parameters.end(), [](const auto &parameter) {
          return (parameter.parameter == Kind::MaterialVector4 &&
                  (parameter.slot == 6U || parameter.slot == 7U)) ||
                 (parameter.parameter == Kind::MaterialScalar && parameter.slot == 8U);
        });
    if (!require(Kind::MaterialScalar, 0U, strideSpace, 1U) ||
        !require(Kind::MaterialVector2, 1U, Space::Unitless, 2U) ||
        (!custom && !require(Kind::MaterialScalar, 2U, Space::Unitless, 1U))) {
      return false;
    }
    // Authored weights replace the Gaussian kernel. Requiring a dummy sigma
    // would acknowledge a parameter that the shader never consumes.
    if (custom &&
        (!require(Kind::MaterialVector4, 6U, Space::Unitless, 4U) ||
         !require(Kind::MaterialVector4, 7U, Space::Unitless, 4U) ||
         !require(Kind::MaterialScalar, 8U, Space::Unitless, 1U)))
      return false;
    // An explicit box kernel shares the separable pass topology. Its radius
    // is the sample stride, while sampleCount controls its finite support.
    for (const auto slot : {3U, 4U, 5U, 9U, 10U, 11U, 12U}) {
      if (FindTextExecutionParameter(node, Kind::MaterialScalar, slot) &&
          !require(Kind::MaterialScalar, slot, Space::Unitless, 1U))
        return false;
    }
    break;
  }
  case Capability::MaterialWeightedAxisBoxBlur:
    if (!require(Kind::MaterialScalar, 0U, Space::ReferencePixels, 1U) ||
        !require(Kind::MaterialVector2, 1U, Space::Unitless, 2U) ||
        !require(Kind::MaterialScalar, 2U, Space::Unitless, 1U) ||
        !require(Kind::MaterialScalar, 3U, Space::Unitless, 1U)) {
      return false;
    }
    for (const auto slot : {4U, 5U}) {
      if (FindTextExecutionParameter(node, Kind::MaterialVector2, slot) &&
          !require(Kind::MaterialVector2, slot, Space::ReferencePixels, 2U))
        return false;
    }
    for (const auto slot : {6U, 7U, 8U}) {
      if (FindTextExecutionParameter(node, Kind::MaterialScalar, slot) &&
          !require(Kind::MaterialScalar, slot, Space::Unitless, 1U))
        return false;
    }
    break;
  case Capability::MaterialTurbulenceDisplacement:
    if (const auto *mode = FindTextExecutionParameter(node, Kind::MaterialScalar, 13U);
        mode && mode->values.front() == 2.0F) {
      if (!require(Kind::MaterialScalar, 13U, Space::Unitless, 1U) ||
          !require(Kind::MaterialScalar, 0U, Space::Unitless, 1U) ||
          !require(Kind::MaterialScalar, 1U, Space::Unitless, 1U) ||
          !require(Kind::MaterialScalar, 2U, Space::Unitless, 1U) ||
          !require(Kind::MaterialVector2, 3U, Space::Unitless, 2U))
        return false;
      break;
    }
    if (!require(Kind::MaterialVector2, 0U, Space::Unitless, 2U) ||
        !require(Kind::MaterialScalar, 1U, Space::Unitless, 1U) ||
        !require(Kind::MaterialScalar, 2U, Space::Unitless, 1U) ||
        !require(Kind::MaterialScalar, 3U, Space::Unitless, 1U) ||
        !require(Kind::MaterialVector2, 4U, Space::Unitless, 2U) ||
        !require(Kind::MaterialScalar, 5U, Space::Unitless, 1U) ||
        !require(Kind::MaterialVector2, 11U, Space::Unitless, 2U)) {
      return false;
    }
    for (std::uint32_t slot = 6U; slot <= 10U; ++slot)
      if (!require(Kind::MaterialScalar, slot, Space::Unitless, 1U))
        return false;
    if (FindTextExecutionParameter(node, Kind::MaterialScalar, 12U) &&
        !require(Kind::MaterialScalar, 12U, Space::Unitless, 1U))
      return false;
    if (const auto *mode = FindTextExecutionParameter(node, Kind::MaterialScalar, 13U)) {
      if (!require(Kind::MaterialScalar, 13U, Space::Unitless, 1U) ||
          (mode->values.front() != 0.0F && mode->values.front() != 1.0F)) {
        error = "text turbulence mode must be zero, one or two";
        return false;
      }
      if (mode->values.front() == 1.0F &&
          (!require(Kind::MaterialVector2, 14U, Space::ReferencePixels, 2U) ||
           !require(Kind::MaterialScalar, 15U, Space::Unitless, 1U)))
        return false;
    }
    break;
  case Capability::MaterialDeepGlowComposite: {
    const auto *mode = FindTextExecutionParameter(node, Kind::MaterialScalar, 5U);
    if (mode && (!require(Kind::MaterialScalar, 5U, Space::Unitless, 1U) ||
                 (mode->values.front() != 0.0F && mode->values.front() != 1.0F &&
                  mode->values.front() != 2.0F))) {
      error = "text deep glow requires a Page legacy/screen/depth-screen selector";
      return false;
    }
    if (!require(Kind::MaterialScalar, 0U, Space::Unitless, 1U) ||
        !require(Kind::MaterialScalar, 1U, Space::Unitless, 1U)) {
      return false;
    }
    if ((!mode || mode->values.front() == 0.0F) &&
        (!require(Kind::MaterialScalar, 2U, Space::ReferencePixels, 1U) ||
         !require(Kind::MaterialScalar, 3U, Space::Unitless, 1U) ||
         !require(Kind::MaterialVector4, 4U, Space::Unitless, 4U)))
      return false;
    if (mode && mode->values.front() == 2.0F &&
        (!require(Kind::MaterialVector4, 6U, Space::Unitless, 4U) ||
         !require(Kind::MaterialScalar, 7U, Space::Unitless, 1U) ||
         !require(Kind::MaterialScalar, 8U, Space::Unitless, 1U)))
      return false;
    break;
  }
  case Capability::MaterialSdfAlphaOutline: {
    if (!require(Kind::MaterialScalar, 0U, Space::ReferencePixels, 1U) ||
        !require(Kind::MaterialVector2, 1U, Space::Unitless, 2U)) {
      return false;
    }
    if (const auto *sampling = FindTextExecutionParameter(node, Kind::MaterialScalar, 2U)) {
      if (!require(Kind::MaterialScalar, 2U, Space::Unitless, 1U) ||
          (sampling->values[0] != 0.0F && sampling->values[0] != 1.0F)) {
        error = "text alpha outline requires a UV/reference sampling selector";
        return false;
      }
    }
    break;
  }
  case Capability::MaterialSdfLightSweep:
    if (!require(Kind::MaterialScalar, 0U, Space::Unitless, 1U) ||
        !require(Kind::MaterialScalar, 1U, Space::ReferencePixels, 1U) ||
        !require(Kind::MaterialVector4, 2U, Space::Unitless, 4U) ||
        !require(Kind::MaterialScalar, 3U, Space::Unitless, 1U) ||
        !require(Kind::MaterialScalar, 4U, Space::Unitless, 1U)) {
      return false;
    }
    break;
  case Capability::MaterialSdfCutGlow:
    if (!require(Kind::MaterialScalar, 0U, Space::Unitless, 1U) ||
        !require(Kind::MaterialScalar, 1U, Space::Unitless, 1U) ||
        !require(Kind::MaterialVector4, 2U, Space::Unitless, 4U) ||
        !require(Kind::MaterialVector2, 3U, Space::ReferencePixels, 2U)) {
      return false;
    }
    break;
  case Capability::MaterialShapedRampMask:
    if (!require(Kind::MaterialVector2, 0U, Space::Unitless, 2U) ||
        !require(Kind::MaterialVector2, 1U, Space::Unitless, 2U) ||
        !require(Kind::MaterialScalar, 2U, Space::Unitless, 1U) ||
        !require(Kind::MaterialScalar, 3U, Space::Unitless, 1U) ||
        !require(Kind::MaterialScalar, 4U, Space::Unitless, 1U) ||
        !require(Kind::MaterialVector2, 5U, Space::Unitless, 2U)) {
      return false;
    }
    break;
  case Capability::MaterialMaskedDualOffsetCrossfade:
    if (!require(Kind::MaterialScalar, 0U, Space::Unitless, 1U) ||
        !require(Kind::MaterialVector2, 1U, Space::Unitless, 2U) ||
        !require(Kind::MaterialVector2, 2U, Space::Unitless, 2U) ||
        !require(Kind::MaterialScalar, 3U, Space::Unitless, 1U) ||
        !require(Kind::MaterialScalar, 4U, Space::Unitless, 1U) ||
        !require(Kind::MaterialVector2, 5U, Space::Unitless, 2U) ||
        !require(Kind::MaterialScalar, 6U, Space::Unitless, 1U)) {
      return false;
    }
    break;
  case Capability::MaterialLineColumnNoiseTrail:
    if (!require(Kind::MaterialScalar, 0U, Space::Unitless, 1U) ||
        !require(Kind::MaterialScalar, 1U, Space::Unitless, 1U) ||
        !require(Kind::MaterialScalar, 2U, Space::Unitless, 1U) ||
        !require(Kind::MaterialVector2, 3U, Space::Unitless, 2U)) {
      return false;
    }
    break;
  case Capability::MaterialParticleScatterComposite:
    if (!require(Kind::MaterialScalar, 0U, Space::Unitless, 1U) ||
        !require(Kind::MaterialVector2, 1U, Space::Unitless, 2U) ||
        !require(Kind::MaterialVector3, 2U, Space::Unitless, 3U) ||
        !require(Kind::MaterialVector3, 4U, Space::Unitless, 3U) ||
        !require(Kind::MaterialScalar, 3U, Space::Unitless, 1U)) {
      return false;
    }
    break;
  case Capability::MaterialSdfWaveEnergy:
    for (std::uint32_t slot = 0U; slot < 4U; ++slot)
      if (!require(Kind::MaterialScalar, slot, Space::Unitless, 1U))
        return false;
    break;
  case Capability::MaterialMaskMotionNoiseGlow:
    // progress, y_indez, GlowRange, mask_right, mask_width, w_stren,
    // texFixScale. These are normalized controls, not reference-pixel radii.
    for (std::uint32_t slot = 0U; slot <= 6U; ++slot)
      if (!require(Kind::MaterialScalar, slot, Space::Unitless, 1U))
        return false;
    break;
  case Capability::MaterialFaceNoiseShadow:
    if (!require(Kind::MaterialScalar, 0U, Space::Unitless, 1U) ||
        !require(Kind::MaterialScalar, 1U, Space::Unitless, 1U) ||
        !require(Kind::MaterialVector2, 2U, Space::ReferencePixels, 2U) ||
        !require(Kind::MaterialVector2, 3U, Space::Unitless, 2U) ||
        !require(Kind::MaterialScalar, 4U, Space::Unitless, 1U) ||
        !require(Kind::MaterialScalar, 5U, Space::Unitless, 1U) ||
        !require(Kind::MaterialScalar, 6U, Space::Unitless, 1U)) {
      return false;
    }
    break;
  case Capability::MaterialProjectionMeshComposite: {
    if (!require(Kind::MaterialScalar, 4U, Space::Unitless, 1U) ||
        !require(Kind::MaterialScalar, 0U, Space::Unitless, 1U))
      return false;
    const auto *pass = FindTextExecutionParameter(node, Kind::MaterialScalar, 4U);
    if (!pass || !std::isfinite(pass->values.front()) ||
        pass->values.front() < 0.0F || pass->values.front() > 4.0F ||
        pass->values.front() != std::floor(pass->values.front())) {
      error = "text projection composite requires a Page pass selector [0, 4]";
      return false;
    }
    const float mode = pass->values.front();
    if (mode <= 1.0F &&
        !require(Kind::MaterialVector2, 1U, Space::ReferencePixels, 2U))
      return false;
    if ((mode == 0.0F || mode == 3.0F) &&
        !require(Kind::MaterialScalar, 2U, Space::Unitless, 1U))
      return false;
    if (mode == 3.0F &&
        (!require(Kind::MaterialScalar, 3U, Space::Unitless, 1U) ||
         !require(Kind::MaterialVector4, 5U, Space::Unitless, 4U))) {
      return false;
    }
    break;
  }
  case Capability::MaterialVatRbdMesh:
    if (!require(Kind::MaterialScalar, 0U, Space::Unitless, 1U) ||
        !require(Kind::MaterialScalar, 1U, Space::Unitless, 1U) ||
        !require(Kind::MaterialScalar, 2U, Space::Unitless, 1U) ||
        !require(Kind::MaterialVector2, 4U, Space::ReferencePixels, 2U) ||
        !require(Kind::MaterialScalar, 5U, Space::Unitless, 1U) ||
        !require(Kind::MaterialScalar, 6U, Space::Unitless, 1U) ||
        !require(Kind::MaterialScalar, 7U, Space::Unitless, 1U) ||
        !require(Kind::MaterialScalar, 8U, Space::Unitless, 1U) ||
        !require(Kind::MaterialScalar, 9U, Space::Unitless, 1U) ||
        !require(Kind::MaterialScalar, 10U, Space::Unitless, 1U) ||
        !require(Kind::MaterialVector3, 11U, Space::Unitless, 3U) ||
        !require(Kind::MaterialVector3, 12U, Space::Unitless, 3U) ||
        !require(Kind::MaterialScalar, 13U, Space::Unitless, 1U) ||
        !require(Kind::MaterialScalar, 14U, Space::Unitless, 1U)) {
      return false;
    }
    break;
  case Capability::MaterialMultimeshTextComposite: {
    if (!require(Kind::MaterialScalar, 7U, Space::Unitless, 1U))
      return false;
    const auto *mode =
        FindTextExecutionParameter(node, Kind::MaterialScalar, 7U);
    if (!mode ||
        (mode->values.front() != 0.0F && mode->values.front() != 1.0F)) {
      error = "text multimesh pass requires a Page feedback/composite selector";
      return false;
    }
    if (mode->values.front() == 0.0F) {
      if (!require(Kind::MaterialScalar, 0U, Space::Unitless, 1U))
        return false;
    } else if (!require(Kind::MaterialScalar, 1U, Space::Unitless, 1U) ||
               !require(Kind::MaterialVector2, 2U, Space::Unitless, 2U) ||
               !require(Kind::MaterialVector2, 3U, Space::Unitless, 2U) ||
               !require(Kind::MaterialVector4, 6U, Space::Unitless, 4U)) {
      return false;
    }
    break;
  }
  case Capability::MaterialRadialMediaComposite:
    if (!require(Kind::MaterialScalar, 0U, Space::Unitless, 1U) ||
        !require(Kind::MaterialVector2, 1U, Space::Unitless, 2U) ||
        !require(Kind::MaterialScalar, 2U, Space::Unitless, 1U) ||
        !require(Kind::MaterialVector2, 3U, Space::Unitless, 2U) ||
        !require(Kind::MaterialScalar, 4U, Space::Unitless, 1U)) {
      return false;
    }
    if (FindTextExecutionParameter(node, Kind::MaterialScalar, 5U) &&
        !require(Kind::MaterialScalar, 5U, Space::Unitless, 1U))
      return false;
    break;
  case Capability::MaterialSpiralSdfWarp:
    if (!require(Kind::MaterialScalar, 0U, Space::Unitless, 1U) ||
        !require(Kind::MaterialScalar, 1U, Space::Unitless, 1U) ||
        !require(Kind::MaterialVector2, 2U, Space::ReferencePixels, 2U)) {
      return false;
    }
    if (FindTextExecutionParameter(node, Kind::MaterialVector2, 3U) &&
        !require(Kind::MaterialVector2, 3U, Space::ReferencePixels, 2U))
      return false;
    break;
  case Capability::MaterialFluxTurbulentBlend: {
    if (!require(Kind::MaterialScalar, 11U, Space::Unitless, 1U))
      return false;
    const auto *modeParameter =
        FindTextExecutionParameter(node, Kind::MaterialScalar, 11U);
    if (!modeParameter) {
      error = "text flux pass selection requires a Page parameter";
      return false;
    }
    const auto mode = modeParameter->values.front();
    if (mode == 0.0F) {
      if (!require(Kind::MaterialScalar, 0U, Space::Unitless, 1U) ||
          !require(Kind::MaterialScalar, 1U, Space::Unitless, 1U) ||
          !require(Kind::MaterialVector2, 2U, Space::Unitless, 2U) ||
          !require(Kind::MaterialScalar, 3U, Space::Unitless, 1U) ||
          !require(Kind::MaterialVector4, 5U, Space::Unitless, 4U) ||
          !require(Kind::MaterialScalar, 7U, Space::Unitless, 1U))
        return false;
    } else if (mode == 1.0F) {
      if (!require(Kind::MaterialScalar, 1U, Space::Unitless, 1U) ||
          !require(Kind::MaterialVector2, 2U, Space::Unitless, 2U) ||
          !require(Kind::MaterialScalar, 4U, Space::Unitless, 1U) ||
          !require(Kind::MaterialScalar, 8U, Space::Unitless, 1U) ||
          !require(Kind::MaterialScalar, 9U, Space::Unitless, 1U))
        return false;
    } else if (mode == 2.0F) {
      if (!require(Kind::MaterialScalar, 6U, Space::Unitless, 1U) ||
          !require(Kind::MaterialVector3, 12U, Space::Unitless, 3U) ||
          !require(Kind::MaterialVector2, 14U, Space::Unitless, 2U))
        return false;
    } else {
      error =
          "text flux pass must select color, displacement, or bloom composite";
      return false;
    }
    break;
  }
  case Capability::MaterialCubeProjectionComposite:
    if (!require(Kind::MaterialScalar, 0U, Space::Unitless, 1U) ||
        !require(Kind::MaterialScalar, 1U, Space::Unitless, 1U) ||
        !require(Kind::MaterialScalar, 2U, Space::Unitless, 1U) ||
        !require(Kind::MaterialScalar, 4U, Space::Unitless, 1U)) {
      return false;
    }
    if (FindTextExecutionParameter(node, Kind::MaterialVector2, 4U) &&
        !require(Kind::MaterialVector2, 4U, Space::ReferencePixels, 2U))
      return false;
    if (FindTextExecutionParameter(node, Kind::MaterialScalar, 6U) &&
        !require(Kind::MaterialScalar, 6U, Space::Unitless, 1U))
      return false;
    break;
  case Capability::MaterialCylinderProjectionComposite:
    if (!require(Kind::MaterialScalar, 0U, Space::Unitless, 1U) ||
        !require(Kind::MaterialVector2, 1U, Space::ReferencePixels, 2U) ||
        !require(Kind::MaterialScalar, 2U, Space::Unitless, 1U) ||
        !require(Kind::MaterialScalar, 3U, Space::Unitless, 1U) ||
        !require(Kind::MaterialScalar, 4U, Space::Unitless, 1U) ||
        !require(Kind::MaterialScalar, 5U, Space::Unitless, 1U)) {
      return false;
    }
    for (const auto slot : {6U, 7U, 8U, 9U}) {
      if (FindTextExecutionParameter(node, Kind::MaterialScalar, slot) &&
          !require(Kind::MaterialScalar, slot, Space::Unitless, 1U))
        return false;
    }
    break;
  case Capability::MaterialMaskedCutLine:
    if (!require(Kind::MaterialScalar, 0U, Space::Unitless, 1U) ||
        !require(Kind::MaterialScalar, 1U, Space::Unitless, 1U) ||
        !require(Kind::MaterialScalar, 2U, Space::Unitless, 1U) ||
        !require(Kind::MaterialVector2, 3U, Space::ReferencePixels, 2U) ||
        !require(Kind::MaterialScalar, 4U, Space::Unitless, 1U) ||
        !require(Kind::MaterialScalar, 5U, Space::Unitless, 1U) ||
        !require(Kind::MaterialScalar, 6U, Space::Unitless, 1U)) {
      return false;
    }
    break;
  case Capability::MaterialCutLineReveal:
    if (!require(Kind::MaterialScalar, 0U, Space::Unitless, 1U) ||
        !require(Kind::MaterialScalar, 1U, Space::Unitless, 1U) ||
        !require(Kind::MaterialScalar, 2U, Space::Unitless, 1U) ||
        !require(Kind::MaterialScalar, 3U, Space::Unitless, 1U) ||
        !require(Kind::MaterialScalar, 4U, Space::Unitless, 1U)) {
      return false;
    }
    break;
  case Capability::MaterialPackedMultiscaleGaussianBlur: {
    const auto *sampling = FindTextExecutionParameter(node, Kind::MaterialScalar, 6U);
    if (sampling &&
        (!require(Kind::MaterialScalar, 6U, Space::Unitless, 1U) ||
         (sampling->values[0] != 0.0F && sampling->values[0] != 1.0F))) {
      error = "text packed blur requires a Page reference/output sampling selector";
      return false;
    }
    const bool outputSampling = sampling && sampling->values[0] == 1.0F;
    if (!require(Kind::MaterialScalar, 0U, Space::Unitless, 1U) ||
        !require(Kind::MaterialVector2, 1U, Space::Unitless, 2U) ||
        !require(Kind::MaterialScalar, 2U,
                 outputSampling ? Space::Unitless : Space::ReferencePixels, 1U) ||
        !require(Kind::MaterialScalar, 3U, Space::Unitless, 1U) ||
        !require(Kind::MaterialScalar, 4U, Space::Unitless, 1U) ||
        !require(Kind::MaterialScalar, 5U, Space::Unitless, 1U)) {
      return false;
    }
    break;
  }
  case Capability::MaterialDualPackedGlowComposite:
    if (!require(Kind::MaterialScalar, 0U, Space::Unitless, 1U) ||
        !require(Kind::MaterialVector4, 1U, Space::Unitless, 4U) ||
        !require(Kind::MaterialVector3, 2U, Space::Unitless, 3U) ||
        !require(Kind::MaterialScalar, 3U, Space::Unitless, 1U) ||
        !require(Kind::MaterialVector4, 4U, Space::Unitless, 4U) ||
        !require(Kind::MaterialScalar, 5U, Space::Unitless, 1U)) {
      return false;
    }
    break;
  case Capability::MaterialEncodedAlphaDistanceBlur:
    if (!require(Kind::MaterialScalar, 0U, Space::Unitless, 1U) ||
        !require(Kind::MaterialVector2, 1U, Space::Unitless, 2U) ||
        !require(Kind::MaterialScalar, 2U, Space::Unitless, 1U) ||
        !require(Kind::MaterialScalar, 3U, Space::Unitless, 1U) ||
        !require(Kind::MaterialScalar, 4U, Space::Unitless, 1U) ||
        !require(Kind::MaterialScalar, 5U, Space::Unitless, 1U)) {
      return false;
    }
    break;
  case Capability::MaterialSdfNormalLightSweep:
    if (!require(Kind::MaterialVector3, 0U, Space::Unitless, 3U) ||
        !require(Kind::MaterialVector3, 1U, Space::Unitless, 3U) ||
        !require(Kind::MaterialScalar, 2U, Space::Unitless, 1U) ||
        !require(Kind::MaterialScalar, 3U, Space::Unitless, 1U) ||
        !require(Kind::MaterialScalar, 4U, Space::Unitless, 1U) ||
        !require(Kind::MaterialVector2, 5U, Space::Unitless, 2U) ||
        !require(Kind::MaterialScalar, 6U, Space::Unitless, 1U) ||
        !require(Kind::MaterialVector4, 7U, Space::Unitless, 4U)) {
      return false;
    }
    break;
  case Capability::MaterialLightSweepPackedGlowComposite:
    if (!require(Kind::MaterialScalar, 0U, Space::Unitless, 1U) ||
        !require(Kind::MaterialScalar, 1U, Space::Unitless, 1U) ||
        !require(Kind::MaterialScalar, 2U, Space::Unitless, 1U) ||
        !require(Kind::MaterialScalar, 3U, Space::Unitless, 1U)) {
      return false;
    }
    break;
  case Capability::MaterialRadialDecayHsvGlow:
    if (!require(Kind::MaterialScalar, 0U, Space::Unitless, 1U) ||
        !require(Kind::MaterialVector2, 1U, Space::Unitless, 2U)) {
      return false;
    }
    break;
  case Capability::MaterialFixedNineTapAxisBlur:
    if (!require(Kind::MaterialScalar, 0U, Space::ReferencePixels, 1U) ||
        !require(Kind::MaterialVector2, 1U, Space::Unitless, 2U)) {
      return false;
    }
    break;
  case Capability::MaterialNoiseThresholdDissolve:
    if (!require(Kind::MaterialScalar, 0U, Space::Unitless, 1U) ||
        !require(Kind::MaterialScalar, 1U, Space::Unitless, 1U) ||
        !require(Kind::MaterialVector2, 0U, Space::Unitless, 2U)) {
      return false;
    }
    break;
  case Capability::MaterialHsvOriginalOverBlur:
    break;
  default:
    return true;
  }
  if (node.parameters.size() != expectedCount) {
    error = "text execution material has parameters outside its closed "
            "capability contract: " +
            node.nodeId;
    return false;
  }
  const auto validateSample = [&](const std::optional<std::uint64_t>
                                      stableUnitId) {
    const auto find = [&](const Kind kind, const std::uint32_t slot) {
      return FindTextExecutionParameter(node, kind, slot, stableUnitId);
    };
    const auto scalar = [&](const std::uint32_t slot) {
      return find(Kind::MaterialScalar, slot)->values.front();
    };
    switch (node.capability) {
    case Capability::MaterialAlphaModulate:
      if (scalar(0U) < 0.0F || scalar(0U) > 1.0F) {
        error =
            "text execution alpha modulation is outside [0, 1]: " + node.nodeId;
        return false;
      }
      break;
    case Capability::MaterialColorModulate: {
      const auto *color = find(Kind::MaterialVector4, 0U);
      if (std::any_of(
              color->values.begin(), color->values.end(),
              [](const float value) { return value < 0.0F || value > 1.0F; })) {
        error =
            "text execution color modulation is outside [0, 1]: " + node.nodeId;
        return false;
      }
      break;
    }
    case Capability::MaterialNoiseThresholdDissolve: {
      const auto *noiseScale = find(Kind::MaterialVector2, 0U);
      if (scalar(0U) < 0.0F || scalar(0U) > 1.0F || scalar(1U) <= 0.0F ||
          scalar(1U) > 1.0F || noiseScale->values[0] <= 0.0F ||
          noiseScale->values[1] <= 0.0F) {
        error = "text execution noise-threshold dissolve parameter is outside "
                "its closed range: " +
                node.nodeId;
        return false;
      }
      break;
    }
    case Capability::MaterialThresholdRevealBlur:
      if (scalar(0U) < 0.0F || scalar(0U) > 1.0F || scalar(1U) < 0.0F) {
        error = "text execution reveal material parameter is outside its "
                "closed range: " +
                node.nodeId;
        return false;
      }
      break;
    case Capability::MaterialGlyphUvBallisticEchoComposite:
      if (scalar(0U) < 0.0F || scalar(0U) > 1.0F) {
        error = "text execution ballistic material time is outside [0, 1]: " +
                node.nodeId;
        return false;
      }
      break;
    case Capability::MaterialThresholdTransitionBlur: {
      const auto *direction = find(Kind::MaterialVector2, 0U);
      if (scalar(0U) < 0.0F || scalar(0U) > 1.0F || scalar(1U) < 0.0F ||
          scalar(2U) < 0.0F || scalar(3U) < 0.0F ||
          std::hypot(direction->values[0], direction->values[1]) <= 0.0F) {
        error = "text execution transition material parameter is outside its "
                "closed range: " +
                node.nodeId;
        return false;
      }
      break;
    }
    case Capability::MaterialDirectionalBoxBlur: {
      const auto *direction = find(Kind::MaterialVector2, 1U);
      if (scalar(0U) < 0.0F ||
          std::hypot(direction->values[0], direction->values[1]) <= 0.0F) {
        error = "text execution directional blur parameter is outside its "
                "closed range: " +
                node.nodeId;
        return false;
      }
      break;
    }
    case Capability::MaterialSeparableGaussianBlur:
    case Capability::MaterialWeightedAxisBoxBlur: {
      const auto *direction = find(Kind::MaterialVector2, 1U);
      const auto *sigma = find(Kind::MaterialScalar, 2U);
      const bool invalidShape =
          node.capability == Capability::MaterialSeparableGaussianBlur
              ? sigma && sigma->values.front() <= 0.0F
              : scalar(2U) < 0.0F || scalar(3U) < 0.0F;
      if (scalar(0U) < 0.0F || invalidShape ||
          std::hypot(direction->values[0], direction->values[1]) <= 0.0F) {
        error = "text execution blur material parameter is outside its closed "
                "range: " +
                node.nodeId;
        return false;
      }
      break;
    }
    case Capability::MaterialDeepGlowComposite: {
      const auto *mode = find(Kind::MaterialScalar, 5U);
      const bool screen = mode && mode->values.front() >= 1.0F;
      // Validate every admitted Page or per-unit sample, without assuming that
      // the intensity/gamma slots also have a Page fallback.
      for (const auto &parameter : node.parameters) {
        if (parameter.parameter != Kind::MaterialScalar)
          continue;
        const float value = parameter.values.front();
        bool invalid = !std::isfinite(value);
        if (parameter.slot == 0U || parameter.slot == 2U)
          invalid |= value < 0.0F;
        if (parameter.slot == 1U)
          invalid |= screen ? value <= 0.0F : value < 0.0F || value > 1.0F;
        if (parameter.slot == 3U)
          invalid |= value < 1.0F || value > 8.0F || value != std::floor(value);
        if (parameter.slot == 7U || parameter.slot == 8U)
          invalid |= value < 0.0F || value > 1.0F;
        if (invalid) {
          error = "text execution deep-glow parameter is outside its closed "
                  "range: " +
                  node.nodeId;
          return false;
        }
      }
      break;
    }
    case Capability::MaterialRadialDecayHsvGlow:
      if (scalar(0U) < 0.0F) {
        error =
            "text execution radial-decay strength is negative: " + node.nodeId;
        return false;
      }
      break;
    case Capability::MaterialFixedNineTapAxisBlur: {
      const auto *axis = find(Kind::MaterialVector2, 1U);
      if (scalar(0U) < 0.0F ||
          std::hypot(axis->values[0], axis->values[1]) <= 0.0F) {
        error = "text execution fixed-nine blur parameter is outside its "
                "closed range: " +
                node.nodeId;
        return false;
      }
      break;
    }
    case Capability::MaterialCubeProjectionComposite:
    case Capability::MaterialCylinderProjectionComposite: {
      const std::uint32_t fovSlot =
          node.capability == Capability::MaterialCubeProjectionComposite ? 2U
                                                                         : 3U;
      if (scalar(fovSlot) <= 0.0F || scalar(fovSlot) >= 180.0F) {
        error =
            "text execution projection field-of-view is outside (0, 180): " +
            node.nodeId;
        return false;
      }
      if (node.capability == Capability::MaterialCubeProjectionComposite) {
        const auto *axis = find(Kind::MaterialScalar, 6U);
        if (axis && axis->values.front() != 0.0F &&
            axis->values.front() != 1.0F) {
          error = "text cube projection axis must be X (0) or Y (1)";
          return false;
        }
      } else if (node.capability ==
                 Capability::MaterialCylinderProjectionComposite) {
        const auto *extent = find(Kind::MaterialVector2, 1U);
        if (extent->values[0] <= 0.0F || extent->values[1] <= 0.0F ||
            scalar(2U) <= 0.0F || scalar(5U) < 0.0F || scalar(5U) > 1.0F) {
          error = "text cylinder projection requires a positive extent and UV "
                  "scale";
          return false;
        }
      }
      break;
    }
    default:
      break;
    }
    return true;
  };
  // Closed slots may all be per-unit. Validate the same stable topology that
  // admission established instead of dereferencing a nonexistent Page value.
  if (!hasPerUnitContract)
    return validateSample(std::nullopt);
  for (const auto stableUnitId : perUnitIdentities)
    if (!validateSample(stableUnitId))
      return false;
  return true;
}

} // namespace videocut::skia_runtime::internal
