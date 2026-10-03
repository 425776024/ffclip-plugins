#include "videocut/text_composition/TextInvalidation.h"

#include <algorithm>
#include <utility>

namespace videocut::text_composition {
namespace {

void AppendUnique(std::vector<std::string> &values,
                  const std::string &value) {
  if (!value.empty() &&
      std::find(values.begin(), values.end(), value) == values.end())
    values.push_back(value);
}

TextInvalidationImpact DependencyClosure(TextInvalidationImpact impact)
    noexcept {
  if (text::HasInvalidationImpact(impact,
                                  TextInvalidationImpact::Resources)) {
    impact |= TextInvalidationImpact::Layout;
    impact |= TextInvalidationImpact::GlyphMaterial;
    impact |= TextInvalidationImpact::Backdrop;
    impact |= TextInvalidationImpact::PostEffectState;
  }
  if (text::HasInvalidationImpact(impact, TextInvalidationImpact::Layout)) {
    impact |= TextInvalidationImpact::GlyphMaterial;
    impact |= TextInvalidationImpact::Backdrop;
    impact |= TextInvalidationImpact::Animation;
    impact |= TextInvalidationImpact::PostEffectState;
  }
  if (text::HasInvalidationImpact(impact,
                                  TextInvalidationImpact::GlyphMaterial) ||
      text::HasInvalidationImpact(impact, TextInvalidationImpact::Backdrop) ||
      text::HasInvalidationImpact(impact, TextInvalidationImpact::Animation) ||
      text::HasInvalidationImpact(impact,
                                  TextInvalidationImpact::PostEffectState))
    impact |= TextInvalidationImpact::Composite;
  return impact;
}

} // namespace

TextInvalidationPlan
PlanTextInvalidation(const text::TextPropertyPatch &patch) noexcept {
  try {
    TextInvalidationPlan plan;
    for (const auto &assignment : patch.assignments) {
      const auto *descriptor =
          text::DescribeTextProperty(assignment.address.property);
      plan.impact |= descriptor ? descriptor->invalidation
                                : TextInvalidationImpact::Resources |
                                      TextInvalidationImpact::Layout |
                                      TextInvalidationImpact::GlyphMaterial |
                                      TextInvalidationImpact::Backdrop |
                                      TextInvalidationImpact::Animation |
                                      TextInvalidationImpact::PostEffectState |
                                      TextInvalidationImpact::Composite;
      const auto &target = assignment.address.target;
      for (const auto &paragraphId : target.paragraphIds)
        AppendUnique(plan.paragraphIds, paragraphId);
      for (const auto &runId : target.runIds)
        AppendUnique(plan.runIds, runId);
      if (target.scope == text::TextPropertyScope::GlyphMaterialLayer)
        AppendUnique(plan.glyphLayerIds, target.layerId);
      if (target.scope == text::TextPropertyScope::BackdropLayer)
        AppendUnique(plan.backdropLayerIds, target.layerId);
    }
    plan.impact = DependencyClosure(plan.impact);
    return plan;
  } catch (...) {
    TextInvalidationPlan conservative;
    conservative.impact = TextInvalidationImpact::Resources |
                          TextInvalidationImpact::Layout |
                          TextInvalidationImpact::GlyphMaterial |
                          TextInvalidationImpact::Backdrop |
                          TextInvalidationImpact::Animation |
                          TextInvalidationImpact::PostEffectState |
                          TextInvalidationImpact::Composite;
    return conservative;
  }
}

} // namespace videocut::text_composition
